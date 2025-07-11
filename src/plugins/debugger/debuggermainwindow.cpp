// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "debuggermainwindow.h"
#include "debuggerconstants.h"
#include "debuggertr.h"
#include "enginemanager.h"

#include <coreplugin/actionmanager/actioncontainer.h>
#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/actionmanager/command.h>
#include <coreplugin/coreconstants.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/icore.h>
#include <coreplugin/modemanager.h>
#include <coreplugin/outputpane.h>
#include <coreplugin/rightpane.h>

#include <projectexplorer/projectexplorericons.h>

#include <utils/algorithm.h>
#include <utils/styledbar.h>
#include <utils/qtcassert.h>
#include <utils/proxyaction.h>
#include <utils/utilsicons.h>
#include <utils/stylehelper.h>

#include <QAction>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLoggingCategory>
#include <QMenu>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>

using namespace Debugger;
using namespace Core;

static Q_LOGGING_CATEGORY(perspectivesLog, "qtc.utils.perspectives", QtWarningMsg)

namespace Utils {

const char LAST_PERSPECTIVE_KEY[]   = "LastPerspective";
const char MAINWINDOW_KEY[]         = "Debugger.MainWindow";
const char SHOW_CENTRALWIDGET_KEY[] = "ShowCentralWidget";
const char STATE_KEY2[]             = "State2";
const char CHANGED_DOCK_KEY[]       = "ChangedDocks";

static DebuggerMainWindow *theMainWindow = nullptr;

class DockOperation
{
public:
    void setupLayout();
    void ensureDockExists();

    QString name() const { QTC_ASSERT(widget, return QString()); return widget->objectName(); }
    bool changedByUser() const;
    void recordVisibility();

    Utils::Id commandId;
    QPointer<QWidget> widget;
    QPointer<QDockWidget> dock;
    QPointer<QWidget> anchorWidget;
    QPointer<Utils::ProxyAction> toggleViewAction;
    Perspective::OperationType operationType = Perspective::Raise;
    bool visibleByDefault = true;
    Qt::DockWidgetArea area = Qt::BottomDockWidgetArea;
};

class PerspectivePrivate
{
public:
    ~PerspectivePrivate();

    void showInnerToolBar();
    void hideInnerToolBar();
    void restoreLayout();
    void saveLayout();
    void resetPerspective();
    void populatePerspective();
    void depopulatePerspective();
    void saveAsLastUsedPerspective();
    Context context() const;

    QString settingsId() const;
    QToolButton *setupToolButton(QAction *action);

    QString m_id;
    QString m_name;
    QString m_parentPerspectiveId;
    QString m_settingsId;
    QList<DockOperation> m_dockOperations;
    QPointer<QWidget> m_centralWidget;
    Perspective::Callback m_aboutToActivateCallback;
    QPointer<QWidget> m_innerToolBar;
    QHBoxLayout *m_innerToolBarLayout = nullptr;
    QPointer<QWidget> m_switcher;
    QString m_lastActiveSubPerspectiveId;
};

class DebuggerMainWindowPrivate : public QObject
{
public:
    DebuggerMainWindowPrivate(DebuggerMainWindow *parent);
    ~DebuggerMainWindowPrivate();

    void depopulateCurrentPerspective();
    void populateCurrentPerspective();
    void destroyPerspective(Perspective *perspective);
    void registerPerspective(Perspective *perspective);
    void resetCurrentPerspective();
    int indexInChooser(Perspective *perspective) const;
    void updatePerspectiveChooserWidth();

    void cleanDocks();
    void setCentralWidget(QWidget *widget);

    QDockWidget *dockForWidget(QWidget *widget) const;

    void setCurrentPerspective(Perspective *perspective)
    {
        const Core::Context oldContext = m_currentPerspective
                ? Context(Id::fromString(m_currentPerspective->id())) : Context();
        m_currentPerspective = perspective;
        const Core::Context newContext = m_currentPerspective
                ? Context(Id::fromString(m_currentPerspective->id())) : Context();
        ICore::updateAdditionalContexts(oldContext, newContext);
    }

    DebuggerMainWindow *q = nullptr;
    QPointer<Perspective> m_currentPerspective = nullptr;
    QComboBox *m_perspectiveChooser = nullptr;
    QMenu *m_perspectiveMenu;
    QStackedWidget *m_centralWidgetStack = nullptr;
    QHBoxLayout *m_subPerspectiveSwitcherLayout = nullptr;
    QHBoxLayout *m_innerToolsLayout = nullptr;
    QPointer<QWidget> m_editorPlaceHolder;
    Utils::StatusLabel *m_statusLabel = nullptr;
    QDockWidget *m_toolBarDock = nullptr;
    bool needRestoreOnModeEnter = false;

    QList<QPointer<Perspective>> m_perspectives;
    QSet<QString> m_persistentChangedDocks; // Dock Ids of docks with non-default visibility.

