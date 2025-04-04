// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "ifindfilter.h"

#include "../coreicons.h"
#include "../coreplugintr.h"

#include <utils/qtcsettings.h>

#include <QApplication>
#include <QKeySequence>
#include <QPainter>
#include <QPixmap>

using namespace Utils;

/*!
    \class Core::IFindFilter
    \inheaderfile coreplugin/find/ifindfilter.h
    \inmodule QtCreator

    \brief The IFindFilter class is the base class for find implementations
    that are invoked by selecting \uicontrol Edit > \uicontrol {Find/Replace} >
    \uicontrol {Advanced Find}.

    Implementations of this class add an additional \uicontrol Scope to the \uicontrol {Advanced
    Find} dialog. That can be any search that requires the user to provide
    a text based search term (potentially with find flags like
    searching case sensitively or using regular expressions). Existing
    scopes are \uicontrol {All Projects} that searches from all files in all projects
    and \uicontrol {Files in File System} where the user provides a directory and file
    patterns to search.

    \image qtcreator-search-reg-exp.webp {Search Results view with search criteria}

    To make your find scope available to the user, you need to implement this
    class, and register an instance of your subclass in the plugin manager.

    A common way to present the search results to the user, is to use the
    shared \uicontrol{Search Results} pane.

    \image qtcreator-search-results-reg-exp.webp {Search Results view with search results}

    If you want to implement a find filter that is doing a file based text
    search, you should use \l Core::BaseTextFind, which already implements all
    the details for this kind of search, only requiring you to provide an
    iterator over the file names of the files that should be searched.

    If you want to implement a more specialized find filter, you need to:
    \list
        \li Start your search in a separate thread
        \li Make this known to the Core::ProgressManager, for a progress bar
           and the ability to cancel the search
        \li Interface with the shared \uicontrol{Search Results} pane, to show
           the search results, handle the event that the user click on one
           of the search result items, and possible handle a global replace
           of all or some of the search result items.
    \endlist

    Luckily QtConcurrent and the search results pane provide the frameworks
    that make it relatively easy to implement,
    while ensuring a common way for the user.

    The common pattern is roughly to first implement the actual search
    within a QtConcurrent based function. That is
    a function that takes a \c{QFutureInterface<MySearchResult> &future}
    as the first parameter and the other information needed for the search
    as additional parameters. It should set useful progress information
    on the QFutureInterface, regularly check for \c{future.isPaused()}
    and \c{future.isCanceled()}, and report the search results
    (possibly in chunks) via \c{future.reportResult}.

    In the find filter's \c find() or \c replaceAll() function, get the shared
    \uicontrol{Search Results} window, initiate a new search and connect the
    signals for handling selection of results and the replace action
    (see the Core::SearchResultWindow class for details).
    Start your search implementation via the corresponding QtConcurrent
    functions. Add the returned QFuture object to the Core::ProgressManager.
    Use a QFutureWatcher on the returned QFuture object to receive a signal
    when your search implementation reports search results, and add these
    to the shared \uicontrol{Search Results} window.
*/

/*!
    \fn QString Core::IFindFilter::id() const
    Returns the unique string identifier for this find filter.

    Usually should be something like "MyPlugin.MyFindFilter".
*/

/*!
    \fn QString Core::IFindFilter::displayName() const
    Returns the name of the find filter or scope as presented to the user.

    This is the name that appears in the scope selection combo box, for example.
    Always return a translatable string. That is, use \c {Tr::tr()} for the return
    value.
*/

/*!
    \fn bool Core::IFindFilter::isEnabled() const
    Returns whether the user should be able to select this find filter
    at the moment.

    This is used for the \uicontrol {Current Projects} scope, for example. If the user
    has not
    opened a project, the scope is disabled.

    \sa enabledChanged()
*/

/*!
    \fn bool Core::IFindFilter::isValid() const
    Returns whether the find filter is valid.

    \sa validChanged()
*/

