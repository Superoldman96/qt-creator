// Copyright (C) 2016 Andre Hartmann.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "customparser.h"

#include "projectexplorer.h"
#include "projectexplorerconstants.h"
#include "projectexplorertr.h"
#include "task.h"

#include <coreplugin/icore.h>

#include <utils/algorithm.h>
#include <utils/qtcassert.h>

#include <QCheckBox>
#include <QLabel>
#include <QPair>
#include <QString>
#include <QVBoxLayout>

#ifdef WITH_TESTS
#include "outputparser_test.h"
#include <QTest>
#endif

using namespace ProjectExplorer::Internal;
using namespace Utils;

const char idKey[] = "Id";
const char nameKey[] = "Name";
const char errorKey[] = "Error";
const char warningKey[] = "Warning";
const char patternKey[] = "Pattern";
const char lineNumberCapKey[] = "LineNumberCap";
const char fileNameCapKey[] = "FileNameCap";
const char messageCapKey[] = "MessageCap";
const char channelKey[] = "Channel";
const char exampleKey[] = "Example";
const char buildDefaultKey[] = "BuildDefault";
const char runDefaultKey[] = "RunDefault";

namespace ProjectExplorer {

bool CustomParserExpression::operator ==(const CustomParserExpression &other) const
{
    return pattern() == other.pattern() && fileNameCap() == other.fileNameCap()
            && lineNumberCap() == other.lineNumberCap() && messageCap() == other.messageCap()
            && channel() == other.channel() && example() == other.example();
}

QString CustomParserExpression::pattern() const
{
    return m_regExp.pattern();
}

void CustomParserExpression::setPattern(const QString &pattern)
{
    m_regExp.setPattern(pattern);
    QTC_CHECK(m_regExp.isValid());
}

CustomParserExpression::CustomParserChannel CustomParserExpression::channel() const
{
    return m_channel;
}

void CustomParserExpression::setChannel(CustomParserExpression::CustomParserChannel channel)
{
    if (channel == ParseNoChannel || channel > ParseBothChannels)
        channel = ParseBothChannels;
    m_channel = channel;
}

QString CustomParserExpression::example() const
{
    return m_example;
}

void CustomParserExpression::setExample(const QString &example)
{
    m_example = example;
}

int CustomParserExpression::messageCap() const
{
    return m_messageCap;
}

void CustomParserExpression::setMessageCap(int messageCap)
{
    m_messageCap = messageCap;
}

Store CustomParserExpression::toMap() const
{
    Store map;
    map.insert(patternKey, pattern());
    map.insert(messageCapKey, messageCap());
    map.insert(fileNameCapKey, fileNameCap());
    map.insert(lineNumberCapKey, lineNumberCap());
    map.insert(exampleKey, example());
    map.insert(channelKey, channel());
    return map;
}

void CustomParserExpression::fromMap(const Store &map)
{
    setPattern(map.value(patternKey).toString());
    setMessageCap(map.value(messageCapKey).toInt());
    setFileNameCap(map.value(fileNameCapKey).toInt());
    setLineNumberCap(map.value(lineNumberCapKey).toInt());
    setExample(map.value(exampleKey).toString());
    setChannel(static_cast<CustomParserChannel>(map.value(channelKey).toInt()));
}

int CustomParserExpression::lineNumberCap() const
{
    return m_lineNumberCap;
}

void CustomParserExpression::setLineNumberCap(int lineNumberCap)
{
    m_lineNumberCap = lineNumberCap;
}

int CustomParserExpression::fileNameCap() const
{
    return m_fileNameCap;
}

void CustomParserExpression::setFileNameCap(int fileNameCap)
{
    m_fileNameCap = fileNameCap;
}

bool CustomParserSettings::operator ==(const CustomParserSettings &other) const
{
    return id == other.id && displayName == other.displayName
            && error == other.error && warning == other.warning
            && buildDefault == other.buildDefault
            && runDefault == other.runDefault;
}

Store CustomParserSettings::toMap() const
{
    Store map;
    map.insert(idKey, id.toSetting());
    map.insert(nameKey, displayName);
    map.insert(errorKey, variantFromStore(error.toMap()));
    map.insert(warningKey, variantFromStore(warning.toMap()));
    map.insert(buildDefaultKey, buildDefault);
    map.insert(runDefaultKey, runDefault);
    return map;
}

void CustomParserSettings::fromMap(const Store &map)
{
    id = Id::fromSetting(map.value(idKey));
    displayName = map.value(nameKey).toString();
    error.fromMap(storeFromVariant(map.value(errorKey)));
    warning.fromMap(storeFromVariant(map.value(warningKey)));
    buildDefault = map.value(buildDefaultKey).toBool();
    runDefault = map.value(runDefaultKey).toBool();
}

CustomParsersAspect::CustomParsersAspect(BuildConfiguration *bc)
{
    Q_UNUSED(bc)
    setId("CustomOutputParsers");
    setSettingsKey("CustomOutputParsers");
    setDisplayName(Tr::tr("Custom Output Parsers"));
    addDataExtractor(this, &CustomParsersAspect::parsers, &Data::parsers);
    setConfigWidgetCreator([this] {
        const auto widget =
                new CustomParsersSelectionWidget(CustomParsersSelectionWidget::InRunConfig);
        for (const auto &s : ProjectExplorerPlugin::customParsers()) {
            if (s.runDefault && !m_parsers.contains(s.id))
                m_parsers.append(s.id);
        }
        widget->setSelectedParsers(m_parsers);
        connect(widget, &Internal::CustomParsersSelectionWidget::selectionChanged,
                this, [this, widget] { m_parsers = widget->selectedParsers(); });
        return widget;
    });
}

OutputTaskParser *createCustomParserFromId(Utils::Id id)
{
    const CustomParserSettings parser = findOrDefault(ProjectExplorerPlugin::customParsers(),
            [id](const CustomParserSettings &p) { return p.id == id; });
    if (parser.id.isValid())
        return new Internal::CustomParser(parser);
    return nullptr;
}

void CustomParsersAspect::fromMap(const Store &map)
{
    m_parsers = transform(map.value(settingsKey()).toList(), &Id::fromSetting);
}

void CustomParsersAspect::toMap(Store &map) const
{
    map.insert(settingsKey(), transform(m_parsers, &Id::toSetting));
}

namespace Internal {

CustomParser::CustomParser(const CustomParserSettings &settings)
{
    setObjectName("CustomParser");

    setSettings(settings);
}

void CustomParser::setSettings(const CustomParserSettings &settings)
{
    m_error = settings.error;
    m_warning = settings.warning;
}

OutputLineParser::Result CustomParser::handleLine(const QString &line, OutputFormat type)
{
    const CustomParserExpression::CustomParserChannel channel = type == StdErrFormat
            ? CustomParserExpression::ParseStdErrChannel
            : CustomParserExpression::ParseStdOutChannel;
    return parseLine(line, channel);
}

OutputLineParser::Result CustomParser::hasMatch(
        const QString &line,
        CustomParserExpression::CustomParserChannel channel,
        const CustomParserExpression &expression,
        Task::TaskType taskType
        )
{
    if (!(channel & expression.channel()))
        return Status::NotHandled;

    if (expression.pattern().isEmpty())
        return Status::NotHandled;

    const QRegularExpressionMatch match = expression.match(line);
    if (!match.hasMatch())
        return Status::NotHandled;

    const FilePath fileName = absoluteFilePath(FilePath::fromString(
                                                   match.captured(expression.fileNameCap())));
    const int lineNumber = match.captured(expression.lineNumberCap()).toInt();
    const QString message = match.captured(expression.messageCap());
    LinkSpecs linkSpecs;
    addLinkSpecForAbsoluteFilePath(linkSpecs, fileName, lineNumber, -1, match,
                                   expression.fileNameCap());
    scheduleTask(CompileTask(taskType, message, fileName, lineNumber), 1);
    return {Status::Done, linkSpecs};
}

OutputLineParser::Result CustomParser::parseLine(
        const QString &rawLine,
        CustomParserExpression::CustomParserChannel channel
        )
{
    const QString line = rightTrimmed(rawLine);
    const Result res = hasMatch(line, channel, m_error, Task::Error);
    if (res.status != Status::NotHandled)
        return res;
    return hasMatch(line, channel, m_warning, Task::Warning);
}

namespace {
class SelectionWidget : public QWidget
{
    Q_OBJECT
public:
    SelectionWidget(CustomParsersSelectionWidget::Embedded where, QWidget *parent)
        : QWidget(parent), m_where(where)
    {
        const auto layout = new QVBoxLayout(this);
        const auto explanatoryLabel = new QLabel(Tr::tr(
            "Custom output parsers scan command line output for user-provided error patterns<br>"
            "to create entries in Issues.<br>"
            "The parsers can be configured <a href=\"dummy\">here</a>."));
        layout->addWidget(explanatoryLabel);
        layout->setContentsMargins(0, 0, 0, 0);
        connect(explanatoryLabel, &QLabel::linkActivated, [] {
            Core::ICore::showOptionsDialog(Constants::CUSTOM_PARSERS_SETTINGS_PAGE_ID);
        });
        updateUi();
        connect(ProjectExplorerPlugin::instance(), &ProjectExplorerPlugin::customParsersChanged,
                this, &SelectionWidget::updateUi);
    }

