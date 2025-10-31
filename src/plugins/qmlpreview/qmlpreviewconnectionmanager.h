// Copyright (C) 2019 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "qmlpreviewplugin.h"
#include "qmlpreviewclient.h"
#include "qmldebugtranslationclient.h"
#include "qmlpreviewfileontargetfinder.h"

#include <qmldebug/qmldebugconnectionmanager.h>

#include <QtTaskTree/QTaskTree>

#include <utils/fileinprojectfinder.h>
#include <utils/filesystemwatcher.h>

namespace ProjectExplorer { class BuildConfiguration; }

namespace QmlPreview {

class QmlPreviewConnectionManager : public QmlDebug::QmlDebugConnectionManager
{
    Q_OBJECT
public:
    virtual ~QmlPreviewConnectionManager();

    explicit QmlPreviewConnectionManager(QObject *parent = nullptr);
    void setBuildConfiguration(ProjectExplorer::BuildConfiguration *bc);
    void setFileLoader(QmlPreviewFileLoader fileLoader);
    void setFileClassifier(QmlPreviewFileClassifier fileClassifier);
    void setFpsHandler(QmlPreviewFpsHandler fpsHandler);
    void setQmlDebugTranslationClientCreator(QmlDebugTranslationClientFactoryFunction creator);

signals:
    void loadFile(const QString &filename, const QString &changedFile, const QByteArray &contents);
    void zoom(float zoomFactor);
    void language(const QString &locale);
    void rerun();
    void restart();

protected:
    void createClients() override;
    void destroyClients() override;

private:
    void createPreviewClient();
    void createDebugTranslationClient();
    void clearClient(QObject *client);
    QUrl findValidI18nDirectoryAsUrl(const QString &locale);
    Utils::FileInProjectFinder m_projectFileFinder;
    QmlPreviewFileOnTargetFinder m_targetFileFinder;
    QPointer<QmlPreviewClient> m_qmlPreviewClient;
    std::unique_ptr<QmlDebugTranslationClient> m_qmlDebugTranslationClient;
    Utils::FileSystemWatcher m_fileSystemWatcher;
    QUrl m_lastLoadedUrl;
    QString m_lastUsedLanguage;
    QmlPreviewFileLoader m_fileLoader = nullptr;
    QmlPreviewFileClassifier m_fileClassifier = nullptr;
    QmlPreviewFpsHandler m_fpsHandler = nullptr;
    QmlDebugTranslationClientFactoryFunction m_createDebugTranslationClientMethod;
};

class QmlPreviewConnectionManagerTaskAdapter final
{
public:
    ~QmlPreviewConnectionManagerTaskAdapter()
    {
        if (m_task)
            m_task->disconnectFromServer();
    }
    void operator()(QmlPreviewConnectionManager *task, QTaskInterface *iface)
    {
        m_task = task;
        QObject::connect(task, &QmlPreviewConnectionManager::connectionClosed, iface, [iface] {
            iface->reportDone(QtTaskTree::DoneResult::Success);
        }, Qt::SingleShotConnection);
        QObject::connect(task, &QmlPreviewConnectionManager::connectionFailed, iface, [iface] {
            iface->reportDone(QtTaskTree::DoneResult::Error);
        }, Qt::SingleShotConnection);
        task->connectToServer();
    }

private:
    QmlPreviewConnectionManager *m_task = nullptr;
};

using QmlPreviewConnectionManagerTask
    = QCustomTask<QmlPreviewConnectionManager, QmlPreviewConnectionManagerTaskAdapter>;

} // namespace QmlPreview
