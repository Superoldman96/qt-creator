// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "debuggerengine.h"

#include "debuggerinternalconstants.h"
#include "debuggeractions.h"
#include "debuggercore.h"
#include "debuggerdialogs.h"
#include "debuggericons.h"
#include "debuggerkitaspect.h"
#include "debuggerrunconfigurationaspect.h"
#include "debuggertooltipmanager.h"
#include "debuggertr.h"

#include "breakhandler.h"
#include "debuggermainwindow.h"
#include "disassembleragent.h"
#include "enginemanager.h"
#include "localsandexpressionswindow.h"
#include "logwindow.h"
#include "memoryagent.h"
#include "moduleshandler.h"
#include "registerhandler.h"
#include "peripheralregisterhandler.h"
#include "shared/peutils.h"
#include "sourcefileshandler.h"
#include "sourceutils.h"
#include "stackhandler.h"
#include "stackwindow.h"
#include "threadshandler.h"
#include "watchhandler.h"
#include "watchutils.h"
#include "watchwindow.h"

#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/editormanager/ieditor.h>
#include <coreplugin/icore.h>
#include <coreplugin/idocument.h>
#include <coreplugin/messagebox.h>
#include <coreplugin/modemanager.h>
#include <coreplugin/progressmanager/progressmanager.h>
#include <coreplugin/progressmanager/futureprogress.h>

#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/devicesupport/devicemanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/projectmanager.h>
#include <projectexplorer/qmldebugcommandlinearguments.h>
#include <projectexplorer/runcontrol.h>
#include <projectexplorer/sysrootkitaspect.h>
#include <projectexplorer/toolchainkitaspect.h>

#include <qtsupport/qtkitaspect.h>

#include <texteditor/texteditor.h>
#include <texteditor/texteditorsettings.h>
#include <texteditor/fontsettings.h>

#include <utils/algorithm.h>
#include <utils/basetreeview.h>
#include <utils/checkablemessagebox.h>
#include <utils/fileutils.h>
#include <utils/macroexpander.h>
#include <utils/processhandle.h>
#include <utils/processinterface.h>
#include <utils/qtcassert.h>
#include <utils/qtcprocess.h>
#include <utils/styledbar.h>
#include <utils/url.h>
#include <utils/utilsicons.h>

#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QDockWidget>
#include <QHeaderView>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

using namespace Core;
using namespace Debugger::Internal;
using namespace ProjectExplorer;
using namespace TextEditor;
using namespace Utils;

//#define WITH_BENCHMARK
#ifdef WITH_BENCHMARK
#include <valgrind/callgrind.h>
#endif

