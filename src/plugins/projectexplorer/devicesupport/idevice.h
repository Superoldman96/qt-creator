// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "../projectexplorer_export.h"
#include "idevicefwd.h"

#include <solutions/tasking/tasktree.h>

#include <utils/aspects.h>
#include <utils/filepath.h>
#include <utils/hostosinfo.h>
#include <utils/id.h>
#include <utils/portlist.h>
#include <utils/result.h>
#include <utils/store.h>

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QList>
#include <QObject>
#include <QUrl>

#include <functional>

QT_BEGIN_NAMESPACE
class QPixmap;
class QWidget;
QT_END_NAMESPACE

namespace Utils {
class CommandLine;
class DeviceFileAccess;
class Environment;
class PortList;
class Port;
class Process;
class ProcessInterface;
} // Utils

namespace ProjectExplorer {

class FileTransferInterface;
class FileTransferSetupData;
class Kit;
class SshParameters;
class SshParametersAspectContainer;
class Target;
class Task;

namespace Internal { class IDevicePrivate; }

class IDeviceWidget;
class DeviceTester;

class PROJECTEXPLORER_EXPORT DeviceProcessSignalOperation : public QObject
{
    Q_OBJECT
public:
    using Ptr = std::shared_ptr<DeviceProcessSignalOperation>;

    virtual void killProcess(qint64 pid) = 0;
    virtual void killProcess(const QString &filePath) = 0;
    virtual void interruptProcess(qint64 pid) = 0;

    void setDebuggerCommand(const Utils::FilePath &cmd);

signals:
    // If the error message is empty the operation was successful
    void finished(const Utils::Result<> &result);

protected:
    explicit DeviceProcessSignalOperation();

    Utils::FilePath m_debuggerCommand;
};

// See cpp file for documentation.
class PROJECTEXPLORER_EXPORT IDevice
        : public Utils::AspectContainer, public std::enable_shared_from_this<IDevice>
{
    friend class Internal::IDevicePrivate;
public:
    using Ptr = IDevicePtr;
    using ConstPtr = IDeviceConstPtr;
    template <class ...Args> using Continuation = std::function<void(Args...)>;

    enum Origin { ManuallyAdded, AutoDetected, AddedBySdk };
    enum MachineType { Hardware, Emulator };

    virtual ~IDevice();

    QString displayName() const;
    void setDisplayName(const QString &name);

    QString defaultDisplayName() const;
    void setDefaultDisplayName(const QString &name);

    void addDisplayNameToLayout(Layouting::Layout &layout) const;

    // Provide some information on the device suitable for formated
    // output, e.g. in tool tips. Get a list of name value pairs.
    class DeviceInfoItem {
    public:
        DeviceInfoItem(const QString &k, const QString &v) : key(k), value(v) { }

        QString key;
        QString value;
    };
    using DeviceInfo = QList<DeviceInfoItem>;
    virtual DeviceInfo deviceInformation() const;

    Utils::Id type() const;
    void setType(Utils::Id type);

    bool isAutoDetected() const;
    bool isFromSdk() const;
    Utils::Id id() const;

    virtual QList<Task> validate() const;

    QString displayType() const;
    Utils::OsType osType() const;

    virtual IDeviceWidget *createWidget() = 0;

    struct DeviceAction {
        QString display;
        std::function<void(const IDevice::Ptr &device)> execute;
    };
    void addDeviceAction(const DeviceAction &deviceAction);
    const QList<DeviceAction> deviceActions() const;

    virtual Tasking::ExecutableItem portsGatheringRecipe(
        const Tasking::Storage<Utils::PortsOutputData> &output) const;
    virtual bool canCreateProcessModel() const { return false; }
    virtual bool hasDeviceTester() const { return false; }
    virtual DeviceTester *createDeviceTester();
    void setIsTesting(bool isTesting);
    bool isTesting() const;

    virtual bool canMount(const Utils::FilePath &filePath) const;

    virtual DeviceProcessSignalOperation::Ptr signalOperation() const;

    enum DeviceState { DeviceReadyToUse, DeviceConnected, DeviceDisconnected, DeviceStateUnknown };
    virtual DeviceState deviceState() const;
    void setDeviceState(const DeviceState state);
    virtual QString deviceStateToString() const;
    QPixmap deviceStateIcon() const;

    static Utils::Id typeFromMap(const Utils::Store &map);
    static Utils::Id idFromMap(const Utils::Store &map);

    static QString defaultPrivateKeyFilePath();
    static QString defaultPublicKeyFilePath();

    SshParameters sshParameters() const;
    void setDefaultSshParameters(const SshParameters &sshParameters);

    SshParametersAspectContainer &sshParametersAspectContainer() const;

    enum ControlChannelHint { QmlControlChannel };
    virtual QUrl toolControlChannel(const ControlChannelHint &) const;

    Utils::PortList freePorts() const;
    void setFreePorts(const Utils::PortList &freePorts);

    MachineType machineType() const;
    void setMachineType(MachineType machineType);

    virtual Utils::FilePath rootPath() const;
    virtual Utils::FilePath filePath(const QString &pathOnDevice) const;

    Utils::FilePath debugServerPath() const;
    void setDebugServerPath(const Utils::FilePath &path);

    Utils::FilePath qmlRunCommand() const;
    void setQmlRunCommand(const Utils::FilePath &path);

    void setExtraData(Utils::Id kind, const QVariant &data);
    QVariant extraData(Utils::Id kind) const;

    void setupId(Origin origin, Utils::Id id = Utils::Id());

    bool canOpenTerminal() const;
    Utils::Result<> openTerminal(const Utils::Environment &env,
                                           const Utils::FilePath &workingDir) const;

    bool isWindowsDevice() const { return osType() == Utils::OsTypeWindows; }
    bool isLinuxDevice() const { return osType() == Utils::OsTypeLinux; }
    bool isMacDevice() const { return osType() == Utils::OsTypeMac; }
    bool isAnyUnixDevice() const;

    Utils::DeviceFileAccess *fileAccess() const;
    virtual bool handlesFile(const Utils::FilePath &filePath) const;

    virtual Utils::FilePath searchExecutableInPath(const QString &fileName) const;
    virtual Utils::FilePath searchExecutable(const QString &fileName,
                                             const Utils::FilePaths &dirs) const;

    virtual Utils::ProcessInterface *createProcessInterface() const;
    virtual FileTransferInterface *createFileTransferInterface(
            const FileTransferSetupData &setup) const;

    Utils::Environment systemEnvironment() const;
    virtual Utils::Result<Utils::Environment> systemEnvironmentWithError() const;

    virtual void aboutToBeRemoved() const {}

    virtual bool ensureReachable(const Utils::FilePath &other) const;
    virtual Utils::Result<Utils::FilePath> localSource(const Utils::FilePath &other) const;

    virtual bool prepareForBuild(const Target *target);
    virtual std::optional<Utils::FilePath> clangdExecutable() const;

    virtual void checkOsType() {}

    void doApply() const;

public:
    Utils::BoolAspect allowEmptyCommand{this};
    Utils::StringSelectionAspect linkDevice{this};
    Utils::BoolAspect sshForwardDebugServerPort{this};
    Utils::FilePathAspect debugServerPathAspect{this};
    Utils::FilePathAspect qmlRunCommandAspect{this};
    Utils::PortListAspect freePortsAspect{this};

protected:
    IDevice();

    virtual void fromMap(const Utils::Store &map);
    virtual void toMap(Utils::Store &map) const;

    using OpenTerminal = std::function<Utils::Result<>(const Utils::Environment &,
                                                                 const Utils::FilePath &)>;
    void setOpenTerminal(const OpenTerminal &openTerminal);
    void setDisplayType(const QString &type);
    void setOsType(Utils::OsType osType);
    void setFileAccess(Utils::DeviceFileAccess *fileAccess);
    void setFileAccessFactory(std::function<Utils::DeviceFileAccess *()> fileAccessFactory);

private:
    IDevice(const IDevice &) = delete;
    IDevice &operator=(const IDevice &) = delete;

    int version() const;
    void setFromSdk();

    const std::unique_ptr<Internal::IDevicePrivate> d;
    friend class DeviceManager;
};

class PROJECTEXPLORER_EXPORT DeviceConstRef
{
public:
    DeviceConstRef(const IDevice::ConstPtr &device);
    DeviceConstRef(const IDevice::Ptr &device);
    virtual ~DeviceConstRef();

