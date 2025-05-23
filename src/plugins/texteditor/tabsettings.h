// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "texteditorsupport_global.h"

#include <utils/filepath.h>
#include <utils/qtcsettings.h>
#include <utils/store.h>

#include <QTextBlock>

#include <functional>

namespace TextEditor {

// Tab settings: Data type the GeneralSettingsPage acts on
// with some convenience functions for formatting.
class TEXTEDITORSUPPORT_EXPORT TabSettings
{
public:
    enum TabPolicy {
        SpacesOnlyTabPolicy = 0,
        TabsOnlyTabPolicy
    };

    // This enum must match the indexes of continuationAlignBehavior widget
    enum ContinuationAlignBehavior {
        NoContinuationAlign = 0,
        ContinuationAlignWithSpaces = 1,
        ContinuationAlignWithIndent = 2
    };

    TabSettings() = default;
    TabSettings(TabPolicy tabPolicy, int tabSize,
                int indentSize, ContinuationAlignBehavior continuationAlignBehavior);

    Utils::Store toMap() const;
    void fromMap(const Utils::Store &map);

    TabSettings autoDetect(const QTextDocument *document) const;

    int lineIndentPosition(const QString &text) const;
    int columnAt(const QString &text, int position) const;
    int columnAtCursorPosition(const QTextCursor &cursor) const;
    int positionAtColumn(const QString &text, int column, int *offset = nullptr, bool allowOverstep = false) const;
    int columnCountForText(const QString &text, int startColumn = 0) const;
    int indentedColumn(int column, bool doIndent = true) const;
    QString indentationString(int startColumn, int targetColumn, int padding) const;
    int indentationColumn(const QString &text) const;
    static int maximumPadding(const QString &text);

    void indentLine(const QTextBlock &block, int newIndent, int padding = 0) const;
    void reindentLine(QTextBlock block, int delta) const;

    bool isIndentationClean(const QTextBlock &block, const int indent) const;

    friend bool operator==(const TabSettings &t1, const TabSettings &t2) { return t1.equals(t2); }
    friend bool operator!=(const TabSettings &t1, const TabSettings &t2) { return !t1.equals(t2); }

    static int firstNonSpace(const QString &text);
    static QString indentationString(const QString &text);
    static inline bool onlySpace(const QString &text) { return firstNonSpace(text) == text.length(); }
    static int spacesLeftFromPosition(const QString &text, int position);
    static bool cursorIsAtBeginningOfLine(const QTextCursor &cursor);
    static int trailingWhitespaces(const QString &text);
    static void removeTrailingWhitespace(const QTextBlock &block);

    bool m_autoDetect = true;
    TabPolicy m_tabPolicy = SpacesOnlyTabPolicy;
    int m_tabSize = 8;
    int m_indentSize = 4;
    ContinuationAlignBehavior m_continuationAlignBehavior = ContinuationAlignWithSpaces;

    bool equals(const TabSettings &ts) const;

    using Retriever = std::function<TabSettings(const Utils::FilePath &)>;
    static void setRetriever(const Retriever &retriever);
    static TabSettings settingsForFile(const Utils::FilePath &filePath);
};

} // namespace TextEditor

Q_DECLARE_METATYPE(TextEditor::TabSettings)
Q_DECLARE_METATYPE(TextEditor::TabSettings::TabPolicy)
Q_DECLARE_METATYPE(TextEditor::TabSettings::ContinuationAlignBehavior)
