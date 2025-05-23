// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "flamegraphmodel.h"
#include "qmlprofilermodelmanager.h"
#include "qmlprofilertr.h"

#include <utils/algorithm.h>
#include <utils/qtcassert.h>

#include <QQueue>
#include <QSet>
#include <QString>

namespace QmlProfiler::Internal {

static inline quint64 supportedFeatures()
{
    return Constants::QML_JS_RANGE_FEATURES | (1ULL << ProfileMemory);
}

FlameGraphModel::FlameGraphModel(QmlProfilerModelManager *modelManager,
                                 QObject *parent) : QAbstractItemModel(parent)
{
    m_modelManager = modelManager;
    m_callStack.append(QmlEvent());
    m_compileStack.append(QmlEvent());
    m_callStackTop = &m_stackBottom;
    m_compileStackTop = &m_stackBottom;
    connect(modelManager, &QmlProfilerModelManager::typeDetailsFinished,
            this, &FlameGraphModel::onTypeDetailsFinished);
    connect(modelManager->notesModel(), &Timeline::TimelineNotesModel::changed,
            this, [this](int typeId, int, int){loadNotes(typeId, true);});
    m_acceptedFeatures = supportedFeatures();

    modelManager->registerFeatures(m_acceptedFeatures,
                                   std::bind(&FlameGraphModel::loadEvent, this,
                                             std::placeholders::_1, std::placeholders::_2),
                                   std::bind(&FlameGraphModel::beginResetModel, this),
                                   std::bind(&FlameGraphModel::finalize, this),
                                   std::bind(&FlameGraphModel::clear, this));
}

void FlameGraphModel::clear()
{
    beginResetModel();
    m_stackBottom = FlameGraphData(nullptr, -1, 0);
    m_callStack.clear();
    m_compileStack.clear();
    m_callStack.append(QmlEvent());
    m_compileStack.append(QmlEvent());
    m_callStackTop = &m_stackBottom;
    m_compileStackTop = &m_stackBottom;
    m_typeIdsWithNotes.clear();
    endResetModel();
}

void FlameGraphModel::loadNotes(int typeIndex, bool emitSignal)
{
    QSet<int> changedNotes;
    Timeline::TimelineNotesModel *notes = m_modelManager->notesModel();
    if (typeIndex == -1) {
        changedNotes = m_typeIdsWithNotes;
        m_typeIdsWithNotes.clear();
        for (int i = 0; i < notes->count(); ++i)
            m_typeIdsWithNotes.insert(notes->typeId(i));
        changedNotes += m_typeIdsWithNotes;
    } else {
        changedNotes.insert(typeIndex);
        if (notes->byTypeId(typeIndex).isEmpty())
            m_typeIdsWithNotes.remove(typeIndex);
        else
            m_typeIdsWithNotes.insert(typeIndex);
    }

    if (emitSignal)
        emit dataChanged(QModelIndex(), QModelIndex(), QList<int>() << NoteRole);
}

void FlameGraphModel::loadEvent(const QmlEvent &event, const QmlEventType &type)
{
    if (!(m_acceptedFeatures & (1ULL << type.feature())))
        return;

    const bool isCompiling = (type.rangeType() == Compiling);
    QStack<QmlEvent> &stack =  isCompiling ? m_compileStack : m_callStack;
    FlameGraphData *&stackTop = isCompiling ? m_compileStackTop : m_callStackTop;
    QTC_ASSERT(stackTop, return);

    if (type.message() == MemoryAllocation) {
        if (type.detailType() == HeapPage)
            return; // We're only interested in actual allocations, not heap pages being mmap'd

        qint64 amount = event.number<qint64>(0);
        if (amount < 0)
            return; // We're not interested in GC runs here

        for (FlameGraphData *data = stackTop; data; data = data->parent) {
            ++data->allocations;
            data->memory += amount;
        }

    } else if (event.rangeStage() == RangeEnd) {
        QTC_ASSERT(stackTop != &m_stackBottom, return);
        QTC_ASSERT(stackTop->typeIndex == event.typeIndex(), return);
        stackTop->duration += event.timestamp() - stack.top().timestamp();
        stack.pop();
        stackTop = stackTop->parent;
    } else {
        QTC_ASSERT(event.rangeStage() == RangeStart, return);
        stack.push(event);
        stackTop = pushChild(stackTop, event);
    }
    QTC_CHECK(stackTop);
}

void FlameGraphModel::finalize()
{
    for (FlameGraphData *child : std::as_const(m_stackBottom.children))
        m_stackBottom.duration += child->duration;

    loadNotes(-1, false);
    endResetModel();
}

void FlameGraphModel::onTypeDetailsFinished()
{
    emit dataChanged(QModelIndex(), QModelIndex(), QList<int>(1, DetailsRole));
}

void FlameGraphModel::restrictToFeatures(quint64 visibleFeatures)
{
    visibleFeatures = visibleFeatures & supportedFeatures();
    if (visibleFeatures == m_acceptedFeatures)
        return;

    m_acceptedFeatures = visibleFeatures;

    clear();

    QFutureInterface<void> future;
    const auto filter = m_modelManager->rangeFilter(m_modelManager->traceStart(),
                                                    m_modelManager->traceEnd());
    m_modelManager->replayQmlEvents(filter(std::bind(&FlameGraphModel::loadEvent, this,
                                                     std::placeholders::_1, std::placeholders::_2)),
                                    std::bind(&FlameGraphModel::beginResetModel, this),
                                    std::bind(&FlameGraphModel::finalize, this),
                                    [this](const QString &message) {
        if (!message.isEmpty()) {
            emit m_modelManager->error(Tr::tr("Could not re-read events from temporary trace file: %1")
                                           .arg(message));
        }
        endResetModel();
        clear();
    }, future);
}

static QString nameForType(RangeType typeNumber)
{
    switch (typeNumber) {
    case Compiling: return Tr::tr("Compile");
    case Creating: return Tr::tr("Create");
    case Binding: return Tr::tr("Binding");
    case HandlingSignal: return Tr::tr("Signal");
    case Javascript: return Tr::tr("JavaScript");
    default: Q_UNREACHABLE();
    }
}

QVariant FlameGraphModel::lookup(const FlameGraphData &stats, int role) const
{
    switch (role) {
    case TypeIdRole: return stats.typeIndex;
    case NoteRole: {
        QString ret;
        if (!m_typeIdsWithNotes.contains(stats.typeIndex))
            return ret;
        Timeline::TimelineNotesModel *notes = m_modelManager->notesModel();
        const QList<QVariant> items = notes->byTypeId(stats.typeIndex);
        for (const QVariant &item : items) {
            if (ret.isEmpty())
                ret = notes->text(item.toInt());
            else
                ret += QChar::LineFeed + notes->text(item.toInt());
        }
        return ret;
    }
    case DurationRole: return stats.duration;
    case CallCountRole: return stats.calls;
    case TimePerCallRole: return stats.duration / stats.calls;
    case AllocationsRole: return stats.allocations;
    case MemoryRole: return stats.memory;
    default: break;
    }

    if (stats.typeIndex != -1) {
        const QmlEventType &type = m_modelManager->eventType(stats.typeIndex);

        switch (role) {
        case FilenameRole: return type.location().filename();
        case LineRole: return type.location().line();
        case ColumnRole: return type.location().column();
        case TypeRole: return nameForType(type.rangeType());
        case RangeTypeRole: return type.rangeType();
        case DetailsRole: return type.data().isEmpty() ?
                        Tr::tr("Source code not available") : type.data();
        case LocationRole: return type.displayName();
        default: return QVariant();
        }
    } else {
        return QVariant();
    }
}

FlameGraphData::FlameGraphData(FlameGraphData *parent, int typeIndex, qint64 duration) :
    duration(duration), calls(1), memory(0), allocations(0), typeIndex(typeIndex), parent(parent) {}

FlameGraphData::~FlameGraphData()
{
    qDeleteAll(children);
}

FlameGraphData *FlameGraphModel::pushChild(FlameGraphData *parent, const QmlEvent &data)
{
    QList<FlameGraphData *> &siblings = parent->children;

    for (auto it = siblings.begin(), end = siblings.end(); it != end; ++it) {
        FlameGraphData *child = *it;
        if (child->typeIndex == data.typeIndex()) {
            ++child->calls;
            for (auto back = it, front = siblings.begin(); back != front;) {
                 --back;
                if ((*back)->calls >= (*it)->calls)
                    break;
                qSwap(*it, *back);
                it = back;
            }
            return child;
        }
    }

    auto child = new FlameGraphData(parent, data.typeIndex());
    parent->children.append(child);
    return child;
}

QModelIndex FlameGraphModel::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid()) {
        auto parentData = static_cast<const FlameGraphData *>(parent.internalPointer());
        return createIndex(row, column, parentData->children[row]);
    } else {
        return createIndex(row, column, row >= 0 ? m_stackBottom.children[row] : nullptr);
    }
}