    QHash<QString, PerspectiveState> m_lastPerspectiveStates;      // Perspective::id() -> MainWindow::state()
    QHash<QString, PerspectiveState> m_lastTypePerspectiveStates;  // Perspective::settingsId() -> MainWindow::state()
};

class TweakedCombo : public QComboBox // ensures that all items of the popup are readable
{
public:
    explicit TweakedCombo(QWidget *parent = nullptr) : QComboBox(parent) {}
    void showPopup() override
    {
        QTC_ASSERT(view(), return);
        view()->setMinimumWidth(view()->sizeHintForColumn(0));
        QComboBox::showPopup();
    }
};

DebuggerMainWindowPrivate::DebuggerMainWindowPrivate(DebuggerMainWindow *parent)
    : q(parent)
{
    m_centralWidgetStack = new QStackedWidget;
    m_statusLabel = new Utils::StatusLabel;
    m_statusLabel->setObjectName("DebuggerStatusLabel"); // used by Squish
    StyleHelper::setPanelWidget(m_statusLabel);
    m_statusLabel->setIndent(2 * QFontMetrics(q->font()).horizontalAdvance(QChar('x')));
    m_editorPlaceHolder = new EditorManagerPlaceHolder;

    m_perspectiveChooser = new TweakedCombo;
    m_perspectiveChooser->setObjectName("PerspectiveChooser");
    StyleHelper::setPanelWidget(m_perspectiveChooser);
    m_perspectiveChooser->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(m_perspectiveChooser, &QComboBox::activated, this, [this](int item) {
        Perspective *perspective = Perspective::findPerspective(m_perspectiveChooser->itemData(item).toString());
        QTC_ASSERT(perspective, return);
        if (auto subPerspective = Perspective::findPerspective(perspective->d->m_lastActiveSubPerspectiveId))
            subPerspective->select();
        else
            perspective->select();
    });

    m_perspectiveMenu = new QMenu;
    connect(m_perspectiveMenu, &QMenu::aboutToShow, this, [this] {
        m_perspectiveMenu->clear();
        DebuggerMainWindow::addPerspectiveMenu(m_perspectiveMenu);
    });

    auto viewButton = new QToolButton;
    viewButton->setText(Tr::tr("&Views"));

    auto closeButton = new QToolButton();
    closeButton->setIcon(Utils::Icons::CLOSE_SPLIT_BOTTOM.icon());
    closeButton->setToolTip(Tr::tr("Leave Debug Mode"));

    auto toolbar = new Utils::StyledBar;
    toolbar->setProperty(StyleHelper::C_TOP_BORDER, true);

    // "Engine switcher" style comboboxes
    auto subPerspectiveSwitcher = new QWidget;
    m_subPerspectiveSwitcherLayout = new QHBoxLayout(subPerspectiveSwitcher);
    m_subPerspectiveSwitcherLayout->setContentsMargins(0, 0, 0, 0);
    m_subPerspectiveSwitcherLayout->setSpacing(0);

    // All perspective toolbars will get inserted here, but only
    // the current perspective's toolbar is set visible.
    auto innerTools = new QWidget;
    m_innerToolsLayout = new QHBoxLayout(innerTools);
    m_innerToolsLayout->setContentsMargins(0, 0, 0, 0);
    m_innerToolsLayout->setSpacing(0);

    auto hbox = new QHBoxLayout(toolbar);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);
    hbox->addWidget(m_perspectiveChooser);
    hbox->addWidget(subPerspectiveSwitcher);
    hbox->addWidget(innerTools);
    hbox->addWidget(m_statusLabel);
    hbox->addStretch(1);
    hbox->addWidget(new Utils::StyledSeparator);
    hbox->addWidget(viewButton);
    hbox->addWidget(closeButton);

    auto scrolledToolbar = new QScrollArea;
    scrolledToolbar->setWidget(toolbar);
    scrolledToolbar->setFrameStyle(QFrame::NoFrame);
    scrolledToolbar->setWidgetResizable(true);
    scrolledToolbar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrolledToolbar->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    StyleHelper::setPanelWidgetSingleRow(scrolledToolbar);

    auto dock = new QDockWidget(Tr::tr("Toolbar"), q);
    dock->setObjectName("Toolbar");
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setAllowedAreas(Qt::BottomDockWidgetArea);
    dock->setTitleBarWidget(new QWidget(dock)); // hide title bar
    dock->setProperty("managed_dockwidget", "true");
    dock->setWidget(scrolledToolbar);

    m_toolBarDock = dock;
    q->addDockWidget(Qt::BottomDockWidgetArea, m_toolBarDock);

    connect(viewButton, &QAbstractButton::clicked, this, [viewButton] {
        ActionContainer *viewsMenu = ActionManager::actionContainer(Core::Constants::M_VIEW_VIEWS);
        viewsMenu->menu()->exec(viewButton->mapToGlobal(QPoint()));
    });

    connect(closeButton, &QAbstractButton::clicked, [] {
        ModeManager::activateMode(Core::Constants::MODE_EDIT);
    });
}

DebuggerMainWindowPrivate::~DebuggerMainWindowPrivate()
{
    delete m_editorPlaceHolder;
    delete m_perspectiveMenu;
}

