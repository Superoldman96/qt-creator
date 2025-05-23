// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qmljssemantichighlighter.h"

#include "qmljseditordocument.h"

#include <qmljs/qmljsdocument.h>
#include <qmljs/qmljsscopechain.h>
#include <qmljs/qmljsscopebuilder.h>
#include <qmljs/qmljsevaluate.h>
#include <qmljs/qmljscontext.h>
#include <qmljs/qmljsbind.h>
#include <qmljs/qmljsutils.h>
#include <qmljs/parser/qmljsast_p.h>
#include <qmljs/parser/qmljsastvisitor_p.h>
#include <qmljs/qmljsstaticanalysismessage.h>
#include <texteditor/syntaxhighlighter.h>
#include <texteditor/textdocument.h>
#include <texteditor/texteditorconstants.h>
#include <texteditor/texteditorsettings.h>
#include <texteditor/fontsettings.h>
#include <utils/async.h>
#include <utils/algorithm.h>
#include <utils/qtcassert.h>

#include <QDebug>
#include <QTextDocument>
#include <QThreadPool>

using namespace QmlJS;
using namespace QmlJS::AST;

namespace QmlJSEditor {

namespace {

static bool isIdScope(const ObjectValue *scope, const QList<const QmlComponentChain *> &chain)
{
    for (const QmlComponentChain *c : chain) {
        if (c->idScope() == scope)
            return true;
        if (isIdScope(scope, c->instantiatingComponents()))
            return true;
    }
    return false;
}

class CollectStateNames : protected Visitor
{
    QStringList m_stateNames;
    bool m_inStateType = false;
    ScopeChain m_scopeChain;
    const CppComponentValue *m_statePrototype;

public:
    CollectStateNames(const ScopeChain &scopeChain)
        : m_scopeChain(scopeChain)
    {
        m_statePrototype = scopeChain.context()->valueOwner()->cppQmlTypes().objectByCppName(QLatin1String("QDeclarativeState"));
    }

    QStringList operator()(Node *ast)
    {
        m_stateNames.clear();
        if (!m_statePrototype)
            return m_stateNames;
        m_inStateType = false;
        accept(ast);
        return m_stateNames;
    }

protected:
    void accept(Node *ast)
    {
        if (ast)
            ast->accept(this);
    }

    bool preVisit(Node *ast) override
    {
        return ast->uiObjectMemberCast()
                || cast<UiProgram *>(ast)
                || cast<UiObjectInitializer *>(ast)
                || cast<UiObjectMemberList *>(ast)
                || cast<UiArrayMemberList *>(ast);
    }

    bool hasStatePrototype(Node *ast)
    {
        Bind *bind = m_scopeChain.document()->bind();
        const ObjectValue *v = bind->findQmlObject(ast);
        if (!v)
            return false;
        PrototypeIterator it(v, m_scopeChain.context());
        while (it.hasNext()) {
            const ObjectValue *proto = it.next();
            const CppComponentValue *qmlProto = value_cast<CppComponentValue>(proto);
            if (!qmlProto)
                continue;
            if (qmlProto->metaObject() == m_statePrototype->metaObject())
                return true;
        }
        return false;
    }

    bool visit(UiObjectDefinition *ast) override
    {
        const bool old = m_inStateType;
        m_inStateType = hasStatePrototype(ast);
        accept(ast->initializer);
        m_inStateType = old;
        return false;
    }

    bool visit(UiObjectBinding *ast) override
    {
        const bool old = m_inStateType;
        m_inStateType = hasStatePrototype(ast);
        accept(ast->initializer);
        m_inStateType = old;
        return false;
    }

    bool visit(UiScriptBinding *ast) override
    {
        if (!m_inStateType)
            return false;
        if (!ast->qualifiedId || ast->qualifiedId->name.isEmpty() || ast->qualifiedId->next)
            return false;
        if (ast->qualifiedId->name != QLatin1String("name"))
            return false;

        auto expStmt = cast<const ExpressionStatement *>(ast->statement);
        if (!expStmt)
            return false;
        auto strLit = cast<const StringLiteral *>(expStmt->expression);
        if (!strLit || strLit->value.isEmpty())
            return false;

        m_stateNames += strLit->value.toString();

        return false;
    }

