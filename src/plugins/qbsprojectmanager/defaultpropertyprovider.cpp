// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "defaultpropertyprovider.h"

#include "qbskitaspect.h"
#include "qbsprojectmanagerconstants.h"
#include "qbsprojectmanagertr.h"

#include <android/androidconstants.h>
#include <coreplugin/messagemanager.h>
#include <baremetal/baremetalconstants.h>
#include <ios/iosconstants.h>
#include <projectexplorer/abi.h>
#include <projectexplorer/devicesupport/devicekitaspects.h>
#include <projectexplorer/devicesupport/idevice.h>
#include <projectexplorer/gcctoolchain.h>
#include <projectexplorer/kit.h>
#include <projectexplorer/toolchain.h>
#include <projectexplorer/sysrootkitaspect.h>
#include <projectexplorer/toolchainkitaspect.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/msvctoolchain.h>
#include <qtsupport/baseqtversion.h>
#include <qtsupport/qtkitaspect.h>
#include <utils/hostosinfo.h>
#include <utils/qtcassert.h>
#include <webassembly/webassemblyconstants.h>

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>

using namespace ProjectExplorer;

namespace QbsProjectManager {
using namespace Constants;

namespace Internal {
using namespace ProjectExplorer::Constants;
using namespace Ios::Constants;

static QString extractToolchainPrefix(QString *compilerName)
{
    QString prefix;
    const QStringList candidates = {QLatin1String("g++"), QLatin1String("clang++"),
                                    QLatin1String("gcc"), QLatin1String("clang")};
    for (const QString &candidate : candidates) {
        const QString suffix = QLatin1Char('-') + candidate;
        const int suffixIndex = compilerName->lastIndexOf(suffix);
        if (suffixIndex == -1)
            continue;
        prefix = compilerName->left(suffixIndex + 1);
        compilerName->remove(0, suffixIndex + 1);
        break;
    }
    return prefix;
}

static QString targetPlatform(const ProjectExplorer::Abi &abi, const ProjectExplorer::Kit *k)
{
    const Utils::Id deviceType = ProjectExplorer::RunDeviceTypeKitAspect::deviceTypeId(k);
    if (deviceType == WebAssembly::Constants::WEBASSEMBLY_DEVICE_TYPE)
        return "wasm-emscripten";

    switch (abi.os()) {
    case ProjectExplorer::Abi::WindowsOS:
        return QLatin1String("windows");
   case ProjectExplorer::Abi::DarwinOS:
        if (deviceType == DESKTOP_DEVICE_TYPE)
            return QLatin1String("macos");
        if (deviceType == IOS_DEVICE_TYPE)
            return QLatin1String("ios");
        if (deviceType == IOS_SIMULATOR_TYPE)
            return QLatin1String("ios-simulator");
        return QLatin1String("darwin");
    case ProjectExplorer::Abi::LinuxOS:
        if (abi.osFlavor() == ProjectExplorer::Abi::AndroidLinuxFlavor)
            return QLatin1String("android");
        return QLatin1String("linux");
    case ProjectExplorer::Abi::BsdOS:
        switch (abi.osFlavor()) {
        case ProjectExplorer::Abi::FreeBsdFlavor:
            return QLatin1String("freebsd");
        case ProjectExplorer::Abi::NetBsdFlavor:
            return QLatin1String("netbsd");
        case ProjectExplorer::Abi::OpenBsdFlavor:
            return QLatin1String("openbsd");
        default:
            break;
        }
        return QLatin1String("bsd");
    case ProjectExplorer::Abi::QnxOS:
        return QLatin1String("qnx");
    case ProjectExplorer::Abi::UnixOS:
        if (abi.osFlavor() == ProjectExplorer::Abi::SolarisUnixFlavor)
            return QLatin1String("solaris");
        return QLatin1String("unix");
    case ProjectExplorer::Abi::VxWorks:
        return QLatin1String("vxworks");
    case ProjectExplorer::Abi::BareMetalOS:
    case ProjectExplorer::Abi::UnknownOS:
        break;
    }
    return QString();
}

static QString toolchainType(const ProjectExplorer::Toolchain *tc)
{
    const Utils::Id type = tc->typeId();
    if (type == ProjectExplorer::Constants::CLANG_TOOLCHAIN_TYPEID
            || (type == Android::Constants::ANDROID_TOOLCHAIN_TYPEID
                && tc->compilerCommand().fileName().contains("clang"))) {
        return "clang";
    }
    if (type == ProjectExplorer::Constants::GCC_TOOLCHAIN_TYPEID
               || type == Android::Constants::ANDROID_TOOLCHAIN_TYPEID) {
        return "gcc"; // TODO: Detect llvm-gcc
    }
    if (type == ProjectExplorer::Constants::MINGW_TOOLCHAIN_TYPEID)
        return "mingw";
    if (type == ProjectExplorer::Constants::CLANG_CL_TOOLCHAIN_TYPEID)
        return "clang-cl";
    if (type == ProjectExplorer::Constants::MSVC_TOOLCHAIN_TYPEID)
        return "msvc";
    if (type == BareMetal::Constants::IAREW_TOOLCHAIN_TYPEID)
        return "iar";
    if (type == BareMetal::Constants::KEIL_TOOLCHAIN_TYPEID)
        return "keil";
    if (type == BareMetal::Constants::SDCC_TOOLCHAIN_TYPEID)
        return "sdcc";
    if (type == WebAssembly::Constants::WEBASSEMBLY_TOOLCHAIN_TYPEID)
        return "emscripten";
    return {};
}

static QString architecture(const ProjectExplorer::Abi &targetAbi)
{
    if (targetAbi.architecture() == Abi::AsmJsArchitecture)
        return "wasm";

    if (targetAbi.architecture() != ProjectExplorer::Abi::UnknownArchitecture) {
        QString architecture = ProjectExplorer::Abi::toString(targetAbi.architecture());

        if (targetAbi.osFlavor() == ProjectExplorer::Abi::AndroidLinuxFlavor) {
            switch (targetAbi.architecture()) {
            case ProjectExplorer::Abi::X86Architecture:
                if (targetAbi.wordWidth() == 64)
                    architecture += "_64";
                return architecture;
            case ProjectExplorer::Abi::ArmArchitecture:
                if (targetAbi.wordWidth() == 64)
                    architecture += "64";
                else
                    architecture += "v7a";
                return architecture;
            case ProjectExplorer::Abi::LoongArchArchitecture:
                if (targetAbi.wordWidth() == 64)
                    architecture += "_64";
                return architecture;
            default:
                break;
            }
        }
        // We have to be conservative tacking on suffixes to arch names because an arch that is
        // already 64-bit may get an incorrect name as a result (i.e. Itanium)
        if (targetAbi.wordWidth() == 64) {
            switch (targetAbi.architecture()) {
            case ProjectExplorer::Abi::X86Architecture:
                architecture.append(QLatin1Char('_'));
                [[fallthrough]];
            case ProjectExplorer::Abi::ArmArchitecture:
                // ARM sub-architectures are currently not handled, which is kind of problematic
            case ProjectExplorer::Abi::MipsArchitecture:
            case ProjectExplorer::Abi::LoongArchArchitecture:
            case ProjectExplorer::Abi::PowerPCArchitecture:
                architecture.append(QString::number(targetAbi.wordWidth()));
                break;
            default:
                break;
            }
        }

        return architecture;
    }

    return QString();
}

static bool isMultiTargetingToolchain(const ProjectExplorer::Toolchain *tc)
{
    // Clang and QCC are multi-targeting compilers; others (GCC/MinGW, MSVC, ICC) are not
    return tc->targetAbi().os() == ProjectExplorer::Abi::QnxOS
            || tc->typeId() == ProjectExplorer::Constants::CLANG_TOOLCHAIN_TYPEID;
}

static QStringList architectures(const ProjectExplorer::Toolchain *tc)
{
    // For platforms which can have builds for multiple architectures in a single configuration
    // (Darwin, Android), regardless of whether the toolchain is multi-targeting or not (Clang
    // always is, but Android GCC is not), let qbs automatically determine the list of architectures
    // to build for by default. Similarly, if the underlying toolchain only targets a single
    // architecture there's no reason to duplicate the detection logic here.
    // Handles: GCC/MinGW, ICC, MSVC, Clang (Darwin, Android)
    if (tc->targetAbi().os() == ProjectExplorer::Abi::DarwinOS
            || tc->targetAbi().osFlavor() == ProjectExplorer::Abi::AndroidLinuxFlavor
            || !isMultiTargetingToolchain(tc))
        return { };

    // This attempts to use the preferred architecture for toolchains which are multi-targeting.
    // Handles: Clang (Linux/UNIX), QCC
    const auto arch = architecture(tc->targetAbi());
    if (!arch.isEmpty())
        return { arch };
    return { };
}

QVariantMap DefaultPropertyProvider::properties(const ProjectExplorer::Kit *k,
                                                const QVariantMap &defaultData) const
{
    QTC_ASSERT(k, return defaultData);
    QVariantMap data = autoGeneratedProperties(k, defaultData);
    const QVariantMap customProperties = QbsKitAspect::properties(k);
    for (QVariantMap::ConstIterator it = customProperties.constBegin();
         it != customProperties.constEnd(); ++it) {
        data.insert(it.key(), it.value());
    }
    return data;
}

static void filterCompilerLinkerFlags(const ProjectExplorer::Abi &targetAbi, QStringList &flags)
{
    for (int i = 0; i < flags.size(); ) {
        if (targetAbi.architecture() != ProjectExplorer::Abi::UnknownArchitecture
            && (flags[i] == "-arch" || flags[i] == "-target") && i + 1 < flags.size()) {
            flags.removeAt(i);
            flags.removeAt(i);
        } else {
            ++i;
        }
    }
}

QVariantMap DefaultPropertyProvider::autoGeneratedProperties(const ProjectExplorer::Kit *k,
                                                             const QVariantMap &defaultData) const
{
    QVariantMap data = defaultData;

    const QString sysroot = SysRootKitAspect::sysRoot(k).toUserOutput();
    if (!sysroot.isEmpty())
        data.insert(QLatin1String(QBS_SYSROOT), sysroot);

    Toolchain *tcC = ToolchainKitAspect::cToolchain(k);
    Toolchain *tcCxx = ToolchainKitAspect::cxxToolchain(k);
    if (!tcC && !tcCxx)
        return data;

    Toolchain *mainTc = tcCxx ? tcCxx : tcC;

    Abi targetAbi = mainTc->targetAbi();

    auto archs = architectures(mainTc);
    if (!archs.isEmpty())
        data.insert(QLatin1String(QBS_ARCHITECTURES), archs);
    if (mainTc->targetAbi() != Abi::abiFromTargetTriplet(mainTc->originalTargetTriple())
            || targetAbi.osFlavor() == Abi::AndroidLinuxFlavor) {
        data.insert(QLatin1String(QBS_ARCHITECTURE), architecture(mainTc->targetAbi()));
    } else if (archs.count() == 1) {
        data.insert(QLatin1String(QBS_ARCHITECTURE), archs.first());
    }
    data.insert(QLatin1String(QBS_TARGETPLATFORM), targetPlatform(targetAbi, k));

    QString toolchain = toolchainType(mainTc);
    if (targetAbi.osFlavor() == Abi::AndroidLinuxFlavor) {
        const IDevice::ConstPtr dev = RunDeviceKitAspect::device(k);
        if (dev) {
            const QString sdkDir = k->value(Android::Constants::ANDROID_KIT_SDK).toString();
            if (!sdkDir.isEmpty())
                data.insert("Android.sdk.sdkDir", sdkDir);
            const QString ndkDir = k->value(Android::Constants::ANDROID_KIT_NDK).toString();
            if (!ndkDir.isEmpty()) {
                data.insert("Android.sdk.ndkDir", ndkDir);
                data.insert("Android.ndk.ndkDir", ndkDir);
            }
        }
        QtSupport::QtVersion *qtVersion = QtSupport::QtKitAspect::qtVersion(k);
        if (qtVersion) {
            data.remove(QBS_ARCHITECTURES);
            data.remove(QBS_ARCHITECTURE);
            QStringList abis;
            for (const auto &abi : qtVersion->qtAbis())
                abis << architecture(abi);
            if (abis.size() == 1)
                data.insert(QLatin1String(QBS_ARCHITECTURE), abis.first());
            else
                data.insert(QLatin1String(QBS_ARCHITECTURES), abis);
        }
    } else {
        Utils::FilePath cCompilerPath;
        if (tcC)
            cCompilerPath = tcC->compilerCommand();

        Utils::FilePath cxxCompilerPath;
        if (tcCxx)
            cxxCompilerPath = tcCxx->compilerCommand();

        QString cCompilerName = cCompilerPath.fileName();
        QString cxxCompilerName = cxxCompilerPath.fileName();
        const QString cToolchainPrefix = extractToolchainPrefix(&cCompilerName);
        const QString cxxToolchainPrefix = extractToolchainPrefix(&cxxCompilerName);

        Utils::FilePath mainFilePath;
        QString mainCompilerName;
        QString mainToolchainPrefix;
        if (tcCxx) {
            mainFilePath = cxxCompilerPath;
            mainCompilerName = cxxCompilerName;
            mainToolchainPrefix = cxxToolchainPrefix;
        } else {
            mainFilePath = cCompilerPath;
            mainCompilerName = cCompilerName;
            mainToolchainPrefix = cToolchainPrefix;
        }

        if (!mainToolchainPrefix.isEmpty())
            data.insert(QLatin1String(CPP_TOOLCHAINPREFIX), mainToolchainPrefix);

        if (toolchain == "clang-cl") {
            data.insert(QLatin1String(CPP_COMPILERNAME), mainCompilerName);
            const auto clangClToolchain =
                    static_cast<ProjectExplorer::Internal::ClangClToolchain *>(mainTc);
            data.insert(QLatin1String(CPP_VCVARSALLPATH), clangClToolchain->varsBat());
        } else if (toolchain == "msvc") {
            data.insert(QLatin1String(CPP_COMPILERNAME), mainCompilerName);
        } else {
            if (!mainCompilerName.isEmpty())
                data.insert(QLatin1String(CPP_COMPILERNAME), mainCompilerName);
            if (!cCompilerName.isEmpty())
                data.insert(QLatin1String(CPP_CCOMPILERNAME), cCompilerName);
            if (!cxxCompilerName.isEmpty())
                data.insert(QLatin1String(CPP_CXXCOMPILERNAME), cxxCompilerName);
        }

        if (tcC && tcCxx && !cCompilerPath.isEmpty() && !cxxCompilerPath.isEmpty()
                && cCompilerPath.absolutePath() != cxxCompilerPath.absolutePath()) {
            Core::MessageManager::writeFlashing(
                Tr::tr("C and C++ compiler paths differ. C compiler may not work."));
        }
        data.insert(QLatin1String(CPP_TOOLCHAINPATH), mainFilePath.absolutePath().path());

        if (auto gcc = mainTc->asGccToolchain()) {
            QStringList compilerFlags = gcc->platformCodeGenFlags();
            filterCompilerLinkerFlags(targetAbi, compilerFlags);
            data.insert(QLatin1String(CPP_PLATFORMCOMMONCOMPILERFLAGS), compilerFlags);

            QStringList linkerFlags = gcc->platformLinkerFlags();
            filterCompilerLinkerFlags(targetAbi, linkerFlags);
            data.insert(QLatin1String(CPP_PLATFORMLINKERFLAGS), linkerFlags);
        }
        if (targetAbi.os() == ProjectExplorer::Abi::DarwinOS) {
            // Reverse engineer the Xcode developer path from the compiler path
            static const QRegularExpression compilerRe(
                QStringLiteral("^(?<developerpath>.*)/Toolchains/(?:.+)\\.xctoolchain/usr/bin$"));
            const QRegularExpressionMatch compilerReMatch = compilerRe.match(cxxCompilerPath.absolutePath().toUrlishString());
            if (compilerReMatch.hasMatch()) {
                const QString developerPath = compilerReMatch.captured(QStringLiteral("developerpath"));
                data.insert(QLatin1String(XCODE_DEVELOPERPATH), developerPath);
                toolchain = "xcode";

                // If the sysroot is part of this developer path, set the canonical SDK name
                const QDir sysrootdir(QDir::cleanPath(sysroot));
                const QString sysrootAbs = sysrootdir.absolutePath();
                const QSettings sdkSettings(
                    sysrootdir.absoluteFilePath(QStringLiteral("SDKSettings.plist")),
                    QSettings::NativeFormat);
                const QString version(
                    sdkSettings.value(QStringLiteral("Version")).toString());
                QString canonicalName(
                    sdkSettings.value(QStringLiteral("CanonicalName")).toString());
                canonicalName.chop(version.size());
                if (!canonicalName.isEmpty() && !version.isEmpty()
                        && sysrootAbs.startsWith(developerPath)) {
                    if (sysrootAbs.endsWith(QStringLiteral("/%1.sdk").arg(canonicalName + version),
                                            Qt::CaseInsensitive)) {
                        data.insert(QLatin1String(XCODE_SDK), QString(canonicalName + version));
                    }
                    if (sysrootAbs.endsWith(QStringLiteral("/%1.sdk").arg(canonicalName),
                                            Qt::CaseInsensitive)) {
                        data.insert(QLatin1String(XCODE_SDK), canonicalName);
                    }
                }
            }
        }
    }

    if (!toolchain.isEmpty())
        data.insert("qbs.toolchainType", toolchain);

    return data;
}

} // namespace Internal
} // namespace QbsProjectManager