DebuggerMainWindow::DebuggerMainWindow()
    : d(new DebuggerMainWindowPrivate(this))
{
    setDockNestingEnabled(true);
    setDockActionsVisible(false);
    setDocumentMode(true);

    connect(this, &FancyMainWindow::resetLayout,
            d, &DebuggerMainWindowPrivate::resetCurrentPerspective);

    Context debugcontext(Debugger::Constants::C_DEBUGMODE);

    ActionContainer *viewsMenu = ActionManager::actionContainer(Core::Constants::M_VIEW_VIEWS);
    Command *cmd = ActionManager::registerAction(showCentralWidgetAction(),
        "Debugger.Views.ShowCentralWidget", debugcontext);
    cmd->setAttribute(Command::CA_Hide);
    cmd->setAttribute(Command::CA_UpdateText);
    viewsMenu->addAction(cmd, Core::Constants::G_DEFAULT_THREE);
    cmd = ActionManager::registerAction(menuSeparator1(),
        "Debugger.Views.Separator1", debugcontext);
    cmd->setAttribute(Command::CA_Hide);
    viewsMenu->addAction(cmd, Core::Constants::G_DEFAULT_THREE);
    cmd = ActionManager::registerAction(resetLayoutAction(),
        "Debugger.Views.ResetSimple", debugcontext);
    cmd->setAttribute(Command::CA_Hide);
    viewsMenu->addAction(cmd, Core::Constants::G_DEFAULT_THREE);

    // HACK: See QTCREATORBUG-23755. This ensures the showCentralWidget()
    // call in restorePersistentSettings() below has something to operate on,
    // and a plain QWidget is what we'll use anyway as central widget.
    setCentralWidget(new QWidget);

    restorePersistentSettings();
}

DebuggerMainWindow::~DebuggerMainWindow()
{
    delete d;
}

void DebuggerMainWindow::contextMenuEvent(QContextMenuEvent *ev)
{
    ActionContainer *viewsMenu = ActionManager::actionContainer(Core::Constants::M_VIEW_VIEWS);
    viewsMenu->menu()->exec(ev->globalPos());
}

void DebuggerMainWindow::ensureMainWindowExists()
{
    if (!theMainWindow)
        theMainWindow = new DebuggerMainWindow;
}

void DebuggerMainWindow::doShutdown()
{
    QTC_ASSERT(theMainWindow, return);

    theMainWindow->savePersistentSettings();

    delete theMainWindow;
    theMainWindow = nullptr;
}

void DebuggerMainWindowPrivate::registerPerspective(Perspective *perspective)
{
    QString parentPerspective = perspective->d->m_parentPerspectiveId;
    // Add only "main" perspectives to the chooser.
    if (parentPerspective.isEmpty())
        m_perspectiveChooser->addItem(perspective->d->m_name, perspective->d->m_id);
    m_perspectives.append(perspective);
}

void DebuggerMainWindowPrivate::destroyPerspective(Perspective *perspective)
{
    qCDebug(perspectivesLog) << "ABOUT TO DESTROY PERSPECTIVE" << perspective->id();

    const bool wasCurrent = perspective == m_currentPerspective;
    if (wasCurrent) {
        qCDebug(perspectivesLog) << "RAMPING IT DOWN FIRST AS IT WAS CURRENT" << perspective->id();
        perspective->rampDownAsCurrent();
    }

    m_perspectives.removeAll(perspective);

    // Dynamic perspectives are currently not visible in the chooser.
    // This might change in the future, make sure we notice.
    const int idx = indexInChooser(perspective);
    if (idx != -1)
        m_perspectiveChooser->removeItem(idx);

    for (DockOperation &op : perspective->d->m_dockOperations) {
        if (op.commandId.isValid())
            ActionManager::unregisterAction(op.toggleViewAction, op.commandId);
        if (op.dock) {
            theMainWindow->removeDockWidget(op.dock);
            op.widget->setParent(nullptr); // Prevent deletion
            op.dock->setParent(nullptr);
            delete op.dock;
            op.dock = nullptr;
        }
    }

    if (wasCurrent) {
        if (Perspective *parent = Perspective::findPerspective(perspective->d->m_parentPerspectiveId)) {
            qCDebug(perspectivesLog) << "RAMPING UP PARENT PERSPECTIVE" << parent->id();
            parent->rampUpAsCurrent();
        } else {
            qCDebug(perspectivesLog) << "RAMPED DOWN WAS ACTION, BUT NO PARENT AVAILABLE. TAKE FIRST BEST";
            if (QTC_GUARD(!m_perspectives.isEmpty()))
                m_perspectives.first()->rampUpAsCurrent();
        }
    }

    qCDebug(perspectivesLog) << "DESTROYED PERSPECTIVE" << perspective->id();
}

void DebuggerMainWindow::showStatusMessage(const QString &message, int timeoutMS)
{
    if (theMainWindow)
        theMainWindow->d->m_statusLabel->showStatusMessage(message, timeoutMS);
}

