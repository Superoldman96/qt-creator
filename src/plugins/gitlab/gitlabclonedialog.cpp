// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "gitlabclonedialog.h"

#include "gitlabprojectsettings.h"
#include "gitlabtr.h"
#include "resultparser.h"

#include <coreplugin/documentmanager.h>
#include <coreplugin/vcsmanager.h>

#include <git/gitclient.h>

#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/projectmanager.h>

#include <utils/algorithm.h>
#include <utils/commandline.h>
#include <utils/environment.h>
#include <utils/fancylineedit.h>
#include <utils/filepath.h>
#include <utils/infolabel.h>
#include <utils/mimeutils.h>
#include <utils/pathchooser.h>
#include <utils/qtcprocess.h>
#include <utils/qtcassert.h>

#include <vcsbase/vcsbaseplugin.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

using namespace QtTaskTree;
using namespace Utils;
using namespace VcsBase;

namespace GitLab {

GitLabCloneDialog::GitLabCloneDialog(const Project &project, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(Tr::tr("Clone Repository"));
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(Tr::tr("Specify repository URL, checkout path and directory.")));
    QHBoxLayout *centerLayout = new QHBoxLayout;
    QFormLayout *form = new QFormLayout;
    m_repositoryCB = new QComboBox(this);
    m_repositoryCB->addItems({project.sshUrl, project.httpUrl});
    form->addRow(Tr::tr("Repository"), m_repositoryCB);
    m_pathChooser = new PathChooser(this);
    m_pathChooser->setExpectedKind(PathChooser::ExistingDirectory);
    form->addRow(Tr::tr("Path"), m_pathChooser);
    m_directoryLE = new FancyLineEdit(this);
    m_directoryLE->setValidationFunction([this](const QString &text) -> Result<> {
        const FilePath fullPath = m_pathChooser->filePath().pathAppended(text);
        if (fullPath.exists())
            return ResultError(Tr::tr("Path \"%1\" already exists.").arg(fullPath.toUserOutput()));
        return ResultOk;
    });
    form->addRow(Tr::tr("Directory"), m_directoryLE);
    m_submodulesCB = new QCheckBox(this);
    form->addRow(Tr::tr("Recursive"), m_submodulesCB);
    form->addItem(new QSpacerItem(10, 10));
    centerLayout->addLayout(form);
    m_cloneOutput = new QPlainTextEdit(this);
    m_cloneOutput->setReadOnly(true);
    centerLayout->addWidget(m_cloneOutput);
    layout->addLayout(centerLayout);
    m_infoLabel = new InfoLabel(this);
    layout->addWidget(m_infoLabel);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_cloneButton = new QPushButton(Tr::tr("Clone"), this);
    buttons->addButton(m_cloneButton, QDialogButtonBox::ActionRole);
    m_cancelButton = buttons->button(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    setLayout(layout);

    m_pathChooser->setFilePath(Core::DocumentManager::projectsDirectory());
    auto [host, path, port]
            = GitLabProjectSettings::remotePartsFromRemote(m_repositoryCB->currentText());
    int slashIndex = path.indexOf('/');
    QTC_ASSERT(slashIndex > 0, return);
    m_directoryLE->setText(path.mid(slashIndex + 1));

    connect(m_pathChooser, &PathChooser::textChanged, this, [this] {
        m_directoryLE->validate();
        updateUi();
    });
    connect(m_pathChooser, &PathChooser::validChanged, this, &GitLabCloneDialog::updateUi);
    connect(m_directoryLE, &FancyLineEdit::textChanged, this, &GitLabCloneDialog::updateUi);
    connect(m_directoryLE, &FancyLineEdit::validChanged, this, &GitLabCloneDialog::updateUi);
    connect(m_cloneButton, &QPushButton::clicked, this, &GitLabCloneDialog::cloneProject);
    connect(m_cancelButton, &QPushButton::clicked, this, [this] {
        if (m_taskTreeRunner.isRunning()) {
            m_cloneOutput->appendPlainText(Tr::tr("User canceled process."));
            m_cancelButton->setEnabled(false);
            m_taskTreeRunner.cancel();
        } else {
            reject();
        }
    });

    updateUi();
    resize(575, 265);
}

GitLabCloneDialog::~GitLabCloneDialog()
{
    if (m_taskTreeRunner.isRunning())
        m_taskTreeRunner.cancel();
}

void GitLabCloneDialog::updateUi()
{
    bool pathValid = m_pathChooser->isValid();
    bool directoryValid = m_directoryLE->isValid();
    m_cloneButton->setEnabled(pathValid && directoryValid);
    if (!pathValid) {
        m_infoLabel->setText(m_pathChooser->errorMessage());
        m_infoLabel->setType(InfoLabel::Error);
    } else if (!directoryValid) {
        m_infoLabel->setText(m_directoryLE->errorMessage());
        m_infoLabel->setType(InfoLabel::Error);
    }
    m_infoLabel->setVisible(!pathValid || !directoryValid);
}

static FilePaths scanDirectoryForFiles(const FilePath &directory)
{
    FilePaths result;
    const FilePaths entries = directory.dirEntries(QDir::AllEntries | QDir::NoDotAndDotDot);

    for (const FilePath &entry : entries) {
        if (entry.isDir())
            result.append(scanDirectoryForFiles(entry));
        else
            result.append(entry);
    }
    return result;
}

void GitLabCloneDialog::cloneProject()
{
    auto *vc = static_cast<VersionControlBase *>(Core::VcsManager::versionControl(Id("G.Git")));
    QTC_ASSERT(vc, return);
    const QStringList extraArgs = m_submodulesCB->isChecked() ? QStringList{ "--recursive" }
                                                              : QStringList{};

    const auto callback = [cloneOutput = QPointer<QPlainTextEdit>(m_cloneOutput)](const QString &text) {
        if (cloneOutput)
            cloneOutput->appendPlainText(text);
    };

    const auto onSetup = [this] {
        QApplication::setOverrideCursor(Qt::WaitCursor);

        m_cloneOutput->clear();
        m_cloneButton->setEnabled(false);
        m_pathChooser->setReadOnly(true);
        m_directoryLE->setReadOnly(true);
    };
    const auto onDone = [this](DoneWith result) {
        QApplication::restoreOverrideCursor();
        const QString emptyLine("\n\n");

        m_cloneOutput->appendPlainText(emptyLine);

        if (result == DoneWith::Success) {
            m_cloneOutput->appendPlainText(Tr::tr("Cloning succeeded.") + emptyLine);
            m_cloneButton->setEnabled(false);

            const FilePath base = m_pathChooser->filePath().pathAppended(m_directoryLE->text());
            FilePaths filesWeMayOpen = filtered(scanDirectoryForFiles(base), [](const FilePath &f) {
                return ProjectExplorer::ProjectManager::canOpenProjectForMimeType(mimeTypeForFile(f));
            });

            // limit the files to the top-most level item(s)
            int minimum = std::numeric_limits<int>::max();
            for (const FilePath &f : std::as_const(filesWeMayOpen)) {
                int parentCount = f.toUrlishString().count('/');
                if (parentCount < minimum)
                    minimum = parentCount;
            }
            filesWeMayOpen = filtered(filesWeMayOpen, [minimum](const FilePath &f) {
                return f.toUrlishString().count('/') == minimum;
            });

            hide(); // avoid too many dialogs.. FIXME: maybe change to some wizard approach?
            if (filesWeMayOpen.isEmpty()) {
                QMessageBox::warning(this, Tr::tr("Warning"),
                                     Tr::tr("Cloned project does not have a project file that can be "
                                            "opened. Try importing the project as a generic project."));
                accept();
            } else {
                const QStringList pFiles = Utils::transform(filesWeMayOpen, [base](const FilePath &f) {
                    return f.relativeNativePathFromDir(base);
                });
                bool ok = false;
                const QString fileToOpen
                    = QInputDialog::getItem(this, Tr::tr("Open Project"),
                                            Tr::tr("Choose the project file to be opened."),
                                            pFiles, 0, false, &ok);
                accept();
                if (ok && !fileToOpen.isEmpty())
                    ProjectExplorer::ProjectExplorerPlugin::openProject(base.pathAppended(fileToOpen));
            }
        } else {
            m_cloneOutput->appendPlainText(Tr::tr("Cloning failed.") + emptyLine);
            const FilePath fullPath = m_pathChooser->filePath().pathAppended(m_directoryLE->text());
            fullPath.removeRecursively();
            m_cloneButton->setEnabled(true);
            m_cancelButton->setEnabled(true);
            m_pathChooser->setReadOnly(false);
            m_directoryLE->setReadOnly(false);
            m_directoryLE->validate();
        }
    };

    const Group recipe {
        onGroupSetup(onSetup),
        vc->cloneTask({m_repositoryCB->currentText(), m_pathChooser->absoluteFilePath(),
                       m_directoryLE->text(), extraArgs, callback, callback}),
        onGroupDone(onDone)
    };

    m_taskTreeRunner.start(recipe);
}

} // namespace GitLab
