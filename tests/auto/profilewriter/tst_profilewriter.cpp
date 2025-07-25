// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include <qmakevfs.h>
#include <qmakeparser.h>
#include <prowriter.h>

#include <utils/filepath.h>

#include <QTest>

using namespace Utils;

const FilePath BASE_DIR("/some/stuff");

///////////// callbacks for parser/evaluator

static void print(const QString &fileName, int lineNo, const QString &msg)
{
    if (lineNo > 0)
        qWarning("%s(%d): %s", qPrintable(fileName), lineNo, qPrintable(msg));
    else if (lineNo)
        qWarning("%s: %s", qPrintable(fileName), qPrintable(msg));
    else
        qWarning("%s", qPrintable(msg));
}

class ParseHandler : public QMakeParserHandler {
public:
    void message(int /* type */, const QString &msg, const QString &fileName, int lineNo) override
        { print(fileName, lineNo, msg); }
};

static ParseHandler parseHandler;

//////////////// the actual autotest

typedef QmakeProjectManager::Internal::ProWriter PW;

class tst_ProFileWriter : public QObject
{
    Q_OBJECT

private slots:
    void adds_data();
    void adds();
    void removes_data();
    void removes();
    void multiVar();
    void addFiles();
    void removeFiles();
};

static QStringList strList(const char * const *array)
{
    QStringList values;
    for (const char * const *value = array; *value; value++)
        values << QString::fromLatin1(*value);
    return values;
}