    void throwRecursionDepthError() override
    {
        qWarning("Warning: Hit maximum recursion depth while visitin AST in CollectStateNames");
    }
};

class CollectionTask : protected Visitor
{
public:
    enum Flags {
        AddMessagesHighlights,
        SkipMessagesHighlights,
    };
    CollectionTask(QPromise<SemanticHighlighter::Use> &promise,
                   const QmlJSTools::SemanticInfo &semanticInfo,
                   const TextEditor::FontSettings &fontSettings,
                   Flags flags)
        : m_promise(promise)
        , m_semanticInfo(semanticInfo)
        , m_fontSettings(fontSettings)
        , m_scopeChain(semanticInfo.scopeChain())
        , m_scopeBuilder(&m_scopeChain)
        , m_lineOfLastUse(0)
        , m_nextExtraFormat(SemanticHighlighter::Max)
        , m_currentDelayedUse(0)
    {
        int nMessages = 0;
        if (m_scopeChain.document()->language().isFullySupportedLanguage()) {
            nMessages = m_scopeChain.document()->diagnosticMessages().size()
                    + m_semanticInfo.semanticMessages.size()
                    + m_semanticInfo.staticAnalysisMessages.size();
            m_delayedUses.reserve(nMessages);
            m_diagnosticRanges.reserve(nMessages);
            m_extraFormats.reserve(nMessages);
            if (flags == AddMessagesHighlights) {
                addMessages(m_scopeChain.document()->diagnosticMessages(), m_scopeChain.document());
                addMessages(m_semanticInfo.semanticMessages, m_semanticInfo.document);
                addMessages(m_semanticInfo.staticAnalysisMessages, m_semanticInfo.document);
            }

            Utils::sort(m_delayedUses, sortByLinePredicate);
        }
        m_currentDelayedUse = 0;
    }

    QVector<QTextLayout::FormatRange> diagnosticRanges()
    {
        return m_diagnosticRanges;
    }

    QHash<int, QTextCharFormat> extraFormats()
    {
        return m_extraFormats;
    }

    void run()
    {
        Node *root = m_scopeChain.document()->ast();
        m_stateNames = CollectStateNames(m_scopeChain)(root);
        accept(root);
        while (m_currentDelayedUse < m_delayedUses.size())
            m_uses.append(m_delayedUses.value(m_currentDelayedUse++));
        flush();
    }

protected:
    void accept(Node *ast)
    {
        if (m_promise.isCanceled())
            return;
        if (ast)
            ast->accept(this);
    }

    void scopedAccept(Node *ast, Node *child)
    {
        if (m_promise.isCanceled())
            return;
        m_scopeBuilder.push(ast);
        accept(child);
        m_scopeBuilder.pop();
    }

    void processName(QStringView name, SourceLocation location)
    {
        if (name.isEmpty())
            return;

        const QString &nameStr = name.toString();
        const ObjectValue *scope = nullptr;
        const Value *value = m_scopeChain.lookup(nameStr, &scope);
        if (!value || !scope)
            return;

        SemanticHighlighter::UseType type = SemanticHighlighter::UnknownType;
        if (m_scopeChain.qmlTypes() == scope) {
            type = SemanticHighlighter::QmlTypeType;
        } else if (m_scopeChain.qmlScopeObjects().contains(scope)) {
            type = SemanticHighlighter::ScopeObjectPropertyType;
        } else if (m_scopeChain.jsScopes().contains(scope)) {
            type = SemanticHighlighter::JsScopeType;
        } else if (m_scopeChain.jsImports() == scope) {
            type = SemanticHighlighter::JsImportType;
        } else if (m_scopeChain.globalScope() == scope) {
            type = SemanticHighlighter::JsGlobalType;
        } else if (QSharedPointer<const QmlComponentChain> chain = m_scopeChain.qmlComponentChain()) {
            if (scope == chain->idScope()) {
                type = SemanticHighlighter::LocalIdType;
            } else if (isIdScope(scope, chain->instantiatingComponents())) {
                type = SemanticHighlighter::ExternalIdType;
            } else if (scope == chain->rootObjectScope()) {
                type = SemanticHighlighter::RootObjectPropertyType;
            } else  { // check for this?
                type = SemanticHighlighter::ExternalObjectPropertyType;
            }
        }

        if (type != SemanticHighlighter::UnknownType) {
            // do not add uses of length 0 - this messes up highlighting (e.g. anon functions)
            if (location.length != 0)
                addUse(location, type);
        }
    }