namespace Debugger {

QDebug operator<<(QDebug d, DebuggerState state)
{
    //return d << DebuggerEngine::stateName(state) << '(' << int(state) << ')';
    return d << DebuggerEngine::stateName(state);
}

QDebug operator<<(QDebug str, const DebuggerRunParameters &rp)
{
    QDebug nospace = str.nospace();
    nospace << "executable=" << rp.inferior().command.executable()
            << " coreFile=" << rp.coreFile()
            << " processArgs=" << rp.inferior().command.arguments()
            << " inferior environment=<" << rp.inferior().environment.toStringList().size() << " variables>"
            << " debugger environment=<" << rp.debugger().environment.toStringList().size() << " variables>"
            << " workingDir=" << rp.inferior().workingDirectory
            << " attachPID=" << rp.attachPid().pid()
            << " remoteChannel=" << rp.remoteChannel()
            << " abi=" << rp.toolChainAbi().toString() << '\n';
    return str;
}

DebuggerRunParameters DebuggerRunParameters::fromRunControl(ProjectExplorer::RunControl *runControl)
{
    Kit *kit = runControl->kit();
    QTC_ASSERT(kit, return {});

    DebuggerRunParameters params;

    params.m_attachPid = runControl->attachPid();
    params.m_displayName = runControl->displayName();

    if (auto symbolsAspect = runControl->aspectData<SymbolFileAspect>())
        params.setSymbolFile(symbolsAspect->filePath);
    if (auto terminalAspect = runControl->aspectData<TerminalAspect>())
        params.m_useTerminal = terminalAspect->useTerminal;
    if (auto runAsRootAspect = runControl->aspectData<RunAsRootAspect>())
        params.m_runAsRoot = runAsRootAspect->value;

    params.setSysRoot(SysRootKitAspect::sysRoot(kit));
    params.m_macroExpander = runControl->macroExpander();
    params.m_debugger = DebuggerKitAspect::runnable(kit);
    params.m_cppEngineType = DebuggerKitAspect::engineType(kit);
    params.m_version = DebuggerKitAspect::version(kit);

    if (QtSupport::QtVersion *qtVersion = QtSupport::QtKitAspect::qtVersion(kit))
        params.m_qtSourceLocation = qtVersion->sourcePath();

    if (auto aspect = runControl->aspectData<DebuggerRunConfigurationAspect>()) {
        if (!aspect->useCppDebugger)
            params.m_cppEngineType = NoEngineType;
        params.m_isQmlDebugging = aspect->useQmlDebugger;
        params.m_isPythonDebugging = aspect->usePythonDebugger;
        params.m_multiProcess = aspect->useMultiProcess;
        params.m_additionalStartupCommands = aspect->overrideStartup;

        if (aspect->useCppDebugger) {
            if (DebuggerKitAspect::debugger(kit)) {
                const Tasks tasks = DebuggerKitAspect::validateDebugger(kit);
                for (const Task &t : tasks) {
                    if (!t.isWarning())
                        params.m_validationErrors.append(t.description());
                }
            } else {
                params.m_validationErrors.append(Tr::tr("The kit does not have a debugger set."));
            }
        }
    }

    ProcessRunData inferior = runControl->runnable();
    // Normalize to work around QTBUG-17529 (QtDeclarative fails with 'File name case mismatch'...)
    inferior.workingDirectory = inferior.workingDirectory.normalizedPathName();
    params.setInferior(inferior);

    const QString envBinary = qtcEnvironmentVariable("QTC_DEBUGGER_PATH");
    if (!envBinary.isEmpty())
        params.m_debugger.command.setExecutable(FilePath::fromString(envBinary));

    if (Project *project = runControl->project()) {
        params.m_projectSourceDirectory = project->projectDirectory();
        params.m_projectSourceFiles = project->files(Project::SourceFiles);
    } else {
        params.m_projectSourceDirectory = params.debugger().command.executable().parentDir();
        params.m_projectSourceFiles.clear();
    }

    params.m_toolChainAbi = ToolchainKitAspect::targetAbi(kit);

    bool ok = false;
    const int nativeMixedOverride = qtcEnvironmentVariableIntValue("QTC_DEBUGGER_NATIVE_MIXED", &ok);
    if (ok)
        params.m_nativeMixedEnabled = bool(nativeMixedOverride);

    if (QtSupport::QtVersion *baseQtVersion = QtSupport::QtKitAspect::qtVersion(kit)) {
        const QVersionNumber qtVersion = baseQtVersion->qtVersion();
        params.m_qtNamespace = baseQtVersion->qtNamespace();
        params.m_qtVersion = 0x10000 * qtVersion.majorVersion()
                           + 0x100 * qtVersion.minorVersion()
                           + qtVersion.microVersion();
    }

    return params;
}

static bool breakOnMainNextTime = false;

void DebuggerRunParameters::setBreakOnMainNextTime()
{
    breakOnMainNextTime = true;
}

void DebuggerRunParameters::setupPortsGatherer(ProjectExplorer::RunControl *runControl) const
{
    if (isCppDebugging())
        runControl->requestDebugChannel();
    if (isQmlDebugging())
        runControl->requestQmlChannel();
}

Result<> DebuggerRunParameters::fixupParameters(ProjectExplorer::RunControl *runControl)
{
    if (m_symbolFile.isEmpty())
        m_symbolFile = m_inferior.command.executable();

    // Set a Qt Creator-specific environment variable, to able to check for it in debugger
    // scripts.
    m_debugger.environment.set("QTC_DEBUGGER_PROCESS", "1");

    // Copy over DYLD_IMAGE_SUFFIX etc
    for (const auto &var :
         QStringList({"DYLD_IMAGE_SUFFIX", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"}))
        if (m_inferior.environment.hasKey(var))
            m_debugger.environment.set(var, m_inferior.environment.expandedValueForKey(var));

    // validate debugger if C++ debugging is enabled
    if (!m_validationErrors.isEmpty())
        return ResultError(m_validationErrors.join('\n'));

    if (m_isQmlDebugging) {
        const auto device = runControl->device();
        if (device && device->type() == ProjectExplorer::Constants::DESKTOP_DEVICE_TYPE) {
            if (m_qmlServer.port() <= 0) {
                m_qmlServer = Utils::urlFromLocalHostAndFreePort();
                if (m_qmlServer.port() <= 0)
                    return ResultError(Tr::tr("Not enough free ports for QML debugging."));
            }
            // Makes sure that all bindings go through the JavaScript engine, so that
            // breakpoints are actually hit!
            const QString optimizerKey = "QML_DISABLE_OPTIMIZER";
            if (!m_inferior.environment.hasKey(optimizerKey))
                m_inferior.environment.set(optimizerKey, "1");
        }
    }

    if (settings().autoEnrichParameters()) {
        if (m_debugInfoLocation.isEmpty())
            m_debugInfoLocation = m_sysRoot / "/usr/lib/debug";
        if (m_debugSourceLocation.isEmpty()) {
            const QString base = m_sysRoot.toUrlishString() + "/usr/src/debug/";
            m_debugSourceLocation.append(base + "qt5base/src/corelib");
            m_debugSourceLocation.append(base + "qt5base/src/gui");
            m_debugSourceLocation.append(base + "qt5base/src/network");
        }
    }

    if (m_isQmlDebugging) {
        QmlDebugServicesPreset service;
        if (isCppDebugging()) {
            if (m_nativeMixedEnabled) {
                service = QmlNativeDebuggerServices;
            } else {
                service = QmlDebuggerServices;
            }
        } else {
            service = QmlDebuggerServices;
        }
        if (m_startMode != AttachToLocalProcess && m_startMode != AttachToCrashedProcess) {
            const QString qmlarg = isCppDebugging() && m_nativeMixedEnabled
                                 ? qmlDebugNativeArguments(service, false)
                                 : qmlDebugTcpArguments(service, m_qmlServer);
            m_inferior.command.addArg(qmlarg);
        }
    }

    if (m_startMode == NoStartMode)
        m_startMode = StartInternal;

    if (breakOnMainNextTime) {
        m_breakOnMain = true;
        breakOnMainNextTime = false;
    }

    if (HostOsInfo::isWindowsHost()) {
        // Otherwise command lines with '> tmp.log' hang.
        ProcessArgs::SplitError perr;
        ProcessArgs::prepareShellArgs(m_inferior.command.arguments(), &perr,
                                 HostOsInfo::hostOs(), nullptr,
                                 m_inferior.workingDirectory);
        if (perr != ProcessArgs::SplitOk) {
            // perr == BadQuoting is never returned on Windows
            // FIXME? QTCREATORBUG-2809
            return ResultError(Tr::tr("Debugging complex command lines "
                                        "is currently not supported on Windows."));
        }
    }

    if (isNativeMixedDebugging())
        m_inferior.environment.set("QV4_FORCE_INTERPRETER", "1");

    if (settings().forceLoggingToConsole())
        m_inferior.environment.set("QT_LOGGING_TO_CONSOLE", "1");

    return ResultOk;
}

void DebuggerRunParameters::setStartMode(DebuggerStartMode startMode)
{
    m_startMode = startMode;
    if (startMode != AttachToQmlServer)
        return;

    m_cppEngineType = NoEngineType;
    m_isQmlDebugging = true;
    m_closeMode = KillAtClose;

    // FIXME: This is horribly wrong.
    // get files from all the projects in the session
    QList<Project *> projects = ProjectManager::projects();
    if (Project *startupProject = ProjectManager::startupProject()) {
        // startup project first
        projects.removeOne(startupProject);
        projects.insert(0, startupProject);
    }
    for (Project *project : std::as_const(projects))
        m_projectSourceFiles.append(project->files(Project::SourceFiles));
    if (!projects.isEmpty())
        m_projectSourceDirectory = projects.first()->projectDirectory();
}

void DebuggerRunParameters::addSolibSearchDir(const QString &str)
{
    QString path = str;
    path.replace("%{sysroot}", m_sysRoot.toUrlishString());
    m_solibSearchPath.append(FilePath::fromString(path));
}

static QStringList splitCommandHelper(const QString &text, const MacroExpander *expander)
{
    QStringList result;

    if (!text.isEmpty()) {
        const QStringList commands = expander->expand(text).split('\n');
        for (const QString &command : commands) {
            const QString trimmed = command.trimmed();
            if (!trimmed.isEmpty())
                result.append(trimmed);
        }
    }

    return result;
}

QStringList DebuggerRunParameters::commandsAfterConnect() const
{
    return splitCommandHelper(m_commandsAfterConnect, m_macroExpander);
}

QStringList DebuggerRunParameters::commandsForReset() const
{
    return splitCommandHelper(m_commandsForReset, m_macroExpander);
}

bool DebuggerRunParameters::isCppDebugging() const
{
    return cppEngineType() != NoEngineType;
}

bool DebuggerRunParameters::isNativeMixedDebugging() const
{
    return m_nativeMixedEnabled && isCppDebugging() && m_isQmlDebugging;
}

FilePaths DebuggerRunParameters::findQmlFile(const QUrl &url) const
{
    return m_qmlFileFinder.findFile(url);
}

void DebuggerRunParameters::populateQmlFileFinder(const RunControl *runControl)
{
    m_qmlFileFinder.setProjectDirectory(projectSourceDirectory());
    m_qmlFileFinder.setProjectFiles(projectSourceFiles());
    m_qmlFileFinder.setAdditionalSearchDirectories(additionalSearchDirectories());
    m_qmlFileFinder.setSysroot(sysRoot());
    QtSupport::QtVersion::populateQmlFileFinder(&m_qmlFileFinder, runControl->buildConfiguration());
}

namespace Internal {

static bool debuggerActionsEnabledHelper(DebuggerState state)
{
    switch (state) {
    case InferiorRunOk:
    case InferiorUnrunnable:
    case InferiorStopOk:
        return true;
    case InferiorStopRequested:
    case InferiorRunRequested:
    case InferiorRunFailed:
    case DebuggerNotReady:
    case EngineSetupRequested:
    case EngineSetupFailed:
    case EngineRunRequested:
    case EngineRunFailed:
    case InferiorStopFailed:
    case InferiorShutdownRequested:
    case InferiorShutdownFinished:
    case EngineShutdownRequested:
    case EngineShutdownFinished:
    case DebuggerFinished:
        return false;
    }
    return false;
}

Location::Location(const StackFrame &frame, bool marker)
{
    m_fileName = frame.file;
    m_textPosition = {frame.line, -1};
    m_needsMarker = marker;
    m_functionName = frame.function;
    m_hasDebugInfo = frame.isUsable();
    m_address = frame.address;
    m_from = frame.module;
}

LocationMark::LocationMark(DebuggerEngine *engine, const FilePath &file, int line)
    : TextMark(file, line, {Tr::tr("Debugger Location"), Constants::TEXT_MARK_CATEGORY_LOCATION})
    , m_engine(engine)
{
    setPriority(TextMark::HighPriority);
    setIsLocationMarker(true);
    updateIcon();
}

void LocationMark::updateIcon()
{
    const Icon *icon = &Icons::WATCHPOINT;
    if (m_engine && EngineManager::currentEngine() == m_engine)
        icon = m_engine->isReverseDebugging() ? &Icons::REVERSE_LOCATION : &Icons::LOCATION;
    setIcon(icon->icon());
}

bool LocationMark::isDraggable() const
{
    return m_engine && m_engine->hasCapability(JumpToLineCapability);
}

void LocationMark::dragToLine(int line)
{
    if (m_engine) {
        if (BaseTextEditor *textEditor = BaseTextEditor::currentTextEditor()) {
            ContextData location = getLocationContext(textEditor->textDocument(), line);
            if (location.isValid())
                m_engine->executeJumpToLine(location);
        }
    }
}

//////////////////////////////////////////////////////////////////////
//
// MemoryAgentSet
//
//////////////////////////////////////////////////////////////////////

class MemoryAgentSet
{
public:
    ~MemoryAgentSet()
    {
        qDeleteAll(m_agents);
        m_agents.clear();
    }

    // Called by engine to create a new view.
    void createBinEditor(const MemoryViewSetupData &data, DebuggerEngine *engine)
    {
        auto agent = new MemoryAgent(data, engine);
        if (agent->isUsable()) {
            m_agents.push_back(agent);
        } else {
            delete agent;
            AsynchronousMessageBox::warning(
                        Tr::tr("No Memory Viewer Available"),
                        Tr::tr("The memory contents cannot be shown as no viewer plugin "
                                           "for binary data has been loaded."));
        }
    }

    // On stack frame completed and on request.
    void updateContents()
    {
        for (MemoryAgent *agent : m_agents) {
            if (agent)
                agent->updateContents();
        }
    }

    void handleDebuggerFinished()
    {
        for (MemoryAgent *agent : m_agents) {
            if (agent)
                agent->setFinished(); // Prevent triggering updates, etc.
        }
    }

private:
    std::vector<MemoryAgent *> m_agents;
};



//////////////////////////////////////////////////////////////////////
//
// DebuggerEnginePrivate
//
//////////////////////////////////////////////////////////////////////

class DebuggerEnginePrivate : public QObject
{
    Q_OBJECT

public:
    DebuggerEnginePrivate(DebuggerEngine *engine)
        : m_engine(engine),
          m_breakHandler(engine),
          m_modulesHandler(engine),
          m_registerHandler(engine),
          m_peripheralRegisterHandler(engine),
          m_sourceFilesHandler(engine),
          m_stackHandler(engine),
          m_threadsHandler(engine),
          m_watchHandler(engine),
          m_disassemblerAgent(engine),
          m_toolTipManager(engine)
    {
        m_debuggerName = Tr::tr("Debugger");

        m_logWindow = new LogWindow(m_engine); // Needed before start()
        m_logWindow->setObjectName("Debugger.Dock.Output");

        connect(&settings().enableReverseDebugging, &BaseAspect::changed, this, [this] {
            updateState();
            for (const QPointer<DebuggerEngine> &companion : std::as_const(m_companionEngines))
                companion->d->updateState();
        });
        static int contextCount = 0;
        m_context = Context(Id("Debugger.Engine.").withSuffix(++contextCount));

        ActionManager::registerAction(&m_continueAction, Constants::CONTINUE, m_context);
        ActionManager::registerAction(&m_exitAction, Constants::STOP, m_context);
        ActionManager::registerAction(&m_interruptAction, Constants::INTERRUPT, m_context);
        ActionManager::registerAction(&m_abortAction, Constants::ABORT, m_context);
        ActionManager::registerAction(&m_stepOverAction, Constants::NEXT, m_context);
        ActionManager::registerAction(&m_stepIntoAction, Constants::STEP, m_context);
        ActionManager::registerAction(&m_stepOutAction, Constants::STEPOUT, m_context);
        ActionManager::registerAction(&m_runToLineAction, Constants::RUNTOLINE, m_context);
        ActionManager::registerAction(&m_runToSelectedFunctionAction, Constants::RUNTOSELECTEDFUNCTION, m_context);
        ActionManager::registerAction(&m_jumpToLineAction, Constants::JUMPTOLINE, m_context);
        ActionManager::registerAction(&m_returnFromFunctionAction, Constants::RETURNFROMFUNCTION, m_context);
        ActionManager::registerAction(&m_detachAction, Constants::DETACH, m_context);
        ActionManager::registerAction(&m_resetAction, Constants::RESET, m_context);
        ActionManager::registerAction(&m_watchAction, Constants::WATCH, m_context);
        ActionManager::registerAction(&m_operateByInstructionAction, Constants::OPERATE_BY_INSTRUCTION, m_context);
        ActionManager::registerAction(&m_openMemoryEditorAction, Constants::OPEN_MEMORY_EDITOR, m_context);
        ActionManager::registerAction(&m_frameUpAction, Constants::FRAME_UP, m_context);
        ActionManager::registerAction(&m_frameDownAction, Constants::FRAME_DOWN, m_context);
    }

    ~DebuggerEnginePrivate()
    {
        ActionManager::unregisterAction(&m_continueAction, Constants::CONTINUE);
        ActionManager::unregisterAction(&m_exitAction, Constants::STOP);
        ActionManager::unregisterAction(&m_interruptAction, Constants::INTERRUPT);
        ActionManager::unregisterAction(&m_abortAction, Constants::ABORT);
        ActionManager::unregisterAction(&m_stepOverAction, Constants::NEXT);
        ActionManager::unregisterAction(&m_stepIntoAction, Constants::STEP);
        ActionManager::unregisterAction(&m_stepOutAction, Constants::STEPOUT);
        ActionManager::unregisterAction(&m_runToLineAction, Constants::RUNTOLINE);
        ActionManager::unregisterAction(&m_runToSelectedFunctionAction, Constants::RUNTOSELECTEDFUNCTION);
        ActionManager::unregisterAction(&m_jumpToLineAction, Constants::JUMPTOLINE);
        ActionManager::unregisterAction(&m_returnFromFunctionAction, Constants::RETURNFROMFUNCTION);
        ActionManager::unregisterAction(&m_detachAction, Constants::DETACH);
        ActionManager::unregisterAction(&m_resetAction, Constants::RESET);
        ActionManager::unregisterAction(&m_watchAction, Constants::WATCH);
        ActionManager::unregisterAction(&m_operateByInstructionAction, Constants::OPERATE_BY_INSTRUCTION);
        ActionManager::unregisterAction(&m_openMemoryEditorAction, Constants::OPEN_MEMORY_EDITOR);
        ActionManager::unregisterAction(&m_frameUpAction, Constants::FRAME_UP);
        ActionManager::unregisterAction(&m_frameDownAction, Constants::FRAME_DOWN);
        destroyPerspective();

        delete m_logWindow;
        delete m_breakWindow;
        delete m_returnWindow;
        delete m_localsWindow;
        delete m_watchersWindow;
        delete m_inspectorWindow;
        delete m_registerWindow;
        delete m_peripheralRegisterWindow;
        delete m_modulesWindow;
        delete m_sourceFilesWindow;
        delete m_stackWindow;
        delete m_threadsWindow;

        delete m_breakView;
        delete m_returnView;
        delete m_localsView;
        delete m_watchersView;
        delete m_inspectorView;
        delete m_registerView;
        delete m_peripheralRegisterView;
        delete m_modulesView;
        delete m_sourceFilesView;
        delete m_stackView;
        delete m_threadsView;
    }

    void updateActionToolTips()
    {
        // update tooltips that are visible on the button in the mode selector
        const QString displayName = m_engine->displayName();
        m_continueAction.setToolTip(Tr::tr("Continue %1").arg(displayName));
        m_interruptAction.setToolTip(Tr::tr("Interrupt %1").arg(displayName));
    }

    void setupViews();

    void destroyPerspective()
    {
        if (!m_perspective)
            return;

        Perspective *perspective = m_perspective;
        m_perspective = nullptr;

        EngineManager::unregisterEngine(m_engine);

        // This triggers activity in the EngineManager which
        // recognizes the rampdown by the m_perpective == nullptr above.
        perspective->destroy();

        // disconnect the follow font size connection
        TextEditorSettings::instance()->disconnect(this);

        delete perspective;
    }

    void updateReturnViewHeader(int section, int, int newSize)
    {
        if (m_perspective && m_returnView && m_returnView->header())
            m_returnView->header()->resizeSection(section, newSize);
    }

    void doShutdownEngine()
    {
        m_engine->setState(EngineShutdownRequested);
        m_engine->startDying();
        m_engine->showMessage("CALL: SHUTDOWN ENGINE");
        m_engine->shutdownEngine();
    }

    void doShutdownInferior()
    {
        m_engine->setState(InferiorShutdownRequested);
        //QTC_ASSERT(isMasterEngine(), return);
        resetLocation();
        m_engine->showMessage("CALL: SHUTDOWN INFERIOR");
        m_engine->shutdownInferior();
    }

    void doFinishDebugger()
    {
        QTC_ASSERT(m_state == EngineShutdownFinished, qDebug() << m_state);
        resetLocation();
        m_progress.setProgressValue(1000);
        m_progress.reportFinished();
        m_modulesHandler.removeAll();
        m_stackHandler.removeAll();
        m_threadsHandler.removeAll();
        m_watchHandler.cleanup();
        m_engine->showMessage(Tr::tr("Debugger finished."), StatusBar);
        m_engine->setState(DebuggerFinished); // Also destroys views.
        if (settings().switchModeOnExit())
            EngineManager::deactivateDebugMode();
    }

    void scheduleResetLocation()
    {
        m_stackHandler.scheduleResetLocation();
        m_watchHandler.scheduleResetLocation();
        m_disassemblerAgent.scheduleResetLocation();
        m_locationTimer.setSingleShot(true);
        m_locationTimer.start(80);
    }

    void resetLocation()
    {
        m_lookupRequests.clear();
        m_locationTimer.stop();
        m_locationMark.reset();
        m_stackHandler.resetLocation();
        m_disassemblerAgent.resetLocation();
        m_toolTipManager.resetLocation();
        m_breakHandler.resetLocation();
    }

public:
    void setInitialActionStates();
    void setBusyCursor(bool on);
    void cleanupViews();
    void updateState();
    void updateReverseActions();

    DebuggerEngine *m_engine = nullptr; // Not owned.
    QString m_runId;
    QString m_debuggerName;
    QString m_debuggerType;
    QPointer<Perspective> m_perspective;
    DebuggerRunParameters m_runParameters;
    IDevice::ConstPtr m_device;

    QList<QPointer<DebuggerEngine>> m_companionEngines;
    bool m_isPrimaryEngine = true;

    // The current state.
    DebuggerState m_state = DebuggerNotReady;

    ProcessHandle m_inferiorPid;

    BreakHandler m_breakHandler;
    ModulesHandler m_modulesHandler;
    RegisterHandler m_registerHandler;
    PeripheralRegisterHandler m_peripheralRegisterHandler;
    SourceFilesHandler m_sourceFilesHandler;
    StackHandler m_stackHandler;
    ThreadsHandler m_threadsHandler;
    WatchHandler m_watchHandler;
    QFutureInterface<void> m_progress;

    DisassemblerAgent m_disassemblerAgent;
    MemoryAgentSet m_memoryAgents;
    QScopedPointer<LocationMark> m_locationMark;
    QTimer m_locationTimer;

    QString m_qtNamespace;

    // Safety net to avoid infinite lookups.
    QHash<QString, int> m_lookupRequests; // FIXME: Integrate properly.
    QPointer<QWidget> m_alertBox;

    QPointer<BaseTreeView> m_breakView;
    QPointer<BaseTreeView> m_returnView;
    QPointer<BaseTreeView> m_localsView;
    QPointer<BaseTreeView> m_watchersView;
    QPointer<WatchTreeView> m_inspectorView;
    QPointer<BaseTreeView> m_registerView;
    QPointer<BaseTreeView> m_peripheralRegisterView;
    QPointer<BaseTreeView> m_modulesView;
    QPointer<BaseTreeView> m_sourceFilesView;
    QPointer<BaseTreeView> m_stackView;
    QPointer<BaseTreeView> m_threadsView;
    QPointer<QWidget> m_breakWindow;
    QPointer<QWidget> m_returnWindow;
    QPointer<QWidget> m_localsWindow;
    QPointer<QWidget> m_watchersWindow;
    QPointer<QWidget> m_inspectorWindow;
    QPointer<QWidget> m_registerWindow;
    QPointer<QWidget> m_peripheralRegisterWindow;
    QPointer<QWidget> m_modulesWindow;
    QPointer<QWidget> m_sourceFilesWindow;
    QPointer<QWidget> m_stackWindow;
    QPointer<QWidget> m_threadsWindow;
    QPointer<LogWindow> m_logWindow;
    QPointer<LocalsAndInspectorWindow> m_localsAndInspectorWindow;

    QPointer<QLabel> m_threadLabel;

    bool m_busy = false;
    bool m_isDying = false;

    QAction m_detachAction;
    OptionalAction m_continueAction{Tr::tr("Continue")};
    QAction m_exitAction{Tr::tr("Stop Debugger")}; // On application output button if "Stop" is possible
    OptionalAction m_interruptAction{Tr::tr("Interrupt")}; // On the fat debug button if "Pause" is possible
    QAction m_abortAction{Tr::tr("Abort Debugging")};
    QAction m_stepIntoAction{Tr::tr("Step Into")};
    QAction m_stepOutAction{Tr::tr("Step Out")};
    QAction m_toggleEnableBreakpointsAction{Tr::tr("Disable All Breakpoints")};
    QAction m_runToLineAction{Tr::tr("Run to Line")}; // In the debug menu
    QAction m_runToSelectedFunctionAction{Tr::tr("Run to Selected Function")};
    QAction m_jumpToLineAction{Tr::tr("Jump to Line")};
    QAction m_frameUpAction{Tr::tr("Move to Calling Frame")};
    QAction m_frameDownAction{Tr::tr("Move to Called Frame")};
    QAction m_openMemoryEditorAction{Tr::tr("Memory...")};
    // In the Debug menu.
    QAction m_returnFromFunctionAction{Tr::tr("Immediately Return From Inner Function")};
    QAction m_stepOverAction{Tr::tr("Step Over")};
    QAction m_watchAction{Tr::tr("Add Expression Evaluator")};
    QAction m_setOrRemoveBreakpointAction{Tr::tr("Set or Remove Breakpoint")};
    QAction m_enableOrDisableBreakpointAction{Tr::tr("Enable or Disable Breakpoint")};
    QAction m_resetAction{Tr::tr("Restart Debugging")};
    OptionalAction m_operateByInstructionAction{Tr::tr("Operate by Instruction")};
    QAction m_recordForReverseOperationAction{Tr::tr("Record Information to Allow Reversal of Direction")};
    OptionalAction m_operateInReverseDirectionAction{Tr::tr("Reverse Direction")};
    OptionalAction m_snapshotAction{Tr::tr("Take Snapshot of Process State")};

    DebuggerToolTipManager m_toolTipManager;
    Context m_context;
};

void DebuggerEnginePrivate::setupViews()
{
    const DebuggerRunParameters &rp = m_runParameters;
    const QString engineId = EngineManager::registerEngine(m_engine);

    QTC_CHECK(!m_perspective);

    Perspective *currentPerspective = DebuggerMainWindow::instance()->currentPerspective();

    const QString perspectiveId = "Debugger.Perspective." + m_runId + '.' + m_debuggerName;
    const QString settingsId = "Debugger.Perspective." + m_debuggerName;
    const QString parentPerspectiveId = currentPerspective ? currentPerspective->id()
                                                           : Constants::PRESET_PERSPECTIVE_ID;

    m_perspective
        = new Perspective(perspectiveId, m_engine->displayName(), parentPerspectiveId, settingsId);

    m_progress.setProgressRange(0, 1000);
    const QString msg = m_companionEngines.isEmpty()
            ? Tr::tr("Launching Debugger")
            : Tr::tr("Launching %1 Debugger").arg(m_debuggerName);
    FutureProgress *fp = ProgressManager::addTask(m_progress.future(), msg, "Debugger.Launcher");
    connect(fp, &FutureProgress::canceled, m_engine, &DebuggerEngine::quitDebugger);
    m_progress.reportStarted();

    m_inferiorPid = rp.attachPid();
//    if (m_inferiorPid.isValid())
//        m_runControl->setApplicationProcessHandle(m_inferiorPid);

    m_operateByInstructionAction.setEnabled(true);
    m_operateByInstructionAction.setVisible(m_engine->hasCapability(DisassemblerCapability));
    m_operateByInstructionAction.setIcon(Debugger::Icons::SINGLE_INSTRUCTION_MODE.icon());
    m_operateByInstructionAction.setCheckable(true);
    m_operateByInstructionAction.setChecked(false);
    m_operateByInstructionAction.setToolTip("<p>" + Tr::tr("Switches the debugger to instruction-wise "
        "operation mode. In this mode, stepping operates on single "
        "instructions and the source location view also shows the "
        "disassembled instructions."));
    m_operateByInstructionAction.setIconVisibleInMenu(false);
    connect(&m_operateByInstructionAction, &QAction::triggered,
            m_engine, &DebuggerEngine::operateByInstructionTriggered);

    m_frameDownAction.setEnabled(true);
    connect(&m_frameDownAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleFrameDown);

    m_frameUpAction.setEnabled(true);
    connect(&m_frameUpAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleFrameUp);

    m_openMemoryEditorAction.setEnabled(true);
    m_openMemoryEditorAction.setVisible(m_engine->hasCapability(ShowMemoryCapability));
    connect(&m_openMemoryEditorAction, &QAction::triggered,
            m_engine, &DebuggerEngine::openMemoryEditor);

    QTC_ASSERT(m_state == DebuggerNotReady || m_state == DebuggerFinished, qDebug() << m_state);
    m_progress.setProgressValue(200);

    connect(&m_locationTimer, &QTimer::timeout,
            this, &DebuggerEnginePrivate::resetLocation);

    QtcSettings *settings = ICore::settings();

    m_modulesView = new BaseTreeView;
    m_modulesView->setModel(m_modulesHandler.model());
    m_modulesView->setSortingEnabled(true);
    m_modulesView->setSettings(settings, "Debugger.ModulesView");
    m_modulesView->enableColumnHiding();
    connect(m_modulesView, &BaseTreeView::aboutToShow,
            m_engine, &DebuggerEngine::reloadModules,
            Qt::QueuedConnection);
    m_modulesWindow = addSearch(m_modulesView);
    m_modulesWindow->setObjectName("Debugger.Dock.Modules." + engineId);
    m_modulesWindow->setWindowTitle(Tr::tr("&Modules"));

    m_registerView = new BaseTreeView;
    m_registerView->setModel(m_registerHandler.model());
    m_registerView->setRootIsDecorated(true);
    m_registerView->setSettings(settings, "Debugger.RegisterView");
    m_registerView->enableColumnHiding();
    connect(m_registerView, &BaseTreeView::aboutToShow,
            m_engine, &DebuggerEngine::reloadRegisters,
            Qt::QueuedConnection);
    m_registerWindow = addSearch(m_registerView);
    m_registerWindow->setObjectName("Debugger.Dock.Register." + engineId);
    m_registerWindow->setWindowTitle(Tr::tr("Reg&isters"));

    m_peripheralRegisterView = new BaseTreeView;
    m_peripheralRegisterView->setModel(m_peripheralRegisterHandler.model());
    m_peripheralRegisterView->setRootIsDecorated(true);
    m_peripheralRegisterView->setSettings(settings, "Debugger.PeripheralRegisterView");
    m_peripheralRegisterView->enableColumnHiding();
    connect(m_peripheralRegisterView, &BaseTreeView::aboutToShow,
            m_engine, &DebuggerEngine::reloadPeripheralRegisters,
            Qt::QueuedConnection);
    m_peripheralRegisterWindow = addSearch(m_peripheralRegisterView);
    m_peripheralRegisterWindow->setObjectName("Debugger.Dock.PeripheralRegister." + engineId);
    m_peripheralRegisterWindow->setWindowTitle(Tr::tr("Peripheral Reg&isters"));

    m_stackView = new StackTreeView;
    m_stackView->setModel(m_stackHandler.model());
    m_stackView->setSettings(settings, "Debugger.StackView");
    m_stackView->setIconSize(QSize(10, 10));
    m_stackView->enableColumnHiding();
    m_stackWindow = addSearch(m_stackView);
    m_stackWindow->setObjectName("Debugger.Dock.Stack." + engineId);
    m_stackWindow->setWindowTitle(Tr::tr("&Stack"));

    m_sourceFilesView = new BaseTreeView;
    m_sourceFilesView->setModel(m_sourceFilesHandler.model());
    m_sourceFilesView->setSortingEnabled(true);
    m_sourceFilesView->setSettings(settings, "Debugger.SourceFilesView");
    m_sourceFilesView->enableColumnHiding();
    connect(m_sourceFilesView, &BaseTreeView::aboutToShow,
            m_engine, &DebuggerEngine::reloadSourceFiles,
            Qt::QueuedConnection);
    m_sourceFilesWindow = addSearch(m_sourceFilesView);
    m_sourceFilesWindow->setObjectName("Debugger.Dock.SourceFiles." + engineId);
    m_sourceFilesWindow->setWindowTitle(Tr::tr("Source Files"));

    m_threadsView = new BaseTreeView;
    m_threadsView->setModel(m_threadsHandler.model());
    m_threadsView->setSortingEnabled(true);
    m_threadsView->setSettings(settings, "Debugger.ThreadsView");
    m_threadsView->setIconSize(QSize(10, 10));
    m_threadsView->setSpanColumn(ThreadData::FunctionColumn);
    m_threadsView->enableColumnHiding();
    m_threadsWindow = addSearch(m_threadsView);
    m_threadsWindow->setObjectName("Debugger.Dock.Threads." + engineId);
    m_threadsWindow->setWindowTitle(Tr::tr("&Threads"));

    m_returnView = new WatchTreeView{ReturnType};
    m_returnView->setModel(m_watchHandler.model());
    m_returnWindow = addSearch(m_returnView);
    m_returnWindow->setObjectName("CppDebugReturn");
    m_returnWindow->setWindowTitle(Tr::tr("Locals"));
    m_returnWindow->setVisible(false);

    m_localsView = new WatchTreeView{LocalsType};
    m_localsView->setModel(m_watchHandler.model());
    m_localsView->setSettings(settings, "Debugger.LocalsView");
    m_localsWindow = addSearch(m_localsView);
    m_localsWindow->setObjectName("Debugger.Dock.Locals." + engineId);
    m_localsWindow->setWindowTitle(Tr::tr("Locals"));

    m_inspectorView = new WatchTreeView{InspectType};
    m_inspectorView->setModel(m_watchHandler.model());
    m_inspectorView->setSettings(settings, "Debugger.LocalsView"); // sic! same as locals view.
    m_inspectorWindow = addSearch(m_inspectorView);
    m_inspectorWindow->setObjectName("Debugger.Dock.Inspector." + engineId);
    m_inspectorWindow->setWindowTitle(Tr::tr("Locals"));

    m_watchersView = new WatchTreeView{WatchersType};
    m_watchersView->setModel(m_watchHandler.model());
    m_watchersView->setSettings(settings, "Debugger.WatchersView");
    m_watchersWindow = addSearch(m_watchersView);
    m_watchersWindow->setObjectName("Debugger.Dock.Watchers." + engineId);
    m_watchersWindow->setWindowTitle(Tr::tr("&Expressions"));

    m_localsAndInspectorWindow = new LocalsAndInspectorWindow(
                m_localsWindow, m_inspectorWindow, m_returnWindow);
    m_localsAndInspectorWindow->setObjectName("Debugger.Dock.LocalsAndInspector." + engineId);
    m_localsAndInspectorWindow->setWindowTitle(m_localsWindow->windowTitle());

    // Locals
    connect(m_localsView->header(), &QHeaderView::sectionResized,
            this, &DebuggerEnginePrivate::updateReturnViewHeader, Qt::QueuedConnection);

    m_breakView = new BaseTreeView;
    m_breakView->setIconSize(QSize(10, 10));
    m_breakView->setWindowIcon(Icons::BREAKPOINTS.icon());
    m_breakView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_breakView->setSpanColumn(BreakpointFunctionColumn);
    m_breakView->setSettings(settings, "Debugger.BreakWindow");
    m_breakView->setModel(m_breakHandler.model());
    m_breakView->setRootIsDecorated(true);
    m_breakView->enableColumnHiding();
    m_breakWindow = addSearch(m_breakView);
    m_breakWindow->setObjectName("Debugger.Dock.Break." + engineId);
    m_breakWindow->setWindowTitle(Tr::tr("&Breakpoints"));

    if (currentPerspective && currentPerspective->id() == Constants::DAP_PERSPECTIVE_ID)
        m_perspective->useSubPerspectiveSwitcher(EngineManager::dapEngineChooser());
    else
        m_perspective->useSubPerspectiveSwitcher(EngineManager::engineChooser());

    m_perspective->addToolBarAction(&m_continueAction);
    m_perspective->addToolBarAction(&m_interruptAction);

    m_perspective->addToolBarAction(&m_exitAction);
    m_perspective->addToolBarAction(&m_stepOverAction);
    m_perspective->addToolBarAction(&m_stepIntoAction);
    m_perspective->addToolBarAction(&m_stepOutAction);
    m_perspective->addToolBarAction(&m_resetAction);
    m_perspective->addToolBarAction(&m_operateByInstructionAction);

    connect(&m_detachAction, &QAction::triggered, m_engine, &DebuggerEngine::handleExecDetach);

    m_continueAction.setIcon(Icons::DEBUG_CONTINUE_SMALL_TOOLBAR.icon());
    connect(&m_continueAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleExecContinue);

    m_exitAction.setIcon(Icons::DEBUG_EXIT_SMALL_TOOLBAR.icon());
    connect(&m_exitAction, &QAction::triggered,
            m_engine, &DebuggerEngine::requestRunControlStop);

    m_interruptAction.setIcon(Icons::DEBUG_INTERRUPT_SMALL_TOOLBAR.icon());
    connect(&m_interruptAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleExecInterrupt);

    m_abortAction.setToolTip(Tr::tr("Aborts debugging and resets the debugger to the initial state."));
    connect(&m_abortAction, &QAction::triggered,
            m_engine, &DebuggerEngine::abortDebugger);

    m_resetAction.setToolTip(Tr::tr("Restarts the debugging session."));
    m_resetAction.setIcon(Icons::RESTART_TOOLBAR.icon());
    connect(&m_resetAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleReset);

    m_stepOverAction.setIcon(Icons::STEP_OVER_TOOLBAR.icon());
    connect(&m_stepOverAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleExecStepOver);

    m_stepIntoAction.setIcon(Icons::STEP_INTO_TOOLBAR.icon());
    connect(&m_stepIntoAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleExecStepIn);

    m_stepOutAction.setIcon(Icons::STEP_OUT_TOOLBAR.icon());
    connect(&m_stepOutAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleExecStepOut);

    connect(&m_runToLineAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleExecRunToLine);

    connect(&m_runToSelectedFunctionAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleExecRunToSelectedFunction);

    connect(&m_returnFromFunctionAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleExecReturn);

    connect(&m_jumpToLineAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleExecJumpToLine);

    connect(&m_watchAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleAddToWatchWindow);

    m_toggleEnableBreakpointsAction.setIcon(Icons::BREAKPOINT_DISABLED.icon()); // FIXME better icon
    m_toggleEnableBreakpointsAction.setCheckable(true);
    m_perspective->addToolBarAction(&m_toggleEnableBreakpointsAction);
    connect(&m_toggleEnableBreakpointsAction, &QAction::triggered,
            m_engine, [this](bool checked) {
        BreakHandler *handler = m_engine->breakHandler();
        const auto bps = m_engine->breakHandler()->breakpoints();
        for (const auto &bp : bps) {
            if (auto gbp = bp->globalBreakpoint())
                gbp->setEnabled(!checked, false);
            handler->requestBreakpointEnabling(bp, !checked);
        }
    });
    connect(m_engine->breakHandler(), &BreakHandler::dataChanged,
            m_engine, [this] {
        const auto bps = m_engine->breakHandler()->breakpoints();
        const auto [enabled, disabled] = Utils::partition(bps, &BreakpointItem::isEnabled);
        if (!enabled.isEmpty() && !disabled.isEmpty())
            return;
        m_toggleEnableBreakpointsAction.setChecked(!disabled.isEmpty());
    });

    m_perspective->addToolBarAction(&m_recordForReverseOperationAction);
    connect(&m_recordForReverseOperationAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleRecordReverse);

    m_perspective->addToolBarAction(&m_operateInReverseDirectionAction);
    connect(&m_operateInReverseDirectionAction, &QAction::triggered,
            m_engine, &DebuggerEngine::handleReverseDirection);

    m_perspective->addToolBarAction(&m_snapshotAction);
    connect(&m_snapshotAction, &QAction::triggered,
            m_engine, &DebuggerEngine::createSnapshot);

    m_perspective->addToolbarSeparator();

    m_threadLabel = new QLabel(Tr::tr("Threads:"));
    m_perspective->addToolBarWidget(m_threadLabel);
    m_perspective->addToolBarWidget(m_threadsHandler.threadSwitcher());

    connect(TextEditorSettings::instance(), &TextEditorSettings::fontSettingsChanged,
            this, [this](const FontSettings &fs) {
        if (!Internal::settings().fontSizeFollowsEditor())
            return;
        const qreal size = fs.fontZoom() * fs.fontSize() / 100.;
        QFont font = m_breakWindow->font();
        font.setPointSizeF(size);
        m_breakWindow->setFont(font);
        m_logWindow->setFont(font);
        m_localsWindow->setFont(font);
        m_modulesWindow->setFont(font);
        //m_consoleWindow->setFont(font);
        m_registerWindow->setFont(font);
        m_peripheralRegisterWindow->setFont(font);
        m_returnWindow->setFont(font);
        m_sourceFilesWindow->setFont(font);
        m_stackWindow->setFont(font);
        m_threadsWindow->setFont(font);
        m_watchersWindow->setFont(font);
        m_inspectorWindow->setFont(font);
    });

    m_perspective->addWindow(m_stackWindow, Perspective::SplitVertical, nullptr);
    m_perspective->addWindow(m_breakWindow, Perspective::SplitHorizontal, m_stackWindow);
    m_perspective->addWindow(m_threadsWindow, Perspective::AddToTab, m_breakWindow);
    m_perspective->addWindow(m_modulesWindow, Perspective::AddToTab, m_threadsWindow, false);
    m_perspective->addWindow(m_sourceFilesWindow, Perspective::AddToTab, m_modulesWindow, false);
    m_perspective->addWindow(m_localsAndInspectorWindow, Perspective::AddToTab, nullptr, true, Qt::RightDockWidgetArea);
    m_perspective->addWindow(m_watchersWindow, Perspective::SplitVertical, m_localsAndInspectorWindow, true, Qt::RightDockWidgetArea);
    m_perspective->addWindow(m_registerWindow, Perspective::AddToTab, m_localsAndInspectorWindow, false, Qt::RightDockWidgetArea);
    m_perspective->addWindow(m_peripheralRegisterWindow, Perspective::AddToTab, m_localsAndInspectorWindow, false, Qt::RightDockWidgetArea);
    m_perspective->addWindow(m_logWindow, Perspective::AddToTab, nullptr, false, Qt::TopDockWidgetArea);

    m_perspective->select();
    m_watchHandler.loadSessionDataForEngine();
}

//////////////////////////////////////////////////////////////////////
//
// DebuggerEngine
//
//////////////////////////////////////////////////////////////////////

DebuggerEngine::DebuggerEngine()
  : d(new DebuggerEnginePrivate(this))
{
}

DebuggerEngine::~DebuggerEngine()
{
//    EngineManager::unregisterEngine(this);
    delete d;
}

void DebuggerEngine::setDebuggerName(const QString &name)
{
    d->m_debuggerName = name;
    d->updateActionToolTips();
}

QString DebuggerEngine::debuggerName() const
{
    return d->m_debuggerName;
}

void DebuggerEngine::setDebuggerType(const QString &type)
{
    d->m_debuggerType = type;
}

QString DebuggerEngine::debuggerType() const
{
    return d->m_debuggerType;
}

QString DebuggerEngine::stateName(int s)
{
#    define SN(x) case x: return QLatin1String(#x);
    switch (s) {
        SN(DebuggerNotReady)
        SN(EngineSetupRequested)
        SN(EngineSetupFailed)
        SN(EngineRunFailed)
        SN(EngineRunRequested)
        SN(InferiorRunRequested)
        SN(InferiorRunOk)
        SN(InferiorRunFailed)
        SN(InferiorUnrunnable)
        SN(InferiorStopRequested)
        SN(InferiorStopOk)
        SN(InferiorStopFailed)
        SN(InferiorShutdownRequested)
        SN(InferiorShutdownFinished)
        SN(EngineShutdownRequested)
        SN(EngineShutdownFinished)
        SN(DebuggerFinished)
    }
    return QLatin1String("<unknown>");
#    undef SN
}

void DebuggerEngine::notifyExitCode(int code)
{
    d->m_runParameters.setExitCode(code);
}

void DebuggerEngine::showStatusMessage(const QString &msg, int timeout) const
{
    showMessage(msg, StatusBar, timeout);
}

void DebuggerEngine::updateLocalsWindow(bool showReturn)
{
    QTC_ASSERT(d->m_returnWindow, return);
    QTC_ASSERT(d->m_localsView, return);
    d->m_returnWindow->setVisible(showReturn);
    d->m_localsView->resizeColumns();
}

bool DebuggerEngine::isRegistersWindowVisible() const
{
    QTC_ASSERT(d->m_registerWindow, return false);
    return d->m_registerWindow->isVisible();
}

bool DebuggerEngine::isPeripheralRegistersWindowVisible() const
{
    QTC_ASSERT(d->m_peripheralRegisterWindow, return false);
    return d->m_peripheralRegisterWindow->isVisible();
}

bool DebuggerEngine::isModulesWindowVisible() const
{
    QTC_ASSERT(d->m_modulesWindow, return false);
    return d->m_modulesWindow->isVisible();
}

void DebuggerEngine::frameUp()
{
    int currentIndex = stackHandler()->currentIndex();
    activateFrame(qMin(currentIndex + 1, stackHandler()->stackSize() - 1));
}

void DebuggerEngine::frameDown()
{
    int currentIndex = stackHandler()->currentIndex();
    activateFrame(qMax(currentIndex - 1, 0));
}

void DebuggerEngine::doUpdateLocals(const UpdateParameters &)
{
}

ModulesHandler *DebuggerEngine::modulesHandler() const
{
    return &d->m_modulesHandler;
}

RegisterHandler *DebuggerEngine::registerHandler() const
{
    return &d->m_registerHandler;
}

PeripheralRegisterHandler *DebuggerEngine::peripheralRegisterHandler() const
{
    return &d->m_peripheralRegisterHandler;
}

StackHandler *DebuggerEngine::stackHandler() const
{
    return &d->m_stackHandler;
}

ThreadsHandler *DebuggerEngine::threadsHandler() const
{
    return &d->m_threadsHandler;
}

WatchHandler *DebuggerEngine::watchHandler() const
{
    return &d->m_watchHandler;
}

SourceFilesHandler *DebuggerEngine::sourceFilesHandler() const
{
    return &d->m_sourceFilesHandler;
}

BreakHandler *DebuggerEngine::breakHandler() const
{
    return &d->m_breakHandler;
}

LogWindow *DebuggerEngine::logWindow() const
{
    return d->m_logWindow;
}

DisassemblerAgent *DebuggerEngine::disassemblerAgent() const
{
    return &d->m_disassemblerAgent;
}

void DebuggerEngine::fetchMemory(MemoryAgent *, quint64 addr, quint64 length)
{
    Q_UNUSED(addr)
    Q_UNUSED(length)
}

void DebuggerEngine::changeMemory(MemoryAgent *, quint64 addr, const QByteArray &data)
{
    Q_UNUSED(addr)
    Q_UNUSED(data)
}

void DebuggerEngine::setRegisterValue(const QString &name, const QString &value)
{
    Q_UNUSED(name)
    Q_UNUSED(value)
}

void DebuggerEngine::setPeripheralRegisterValue(quint64 address, quint64 value)
{
    Q_UNUSED(address)
    Q_UNUSED(value)
}

void DebuggerEngine::setRunParameters(const DebuggerRunParameters &runParameters)
{
    d->m_runParameters = runParameters;
    d->updateActionToolTips();
}

void DebuggerEngine::setRunId(const QString &id)
{
    d->m_runId = id;
}

void DebuggerEngine::setDevice(const IDeviceConstPtr &device)
{
    d->m_device = device;
    validateRunParameters(d->m_runParameters);
    d->setupViews();
}

void DebuggerEngine::start()
{
    d->m_watchHandler.resetWatchers();
    d->setInitialActionStates();
    setState(EngineSetupRequested);
    showMessage("CALL: SETUP ENGINE");
    setupEngine();
}

void DebuggerEngine::resetLocation()
{
    // Do it after some delay to avoid flicker.
    d->scheduleResetLocation();
}

void DebuggerEngine::gotoLocation(const Location &loc)
{
     d->resetLocation();

    if (loc.canBeDisassembled()
            && ((hasCapability(OperateByInstructionCapability) && operatesByInstruction())
                || !loc.hasDebugInfo()) )
    {
        d->m_disassemblerAgent.setLocation(loc);
        return;
    }

    if (loc.fileName().isEmpty()) {
        showMessage("CANNOT GO TO THIS LOCATION");
        return;
    }
    const FilePath file = loc.fileName();
    const int line = loc.textPosition().line;
    bool newEditor = false;
    IEditor *editor = EditorManager::openEditor(file,
                                                Id(),
                                                EditorManager::IgnoreNavigationHistory
                                                    | EditorManager::DoNotSwitchToDesignMode
                                                    | EditorManager::SwitchSplitIfAlreadyVisible,
                                                &newEditor);
    QTC_ASSERT(editor, return); // Unreadable file?

    editor->gotoLine(line, 0, !settings().stationaryEditorWhileStepping());

    if (newEditor)
        editor->document()->setProperty(Constants::OPENED_BY_DEBUGGER, true);

    if (loc.needsMarker()) {
        d->m_locationMark.reset(new LocationMark(this, loc.fileName(), line));
        d->m_locationMark->setToolTip(Tr::tr("Current debugger location of %1").arg(displayName()));
    }

    d->m_breakHandler.setLocation(loc);
    d->m_watchHandler.setLocation(loc);
}

void DebuggerEngine::gotoCurrentLocation()
{
    if (d->m_state == InferiorStopOk || d->m_state == InferiorUnrunnable) {
        int top = stackHandler()->currentIndex();
        if (top >= 0)
            gotoLocation(stackHandler()->currentFrame());
    }
}

const DebuggerRunParameters &DebuggerEngine::runParameters() const
{
    return d->m_runParameters;
}

IDevice::ConstPtr DebuggerEngine::device() const
{
    return d->m_device;
}

QList<DebuggerEngine *> DebuggerEngine::companionEngines() const
{
    return Utils::transform(d->m_companionEngines, &QPointer<DebuggerEngine>::get);
}

DebuggerState DebuggerEngine::state() const
{
    return d->m_state;
}

void DebuggerEngine::abortDebugger()
{
    resetLocation();
    if (!d->m_isDying) {
        // Be friendly the first time. This will change targetState().
        showMessage("ABORTING DEBUGGER. FIRST TIME.");
        quitDebugger();
    } else {
        // We already tried. Try harder.
        showMessage("ABORTING DEBUGGER. SECOND TIME.");
        abortDebuggerProcess();
        emit requestRunControlStop();
    }
}

void DebuggerEngine::updateUi(bool isCurrentEngine)
{
    updateState();
    if (isCurrentEngine) {
        gotoCurrentLocation();
    } else {
        d->m_locationMark.reset();
        d->m_disassemblerAgent.resetLocation();
    }
}

static bool isAllowedTransition(DebuggerState from, DebuggerState to)
{
    switch (from) {
    case DebuggerNotReady:
        return to == EngineSetupRequested;

    case EngineSetupRequested:
        return to == EngineRunRequested
            || to == EngineSetupFailed
            || to == EngineShutdownRequested;
    case EngineSetupFailed:
        // In is the engine's task to go into a proper "Shutdown"
        // state before calling notifyEngineSetupFailed
        return to == DebuggerFinished;

    case EngineRunRequested:
        return to == EngineRunFailed
            || to == InferiorRunRequested
            || to == InferiorRunOk
            || to == InferiorStopOk
            || to == InferiorUnrunnable;
    case EngineRunFailed:
        return to == EngineShutdownRequested;

    case InferiorRunRequested:
        return to == InferiorRunOk || to == InferiorRunFailed;
    case InferiorRunFailed:
        return to == InferiorStopOk;
    case InferiorRunOk:
        return to == InferiorStopRequested
            || to == InferiorStopOk             // A spontaneous stop.
            || to == InferiorShutdownFinished;  // A spontaneous exit.

    case InferiorStopRequested:
        return to == InferiorStopOk || to == InferiorStopFailed;
    case InferiorStopOk:
        return to == InferiorRunRequested || to == InferiorShutdownRequested
            || to == InferiorStopOk || to == InferiorShutdownFinished;
    case InferiorStopFailed:
        return to == EngineShutdownRequested;

    case InferiorUnrunnable:
        return to == InferiorShutdownRequested;
    case InferiorShutdownRequested:
        return to == InferiorShutdownFinished;
    case InferiorShutdownFinished:
        return to == EngineShutdownRequested;

    case EngineShutdownRequested:
        return to == EngineShutdownFinished;
    case EngineShutdownFinished:
        return to == DebuggerFinished;

    case DebuggerFinished:
        return to == EngineSetupRequested; // Happens on restart.
    }

    qDebug() << "UNKNOWN DEBUGGER STATE:" << from;
    return false;
}

void DebuggerEngine::notifyEngineSetupFailed()
{
    showMessage("NOTE: ENGINE SETUP FAILED");
    QTC_ASSERT(state() == EngineSetupRequested, qDebug() << this << state());
    setState(EngineSetupFailed);
    if (d->m_isPrimaryEngine) {
        showMessage(Tr::tr("Debugging has failed."), NormalMessageFormat);
        d->m_progress.setProgressValue(900);
        d->m_progress.reportCanceled();
        d->m_progress.reportFinished();
    }

    setState(DebuggerFinished);
}

void DebuggerEngine::notifyEngineSetupOk()
{
//#ifdef WITH_BENCHMARK
//    CALLGRIND_START_INSTRUMENTATION;
//#endif
    showMessage("NOTE: ENGINE SETUP OK");
    QTC_ASSERT(state() == EngineSetupRequested, qDebug() << this << state());
    setState(EngineRunRequested);
    showMessage("CALL: RUN ENGINE");
    d->m_progress.setProgressValue(300);
}

void DebuggerEngine::notifyEngineRunOkAndInferiorUnrunnable()
{
    showMessage("NOTE: INFERIOR UNRUNNABLE");
    d->m_progress.setProgressValue(1000);
    d->m_progress.reportFinished();
    QTC_ASSERT(state() == EngineRunRequested, qDebug() << this << state());
    showStatusMessage(Tr::tr("Loading finished."));
    setState(InferiorUnrunnable);
}

void DebuggerEngine::notifyEngineRunFailed()
{
    showMessage("NOTE: ENGINE RUN FAILED");
    QTC_ASSERT(state() == EngineRunRequested, qDebug() << this << state());
    d->m_progress.setProgressValue(900);
    d->m_progress.reportCanceled();
    d->m_progress.reportFinished();
    showStatusMessage(Tr::tr("Run failed."));
    setState(EngineRunFailed);
    d->doShutdownEngine();
}

void DebuggerEngine::notifyEngineRunAndInferiorRunOk()
{
    showMessage("NOTE: ENGINE RUN AND INFERIOR RUN OK");
    d->m_progress.setProgressValue(1000);
    d->m_progress.reportFinished();
    QTC_ASSERT(state() == EngineRunRequested, qDebug() << this << state());
    showStatusMessage(Tr::tr("Running."));
    setState(InferiorRunOk);
}

void DebuggerEngine::notifyEngineRunAndInferiorStopOk()
{
    showMessage("NOTE: ENGINE RUN AND INFERIOR STOP OK");
    d->m_progress.setProgressValue(1000);
    d->m_progress.reportFinished();
    QTC_ASSERT(state() == EngineRunRequested, qDebug() << this << state());
    showStatusMessage(Tr::tr("Stopped."));
    setState(InferiorStopOk);
}

void DebuggerEngine::notifyInferiorRunRequested()
{
    showMessage("NOTE: INFERIOR RUN REQUESTED");
    QTC_ASSERT(state() == InferiorStopOk, qDebug() << this << state());
    showStatusMessage(Tr::tr("Run requested..."));
    setState(InferiorRunRequested);
}

void DebuggerEngine::notifyInferiorRunOk()
{
    if (state() == InferiorRunOk) {
        showMessage("NOTE: INFERIOR RUN OK - REPEATED.");
        return;
    }
    showMessage("NOTE: INFERIOR RUN OK");
    showStatusMessage(Tr::tr("Running."));
    // Transition from StopRequested can happen in remotegdbadapter.
    QTC_ASSERT(state() == InferiorRunRequested
        || state() == InferiorStopOk
        || state() == InferiorStopRequested, qDebug() << this << state());
    setState(InferiorRunOk);
}

void DebuggerEngine::notifyInferiorRunFailed()
{
    showMessage("NOTE: INFERIOR RUN FAILED");
    QTC_ASSERT(state() == InferiorRunRequested, qDebug() << this << state());
    setState(InferiorRunFailed);
    setState(InferiorStopOk);
    if (isDying())
        d->doShutdownInferior();
}

void DebuggerEngine::notifyInferiorStopOk()
{
    showMessage("NOTE: INFERIOR STOP OK");
    // Ignore spurious notifications after we are set to die.
    if (isDying()) {
        showMessage("NOTE: ... WHILE DYING. ");
        // Forward state to "StopOk" if needed.
        if (state() == InferiorStopRequested
                || state() == InferiorRunRequested
                || state() == InferiorRunOk) {
            showMessage("NOTE: ... FORWARDING TO 'STOP OK'. ");
            setState(InferiorStopOk);
        }
        if (state() == InferiorStopOk || state() == InferiorStopFailed)
            d->doShutdownInferior();
        showMessage("NOTE: ... IGNORING STOP MESSAGE");
        return;
    }
    QTC_ASSERT(state() == InferiorStopRequested, qDebug() << this << state());
    showMessage(Tr::tr("Stopped."), StatusBar);
    setState(InferiorStopOk);
}

void DebuggerEngine::notifyInferiorSpontaneousStop()
{
    showMessage("NOTE: INFERIOR SPONTANEOUS STOP");
    QTC_ASSERT(state() == InferiorRunOk, qDebug() << this << state());
    if (QTC_GUARD(d->m_perspective))
        d->m_perspective->select();
    showMessage(Tr::tr("Stopped."), StatusBar);
    setState(InferiorStopOk);
    if (settings().raiseOnInterrupt())
        ICore::raiseWindow(DebuggerMainWindow::instance());
}

void DebuggerEngine::notifyInferiorStopFailed()
{
    showMessage("NOTE: INFERIOR STOP FAILED");
    QTC_ASSERT(state() == InferiorStopRequested, qDebug() << this << state());
    setState(InferiorStopFailed);
    d->doShutdownEngine();
}

void DebuggerEnginePrivate::setInitialActionStates()
{
    if (m_returnWindow)
        m_returnWindow->setVisible(false);
    setBusyCursor(false);

    m_recordForReverseOperationAction.setCheckable(true);
    m_recordForReverseOperationAction.setChecked(false);
    m_recordForReverseOperationAction.setIcon(Icons::RECORD_OFF.icon());
    m_recordForReverseOperationAction.setToolTip(QString("<html><head/><body><p>%1</p><p>"
                                                         "<b>%2</b>%3</p></body></html>").arg(
                         Tr::tr("Record information to enable stepping backwards."),
                         Tr::tr("Note: "),
                         Tr::tr("This feature is very slow and unstable on the GDB side. "
                            "It exhibits unpredictable behavior when going backwards over system "
                            "calls and is very likely to destroy your debugging session.")));

    m_operateInReverseDirectionAction.setCheckable(true);
    m_operateInReverseDirectionAction.setChecked(false);
    m_operateInReverseDirectionAction.setIcon(Icons::DIRECTION_FORWARD.icon());

    m_snapshotAction.setIcon(Utils::Icons::SNAPSHOT_TOOLBAR.icon());

    m_detachAction.setEnabled(false);

    m_watchAction.setEnabled(true);
    m_setOrRemoveBreakpointAction.setEnabled(false);
    m_enableOrDisableBreakpointAction.setEnabled(false);
    m_snapshotAction.setEnabled(false);
    m_operateByInstructionAction.setEnabled(false);

    m_exitAction.setEnabled(false);
    m_abortAction.setEnabled(false);
    m_resetAction.setEnabled(false);

    m_interruptAction.setEnabled(false);
    m_continueAction.setEnabled(false);

    m_stepIntoAction.setEnabled(true);
    m_stepOutAction.setEnabled(false);
    m_runToLineAction.setEnabled(false);
    m_runToLineAction.setVisible(false);
    m_runToSelectedFunctionAction.setEnabled(true);
    m_returnFromFunctionAction.setEnabled(false);
    m_jumpToLineAction.setEnabled(false);
    m_jumpToLineAction.setVisible(false);
    m_stepOverAction.setEnabled(true);

    settings().autoDerefPointers.setEnabled(true);
    settings().expandStack.setEnabled(false);

    if (m_threadLabel)
        m_threadLabel->setEnabled(false);
}

void DebuggerEnginePrivate::updateState()
{
    // Can happen in mixed debugging.
    if (!m_threadLabel)
        return;
    QTC_ASSERT(m_threadLabel, return);

    if (m_isDying)
        return;

    const DebuggerState state = m_state;
    const bool companionPreventsAction = m_engine->companionPreventsActions();

    // Fixme: hint Tr::tr("Debugger is Busy");
    // Exactly one of m_interuptAction and m_continueAction should be
    // visible, possibly disabled.
    if (state == DebuggerNotReady) {
        // Happens when companion starts, otherwise this should not happen.
        //QTC_CHECK(m_companionEngine);
        m_interruptAction.setVisible(true);
        m_interruptAction.setEnabled(false);
        m_continueAction.setVisible(false);
        m_continueAction.setEnabled(false);
        m_stepOverAction.setEnabled(true);
        m_stepIntoAction.setEnabled(true);
        m_stepOutAction.setEnabled(false);
        m_exitAction.setEnabled(false);
    } else if (state == InferiorStopOk) {
        // F5 continues, Shift-F5 kills. It is "continuable".
        m_interruptAction.setVisible(false);
        m_interruptAction.setEnabled(false);
        m_continueAction.setVisible(true);
        m_continueAction.setEnabled(!companionPreventsAction);
        m_stepOverAction.setEnabled(!companionPreventsAction);
        m_stepIntoAction.setEnabled(!companionPreventsAction);
        m_stepOutAction.setEnabled(!companionPreventsAction);
        m_exitAction.setEnabled(true);
        m_localsAndInspectorWindow->setShowLocals(true);
    } else if (state == InferiorRunOk) {
        // Shift-F5 interrupts. It is also "interruptible".
        m_interruptAction.setVisible(true);
        m_interruptAction.setEnabled(!companionPreventsAction);
        m_continueAction.setVisible(false);
        m_continueAction.setEnabled(false);
        m_stepOverAction.setEnabled(false);
        m_stepIntoAction.setEnabled(false);
        m_stepOutAction.setEnabled(false);
        m_exitAction.setEnabled(true);
        m_localsAndInspectorWindow->setShowLocals(false);
    } else if (state == DebuggerFinished) {
        // We don't want to do anything anymore.
        m_interruptAction.setVisible(true);
        m_interruptAction.setEnabled(false);
        m_continueAction.setVisible(false);
        m_continueAction.setEnabled(false);
        m_stepOverAction.setEnabled(false);
        m_stepIntoAction.setEnabled(false);
        m_stepOutAction.setEnabled(false);
        m_exitAction.setEnabled(false);
        setBusyCursor(false);
        cleanupViews();
    } else if (state == InferiorUnrunnable) {
        // We don't want to do anything anymore.
        m_interruptAction.setVisible(true);
        m_interruptAction.setEnabled(false);
        m_continueAction.setVisible(false);
        m_continueAction.setEnabled(false);
        m_stepOverAction.setEnabled(false);
        m_stepIntoAction.setEnabled(false);
        m_stepOutAction.setEnabled(false);
        m_exitAction.setEnabled(true);
        // show locals in core dumps
        m_localsAndInspectorWindow->setShowLocals(true);
    } else {
        // Everything else is "undisturbable".
        m_interruptAction.setVisible(true);
        m_interruptAction.setEnabled(false);
        m_continueAction.setVisible(false);
        m_continueAction.setEnabled(false);
        m_stepOverAction.setEnabled(false);
        m_stepIntoAction.setEnabled(false);
        m_stepOutAction.setEnabled(false);
        m_exitAction.setEnabled(false);
    }

    const bool threadsEnabled = state == InferiorStopOk || state == InferiorUnrunnable;
    m_threadsHandler.threadSwitcher()->setEnabled(threadsEnabled);
    m_threadLabel->setEnabled(threadsEnabled);

    const bool isCore = m_engine->runParameters().startMode() == AttachToCore;
    const bool stopped = state == InferiorStopOk;
    const bool detachable = stopped && !isCore;
    m_detachAction.setEnabled(detachable);

    updateReverseActions();

    const bool canSnapshot = m_engine->hasCapability(SnapshotCapability);
    m_snapshotAction.setVisible(canSnapshot);
    m_snapshotAction.setEnabled(stopped && !isCore);

    m_watchAction.setEnabled(true);
    m_setOrRemoveBreakpointAction.setEnabled(true);
    m_enableOrDisableBreakpointAction.setEnabled(true);

    const bool canOperateByInstruction = m_engine->hasCapability(OperateByInstructionCapability);
    m_operateByInstructionAction.setVisible(canOperateByInstruction);
    m_operateByInstructionAction.setEnabled(canOperateByInstruction && (stopped || isCore));

    m_abortAction.setEnabled(state != DebuggerNotReady
                                      && state != DebuggerFinished);
    m_resetAction.setEnabled((stopped || state == DebuggerNotReady)
                              && m_engine->hasCapability(ResetInferiorCapability));

    m_stepIntoAction.setEnabled(stopped || state == DebuggerNotReady);
    m_stepIntoAction.setToolTip(QString());

    m_stepOverAction.setEnabled(stopped || state == DebuggerNotReady);
    m_stepOverAction.setToolTip(QString());

    m_stepOutAction.setEnabled(stopped);

    const bool canRunToLine = m_engine->hasCapability(RunToLineCapability);
    m_runToLineAction.setVisible(canRunToLine);
    m_runToLineAction.setEnabled(stopped && canRunToLine);

    m_runToSelectedFunctionAction.setEnabled(stopped);

    const bool canReturnFromFunction = m_engine->hasCapability(ReturnFromFunctionCapability);
    m_returnFromFunctionAction.setVisible(canReturnFromFunction);
    m_returnFromFunctionAction.setEnabled(stopped && canReturnFromFunction);

    const bool canJump = m_engine->hasCapability(JumpToLineCapability);
    m_jumpToLineAction.setVisible(canJump);
    m_jumpToLineAction.setEnabled(stopped && canJump);

    const bool actionsEnabled = m_engine->debuggerActionsEnabled();
    const bool canDeref = actionsEnabled && m_engine->hasCapability(AutoDerefPointersCapability);
    settings().autoDerefPointers.setEnabled(canDeref);
    settings().autoDerefPointers.setEnabled(true);
    settings().expandStack.setEnabled(actionsEnabled);

    const bool notbusy = state == InferiorStopOk
        || state == DebuggerNotReady
        || state == DebuggerFinished
        || state == InferiorUnrunnable;
    setBusyCursor(!notbusy);
}

void DebuggerEnginePrivate::updateReverseActions()
{
    const bool stopped = m_state == InferiorStopOk;
    const bool reverseEnabled = settings().enableReverseDebugging();
    const bool canReverse = reverseEnabled && m_engine->hasCapability(ReverseSteppingCapability);
    const bool doesRecord = m_recordForReverseOperationAction.isChecked();

    m_recordForReverseOperationAction.setVisible(canReverse);
    m_recordForReverseOperationAction.setEnabled(canReverse && stopped);
    m_recordForReverseOperationAction.setIcon(doesRecord
                                              ? Icons::RECORD_ON.icon()
                                              : Icons::RECORD_OFF.icon());

    m_operateInReverseDirectionAction.setVisible(canReverse);
    m_operateInReverseDirectionAction.setEnabled(canReverse && stopped && doesRecord);
    m_operateInReverseDirectionAction.setIcon(Icons::DIRECTION_BACKWARD.icon());
    m_operateInReverseDirectionAction.setText(Tr::tr("Operate in Reverse Direction"));
}

void DebuggerEnginePrivate::cleanupViews()
{
    const bool closeSource = settings().closeSourceBuffersOnExit();
    const bool closeMemory = settings().closeMemoryBuffersOnExit();

    QList<IDocument *> toClose;
    const QList<IDocument *> documents = DocumentModel::openedDocuments();
    for (IDocument *document : documents) {
        const bool isMemory = document->property(Constants::OPENED_WITH_DISASSEMBLY).toBool();
        if (document->property(Constants::OPENED_BY_DEBUGGER).toBool()) {
            bool keepIt = true;
            if (document->isModified())
                keepIt = true;
            else if (document->filePath().toUrlishString().contains("qeventdispatcher"))
                keepIt = false;
            else if (isMemory)
                keepIt = !closeMemory;
            else
                keepIt = !closeSource;

            if (keepIt)
                document->setProperty(Constants::OPENED_BY_DEBUGGER, false);
            else
                toClose.append(document);
        }
    }
    EditorManager::closeDocuments(toClose);
}

void DebuggerEnginePrivate::setBusyCursor(bool busy)
{
    //STATE_DEBUG("BUSY FROM: " << m_busy << " TO: " << busy);
    if (m_isDying)
        return;
    if (busy == m_busy)
        return;
    m_busy = busy;
    const QCursor cursor(busy ? Qt::BusyCursor : Qt::ArrowCursor);
    m_breakWindow->setCursor(cursor);
    //m_consoleWindow->setCursor(cursor);
    m_localsWindow->setCursor(cursor);
    m_modulesWindow->setCursor(cursor);
    m_logWindow->setCursor(cursor);
    m_registerWindow->setCursor(cursor);
    m_peripheralRegisterWindow->setCursor(cursor);
    m_returnWindow->setCursor(cursor);
    m_sourceFilesWindow->setCursor(cursor);
    m_stackWindow->setCursor(cursor);
    m_threadsWindow->setCursor(cursor);
    m_watchersWindow->setCursor(cursor);
}

void DebuggerEngine::notifyInferiorShutdownFinished()
{
    showMessage("INFERIOR FINISHED SHUT DOWN");
    QTC_ASSERT(state() == InferiorShutdownRequested, qDebug() << this << state());
    setState(InferiorShutdownFinished);
    d->doShutdownEngine();
}

void DebuggerEngine::notifyInferiorIll()
{
    showMessage("NOTE: INFERIOR ILL");
    // This can be issued in almost any state. The debugged process could
    // still be alive as some previous notifications might have been bogus.
    startDying();
    if (state() == InferiorRunRequested) {
        // We asked for running, but did not see a response.
        // Assume the inferior is dead.
        // FIXME: Use timeout?
        setState(InferiorRunFailed);
        setState(InferiorStopOk);
    }
    d->doShutdownInferior();
}

void DebuggerEngine::notifyEngineShutdownFinished()
{
    showMessage("NOTE: ENGINE SHUTDOWN FINISHED");
    QTC_ASSERT(state() == EngineShutdownRequested, qDebug() << this << state());
    setState(EngineShutdownFinished);
    d->doFinishDebugger();
}

void DebuggerEngine::notifyEngineIll()
{
//#ifdef WITH_BENCHMARK
//    CALLGRIND_STOP_INSTRUMENTATION;
//    CALLGRIND_DUMP_STATS;
//#endif
    showMessage("NOTE: ENGINE ILL ******");
    startDying();
    switch (state()) {
        case InferiorRunRequested:
        case InferiorRunOk:
            // The engine does not look overly ill right now, so attempt to
            // properly interrupt at least once. If that fails, we are on the
            // shutdown path due to d->m_targetState anyways.
            setState(InferiorStopRequested, true);
            showMessage("ATTEMPT TO INTERRUPT INFERIOR");
            interruptInferior();
            break;
        case InferiorStopRequested:
            notifyInferiorStopFailed();
            break;
        case InferiorStopOk:
            showMessage("FORWARDING STATE TO InferiorShutdownFinished");
            setState(InferiorShutdownFinished, true);
            d->doShutdownEngine();
            break;
        default:
            d->doShutdownEngine();
            break;
    }
}

void DebuggerEngine::notifyEngineSpontaneousShutdown()
{
#ifdef WITH_BENCHMARK
    CALLGRIND_STOP_INSTRUMENTATION;
    CALLGRIND_DUMP_STATS;
#endif
    showMessage("NOTE: ENGINE SPONTANEOUS SHUTDOWN");
    setState(EngineShutdownFinished, true);
    d->doFinishDebugger();
}

void DebuggerEngine::notifyInferiorExited()
{
#ifdef WITH_BENCHMARK
    CALLGRIND_STOP_INSTRUMENTATION;
    CALLGRIND_DUMP_STATS;
#endif
    showMessage("NOTE: INFERIOR EXITED");
    d->resetLocation();
    setState(InferiorShutdownFinished);
    d->doShutdownEngine();
}

void DebuggerEngine::updateState()
{
    d->updateState();
}

WatchTreeView *DebuggerEngine::inspectorView()
{
    return d->m_inspectorView;
}

void DebuggerEngine::showMessage(const QString &msg, int channel, int timeout) const
{
    //qDebug() << "PLUGIN OUTPUT: " << channel << msg;
    QTC_ASSERT(d->m_logWindow, qDebug() << "MSG: " << msg; return);
    switch (channel) {
        case StatusBar:
            d->m_logWindow->showInput(LogMisc, msg);
            d->m_logWindow->showOutput(LogMisc, msg);
            DebuggerMainWindow::showStatusMessage(msg, timeout);
            break;
        case LogMiscInput:
            d->m_logWindow->showInput(LogMisc, msg);
            d->m_logWindow->showOutput(LogMisc, msg);
            break;
        case LogInput:
            d->m_logWindow->showInput(LogInput, msg);
            d->m_logWindow->showOutput(LogInput, msg);
            break;
        case LogOutput:
        case LogWarning:
            d->m_logWindow->showOutput(channel, msg);
            break;
        case LogError:
            d->m_logWindow->showInput(LogError, "ERROR: " + msg);
            d->m_logWindow->showOutput(LogError, "ERROR: " + msg);
            break;
        case AppOutput:
        case AppStuff:
            d->m_logWindow->showOutput(channel, msg);
            emit postMessageRequested(msg, StdOutFormat, false);
            break;
        case AppError:
            d->m_logWindow->showOutput(channel, msg);
            emit postMessageRequested(msg, StdErrFormat, false);
            break;
        default:
            d->m_logWindow->showOutput(channel, QString("[%1] %2").arg(debuggerName(), msg));
            break;
    }
}

void DebuggerEngine::notifyDebuggerProcessFinished(const ProcessResultData &result,
                                                   const QString &backendName)
{
    showMessage(QString("%1 PROCESS FINISHED, status %2, exit code %3 (0x%4)")
                    .arg(backendName)
                    .arg(result.m_exitStatus)
                    .arg(result.m_exitCode)
                    .arg(QString::number(result.m_exitCode, 16)));

    switch (state()) {
    case DebuggerFinished:
        // Nothing to do.
        break;
    case EngineSetupRequested:
        notifyEngineSetupFailed();
        break;
    case EngineShutdownRequested:
    case InferiorShutdownRequested:
        notifyEngineShutdownFinished();
        break;
    case InferiorRunOk:
        // This could either be a real gdb/lldb crash or a quickly exited inferior
        // in the terminal adapter. In this case the stub proc will die soon,
        // too, so there's no need to act here.
        showMessage(QString("The %1 process exited somewhat unexpectedly.").arg(backendName));
        notifyEngineSpontaneousShutdown();
        break;
    default: {
        // Initiate shutdown sequence
        notifyInferiorIll();
        const QString msg = result.m_exitStatus == QProcess::CrashExit ?
                Tr::tr("The %1 process terminated.") :
                Tr::tr("The %2 process terminated unexpectedly (exit code %1).").arg(result.m_exitCode);
        AsynchronousMessageBox::critical(Tr::tr("Unexpected %1 Exit").arg(backendName),
                                         msg.arg(backendName));
        break;
    }
    }
}

static QString msgStateChanged(DebuggerState oldState, DebuggerState newState, bool forced)
{
    QString result;
    QTextStream str(&result);
    str << "State changed";
    if (forced)
        str << " BY FORCE";
    str << " from " << DebuggerEngine::stateName(oldState) << '(' << oldState
        << ") to " << DebuggerEngine::stateName(newState) << '(' << newState << ')';
    return result;
}

void DebuggerEngine::setState(DebuggerState state, bool forced)
{
    const QString msg = msgStateChanged(d->m_state, state, forced);

    DebuggerState oldState = d->m_state;
    d->m_state = state;

    if (!forced && !isAllowedTransition(oldState, state))
        qDebug() << "*** UNEXPECTED STATE TRANSITION: " << this << msg;

    if (state == EngineRunRequested) {
        emit engineStarted();
        if (d->m_perspective)
            d->m_perspective->select();
    }

    showMessage(msg, LogDebug);

    d->updateState();
    for (const QPointer<DebuggerEngine> &companion : std::as_const(d->m_companionEngines))
        companion->d->updateState();

    if (oldState != d->m_state)
        emit EngineManager::instance()->engineStateChanged(this);

    if (state == DebuggerFinished) {
        d->setBusyCursor(false);

        // Give up ownership on claimed breakpoints.
        d->m_breakHandler.releaseAllBreakpoints();
        d->m_toolTipManager.deregisterEngine();
        d->m_memoryAgents.handleDebuggerFinished();

        d->destroyPerspective();
        emit engineFinished();
    }
}

bool DebuggerEngine::isPrimaryEngine() const
{
    return d->m_isPrimaryEngine;
}

bool DebuggerEngine::canDisplayTooltip() const
{
    return state() == InferiorStopOk;
}

QString DebuggerEngine::expand(const QString &string) const
{
    return runParameters().macroExpander()->expand(string);
}

QString DebuggerEngine::nativeStartupCommands() const
{
    QStringList lines = settings().gdbStartupCommands().split('\n');
    lines += runParameters().additionalStartupCommands().split('\n');

    lines = Utils::filtered(lines, [](const QString line) {
        const QString trimmed = line.trimmed();
        return !trimmed.isEmpty() && !trimmed.startsWith('#');
    });

    return lines.join('\n');
}

Perspective *DebuggerEngine::perspective() const
{
    return d->m_perspective;
}

void DebuggerEngine::updateMarkers()
{
    if (d->m_locationMark)
        d->m_locationMark->updateIcon();

    d->m_disassemblerAgent.updateLocationMarker();
}

void DebuggerEngine::updateToolTips()
{
    d->m_toolTipManager.updateToolTips();
}

DebuggerToolTipManager *DebuggerEngine::toolTipManager()
{
    return &d->m_toolTipManager;
}

bool DebuggerEngine::operatesByInstruction() const
{
    return d->m_operateByInstructionAction.isChecked();
}

bool DebuggerEngine::debuggerActionsEnabled() const
{
    return debuggerActionsEnabledHelper(d->m_state);
}

void DebuggerEngine::operateByInstructionTriggered(bool on)
{
    // Go to source only if we have the file.
    //    if (DebuggerEngine *cppEngine = m_engine->cppEngine()) {
    d->m_stackHandler.rootItem()->updateAll();
    if (d->m_stackHandler.currentIndex() >= 0) {
        const StackFrame frame = d->m_stackHandler.currentFrame();
        if (on || frame.isUsable())
            gotoLocation(Location(frame, true));
    }
    //    }
}

bool DebuggerEngine::companionPreventsActions() const
{
    return false;
}

void DebuggerEngine::notifyInferiorPid(const ProcessHandle &pid)
{
    if (d->m_inferiorPid == pid)
        return;
    d->m_inferiorPid = pid;
    if (pid.isValid()) {
        showMessage(Tr::tr("Taking notice of pid %1").arg(pid.pid()));
        const DebuggerStartMode sm = runParameters().startMode();
        if (sm == StartInternal || sm == StartExternal || sm == AttachToLocalProcess)
            d->m_inferiorPid.activate();
    }
}

qint64 DebuggerEngine::inferiorPid() const
{
    return d->m_inferiorPid.pid();
}

bool DebuggerEngine::isReverseDebugging() const
{
    return d->m_operateInReverseDirectionAction.isChecked();
}

void DebuggerEngine::handleBeginOfRecordingReached()
{
    showStatusMessage(Tr::tr("Reverse-execution history exhausted. Going forward again."));
    d->m_operateInReverseDirectionAction.setChecked(false);
    d->updateReverseActions();
}

void DebuggerEngine::handleRecordingFailed()
{
    showStatusMessage(Tr::tr("Reverse-execution recording failed."));
    d->m_operateInReverseDirectionAction.setChecked(false);
    d->m_recordForReverseOperationAction.setChecked(false);
    d->updateReverseActions();
    executeRecordReverse(false);
}

// Called by DebuggerRunControl.
void DebuggerEngine::quitDebugger()
{
    showMessage(QString("QUIT DEBUGGER REQUESTED IN STATE %1").arg(state()));
    startDying();
    switch (state()) {
    case InferiorStopOk:
    case InferiorStopFailed:
    case InferiorUnrunnable:
        d->doShutdownInferior();
        break;
    case InferiorRunOk:
        setState(InferiorStopRequested);
        showMessage(Tr::tr("Attempting to interrupt."), StatusBar);
        interruptInferior();
        break;
    case EngineSetupRequested:
        notifyEngineSetupFailed();
        break;
    case EngineRunRequested:
        notifyEngineRunFailed();
        break;
    case EngineShutdownRequested:
    case InferiorShutdownRequested:
        break;
    case DebuggerNotReady:
    case EngineSetupFailed:
    case InferiorShutdownFinished:
    case EngineRunFailed:
    case EngineShutdownFinished:
    case DebuggerFinished:
        break;
    case InferiorRunRequested:
    case InferiorRunFailed:
    case InferiorStopRequested:
        // FIXME: We should disable the actions connected to that.
        notifyInferiorIll();
        break;
    }
}

void DebuggerEngine::requestInterruptInferior()
{
    QTC_ASSERT(state() == InferiorRunOk, qDebug() << this << state());
    setState(InferiorStopRequested);
    showMessage("CALL: INTERRUPT INFERIOR");
    showMessage(Tr::tr("Attempting to interrupt."), StatusBar);
    interruptInferior();
}

void DebuggerEngine::progressPing()
{
    int progress = qMin(d->m_progress.progressValue() + 2, 800);
    d->m_progress.setProgressValue(progress);
}

void DebuggerEngine::addCompanionEngine(DebuggerEngine *engine)
{
    d->m_companionEngines << engine;
}

void DebuggerEngine::setSecondaryEngine()
{
    d->m_isPrimaryEngine = false;
}

bool DebuggerEngine::usesTerminal() const
{
    return d->m_runParameters.useTerminal();
}

qint64 DebuggerEngine::applicationPid() const
{
    QTC_CHECK(usesTerminal());
    return d->m_runParameters.applicationPid();
}

qint64 DebuggerEngine::applicationMainThreadId() const
{
    QTC_CHECK(usesTerminal());
    return d->m_runParameters.applicationMainThreadId();
}

void DebuggerEngine::selectWatchData(const QString &)
{
}

void DebuggerEngine::watchPoint(const QPoint &pnt)
{
    DebuggerCommand cmd("watchPoint", NeedsFullStop);
    cmd.arg("x", pnt.x());
    cmd.arg("y", pnt.y());
    cmd.callback = [this](const DebuggerResponse &response) {
        qulonglong addr = response.data["selected"].toAddress();
        if (addr == 0)
            showMessage(Tr::tr("Could not find a widget."), StatusBar);
        // Add the watcher entry nevertheless, as that's the place where
        // the user expects visual feedback.
        watchHandler()->watchExpression(response.data["expr"].data(), QString(), true);
    };
    runCommand(cmd);
}

void DebuggerEngine::runCommand(const DebuggerCommand &)
{
    // Overridden in the engines that use the interface.
    QTC_CHECK(false);
}

void DebuggerEngine::fetchDisassembler(DisassemblerAgent *)
{
}

void DebuggerEngine::activateFrame(int)
{
}

void DebuggerEngine::reloadModules()
{
}

void DebuggerEngine::examineModules()
{
}

void DebuggerEngine::loadSymbols(const FilePath &)
{
}

void DebuggerEngine::loadAllSymbols()
{
}

void DebuggerEngine::loadSymbolsForStack()
{
}

void DebuggerEngine::requestModuleSymbols(const FilePath &)
{
}

void DebuggerEngine::requestModuleSections(const FilePath &)
{
}

void DebuggerEngine::reloadRegisters()
{
}

void DebuggerEngine::reloadPeripheralRegisters()
{
}

void DebuggerEngine::reloadSourceFiles()
{
}

void DebuggerEngine::reloadFullStack()
{
}

void DebuggerEngine::loadAdditionalQmlStack()
{
}

void DebuggerEngine::reloadDebuggingHelpers()
{
}

void DebuggerEngine::addOptionPages(QList<IOptionsPage*> *) const
{
}

QString DebuggerEngine::qtNamespace() const
{
    return d->m_qtNamespace;
}

void DebuggerEngine::setQtNamespace(const QString &ns)
{
    d->m_qtNamespace = ns;
}

void DebuggerEngine::createSnapshot()
{
}

void DebuggerEngine::updateLocals()
{
    // if the engine is not running - do nothing
    if (state() == DebuggerState::DebuggerFinished || state() == DebuggerState::DebuggerNotReady)
        return;

    watchHandler()->resetValueCache();
    doUpdateLocals(UpdateParameters());
}

Context DebuggerEngine::debuggerContext() const
{
    return d->m_context;
}

void DebuggerEngine::updateAll()
{
}

QString DebuggerEngine::displayName() const
{
    //: e.g. LLDB for "myproject", shows up i
    return Tr::tr("%1 for \"%2\"").arg(d->m_debuggerName, runParameters().displayName());
}

void DebuggerEngine::insertBreakpoint(const Breakpoint &bp)
{
    QTC_ASSERT(bp, return);
    BreakpointState state = bp->state();
    QTC_ASSERT(state == BreakpointInsertionRequested,
               qDebug() << bp->modelId() << this << state);
    QTC_CHECK(false);
}

void DebuggerEngine::removeBreakpoint(const Breakpoint &bp)
{
    QTC_ASSERT(bp, return);
    BreakpointState state = bp->state();
    QTC_ASSERT(state == BreakpointRemoveRequested,
               qDebug() << bp->responseId() << this << state);
    QTC_CHECK(false);
}

void DebuggerEngine::updateBreakpoint(const Breakpoint &bp)
{
    QTC_ASSERT(bp, return);
    BreakpointState state = bp->state();
    QTC_ASSERT(state == BreakpointUpdateRequested,
               qDebug() << bp->responseId() << this << state);
    QTC_CHECK(false);
}

void DebuggerEngine::enableSubBreakpoint(const SubBreakpoint &sbp, bool)
{
    QTC_ASSERT(sbp, return);
    QTC_CHECK(false);
}

void DebuggerEngine::assignValueInDebugger(WatchItem *,
    const QString &, const QVariant &)
{
}

void DebuggerEngine::handleRecordReverse(bool record)
{
    executeRecordReverse(record);
    d->updateReverseActions();
}

void DebuggerEngine::handleReverseDirection(bool reverse)
{
    executeReverse(reverse);
    updateMarkers();
    d->updateReverseActions();
}

void DebuggerEngine::executeDebuggerCommand(const QString &)
{
    showMessage(Tr::tr("This debugger cannot handle user input."), StatusBar);
}

bool DebuggerEngine::isDying() const
{
    return d->m_isDying;
}

QString DebuggerEngine::msgStopped(const QString &reason)
{
    return reason.isEmpty() ? Tr::tr("Stopped.") : Tr::tr("Stopped: \"%1\".").arg(reason);
}

QString DebuggerEngine::msgStoppedBySignal(const QString &meaning,
    const QString &name)
{
    return Tr::tr("Stopped: %1 (Signal %2).").arg(meaning, name);
}

QString DebuggerEngine::msgStoppedByException(const QString &description,
    const QString &threadId)
{
    return Tr::tr("Stopped in thread %1 by: %2.").arg(threadId, description);
}

QString DebuggerEngine::msgInterrupted()
{
    return Tr::tr("Interrupted.");
}

bool DebuggerEngine::showStoppedBySignalMessageBox(QString meaning, QString name)
{
    if (d->m_alertBox)
        return false;

    if (name.isEmpty())
        name = ' ' + Tr::tr("<Unknown>", "name") + ' ';
    if (meaning.isEmpty())
        meaning = ' ' + Tr::tr("<Unknown>", "meaning") + ' ';
    const QString msg = QString("<p>%1</p>"
                                "<table>"
                                "<tr><td>%2</td><td>%3</td></tr>"
                                "<tr><td>%4</td><td>%5</td></tr>"
                                "</table>")
                            .arg(
                                Tr::tr("The debugged process stopped because it received a signal "
                                       "from the operating system."),
                                Tr::tr("Signal name:"),
                                name,
                                Tr::tr("Signal meaning:"),
                                meaning);

    d->m_alertBox = AsynchronousMessageBox::information(Tr::tr("Signal Received"), msg);
    return true;
}

void DebuggerEngine::showStoppedByExceptionMessageBox(const QString &description)
{
    const QString msg = "<p>"
                        + Tr::tr("The debugged process stopped because it triggered an exception.")
                        + "</p><p>" + description + "</p>";
    AsynchronousMessageBox::information(Tr::tr("Exception Triggered"), msg);
}

void DebuggerEngine::openMemoryView(const MemoryViewSetupData &data)
{
    d->m_memoryAgents.createBinEditor(data, this);
}

void DebuggerEngine::updateMemoryViews()
{
    d->m_memoryAgents.updateContents();
}

void DebuggerEngine::openDisassemblerView(const Location &location)
{
    DisassemblerAgent *agent = new DisassemblerAgent(this);
    agent->setLocation(location);
}

void DebuggerEngine::raiseWatchersWindow()
{
    if (d->m_watchersView && d->m_watchersWindow) {
        auto currentPerspective = DebuggerMainWindow::currentPerspective();
        QTC_ASSERT(currentPerspective, return);
        // if a companion engine has taken over - do not raise the watchers
        if (currentPerspective->name() != d->m_engine->displayName())
            return;

        if (auto dock = qobject_cast<QDockWidget *>(d->m_watchersWindow->parentWidget())) {
            if (QAction *act = dock->toggleViewAction()) {
                if (!act->isChecked())
                    QTimer::singleShot(1, act, [act] { act->trigger(); });
                dock->raise();
            }
        }
    }
}

void DebuggerEngine::openMemoryEditor()
{
    if (std::optional<quint64> result = runAddressDialog(0)) {
        MemoryViewSetupData data;
        data.startAddress = *result;
        openMemoryView(data);
    }
}

void DebuggerEngine::updateLocalsView(const GdbMi &all)
{
    WatchHandler *handler = watchHandler();

    const GdbMi typeInfo = all["typeinfo"];
    handler->recordTypeInfo(typeInfo);

    const GdbMi data = all["data"];
    handler->insertItems(data);

    const GdbMi ns = all["qtnamespace"];
    if (ns.isValid()) {
        setQtNamespace(ns.data());
        showMessage("FOUND NAMESPACED QT: " + ns.data());
    }

    static int count = 0;
    showMessage(QString("<Rebuild Watchmodel %1 @ %2 >")
                .arg(++count).arg(LogWindow::logTimeStamp()), LogMiscInput);
    showMessage(Tr::tr("Finished retrieving data."), 400, StatusBar);

    d->m_toolTipManager.updateToolTips();

    const bool partial = all["partial"].toInt();
    if (!partial)
        updateMemoryViews();
}

bool DebuggerEngine::canHandleToolTip(const DebuggerToolTipContext &context) const
{
    return state() == InferiorStopOk && context.isCppEditor;
}

void DebuggerEngine::updateItem(const QString &iname)
{
    WatchHandler *handler = watchHandler();
    const int maxArrayCount = handler->maxArrayCount(iname);
    if (d->m_lookupRequests.value(iname, -1) == maxArrayCount) {
        showMessage(QString("IGNORING REPEATED REQUEST TO EXPAND " + iname));
        WatchItem *item = handler->findItem(iname);
        QTC_CHECK(item);
        WatchModelBase *model = handler->model();
        QTC_CHECK(model);
        if (item && !item->wantsChildren) {
            updateToolTips();
            return;
        }
        if (item && !model->hasChildren(model->indexForItem(item))) {
            handler->notifyUpdateStarted(UpdateParameters(iname));
            item->setValue(decodeData({}, "notaccessible"));
            item->setHasChildren(false);
            item->outdated = false;
            item->update();
            handler->notifyUpdateFinished();
            return;
        }
        // We could legitimately end up here after expanding + closing + re-expaning an item.
    }
    d->m_lookupRequests[iname] = maxArrayCount;

    UpdateParameters params;
    params.partialVariable = iname;
    doUpdateLocals(params);
}

void DebuggerEngine::reexpandItems(const QSet<QString> &)
{
}

void DebuggerEngine::updateWatchData(const QString &iname)
{
    // This is used in cases where re-evaluation is ok for the same iname
    // e.g. when changing the expression in a watcher.
    UpdateParameters params;
    params.partialVariable = iname;
    params.qmlFocusOnFrame = false;
    doUpdateLocals(params);
}

void DebuggerEngine::expandItem(const QString &iname)
{
    updateItem(iname);
}

void DebuggerEngine::handleExecDetach()
{
    resetLocation();
    detachDebugger();
}

void DebuggerEngine::handleExecContinue()
{
    resetLocation();
    continueInferior();
}

void DebuggerEngine::handleExecInterrupt()
{
    resetLocation();
    requestInterruptInferior();
}

void DebuggerEngine::handleReset()
{
    resetLocation();
    resetInferior();
}

void DebuggerEngine::handleExecStepIn()
{
    resetLocation();
    executeStepIn(operatesByInstruction());
}

void DebuggerEngine::handleExecStepOver()
{
    resetLocation();
    executeStepOver(operatesByInstruction());
}

void DebuggerEngine::handleExecStepOut()
{
    resetLocation();
    executeStepOut();
}

void DebuggerEngine::handleExecReturn()
{
    resetLocation();
    executeReturn();
}

void DebuggerEngine::handleExecJumpToLine()
{
    resetLocation();
    if (BaseTextEditor *textEditor = BaseTextEditor::currentTextEditor()) {
        ContextData location = getLocationContext(textEditor->textDocument(),
                                                  textEditor->currentLine());
        if (location.isValid())
            executeJumpToLine(location);
    }
}

void DebuggerEngine::handleExecRunToLine()
{
    resetLocation();
    if (BaseTextEditor *textEditor = BaseTextEditor::currentTextEditor()) {
        ContextData location = getLocationContext(textEditor->textDocument(),
                                                  textEditor->currentLine());
        if (location.isValid())
            executeRunToLine(location);
    }
}

void DebuggerEngine::handleExecRunToSelectedFunction()
{
    BaseTextEditor *textEditor = BaseTextEditor::currentTextEditor();
    QTC_ASSERT(textEditor, return);
    QTextCursor cursor = textEditor->textCursor();
    QString functionName = cursor.selectedText();
    if (functionName.isEmpty()) {
        const QTextBlock block = cursor.block();
        const QStringList lineList = block.text().trimmed().split('(');
        for (const QString &str : lineList) {
            QString a;
            for (int i = str.size(); --i >= 0; ) {
                if (!str.at(i).isLetterOrNumber())
                    break;
                a = str.at(i) + a;
            }
            if (!a.isEmpty()) {
                functionName = a;
                break;
            }
        }
    }

    if (functionName.isEmpty()) {
        showMessage(Tr::tr("No function selected."), StatusBar);
    } else {
        showMessage(Tr::tr("Running to function \"%1\".").arg(functionName), StatusBar);
        resetLocation();
        executeRunToFunction(functionName);
    }
}

void DebuggerEngine::handleAddToWatchWindow()
{
    // Requires a selection, but that's the only case we want anyway.
    BaseTextEditor *textEditor = BaseTextEditor::currentTextEditor();
    if (!textEditor)
        return;
    QTextCursor tc = textEditor->textCursor();
    QString exp;
    if (tc.hasSelection()) {
        exp = tc.selectedText();
    } else {
        int line, column;
        exp = cppExpressionAt(textEditor->editorWidget(), tc.position(), &line, &column);
    }
    if (hasCapability(WatchComplexExpressionsCapability))
        exp = removeObviousSideEffects(exp);
    else
        exp = fixCppExpression(exp);
    exp = exp.trimmed();
    if (exp.isEmpty()) {
        // Happens e.g. when trying to evaluate 'char' or 'return'.
        AsynchronousMessageBox::warning(Tr::tr("Warning"),
                                        Tr::tr("Select a valid expression to evaluate."));
        return;
    }
    watchHandler()->watchVariable(exp);
}

void DebuggerEngine::handleFrameDown()
{
    frameDown();
}

void DebuggerEngine::handleFrameUp()
{
    frameUp();
}

void DebuggerEngine::checkState(DebuggerState state, const char *file, int line)
{
    const DebuggerState current = d->m_state;
    if (current == state)
        return;

    QString msg = QString("UNEXPECTED STATE: %1  WANTED: %2 IN %3:%4")
                .arg(stateName(current)).arg(stateName(state)).arg(QLatin1String(file)).arg(line);

    showMessage(msg, LogError);
    qDebug("%s", qPrintable(msg));
}

bool DebuggerEngine::isNativeMixedEnabled() const
{
    return d->m_runParameters.isNativeMixedDebugging();
}

bool DebuggerEngine::isNativeMixedActive() const
{
    return isNativeMixedEnabled(); //&& boolSetting(OperateNativeMixed);
}

bool DebuggerEngine::isNativeMixedActiveFrame() const
{
    if (!isNativeMixedActive())
        return false;
    if (stackHandler()->rowCount() == 0)
        return false;
    StackFrame frame = stackHandler()->frameAt(0);
    return frame.language == QmlLanguage;
}

void DebuggerEngine::startDying() const
{
    d->m_isDying = true;
}

QString DebuggerEngine::runId() const
{
    return d->m_runId;
}

QString DebuggerEngine::formatStartParameters() const
{
    const DebuggerRunParameters &rp = d->m_runParameters;
    QString rc;
    QTextStream str(&rc);
    str << "Start parameters: '" << rp.displayName() << "' mode: " << rp.startMode()
        << "\nABI: " << rp.toolChainAbi().toString() << '\n';
    str << "Languages: ";
    if (rp.isCppDebugging())
        str << "c++ ";
    if (rp.isQmlDebugging())
        str << "qml";
    str << '\n';
    if (!rp.inferior().command.isEmpty()) {
        str << "Executable: " << rp.inferior().command.toUserOutput();
        if (usesTerminal())
            str << " [terminal]";
        str << '\n';
        if (!rp.inferior().workingDirectory.isEmpty())
            str << "Directory: " << rp.inferior().workingDirectory.toUserOutput() << '\n';
    }
    if (!rp.debugger().command.isEmpty())
        str << "Debugger: " << rp.debugger().command.toUserOutput() << '\n';
    if (!rp.coreFile().isEmpty())
        str << "Core: " << rp.coreFile().toUserOutput() << '\n';
    if (rp.attachPid().isValid())
        str << "PID: " << rp.attachPid().pid() << ' ' << rp.crashParameter() << '\n';
    if (!rp.projectSourceDirectory().isEmpty()) {
        str << "Project: " << rp.projectSourceDirectory().toUserOutput() << '\n';
        str << "Additional Search Directories:";
        for (const FilePath &dir : rp.additionalSearchDirectories())
            str << ' ' << dir;
        str << '\n';
    }
    if (!rp.remoteChannel().isEmpty())
        str << "Remote: " << rp.remoteChannel().toDisplayString() << '\n';
    if (!rp.qmlServer().host().isEmpty())
        str << "QML server: " << rp.qmlServer().host() << ':' << rp.qmlServer().port() << '\n';
    str << "Sysroot: " << rp.sysRoot() << '\n';
    str << "Debug Source Location: " << rp.debugSourceLocation().join(':') << '\n';
    return rc;
}

static void createNewDock(QWidget *widget)
{
    auto dockWidget = new QDockWidget;
    dockWidget->setWidget(widget);
    dockWidget->setWindowTitle(widget->windowTitle());
    dockWidget->setFeatures(QDockWidget::DockWidgetClosable);
    dockWidget->show();
}

void DebuggerEngine::showModuleSymbols(const FilePath &moduleName, const Symbols &symbols)
{
    auto w = new QTreeWidget;
    w->setUniformRowHeights(true);
    w->setColumnCount(5);
    w->setRootIsDecorated(false);
    w->setAlternatingRowColors(true);
    w->setSortingEnabled(true);
    w->setObjectName("Symbols." + moduleName.toFSPathString());
    QStringList header;
    header.append(Tr::tr("Symbol"));
    header.append(Tr::tr("Address"));
    header.append(Tr::tr("Code"));
    header.append(Tr::tr("Section"));
    header.append(Tr::tr("Name"));
    w->setHeaderLabels(header);
    w->setWindowTitle(Tr::tr("Symbols in \"%1\"").arg(moduleName.toUserOutput()));
    for (const Symbol &s : symbols) {
        auto it = new QTreeWidgetItem;
        it->setData(0, Qt::DisplayRole, s.name);
        it->setData(1, Qt::DisplayRole, s.address);
        it->setData(2, Qt::DisplayRole, s.state);
        it->setData(3, Qt::DisplayRole, s.section);
        it->setData(4, Qt::DisplayRole, s.demangled);
        w->addTopLevelItem(it);
    }
    createNewDock(w);
}

void DebuggerEngine::showModuleSections(const FilePath &moduleName, const Sections &sections)
{
    auto w = new QTreeWidget;
    w->setUniformRowHeights(true);
    w->setColumnCount(5);
    w->setRootIsDecorated(false);
    w->setAlternatingRowColors(true);
    w->setSortingEnabled(true);
    w->setObjectName("Sections." + moduleName.toFSPathString());
    QStringList header;
    header.append(Tr::tr("Name"));
    header.append(Tr::tr("From"));
    header.append(Tr::tr("To"));
    header.append(Tr::tr("Address"));
    header.append(Tr::tr("Flags"));
    w->setHeaderLabels(header);
    w->setWindowTitle(Tr::tr("Sections in \"%1\"").arg(moduleName.toUserOutput()));
    for (const Section &s : sections) {
        auto it = new QTreeWidgetItem;
        it->setData(0, Qt::DisplayRole, s.name);
        it->setData(1, Qt::DisplayRole, s.from);
        it->setData(2, Qt::DisplayRole, s.to);
        it->setData(3, Qt::DisplayRole, s.address);
        it->setData(4, Qt::DisplayRole, s.flags);
        w->addTopLevelItem(it);
    }
    createNewDock(w);
}

// CppDebuggerEngine

Context CppDebuggerEngine::languageContext() const
{
    return Context(Constants::C_CPPDEBUGGER);
}

void CppDebuggerEngine::validateRunParameters(DebuggerRunParameters &rp)
{
    static const Key warnOnInappropriateDebuggerKey = "DebuggerWarnOnInappropriateDebugger";

    const bool warnOnRelease = settings().warnOnReleaseBuilds()
                               && rp.toolChainAbi().osFlavor() != Abi::AndroidLinuxFlavor;
    bool warnOnInappropriateDebugger = false;
    QString detailedWarning;
    switch (rp.toolChainAbi().binaryFormat()) {
    case Abi::PEFormat: {
        if (CheckableDecider(warnOnInappropriateDebuggerKey).shouldAskAgain()) {
            QString preferredDebugger;
            if (rp.toolChainAbi().osFlavor() == Abi::WindowsMSysFlavor) {
                if (rp.cppEngineType() == CdbEngineType)
                    preferredDebugger = "GDB";
            } else if (rp.cppEngineType() != CdbEngineType && rp.cppEngineType() != LldbEngineType) {
                // osFlavor() is MSVC, so the recommended debugger is still CDB,
                // but don't warn for LLDB which starts to be usable, too.
                preferredDebugger = "CDB";
            }
            if (!preferredDebugger.isEmpty()) {
                warnOnInappropriateDebugger = true;
                detailedWarning = Tr::tr(
                                      "The executable uses the Portable Executable format.\n"
                                      "Selecting %1 as debugger would improve the debugging "
                                      "experience for this binary format.")
                                      .arg(preferredDebugger);
                break;
            }
        }
        if (warnOnRelease
                && rp.cppEngineType() == CdbEngineType
                && rp.startMode() != AttachToRemoteServer) {
            QTC_ASSERT(!rp.symbolFile().isEmpty(), return);
            if (!rp.symbolFile().exists() && !rp.symbolFile().endsWith(".exe"))
                rp.setSymbolFile(rp.symbolFile().stringAppended(".exe"));
            QString errorMessage;
            QStringList rc;
            if (getPDBFiles(rp.symbolFile().toUrlishString(), &rc, &errorMessage) && !rc.isEmpty())
                return;
            if (!errorMessage.isEmpty()) {
                detailedWarning.append('\n');
                detailedWarning.append(errorMessage);
            }
        } else {
            return;
        }
        break;
    }
    case Abi::ElfFormat: {
        if (CheckableDecider(warnOnInappropriateDebuggerKey).shouldAskAgain()) {
            if (rp.cppEngineType() == CdbEngineType) {
                warnOnInappropriateDebugger = true;
                detailedWarning = Tr::tr(
                    "The inferior is in the ELF format.\n"
                    "Selecting GDB or LLDB as debugger would improve the debugging "
                    "experience for this binary format.");
                break;
            }
        }

        ElfReader reader(rp.symbolFile());
        const ElfData elfData = reader.readHeaders();
        const QString error = reader.errorString();

        showMessage("EXAMINING " + rp.symbolFile().toUrlishString(), LogDebug);
        QByteArray msg = "ELF SECTIONS: ";

        static const QList<QByteArray> interesting = {
            ".debug_info",
            ".debug_abbrev",
            ".debug_line",
            ".debug_str",
            ".debug_loc",
            ".debug_range",
            ".gdb_index",
            ".note.gnu.build-id",
            ".gnu.hash",
            ".gnu_debuglink"
        };

        QSet<QByteArray> seen;
        for (const ElfSectionHeader &header : elfData.sectionHeaders) {
            msg.append(header.name);
            msg.append(' ');
            if (interesting.contains(header.name))
                seen.insert(header.name);
        }
        showMessage(QString::fromUtf8(msg), LogDebug);

        if (!error.isEmpty()) {
            showMessage("ERROR WHILE READING ELF SECTIONS: " + error, LogDebug);
            return;
        }

        if (elfData.sectionHeaders.isEmpty()) {
            showMessage("NO SECTION HEADERS FOUND. IS THIS AN EXECUTABLE?", LogDebug);
            return;
        }

        // Note: .note.gnu.build-id also appears in regular release builds.
        // bool hasBuildId = elfData.indexOf(".note.gnu.build-id") >= 0;
        bool hasEmbeddedInfo = elfData.indexOf(".debug_info") >= 0;
        bool hasLink = elfData.indexOf(".gnu_debuglink") >= 0;
        if (hasEmbeddedInfo) {
            const QMap<QString, QString> sourcePathMap = settings().sourcePathMap();
            QList<QPair<QRegularExpression, QString>> globalRegExpSourceMap;
            globalRegExpSourceMap.reserve(sourcePathMap.size());
            for (auto it = sourcePathMap.begin(), end = sourcePathMap.end(); it != end; ++it) {
                if (it.key().startsWith('(')) {
                    const QString expanded = rp.macroExpander()->expand(it.value());
                    if (!expanded.isEmpty())
                        globalRegExpSourceMap.push_back({QRegularExpression(it.key()), expanded});
                }
            }
            if (globalRegExpSourceMap.isEmpty())
                return;
            if (std::unique_ptr<ElfMapper> mapper = reader.readSection(".debug_str")) {
                const char *str = mapper->start;
                const char *limit = str + mapper->fdlen;
                bool found = false;
                while (str < limit) {
                    const QString string = QString::fromUtf8(str);
                    for (auto pair : std::as_const(globalRegExpSourceMap)) {
                        const QRegularExpressionMatch match = pair.first.match(string);
                        if (match.hasMatch()) {
                            rp.insertSourcePath(string.left(match.capturedStart()) + match.captured(1),
                                                pair.second);
                            found = true;
                            break;
                        }
                    }
                    if (found)
                        break;

                    const int len = int(strlen(str));
                    if (len == 0)
                        break;
                    str += len + 1;
                }
            }
        }
        if (hasEmbeddedInfo || hasLink)
            return;

        for (const QByteArray &name : std::as_const(interesting)) {
            const QString found = seen.contains(name) ? Tr::tr("Found.")
                                                      : Tr::tr("Not found.");
            detailedWarning.append('\n' + Tr::tr("Section %1: %2").arg(QString::fromUtf8(name)).arg(found));
        }
        break;
    }
    default:
        return;
    }
    if (warnOnInappropriateDebugger) {
        CheckableMessageBox::information(
            Tr::tr("Warning"),
            Tr::tr("The selected debugger may be inappropriate for the inferior.\n"
                   "Examining symbols and setting breakpoints by file name and line number "
                   "may fail.\n")
                + '\n' + detailedWarning,
            warnOnInappropriateDebuggerKey);
    } else if (warnOnRelease) {
        AsynchronousMessageBox::information(Tr::tr("Warning"),
               Tr::tr("This does not seem to be a \"Debug\" build.\n"
                  "Setting breakpoints by file name and line number may fail.")
               + '\n' + detailedWarning);
    }
}

} // namespace Internal
} // namespace Debugger

#include "debuggerengine.moc"
