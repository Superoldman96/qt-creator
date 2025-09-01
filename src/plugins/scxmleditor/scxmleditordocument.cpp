// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "scxmleditordocument.h"
#include "mainwidget.h"
#include "scxmleditorconstants.h"

#include <qtsupport/qtkitaspect.h>

#include <utils/fileutils.h>
#include <utils/mimeconstants.h>
#include <utils/qtcassert.h>

#include <QFileInfo>
#include <QGuiApplication>
#include <QTextDocument>

using namespace Utils;

using namespace ScxmlEditor::Common;

namespace ScxmlEditor::Internal {

ScxmlEditorDocument::ScxmlEditorDocument(MainWidget *designWidget, QObject *parent)
    : m_designWidget(designWidget)
{
    setMimeType(Utils::Constants::SCXML_MIMETYPE);
    setParent(parent);
    setId(Utils::Id(ScxmlEditor::Constants::K_SCXML_EDITOR_ID));

    // Designer needs UTF-8 regardless of settings.
    setEncoding(TextEncoding::Utf8);
    connect(m_designWidget.data(), &Common::MainWidget::dirtyChanged, this, [this]{
        emit changed();
    });
}

Result<> ScxmlEditorDocument::open(const FilePath &filePath, const FilePath &realFilePath)
{
    Q_UNUSED(realFilePath)

    if (filePath.isEmpty())
        return ResultError("File path is empty"); // FIXME: Use something better

    if (!m_designWidget)
        return ResultError(ResultAssert);

    const FilePath &absoluteFilePath = filePath.absoluteFilePath();
    if (!m_designWidget->load(absoluteFilePath))
        return ResultError(m_designWidget->errorMessage());

    setFilePath(absoluteFilePath);

    return ResultOk;
}

Result<> ScxmlEditorDocument::saveImpl(const FilePath &filePath, SaveOption option)
{
    if (filePath.isEmpty())
        return ResultError("ASSERT: ScxmlEditorDocument: filePath.isEmpty()");

    bool dirty = m_designWidget->isDirty();

    m_designWidget->setFilePath(filePath);
    if (!m_designWidget->save()) {
        m_designWidget->setFilePath(this->filePath());
        return ResultError(m_designWidget->errorMessage());
    }

    if (option == SaveOption::AutoSave) {
        m_designWidget->setFilePath(this->filePath());
        m_designWidget->save();
        return ResultOk;
    }

    setFilePath(filePath);

    if (dirty != m_designWidget->isDirty())
        emit changed();

    return ResultOk;
}

void ScxmlEditorDocument::setFilePath(const FilePath &filePath)
{
    m_designWidget->setFilePath(filePath);
    IDocument::setFilePath(filePath);
}

bool ScxmlEditorDocument::shouldAutoSave() const
{
    return false;
}

bool ScxmlEditorDocument::isSaveAsAllowed() const
{
    return true;
}

MainWidget *ScxmlEditorDocument::designWidget() const
{
    return m_designWidget;
}

bool ScxmlEditorDocument::isModified() const
{
    return m_designWidget && m_designWidget->isDirty();
}

Result<> ScxmlEditorDocument::reload(ReloadFlag flag, ChangeType type)
{
    Q_UNUSED(type)
    if (flag == FlagIgnore)
        return ResultOk;
    emit aboutToReload();
    QString errorString;
    emit reloadRequested(&errorString, filePath());
    const bool success = errorString.isEmpty();
    emit reloadFinished(success);
    return makeResult(success, errorString);
}

bool ScxmlEditorDocument::supportsEncoding(const TextEncoding &encoding) const
{
    return encoding.isUtf8();
}

QString ScxmlEditorDocument::designWidgetContents() const
{
    return m_designWidget->contents();
}

void ScxmlEditorDocument::syncXmlFromDesignWidget()
{
    document()->setPlainText(designWidgetContents());
}

} // namespace ScxmlEditor::Internal
