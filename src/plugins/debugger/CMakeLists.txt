add_qtc_plugin(Debugger
  DEPENDS LanguageUtils QmlDebug QmlJS registryaccess
  PLUGIN_DEPENDS Core CppEditor ProjectExplorer QtSupport TextEditor
  PLUGIN_RECOMMENDS QmakeProjectManager
  PLUGIN_TEST_DEPENDS QmakeProjectManager
  DEFINES
    CLANG_BINDIR="${LLVM_TOOLS_BINARY_DIR}"
  SOURCES
    analyzer/analyzerutils.h
    analyzer/detailederrorview.cpp analyzer/detailederrorview.h
    breakhandler.cpp breakhandler.h
    breakpoint.cpp breakpoint.h
    cdb/cdbengine.cpp cdb/cdbengine.h
    cdb/cdboptionspage.cpp cdb/cdboptionspage.h
    cdb/cdbparsehelpers.cpp cdb/cdbparsehelpers.h
    cdb/stringinputstream.cpp cdb/stringinputstream.h
    commonoptionspage.cpp commonoptionspage.h
    console/console.cpp console/console.h
    console/consoleedit.cpp console/consoleedit.h
    console/consoleitem.cpp console/consoleitem.h
    console/consoleitemdelegate.cpp console/consoleitemdelegate.h
    console/consoleitemmodel.cpp console/consoleitemmodel.h
    console/consoleproxymodel.cpp console/consoleproxymodel.h
    console/consoleview.cpp console/consoleview.h
    dap/cmakedapengine.cpp dap/cmakedapengine.h
    dap/dapclient.cpp dap/dapclient.h
    dap/dapengine.cpp dap/dapengine.h
    dap/gdbdapengine.cpp dap/gdbdapengine.h
    dap/lldbdapengine.cpp dap/lldbdapengine.h
    dap/pydapengine.cpp dap/pydapengine.h
    debugger_global.h
    debuggeractions.cpp debuggeractions.h
    debuggerconstants.h
    debuggercore.h
    debuggerdialogs.cpp debuggerdialogs.h
    debuggerengine.cpp debuggerengine.h
    debuggericons.cpp debuggericons.h
    debuggerinternalconstants.h
    debuggeritem.cpp debuggeritem.h
    debuggeritemmanager.cpp debuggeritemmanager.h
    debuggerkitaspect.cpp debuggerkitaspect.h
    debuggermainwindow.cpp debuggermainwindow.h
    debuggerplugin.cpp
    debuggerprotocol.cpp debuggerprotocol.h
    debuggerrunconfigurationaspect.cpp debuggerrunconfigurationaspect.h
    debuggerruncontrol.cpp debuggerruncontrol.h
    debuggersourcepathmappingwidget.cpp debuggersourcepathmappingwidget.h
    debuggertest.cpp debuggertest.h
    debuggertooltipmanager.cpp debuggertooltipmanager.h
    debuggertr.h
    disassembleragent.cpp disassembleragent.h
    disassemblerlines.cpp disassemblerlines.h
    gdb/gdbengine.cpp gdb/gdbengine.h
    gdb/gdbsettings.cpp gdb/gdbsettings.h
    imageviewer.cpp imageviewer.h
    enginemanager.cpp enginemanager.h
    lldb/lldbengine.cpp lldb/lldbengine.h
    loadcoredialog.cpp loadcoredialog.h
    localsandexpressionswindow.cpp localsandexpressionswindow.h
    logwindow.cpp logwindow.h
    memoryagent.cpp memoryagent.h
    moduleshandler.cpp moduleshandler.h
    outputcollector.cpp outputcollector.h
    pdb/pdbengine.cpp pdb/pdbengine.h
    peripheralregisterhandler.cpp peripheralregisterhandler.h
    procinterrupt.cpp procinterrupt.h
    qml/interactiveinterpreter.cpp qml/interactiveinterpreter.h
    #qml/qmlcppengine.cpp qml/qmlcppengine.h
    qml/qmlengine.cpp qml/qmlengine.h
    qml/qmlengineutils.cpp qml/qmlengineutils.h
    qml/qmlinspectoragent.cpp qml/qmlinspectoragent.h
    qml/qmlv8debuggerclientconstants.h
    registerhandler.cpp registerhandler.h
    shared/cdbsymbolpathlisteditor.cpp shared/cdbsymbolpathlisteditor.h
    shared/coredumputils.cpp shared/coredumputils.h
    shared/hostutils.cpp shared/hostutils.h
    shared/peutils.cpp shared/peutils.h
    shared/symbolpathsdialog.cpp shared/symbolpathsdialog.h
    simplifytype.cpp simplifytype.h
    sourceagent.cpp sourceagent.h
    sourcefileshandler.cpp sourcefileshandler.h
    sourceutils.cpp sourceutils.h
    stackframe.cpp stackframe.h
    stackhandler.cpp stackhandler.h
    stackwindow.cpp stackwindow.h
    terminal.cpp terminal.h
    threaddata.h
    threadshandler.cpp threadshandler.h
    unstartedappwatcherdialog.cpp unstartedappwatcherdialog.h
    uvsc/uvscclient.cpp uvsc/uvscclient.h
    uvsc/uvscdatatypes.h
    uvsc/uvscengine.cpp uvsc/uvscengine.h
    uvsc/uvscfunctions.h
    uvsc/uvscutils.cpp uvsc/uvscutils.h
    watchdata.cpp watchdata.h
    watchdelegatewidgets.cpp watchdelegatewidgets.h
    watchhandler.cpp watchhandler.h
    watchutils.cpp watchutils.h
    watchwindow.cpp watchwindow.h
)

extend_qtc_plugin(Debugger
  CONDITION WIN32
  SOURCES registerpostmortemaction.cpp registerpostmortemaction.h
  DEFINES UNICODE _UNICODE
)

file(GLOB_RECURSE images RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} images/*)
qtc_add_resources(Debugger "images"
  PREFIX "/debugger"
  BASE "."
  FILES ${images}
)
file(GLOB_RECURSE test_cases RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} unit-tests/*)
qtc_add_resources(Debugger "testcases"
  CONDITION WITH_TESTS
  PREFIX "/debugger"
  BASE "."
  FILES ${test_cases}
)