    void setSelectedParsers(const QList<Utils::Id> &parsers)
    {
        for (const auto &p : std::as_const(parserCheckBoxes))
            p.first->setChecked(parsers.contains(p.second));
        emit selectionChanged();
    }

    QList<Utils::Id> selectedParsers() const
    {
        QList<Utils::Id> parsers;
        for (const auto &p : std::as_const(parserCheckBoxes)) {
            if (p.first->isChecked())
                parsers << p.second;
        }
        return parsers;
    }

signals:
    void selectionChanged();

private:
    void updateUi()
    {
        const auto layout = qobject_cast<QVBoxLayout *>(this->layout());
        QTC_ASSERT(layout, return);
        QList<Utils::Id> parsers = selectedParsers();
        for (const auto &p : std::as_const(parserCheckBoxes))
            delete p.first;
        parserCheckBoxes.clear();
        for (const CustomParserSettings &s : ProjectExplorerPlugin::customParsers()) {
            const auto checkBox = new QCheckBox(s.displayName, this);
            bool isSelected = parsers.contains(s.id);
            bool projectDefault =
                       (m_where == CustomParsersSelectionWidget::InBuildConfig && s.buildDefault)
                    || (m_where == CustomParsersSelectionWidget::InRunConfig  && s.runDefault);
            if (projectDefault) {
                //: %1 = parser display name
                checkBox->setText(Tr::tr("%1 (project default)").arg(s.displayName));
                if (!isSelected)
                     parsers.append(s.id);
            }
            checkBox->setCheckState(parsers.contains(s.id) ? Qt::Checked : Qt::Unchecked);
            connect(checkBox, &QCheckBox::stateChanged, this, &SelectionWidget::selectionChanged);
            parserCheckBoxes.push_back({checkBox, s.id});
            layout->addWidget(checkBox);
        }
        setSelectedParsers(parsers);
    }

