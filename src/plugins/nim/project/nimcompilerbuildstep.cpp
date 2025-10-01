// Copyright (C) Filippo Cucchetto <filippocucchetto@gmail.com>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "nimcompilerbuildstep.h"

#include "nimconstants.h"
#include "nimoutputtaskparser.h"
#include "nimtr.h"
#include "project/nimproject.h"

#include <projectexplorer/buildconfiguration.h>
#include <projectexplorer/ioutputparser.h>
#include <projectexplorer/processparameters.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/toolchain.h>
#include <projectexplorer/toolchainkitaspect.h>

#include <utils/qtcprocess.h>
#include <utils/qtcassert.h>

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QTextEdit>

using namespace ProjectExplorer;
using namespace Utils;

namespace Nim {

NimCompilerBuildStep::NimCompilerBuildStep(BuildStepList *parentList, Id id)
    : AbstractProcessStep(parentList, id)
{
    setCommandLineProvider([this] { return commandLine(); });

    connect(project(), &Project::fileListChanged,
            this, &NimCompilerBuildStep::updateTargetNimFile);
}

void NimCompilerBuildStep::setupOutputFormatter(OutputFormatter *formatter)
{
    formatter->addLineParser(new NimParser);
    formatter->addLineParsers(kit()->createOutputParsers());
    formatter->addSearchDir(buildDirectory());
    AbstractProcessStep::setupOutputFormatter(formatter);
}

QWidget *NimCompilerBuildStep::createConfigWidget()
{
    auto widget = new QWidget;

    setDisplayName(Tr::tr("Nim build step"));
    setSummaryText(Tr::tr("Nim build step"));

    auto targetComboBox = new QComboBox(widget);

    auto additionalArgumentsLineEdit = new QLineEdit(widget);

    auto commandTextEdit = new QTextEdit(widget);
    commandTextEdit->setReadOnly(true);
    commandTextEdit->setMinimumSize(QSize(0, 0));

    auto defaultArgumentsComboBox = new QComboBox(widget);
    defaultArgumentsComboBox->addItem(Tr::tr("None", "No default arguments"));
    defaultArgumentsComboBox->addItem(msgBuildConfigurationDebug());
    defaultArgumentsComboBox->addItem(msgBuildConfigurationRelease());

    auto formLayout = new QFormLayout(widget);
    formLayout->addRow(Tr::tr("Target:"), targetComboBox);
    formLayout->addRow(Tr::tr("Default arguments:"), defaultArgumentsComboBox);
    formLayout->addRow(Tr::tr("Extra arguments:"),  additionalArgumentsLineEdit);
    formLayout->addRow(Tr::tr("Command:"), commandTextEdit);

    auto updateUi = [this, commandTextEdit, targetComboBox, additionalArgumentsLineEdit,
                     defaultArgumentsComboBox] {
        const CommandLine cmd = commandLine();
        const QStringList parts = ProcessArgs::splitArgs(cmd.toUserOutput(), HostOsInfo::hostOs());

        commandTextEdit->setText(parts.join(QChar::LineFeed));

        // Re enter the files
        targetComboBox->clear();
        const FilePaths files = project()->files(Project::AllFiles);
        for (const FilePath &file : files) {
            if (file.endsWith(".nim"))
                targetComboBox->addItem(file.fileName(), file.toVariant());
        }

        const int index = targetComboBox->findData(m_targetNimFile.toVariant());
        targetComboBox->setCurrentIndex(index);

        const QString text = m_userCompilerOptions.join(QChar::Space);
        additionalArgumentsLineEdit->setText(text);

        defaultArgumentsComboBox->setCurrentIndex(m_defaultOptions);
    };

    connect(project(), &Project::fileListChanged, this, updateUi);

    connect(targetComboBox, &QComboBox::activated, this, [this, targetComboBox, updateUi] {
        const QVariant data = targetComboBox->currentData();
        m_targetNimFile = FilePath::fromString(data.toString());
        updateUi();
    });

    connect(additionalArgumentsLineEdit, &QLineEdit::textEdited,
            this, [this, updateUi](const QString &text) {
        m_userCompilerOptions = text.split(QChar::Space);
        updateUi();
    });

    connect(defaultArgumentsComboBox, &QComboBox::activated, this, [this, updateUi](int index) {
        m_defaultOptions = static_cast<DefaultBuildOptions>(index);
        updateUi();
    });

    updateUi();

    return widget;
}

void NimCompilerBuildStep::fromMap(const Store &map)
{
    AbstractProcessStep::fromMap(map);
    m_userCompilerOptions = map[Constants::C_NIMCOMPILERBUILDSTEP_USERCOMPILEROPTIONS].toString().split('|');
    m_defaultOptions = static_cast<DefaultBuildOptions>(map[Constants::C_NIMCOMPILERBUILDSTEP_DEFAULTBUILDOPTIONS].toInt());
    m_targetNimFile = FilePath::fromString(map[Constants::C_NIMCOMPILERBUILDSTEP_TARGETNIMFILE].toString());
}

void NimCompilerBuildStep::toMap(Store &map) const
{
    AbstractProcessStep::toMap(map);
    map[Constants::C_NIMCOMPILERBUILDSTEP_USERCOMPILEROPTIONS] = m_userCompilerOptions.join('|');
    map[Constants::C_NIMCOMPILERBUILDSTEP_DEFAULTBUILDOPTIONS] = m_defaultOptions;
    map[Constants::C_NIMCOMPILERBUILDSTEP_TARGETNIMFILE] = m_targetNimFile.toSettings();
}

void NimCompilerBuildStep::setBuildType(BuildConfiguration::BuildType buildType)
{
    switch (buildType) {
    case BuildConfiguration::Release:
        m_defaultOptions = DefaultBuildOptions::Release;
        break;
    case BuildConfiguration::Debug:
        m_defaultOptions = DefaultBuildOptions::Debug;
        break;
    default:
        m_defaultOptions = DefaultBuildOptions::Empty;
        break;
    }

    updateTargetNimFile();
}

CommandLine NimCompilerBuildStep::commandLine()
{
    auto bc = qobject_cast<NimBuildConfiguration *>(buildConfiguration());
    QTC_ASSERT(bc, return {});

    auto tc = ToolchainKitAspect::toolchain(kit(), Constants::C_NIMLANGUAGE_ID);
    QTC_ASSERT(tc, return {});

    CommandLine cmd{tc->compilerCommand()};

    cmd.addArg("c");

    if (m_defaultOptions == Release)
        cmd.addArg("-d:release");
    else if (m_defaultOptions == Debug)
        cmd.addArgs({"--debugInfo", "--lineDir:on"});

    cmd.addArg("--out:" + outFilePath().path());
    cmd.addArg("--nimCache:" + bc->cacheDirectory().path());

    for (const QString &arg : std::as_const(m_userCompilerOptions)) {
        if (!arg.isEmpty())
            cmd.addArg(arg);
    }

    if (!m_targetNimFile.isEmpty())
        cmd.addArg(m_targetNimFile.path());

    return cmd;
}

FilePath NimCompilerBuildStep::outFilePath() const
{
    const QString targetName = m_targetNimFile.baseName();
    return buildDirectory().pathAppended(targetName).withExecutableSuffix();
}

void NimCompilerBuildStep::updateTargetNimFile()
{
    if (!m_targetNimFile.isEmpty())
        return;

    const FilePaths files = project()->files(Project::AllFiles);
    for (const FilePath &file : files) {
        if (file.endsWith(".nim")) {
            m_targetNimFile = file;
            break;
        }
    }
}

// NimCompilerBuildStepFactory

NimCompilerBuildStepFactory::NimCompilerBuildStepFactory()
{
    registerStep<NimCompilerBuildStep>(Constants::C_NIMCOMPILERBUILDSTEP_ID);
    setDisplayName(Tr::tr("Nim Compiler Build Step"));
    setSupportedStepList(ProjectExplorer::Constants::BUILDSTEPS_BUILD);
    setSupportedConfiguration(Constants::C_NIMBUILDCONFIGURATION_ID);
    setRepeatable(false);
}

} // namespace Nim