void DebuggerMainWindow::enterDebugMode()
{
    theMainWindow->setDockActionsVisible(true);
    QTC_CHECK(theMainWindow->d->m_currentPerspective == nullptr);
    if (theMainWindow->d->needRestoreOnModeEnter)
        theMainWindow->restorePersistentSettings();

    QtcSettings *settings = ICore::settings();
    const QString lastPerspectiveId = settings->value(LAST_PERSPECTIVE_KEY).toString();
    Perspective *perspective = Perspective::findPerspective(lastPerspectiveId);
    // If we don't find a perspective with the stored name, pick any.
    // This can happen e.g. when a plugin was disabled that provided
    // the stored perspective, or when the save file was modified externally.
    if (!perspective && !theMainWindow->d->m_perspectives.isEmpty())
        perspective = theMainWindow->d->m_perspectives.first();

    // There's at least the debugger preset perspective that should be found above.
    QTC_ASSERT(perspective, return);

    if (auto sub = Perspective::findPerspective(perspective->d->m_lastActiveSubPerspectiveId)) {
        qCDebug(perspectivesLog) << "SWITCHING TO SUBPERSPECTIVE" << sub->d->m_id;
        perspective = sub;
    }

    perspective->rampUpAsCurrent();
}

void DebuggerMainWindow::leaveDebugMode()
{
    theMainWindow->d->needRestoreOnModeEnter = true;
    theMainWindow->savePersistentSettings();

    if (theMainWindow->d->m_currentPerspective)
        theMainWindow->d->m_currentPerspective->rampDownAsCurrent();
    QTC_CHECK(theMainWindow->d->m_currentPerspective == nullptr);

    theMainWindow->setDockActionsVisible(false);

    // Hide dock widgets manually in case they are floating.
    for (QDockWidget *dockWidget : theMainWindow->dockWidgets()) {
        if (dockWidget->isFloating())
            dockWidget->setVisible(false);
    }
}

void DebuggerMainWindow::restorePersistentSettings()
{
    qCDebug(perspectivesLog) << "RESTORE ALL PERSPECTIVES";
    QtcSettings *settings = ICore::settings();
    settings->beginGroup(MAINWINDOW_KEY);

    const QHash<QString, QVariant> states2 = settings->value(STATE_KEY2).toHash();
    d->m_lastTypePerspectiveStates.clear();
    for (auto it = states2.begin(); it != states2.end(); ++it) {
        PerspectiveState state;
        if (it->canConvert<QVariantMap>()) {
            state = PerspectiveState::fromSettings(storeFromMap(it->toMap()));
        } else {
            // legacy for up to QtC 12
            state = it->value<PerspectiveState>();
        }
        QTC_ASSERT(state.hasWindowState(), continue);
        d->m_lastTypePerspectiveStates.insert(it.key(), state);
    }

    showCentralWidget(settings->value(SHOW_CENTRALWIDGET_KEY, true).toBool());
    d->m_persistentChangedDocks = Utils::toSet(settings->value(CHANGED_DOCK_KEY).toStringList());
    settings->endGroup();

    qCDebug(perspectivesLog) << "LOADED CHANGED DOCKS:" << d->m_persistentChangedDocks;
}

Perspective *DebuggerMainWindow::currentPerspective()
{
    return theMainWindow->d->m_currentPerspective;
}

void DebuggerMainWindow::savePersistentSettings() const
{
    // The current one might have active, non saved changes.
    if (Perspective *perspective = d->m_currentPerspective)
        perspective->d->saveLayout();

    QVariantHash states;
    qCDebug(perspectivesLog) << "PERSPECTIVE TYPES: " << d->m_lastTypePerspectiveStates.keys();
    for (auto it = d->m_lastTypePerspectiveStates.cbegin();
         it != d->m_lastTypePerspectiveStates.cend(); ++it) {
        const QString &type = it.key();
        const PerspectiveState &state = it.value();
        qCDebug(perspectivesLog) << "PERSPECTIVE TYPE " << type
                                 << " HAS STATE: " << !state.mainWindowState.isEmpty();
        QTC_ASSERT(!state.mainWindowState.isEmpty(), continue);
        states.insert(type, mapFromStore(state.toSettings()));
    }

    QtcSettings *settings = ICore::settings();
    settings->beginGroup(MAINWINDOW_KEY);
    settings->setValue(CHANGED_DOCK_KEY, QStringList(Utils::toList(d->m_persistentChangedDocks)));
    settings->setValue(STATE_KEY2, states);
    settings->setValue(SHOW_CENTRALWIDGET_KEY, isCentralWidgetShown());
    settings->endGroup();

    qCDebug(perspectivesLog) << "SAVED CHANGED DOCKS:" << d->m_persistentChangedDocks;
}

QWidget *DebuggerMainWindow::centralWidgetStack()
{
    return theMainWindow ? theMainWindow->d->m_centralWidgetStack : nullptr;
}

void DebuggerMainWindow::addSubPerspectiveSwitcher(QWidget *widget)
{
    widget->setVisible(false);
    StyleHelper::setPanelWidget(widget);
    d->m_subPerspectiveSwitcherLayout->addWidget(widget);
}

void DebuggerMainWindow::addPerspectiveMenu(QMenu *menu)
{
    if (!theMainWindow)
        return;
    for (Perspective *perspective : std::as_const(theMainWindow->d->m_perspectives)) {
        menu->addAction(perspective->d->m_name, perspective, [perspective] {
            if (auto subPerspective = Perspective::findPerspective(
                    perspective->d->m_lastActiveSubPerspectiveId))
                subPerspective->select();
            else
                perspective->select();
        });
    }
}

DebuggerMainWindow *DebuggerMainWindow::instance()
{
    return theMainWindow;
}

