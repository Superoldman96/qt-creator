// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "qmlprofilernotesmodel.h"
#include "qmlprofilereventtypes.h"
#include "qmleventlocation.h"

#include <QSet>
#include <QStack>
#include <QAbstractItemModel>

namespace QmlProfiler {
namespace Internal {

struct FlameGraphData {
    FlameGraphData(FlameGraphData *parent = nullptr, int typeIndex = -1, qint64 duration = 0);
    ~FlameGraphData();

    qint64 duration;
    qint64 calls;
    qint64 memory;

    int allocations;
    int typeIndex;

    FlameGraphData *parent;
    QList<FlameGraphData *> children;
};

class FlameGraphModel : public QAbstractItemModel
{
    Q_OBJECT
    QML_NAMED_ELEMENT(QmlProfilerFlameGraphModel)
    QML_UNCREATABLE("use the context property")
public:
    enum Role {
        TypeIdRole = Qt::UserRole + 1, // Sort by data, not by displayed string
        TypeRole,
        DurationRole,
        CallCountRole,
        DetailsRole,
        FilenameRole,
        LineRole,
        ColumnRole,
        NoteRole,
        TimePerCallRole,
        RangeTypeRole,
        LocationRole,
        AllocationsRole,
        MemoryRole,
        MaxRole
    };
    Q_ENUM(Role)

    FlameGraphModel(QmlProfilerModelManager *modelManager, QObject *parent = nullptr);

    QModelIndex index(int row, int column,
                      const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    Q_INVOKABLE int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    Q_INVOKABLE int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QmlProfilerModelManager *modelManager() const;

    void loadEvent(const QmlEvent &event, const QmlEventType &type);
    void finalize();
    void onTypeDetailsFinished();
    void restrictToFeatures(quint64 visibleFeatures);
    void loadNotes(int typeId, bool emitSignal);
    void clear();

signals:
    void gotoSourceLocation(const QString &fileName, int lineNumber, int columnNumber);

private:
    QVariant lookup(const FlameGraphData &data, int role) const;
    FlameGraphData *pushChild(FlameGraphData *parent, const QmlEvent &data);

    // used by binding loop detection
    QStack<QmlEvent> m_callStack;
    QStack<QmlEvent> m_compileStack;
    FlameGraphData m_stackBottom;
    FlameGraphData *m_callStackTop;
    FlameGraphData *m_compileStackTop;

    quint64 m_acceptedFeatures;
    QmlProfilerModelManager *m_modelManager;

    QSet<int> m_typeIdsWithNotes;
};

} // namespace Internal
} // namespace QmlProfiler