/*!
    \fn bool Core::IFindFilter::isReplaceSupported() const
    Returns whether the find filter supports search and replace.

    The default value is false, override this function to return \c true, if
    your find filter supports global search and replace.
*/

/*!
    \fn bool Core::IFindFilter::showSearchTermInput() const
    Returns whether the find filter wants to show the search term line edit.

    The default value is \c true, override this function to return \c false, if
    your find filter does not want to show the search term line edit.
*/

/*!
    \fn void Core::IFindFilter::findAll(const QString &txt, Utils::FindFlags findFlags)
    This function is called when the user selected this find scope and
    initiated a search.

    You should start a thread which actually performs the search for \a txt
    using the given \a findFlags
    (add it to Core::ProgressManager for a progress bar) and presents the
    search results to the user (using the \uicontrol{Search Results} output pane).
    For more information, see the descriptions of this class,
    Core::ProgressManager, and Core::SearchResultWindow.

    \sa replaceAll()
    \sa Core::ProgressManager
    \sa Core::SearchResultWindow
*/

/*!
    \fn void Core::IFindFilter::replaceAll(const QString &txt, Utils::FindFlags findFlags)
    Override this function if you want to support search and replace.

    This function is called when the user selected this find scope and
    initiated a search and replace.
    The default implementation does nothing.

    You should start a thread which actually performs the search for \a txt
    using the given \a findFlags
    (add it to Core::ProgressManager for a progress bar) and presents the
    search results to the user (using the \uicontrol{Search Results} output pane).
    For more information see the descriptions of this class,
    Core::ProgressManager, and Core::SearchResultWindow.

    \sa findAll()
    \sa Core::ProgressManager
    \sa Core::SearchResultWindow
*/

/*!
    \fn QWidget *Core::IFindFilter::createConfigWidget()
    Returns a widget that contains additional controls for options
    for this find filter.

    The widget will be shown below the common options in the \uicontrol {Advanced Find}
    dialog. It will be reparented and deleted by the find plugin.
*/

/*!
    \fn void Core::IFindFilter::enabledChanged(bool enabled)

    This signal is emitted when the \a enabled state of this find filter
    changes.
*/

/*!
    \fn void Core::IFindFilter::validChanged(bool valid)

    This signal is emitted when the \a valid state of this find filter changes.
*/

/*!
    \fn void Core::IFindFilter::displayNameChanged()

    This signal is emitted when the display name of this find filter changes.
*/