Perspective *Perspective::findPerspective(const QString &perspectiveId)
{
    QTC_ASSERT(theMainWindow, return nullptr);
    return Utils::findOr(theMainWindow->d->m_perspectives, nullptr,
                         [perspectiveId](Perspective *perspective) {
        return perspective && perspective->d->m_id == perspectiveId;
    });
}

bool Perspective::isCurrent() const
{
    return theMainWindow->d->m_currentPerspective == this;
}

QDockWidget *DebuggerMainWindowPrivate::dockForWidget(QWidget *widget) const
{
    QTC_ASSERT(widget, return nullptr);

    for (QDockWidget *dock : q->dockWidgets()) {
        if (dock->widget() == widget)
            return dock;
    }

    return nullptr;
}

void DebuggerMainWindowPrivate::resetCurrentPerspective()
{
    QTC_ASSERT(m_currentPerspective, return);
    cleanDocks();
    setCentralWidget(m_currentPerspective->d->m_centralWidget);
    q->showCentralWidget(true);
    m_currentPerspective->d->resetPerspective();
}

void DebuggerMainWindowPrivate::setCentralWidget(QWidget *widget)
{
    if (widget) {
        m_centralWidgetStack->addWidget(widget);
        q->showCentralWidgetAction()->setText(widget->windowTitle());
    } else {
        m_centralWidgetStack->addWidget(m_editorPlaceHolder);
        q->showCentralWidgetAction()->setText(Tr::tr("Editor"));
    }
}

void PerspectivePrivate::resetPerspective()
{
    showInnerToolBar();

    for (DockOperation &op : m_dockOperations) {
        if (!op.dock) {
            qCDebug(perspectivesLog) << "RESET UNUSED " << op.name();
        } else if (op.operationType == Perspective::Raise) {
            QTC_ASSERT(op.dock, qCDebug(perspectivesLog) << op.name(); continue);
            op.dock->raise();
        } else {
            op.setupLayout();
            op.dock->setVisible(op.visibleByDefault);
            theMainWindow->d->m_persistentChangedDocks.remove(op.name());
            qCDebug(perspectivesLog) << "SETTING " << op.name()
                                     << " TO ACTIVE: " << op.visibleByDefault;
        }
    }
}

void DockOperation::setupLayout()
{
    QTC_ASSERT(widget, return);
    QTC_ASSERT(operationType != Perspective::Raise, return);
    QTC_ASSERT(dock, return);

    QDockWidget *anchor = nullptr;
    if (anchorWidget)
        anchor = theMainWindow->d->dockForWidget(anchorWidget);
    else if (area == Qt::BottomDockWidgetArea)
        anchor = theMainWindow->d->m_toolBarDock;

    if (anchor) {
        switch (operationType) {
        case Perspective::AddToTab:
            theMainWindow->tabifyDockWidget(anchor, dock);
            break;
        case Perspective::SplitHorizontal:
            theMainWindow->splitDockWidget(anchor, dock, Qt::Horizontal);
            break;
        case Perspective::SplitVertical:
            theMainWindow->splitDockWidget(anchor, dock, Qt::Vertical);
            break;
        default:
            break;
        }
    } else {
        theMainWindow->addDockWidget(area, dock);
    }
}

void DockOperation::ensureDockExists()
{
    if (dock)
        return;

    dock = theMainWindow->addDockForWidget(widget);

    if (theMainWindow->restoreDockWidget(dock)) {
        qCDebug(perspectivesLog) << "RESTORED SUCCESSFULLY" << commandId;
    } else {
        qCDebug(perspectivesLog) << "COULD NOT RESTORE" << commandId;
        setupLayout();
    }

    toggleViewAction->setAction(dock->toggleViewAction());

    QObject::connect(dock->toggleViewAction(), &QAction::triggered,
                     dock->toggleViewAction(), [this] { recordVisibility(); });
}

bool DockOperation::changedByUser() const
{
    return theMainWindow->d->m_persistentChangedDocks.contains(name());
}

void DockOperation::recordVisibility()
{
    if (operationType != Perspective::Raise) {
        if (toggleViewAction->isChecked() == visibleByDefault)
            theMainWindow->d->m_persistentChangedDocks.remove(name());
        else
            theMainWindow->d->m_persistentChangedDocks.insert(name());
    }
    qCDebug(perspectivesLog) << "RECORDING DOCK VISIBILITY " << name()
                             << toggleViewAction->isChecked()
                             << theMainWindow->d->m_persistentChangedDocks;
}

int DebuggerMainWindowPrivate::indexInChooser(Perspective *perspective) const
{
    return perspective ? m_perspectiveChooser->findData(perspective->d->m_id) : -1;
}

void DebuggerMainWindowPrivate::updatePerspectiveChooserWidth()
{
    Perspective *perspective = m_currentPerspective;
    int index = indexInChooser(m_currentPerspective);
    if (index == -1) {
        perspective = Perspective::findPerspective(m_currentPerspective->d->m_parentPerspectiveId);
        if (perspective)
            index = indexInChooser(perspective);
    }

    if (index != -1) {
        m_perspectiveChooser->setCurrentIndex(index);

        const int contentWidth =
            m_perspectiveChooser->fontMetrics().horizontalAdvance(perspective->d->m_name);
        QStyleOptionComboBox option;
        option.initFrom(m_perspectiveChooser);
        const QSize sz(contentWidth, 1);
        const int width = m_perspectiveChooser->style()->sizeFromContents(
                    QStyle::CT_ComboBox, &option, sz).width();
        m_perspectiveChooser->setFixedWidth(width);
    }
}

