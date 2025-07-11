// Copyright (C) 2019 Denis Shienkov <denis.shienkov@gmail.com>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "sdccparser.h"

#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/task.h>

#include <QRegularExpression>

using namespace ProjectExplorer;
using namespace Utils;

namespace BareMetal::Internal {

// Helpers:

static Task::TaskType taskType(const QString &msgType)
{
    if (msgType == "warning" || msgType == "Warning") {
        return Task::TaskType::Warning;
    } else if (msgType == "error" || msgType == "Error"
               || msgType == "syntax error") {
        return Task::TaskType::Error;
    }
    return Task::TaskType::Unknown;
}

// SdccParser

SdccParser::SdccParser()
{
    setObjectName("SdccParser");
}

Utils::Id SdccParser::id()
{
    return "BareMetal.OutputParser.Sdcc";
}

void SdccParser::newTask(const Task &task)
{
    flush();
    m_lastTask = task;
    m_lines = 1;
}

void SdccParser::amendDescription(const QString &desc)
{
    m_lastTask.addToDetails(desc);
    ++m_lines;
}

OutputLineParser::Result SdccParser::handleLine(const QString &line, OutputFormat type)
{
    if (type == StdOutFormat)
        return Status::NotHandled;

    const QString lne = rightTrimmed(line);

    QRegularExpression re;
    QRegularExpressionMatch match;

    re.setPattern("^(.+\\.\\S+):(\\d+): (warning|error) (\\d+): (.+)$");
    match = re.match(lne);
    if (match.hasMatch()) {
        enum CaptureIndex { FilePathIndex = 1, LineNumberIndex,
                            MessageTypeIndex, MessageCodeIndex, MessageTextIndex };
        const Utils::FilePath fileName = Utils::FilePath::fromUserInput(
                    match.captured(FilePathIndex));
        const int lineno = match.captured(LineNumberIndex).toInt();
        const Task::TaskType type = taskType(match.captured(MessageTypeIndex));
        const QString descr = match.captured(MessageTextIndex);
        newTask(CompileTask(type, descr, absoluteFilePath(fileName), lineno));
        LinkSpecs linkSpecs;
        addLinkSpecForAbsoluteFilePath(
            linkSpecs, m_lastTask.file(), m_lastTask.line(), m_lastTask.column(), match, FilePathIndex);
        return {Status::InProgress, linkSpecs};
    }

    re.setPattern("^(.+\\.\\S+):(\\d+): (Error|error|syntax error): (.+)$");
    match = re.match(lne);
    if (match.hasMatch()) {
        enum CaptureIndex { FilePathIndex = 1, LineNumberIndex,
                            MessageTypeIndex, MessageTextIndex };
        const Utils::FilePath fileName = Utils::FilePath::fromUserInput(
                    match.captured(FilePathIndex));
        const int lineno = match.captured(LineNumberIndex).toInt();
        const Task::TaskType type = taskType(match.captured(MessageTypeIndex));
        const QString descr = match.captured(MessageTextIndex);
        newTask(CompileTask(type, descr, absoluteFilePath(fileName), lineno));
        LinkSpecs linkSpecs;
        addLinkSpecForAbsoluteFilePath(
            linkSpecs, m_lastTask.file(), m_lastTask.line(), m_lastTask.column(), match, FilePathIndex);
        return {Status::InProgress, linkSpecs};
    }

    re.setPattern("^at (\\d+): (warning|error) \\d+: (.+)$");
    match = re.match(lne);
    if (match.hasMatch()) {
        enum CaptureIndex { MessageCodeIndex = 1, MessageTypeIndex, MessageTextIndex };
        const Task::TaskType type = taskType(match.captured(MessageTypeIndex));
        const QString descr = match.captured(MessageTextIndex);
        newTask(CompileTask(type, descr));
        return Status::InProgress;
    }

    re.setPattern("^\\?ASlink-(Warning|Error)-(.+)$");
    match = re.match(lne);
    if (match.hasMatch()) {
        enum CaptureIndex { MessageTypeIndex = 1, MessageTextIndex };
        const Task::TaskType type = taskType(match.captured(MessageTypeIndex));
        const QString descr = match.captured(MessageTextIndex);
        newTask(CompileTask(type, descr));
        return Status::InProgress;
    }

    if (!m_lastTask.isNull()) {
        amendDescription(lne);
        return Status::InProgress;
    }

    flush();
    return Status::NotHandled;
}

void SdccParser::flush()
{
    if (m_lastTask.isNull())
        return;

    setDetailsFormat(m_lastTask);
    Task t = m_lastTask;
    m_lastTask.clear();
    scheduleTask(t, m_lines, 1);
    m_lines = 0;
}

} // BareMetal::Internal

// Unit tests:

#ifdef WITH_TESTS
#include <projectexplorer/outputparser_test.h>
#include <QTest>

namespace BareMetal::Internal {

class SdccParserTest final : public QObject
{
   Q_OBJECT

private slots:
   void testSdccOutputParsers_data();
   void testSdccOutputParsers();
};

void SdccParserTest::testSdccOutputParsers_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<OutputParserTester::Channel>("inputChannel");
    QTest::addColumn<QStringList>("childStdOutLines");
    QTest::addColumn<QStringList>("childStdErrLines");
    QTest::addColumn<Tasks>("tasks");