    QList<QPair<QCheckBox *, Utils::Id>> parserCheckBoxes;
    const CustomParsersSelectionWidget::Embedded m_where;
};
} // anonymous namespace

CustomParsersSelectionWidget::CustomParsersSelectionWidget(Embedded where, QWidget *parent)
    : DetailsWidget(parent)
{
    const auto widget = new SelectionWidget(where, this);
    connect(widget, &SelectionWidget::selectionChanged, this, [this] {
        updateSummary();
        emit selectionChanged();
    });
    setWidget(widget);
    updateSummary();
}

void CustomParsersSelectionWidget::setSelectedParsers(const QList<Utils::Id> &parsers)
{
    qobject_cast<SelectionWidget *>(widget())->setSelectedParsers(parsers);
}

QList<Utils::Id> CustomParsersSelectionWidget::selectedParsers() const
{
    return qobject_cast<SelectionWidget *>(widget())->selectedParsers();
}

void CustomParsersSelectionWidget::updateSummary()
{
    const QList<Utils::Id> parsers
            = qobject_cast<SelectionWidget *>(widget())->selectedParsers();
    if (parsers.isEmpty())
        setSummaryText(Tr::tr("There are no custom parsers active"));
    else
        setSummaryText(Tr::tr("There are %n custom parsers active", nullptr, parsers.count()));
}

// Unit tests:

#ifdef WITH_TESTS
class CustomParserTest : public QObject
{
    Q_OBJECT

private slots:
    void test_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("workDir");
        QTest::addColumn<OutputParserTester::Channel>("inputChannel");
        QTest::addColumn<CustomParserExpression::CustomParserChannel>("filterErrorChannel");
        QTest::addColumn<CustomParserExpression::CustomParserChannel>("filterWarningChannel");
        QTest::addColumn<QString>("errorPattern");
        QTest::addColumn<int>("errorFileNameCap");
        QTest::addColumn<int>("errorLineNumberCap");
        QTest::addColumn<int>("errorMessageCap");
        QTest::addColumn<QString>("warningPattern");
        QTest::addColumn<int>("warningFileNameCap");
        QTest::addColumn<int>("warningLineNumberCap");
        QTest::addColumn<int>("warningMessageCap");
        QTest::addColumn<QStringList>("childStdOutLines");
        QTest::addColumn<QStringList>("childStdErrLines");
        QTest::addColumn<Tasks>("tasks");