void DebuggerMainWindowPrivate::cleanDocks()
{
    m_statusLabel->clear();

    for (QDockWidget *dock : q->dockWidgets()) {
        dock->setFloating(false);
        if (dock != m_toolBarDock)
            dock->setVisible(false);
    }
}

void PerspectivePrivate::depopulatePerspective()
{
    ICore::removeAdditionalContext(context());
    theMainWindow->d->m_centralWidgetStack
            ->removeWidget(m_centralWidget ? m_centralWidget : theMainWindow->d->m_editorPlaceHolder);

    theMainWindow->d->m_statusLabel->clear();

    qCDebug(perspectivesLog) << "DEPOPULATE PERSPECTIVE" << m_id;
    for (QDockWidget *dock : theMainWindow->dockWidgets()) {
        if (dock != theMainWindow->d->m_toolBarDock)
            dock->setVisible(false);
    }

    hideInnerToolBar();
}

void PerspectivePrivate::saveAsLastUsedPerspective()
{
    if (Perspective *parent = Perspective::findPerspective(m_parentPerspectiveId))
        parent->d->m_lastActiveSubPerspectiveId = m_id;
    else
        m_lastActiveSubPerspectiveId.clear();

    const QString &lastKey = m_parentPerspectiveId.isEmpty() ? m_id : m_parentPerspectiveId;
    qCDebug(perspectivesLog) << "SAVE AS LAST USED PERSPECTIVE" << lastKey;
    ICore::settings()->setValue(LAST_PERSPECTIVE_KEY, lastKey);
}

void PerspectivePrivate::populatePerspective()
{
    showInnerToolBar();

    if (m_centralWidget) {
        theMainWindow->d->m_centralWidgetStack->addWidget(m_centralWidget);
        theMainWindow->showCentralWidgetAction()->setText(m_centralWidget->windowTitle());
    } else {
        theMainWindow->d->m_centralWidgetStack->addWidget(theMainWindow->d->m_editorPlaceHolder);
        theMainWindow->showCentralWidgetAction()->setText(Tr::tr("Editor"));
    }

    ICore::addAdditionalContext(context());

    restoreLayout();

    if (!m_centralWidget) {
        if (IEditor *editor = EditorManager::currentEditor())
            editor->widget()->setFocus();
    }
}

// Perspective

Perspective::Perspective(const QString &id, const QString &name,
                         const QString &parentPerspectiveId,
                         const QString &settingsId)
    : d(new PerspectivePrivate)
{
    d->m_id = id;
    d->m_name = name;
    d->m_parentPerspectiveId = parentPerspectiveId;
    d->m_settingsId = settingsId;

    DebuggerMainWindow::ensureMainWindowExists();
    theMainWindow->d->registerPerspective(this);

    d->m_innerToolBar = new QWidget;
    d->m_innerToolBar->setVisible(false);
    theMainWindow->d->m_innerToolsLayout->addWidget(d->m_innerToolBar);

    d->m_innerToolBarLayout = new QHBoxLayout(d->m_innerToolBar);
    d->m_innerToolBarLayout->setContentsMargins(0, 0, 0, 0);
    d->m_innerToolBarLayout->setSpacing(0);
}

Perspective::~Perspective()
{
    if (theMainWindow) {
        delete d->m_innerToolBar;
        d->m_innerToolBar = nullptr;
    }
    delete d;
}

void Perspective::setCentralWidget(QWidget *centralWidget)
{
    QTC_ASSERT(d->m_centralWidget == nullptr, return);
    d->m_centralWidget = centralWidget;
}

QString Perspective::id() const
{
    return d->m_id;
}

QString Perspective::parentPerspectiveId() const
{
    return d->m_parentPerspectiveId;
}

QString Perspective::name() const
{
    return d->m_name;
}

void Perspective::setAboutToActivateCallback(const Perspective::Callback &cb)
{
    d->m_aboutToActivateCallback = cb;
}

void Perspective::setEnabled(bool enabled)
{
    QTC_ASSERT(theMainWindow, return);
    const int index = theMainWindow->d->indexInChooser(this);
    QTC_ASSERT(index != -1, return);
    auto model = qobject_cast<QStandardItemModel*>(theMainWindow->d->m_perspectiveChooser->model());
    QTC_ASSERT(model, return);
    QStandardItem *item = model->item(index, 0);
    item->setFlags(enabled ? item->flags() | Qt::ItemIsEnabled : item->flags() & ~Qt::ItemIsEnabled );
}

QToolButton *PerspectivePrivate::setupToolButton(QAction *action)
{
    QTC_ASSERT(action, return nullptr);
    auto toolButton = new QToolButton(m_innerToolBar);
    StyleHelper::setPanelWidget(toolButton);
    toolButton->setDefaultAction(action);
    toolButton->setToolTip(action->toolTip());
    m_innerToolBarLayout->addWidget(toolButton);
    return toolButton;
}