    void processTypeId(UiQualifiedId *typeId)
    {
        if (!typeId)
            return;
        if (m_scopeChain.context()->lookupType(m_scopeChain.document().data(), typeId))
            addUse(fullLocationForQualifiedId(typeId), SemanticHighlighter::QmlTypeType);
    }

    void processBindingName(UiQualifiedId *localId)
    {
        if (!localId)
            return;
        addUse(fullLocationForQualifiedId(localId), SemanticHighlighter::BindingNameType);
    }

    bool visit(UiImport *ast) override
    {
        processName(ast->importId, ast->importIdToken);
        return true;
    }

    bool visit(UiObjectDefinition *ast) override
    {
        if (m_scopeChain.document()->bind()->isGroupedPropertyBinding(ast))
            processBindingName(ast->qualifiedTypeNameId);
        else
            processTypeId(ast->qualifiedTypeNameId);
        scopedAccept(ast, ast->initializer);
        return false;
    }

    bool visit(UiObjectBinding *ast) override
    {
        processTypeId(ast->qualifiedTypeNameId);
        processBindingName(ast->qualifiedId);
        scopedAccept(ast, ast->initializer);
        return false;
    }

    bool visit(UiScriptBinding *ast) override
    {
        processBindingName(ast->qualifiedId);
        scopedAccept(ast, ast->statement);
        return false;
    }

    bool visit(UiArrayBinding *ast) override
    {
        processBindingName(ast->qualifiedId);
        return true;
    }

    bool visit(UiPublicMember *ast) override
    {
        if (ast->typeToken.isValid() && ast->memberType) {
            if (m_scopeChain.context()->lookupType(m_scopeChain.document().data(), QStringList(ast->memberType->name.toString())))
                addUse(ast->typeToken, SemanticHighlighter::QmlTypeType);
        }
        if (ast->identifierToken.isValid())
            addUse(ast->identifierToken, SemanticHighlighter::BindingNameType);
        if (ast->statement)
            scopedAccept(ast, ast->statement);
        if (ast->binding)
            // this is not strictly correct for Components, as their context depends from where they
            // are instantiated, but normally not too bad as approximation
            scopedAccept(ast, ast->binding);
        return false;
    }

    bool visit(FunctionExpression *ast) override
    {
        processName(ast->name, ast->identifierToken);
        scopedAccept(ast, ast->body);
        return false;
    }

    bool visit(FunctionDeclaration *ast) override
    {
        return visit(static_cast<FunctionExpression *>(ast));
    }

    bool visit(UiEnumMemberList *ast) override
    {
        for (auto it = ast; it; it = it->next)
            addUse(it->memberToken, SemanticHighlighter::FieldType);
        return true;
    }

    bool visit(PatternElement *ast) override
    {
        if (ast->isVariableDeclaration())
            processName(ast->bindingIdentifier, ast->identifierToken);
        return true;
    }

    bool visit(ForEachStatement *ast) override
    {
        if (ast->type == ForEachType::Of)
            addUse(ast->inOfToken, SemanticHighlighter::KeywordType);
        return true;
    }

    bool visit(IdentifierExpression *ast) override
    {
        processName(ast->name, ast->identifierToken);
        return false;
    }