namespace Core {

static QList<IFindFilter *> g_findFilters;

/*!
    \internal
*/
IFindFilter::IFindFilter()
{
    g_findFilters.append(this);
}

/*!
    \internal
*/
IFindFilter::~IFindFilter()
{
    g_findFilters.removeOne(this);
}

/*!
    Returns a list of find filters.
*/
const QList<IFindFilter *> IFindFilter::allFindFilters()
{
    return g_findFilters;
}

/*!
    Returns the shortcut that can be used to open the advanced find
    dialog with this filter or scope preselected.

    Usually return an empty shortcut here, the user can still choose and
    assign a specific shortcut to this find scope via the preferences.
*/
QKeySequence IFindFilter::defaultShortcut() const
{
    return QKeySequence();
}

/*!
    Returns the find flags, like whole words or regular expressions,
    that this find filter supports.

    Depending on the returned value, the default find option widgets are
    enabled or disabled.
    The default is Utils::FindCaseSensitively, Utils::FindRegularExpression
    and Uitls::FindWholeWords.
*/
FindFlags IFindFilter::supportedFindFlags() const
{
    return FindCaseSensitively
         | FindRegularExpression
         | FindWholeWords;
}

/*!
    Returns a Store with the find filter's settings to store
    in the session. Default values should not be saved.
    The default implementation returns an empty store.

    \sa restore()
*/
Store IFindFilter::save() const
{
    return {};
}

/*!
    Restores the find filter's settings from the Store \a s.
    Settings that are not present in the store should be reset to
    the default.
    The default implementation does nothing.

    \sa save()
*/
void IFindFilter::restore([[maybe_unused]] const Utils::Store &s) {}

/*!
    Called at shutdown to write the state of the additional options
    for this find filter to the \a settings.

    \deprecated [14.0] Implement save() instead.
*/
void IFindFilter::writeSettings(Utils::QtcSettings *settings)
{
    settings->remove(settingsKey()); // make sure defaults are removed
    storeToSettings(settingsKey(), settings, save());
}

/*!
    Called at startup to read the state of the additional options
    for this find filter from the \a settings.

    \deprecated [14.0] Implement restore() instead.
*/
void IFindFilter::readSettings(Utils::QtcSettings *settings)
{
    restore(storeFromSettings(settingsKey(), settings));
}

/*!
    \internal
    \deprecated [14.0]
*/
QByteArray IFindFilter::settingsKey() const
{
    return id().toUtf8();
}

/*!
    Returns icons for the find flags \a flags.
*/
QPixmap IFindFilter::pixmapForFindFlags(FindFlags flags)
{
    static const QPixmap casesensitiveIcon = Icons::FIND_CASE_INSENSITIVELY.pixmap();
    static const QPixmap regexpIcon = Icons::FIND_REGEXP.pixmap();
    static const QPixmap wholewordsIcon = Icons::FIND_WHOLE_WORD.pixmap();
    static const QPixmap preservecaseIcon = Icons::FIND_PRESERVE_CASE.pixmap();
    bool casesensitive = flags & FindCaseSensitively;
    bool wholewords = flags & FindWholeWords;
    bool regexp = flags & FindRegularExpression;
    bool preservecase = flags & FindPreserveCase;
    int width = 0;
    if (casesensitive)
        width += casesensitiveIcon.width();
    if (wholewords)
        width += wholewordsIcon.width();
    if (regexp)
        width += regexpIcon.width();
    if (preservecase)
        width += preservecaseIcon.width();
    if (width == 0)
        return QPixmap();
    QPixmap pixmap(QSize(width, casesensitiveIcon.height()));
    pixmap.fill(QColor(0xff, 0xff, 0xff, 0x28)); // Subtile contrast for dark themes
    const int dpr = int(qApp->devicePixelRatio());
    pixmap.setDevicePixelRatio(dpr);
    QPainter painter(&pixmap);
    int x = 0;
    if (casesensitive) {
        painter.drawPixmap(x, 0, casesensitiveIcon);
        x += casesensitiveIcon.width() / dpr;
    }
    if (wholewords) {
        painter.drawPixmap(x, 0, wholewordsIcon);
        x += wholewordsIcon.width() / dpr;
    }
    if (regexp) {
        painter.drawPixmap(x, 0, regexpIcon);
        x += regexpIcon.width() / dpr;
    }
    if (preservecase)
        painter.drawPixmap(x, 0, preservecaseIcon);
    return pixmap;
}

/*!
    Returns descriptive text labels for the find flags \a flags.
*/
QString IFindFilter::descriptionForFindFlags(FindFlags flags)
{
    QStringList flagStrings;
    if (flags & FindCaseSensitively)
        flagStrings.append(Tr::tr("Case sensitive"));
    if (flags & FindWholeWords)
        flagStrings.append(Tr::tr("Whole words"));
    if (flags & FindRegularExpression)
        flagStrings.append(Tr::tr("Regular expressions"));
    if (flags & FindPreserveCase)
        flagStrings.append(Tr::tr("Preserve case"));
    QString description = Tr::tr("Flags: %1");
    if (flagStrings.isEmpty())
        description = description.arg(Tr::tr("None", "No find flags"));
    else
        description = description.arg(flagStrings.join(Tr::tr(", ")));
    return description;
}

} // namespace Core