void Perspective::addToolBarAction(QAction *action)
{
    QTC_ASSERT(action, return);
    d->setupToolButton(action);
}

void Perspective::addToolBarAction(OptionalAction *action)
{
    QTC_ASSERT(action, return);
    action->m_toolButton = d->setupToolButton(action);
}

void Perspective::addToolBarWidget(QWidget *widget)
{
    QTC_ASSERT(widget, return);
    // QStyle::polish is called before it is added to the toolbar, explicitly make it a panel widget
    StyleHelper::setPanelWidget(widget);
    widget->setParent(d->m_innerToolBar);
    d->m_innerToolBarLayout->addWidget(widget);
}

void Perspective::useSubPerspectiveSwitcher(QWidget *widget)
{
    d->m_switcher = widget;
}

void Perspective::addToolbarSeparator()
{
    d->m_innerToolBarLayout->addWidget(new StyledSeparator(d->m_innerToolBar));
}

void Perspective::registerNextPrevShortcuts(QAction *next, QAction *prev)
{
    static const char nextId[] = "Analyzer.nextitem";
    static const char prevId[] = "Analyzer.previtem";

    next->setText(Tr::tr("Next Item"));
    Command * const nextCmd = ActionManager::registerAction(next, nextId,
                                                            Context(Id::fromString(id())));
    nextCmd->augmentActionWithShortcutToolTip(next);
    prev->setText(Tr::tr("Previous Item"));
    Command * const prevCmd = ActionManager::registerAction(prev, prevId,
                                                            Context(Id::fromString(id())));
    prevCmd->augmentActionWithShortcutToolTip(prev);
}

QWidget *Perspective::centralWidget() const
{
    return d->m_centralWidget;
}

Context PerspectivePrivate::context() const
{
    return Context(Id::fromName(m_id.toUtf8()));
}

PerspectivePrivate::~PerspectivePrivate()
{
    for (const DockOperation &op : std::as_const(m_dockOperations))
        delete op.widget;
}

void PerspectivePrivate::showInnerToolBar()
{
    m_innerToolBar->setVisible(true);
    if (m_switcher)
        m_switcher->setVisible(true);
}

void PerspectivePrivate::hideInnerToolBar()
{
    QTC_ASSERT(m_innerToolBar, return);
    m_innerToolBar->setVisible(false);
    if (m_switcher)
        m_switcher->setVisible(false);
}

void Perspective::addWindow(QWidget *widget,
                            Perspective::OperationType type,
                            QWidget *anchorWidget,
                            bool visibleByDefault,
                            Qt::DockWidgetArea area)
{
    QTC_ASSERT(widget, return);
    DockOperation op;
    op.widget = widget;
    op.operationType = type;
    op.anchorWidget = anchorWidget;
    op.visibleByDefault = visibleByDefault;
    op.area = area;

    if (op.operationType != Perspective::Raise) {
        qCDebug(perspectivesLog) << "CREATING DOCK " << op.name()
                                 << "DEFAULT: " << op.visibleByDefault;
        op.commandId = Id("Dock.").withSuffix(op.name());

        op.toggleViewAction = new ProxyAction(this);
        op.toggleViewAction->setText(widget->windowTitle());

        Command *cmd = ActionManager::registerAction(op.toggleViewAction, op.commandId, d->context());
        cmd->setAttribute(Command::CA_Hide);
        ActionManager::actionContainer(Core::Constants::M_VIEW_VIEWS)->addAction(cmd);
    }

    d->m_dockOperations.append(op);
}

void Perspective::destroy()
{
    theMainWindow->d->destroyPerspective(this);
}

void Perspective::rampDownAsCurrent()
{
    QTC_ASSERT(this == theMainWindow->d->m_currentPerspective, return);
    d->saveLayout();
    d->depopulatePerspective();
    theMainWindow->d->setCurrentPerspective(nullptr);

    Debugger::Internal::EngineManager::updatePerspectives();
}

void Perspective::rampUpAsCurrent()
{
    if (d->m_aboutToActivateCallback)
        d->m_aboutToActivateCallback();

    QTC_ASSERT(theMainWindow->d->m_currentPerspective == nullptr, return);
    theMainWindow->d->setCurrentPerspective(this);
    QTC_ASSERT(theMainWindow->d->m_currentPerspective == this, return);

    theMainWindow->showCentralWidget(true);

    d->populatePerspective();

    theMainWindow->d->updatePerspectiveChooserWidth();

    d->saveAsLastUsedPerspective();

    Debugger::Internal::EngineManager::updatePerspectives();
}

void Perspective::select()
{
    Debugger::Internal::EngineManager::activateDebugMode();

    if (theMainWindow->d->m_currentPerspective == this)
        return;

    if (theMainWindow->d->m_currentPerspective)
        theMainWindow->d->m_currentPerspective->rampDownAsCurrent();
    QTC_CHECK(theMainWindow->d->m_currentPerspective == nullptr);

    rampUpAsCurrent();
}