    bool visit(FieldMemberExpression *ast) override
    {
        if (ast->name.isEmpty() || ast->name.first().isLower())
            return true;
        // we only support IdentifierExpression.FieldMemberExpression (enum)
        const FieldMemberExpression *right = ast;
        if (const IdentifierExpression *idExp = cast<IdentifierExpression *>(ast->base)) {
            const ObjectValue *scope = nullptr;
            const Value *value = m_scopeChain.lookup(idExp->name.toString(), &scope);
            if (auto aov = value->asAstObjectValue()) {
                const ObjectValue *inObject = nullptr;
                const Value *enumValue = aov->lookupMember(right->name.toString(),
                                                           m_scopeChain.context(), &inObject);
                if (enumValue && enumValue->asNumberValue()) // can we do better?
                    addUse(right->identifierToken, SemanticHighlighter::FieldType);
            }
            return true;
        }
        // or IdentifierExpression.FieldMemberExpression.FieldMemberExpression (enum)
        const FieldMemberExpression *it = cast<FieldMemberExpression *>(ast->base);
        if (!it)
            return true;
        const IdentifierExpression *idExp = cast<IdentifierExpression *>(it->base);
        if (!idExp)
            return true;
        const ObjectValue *scope = nullptr;
        const Value *value = m_scopeChain.lookup(idExp->name.toString(), &scope);
        const ASTObjectValue *aoValue= value->asAstObjectValue();
        if (!aoValue)
            return true;
        if (auto maybeEnum = aoValue->lookupMember(it->name.toString(), m_scopeChain.context())) {
            if (const UiEnumValue *enumObject = maybeEnum->asUiEnumValue()) {
                addUse(it->identifierToken, SemanticHighlighter::QmlTypeType);
                if (enumObject->keys().contains(right->name))
                    addUse(right->identifierToken, SemanticHighlighter::FieldType);
            }
        }

        return true;
    }

    bool visit(StringLiteral *ast) override
    {
        if (ast->value.isEmpty())
            return false;

        const QString &value = ast->value.toString();
        if (m_stateNames.contains(value))
            addUse(ast->literalToken, SemanticHighlighter::LocalStateNameType);

        return false;
    }

    void addMessages(QList<DiagnosticMessage> messages,
            const Document::Ptr &doc)
    {
        for (const DiagnosticMessage &d : messages) {
            int line = d.loc.startLine;
            int column = qMax(1U, d.loc.startColumn);
            int length = d.loc.length;
            int begin = d.loc.begin();

            if (d.loc.length == 0) {
                QString source(doc->source());
                int end = begin;
                if (begin == source.size() || source.at(begin) == QLatin1Char('\n')
                        || source.at(begin) == QLatin1Char('\r')) {
                    while (begin > end - column && !source.at(--begin).isSpace()) { }
                } else {
                    while (end < source.size() && source.at(++end).isLetterOrNumber()) { }
                }
                column += begin - d.loc.begin();
                length = end-begin;
            }

            QTextCharFormat format;
            if (d.isWarning())
                format = m_fontSettings.toTextCharFormat(TextEditor::C_WARNING);
            else
                format = m_fontSettings.toTextCharFormat(TextEditor::C_ERROR);

            format.setToolTip(d.message);

            collectRanges(begin, length, format);
            addDelayedUse(SemanticHighlighter::Use(line, column, length, addFormat(format)));
        }
    }

    void addMessages(const QList<StaticAnalysis::Message> &messages,
                     const Document::Ptr &doc)
    {
        for (const StaticAnalysis::Message &d : messages) {
            int line = d.location.startLine;
            int column = qMax(1U, d.location.startColumn);
            int length = d.location.length;
            int begin = d.location.begin();

            if (d.location.length == 0) {
                QString source(doc->source());
                int end = begin;
                if (begin == source.size() || source.at(begin) == QLatin1Char('\n')
                        || source.at(begin) == QLatin1Char('\r')) {
                    while (begin > end - column && !source.at(--begin).isSpace()) { }
                } else {
                    while (end < source.size() && source.at(++end).isLetterOrNumber()) { }
                }
                column += begin - d.location.begin();
                length = end-begin;
            }

            QTextCharFormat format;
            if (d.severity == Severity::Warning
                    || d.severity == Severity::MaybeWarning
                    || d.severity == Severity::ReadingTypeInfoWarning) {
                format = m_fontSettings.toTextCharFormat(TextEditor::C_WARNING);
            } else if (d.severity == Severity::Error || d.severity == Severity::MaybeError) {
                format = m_fontSettings.toTextCharFormat(TextEditor::C_ERROR);
            } else if (d.severity == Severity::Hint) {
                format = m_fontSettings.toTextCharFormat(TextEditor::C_WARNING);
                format.setUnderlineColor(Qt::darkGreen);
            }

            format.setToolTip(d.message);

            collectRanges(begin, length, format);
            addDelayedUse(SemanticHighlighter::Use(line, column, length, addFormat(format)));
        }
    }

