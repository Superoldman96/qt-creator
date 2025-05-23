# Copyright (C) 2016 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

source("../../shared/qtcreator.py")

def main():
    startQC()
    if not startedWithoutPluginError():
        return
    # using a temporary directory won't mess up a potentially existing
    createNewQtQuickApplication(tempDir(), "untitled")
    originalText = prepareQmlFile()
    if originalText:
        testReIndent(originalText)
    invokeMenuItem("File", "Exit")

def prepareQmlFile():
    if not openDocument("untitled.appuntitled.Source Files.Main\\.qml"):
        test.fatal("Could not open Main.qml")
        return None
    editor = waitForObject(":Qt Creator_QmlJSEditor::QmlJSTextEditorWidget")
    isDarwin = platform.system() == 'Darwin'
    for i in range(3):
        if not placeCursorToLine(editor, 'title: qsTr("Hello World")'):
            test.fatal("Couldn't find line(s) I'm looking for - QML file seems to "
                       "have changed!\nLeaving test...")
            return None
        # add some copyable code
        if i == 0:
            code = ["", "MouseArea {", "anchors.fill: parent", "onClicked: {",
                    "console.log(parent.title)"]
            typeLines(editor, code)
            # avoid having 'correctly' indented empty line
            if isDarwin:
                type(editor, "<Command+Shift+Left>")
            else:
                type(editor, "<Shift+Home>")
            type(editor, "<Delete>")
            # get back to the first entered line
            for _ in range(5):
                type(editor, "<Up>")
            if isDarwin:
                type(editor, "<Command+Right>")
            else:
                type(editor, "<End>")
        else:
            type(editor, "<Up>")
        type(editor, "<Right>")
        # mark until the end of file
        if isDarwin:
            markText(editor, "End")
        else:
            markText(editor, "Ctrl+End")
        # unmark the closing brace
        type(editor, "<Shift+Up>")
        type(editor, "<Ctrl+c>")
        for _ in range(11):
            type(editor, "<Ctrl+v>")
    # assume the current editor content to be indented correctly
    originalText = "%s" % editor.plainText
    indented = editor.plainText
    lines = str(indented).splitlines()
    test.log("Using %d lines..." % len(lines))
    editor.plainText = "\n".join([line.lstrip() for line in lines]) + "\n"
    return originalText

def testReIndent(originalText):
    editor = waitForObject(":Qt Creator_QmlJSEditor::QmlJSTextEditorWidget")
    type(editor, "<Ctrl+a>")
    filenameCombo = waitForObject(":Qt Creator_FilenameQComboBox")
    test.log("calling re-indent")
    starttime = datetime.utcnow()
    type(editor, "<Ctrl+i>")
    waitFor("str(filenameCombo.currentText).endswith('*')", 25000)
    endtime = datetime.utcnow()
    test.xverify(originalText == str(editor.plainText),
                 "Verify that indenting restored the original text. "
                 "This may fail when empty lines are being indented.")
    invokeMenuItem("File", "Save All")
    waitFor("originalText == str(editor.plainText)", 25000)
    textAfterReIndent = "%s" % editor.plainText
    if originalText==textAfterReIndent:
        test.passes("Text successfully re-indented after saving (indentation took: %d seconds)" % (endtime-starttime).seconds)
    else:
        # shrink the texts - it's huge output that takes long time to finish & screenshot is taken as well
        originalText = shrinkText(originalText, 20)
        textAfterReIndent = shrinkText(textAfterReIndent, 20)
        test.fail("Re-indent of text unsuccessful...",
                  "Original (first 20 lines):\n%s\n\n______________________\nAfter re-indent (first 20 lines):\n%s"
                  % (originalText, textAfterReIndent))

def shrinkText(txt, lines=10):
    return "".join(txt.splitlines(True)[0:lines])