void PerspectivePrivate::restoreLayout()
{
    qCDebug(perspectivesLog) << "RESTORE LAYOUT FOR " << m_id << settingsId();
    PerspectiveState state = theMainWindow->d->m_lastPerspectiveStates.value(m_id);
    if (!state.hasWindowState()) {
        qCDebug(perspectivesLog) << "PERSPECTIVE STATE NOT AVAILABLE BY FULL ID.";
        state = theMainWindow->d->m_lastTypePerspectiveStates.value(settingsId());
        if (state.hasWindowState()) {
            qCDebug(perspectivesLog) << "PERSPECTIVE STATE NOT AVAILABLE BY PERSPECTIVE TYPE";
        } else {
            qCDebug(perspectivesLog) << "PERSPECTIVE STATE AVAILABLE BY PERSPECTIVE TYPE.";
        }
    } else {
        qCDebug(perspectivesLog) << "PERSPECTIVE STATE AVAILABLE BY FULL ID.";
    }

    // The order is important here: While QMainWindow can restore layouts with
    // not-existing docks (some placeholders are used internally), later
    // replacements with restoreDockWidget(dock) trigger a re-layout, resulting
    // in different sizes. So make sure all docks exist first before restoring state.

    qCDebug(perspectivesLog) << "PERSPECTIVE" << m_id << "RESTORING LAYOUT FROM " << settingsId();
    for (DockOperation &op : m_dockOperations) {
        if (op.operationType != Perspective::Raise) {
            op.ensureDockExists();
            QTC_ASSERT(op.dock, continue);
            const bool active = op.visibleByDefault != op.changedByUser();
            op.dock->setVisible(active);
            qCDebug(perspectivesLog) << "RESTORE DOCK " << op.name() << "ACTIVE: " << active
                                     << (active == op.visibleByDefault ? "DEFAULT USER" : "*** NON-DEFAULT USER");
        }
    }

    if (!state.hasWindowState()) {
        qCDebug(perspectivesLog) << "PERSPECTIVE " << m_id << "RESTORE NOT POSSIBLE, NO STORED STATE";
    } else {
        const bool result = state.restoreWindowState(theMainWindow);
        qCDebug(perspectivesLog) << "PERSPECTIVE " << m_id << "RESTORED, SUCCESS: " << result;
    }

    for (DockOperation &op : m_dockOperations) {
        if (op.operationType != Perspective::Raise) {
            QTC_ASSERT(op.dock, continue);
            for (QTreeView *tv : op.dock->findChildren<QTreeView *>()) {
                if (tv->property(PerspectiveState::savesHeaderKey()).toBool()) {
                    const QByteArray s = state.headerViewStates.value(op.name()).toByteArray();
                    if (!s.isEmpty())
                        tv->header()->restoreState(s);
                }
            }
        }
    }
}

void PerspectivePrivate::saveLayout()
{
    qCDebug(perspectivesLog) << "PERSPECTIVE" << m_id << "SAVE LAYOUT TO " << settingsId();
    PerspectiveState state;
    state.mainWindowState = theMainWindow->saveSettings();
    for (DockOperation &op : m_dockOperations) {
        if (op.operationType != Perspective::Raise) {
            QTC_ASSERT(op.dock, continue);
            for (QTreeView *tv : op.dock->findChildren<QTreeView *>()) {
                if (tv->property(PerspectiveState::savesHeaderKey()).toBool()) {
                    if (QHeaderView *hv = tv->header())
                        state.headerViewStates.insert(op.name(), hv->saveState());
                }
            }
        }
    }
    theMainWindow->d->m_lastPerspectiveStates.insert(m_id, state);
    theMainWindow->d->m_lastTypePerspectiveStates.insert(settingsId(), state);
}

QString PerspectivePrivate::settingsId() const
{
    return m_settingsId.isEmpty() ? m_id : m_settingsId;
}

// ToolbarAction

OptionalAction::OptionalAction(const QString &text)
    : QAction(text)
{
}

OptionalAction::~OptionalAction()
{
    delete m_toolButton;
}

void OptionalAction::setVisible(bool on)
{
    QAction::setVisible(on);
    if (m_toolButton)
        m_toolButton->setVisible(on);
}

void OptionalAction::setToolButtonStyle(Qt::ToolButtonStyle style)
{
    QTC_ASSERT(m_toolButton, return);
    m_toolButton->setToolButtonStyle(style);
}

const char *PerspectiveState::savesHeaderKey()
{
    return "SavesHeader";
}

bool PerspectiveState::hasWindowState() const
{
    return !mainWindowState.isEmpty();
}

bool PerspectiveState::restoreWindowState(FancyMainWindow * mainWindow)
{
    if (!mainWindowState.isEmpty())
        return mainWindow->restoreSettings(mainWindowState);
    return false;
}

const char kMainWindowStateKey[] = "MainWindow";
const char kHeaderViewStatesKey[] = "HeaderViewStates";

Store PerspectiveState::toSettings() const
{
    Store result;
    result.insert(kMainWindowStateKey, QVariant::fromValue(mainWindowState));
    result.insert(kHeaderViewStatesKey, QVariant::fromValue(headerViewStates));
    return result;
}
PerspectiveState PerspectiveState::fromSettings(const Store &settings)
{
    PerspectiveState state;
    state.mainWindowState = settings.value(kMainWindowStateKey).value<Store>();
    state.headerViewStates = settings.value(kHeaderViewStatesKey).value<QVariantHash>();
    return state;
}

} // Utils