void tst_ProFileWriter::adds_data()
{
    QTest::addColumn<int>("flags");
    QTest::addColumn<QStringList>("values");
    QTest::addColumn<QString>("scope");
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("output");

    struct Case {
        int flags;
        const char *title;
        const char * const *values;
        const char *scope;
        const char *input;
        const char *output;
    };

    static const char *f_foo[] = {"foo", 0};
    static const char *f_foo_bar[] = {"foo", "bar", 0};
    static const Case cases[] = {
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "add new append multi", f_foo, 0,
            "",
            "SOURCES += \\\n"
            "\tfoo"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "add new append multi after comment", f_foo, 0,
            "# test file",
            "# test file\n"
            "\n"
            "SOURCES += \\\n"
            "\tfoo"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "add new append multi before newlines", f_foo, 0,
            "\n"
            "\n"
            "\n",
            "SOURCES += \\\n"
            "\tfoo\n"
            "\n"
            "\n"
            "\n"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "add new append multi after comment before newlines", f_foo, 0,
            "# test file\n"
            "\n"
            "\n"
            "\n",
            "# test file\n"
            "\n"
            "SOURCES += \\\n"
            "\tfoo\n"
            "\n"
            "\n"
            "\n"
        },
        {
            PW::AppendValues|PW::AssignOperator|PW::MultiLine,
            "add new assign multi", f_foo, 0,
            "# test file",
            "# test file\n"
            "\n"
            "SOURCES = \\\n"
            "\tfoo"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::OneLine,
            "add new append oneline", f_foo, 0,
            "# test file",
            "# test file\n"
            "\n"
            "SOURCES += foo"
        },
        {
            PW::AppendValues|PW::AssignOperator|PW::OneLine,
            "add new assign oneline", f_foo, 0,
            "# test file",
            "# test file\n"
            "\n"
            "SOURCES = foo"
        },
        {
            PW::AppendValues|PW::AssignOperator|PW::OneLine,
            "add new assign oneline after existing", f_foo, 0,
            "# test file\n"
            "\n"
            "HEADERS = foo",
            "# test file\n"
            "\n"
            "HEADERS = foo\n"
            "\n"
            "SOURCES = foo"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "add new ignoring scoped", f_foo, 0,
            "unix:SOURCES = some files",
            "unix:SOURCES = some files\n"
            "\n"
            "SOURCES += \\\n"
            "\tfoo"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "add new after some scope", f_foo, 0,
            "unix {\n"
            "\tSOMEVAR = foo\n"
            "}",
            "unix {\n"
            "\tSOMEVAR = foo\n"
            "}\n"
            "\n"
            "SOURCES += \\\n"
            "\tfoo"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "add to existing (wrong operator)", f_foo, 0,
            "SOURCES = some files",
            "SOURCES = some files \\\n"
            "\tfoo"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "insert at end", f_foo_bar, 0,
            "SOURCES = some files",
            "SOURCES = some files \\\n"
            "\tbar \\\n"
            "\tfoo"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "insert into empty", f_foo_bar, 0,
            "SOURCES =",
            "SOURCES = \\\n"
            "\tbar \\\n"
            "\tfoo"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "insert into middle", f_foo_bar, 0,
            "SOURCES = some files \\\n"
            "    aargh \\\n"
            "    zoo",
            "SOURCES = some files \\\n"
            "    aargh \\\n"
            "    bar \\\n"
            "    foo \\\n"
            "    zoo"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "add to existing after comment (wrong operator)", f_foo, 0,
            "SOURCES = some files   # comment",
            "SOURCES = some files \\   # comment\n"
            "\tfoo"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "add to existing after comment line (wrong operator)", f_foo, 0,
            "SOURCES = some \\\n"
            "   # comment\n"
            "\tfiles",
            "SOURCES = some \\\n"
            "   # comment\n"
            "\tfiles \\\n"
            "\tfoo"
        },
        {
            PW::AppendValues|PW::AssignOperator|PW::MultiLine,
            "add to existing", f_foo, 0,
            "SOURCES = some files",
            "SOURCES = some files \\\n"
            "\tfoo"
        },
        {
            PW::ReplaceValues|PW::AssignOperator|PW::MultiLine,
            "replace existing multi", f_foo_bar, 0,
            "SOURCES = some files",
            "SOURCES = \\\n"
            "\tfoo \\\n"
            "\tbar"
        },
        {
            PW::ReplaceValues|PW::AssignOperator|PW::OneLine,
            "replace existing oneline", f_foo_bar, 0,
            "SOURCES = some files",
            "SOURCES = foo bar"
        },
        {
            PW::ReplaceValues|PW::AssignOperator|PW::OneLine,
            "replace existing complex last", f_foo_bar, 0,
            "SOURCES = some \\\n"
            "   # comment\n"
            "\tfiles",
            "SOURCES = foo bar"
        },
        {
            PW::ReplaceValues|PW::AssignOperator|PW::OneLine,
            "replace existing complex middle 1", f_foo_bar, 0,
            "SOURCES = some \\\n"
            "   # comment\n"
            "\tfiles\n"
            "HEADERS = blubb",
            "SOURCES = foo bar\n"
            "HEADERS = blubb"
        },
        {
            PW::ReplaceValues|PW::AssignOperator|PW::OneLine,
            "replace existing complex middle 2", f_foo_bar, 0,
            "SOURCES = some \\\n"
            "   # comment\n"
            "\tfiles\n"
            "\n"
            "HEADERS = blubb",
            "SOURCES = foo bar\n"
            "\n"
            "HEADERS = blubb"
        },
        {
            PW::ReplaceValues|PW::AssignOperator|PW::OneLine,
            "replace existing complex middle 3", f_foo_bar, 0,
            "SOURCES = some \\\n"
            "   # comment\n"
            "\tfiles \\\n"
            "\n"
            "HEADERS = blubb",
            "SOURCES = foo bar\n"
            "\n"
            "HEADERS = blubb"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::OneLine,
            "scoped new / new scope", f_foo, "dog",
            "# test file\n"
            "SOURCES = yo",
            "# test file\n"
            "SOURCES = yo\n"
            "\n"
            "dog {\n"
            "\tSOURCES += foo\n"
            "}"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::OneLine,
            "scoped new / extend scope", f_foo, "dog",
            "# test file\n"
            "dog {\n"
            "\tHEADERS += yo\n"
            "}",
            "# test file\n"
            "dog {\n"
            "\tHEADERS += yo\n"
            "\n"
            "\tSOURCES += foo\n"
            "}"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::OneLine,
            "scoped new / extend elongated scope", f_foo, "dog",
            "# test file\n"
            "dog {\n"
            "    HEADERS += \\\n"
            "        yo \\\n"
            "        blubb\n"
            "}",
            "# test file\n"
            "dog {\n"
            "    HEADERS += \\\n"
            "        yo \\\n"
            "        blubb\n"
            "\n"
            "\tSOURCES += foo\n"
            "}"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::OneLine,
            "scoped new / extend empty scope", f_foo, "dog",
            "# test file\n"
            "dog {\n"
            "}",
            "# test file\n"
            "dog {\n"
            "\tSOURCES += foo\n"
            "}"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::OneLine,
            "scoped new / extend oneline scope", f_foo, "dog",
            "# test file\n"
            "dog:HEADERS += yo",
            "# test file\n"
            "dog {\n"
            "\tHEADERS += yo\n"
            "\n"
            "\tSOURCES += foo\n"
            "}"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::OneLine,
            "scoped new / extend oneline scope with multiline body", f_foo, "dog",
            "# test file\n"
            "dog:HEADERS += yo \\\n"
            "        you\n"
            "\n"
            "blubb()",
            "# test file\n"
            "dog {\n"
            "\tHEADERS += yo \\\n"
            "        you\n"
            "\n"
            "\tSOURCES += foo\n"
            "}\n"
            "\n"
            "blubb()"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "add new after some scope inside scope", f_foo, "dog",
            "dog {\n"
            "    unix {\n"
            "        SOMEVAR = foo\n"
            "    }\n"
            "}",
            "dog {\n"
            "    unix {\n"
            "        SOMEVAR = foo\n"
            "    }\n"
            "\n"
            "\tSOURCES += \\\n"
            "\t\tfoo\n"
            "}"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::OneLine,
            "scoped new / pseudo-oneline-scope", f_foo, "dog",
            "# test file\n"
            "dog: {\n"
            "}",
            "# test file\n"
            "dog: {\n"
            "\tSOURCES += foo\n"
            "}"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "scoped append", f_foo, "dog",
            "# test file\n"
            "dog:SOURCES = yo",
            "# test file\n"
            "dog:SOURCES = yo \\\n"
            "\t\tfoo"
        },
        {
            PW::AppendValues|PW::AppendOperator|PW::MultiLine,
            "complex scoped append", f_foo, "dog",
            "# test file\n"
            "animal:!dog:SOURCES = yo",
            "# test file\n"
            "animal:!dog:SOURCES = yo\n"
            "\n"
            "dog {\n"
            "\tSOURCES += \\\n"
            "\t\tfoo\n"
            "}"
        },
    };

    for (uint i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const Case *_case = &cases[i];
        QTest::newRow(_case->title)
                << _case->flags
                << strList(_case->values)
                << QString::fromLatin1(_case->scope)
                << QString::fromLatin1(_case->input)
                << QString::fromLatin1(_case->output);
    }
}

