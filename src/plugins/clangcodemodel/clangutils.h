// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "cppeditor/compilationdb.h"
#include <cplusplus/Icons.h>

#include <cppeditor/projectinfo.h>
#include <cppeditor/compileroptionsbuilder.h>

#include <utils/link.h>

#include <QPair>
#include <QTextCursor>

QT_BEGIN_NAMESPACE
class QTextBlock;
QT_END_NAMESPACE

namespace CppEditor {
class ClangDiagnosticConfig;
class CppEditorDocumentHandle;
}

namespace ProjectExplorer { class Project; }

namespace ClangCodeModel {
namespace Internal {

CppEditor::ClangDiagnosticConfig warningsConfigForProject(ProjectExplorer::Project *project);
const QStringList globalClangOptions();

CppEditor::CompilerOptionsBuilder clangOptionsBuilder(
        const CppEditor::ProjectPart &projectPart,
        const CppEditor::ClangDiagnosticConfig &warningsConfig,
        const Utils::FilePath &clangIncludeDir,
        const ProjectExplorer::Macros &extraMacros);

CppEditor::ProjectPart::ConstPtr projectPartForFile(const Utils::FilePath &filePath);

Utils::FilePath currentCppEditorDocumentFilePath();

QString diagnosticCategoryPrefixRemoved(const QString &text);

void generateCompilationDB(
    QPromise<CppEditor::GenerateCompilationDbResult> &promise,
    const QList<CppEditor::ProjectInfo::ConstPtr> &projectInfoList,
    const Utils::FilePath &baseDir,
    CppEditor::CompilationDbPurpose purpose,
    const CppEditor::ClangDiagnosticConfig &warningsConfig,
    const QStringList &projectOptions,
    const Utils::FilePath &clangIncludeDir);

class DiagnosticTextInfo
{
public:
    DiagnosticTextInfo(const QString &text);

    QString textWithoutOption() const;
    QString option() const;
    QString category() const;

    static bool isClazyOption(const QString &option);
    static QString clazyCheckName(const QString &option);

private:
    int getSquareBracketStartIndex() const;

    const QString m_text;
    const int m_squareBracketStartIndex = getSquareBracketStartIndex();
};

class ClangSourceRange
{
public:
    ClangSourceRange(const Utils::Link &start, const Utils::Link &end) : start(start), end(end) {}

    bool contains(int line, int column) const
    {
        if (line < start.target.line || line > end.target.line)
            return false;
        if (line == start.target.line && column < start.target.line)
            return false;
        if (line == end.target.line && column > end.target.column)
            return false;
        return true;
    }

    bool contains(const Utils::Link &sourceLocation) const
    {
        return contains(sourceLocation.target.line, sourceLocation.target.column);
    }

    Utils::Link start;
    Utils::Link end;
};

inline bool operator==(const ClangSourceRange &first, const ClangSourceRange &second)
{
    return first.start == second.start && first.end == second.end;
}

class ClangFixIt
{
public:
    ClangFixIt(const QString &text, const ClangSourceRange &range) : range(range), text(text) {}

    ClangSourceRange range;
    QString text;
};

inline bool operator==(const ClangFixIt &first, const ClangFixIt &second)
{
    return first.text == second.text && first.range == second.range;
}

class ClangDiagnostic
{
public:
    enum class Severity { Ignored, Note, Warning, Error, Fatal };

    Utils::Link location;
    QString text;
    QString category;
    QString enableOption;
    QString disableOption;
    QList<ClangDiagnostic> children;
    QList<ClangFixIt> fixIts;
    Severity severity = Severity::Ignored;
};

inline bool operator==(const ClangDiagnostic &first, const ClangDiagnostic &second)
{
    return first.text == second.text && first.location == second.location;
}
inline bool operator!=(const ClangDiagnostic &first, const ClangDiagnostic &second)
{
    return !(first == second);
}

} // namespace Internal
} // namespace Clang