QModelIndex FlameGraphModel::parent(const QModelIndex &child) const
{
    if (child.isValid()) {
        auto childData = static_cast<const FlameGraphData *>(child.internalPointer());
        return childData->parent == &m_stackBottom ? QModelIndex() :
                                                     createIndex(0, 0, childData->parent);
    } else {
        return QModelIndex();
    }
}

int FlameGraphModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        auto parentData = static_cast<const FlameGraphData *>(parent.internalPointer());
        return parentData->children.count();
    } else {
        return m_stackBottom.children.count();
    }
}

int FlameGraphModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return 1;
}

QVariant FlameGraphModel::data(const QModelIndex &index, int role) const
{
    auto data = static_cast<const FlameGraphData *>(index.internalPointer());
    return lookup(data ? *data : m_stackBottom, role);
}

QHash<int, QByteArray> FlameGraphModel::roleNames() const
{
    static const QHash<int, QByteArray> extraRoles{
        {TypeIdRole, "typeId"},
        {TypeRole, "type"},
        {DurationRole, "duration"},
        {CallCountRole, "callCount"},
        {DetailsRole, "details"},
        {FilenameRole, "filename"},
        {LineRole, "line"},
        {ColumnRole, "column"},
        {NoteRole, "note"},
        {TimePerCallRole, "timePerCall"},
        {RangeTypeRole, "rangeType"},
        {LocationRole, "location" },
        {AllocationsRole, "allocations" },
        {MemoryRole, "memory" }
    };
    QHash<int, QByteArray> roles = QAbstractItemModel::roleNames();
    Utils::addToHash(&roles, extraRoles);
    return roles;
}

QmlProfilerModelManager *FlameGraphModel::modelManager() const
{
    return m_modelManager;
}

} // namespace QmlProfiler::Internal