void tst_ProFileWriter::adds()
{
    QFETCH(int, flags);
    QFETCH(QStringList, values);
    QFETCH(QString, scope);
    QFETCH(QString, input);
    QFETCH(QString, output);

    QStringList lines = input.isEmpty() ? QStringList() : input.split(QLatin1String("\n"));
    QString var = QLatin1String("SOURCES");

    QMakeVfs vfs;
    QMakeParser parser(0, &vfs, &parseHandler);
    ProFile *proFile = parser.parsedProBlock(QString(), // device
                                             QStringView(input),
                                             0,
                                             BASE_DIR.pathAppended("test.pro").path(),
                                             1);
    QVERIFY(proFile);
    PW::putVarValues(proFile, &lines, values, var, PW::PutFlags(flags), scope, "\t");
    proFile->deref();

    QCOMPARE(lines.join(QLatin1Char('\n')), output);
}

void tst_ProFileWriter::removes_data()
{
    QTest::addColumn<QStringList>("values");
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("output");

    struct Case {
        const char *title;
        const char * const *values;
        const char *input;
        const char *output;
    };

    static const char *f_foo[] = {"foo", 0};
    static const char *f_foo_bar[] = {"foo", "bar", 0};
    static const Case cases[] = {
        {
            "remove fail", f_foo,
            "SOURCES = bak bar",
            "SOURCES = bak bar"
        },
        {
            "remove one-line middle", f_foo,
            "SOURCES = bak foo bar",
            "SOURCES = bak bar"
        },
        {
            "remove one-line trailing", f_foo,
            "SOURCES = bak bar foo",
            "SOURCES = bak bar"
        },
        {
            "remove multi-line single leading", f_foo,
            "SOURCES = foo \\\n"
            "    bak \\\n"
            "    bar",
            "SOURCES = \\\n"
            "    bak \\\n"
            "    bar"
        },
        {
            "remove multi-line single middle", f_foo,
            "SOURCES = bak \\\n"
            "    foo \\\n"
            "    bar",
            "SOURCES = bak \\\n"
            "    bar"
        },
        {
            "remove multi-line single trailing", f_foo,
            "SOURCES = bak \\\n"
            "    bar \\\n"
            "    foo",
            "SOURCES = bak \\\n"
            "    bar"
        },
        {
            "remove multi-line single leading with comment", f_foo,
            "SOURCES = foo \\  # comment\n"
            "    bak \\\n"
            "    bar",
            "SOURCES = \\  # foo # comment\n"
            "    bak \\\n"
            "    bar"
        },
        {
            "remove multi-line single middle with comment", f_foo,
            "SOURCES = bak \\\n"
            "    foo \\  # comment\n"
            "    bar",
            "SOURCES = bak \\\n"
            "    \\  # foo # comment\n"
            "    bar"
        },
        {
            "remove multi-line single trailing with comment", f_foo,
            "SOURCES = bak \\\n"
            "    bar \\\n"
            "    foo  # comment",
            "SOURCES = bak \\\n"
            "    bar\n"
            "     # foo # comment"
        },
        {
            "remove multi-line single trailing after empty line", f_foo,
            "SOURCES = bak \\\n"
            "    bar \\\n"
            "    \\\n"
            "    foo",
            "SOURCES = bak \\\n"
            "    bar\n"
        },
        {
            "remove multi-line single trailing after comment line", f_foo,
            "SOURCES = bak \\\n"
            "    bar \\\n"
            "       # just a comment\n"
            "    foo",
            "SOURCES = bak \\\n"
            "    bar\n"
            "       # just a comment"
        },
        {
            "remove multi-line single trailing after empty line with comment", f_foo,
            "SOURCES = bak \\\n"
            "    bar \\\n"
            "    \\ # just a comment\n"
            "    foo",
            "SOURCES = bak \\\n"
            "    bar\n"
            "     # just a comment"
        },
        {
            "remove multiple one-line middle", f_foo_bar,
            "SOURCES = bak foo bar baz",
            "SOURCES = bak baz"
        },
        {
            "remove multiple one-line trailing", f_foo_bar,
            "SOURCES = bak baz foo bar",
            "SOURCES = bak baz"
        },
        {
            "remove multiple one-line interleaved", f_foo_bar,
            "SOURCES = bak foo baz bar",
            "SOURCES = bak baz"
        },
        {
            "remove multiple one-line middle with comment", f_foo_bar,
            "SOURCES = bak foo bar baz   # comment",
            "SOURCES = bak baz   # bar # foo # comment"
        },
        {
            "remove multi-line multiple trailing with empty line with comment", f_foo_bar,
            "SOURCES = bak \\\n"
            "    bar \\\n"
            "    \\ # just a comment\n"
            "    foo",
            "SOURCES = bak\n"
            "     # just a comment"
        },
    };

    for (uint i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const Case *_case = &cases[i];
        QTest::newRow(_case->title)
                << strList(_case->values)
                << QString::fromLatin1(_case->input)
                << QString::fromLatin1(_case->output);
    }
}