    IDevice::ConstPtr lock() const;

    Utils::Id id() const;
    QString displayName() const;
    SshParameters sshParameters() const;
    Utils::FilePath filePath(const QString &pathOnDevice) const;
    QVariant extraData(Utils::Id kind) const;
    Utils::Id linkDeviceId() const;

private:
    std::weak_ptr<const IDevice> m_constDevice;
};

class PROJECTEXPLORER_EXPORT DeviceRef : public DeviceConstRef
{
public:
    DeviceRef(const IDevice::Ptr &device);

    IDevice::Ptr lock() const;

    void setDisplayName(const QString &displayName);
    void setSshParameters(const SshParameters &params);

private:
    std::weak_ptr<IDevice> m_mutableDevice;
};

class PROJECTEXPLORER_EXPORT DeviceTester : public QObject
{
    Q_OBJECT

public:
    enum TestResult { TestSuccess, TestFailure };

    virtual void testDevice() = 0;
    virtual void stopTest() = 0;

signals:
    void progressMessage(const QString &message);
    void errorMessage(const QString &message);
    void finished(ProjectExplorer::DeviceTester::TestResult result);

protected:
    explicit DeviceTester(const IDevice::Ptr &device, QObject *parent = nullptr);
    ~DeviceTester() override;
    const IDevice::Ptr &device() const { return m_device; }

private:
    const IDevice::Ptr m_device;
};

class PROJECTEXPLORER_EXPORT DeviceProcessKiller : public QObject
{
    Q_OBJECT

public:
    void setProcessPath(const Utils::FilePath &path) { m_processPath = path; }
    void start();
    Utils::Result<> result() const { return m_result; }

signals:
    void done(Tasking::DoneResult result);

private:
    Utils::FilePath m_processPath;
    DeviceProcessSignalOperation::Ptr m_signalOperation;
    Utils::Result<> m_result = Utils::ResultOk;
};

using DeviceProcessKillerTask = Tasking::CustomTask<DeviceProcessKiller>;

} // namespace ProjectExplorer
