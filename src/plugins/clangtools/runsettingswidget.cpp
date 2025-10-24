// Copyright (C) 2019 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "runsettingswidget.h"

#include "clangtoolsconstants.h"
#include "clangtoolssettings.h"
#include "clangtoolstr.h"
#include "clangtoolsutils.h"
#include "diagnosticconfigswidget.h"
#include "executableinfo.h"
#include "runsettingswidget.h"

#include <coreplugin/dialogs/ioptionspage.h>

#include <cppeditor/clangdiagnosticconfigswidget.h>
#include <cppeditor/clangdiagnosticconfigsselectionwidget.h>

#include <utils/layoutbuilder.h>
#include <utils/pathchooser.h>

#include <QApplication>
#include <QCheckBox>
#include <QSpinBox>
#include <QThread>

using namespace CppEditor;
using namespace Utils;

namespace ClangTools::Internal {

class RunSettingsWidget;

class SettingsWidget : public Core::IOptionsPageWidget
{
public:
    SettingsWidget();
    ~SettingsWidget() override;

    Utils::FilePath clangTidyPath() const;
    Utils::FilePath clazyStandalonePath() const;

private:
    void apply() final;

    ClangToolsSettings *m_settings;

    Utils::PathChooser *m_clangTidyPathChooser;
    Utils::PathChooser *m_clazyStandalonePathChooser;
    RunSettingsWidget *m_runSettingsWidget;
};

static SettingsWidget *m_settingsWidgetInstance = nullptr;

SettingsWidget::SettingsWidget()
    : m_settings(ClangToolsSettings::instance())
{
    m_settingsWidgetInstance = this;

    auto createPathChooser = [this](ClangToolType tool)
    {
        const QString placeHolderText = toolShippedExecutable(tool).toUserOutput();
        FilePath path = m_settings->executable(tool);
        if (path.isEmpty() && placeHolderText.isEmpty()) {
            path = tool == ClangToolType::Tidy ? FilePath(Constants::CLANG_TIDY_EXECUTABLE_NAME)
                                               : FilePath(Constants::CLAZY_STANDALONE_EXECUTABLE_NAME);
        }
        PathChooser *pathChooser = new PathChooser;
        pathChooser->setExpectedKind(PathChooser::ExistingCommand);
        pathChooser->setPromptDialogTitle(tool == ClangToolType::Tidy ? Tr::tr("Clang-Tidy Executable")
                                                                      : Tr::tr("Clazy Executable"));
        pathChooser->setDefaultValue(placeHolderText);
        pathChooser->setFilePath(path);
        pathChooser->setHistoryCompleter(tool == ClangToolType::Tidy
                                        ? Key("ClangTools.ClangTidyExecutable.History")
                                        : Key("ClangTools.ClazyStandaloneExecutable.History"));
        pathChooser->setCommandVersionArguments({"--version"});
        return pathChooser;
    };
    m_clangTidyPathChooser = createPathChooser(ClangToolType::Tidy);
    m_clazyStandalonePathChooser = createPathChooser(ClangToolType::Clazy);

    m_runSettingsWidget = new RunSettingsWidget;
    m_runSettingsWidget->fromSettings(m_settings->runSettings());

    using namespace Layouting;

    Column {
        Group {
            title(Tr::tr("Executables")),
            Form {
                Tr::tr("Clang-Tidy:"), m_clangTidyPathChooser, br,
                Tr::tr("Clazy-Standalone:"), m_clazyStandalonePathChooser
            }
        },
        m_runSettingsWidget,
        st
    }.attachTo(this);
}

void SettingsWidget::apply()
{
    // Executables
    m_settings->setExecutable(ClangToolType::Tidy, clangTidyPath());
    m_settings->setExecutable(ClangToolType::Clazy, clazyStandalonePath());

    // Run options
    m_settings->setRunSettings(m_runSettingsWidget->toSettings());

    // Custom configs
    const ClangDiagnosticConfigs customConfigs
        = m_runSettingsWidget->diagnosticSelectionWidget()->customConfigs();
    m_settings->setDiagnosticConfigs(customConfigs);

    m_settings->writeSettings();
}

SettingsWidget::~SettingsWidget()
{
    m_settingsWidgetInstance = nullptr;
}

FilePath SettingsWidget::clangTidyPath() const
{
    return m_clangTidyPathChooser->unexpandedFilePath();
}

FilePath SettingsWidget::clazyStandalonePath() const
{
    return m_clazyStandalonePathChooser->unexpandedFilePath();
}

RunSettingsWidget::RunSettingsWidget(QWidget *parent)
    : QWidget(parent)
{
    m_diagnosticWidget = new ClangDiagnosticConfigsSelectionWidget;
    m_preferConfigFile = new QCheckBox(Tr::tr("Prefer .clang-tidy file, if present"));
    m_buildBeforeAnalysis = new QCheckBox(Tr::tr("Build the project before analysis"));
    m_analyzeOpenFiles = new QCheckBox(Tr::tr("Analyze open files"));
    m_parallelJobsSpinBox = new QSpinBox;
    m_parallelJobsSpinBox->setRange(1, 32);

    using namespace Layouting;

    // FIXME: Let RunSettingsWidget inherit from QGroupBox?
    Column {
        Group {
            title(Tr::tr("Run Options")),
            Column {
                m_diagnosticWidget,
                m_preferConfigFile,
                m_buildBeforeAnalysis,
                m_analyzeOpenFiles,
                Row { Tr::tr("Parallel jobs:"), m_parallelJobsSpinBox, st },
            }
        },
        noMargin
    }.attachTo(this);
}

RunSettingsWidget::~RunSettingsWidget() = default;

ClangDiagnosticConfigsSelectionWidget *RunSettingsWidget::diagnosticSelectionWidget()
{
    return m_diagnosticWidget;
}

static ClangDiagnosticConfigsWidget *createEditWidget(const ClangDiagnosticConfigs &configs,
                                                      const Id &configToSelect)
{
    // Determine executable paths
    FilePath clangTidyPath;
    FilePath clazyStandalonePath;
    if (m_settingsWidgetInstance) {
        // Global settings case; executables might not yet applied to settings
        clangTidyPath = m_settingsWidgetInstance->clangTidyPath();
        clangTidyPath = clangTidyPath.isEmpty() ? toolFallbackExecutable(ClangToolType::Tidy)
                                                : fullPath(clangTidyPath);

        clazyStandalonePath = m_settingsWidgetInstance->clazyStandalonePath();
        clazyStandalonePath = clazyStandalonePath.isEmpty()
                            ? toolFallbackExecutable(ClangToolType::Clazy)
                            : fullPath(clazyStandalonePath);
    } else {
        // "Projects Mode > Clang Tools" case, check settings
        clangTidyPath = toolExecutable(ClangToolType::Tidy);
        clazyStandalonePath = toolExecutable(ClangToolType::Clazy);
    }

    return new DiagnosticConfigsWidget(configs,
                                       configToSelect,
                                       ClangTidyInfo(clangTidyPath),
                                       ClazyStandaloneInfo(clazyStandalonePath));
}

void RunSettingsWidget::fromSettings(const RunSettings &s)
{
    disconnect(m_diagnosticWidget, &ClangDiagnosticConfigsSelectionWidget::changed,
               this, &RunSettingsWidget::changed);
    m_diagnosticWidget->refresh(diagnosticConfigsModel(),
                                s.diagnosticConfigId(),
                                createEditWidget);
    connect(m_diagnosticWidget, &ClangDiagnosticConfigsSelectionWidget::changed,
            this, &RunSettingsWidget::changed);

    m_preferConfigFile->setChecked(s.preferConfigFile());
    connect(m_preferConfigFile, &QCheckBox::toggled, this, &RunSettingsWidget::changed);

    disconnect(m_buildBeforeAnalysis, &QCheckBox::toggled, this, nullptr);
    m_buildBeforeAnalysis->setToolTip(hintAboutBuildBeforeAnalysis());
    m_buildBeforeAnalysis->setCheckState(s.buildBeforeAnalysis() ? Qt::Checked : Qt::Unchecked);
    connect(m_buildBeforeAnalysis, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked)
            showHintAboutBuildBeforeAnalysis();
        emit changed();
    });

    disconnect(m_parallelJobsSpinBox, &QSpinBox::valueChanged, this, &RunSettingsWidget::changed);
    m_parallelJobsSpinBox->setValue(s.parallelJobs());
    m_parallelJobsSpinBox->setMinimum(1);
    m_parallelJobsSpinBox->setMaximum(QThread::idealThreadCount());
    connect(m_parallelJobsSpinBox, &QSpinBox::valueChanged, this, &RunSettingsWidget::changed);
    m_analyzeOpenFiles->setChecked(s.analyzeOpenFiles());
    connect(m_analyzeOpenFiles, &QCheckBox::toggled, this, &RunSettingsWidget::changed);
}

RunSettings RunSettingsWidget::toSettings() const
{
    RunSettings s;
    s.setDiagnosticConfigId(m_diagnosticWidget->currentConfigId());
    s.setPreferConfigFile(m_preferConfigFile->isChecked());
    s.setBuildBeforeAnalysis(m_buildBeforeAnalysis->checkState() == Qt::CheckState::Checked);
    s.setParallelJobs(m_parallelJobsSpinBox->value());
    s.setAnalyzeOpenFiles(m_analyzeOpenFiles->checkState() == Qt::CheckState::Checked);

    return s;
}

// ClangToolsOptionsPage

class ClangToolsOptionsPage final : public Core::IOptionsPage
{
public:
    ClangToolsOptionsPage()
    {
        setId(Constants::SETTINGS_PAGE_ID);
        setDisplayName(Tr::tr("Clang Tools"));
        setCategory("T.Analyzer");
        setWidgetCreator([] { return new SettingsWidget; });
    }
};

void setupClangToolsOptionsPage()
{
    static ClangToolsOptionsPage theClangToolsOptionsPage;
}

} // ClangTools::Internal