    QTest::newRow("pass-through stdout")
            << "Sometext" << OutputParserTester::STDOUT
            << QStringList("Sometext") << QStringList()
            << Tasks();
    QTest::newRow("pass-through stderr")
            << "Sometext" << OutputParserTester::STDERR
            << QStringList() << QStringList("Sometext")
            << Tasks();

    // Compiler messages.

    QTest::newRow("Assembler error")
            << QString::fromLatin1("c:\\foo\\main.c:63: Error: Some error")
            << OutputParserTester::STDERR
            << QStringList()
            << QStringList()
            << (Tasks() << CompileTask(Task::Error,
                                       "Some error",
                                       FilePath::fromUserInput("c:\\foo\\main.c"),
                                       63));

    QTest::newRow("Compiler single line warning")
            << QString::fromLatin1("c:\\foo\\main.c:63: warning 123: Some warning")
            << OutputParserTester::STDERR
            << QStringList()
            << QStringList()
            << (Tasks() << CompileTask(Task::Warning,
                                       "Some warning",
                                       FilePath::fromUserInput("c:\\foo\\main.c"),
                                       63));

    QTest::newRow("Compiler multi line warning")
            << QString::fromLatin1("c:\\foo\\main.c:63: warning 123: Some warning\n"
                                   "details #1\n"
                                   "  details #2")
            << OutputParserTester::STDERR
            << QStringList()
            << QStringList()
            << (Tasks() << CompileTask(Task::Warning,
                                       "Some warning\n"
                                       "details #1\n"
                                       "  details #2",
                                       FilePath::fromUserInput("c:\\foo\\main.c"),
                                       63));

    QTest::newRow("Compiler simple single line error")
            << QString::fromLatin1("c:\\foo\\main.c:63: error: Some error")
            << OutputParserTester::STDERR
            << QStringList()
            << QStringList()
            << (Tasks() << CompileTask(Task::Error,
                                       "Some error",
                                       FilePath::fromUserInput("c:\\foo\\main.c"),
                                       63));

    QTest::newRow("Compiler single line error")
            << QString::fromLatin1("c:\\foo\\main.c:63: error 123: Some error")
            << OutputParserTester::STDERR
            << QStringList()
            << QStringList()
            << (Tasks() << CompileTask(Task::Error,
                                       "Some error",
                                       FilePath::fromUserInput("c:\\foo\\main.c"),
                                       63));

    QTest::newRow("Compiler multi line error")
            << QString::fromLatin1("c:\\foo\\main.c:63: error 123: Some error\n"
                                   "details #1\n"
                                   "  details #2")
            << OutputParserTester::STDERR
            << QStringList()
            << QStringList()
            << (Tasks() << CompileTask(Task::Error,
                                       "Some error\n"
                                       "details #1\n"
                                       "  details #2",
                                       FilePath::fromUserInput("c:\\foo\\main.c"),
                                       63));

    QTest::newRow("Compiler syntax error")
            << QString::fromLatin1("c:\\foo\\main.c:63: syntax error: Some error")
            << OutputParserTester::STDERR
            << QStringList()
            << QStringList()
            << (Tasks() << CompileTask(Task::Error,
                                       "Some error",
                                       FilePath::fromUserInput("c:\\foo\\main.c"),
                                       63));

    QTest::newRow("Compiler bad option error")
            << QString::fromLatin1("at 1: error 123: Some error")
            << OutputParserTester::STDERR
            << QStringList()
            << QStringList()
            << (Tasks() << CompileTask(Task::Error,
                                       "Some error"));

    QTest::newRow("Compiler bad option warning")
            << QString::fromLatin1("at 1: warning 123: Some warning")
            << OutputParserTester::STDERR
            << QStringList()
            << QStringList()
            << (Tasks() << CompileTask(Task::Warning,
                                       "Some warning"));

    QTest::newRow("Linker warning")
            << QString::fromLatin1("?ASlink-Warning-Couldn't find library 'foo.lib'")
            << OutputParserTester::STDERR
            << QStringList()
            << QStringList()
            << (Tasks() << CompileTask(Task::Warning,
                                       "Couldn't find library 'foo.lib'"));

    QTest::newRow("Linker error")
            << QString::fromLatin1("?ASlink-Error-<cannot open> : \"foo.rel\"")
            << OutputParserTester::STDERR
            << QStringList()
            << QStringList()
            << (Tasks() << CompileTask(Task::Error,
                                       "<cannot open> : \"foo.rel\""));
}

void SdccParserTest::testSdccOutputParsers()
{
    OutputParserTester testbench;
    testbench.addLineParser(new SdccParser);
    QFETCH(QString, input);
    QFETCH(OutputParserTester::Channel, inputChannel);
    QFETCH(Tasks, tasks);
    QFETCH(QStringList, childStdOutLines);
    QFETCH(QStringList, childStdErrLines);

    testbench.testParsing(input, inputChannel, tasks, childStdOutLines, childStdErrLines);
}

QObject *createSdccParserTest()
{
    return new SdccParserTest;
}

} // BareMetal::Internal

#endif // WITH_TESTS

#include "sdccparser.moc"