void tst_ProFileWriter::removes()
{
    QFETCH(QStringList, values);
    QFETCH(QString, input);
    QFETCH(QString, output);

    QStringList lines = input.split(QLatin1String("\n"));
    QStringList vars; vars << QLatin1String("SOURCES");

    QMakeVfs vfs;
    QMakeParser parser(0, &vfs, &parseHandler);
    ProFile *proFile = parser.parsedProBlock(QString(), // device
                                             QStringView(input),
                                             0,
                                             BASE_DIR.pathAppended("test.pro").path(),
                                             1);
    QVERIFY(proFile);
    QmakeProjectManager::Internal::ProWriter::removeVarValues(proFile, &lines, values, vars);
    proFile->deref();

    QCOMPARE(lines.join(QLatin1Char('\n')), output);
}

void tst_ProFileWriter::multiVar()
{
    QString input = QLatin1String(
            "SOURCES = foo bar\n"
            "# comment line\n"
            "HEADERS = baz bak"
            );
    QStringList lines = input.split(QLatin1String("\n"));
    QString output = QLatin1String(
            "SOURCES = bar\n"
            "# comment line\n"
            "HEADERS = baz"
            );
    FilePaths files = { BASE_DIR / "foo", BASE_DIR / "bak" };
    QStringList vars; vars << QLatin1String("SOURCES") << QLatin1String("HEADERS");

    QMakeVfs vfs;
    QMakeParser parser(0, &vfs, &parseHandler);
    ProFile *proFile = parser.parsedProBlock(QString(), // device
                                             QStringView(input),
                                             0,
                                             BASE_DIR.pathAppended("test.pro").path(),
                                             1);
    QVERIFY(proFile);
    QmakeProjectManager::Internal::ProWriter::removeFiles(proFile, &lines, BASE_DIR, files, vars);
    proFile->deref();

    QCOMPARE(lines.join(QLatin1Char('\n')), output);
}

