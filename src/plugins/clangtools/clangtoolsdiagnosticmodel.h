// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "clangtoolsdiagnostic.h"
#include "clangtoolsprojectsettings.h"
#include "clangtoolsutils.h"

#include <debugger/analyzer/detailederrorview.h>
#include <utils/filesystemwatcher.h>
#include <utils/treemodel.h>

#include <QPointer>
#include <QSortFilterProxyModel>

#include <map>
#include <memory>
#include <optional>

namespace ProjectExplorer { class Project; }

namespace ClangTools {
namespace Internal {

class ClangToolsDiagnosticModel;
class InlineSuppressedDiagnostics;

class FilePathItem : public Utils::TreeItem
{
public:
    FilePathItem(const Utils::FilePath &filePath);
    QVariant data(int column, int role) const override;

private:
    const Utils::FilePath m_filePath;
};

class DiagnosticMark;

class DiagnosticItem : public Utils::TreeItem
{
public:
    DiagnosticItem(const Diagnostic &diag, bool generateMark, ClangToolsDiagnosticModel *model);
    ~DiagnosticItem() override;

    const Diagnostic &diagnostic() const { return m_diagnostic; }
    void setTextMarkVisible(bool visible);

    FixitStatus fixItStatus() const { return m_fixitStatus; }
    void setFixItStatus(const FixitStatus &status, bool updateUI);
    bool scheduleOrUnscheduleFixit(FixitStatus status, bool updateUi);

    bool hasNewFixIts() const;

    bool setData(int column, const QVariant &data, int role) override;

private:
    Qt::ItemFlags flags(int column) const override;
    QVariant data(int column, int role) const override;
    ClangToolsDiagnosticModel *diagModel() const;

private:
    const Diagnostic m_diagnostic;
    FixitStatus m_fixitStatus = FixitStatus::NotAvailable;
    TextEditor::TextMark *m_mark = nullptr;
};

class ExplainingStepItem;

using ClangToolsDiagnosticModelBase
    = Utils::TreeModel<Utils::TreeItem, FilePathItem, DiagnosticItem, ExplainingStepItem>;
class ClangToolsDiagnosticModel : public ClangToolsDiagnosticModelBase
{
    Q_OBJECT

    friend class DiagnosticItem;

public:
    ClangToolsDiagnosticModel(CppEditor::ClangToolType type, QObject *parent = nullptr);

    void addDiagnostics(const Diagnostics &diagnostics, bool generateMarks, Utils::TreeItem *rootItem);
    QSet<Diagnostic> diagnostics() const;

    enum ItemRole {
        DiagnosticRole = Debugger::DetailedErrorView::FullTextRole + 1,
        TextRole,
        CheckBoxEnabledRole,
        DocumentationUrlRole,
        InlineSuppressableRole,
    };

    QSet<QString> allChecks() const;

    void clear();
    void removeWatchedPath(const Utils::FilePath &path);
    void addWatchedPath(const Utils::FilePath &path);
    void resetRootItem(Utils::TreeItem *root) { setRootItem(root); };
    Utils::TreeItem *createRootItem() const;

    std::unique_ptr<InlineSuppressedDiagnostics> createInlineSuppressedDiagnostics();

    const QList<DiagnosticItem *> &itemsWithSameFixits(const DiagnosticItem *item);

signals:
    void fixitStatusChanged(
        const DiagnosticItem *item, FixitStatus oldStatus, FixitStatus newStatus, bool updateUi);

private:
    void connectFileWatcher();
    void onFileChanged(const Utils::FilePath &path);
    void clearAndSetupCache();

    QHash<Utils::FilePath, FilePathItem *> m_filePathToItem;
    QSet<Diagnostic> m_diagnostics;
    std::map<QList<ExplainingStep>, QList<DiagnosticItem *>> m_stepsToItemsCache;
    std::unique_ptr<Utils::FileSystemWatcher> m_filesWatcher;
    const CppEditor::ClangToolType m_type;
};

class FilterOptions {
public:
    QSet<QString> checks;
};
using OptionalFilterOptions = std::optional<FilterOptions>;

class DiagnosticFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    DiagnosticFilterModel(QObject *parent = nullptr);

    void setProject(ProjectExplorer::Project *project);
    void addSuppressedDiagnostics(const SuppressedDiagnosticsList &diags);
    void addSuppressedDiagnostic(const SuppressedDiagnostic &diag);
    ProjectExplorer::Project *project() const { return m_project; }

    OptionalFilterOptions filterOptions() const;
    void setFilterOptions(const OptionalFilterOptions &filterOptions);

    void onFixitStatusChanged(const DiagnosticItem *item,
                              FixitStatus oldStatus,
                              FixitStatus newStatus, bool updateUi);

    void reset();
    int diagnostics() const { return m_diagnostics; }
    int fixitsSchedulable() const { return m_fixitsSchedulable; }
    int fixitsScheduled() const { return m_fixitsScheduled; }

signals:
    void fixitCountersChanged();

private:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &l, const QModelIndex &r) const override;
    struct Counters {
        int diagnostics = 0;
        int fixits = 0;
    };
    Counters countDiagnostics(const QModelIndex &parent, int first, int last) const;
    void handleSuppressedDiagnosticsChanged();
    bool filterAcceptsItem(const DiagnosticItem *item) const;

    QPointer<ProjectExplorer::Project> m_project;
    Utils::FilePath m_lastProjectDirectory;
    SuppressedDiagnosticsList m_suppressedDiagnostics;

    OptionalFilterOptions m_filterOptions;

    int m_diagnostics = 0;
    int m_fixitsSchedulable = 0;
    int m_fixitsScheduled = 0;
};

} // namespace Internal
} // namespace ClangTools
