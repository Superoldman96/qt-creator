// Copyright (C) 2019 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qdbdevicedebugsupport.h"

#include "qdbconstants.h"

#include <debugger/debuggerruncontrol.h>

#include <perfprofiler/perfprofilerconstants.h>

#include <projectexplorer/devicesupport/idevice.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/qmldebugcommandlinearguments.h>
#include <projectexplorer/runcontrol.h>

#include <qmlprojectmanager/qmlprojectconstants.h>

#include <QtTaskTree/QBarrier>

#include <utils/algorithm.h>
#include <utils/qtcprocess.h>
#include <utils/url.h>

using namespace Debugger;
using namespace ProjectExplorer;
using namespace QtTaskTree;
using namespace Utils;

namespace Qdb::Internal {

static ProcessTask qdbDeviceInferiorProcess(RunControl *runControl,
                                            QmlDebugServicesPreset qmlServices,
                                            const ProcessSetupConfig &config = {})
{
    const auto modifier = [runControl, qmlServices](Process &process) {
        CommandLine cmd{runControl->device()->filePath(Constants::AppcontrollerFilepath)};

        int lowerPort = 0;
        int upperPort = 0;

        if (runControl->usesDebugChannel()) {
            cmd.addArg("--debug-gdb");
            lowerPort = upperPort = runControl->debugChannel().port();
        }
        if (runControl->usesQmlChannel()) {
            cmd.addArg("--debug-qml");
            cmd.addArg("--qml-debug-services");
            cmd.addArg(qmlDebugServices(qmlServices));
            lowerPort = upperPort = runControl->qmlChannel().port();
        }
        if (runControl->usesDebugChannel() && runControl->usesQmlChannel()) {
            lowerPort = runControl->debugChannel().port();
            upperPort = runControl->qmlChannel().port();
            if (lowerPort + 1 != upperPort) {
                runControl->postMessage("Need adjacent free ports for combined C++/QML debugging",
                                        ErrorMessageFormat);
                return SetupResult::StopWithError;
            }
        }
        if (runControl->usesPerfChannel()) {
            const Store perfArgs = runControl->settingsData(PerfProfiler::Constants::PerfSettingsId);
            // appcontroller is not very clear about this, but it expects a comma-separated list of arguments.
            // Any literal commas that apper in the args should be escaped by additional commas.
            // See the source at
            // https://code.qt.io/cgit/qt-apps/boot2qt-appcontroller.git/tree/main.cpp?id=658dc91cf561e41704619a55fbb1f708decf134e#n434
            // and adjust if necessary.
            const QString recordArgs = perfArgs[PerfProfiler::Constants::PerfRecordArgsId]
                                           .toString()
                                           .replace(',', ",,")
                                           .split(' ', Qt::SkipEmptyParts)
                                           .join(',');
            cmd.addArg("--profile-perf");
            cmd.addArgs(recordArgs, CommandLine::Raw);
            lowerPort = upperPort = runControl->perfChannel().port();
        }

        cmd.addArg("--port-range");
        cmd.addArg(QString("%1-%2").arg(lowerPort).arg(upperPort));
        cmd.addCommandLineAsArgs(runControl->commandLine());

        process.setCommand(cmd);
        process.setWorkingDirectory(runControl->workingDirectory());
        process.setEnvironment(runControl->environment());
        return SetupResult::Continue;
    };
    return runControl->processTaskWithModifier(modifier, config);
}

class QdbRunWorkerFactory final : public RunWorkerFactory
{
public:
    QdbRunWorkerFactory()
    {
        setId("QdbRunWorkerFactory");
        setRecipeProducer([](RunControl *runControl) {
            const auto modifier = [runControl](Process &process) {
                const CommandLine remoteCommand = runControl->commandLine();
                const FilePath remoteExe = remoteCommand.executable();
                CommandLine cmd{remoteExe.withNewPath(Constants::AppcontrollerFilepath)};
                cmd.addArg(remoteExe.nativePath());
                cmd.addArgs(remoteCommand.arguments(), CommandLine::Raw);
                process.setCommand(cmd);
            };
            return runControl->processRecipe(modifier);
        });
        addSupportedRunMode(ProjectExplorer::Constants::NORMAL_RUN_MODE);
        addSupportedRunConfig(Constants::QdbRunConfigurationId);
        addSupportedRunConfig(QmlProjectManager::Constants::QML_RUNCONFIG_ID);
        addSupportedDeviceType(Qdb::Constants::QdbLinuxOsType);
    }
};

class QdbDebugWorkerFactory final : public RunWorkerFactory
{
public:
    QdbDebugWorkerFactory()
    {
        setId("QdbDebugWorkerFactory");
        setRecipeProducer([](RunControl *runControl) {
            DebuggerRunParameters rp = DebuggerRunParameters::fromRunControl(runControl);
            rp.setupPortsGatherer(runControl);
            rp.setStartMode(Debugger::AttachToRemoteServer);
            rp.setCloseMode(KillAndExitMonitorAtClose);
            rp.setUseContinueInsteadOfRun(true);
            rp.setContinueAfterAttach(true);
            rp.addSolibSearchDir("%{sysroot}/system/lib");
            rp.setSkipDebugServer(true);

            const ProcessTask processTask(qdbDeviceInferiorProcess(runControl, QmlDebuggerServices,
                                                                   {.setupCanceler = false}));
            return Group {
                When (processTask, &Process::started, WorkflowPolicy::StopOnSuccessOrError) >> Do {
                    debuggerRecipe(runControl, rp)
                }
            };
        });
        addSupportedRunMode(ProjectExplorer::Constants::DEBUG_RUN_MODE);
        addSupportedRunConfig(Constants::QdbRunConfigurationId);
        addSupportedRunConfig(QmlProjectManager::Constants::QML_RUNCONFIG_ID);
        addSupportedDeviceType(Qdb::Constants::QdbLinuxOsType);
    }
};

class QdbQmlToolingWorkerFactory final : public RunWorkerFactory
{
public:
    QdbQmlToolingWorkerFactory()
    {
        setId("QdbQmlToolingWorkerFactory");
        setRecipeProducer([](RunControl *runControl) {
            runControl->requestQmlChannel();
            const ProcessTask inferior(qdbDeviceInferiorProcess(
                runControl, servicesForRunMode(runControl->runMode()), {.setupCanceler = false}));
            return Group {
                When (inferior, &Process::started, WorkflowPolicy::StopOnSuccessOrError) >> Do {
                    runControl->createRecipe(runnerIdForRunMode(runControl->runMode()))
                }
            };
        });
        addSupportedRunMode(ProjectExplorer::Constants::QML_PROFILER_RUN_MODE);
        addSupportedRunMode(ProjectExplorer::Constants::QML_PREVIEW_RUN_MODE);
        addSupportedRunConfig(Constants::QdbRunConfigurationId);
        addSupportedRunConfig(QmlProjectManager::Constants::QML_RUNCONFIG_ID);
        addSupportedDeviceType(Qdb::Constants::QdbLinuxOsType);
    }
};

class QdbPerfProfilerWorkerFactory final : public RunWorkerFactory
{
public:
    QdbPerfProfilerWorkerFactory()
    {
        setId("QdbPerfProfilerWorkerFactory");
        setRecipeProducer([](RunControl *runControl) {
            runControl->requestPerfChannel();
            return runControl->processRecipe(qdbDeviceInferiorProcess(
                runControl, NoQmlDebugServices, {.suppressDefaultStdOutHandling = true}));
        });
        addSupportedRunMode(ProjectExplorer::Constants::PERFPROFILER_RUNNER);
        addSupportedDeviceType(Qdb::Constants::QdbLinuxOsType);
        addSupportedRunConfig(Constants::QdbRunConfigurationId);
    }
};

void setupQdbRunWorkers()
{
    static QdbRunWorkerFactory theQdbRunWorkerFactory;
    static QdbDebugWorkerFactory theQdbDebugWorkerFactory;
    static QdbQmlToolingWorkerFactory theQdbQmlToolingWorkerFactory;
    static QdbPerfProfilerWorkerFactory theQdbProfilerWorkerFactory;
}

} // Qdb::Internal