void tst_ProFileWriter::addFiles()
{
    const QStringList equivalentInputs = {"SOURCES = foo.cpp", "SOURCES = foo.cpp \\"};
    for (const QString &input : equivalentInputs) {
        QStringList lines = input.split(QLatin1Char('\n'));
        QString output = QLatin1String(
                    "SOURCES = foo.cpp \\\n"
                    "\tsub/bar.cpp"
                    );

        QMakeVfs vfs;
        QMakeParser parser(0, &vfs, &parseHandler);
        ProFile *proFile = parser.parsedProBlock(QString(), // device
                                                 QStringView(input),
                                                 0,
                                                 BASE_DIR.pathAppended("test.pro").path(),
                                                 1);
        QVERIFY(proFile);
        QmakeProjectManager::Internal::ProWriter::addFiles(proFile, &lines,
                                                           QStringList(BASE_DIR.pathAppended("sub/bar.cpp").path()),
                                                           QLatin1String("SOURCES"), "\t");
        proFile->deref();

        QCOMPARE(lines.join(QLatin1Char('\n')), output);
    }
}

void tst_ProFileWriter::removeFiles()
{
    QString input = QLatin1String(
            "SOURCES = foo.cpp sub/bar.cpp"
            );
    QStringList lines = input.split(QLatin1Char('\n'));
    QString output = QLatin1String(
            "SOURCES = foo.cpp"
            );

    QMakeVfs vfs;
    QMakeParser parser(0, &vfs, &parseHandler);
    ProFile *proFile = parser.parsedProBlock(QString(), // device
                                             QStringView(input),
                                             0,
                                             BASE_DIR.pathAppended("test.pro").path(),
                                             1);
    QVERIFY(proFile);
    QmakeProjectManager::Internal::ProWriter::removeFiles(proFile, &lines, BASE_DIR,
            {BASE_DIR / "sub/bar.cpp"},
            QStringList() << QLatin1String("SOURCES") << QLatin1String("HEADERS"));
    proFile->deref();

    QCOMPARE(lines.join(QLatin1Char('\n')), output);
}

QTEST_GUILESS_MAIN(tst_ProFileWriter)
#include "tst_profilewriter.moc"
