// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "hostosinfo.h"

#include "filepath.h"
#include "utilstr.h"

#include <QDir>

#if !defined(QT_NO_OPENGL) && defined(QT_GUI_LIB)
#include <QOpenGLContext>
#endif

#ifdef Q_OS_LINUX
#include <sys/sysinfo.h>
#endif

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#ifdef Q_OS_MACOS
#include <sys/sysctl.h>
#endif

namespace Utils::HostOsInfo {

static Qt::CaseSensitivity m_overrideFileNameCaseSensitivity = Qt::CaseSensitive;
static bool m_useOverrideFileNameCaseSensitivity = false;

OsArch hostArchitecture()
{
#ifdef Q_OS_WIN
    // Workaround for Creator running in x86 emulation mode on ARM machines
    static const OsArch arch = []() {
        const HANDLE procHandle = GetCurrentProcess();
        ushort processMachine;
        ushort nativeMachine;
        if (IsWow64Process2(procHandle, &processMachine, &nativeMachine)
            && nativeMachine == IMAGE_FILE_MACHINE_ARM64) {
            return OsArchArm64;
        }

        return osArchFromString(QSysInfo::currentCpuArchitecture()).value_or(OsArchUnknown);
    }();
#else
    static const OsArch arch
        = osArchFromString(QSysInfo::currentCpuArchitecture()).value_or(OsArchUnknown);
#endif

    return arch;
}

void setOverrideFileNameCaseSensitivity(Qt::CaseSensitivity sensitivity)
{
    m_useOverrideFileNameCaseSensitivity = true;
    m_overrideFileNameCaseSensitivity = sensitivity;
}

void unsetOverrideFileNameCaseSensitivity()
{
    m_useOverrideFileNameCaseSensitivity = false;
}

std::optional<quint64> totalMemoryInstalledInBytes()
{
#ifdef Q_OS_LINUX
    struct sysinfo info;
    if (sysinfo(&info) == -1)
        return {};
    return info.totalram;
#elif defined(Q_OS_WIN)
    MEMORYSTATUSEX statex;
    statex.dwLength = sizeof statex;
    if (!GlobalMemoryStatusEx(&statex))
        return {};
    return statex.ullTotalPhys;
#elif defined(Q_OS_MACOS)
    int mib[] = {CTL_HW, HW_MEMSIZE};
    int64_t ram;
    size_t length = sizeof(int64_t);
    if (sysctl(mib, 2, &ram, &length, nullptr, 0) == -1)
        return {};
    return ram;
#endif
    return {};
}

const FilePath &root()
{
    static const FilePath rootDir = FilePath::fromUserInput(QDir::rootPath());
    return rootDir;
}

Qt::CaseSensitivity fileNameCaseSensitivity()
{
    return m_useOverrideFileNameCaseSensitivity
               ? m_overrideFileNameCaseSensitivity
               : OsSpecificAspects::fileNameCaseSensitivity(hostOs());
}

OsArch binaryArchitecture()
{
// MSVC:
#if defined(_M_X64)
    return OsArchAMD64;
#elif defined(_M_IX86)
    return OsArchX86;
#elif defined(_M_ARM64)
    return OsArchArm64;
#elif defined(_M_ARM)
    return OsArchArm;
#elif defined(_M_IA64)
    return OsArchItanium;
// GCC / Clang:
#elif defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64)
    return OsArchAMD64;
#elif defined(__i386__) || defined(__i386) || defined(__i486__) || defined(__i586__) \
    || defined(__i686__)
        return OsArchX86;
#elif defined(__aarch64__)
    return OsArchArm64;
#elif defined(__arm__)
    return OsArchArm;
#elif defined(__ia64__) || defined(__itanium__)
    return OsArchItanium;
#else
    static_assert(false, "Unknown architecture, please add detection.");
    return OsArchUnknown;
#endif
}

QString withExecutableSuffix(const QString &executable)
{
    return OsSpecificAspects::withExecutableSuffix(hostOs(), executable);
}

} // namespace Utils