        const QString simplePattern = "^([a-z]+\\.[a-z]+):(\\d+): error: ([^\\s].+)$";
        const FilePath fileName = FilePath::fromUserInput("main.c");

        QTest::newRow("empty patterns")
            << QString::fromLatin1("Sometext")
            << QString()
            << OutputParserTester::STDOUT
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseBothChannels
            << QString() << 1 << 2 << 3
            << QString() << 1 << 2 << 3
            << QStringList("Sometext") << QStringList()
            << Tasks();

        QTest::newRow("pass-through stdout")
            << QString::fromLatin1("Sometext")
            << QString()
            << OutputParserTester::STDOUT
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseBothChannels
            << simplePattern << 1 << 2 << 3
            << QString() << 1 << 2 << 3
            << QStringList("Sometext") << QStringList()
            << Tasks();

        QTest::newRow("pass-through stderr")
            << QString::fromLatin1("Sometext")
            << QString()
            << OutputParserTester::STDERR
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseBothChannels
            << simplePattern << 1 << 2 << 3
            << QString() << 1 << 2 << 3
            << QStringList() << QStringList("Sometext")
            << Tasks();

        const QString simpleError = "main.c:9: error: `sfasdf' undeclared (first use this function)";
        const QString message = "`sfasdf' undeclared (first use this function)";