    void throwRecursionDepthError() override
    {
        qWarning("Warning: Hit Maximum recursion depth when visiting AST in CollectionTask");
    }

private:
    void addUse(const SourceLocation &location, SemanticHighlighter::UseType type)
    {
        addUse(SemanticHighlighter::Use(location.startLine, location.startColumn, location.length, type));
    }

    static const int chunkSize = 50;

    void addUse(const SemanticHighlighter::Use &use)
    {
        while (m_currentDelayedUse < m_delayedUses.size()
               && m_delayedUses.value(m_currentDelayedUse).line < use.line)
            m_uses.append(m_delayedUses.value(m_currentDelayedUse++));

        if (m_uses.size() >= chunkSize) {
            if (use.line > m_lineOfLastUse)
                flush();
        }

        m_lineOfLastUse = qMax(m_lineOfLastUse, use.line);
        m_uses.append(use);
    }

    void addDelayedUse(const SemanticHighlighter::Use &use)
    {
        m_delayedUses.append(use);
    }

    int addFormat(const QTextCharFormat &format)
    {
        int res = m_nextExtraFormat++;
        m_extraFormats.insert(res, format);
        return res;
    }

    void collectRanges(int start, int length, const QTextCharFormat &format) {
        QTextLayout::FormatRange range;
        range.start = start;
        range.length = length;
        range.format = format;
        m_diagnosticRanges.append(range);
    }

    static bool sortByLinePredicate(const SemanticHighlighter::Use &lhs, const SemanticHighlighter::Use &rhs)
    {
        return lhs.line < rhs.line;
    }

    void flush()
    {
        m_lineOfLastUse = 0;

        if (m_uses.isEmpty())
            return;

        Utils::sort(m_uses, sortByLinePredicate);
        for (const SemanticHighlighter::Use &use : std::as_const(m_uses))
            m_promise.addResult(use);
        m_uses.clear();
        m_uses.reserve(chunkSize);
    }

