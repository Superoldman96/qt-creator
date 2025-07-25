# Copyright (C) 2016 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

import random
import string
source("../../shared/qtcreator.py")

toolButton = ("{toolTip='%s' type='QToolButton' unnamed='1' visible='1' "
              "window=':Qt Creator_Core::Internal::MainWindow'}")

def generateRandomFilePath(isWin, isHeader):
    # generate random (fake) file path
    filePath = ''.join(random.choice(string.ascii_letters + string.digits + "/")
                    for _ in range(random.randint(38, 50)))
    if not filePath.startswith("/"):
        filePath = "/" + filePath
    if isWin:
        filePath = "C:" + filePath
    if isHeader:
        filePath += ".h"
    else:
        filePath += ".cpp"
    return filePath

def generateRandomTaskType():
    ranType = random.randint(1, 100)
    if ranType <= 45:
        return 0
    if ranType <= 90:
        return 1
    return 2

def generateMockTasksFile():
    descriptions = ["dummy information", "unknown error", "not found", "syntax error",
                    "missing information", "unused"]
    tasks = ["warn", "error", "other"]
    isWin = platform.system() in ('Microsoft', 'Windows')
    fileName = os.path.join(tempDir(), "dummy_taskfile.tasks")
    tFile = open(fileName, "w")
    tasksCount = [0, 0, 0]
    for counter in range(1100):
        fData = generateRandomFilePath(isWin, counter % 2 == 0)
        lData = random.randint(-1, 10000)
        tasksType = generateRandomTaskType()
        tasksCount[tasksType] += 1
        tData = tasks[tasksType]
        dData = descriptions[random.randint(0, len(descriptions) - 1)]
        tFile.write("%s\t%d\t%s\t%s\n" % (fData, lData, tData, dData))
    tFile.close()
    test.log("Wrote tasks file with %d warnings, %d errors and %d other tasks." % tuple(tasksCount))
    return fileName, tasksCount

def checkOrUncheckMyTasks():
    filterButton = waitForObject(toolButton % 'Filter by categories')
    clickButton(filterButton)
    activateItem(waitForObjectItem("{type='QMenu' unnamed='1' visible='1' "
                                   "window=':Qt Creator_Core::Internal::MainWindow'}",
                                   "My Tasks"))

def getBuildIssuesTypeCounts(model):
    issueTypes = list(map(lambda x: x.data(Qt.UserRole + 1).toInt(), dumpIndices(model)))
    result = [issueTypes.count(0), issueTypes.count(1), issueTypes.count(3)]
    if len(issueTypes) != sum(result):
        test.fatal("Found unexpected value(s) for TaskType...")
    return result

def main():
    tasksFile, issueTypes = generateMockTasksFile()
    expectedNo = sum(issueTypes)
    startQC()
    if not startedWithoutPluginError():
        return
    ensureChecked(":Qt Creator_Issues_Core::Internal::OutputPaneToggleButton")
    model = waitForObject(":Qt Creator.Issues_QListView").model()
    test.verify(model.rowCount() == 0, 'Got an empty issue list to start from.')
    invokeMenuItem("File", "Open File or Project...")
    selectFromFileDialog(tasksFile, False, True)
    starttime = datetime.utcnow()
    waitFor("model.rowCount() == expectedNo", 10000)
    endtime = datetime.utcnow()
    differenceMS = (endtime - starttime).microseconds + (endtime - starttime).seconds * 1000000
    test.verify(differenceMS < 2000000, "Verifying whether loading the tasks "
                "file took less than 2s. (%dµs)" % differenceMS)
    others, errors, warnings = getBuildIssuesTypeCounts(model)
    test.compare(issueTypes, [warnings, errors, others],
                 "Verifying whether all expected errors, warnings and other tasks are listed.")

    # check filtering by 'Show Warnings'
    warnButton = waitForObject(toolButton % 'Show Warnings')
    ensureChecked(warnButton, False)
    waitFor("model.rowCount() == issueTypes[1]", 2000)
    test.compare(model.rowCount(), issueTypes[1], "Verifying only errors are listed.")
    ensureChecked(warnButton, True)
    waitFor("model.rowCount() == expectedNo", 2000)
    test.compare(model.rowCount(), expectedNo, "Verifying all tasks are listed.")

    # check filtering by 'My Tasks'
    checkOrUncheckMyTasks()
    waitFor("model.rowCount() == 0", 2000)
    test.compare(model.rowCount(), 0,
                 "Verifying whether unchecking 'My Tasks' hides all tasks from tasks file.")
    checkOrUncheckMyTasks()
    waitFor("model.rowCount() == expectedNo", 2000)
    test.compare(model.rowCount(), expectedNo,
                 "Verifying whether checking 'My Tasks' displays all tasks from tasks file.")

    # check filtering by 'My Tasks' and 'Show Warnings'
    ensureChecked(warnButton, False)
    waitFor("model.rowCount() == issueTypes[1]", 2000)
    checkOrUncheckMyTasks()
    waitFor("model.rowCount() == 0", 2000)
    test.compare(model.rowCount(), 0,
                 "Verifying whether unchecking 'My Tasks' with disabled 'Show Warnings' hides all.")
    ensureChecked(warnButton, True)
    waitFor("model.rowCount() != 0", 2000)
    test.compare(model.rowCount(), 0,
                 "Verifying whether enabling 'Show Warnings' still displays nothing.")
    checkOrUncheckMyTasks()
    waitFor("model.rowCount() == expectedNo", 2000)
    test.compare(model.rowCount(), expectedNo,
                 "Verifying whether checking 'My Tasks' displays all again.")
    ensureChecked(warnButton, False)
    waitFor("model.rowCount() == issueTypes[1]", 2000)
    test.compare(model.rowCount(), issueTypes[1], "Verifying whether 'My Tasks' with disabled "
                 "'Show Warnings' displays only error tasks.")
    invokeMenuItem("File", "Exit")
