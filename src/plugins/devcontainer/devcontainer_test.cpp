// Copyright (C) 2025 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "devcontainerplugin_constants.h"

#include <coreplugin/messagemanager.h>

#include <devcontainer/devcontainer.h>

#include <projectexplorer/kitmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/projectmanager.h>

#include <utils/algorithm.h>
#include <utils/filepath.h>
#include <utils/infobar.h>
#include <utils/mimeutils.h>
#include <utils/qtcprocess.h>

#include <coreplugin/icore.h>

#include <QTest>
#include <QSignalSpy>

using namespace Utils;

namespace DevContainer::Internal {

static bool testDocker(const FilePath &executable)
{
    Process p;
    p.setCommand({executable, {"info", "--format", "{{.OSType}}"}});
    p.runBlocking();
    const QString platform = p.cleanedStdOut().trimmed();
    return p.result() == ProcessResult::FinishedWithSuccess && platform == "linux";
}

static bool testDockerMount(const FilePath &executable, const FilePath &testDir)
{
    Process p;
    p.setCommand(
        {executable,
         {"run",
          "--rm",
          "--mount",
          "type=bind,source=" + testDir.path() + ",target=/mnt/test",
          "alpine:latest",
          "ls",
          "/mnt/test"}});
    p.runBlocking();
    if (p.result() != ProcessResult::FinishedWithSuccess) {
        qWarning() << "Docker mount test failed:" << p.verboseExitMessage();
        return false;
    }
    return p.result() == ProcessResult::FinishedWithSuccess;
}

class Tests : public QObject
{
    Q_OBJECT

    const FilePath testData{TESTDATA};

signals:
    void deviceUpDone();

private slots:
    void initTestCase()
    {
        if (!testDocker("docker"))
            QSKIP("Docker is not set up correctly, skipping tests.");

        if (!testDockerMount("docker", testData))
            QSKIP("Docker mount test failed, skipping tests.");

        Core::MessageManager::writeDisrupting("Starting DevContainer tests...");
    }

    void testSimpleProject()
    {
        const auto cmakelists = testData / "simpleproject" / "CMakeLists.txt";
        ProjectExplorer::OpenProjectResult opr
            = ProjectExplorer::ProjectExplorerPlugin::openProject(cmakelists);

        QVERIFY(opr);

        QSignalSpy deviceAddedSpy(this, &Tests::deviceUpDone);

        InstanceConfig instanceConfig;
        instanceConfig.configFilePath = testData / "simpleproject" / ".devcontainer"
                                        / "devcontainer.json";
        instanceConfig.workspaceFolder = opr.project()->projectDirectory();

        const auto infoBarEntryId = Utils::Id::fromString(QString(
            "DevContainer.Instantiate.InfoBar." + instanceConfig.workspaceFolder.toUrlishString()));

        Utils::InfoBar *infoBar = Core::ICore::popupInfoBar();
        QVERIFY(infoBar->containsInfo(infoBarEntryId));
        Utils::InfoBarEntry entry
            = Utils::findOrDefault(infoBar->entries(), [infoBarEntryId](const Utils::InfoBarEntry &e) {
                  return e.id() == infoBarEntryId;
              });

        QCOMPARE(entry.id(), infoBarEntryId);
        QCOMPARE(entry.buttons().size(), 1);
        auto yesButton = entry.buttons().first();

        // Trigger loading the DevContainer instance
        yesButton.callback();

        using namespace std::chrono_literals;
        QVERIFY(deviceAddedSpy.wait(
            std::chrono::duration_cast<std::chrono::milliseconds>(1min).count()));

        FilePath expectedRootPath = FilePath::fromParts(
            Constants::DEVCONTAINER_FS_SCHEME, instanceConfig.devContainerId(), u"/");
        QVERIFY(expectedRootPath.exists());
        QVERIFY(!infoBar->containsInfo(infoBarEntryId));

        FilePath expectedLibExecMountPoint = FilePath::fromParts(
            Constants::DEVCONTAINER_FS_SCHEME,
            instanceConfig.devContainerId(),
            u"/custom/libexec/mnt/point");
        QVERIFY(expectedLibExecMountPoint.exists());
        QVERIFY(expectedLibExecMountPoint.isReadableDir());

        FilePath expectedWorkspaceFolder = FilePath::fromParts(
            Constants::DEVCONTAINER_FS_SCHEME,
            instanceConfig.devContainerId(),
            u"/custom/workspace");
        QVERIFY(expectedWorkspaceFolder.exists());
        QVERIFY(expectedWorkspaceFolder.isReadableDir());

        FilePath expectedMainCpp = expectedWorkspaceFolder / "main.cpp";
        QVERIFY(expectedMainCpp.exists());
        QVERIFY(expectedMainCpp.isReadableFile());

        QCOMPARE(
            expectedRootPath.withNewMappedPath(testData / "simpleproject" / "main.cpp"),
            expectedMainCpp);
        QVERIFY(expectedRootPath.ensureReachable(expectedMainCpp));
        QCOMPARE(expectedMainCpp.localSource(), testData / "simpleproject" / "main.cpp");

        ProjectExplorer::ProjectManager::removeProject(opr.project());
        QVERIFY(!expectedRootPath.exists());
    }

    void testWithKit()
    {
        const auto cmakelists = testData / "withkit" / "CMakeLists.txt";
        ProjectExplorer::OpenProjectResult opr
            = ProjectExplorer::ProjectExplorerPlugin::openProject(cmakelists);

        QVERIFY(opr);

        QSignalSpy deviceAddedSpy(this, &Tests::deviceUpDone);

        InstanceConfig instanceConfig;
        instanceConfig.configFilePath = testData / "withkit" / ".devcontainer"
                                        / "devcontainer.json";
        instanceConfig.workspaceFolder = opr.project()->projectDirectory();

        const auto infoBarEntryId = Utils::Id::fromString(QString(
            "DevContainer.Instantiate.InfoBar." + instanceConfig.workspaceFolder.toUrlishString()));

        Utils::InfoBar *infoBar = Core::ICore::popupInfoBar();
        QVERIFY(infoBar->containsInfo(infoBarEntryId));
        Utils::InfoBarEntry entry
            = Utils::findOrDefault(infoBar->entries(), [infoBarEntryId](const Utils::InfoBarEntry &e) {
                  return e.id() == infoBarEntryId;
              });

        QCOMPARE(entry.id(), infoBarEntryId);
        QCOMPARE(entry.buttons().size(), 1);
        auto yesButton = entry.buttons().first();

        // Trigger loading the DevContainer instance
        yesButton.callback();

        using namespace std::chrono_literals;
        QVERIFY(deviceAddedSpy.wait(
            std::chrono::duration_cast<std::chrono::milliseconds>(10min).count()));

        FilePath expectedRootPath
            = FilePath::fromParts(u"devcontainer", instanceConfig.devContainerId(), u"/");
        QVERIFY(expectedRootPath.exists());
        QVERIFY(!infoBar->containsInfo(infoBarEntryId));

        auto kit1 = Utils::findOrDefault(
            ProjectExplorer::KitManager::instance()->kits(),
            [](ProjectExplorer::Kit *k) { return k->displayName() == QLatin1String("DevKit1"); });
        QVERIFY(kit1);
        auto kit2 = Utils::findOrDefault(
            ProjectExplorer::KitManager::instance()->kits(),
            [](ProjectExplorer::Kit *k) { return k->displayName() == QLatin1String("DevKit2"); });
        QVERIFY(kit2);
    }
};

QObject *createDevcontainerTest()
{
    return new Tests;
}

} // namespace DevContainer::Internal

#include "devcontainer_test.moc"