        QTest::newRow("simple error")
            << simpleError
            << QString()
            << OutputParserTester::STDERR
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseBothChannels
            << simplePattern << 1 << 2 << 3
            << QString() << 0 << 0 << 0
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Error, message, fileName, 9)});

        const QString pathPattern = "^([a-z\\./]+):(\\d+): error: ([^\\s].+)$";
        QString workingDir = "/home/src/project";
        FilePath expandedFileName = "/home/src/project/main.c";

        QTest::newRow("simple error with expanded path")
            << "main.c:9: error: `sfasdf' undeclared (first use this function)"
            << workingDir
            << OutputParserTester::STDERR
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseBothChannels
            << pathPattern << 1 << 2 << 3
            << QString() << 0 << 0 << 0
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Error, message, expandedFileName, 9)});

        expandedFileName = "/home/src/project/subdir/main.c";
        QTest::newRow("simple error with subdir path")
            << "subdir/main.c:9: error: `sfasdf' undeclared (first use this function)"
            << workingDir
            << OutputParserTester::STDERR
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseBothChannels
            << pathPattern << 1 << 2 << 3
            << QString() << 0 << 0 << 0
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Error, message, expandedFileName, 9)});

        workingDir = "/home/src/build-project";
        QTest::newRow("simple error with buildir path")
            << "../project/subdir/main.c:9: error: `sfasdf' undeclared (first use this function)"
            << workingDir
            << OutputParserTester::STDERR
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseBothChannels
            << pathPattern << 1 << 2 << 3
            << QString() << 0 << 0 << 0
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Error, message, expandedFileName, 9)});

        QTest::newRow("simple error on wrong channel")
            << simpleError
            << QString()
            << OutputParserTester::STDOUT
            << CustomParserExpression::ParseStdErrChannel << CustomParserExpression::ParseBothChannels
            << simplePattern << 1 << 2 << 3
            << QString() << 0 << 0 << 0
            << QStringList(simpleError) << QStringList()
            << Tasks();

        QTest::newRow("simple error on other wrong channel")
            << simpleError
            << QString()
            << OutputParserTester::STDERR
            << CustomParserExpression::ParseStdOutChannel << CustomParserExpression::ParseBothChannels
            << simplePattern << 1 << 2 << 3
            << QString() << 0 << 0 << 0
            << QStringList() << QStringList(simpleError)
            << Tasks();

        const QString simpleError2 = "Error: Line 19 in main.c: `sfasdf' undeclared (first use this function)";
        const QString simplePattern2 = "^Error: Line (\\d+) in ([a-z]+\\.[a-z]+): ([^\\s].+)$";
        const int lineNumber2 = 19;

        QTest::newRow("another simple error on stderr")
            << simpleError2
            << QString()
            << OutputParserTester::STDERR
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseBothChannels
            << simplePattern2 << 2 << 1 << 3
            << QString() << 1 << 2 << 3
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Error, message, fileName, lineNumber2)});

        QTest::newRow("another simple error on stdout")
            << simpleError2
            << QString()
            << OutputParserTester::STDOUT
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseBothChannels
            << simplePattern2 << 2 << 1 << 3
            << QString() << 1 << 2 << 3
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Error, message, fileName, lineNumber2)});

        const QString simpleWarningPattern = "^([a-z]+\\.[a-z]+):(\\d+): warning: ([^\\s].+)$";
        const QString simpleWarning = "main.c:1234: warning: `helloWorld' declared but not used";
        const QString warningMessage = "`helloWorld' declared but not used";

        QTest::newRow("simple warning")
            << simpleWarning
            << QString()
            << OutputParserTester::STDERR
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseBothChannels
            << QString() << 1 << 2 << 3
            << simpleWarningPattern << 1 << 2 << 3
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Warning, warningMessage, fileName, 1234)});

        const QString simpleWarning2 = "Warning: `helloWorld' declared but not used (main.c:19)";
        const QString simpleWarningPattern2 = "^Warning: (.*) \\(([a-z]+\\.[a-z]+):(\\d+)\\)$";

        QTest::newRow("another simple warning on stdout")
            << simpleWarning2
            << QString()
            << OutputParserTester::STDOUT
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseStdOutChannel
            << simplePattern2 << 1 << 2 << 3
            << simpleWarningPattern2 << 2 << 3 << 1
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Warning, warningMessage, fileName, lineNumber2)});

        QTest::newRow("warning on wrong channel")
            << simpleWarning2
            << QString()
            << OutputParserTester::STDOUT
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseStdErrChannel
            << QString() << 1 << 2 << 3
            << simpleWarningPattern2 << 2 << 3 << 1
            << QStringList(simpleWarning2) << QStringList()
            << Tasks();

        QTest::newRow("warning on other wrong channel")
            << simpleWarning2
            << QString()
            << OutputParserTester::STDERR
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseStdOutChannel
            << QString() << 1 << 2 << 3
            << simpleWarningPattern2 << 2 << 3 << 1
            << QStringList() << QStringList(simpleWarning2)
            << Tasks();

        QTest::newRow("error and *warning*")
            << simpleWarning
            << QString()
            << OutputParserTester::STDERR
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseBothChannels
            << simplePattern << 1 << 2 << 3
            << simpleWarningPattern << 1 << 2 << 3
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Warning, warningMessage, fileName, 1234)});

        QTest::newRow("*error* when equal pattern")
            << simpleError
            << QString()
            << OutputParserTester::STDERR
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseBothChannels
            << simplePattern << 1 << 2 << 3
            << simplePattern << 1 << 2 << 3
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Error, message, fileName, 9)});

        const QString unitTestError = "../LedDriver/LedDriverTest.c:63: FAIL: Expected 0x0080 Was 0xffff";
        const FilePath unitTestFileName = FilePath::fromUserInput("../LedDriver/LedDriverTest.c");
        const QString unitTestMessage = "Expected 0x0080 Was 0xffff";
        const QString unitTestPattern = "^([^:]+):(\\d+): FAIL: ([^\\s].+)$";
        const int unitTestLineNumber = 63;

        QTest::newRow("unit test error")
            << unitTestError
            << QString()
            << OutputParserTester::STDOUT
            << CustomParserExpression::ParseBothChannels << CustomParserExpression::ParseBothChannels
            << unitTestPattern << 1 << 2 << 3
            << QString() << 1 << 2 << 3
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Error, unitTestMessage, unitTestFileName, unitTestLineNumber)});

        const QString leadingSpacesPattern = "^    MESSAGE:(.+)";
        const QString leadingSpacesMessage = "    MESSAGE:Error";
        const QString noLeadingSpacesMessage = "MESSAGE:Error";
        QTest::newRow("leading spaces: match")
            << leadingSpacesMessage
            << QString()
            << OutputParserTester::STDOUT
            << CustomParserExpression::ParseBothChannels
            << CustomParserExpression::ParseBothChannels
            << leadingSpacesPattern << 2 << 3 << 1
            << QString() << 1 << 2 << 3
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Error, "Error", {}, -1)});
        QTest::newRow("leading spaces: no match")
            << noLeadingSpacesMessage
            << QString()
            << OutputParserTester::STDOUT
            << CustomParserExpression::ParseBothChannels
            << CustomParserExpression::ParseBothChannels
            << leadingSpacesPattern << 2 << 3 << 1
            << QString() << 1 << 2 << 3
            << QStringList(noLeadingSpacesMessage) << QStringList()
            << Tasks();
        const QString noLeadingSpacesPattern = "^MESSAGE:(.+)";
        QTest::newRow("no leading spaces: match")
            << noLeadingSpacesMessage
            << QString()
            << OutputParserTester::STDOUT
            << CustomParserExpression::ParseBothChannels
            << CustomParserExpression::ParseBothChannels
            << noLeadingSpacesPattern << 2 << 3 << 1
            << QString() << 1 << 2 << 3
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Error, "Error", {}, -1)});
        QTest::newRow("no leading spaces: no match")
            << leadingSpacesMessage
            << QString()
            << OutputParserTester::STDOUT
            << CustomParserExpression::ParseBothChannels
            << CustomParserExpression::ParseBothChannels
            << noLeadingSpacesPattern << 3 << 2 << 1
            << QString() << 1 << 2 << 3
            << QStringList(leadingSpacesMessage) << QStringList()
            << Tasks();
    }

    void test()
    {
        QFETCH(QString, input);
        QFETCH(QString, workDir);
        QFETCH(OutputParserTester::Channel, inputChannel);
        QFETCH(CustomParserExpression::CustomParserChannel, filterErrorChannel);
        QFETCH(CustomParserExpression::CustomParserChannel, filterWarningChannel);
        QFETCH(QString, errorPattern);
        QFETCH(int,     errorFileNameCap);
        QFETCH(int,     errorLineNumberCap);
        QFETCH(int,     errorMessageCap);
        QFETCH(QString, warningPattern);
        QFETCH(int,     warningFileNameCap);
        QFETCH(int,     warningLineNumberCap);
        QFETCH(int,     warningMessageCap);
        QFETCH(QStringList, childStdOutLines);
        QFETCH(QStringList, childStdErrLines);
        QFETCH(Tasks, tasks);

        CustomParserSettings settings;
        settings.error.setPattern(errorPattern);
        settings.error.setFileNameCap(errorFileNameCap);
        settings.error.setLineNumberCap(errorLineNumberCap);
        settings.error.setMessageCap(errorMessageCap);
        settings.error.setChannel(filterErrorChannel);
        settings.warning.setPattern(warningPattern);
        settings.warning.setFileNameCap(warningFileNameCap);
        settings.warning.setLineNumberCap(warningLineNumberCap);
        settings.warning.setMessageCap(warningMessageCap);
        settings.warning.setChannel(filterWarningChannel);

        CustomParser *parser = new CustomParser;
        parser->setSettings(settings);
        parser->addSearchDir(FilePath::fromString(workDir));
        parser->skipFileExistsCheck();

        OutputParserTester testbench;
        testbench.addLineParser(parser);
        testbench.testParsing(input, inputChannel, tasks, childStdOutLines, childStdErrLines);
    }
};

QObject *createCustomParserTest()
{
    return new CustomParserTest;
}

#endif // WITH_TESTS

} // namespace Internal
} // namespace ProjectExplorer

#include <customparser.moc>
