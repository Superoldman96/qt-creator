// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "cmakeinstallstep.h"

#include "cmakeabstractprocessstep.h"
#include "cmakeautogenparser.h"
#include "cmakebuildsystem.h"
#include "cmakekitaspect.h"
#include "cmakeoutputparser.h"
#include "cmakeprojectconstants.h"
#include "cmakeprojectmanagertr.h"
#include "cmaketool.h"

#include <projectexplorer/buildstep.h>
#include <projectexplorer/buildsteplist.h>
#include <projectexplorer/processparameters.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/projectexplorersettings.h>

#include <utils/layoutbuilder.h>

using namespace Core;
using namespace ProjectExplorer;
using namespace Utils;

namespace CMakeProjectManager::Internal {

// CMakeInstallStep

class CMakeInstallStep final : public CMakeAbstractProcessStep
{
public:
    CMakeInstallStep(BuildStepList *bsl, Id id)
        : CMakeAbstractProcessStep(bsl, id)
    {
        cmakeArguments.setSettingsKey("CMakeProjectManager.InstallStep.CMakeArguments");
        cmakeArguments.setLabelText(Tr::tr("CMake arguments:"));
        cmakeArguments.setDisplayStyle(StringAspect::LineEditDisplay);

        setCommandLineProvider([this] { return cmakeCommand(); });
    }

private:
    CommandLine cmakeCommand() const;

    void setupOutputFormatter(OutputFormatter *formatter) override;
    QWidget *createConfigWidget() override;

    StringAspect cmakeArguments{this};
};

void CMakeInstallStep::setupOutputFormatter(OutputFormatter *formatter)
{
    CMakeOutputParser *cmakeOutputParser = new CMakeOutputParser;
    cmakeOutputParser->setSourceDirectories(
        {project()->projectDirectory(), buildConfiguration()->buildDirectory()});
    formatter->addLineParsers({new CMakeAutogenParser, cmakeOutputParser});
    formatter->addSearchDir(processParameters()->effectiveWorkingDirectory());
    CMakeAbstractProcessStep::setupOutputFormatter(formatter);
}

CommandLine CMakeInstallStep::cmakeCommand() const
{
    CommandLine cmd;
    if (CMakeTool *tool = CMakeKitAspect::cmakeTool(kit()))
        cmd.setExecutable(tool->cmakeExecutable());

    FilePath buildDirectory = ".";
    if (buildConfiguration())
        buildDirectory = buildConfiguration()->buildDirectory();

    cmd.addArgs({"--install", buildDirectory.path()});

    auto bs = qobject_cast<CMakeBuildSystem *>(buildSystem());
    if (bs && bs->isMultiConfigReader()) {
        cmd.addArg("--config");
        cmd.addArg(bs->cmakeBuildType());
    }

    cmd.addArgs(cmakeArguments(), CommandLine::Raw);

    return cmd;
}

QWidget *CMakeInstallStep::createConfigWidget()
{
    auto updateDetails = [this] {
        ProcessParameters param;
        setupProcessParameters(&param);
        param.setCommandLine(cmakeCommand());

        setSummaryText(param.summary(displayName()));
    };

    setDisplayName(Tr::tr("Install", "ConfigWidget display name."));

    using namespace Layouting;
    auto widget = Form { cmakeArguments, noMargin }.emerge();

    updateDetails();

    cmakeArguments.addOnChanged(this, updateDetails);

    connect(buildConfiguration(), &BuildConfiguration::buildDirectoryChanged, this, updateDetails);
    connect(buildConfiguration(), &BuildConfiguration::buildTypeChanged, this, updateDetails);

    return widget;
}

// CMakeInstallStepFactory

class CMakeInstallStepFactory : public ProjectExplorer::BuildStepFactory
{
public:
    CMakeInstallStepFactory()
    {
        registerStep<CMakeInstallStep>(Constants::CMAKE_INSTALL_STEP_ID);
        setDisplayName(
            Tr::tr("CMake Install", "Display name for CMakeProjectManager::CMakeInstallStep id."));
        setSupportedProjectType(Constants::CMAKE_PROJECT_ID);
        setSupportedStepLists({ProjectExplorer::Constants::BUILDSTEPS_DEPLOY});
    }
};

void setupCMakeInstallStep()
{
    static CMakeInstallStepFactory theCMakeInstallStepFactory;
}

} // CMakeProjectManager::Internal