    QPromise<SemanticHighlighter::Use> &m_promise;
    const QmlJSTools::SemanticInfo &m_semanticInfo;
    const TextEditor::FontSettings &m_fontSettings;
    ScopeChain m_scopeChain;
    ScopeBuilder m_scopeBuilder;
    QStringList m_stateNames;
    QVector<SemanticHighlighter::Use> m_uses;
    int m_lineOfLastUse;
    QVector<SemanticHighlighter::Use> m_delayedUses;
    int m_nextExtraFormat;
    int m_currentDelayedUse;
    QHash<int, QTextCharFormat> m_extraFormats;
    QVector<QTextLayout::FormatRange> m_diagnosticRanges;
};

} // anonymous namespace

SemanticHighlighter::SemanticHighlighter(QmlJSEditorDocument *document)
    : QObject(document)
    , m_document(document)
    , m_startRevision(0)
{
    connect(&m_watcher, &QFutureWatcherBase::resultsReadyAt,
            this, &SemanticHighlighter::applyResults);
    connect(&m_watcher, &QFutureWatcherBase::finished,
            this, &SemanticHighlighter::finished);
}

void SemanticHighlighter::rerun(const QmlJSTools::SemanticInfo &semanticInfo)
{
    m_watcher.cancel();

    m_startRevision = m_document->document()->revision();
    auto future = Utils::asyncRun(QThread::LowestPriority, &SemanticHighlighter::run, this,
                                  semanticInfo, TextEditor::TextEditorSettings::fontSettings());
    m_watcher.setFuture(future);
    m_futureSynchronizer.addFuture(future);
}

void SemanticHighlighter::cancel()
{
    m_watcher.cancel();
}

void SemanticHighlighter::applyResults(int from, int to)
{
    if (m_watcher.isCanceled())
        return;
    if (m_startRevision != m_document->document()->revision())
        return;

    if (m_enableHighlighting)
        TextEditor::SemanticHighlighter::incrementalApplyExtraAdditionalFormats(
            m_document->syntaxHighlighter(), m_watcher.future(), from, to, m_extraFormats);
}

void SemanticHighlighter::finished()
{
    if (m_watcher.isCanceled())
        return;
    if (m_startRevision != m_document->document()->revision())
        return;

    if (m_enableWarnings)
        m_document->setDiagnosticRanges(m_diagnosticRanges);

    if (m_enableHighlighting)
        TextEditor::SemanticHighlighter::clearExtraAdditionalFormatsUntilEnd(
            m_document->syntaxHighlighter(), m_watcher.future());
}

void SemanticHighlighter::run(QPromise<Use> &promise,
                              const QmlJSTools::SemanticInfo &semanticInfo,
                              const TextEditor::FontSettings &fontSettings)
{
    CollectionTask task(promise,
                        semanticInfo,
                        fontSettings,
                        (m_enableWarnings ? CollectionTask::AddMessagesHighlights
                                          : CollectionTask::SkipMessagesHighlights));
    reportMessagesInfo(task.diagnosticRanges(), task.extraFormats());
    task.run();
}

void SemanticHighlighter::updateFontSettings(const TextEditor::FontSettings &fontSettings)
{
    m_formats[LocalIdType] = fontSettings.toTextCharFormat(TextEditor::C_QML_LOCAL_ID);
    m_formats[ExternalIdType] = fontSettings.toTextCharFormat(TextEditor::C_QML_EXTERNAL_ID);
    m_formats[QmlTypeType] = fontSettings.toTextCharFormat(TextEditor::C_QML_TYPE_ID);
    m_formats[RootObjectPropertyType] = fontSettings.toTextCharFormat(TextEditor::C_QML_ROOT_OBJECT_PROPERTY);
    m_formats[ScopeObjectPropertyType] = fontSettings.toTextCharFormat(TextEditor::C_QML_SCOPE_OBJECT_PROPERTY);
    m_formats[ExternalObjectPropertyType] = fontSettings.toTextCharFormat(TextEditor::C_QML_EXTERNAL_OBJECT_PROPERTY);
    m_formats[JsScopeType] = fontSettings.toTextCharFormat(TextEditor::C_JS_SCOPE_VAR);
    m_formats[JsImportType] = fontSettings.toTextCharFormat(TextEditor::C_JS_IMPORT_VAR);
    m_formats[JsGlobalType] = fontSettings.toTextCharFormat(TextEditor::C_JS_GLOBAL_VAR);
    m_formats[LocalStateNameType] = fontSettings.toTextCharFormat(TextEditor::C_QML_STATE_NAME);
    m_formats[BindingNameType] = fontSettings.toTextCharFormat(TextEditor::C_BINDING);
    m_formats[FieldType] = fontSettings.toTextCharFormat(TextEditor::C_FIELD);
    m_formats[KeywordType] = fontSettings.toTextCharFormat(TextEditor::C_KEYWORD);
}

void SemanticHighlighter::reportMessagesInfo(const QVector<QTextLayout::FormatRange> &diagnosticRanges,
                                             const QHash<int,QTextCharFormat> &formats)

{
    // tricky usage of m_extraFormats and diagnosticMessages we call this in another thread...
    // but will use them only after a signal sent by that same thread, maybe we should transfer
    // them more explicitly
    m_extraFormats = formats;
    Utils::addToHash(&m_extraFormats, m_formats);
    m_diagnosticRanges = diagnosticRanges;
}

int SemanticHighlighter::startRevision() const
{
    return m_startRevision;
}

void SemanticHighlighter::setEnableWarnings(bool e)
{
    m_enableWarnings = e;
}

void SemanticHighlighter::setEnableHighlighting(bool e)
{
    m_enableHighlighting = e;
}

} // namespace QmlJSEditor
