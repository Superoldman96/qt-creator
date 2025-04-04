// Copyright (C) 2019 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qdbstopapplicationstep.h"

#include "qdbconstants.h"
#include "qdbtr.h"

#include <projectexplorer/devicesupport/devicekitaspects.h>
#include <projectexplorer/devicesupport/idevice.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/target.h>

#include <remotelinux/abstractremotelinuxdeploystep.h>

#include <utils/qtcprocess.h>

using namespace ProjectExplorer;
using namespace Tasking;
using namespace Utils;

namespace Qdb::Internal {

// QdbStopApplicationStep

class QdbStopApplicationStep final : public RemoteLinux::AbstractRemoteLinuxDeployStep
{
public:
    QdbStopApplicationStep(BuildStepList *bsl, Id id)
        : AbstractRemoteLinuxDeployStep(bsl, id)
    {
        setWidgetExpandedByDefault(false);

        setInternalInitializer([this] { return isDeploymentPossible(); });
    }

    GroupItem deployRecipe() final;
};

GroupItem QdbStopApplicationStep::deployRecipe()
{
    const auto onSetup = [this](Process &process) {
        const auto device = RunDeviceKitAspect::device(kit());
        if (!device) {
            addErrorMessage(Tr::tr("No device to stop the application on."));
            return SetupResult::StopWithError;
        }
        QTC_CHECK(device);
        process.setCommand({device->filePath(Constants::AppcontrollerFilepath), {"--stop"}});
        process.setWorkingDirectory("/usr/bin");
        Process *proc = &process;
        connect(proc, &Process::readyReadStandardOutput, this, [this, proc] {
            handleStdOutData(proc->readAllStandardOutput());
        });
        return SetupResult::Continue;
    };
    const auto onDone = [this](const Process &process, DoneWith result) {
        if (result == DoneWith::Success) {
            addProgressMessage(Tr::tr("Stopped the running application."));
            return;
        }
        const QString errorOutput = process.cleanedStdErr();
        const QString failureMessage = Tr::tr("Could not check and possibly stop running application.");
        if (process.exitStatus() == QProcess::CrashExit) {
            addErrorMessage(failureMessage);
        } else if (process.result() != ProcessResult::FinishedWithSuccess) {
            handleStdErrData(process.errorString());
        } else if (errorOutput.contains("Could not connect: Connection refused")) {
            addProgressMessage(Tr::tr("Checked that there is no running application."));
        } else if (!errorOutput.isEmpty()) {
            handleStdErrData(errorOutput);
            addErrorMessage(failureMessage);
        }
    };
    return ProcessTask(onSetup, onDone);
}

// QdbStopApplicationStepFactory

QdbStopApplicationStepFactory::QdbStopApplicationStepFactory()
{
    registerStep<QdbStopApplicationStep>(Constants::QdbStopApplicationStepId);
    setDisplayName(Tr::tr("Stop already running application"));
    setSupportedDeviceType(Constants::QdbLinuxOsType);
    setSupportedStepList(ProjectExplorer::Constants::BUILDSTEPS_DEPLOY);
}

} // Qdb::Internal
