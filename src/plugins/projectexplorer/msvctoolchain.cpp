// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "msvctoolchain.h"

#include "abiwidget.h"
#include "devicesupport/idevice.h"
#include "gcctoolchain.h"
#include "msvcparser.h"
#include "projectexplorer.h"
#include "projectexplorerconstants.h"
#include "projectexplorersettings.h"
#include "projectexplorertr.h"
#include "taskhub.h"

#include <coreplugin/icore.h>

#include <utils/algorithm.h>
#include <utils/async.h>
#include <utils/datafromprocess.h>
#include <utils/environment.h>
#include <utils/fileutils.h>
#include <utils/hostosinfo.h>
#include <utils/pathchooser.h>
#include <utils/qtcprocess.h>
#include <utils/qtcassert.h>
#include <utils/temporarydirectory.h>
#include <utils/winutils.h>

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSettings>
#include <QVersionNumber>

using namespace Utils;

using namespace std::chrono_literals;

#define KEY_ROOT "ProjectExplorer.MsvcToolChain."
static const char varsBatKeyC[] = KEY_ROOT "VarsBat";
static const char varsBatArgKeyC[] = KEY_ROOT "VarsBatArg";
static const char environModsKeyC[] = KEY_ROOT "environmentModifications";

static Q_LOGGING_CATEGORY(Log, "qtc.projectexplorer.toolchain.msvc", QtWarningMsg);

namespace ProjectExplorer::Internal {

// --------------------------------------------------------------------------
// Helpers:
// --------------------------------------------------------------------------

static QThreadPool *envModThreadPool()
{
    static QThreadPool *pool = nullptr;
    if (!pool) {
        pool = new QThreadPool(ProjectExplorerPlugin::instance());
        pool->setMaxThreadCount(1);
    }
    return pool;
}

struct MsvcPlatform
{
    MsvcToolchain::Platform platform;
    const char *name;
    const char *prefix; // VS up until 14.0 (MSVC2015)
    const char *bat;
};

const MsvcPlatform platforms[]
    = {{MsvcToolchain::x86, "x86", "/bin", "vcvars32.bat"},
       {MsvcToolchain::amd64, "amd64", "/bin/amd64", "vcvars64.bat"},
       {MsvcToolchain::x86_amd64, "x86_amd64", "/bin/x86_amd64", "vcvarsx86_amd64.bat"},
       {MsvcToolchain::ia64, "ia64", "/bin/ia64", "vcvars64.bat"},
       {MsvcToolchain::x86_ia64, "x86_ia64", "/bin/x86_ia64", "vcvarsx86_ia64.bat"},
       {MsvcToolchain::arm, "arm", "/bin/arm", "vcvarsarm.bat"},
       {MsvcToolchain::x86_arm, "x86_arm", "/bin/x86_arm", "vcvarsx86_arm.bat"},
       {MsvcToolchain::amd64_arm, "amd64_arm", "/bin/amd64_arm", "vcvarsamd64_arm.bat"},
       {MsvcToolchain::amd64_x86, "amd64_x86", "/bin/amd64_x86", "vcvarsamd64_x86.bat"},
       {MsvcToolchain::x86_arm64, "x86_arm64", "/bin/x86_arm64", "vcvarsx86_arm64.bat"},
       {MsvcToolchain::amd64_arm64, "amd64_arm64", "/bin/amd64_arm64", "vcvarsamd64_arm64.bat"},
       {MsvcToolchain::arm64, "arm64", "/bin/arm64", "vcvarsarm64.bat"},
       {MsvcToolchain::arm64_x86, "arm64_x86", "/bin/arm64_x86", "vcvarsarm64_x86.bat"},
       {MsvcToolchain::arm64_amd64, "arm64_amd64", "/bin/arm64_amd64", "vcvarsarm64_amd64.bat"}};

static QList<const MsvcToolchain *> g_availableMsvcToolchains;

static const MsvcPlatform *platformEntryFromName(const QString &name)
{
    for (const MsvcPlatform &p : platforms) {
        if (name == QLatin1String(p.name))
            return &p;
    }
    return nullptr;
}

static const MsvcPlatform *platformEntry(MsvcToolchain::Platform t)
{
    for (const MsvcPlatform &p : platforms) {
        if (p.platform == t)
            return &p;
    }
    return nullptr;
}

static QString platformName(MsvcToolchain::Platform t)
{
    if (const MsvcPlatform *p = platformEntry(t))
        return QLatin1String(p->name);
    return {};
}

static bool hostPrefersPlatform(MsvcToolchain::Platform platform)
{
    switch (HostOsInfo::hostArchitecture()) {
    case Utils::OsArchAMD64:
        return platform == MsvcToolchain::amd64 || platform == MsvcToolchain::amd64_arm
               || platform == MsvcToolchain::amd64_x86 || platform == MsvcToolchain::amd64_arm64;
    case Utils::OsArchX86:
        return platform == MsvcToolchain::x86 || platform == MsvcToolchain::x86_amd64
               || platform == MsvcToolchain::x86_ia64 || platform == MsvcToolchain::x86_arm
               || platform == MsvcToolchain::x86_arm64;
    case Utils::OsArchArm:
        return platform == MsvcToolchain::arm;
    case Utils::OsArchArm64:
        return platform == MsvcToolchain::arm64
               || platform == MsvcToolchain::arm64_x86 || platform == MsvcToolchain::arm64_amd64;
    case Utils::OsArchItanium:
        return platform == MsvcToolchain::ia64;
    default:
        return false;
    }
}

static bool hostSupportsPlatform(MsvcToolchain::Platform platform)
{
    if (hostPrefersPlatform(platform))
        return true;

    switch (HostOsInfo::hostArchitecture()) {
    // The x86 host toolchains are not the preferred toolchains on amd64 but they are still
    // supported by that host
    case Utils::OsArchAMD64:
        return platform == MsvcToolchain::x86 || platform == MsvcToolchain::x86_amd64
               || platform == MsvcToolchain::x86_ia64 || platform == MsvcToolchain::x86_arm
               || platform == MsvcToolchain::x86_arm64;
    // The Arm64 host can run the cross-compilers via emulation of x86 and amd64
    case Utils::OsArchArm64:
        return platform == MsvcToolchain::x86_arm || platform == MsvcToolchain::x86_arm64
               || platform == MsvcToolchain::amd64_arm || platform == MsvcToolchain::amd64_arm64
               || platform == MsvcToolchain::x86 || platform == MsvcToolchain::x86_amd64
               || platform == MsvcToolchain::amd64 || platform == MsvcToolchain::amd64_x86;
    default:
        return false;
    }
}

static QString fixRegistryPath(const QString &path)
{
    QString result = QDir::fromNativeSeparators(path);
    if (result.endsWith(QLatin1Char('/')))
        result.chop(1);
    return result;
}

struct VisualStudioInstallation
{
    QString vsName;
    QVersionNumber version;
    QString path;       // Main installation path
    QString vcVarsPath; // Path under which the various vc..bat are to be found
    QString vcVarsAll;
    QString displayName;
};

QDebug operator<<(QDebug d, const VisualStudioInstallation &i)
{
    QDebugStateSaver saver(d);
    d.noquote();
    d.nospace();
    d << "VisualStudioInstallation(\"" << i.vsName << "\", v=" << i.version << ", path=\""
      << QDir::toNativeSeparators(i.path) << "\", vcVarsPath=\""
      << QDir::toNativeSeparators(i.vcVarsPath) << "\", vcVarsAll=\""
      << QDir::toNativeSeparators(i.vcVarsAll) << "\")";
    return d;
}

static QString windowsProgramFilesDir()
{
#ifdef Q_OS_WIN64
    const char programFilesC[] = "ProgramFiles(x86)";
#else
    const char programFilesC[] = "ProgramFiles";
#endif
    return QDir::fromNativeSeparators(qtcEnvironmentVariable(programFilesC));
}

static std::optional<VisualStudioInstallation> installationFromPathAndVersion(
    const QString &installationPath, const QVersionNumber &version)
{
    QString vcVarsPath = QDir::fromNativeSeparators(installationPath);
    if (!vcVarsPath.endsWith('/'))
        vcVarsPath += '/';
    if (version.majorVersion() > 14)
        vcVarsPath += QStringLiteral("VC/Auxiliary/Build");
    else
        vcVarsPath += QStringLiteral("VC");

    const QString vcVarsAllPath = vcVarsPath + QStringLiteral("/vcvarsall.bat");
    if (!QFileInfo(vcVarsAllPath).isFile()) {
        qWarning().noquote() << "Unable to find MSVC setup script "
                             << QDir::toNativeSeparators(vcVarsPath) << " in version " << version;
        return std::nullopt;
    }

    const QString versionString = version.toString();
    VisualStudioInstallation installation;
    installation.path = installationPath;
    installation.version = version;
    installation.vsName = versionString;
    installation.vcVarsPath = vcVarsPath;
    installation.vcVarsAll = vcVarsAllPath;
    return installation;
}

// Detect build tools introduced with MSVC2017
static std::optional<VisualStudioInstallation> detectCppBuildTools2017()
{
    const QString installPath = windowsProgramFilesDir()
                                + "/Microsoft Visual Studio/2017/BuildTools";
    const QString vcVarsPath = installPath + "/VC/Auxiliary/Build";
    const QString vcVarsAllPath = vcVarsPath + "/vcvarsall.bat";

    if (!QFileInfo::exists(vcVarsAllPath))
        return std::nullopt;

    VisualStudioInstallation installation;
    installation.path = installPath;
    installation.vcVarsAll = vcVarsAllPath;
    installation.vcVarsPath = vcVarsPath;
    installation.version = QVersionNumber(15);
    installation.vsName = "15.0";

    return installation;
}

static QList<VisualStudioInstallation> detectVisualStudioFromVsWhere(const QString &vswhere)
{
    QList<VisualStudioInstallation> installations;
    Process vsWhereProcess;
    vsWhereProcess.setUtf8Codec();
    vsWhereProcess.setCommand({FilePath::fromString(vswhere),
                        {"-products", "*", "-prerelease", "-legacy", "-format", "json", "-utf8"}});
    vsWhereProcess.runBlocking(5s);
    if (vsWhereProcess.result() != ProcessResult::FinishedWithSuccess) {
        qWarning() << vsWhereProcess.verboseExitMessage();
        return installations;
    }

    const QByteArray output = vsWhereProcess.cleanedStdOut().toUtf8();
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(output, &error);
    if (error.error != QJsonParseError::NoError || doc.isNull()) {
        qWarning() << "Could not parse json document from vswhere output.";
        return installations;
    }

    const QJsonArray versions = doc.array();
    if (versions.isEmpty()) {
        qWarning() << "Could not detect any versions from vswhere output.";
        return installations;
    }

    for (const QJsonValue &vsVersion : versions) {
        const QJsonObject vsVersionObj = vsVersion.toObject();
        if (vsVersionObj.isEmpty()) {
            qWarning() << "Could not obtain object from vswhere version";
            continue;
        }

        QJsonValue value = vsVersionObj.value("installationVersion");
        if (value.isUndefined()) {
            qWarning() << "Could not obtain VS version from json output";
            continue;
        }
        const QString versionString = value.toString();
        QVersionNumber version = QVersionNumber::fromString(versionString);
        value = vsVersionObj.value("installationPath");
        if (value.isUndefined()) {
            qWarning() << "Could not obtain VS installation path from json output";
            continue;
        }
        const QString installationPath = value.toString();
        std::optional<VisualStudioInstallation> installation
            = installationFromPathAndVersion(installationPath, version);

        if (installation) {
            QJsonValue displayName = vsVersionObj.value("displayName");
            if (!displayName.isUndefined())
                installation->displayName = displayName.toString();
            installations.append(*installation);
        }
    }
    return installations;
}

static QList<VisualStudioInstallation> detectVisualStudioFromRegistry()
{
    QList<VisualStudioInstallation> result;
#ifdef Q_OS_WIN64
    const QString keyRoot = QStringLiteral(
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Microsoft\\VisualStudio\\SxS\\");
#else
    const QString keyRoot = QStringLiteral(
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\VisualStudio\\SxS\\");
#endif
    QSettings vsRegistry(keyRoot + QStringLiteral("VS7"), QSettings::NativeFormat);
    QScopedPointer<QSettings> vcRegistry;
    const QStringList keys = vsRegistry.allKeys();
    for (const QString &vsName : keys) {
        const QVersionNumber version = QVersionNumber::fromString(vsName);
        if (!version.isNull()) {
            const QString installationPath = fixRegistryPath(vsRegistry.value(vsName).toString());

            std::optional<VisualStudioInstallation> installation
                = installationFromPathAndVersion(installationPath, version);
            if (installation)
                result.append(*installation);
        }
    }

    // Detect VS 2017 Build Tools
    auto installation = detectCppBuildTools2017();
    if (installation)
        result.append(*installation);

    return result;
}

static QList<VisualStudioInstallation> detectVisualStudio()
{
    const QString vswhere = windowsProgramFilesDir()
                            + "/Microsoft Visual Studio/Installer/vswhere.exe";
    if (QFileInfo::exists(vswhere)) {
        const QList<VisualStudioInstallation> installations = detectVisualStudioFromVsWhere(
            vswhere);
        if (!installations.isEmpty())
            return installations;
    }

    return detectVisualStudioFromRegistry();
}

static unsigned char wordWidthForPlatform(MsvcToolchain::Platform platform)
{
    switch (platform) {
    case ProjectExplorer::Internal::MsvcToolchain::x86:
    case ProjectExplorer::Internal::MsvcToolchain::arm:
    case ProjectExplorer::Internal::MsvcToolchain::x86_arm:
    case ProjectExplorer::Internal::MsvcToolchain::amd64_arm:
    case ProjectExplorer::Internal::MsvcToolchain::amd64_x86:
    case ProjectExplorer::Internal::MsvcToolchain::arm64_x86:
        return 32;
    case ProjectExplorer::Internal::MsvcToolchain::amd64:
    case ProjectExplorer::Internal::MsvcToolchain::x86_amd64:
    case ProjectExplorer::Internal::MsvcToolchain::ia64:
    case ProjectExplorer::Internal::MsvcToolchain::x86_ia64:
    case ProjectExplorer::Internal::MsvcToolchain::amd64_arm64:
    case ProjectExplorer::Internal::MsvcToolchain::x86_arm64:
    case ProjectExplorer::Internal::MsvcToolchain::arm64:
    case ProjectExplorer::Internal::MsvcToolchain::arm64_amd64:
        return 64;
    }

    return 0;
}

static Abi::Architecture archForPlatform(MsvcToolchain::Platform platform)
{
    switch (platform) {
    case ProjectExplorer::Internal::MsvcToolchain::x86:
    case ProjectExplorer::Internal::MsvcToolchain::amd64:
    case ProjectExplorer::Internal::MsvcToolchain::x86_amd64:
    case ProjectExplorer::Internal::MsvcToolchain::amd64_x86:
    case ProjectExplorer::Internal::MsvcToolchain::arm64_x86:
    case ProjectExplorer::Internal::MsvcToolchain::arm64_amd64:
        return Abi::X86Architecture;
    case ProjectExplorer::Internal::MsvcToolchain::arm:
    case ProjectExplorer::Internal::MsvcToolchain::x86_arm:
    case ProjectExplorer::Internal::MsvcToolchain::amd64_arm:
    case ProjectExplorer::Internal::MsvcToolchain::x86_arm64:
    case ProjectExplorer::Internal::MsvcToolchain::amd64_arm64:
    case ProjectExplorer::Internal::MsvcToolchain::arm64:
        return Abi::ArmArchitecture;
    case ProjectExplorer::Internal::MsvcToolchain::ia64:
    case ProjectExplorer::Internal::MsvcToolchain::x86_ia64:
        return Abi::ItaniumArchitecture;
    }

    return Abi::UnknownArchitecture;
}

static Abi findAbiOfMsvc(MsvcToolchain::Type type,
                         MsvcToolchain::Platform platform,
                         const QString &version)
{
    Abi::OSFlavor flavor = Abi::UnknownFlavor;

    QString msvcVersionString = version;
    if (type == MsvcToolchain::WindowsSDK) {
        if (version == QLatin1String("v7.0") || version.startsWith(QLatin1String("6.")))
            msvcVersionString = QLatin1String("9.0");
        else if (version == QLatin1String("v7.0A") || version == QLatin1String("v7.1"))
            msvcVersionString = QLatin1String("10.0");
    }
    if (msvcVersionString.startsWith(QLatin1String("17.")))
        flavor = Abi::WindowsMsvc2022Flavor;
    else if (msvcVersionString.startsWith(QLatin1String("16.")))
        flavor = Abi::WindowsMsvc2019Flavor;
    else if (msvcVersionString.startsWith(QLatin1String("15.")))
        flavor = Abi::WindowsMsvc2017Flavor;
    else if (msvcVersionString.startsWith(QLatin1String("14.")))
        flavor = Abi::WindowsMsvc2015Flavor;
    else if (msvcVersionString.startsWith(QLatin1String("12.")))
        flavor = Abi::WindowsMsvc2013Flavor;
    else if (msvcVersionString.startsWith(QLatin1String("11.")))
        flavor = Abi::WindowsMsvc2012Flavor;
    else if (msvcVersionString.startsWith(QLatin1String("10.")))
        flavor = Abi::WindowsMsvc2010Flavor;
    else if (msvcVersionString.startsWith(QLatin1String("9.")))
        flavor = Abi::WindowsMsvc2008Flavor;
    else
        flavor = Abi::WindowsMsvc2005Flavor;
    const Abi result = Abi(archForPlatform(platform), Abi::WindowsOS, flavor, Abi::PEFormat,
                           wordWidthForPlatform(platform));
    if (!result.isValid())
        qWarning("Unable to completely determine the ABI of MSVC version %s (%s).",
                 qPrintable(version),
                 qPrintable(result.toString()));
    return result;
}

static QString generateDisplayName(const QString &name,
                                   MsvcToolchain::Type t,
                                   MsvcToolchain::Platform p,
                                   const QString &displayName = {})
{
    if (t == MsvcToolchain::WindowsSDK) {
        QString sdkName = name;
        sdkName += QString::fromLatin1(" (%1)").arg(platformName(p));
        return sdkName;
    }
    // Comes as "9.0" from the registry
    QString vcName = QLatin1String("Microsoft Visual C++ Compiler ");
    if (!displayName.isEmpty())
        vcName = QString::fromLatin1("%1 ").arg(displayName);
    vcName += name;
    vcName += QString::fromLatin1(" (%1)").arg(platformName(p));
    return vcName;
}

static QByteArray msvcCompilationDefine(const char *def)
{
    const QByteArray macro(def);
    return "#if defined(" + macro + ")\n__PPOUT__(" + macro + ")\n#endif\n";
}

static QByteArray msvcCompilationFile()
{
    static const char *macros[] = {"_ATL_VER",
                                   "__ATOM__",
                                   "__AVX__",
                                   "__AVX2__",
                                   "_CHAR_UNSIGNED",
                                   "__CLR_VER",
                                   "_CMMN_INTRIN_FUNC",
                                   "_CONTROL_FLOW_GUARD",
                                   "__cplusplus",
                                   "__cplusplus_cli",
                                   "__cplusplus_winrt",
                                   "_CPPLIB_VER",
                                   "_CPPRTTI",
                                   "_CPPUNWIND",
                                   "_DEBUG",
                                   "_DLL",
                                   "_INTEGRAL_MAX_BITS",
                                   "__INTELLISENSE__",
                                   "_ISO_VOLATILE",
                                   "_KERNEL_MODE",
                                   "_M_AAMD64",
                                   "_M_ALPHA",
                                   "_M_AMD64",
                                   "_MANAGED",
                                   "_M_ARM",
                                   "_M_ARM64",
                                   "_M_ARM_ARMV7VE",
                                   "_M_ARM_FP",
                                   "_M_ARM_NT",
                                   "_M_ARMT",
                                   "_M_CEE",
                                   "_M_CEE_PURE",
                                   "_M_CEE_SAFE",
                                   "_MFC_VER",
                                   "_M_FP_EXCEPT",
                                   "_M_FP_FAST",
                                   "_M_FP_PRECISE",
                                   "_M_FP_STRICT",
                                   "_M_IA64",
                                   "_M_IX86",
                                   "_M_IX86_FP",
                                   "_M_MPPC",
                                   "_M_MRX000",
                                   "_M_PPC",
                                   "_MSC_BUILD",
                                   "_MSC_EXTENSIONS",
                                   "_MSC_FULL_VER",
                                   "_MSC_VER",
                                   "_MSVC_LANG",
                                   "__MSVC_RUNTIME_CHECKS",
                                   "_MT",
                                   "_M_THUMB",
                                   "_M_X64",
                                   "_NATIVE_WCHAR_T_DEFINED",
                                   "_OPENMP",
                                   "_PREFAST_",
                                   "__STDC__",
                                   "__STDC_HOSTED__",
                                   "__STDCPP_THREADS__",
                                   "_VC_NODEFAULTLIB",
                                   "_WCHAR_T_DEFINED",
                                   "_WIN32",
                                   "_WIN32_WCE",
                                   "_WIN64",
                                   "_WINRT_DLL",
                                   "_Wp64",
                                   nullptr};
    QByteArray file = "#define __PPOUT__(x) V##x=x\n\n";
    for (int i = 0; macros[i] != nullptr; ++i)
        file += msvcCompilationDefine(macros[i]);
    file += "\nvoid main(){}\n\n";
    return file;
}

// Run MSVC 'cl' compiler to obtain #defines.
// This function must be thread-safe!
//
// Some notes regarding the used approach:
//
// It seems that there is no reliable way to get all the
// predefined macros for a cl invocation. The following two
// approaches are unfortunately limited since both lead to an
// incomplete list of actually predefined macros and come with
// other problems, too.
//
// 1) Maintain a list of predefined macros from the official
//    documentation (for MSVC2015, e.g. [1]). Feed cl with a
//    temporary file that queries the values of those macros.
//
//    Problems:
//     * Maintaining that list.
//     * The documentation is incomplete, we do not get all
//       predefined macros. E.g. the cl from MSVC2015, set up
//       with "vcvars.bat x86_arm", predefines among others
//       _M_ARMT, but that's not reflected in the
//       documentation.
//
// 2) Run cl with the undocumented options /B1 and /Bx, as
//    described in [2].
//
//    Note: With qmake from Qt >= 5.8 it's possible to print
//    the macros formatted as preprocessor code in an easy to
//    read/compare/diff way:
//
//      > cl /nologo /c /TC /B1 qmake NUL
//      > cl /nologo /c /TP /Bx qmake NUL
//
//    Problems:
//     * Using undocumented options.
//     * Resulting macros are incomplete.
//       For example, the nowadays default option /Zc:wchar_t
//       predefines _WCHAR_T_DEFINED, but this is not reflected
//       with this approach.
//
//       To work around this we would need extra cl invocations
//       to get the actual values of the missing macros
//       (approach 1).
//
// Currently we combine both approaches in this way:
//  * As base, maintain the list from the documentation and
//    update it once a new MSVC version is released.
//  * Enrich it with macros that we discover with approach 2
//    once a new MSVC version is released.
//  * Enrich it further with macros that are not covered with
//    the above points.
//
// TODO: Update the predefined macros for MSVC 2017 once the
//       page exists.
//
// [1] https://msdn.microsoft.com/en-us/library/b0084kay.aspx
// [2] http://stackoverflow.com/questions/3665537/how-to-find-out-cl-exes-built-in-macros
Macros MsvcToolchain::msvcPredefinedMacros(const QStringList &cxxflags,
                                           const Environment &env) const
{
    Macros predefinedMacros;

    QStringList toProcess;
    for (auto arg = cxxflags.begin(); arg != cxxflags.end(); ++arg) {
        if (arg->startsWith("/D") || arg->startsWith("-D")) {
            if (arg->length() > 2)
                predefinedMacros.append(Macro::fromKeyValue(arg->mid(2)));
            else if (std::next(arg) != cxxflags.end())
                predefinedMacros.append(Macro::fromKeyValue(*++arg));
        } else if (arg->startsWith("/U") || arg->startsWith("-U")) {
            if (arg->length() > 2) {
                predefinedMacros.append({arg->mid(2).toLocal8Bit(),
                                         MacroType::Undefine});
            } else if (std::next(arg) != cxxflags.end()) {
                predefinedMacros.append({(++arg)->toLocal8Bit(),
                                         MacroType::Undefine});
            }
        } else {
            toProcess.append(*arg);
        }
    }

    TempFileSaver saver(TemporaryDirectory::masterDirectoryPath() + "/envtestXXXXXX.cpp");
    saver.write(msvcCompilationFile());
    if (const Result<> res = saver.finalize(); !res) {
        qWarning("%s: %s", Q_FUNC_INFO, qPrintable(res.error()));
        return predefinedMacros;
    }

    Process cpp;
    cpp.setEnvironment(env);
    cpp.setWorkingDirectory(TemporaryDirectory::masterDirectoryFilePath());
    QStringList arguments;
    const FilePath binary = env.searchInPath(QLatin1String("cl.exe"));
    if (binary.isEmpty()) {
        qWarning("%s: The compiler binary cl.exe could not be found in the path.", Q_FUNC_INFO);
        return predefinedMacros;
    }

    if (language() == ProjectExplorer::Constants::C_LANGUAGE_ID)
        arguments << QLatin1String("/TC");
    arguments << toProcess << QLatin1String("/EP") << saver.filePath().toUserOutput();
    cpp.setCommand({binary, arguments});
    cpp.runBlocking();
    if (cpp.result() != ProcessResult::FinishedWithSuccess)
        return predefinedMacros;

    const QStringList output = Utils::filtered(cpp.cleanedStdOut().split('\n'),
                                               [](const QString &s) { return s.startsWith('V'); });
    for (const QString &line : output)
        predefinedMacros.append(Macro::fromKeyValue(line.mid(1)));
    return predefinedMacros;
}

//
// We want to detect the language version based on the predefined macros.
// Unfortunately MSVC does not conform to standard when it comes to the predefined
// __cplusplus macro - it reports "199711L", even for newer language versions.
//
// However:
//   * For >= Visual Studio 2015 Update 3 predefines _MSVC_LANG which has the proper value
//     of __cplusplus.
//     See https://docs.microsoft.com/en-us/cpp/preprocessor/predefined-macros?view=vs-2017
//   * For >= Visual Studio 2017 Version 15.7 __cplusplus is correct once /Zc:__cplusplus
//     is provided on the command line. Then __cplusplus == _MSVC_LANG.
//     See https://blogs.msdn.microsoft.com/vcblog/2018/04/09/msvc-now-correctly-reports-__cplusplus
//
// We rely on _MSVC_LANG if possible, otherwise on some hard coded language versions
// depending on _MSC_VER.
//
// For _MSV_VER values, see https://docs.microsoft.com/en-us/cpp/preprocessor/predefined-macros?view=vs-2017.
//
Utils::LanguageVersion MsvcToolchain::msvcLanguageVersion(const QStringList & /*cxxflags*/,
                                                          const Utils::Id &language,
                                                          const Macros &macros) const
{
    using Utils::LanguageVersion;

    int mscVer = -1;
    QByteArray msvcLang;
    for (const ProjectExplorer::Macro &macro : macros) {
        if (macro.key == "_MSVC_LANG")
            msvcLang = macro.value;
        if (macro.key == "_MSC_VER")
            mscVer = macro.value.toInt(nullptr);
    }
    QTC_CHECK(mscVer > 0);

    if (language == Constants::CXX_LANGUAGE_ID) {
        if (!msvcLang.isEmpty()) // >= Visual Studio 2015 Update 3
            return Toolchain::cxxLanguageVersion(msvcLang);
        if (mscVer >= 1800) // >= Visual Studio 2013 (12.0)
            return LanguageVersion::CXX14;
        if (mscVer >= 1600) // >= Visual Studio 2010 (10.0)
            return LanguageVersion::CXX11;
        return LanguageVersion::CXX98;
    } else if (language == Constants::C_LANGUAGE_ID) {
        if (mscVer >= 1910) // >= Visual Studio 2017 RTW (15.0)
            return LanguageVersion::C11;
        return LanguageVersion::C99;
    } else {
        QTC_CHECK(false && "Unexpected toolchain language, assuming latest C++ we support.");
        return LanguageVersion::LatestCxx;
    }
}

// Windows: Expand the delayed evaluation references returned by the
// SDK setup scripts: "PATH=!Path!;foo". Some values might expand
// to empty and should not be added
static QString winExpandDelayedEnvReferences(QString in, const Utils::Environment &env)
{
    const QChar exclamationMark = QLatin1Char('!');
    for (int pos = 0; pos < in.size();) {
        // Replace "!REF!" by its value in process environment
        pos = in.indexOf(exclamationMark, pos);
        if (pos == -1)
            break;
        const int nextPos = in.indexOf(exclamationMark, pos + 1);
        if (nextPos == -1)
            break;
        const QString var = in.mid(pos + 1, nextPos - pos - 1);
        const QString replacement = env.expandedValueForKey(var.toUpper());
        in.replace(pos, nextPos + 1 - pos, replacement);
        pos += replacement.size();
    }
    return in;
}

void MsvcToolchain::environmentModifications(QPromise<MsvcToolchain::GenerateEnvResult> &promise,
                                             QString vcvarsBat, QString varsBatArg)
{
    const Utils::Environment inEnv = Utils::Environment::systemEnvironment();
    Utils::Environment outEnv;
    QMap<QString, QString> envPairs;
    Utils::EnvironmentItems diff;
    std::optional<QString> error = generateEnvironmentSettings(inEnv,
                                                                 vcvarsBat,
                                                                 varsBatArg,
                                                                 envPairs);
    if (!error) {
        // Now loop through and process them
        for (auto envIter = envPairs.cbegin(), end = envPairs.cend(); envIter != end; ++envIter) {
            const QString expandedValue = winExpandDelayedEnvReferences(envIter.value(), inEnv);
            if (!expandedValue.isEmpty())
                outEnv.set(envIter.key(), expandedValue);
        }

        diff = inEnv.diff(outEnv, true);
        for (int i = diff.size() - 1; i >= 0; --i) {
            if (diff.at(i).name.startsWith(QLatin1Char('='))) { // Exclude "=C:", "=EXITCODE"
                diff.removeAt(i);
            }
        }
    }

    promise.addResult(MsvcToolchain::GenerateEnvResult{error, diff});
}

void MsvcToolchain::initEnvModWatcher(const QFuture<GenerateEnvResult> &future)
{
    connect(&m_envModWatcher, &QFutureWatcher<GenerateEnvResult>::resultReadyAt, this, [this] {
        const GenerateEnvResult &result = m_envModWatcher.result();
        if (result.error) {
            QString errorMessage = *result.error;
            if (!errorMessage.isEmpty()) {
                Task::TaskType severity;
                if (m_environmentModifications.isEmpty()) {
                    severity = Task::Error;
                } else {
                    severity = Task::Warning;
                    errorMessage.prepend(
                        Tr::tr("Falling back to use the cached environment for \"%1\" after:")
                                .arg(displayName()) + '\n');
                }
                TaskHub::addTask<CompileTask>(severity, errorMessage);
            }
        } else {
            updateEnvironmentModifications(result.environmentItems);
        }
    });
    m_envModWatcher.setFuture(future);
}

bool MsvcToolchain::canShareBundleImpl(const Toolchain &other) const
{
    const auto &otherMsvc = static_cast<const MsvcToolchain &>(other);
    return m_vcvarsBat == otherMsvc.m_vcvarsBat && m_varsBatArg == otherMsvc.m_varsBatArg;
}

void MsvcToolchain::updateEnvironmentModifications(Utils::EnvironmentItems modifications)
{
    Utils::EnvironmentItem::sort(&modifications);
    if (modifications != m_environmentModifications) {
        if (Log().isDebugEnabled()) {
            qCDebug(Log) << "Update environment for " << displayName();
            for (const EnvironmentItem &item : std::as_const(modifications))
                qCDebug(Log) << '\t' << item;
        }
        m_environmentModifications = modifications;
        rescanForCompiler();
        toolChainUpdated();
    } else {
        qCDebug(Log) << "No updates for " << displayName();
    }
}

Utils::Environment MsvcToolchain::readEnvironmentSetting(const Utils::Environment &env) const
{
    Utils::Environment resultEnv = env;
    if (m_environmentModifications.isEmpty()) {
        m_envModWatcher.waitForFinished();
        if (m_envModWatcher.future().isFinished() && !m_envModWatcher.future().isCanceled()) {
            const GenerateEnvResult &result = m_envModWatcher.result();
            if (result.error) {
                const QString &errorMessage = *result.error;
                if (!errorMessage.isEmpty())
                    TaskHub::addTask<CompileTask>(Task::Error, errorMessage);
            } else {
                resultEnv.modify(result.environmentItems);
            }
        }
    } else {
        resultEnv.modify(m_environmentModifications);
    }
    return resultEnv;
}

// --------------------------------------------------------------------------
// MsvcToolchain
// --------------------------------------------------------------------------

static void addToAvailableMsvcToolchains(const MsvcToolchain *toolchain)
{
    if (toolchain->typeId() != Constants::MSVC_TOOLCHAIN_TYPEID)
        return;

    if (!g_availableMsvcToolchains.contains(toolchain))
        g_availableMsvcToolchains.push_back(toolchain);
}

MsvcToolchain::MsvcToolchain(Utils::Id typeId)
    : Toolchain(typeId)
{
    setDisplayName("Microsoft Visual C++ Compiler");
    setTypeDisplayName(Tr::tr("MSVC"));
    addToAvailableMsvcToolchains(this);
    setTargetAbiKey(KEY_ROOT "SupportedAbi");
    setVersionFlagsAndParser({}, [](const QString &, const QString &stdErr) -> QVersionNumber {
        const QString markerString = " Version ";
        const int markerIndex = stdErr.indexOf(markerString);
        if (markerIndex == -1)
            return {};
        const int versionOffset = markerIndex + markerString.size();
        const int spaceIndex = stdErr.indexOf(' ', versionOffset);
        if (spaceIndex == -1)
            return {};
        return QVersionNumber::fromString(stdErr.mid(versionOffset, spaceIndex - versionOffset));
    });
}

void MsvcToolchain::inferWarningsForLevel(int warningLevel, WarningFlags &flags)
{
    // reset all except unrelated flag
    flags = flags & WarningFlags::AsErrors;

    if (warningLevel >= 1)
        flags |= WarningFlags(WarningFlags::Default | WarningFlags::IgnoredQualifiers
                              | WarningFlags::HiddenLocals | WarningFlags::UnknownPragma);
    if (warningLevel >= 2)
        flags |= WarningFlags::All;
    if (warningLevel >= 3) {
        flags |= WarningFlags(WarningFlags::Extra | WarningFlags::NonVirtualDestructor
                              | WarningFlags::SignedComparison | WarningFlags::UnusedLocals
                              | WarningFlags::Deprecated);
    }
    if (warningLevel >= 4)
        flags |= WarningFlags::UnusedParams;
}

MsvcToolchain::~MsvcToolchain()
{
    g_availableMsvcToolchains.removeOne(this);
}

bool MsvcToolchain::isValid() const
{
    if (m_vcvarsBat.isEmpty())
        return false;
    QFileInfo fi(m_vcvarsBat);
    return fi.isFile() && fi.isExecutable();
}

QString MsvcToolchain::originalTargetTriple() const
{
    return targetAbi().wordWidth() == 64 ? QLatin1String("x86_64-pc-windows-msvc")
                                         : QLatin1String("i686-pc-windows-msvc");
}

QStringList MsvcToolchain::suggestedMkspecList() const
{
    // "win32-msvc" is the common MSVC mkspec introduced in Qt 5.8.1
    switch (targetAbi().osFlavor()) {
    case Abi::WindowsMsvc2005Flavor:
        return {"win32-msvc",
                "win32-msvc2005"};
    case Abi::WindowsMsvc2008Flavor:
        return {"win32-msvc",
                "win32-msvc2008"};
    case Abi::WindowsMsvc2010Flavor:
        return {"win32-msvc",
                "win32-msvc2010"};
    case Abi::WindowsMsvc2012Flavor:
        return {"win32-msvc",
                "win32-msvc2012",
                "win32-msvc2010"};
    case Abi::WindowsMsvc2013Flavor:
        return {"win32-msvc",
                "win32-msvc2013",
                "win32-msvc2012",
                "win32-msvc2010"};
    case Abi::WindowsMsvc2015Flavor:
        return {"win32-msvc",
                "win32-msvc2015"};
    case Abi::WindowsMsvc2017Flavor:
        return {"win32-msvc",
                "win32-msvc2017",};
    case Abi::WindowsMsvc2019Flavor:
        return {"win32-msvc",
                "win32-msvc2019",
                "win32-arm64-msvc"};
    case Abi::WindowsMsvc2022Flavor:
        return {"win32-msvc",
                "win32-msvc2022",
                "win32-arm64-msvc"};
    default:
        break;
    }
    return {};
}

Abis MsvcToolchain::supportedAbis() const
{
    Abi abi = targetAbi();
    Abis abis = {abi};
    switch (abi.osFlavor()) {
    case Abi::WindowsMsvc2022Flavor:
        abis << Abi(abi.architecture(),
                    abi.os(),
                    Abi::WindowsMsvc2019Flavor,
                    abi.binaryFormat(),
                    abi.wordWidth(),
                    abi.param());
        Q_FALLTHROUGH();
    case Abi::WindowsMsvc2019Flavor:
        abis << Abi(abi.architecture(),
                    abi.os(),
                    Abi::WindowsMsvc2017Flavor,
                    abi.binaryFormat(),
                    abi.wordWidth(),
                    abi.param());
        Q_FALLTHROUGH();
    case Abi::WindowsMsvc2017Flavor:
        abis << Abi(abi.architecture(),
                    abi.os(),
                    Abi::WindowsMsvc2015Flavor,
                    abi.binaryFormat(),
                    abi.wordWidth(),
                    abi.param());
        break;
    default:
        break;
    }
    return abis;
}

void MsvcToolchain::toMap(Store &data) const
{
    Toolchain::toMap(data);
    data.insert(varsBatKeyC, m_vcvarsBat);
    if (!m_varsBatArg.isEmpty())
        data.insert(varsBatArgKeyC, m_varsBatArg);
    EnvironmentItem::sort(&m_environmentModifications);
    data.insert(environModsKeyC, EnvironmentItem::toVariantList(m_environmentModifications));
}

void MsvcToolchain::fromMap(const Store &data)
{
    Toolchain::fromMap(data);
    if (hasError()) {
        g_availableMsvcToolchains.removeOne(this);
        return;
    }
    m_vcvarsBat = QDir::fromNativeSeparators(data.value(varsBatKeyC).toString());
    m_varsBatArg = data.value(varsBatArgKeyC).toString();

    m_environmentModifications = EnvironmentItem::itemsFromVariantList(
        data.value(environModsKeyC).toList());
    rescanForCompiler();

    initEnvModWatcher(Utils::asyncRun(envModThreadPool(), &MsvcToolchain::environmentModifications,
                                      m_vcvarsBat, m_varsBatArg));

    if (m_vcvarsBat.isEmpty() || !targetAbi().isValid()) {
        reportError();
        g_availableMsvcToolchains.removeOne(this);
    }
}

bool MsvcToolchain::hostPrefersToolchain() const
{
    return hostPrefersPlatform(platform());
}

bool static hasFlagEffectOnMacros(const QString &flag)
{
    if (flag.startsWith("-") || flag.startsWith("/")) {
        const QString f = flag.mid(1);
        if (f.startsWith("I"))
            return false; // Skip include paths
        if (f.startsWith("w", Qt::CaseInsensitive))
            return false; // Skip warning options
        if (f.startsWith("Y") || (f.startsWith("F") && f != "F"))
            return false; // Skip pch-related flags
    }
    return true;
}

Toolchain::MacroInspectionRunner MsvcToolchain::createMacroInspectionRunner() const
{
    Utils::Environment env(m_lastEnvironment);
    addToEnvironment(env);
    MacrosCache macroCache = predefinedMacrosCache();
    const Utils::Id lang = language();

    // This runner must be thread-safe!
    return [this, env, macroCache, lang](const QStringList &cxxflags) {
        const QStringList filteredFlags = Utils::filtered(cxxflags, [](const QString &arg) {
            return hasFlagEffectOnMacros(arg);
        });

        const std::optional<MacroInspectionReport> cachedMacros = macroCache->check(filteredFlags);
        if (cachedMacros)
            return *cachedMacros;

        const Macros macros = msvcPredefinedMacros(filteredFlags, env);

        const auto report = MacroInspectionReport{macros,
                                                  msvcLanguageVersion(filteredFlags, lang, macros)};
        macroCache->insert(filteredFlags, report);

        return report;
    };
}

Utils::LanguageExtensions MsvcToolchain::languageExtensions(const QStringList &cxxflags) const
{
    using Utils::LanguageExtension;
    Utils::LanguageExtensions extensions(LanguageExtension::Microsoft);
    if (cxxflags.contains(QLatin1String("/openmp")))
        extensions |= LanguageExtension::OpenMP;

    // see http://msdn.microsoft.com/en-us/library/0k0w269d%28v=vs.71%29.aspx
    if (cxxflags.contains(QLatin1String("/Za")))
        extensions &= ~Utils::LanguageExtensions(LanguageExtension::Microsoft);

    return extensions;
}

WarningFlags MsvcToolchain::warningFlags(const QStringList &cflags) const
{
    WarningFlags flags = WarningFlags::NoWarnings;
    for (QString flag : cflags) {
        if (!flag.isEmpty() && flag[0] == QLatin1Char('-'))
            flag[0] = QLatin1Char('/');

        if (flag == QLatin1String("/WX")) {
            flags |= WarningFlags::AsErrors;
        } else if (flag == QLatin1String("/W0") || flag == QLatin1String("/w")) {
            inferWarningsForLevel(0, flags);
        } else if (flag == QLatin1String("/W1")) {
            inferWarningsForLevel(1, flags);
        } else if (flag == QLatin1String("/W2")) {
            inferWarningsForLevel(2, flags);
        } else if (flag == QLatin1String("/W3") || flag == QLatin1String("/W4")
                   || flag == QLatin1String("/Wall")) {
            inferWarningsForLevel(3, flags);
        }

        WarningFlagAdder add(flag, flags);
        if (add.triggered())
            continue;
        // http://msdn.microsoft.com/en-us/library/ay4h0tc9.aspx
        add(4263, WarningFlags::OverloadedVirtual);
        // http://msdn.microsoft.com/en-us/library/ytxde1x7.aspx
        add(4230, WarningFlags::IgnoredQualifiers);
        // not exact match, http://msdn.microsoft.com/en-us/library/0hx5ckb0.aspx
        add(4258, WarningFlags::HiddenLocals);
        // http://msdn.microsoft.com/en-us/library/wzxffy8c.aspx
        add(4265, WarningFlags::NonVirtualDestructor);
        // http://msdn.microsoft.com/en-us/library/y92ktdf2%28v=vs.90%29.aspx
        add(4018, WarningFlags::SignedComparison);
        // http://msdn.microsoft.com/en-us/library/w099eeey%28v=vs.90%29.aspx
        add(4068, WarningFlags::UnknownPragma);
        // http://msdn.microsoft.com/en-us/library/26kb9fy0%28v=vs.80%29.aspx
        add(4100, WarningFlags::UnusedParams);
        // http://msdn.microsoft.com/en-us/library/c733d5h9%28v=vs.90%29.aspx
        add(4101, WarningFlags::UnusedLocals);
        // http://msdn.microsoft.com/en-us/library/xb1db44s%28v=vs.90%29.aspx
        add(4189, WarningFlags::UnusedLocals);
        // http://msdn.microsoft.com/en-us/library/ttcz0bys%28v=vs.90%29.aspx
        add(4996, WarningFlags::Deprecated);
    }
    return flags;
}

FilePaths MsvcToolchain::includedFiles(const QStringList &flags,
                                       const FilePath &directoryPath) const
{
    return Toolchain::includedFiles("/FI", flags, directoryPath, PossiblyConcatenatedFlag::Yes);
}

Toolchain::BuiltInHeaderPathsRunner MsvcToolchain::createBuiltInHeaderPathsRunner(
        const Environment &env) const
{
    Utils::Environment fullEnv = env;
    addToEnvironment(fullEnv);

    return [this, fullEnv](const QStringList &, const FilePath &, const QString &) {
        QMutexLocker locker(&m_headerPathsMutex);
        const auto envList = fullEnv.toStringList();
        const auto it = m_headerPathsPerEnv.constFind(envList);
        if (it != m_headerPathsPerEnv.cend())
            return *it;
        return *m_headerPathsPerEnv.insert(envList,
                                           toBuiltInHeaderPaths(fullEnv.pathListValue("INCLUDE")));
    };
}

void MsvcToolchain::addToEnvironment(Utils::Environment &env) const
{
    // We cache the full environment (incoming + modifications by setup script).
    if (!m_resultEnvironment.hasChanges() || env != m_lastEnvironment) {
        qCDebug(Log) << "addToEnvironment: " << displayName();
        m_lastEnvironment = env;
        m_resultEnvironment = readEnvironmentSetting(env);
    }
    env = m_resultEnvironment;
}

static FilePath wrappedMakeCommand(const FilePath &command)
{
    const FilePath wrapperPath = FilePath::currentWorkingPath() / "msvc_make.bat";

    const QByteArray contents =
            "chcp 65001\r\n"
            "\"" + command.nativePath().toLocal8Bit() + "\" %*";

    if (wrapperPath.writeFileContents(contents))
        return wrapperPath;

    return command;
}

FilePath MsvcToolchain::makeCommand(const Environment &environment) const
{
    const bool useJom = globalProjectExplorerSettings().useJom();
    const QString jom("jom.exe");
    const QString nmake("nmake.exe");
    Utils::FilePath tmp;

    FilePath command;
    if (useJom) {
        tmp = environment.searchInPath(jom,
                                       {Core::ICore::libexecPath(),
                                        Core::ICore::libexecPath("jom")});
        if (!tmp.isEmpty())
            command = tmp;
    }

    if (command.isEmpty()) {
        tmp = environment.searchInPath(nmake);
        if (!tmp.isEmpty())
            command = tmp;
    }

    if (command.isEmpty())
        command = FilePath::fromString(useJom ? jom : nmake);

    if (environment.hasKey("VSLANG"))
        return wrappedMakeCommand(command);

    return command;
}

void MsvcToolchain::rescanForCompiler()
{
    if (typeId() == Constants::CLANG_CL_TOOLCHAIN_TYPEID)
        return;

    Utils::Environment env = Utils::Environment::systemEnvironment();
    addToEnvironment(env);

    setCompilerCommand(
          env.searchInPath(QLatin1String("cl.exe"), {}, [](const Utils::FilePath &name) {
              QDir dir(QDir::cleanPath(name.toFileInfo().absolutePath() + QStringLiteral("/..")));
              do {
                  if (QFileInfo::exists(dir.absoluteFilePath(QStringLiteral("vcvarsall.bat")))
                      || QFileInfo::exists(dir.absolutePath() + "/Auxiliary/Build/vcvarsall.bat"))
                      return true;
              } while (dir.cdUp() && !dir.isRoot());
              return false;
          }));
}

QList<OutputLineParser *> MsvcToolchain::createOutputParsers() const
{
    return {new MsvcParser};
}

void MsvcToolchain::setupVarsBat(const Abi &abi, const QString &varsBat, const QString &varsBatArg)
{
    m_lastEnvironment = Utils::Environment::systemEnvironment();
    setTargetAbiNoSignal(abi);
    m_vcvarsBat = varsBat;
    m_varsBatArg = varsBatArg;

    if (!varsBat.isEmpty()) {
        initEnvModWatcher(Utils::asyncRun(envModThreadPool(),
                          &MsvcToolchain::environmentModifications, varsBat, varsBatArg));
    }
}

void MsvcToolchain::resetVarsBat()
{
    m_lastEnvironment = Utils::Environment::systemEnvironment();
    setTargetAbiNoSignal(Abi());
    m_vcvarsBat.clear();
    m_varsBatArg.clear();
}

MsvcToolchain::Platform MsvcToolchain::platform() const
{
    QStringList args = m_varsBatArg.split(' ');
    if (const MsvcPlatform *entry = platformEntryFromName(args.value(0)))
        return entry->platform;
    return Utils::HostOsInfo::hostArchitecture() == Utils::OsArchAMD64 ? amd64 : x86;
}

// --------------------------------------------------------------------------
// MsvcBasedToolchainConfigWidget: Creates a simple GUI without error label
// to display name and varsBat. Derived classes should add the error label and
// call setFromMsvcToolchain().
// --------------------------------------------------------------------------

static QString msvcVarsToDisplay(const MsvcToolchain &tc)
{
    QString varsBatDisplay = QDir::toNativeSeparators(tc.varsBat());
    if (!tc.varsBatArg().isEmpty()) {
        varsBatDisplay += QLatin1Char(' ');
        varsBatDisplay += tc.varsBatArg();
    }
    return varsBatDisplay;
}

static QString msvcVarsToDisplay(const ToolchainBundle &bundle)
{
    return msvcVarsToDisplay(static_cast<const MsvcToolchain &>(*bundle.toolchains().first()));
}

class MsvcBasedToolchainConfigWidget : public ToolchainConfigWidget
{
public:
    explicit MsvcBasedToolchainConfigWidget(const ToolchainBundle &bundle)
        : ToolchainConfigWidget(bundle)
        , m_nameDisplayLabel(new QLabel(this))
        , m_varsBatDisplayLabel(new QLabel(this))
    {
        m_nameDisplayLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
        m_mainLayout->addRow(m_nameDisplayLabel);
        m_varsBatDisplayLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
        m_mainLayout->addRow(Tr::tr("Initialization:"), m_varsBatDisplayLabel);
        if (g_availableMsvcToolchains.isEmpty()) {
            setErrorMessage(
                Tr::tr(
                    "No MSVC toolchains were found. You need to install Visual Studio or the "
                    "Visual Studio Build Tools."));
        }
    }

protected:
    void applyImpl() override {}
    void discardImpl() override { setFromMsvcToolChain(); }
    bool isDirtyImpl() const override { return false; }
    void makeReadOnlyImpl() override {}

    void setFromMsvcToolChain()
    {
        m_varsBatDisplayLabel->setText(msvcVarsToDisplay(bundle()));
    }

protected:
    QLabel *m_nameDisplayLabel;
    QLabel *m_varsBatDisplayLabel;
};

// --------------------------------------------------------------------------
// MsvcToolchainConfigWidget
// --------------------------------------------------------------------------

class MsvcToolchainConfigWidget final : public MsvcBasedToolchainConfigWidget
{
public:
    explicit MsvcToolchainConfigWidget(const ToolchainBundle &bundle)
        : MsvcBasedToolchainConfigWidget(bundle)
        , m_varsBatPathCombo(new QComboBox(this))
        , m_varsBatArchCombo(new QComboBox(this))
        , m_varsBatArgumentsEdit(new QLineEdit(this))
        , m_abiWidget(new AbiWidget)
    {
        m_mainLayout->removeRow(m_mainLayout->rowCount() - 1);

        QHBoxLayout *hLayout = new QHBoxLayout();
        m_varsBatPathCombo->setObjectName("varsBatCombo");
        m_varsBatPathCombo->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
        m_varsBatPathCombo->setEditable(true);
        for (const MsvcToolchain *tmpTc : std::as_const(g_availableMsvcToolchains)) {
            const QString nativeVcVars = QDir::toNativeSeparators(tmpTc->varsBat());
            if (!tmpTc->varsBat().isEmpty()
                && m_varsBatPathCombo->findText(nativeVcVars) == -1) {
                m_varsBatPathCombo->addItem(nativeVcVars);
            }
        }
        const bool isAmd64 = Utils::HostOsInfo::hostArchitecture() == Utils::OsArchAMD64;
        // TODO: Add missing values to MsvcToolChain::Platform
        m_varsBatArchCombo->addItem(Tr::tr("<empty>"), isAmd64 ? MsvcToolchain::amd64 : MsvcToolchain::x86);
        m_varsBatArchCombo->addItem("x86", MsvcToolchain::x86);
        m_varsBatArchCombo->addItem("amd64", MsvcToolchain::amd64);
        m_varsBatArchCombo->addItem("arm", MsvcToolchain::arm);
        m_varsBatArchCombo->addItem("x86_amd64", MsvcToolchain::x86_amd64);
        m_varsBatArchCombo->addItem("x86_arm", MsvcToolchain::x86_arm);
        m_varsBatArchCombo->addItem("x86_arm64", MsvcToolchain::x86_arm64);
        m_varsBatArchCombo->addItem("amd64_x86", MsvcToolchain::amd64_x86);
        m_varsBatArchCombo->addItem("amd64_arm", MsvcToolchain::amd64_arm);
        m_varsBatArchCombo->addItem("amd64_arm64", MsvcToolchain::amd64_arm64);
        m_varsBatArchCombo->addItem("ia64", MsvcToolchain::ia64);
        m_varsBatArchCombo->addItem("x86_ia64", MsvcToolchain::x86_ia64);
        m_varsBatArchCombo->addItem("arm64", MsvcToolchain::arm64);
        m_varsBatArchCombo->addItem("arm64_x86", MsvcToolchain::arm64_x86);
        m_varsBatArchCombo->addItem("arm64_amd64", MsvcToolchain::arm64_amd64);
        m_varsBatArgumentsEdit->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        m_varsBatArgumentsEdit->setToolTip(Tr::tr("Additional arguments for the vcvarsall.bat call"));
        hLayout->addWidget(m_varsBatPathCombo);
        hLayout->addWidget(m_varsBatArchCombo);
        hLayout->addWidget(m_varsBatArgumentsEdit);
        m_mainLayout->addRow(Tr::tr("Initialization:"), hLayout);
        m_mainLayout->addRow(Tr::tr("&ABI:"), m_abiWidget);
        addErrorLabel();
        setFromMsvcToolchain();

        connect(m_varsBatPathCombo, &QComboBox::currentTextChanged,
                this, &MsvcToolchainConfigWidget::handleVcVarsChange);
        connect(m_varsBatArchCombo, &QComboBox::currentTextChanged,
                this, &MsvcToolchainConfigWidget::handleVcVarsArchChange);
        connect(m_varsBatArgumentsEdit, &QLineEdit::textChanged,
                this, &ToolchainConfigWidget::dirty);
        connect(m_abiWidget, &AbiWidget::abiChanged, this, &ToolchainConfigWidget::dirty);
    }

private:
    void applyImpl() final;
    void discardImpl() final;
    bool isDirtyImpl() const final;
    void makeReadOnlyImpl() final;

    void setFromMsvcToolchain();

    void updateAbis();
    void handleVcVarsChange(const QString &vcVars);
    void handleVcVarsArchChange(const QString &arch);

    QString vcVarsArguments() const;

    QComboBox *m_varsBatPathCombo;
    QComboBox *m_varsBatArchCombo;
    QLineEdit *m_varsBatArgumentsEdit;
    AbiWidget *m_abiWidget;
};

void MsvcToolchainConfigWidget::applyImpl()
{
    const QString vcVars = QDir::fromNativeSeparators(m_varsBatPathCombo->currentText());
    bundle().set(&MsvcToolchain::setupVarsBat, m_abiWidget->currentAbi(), vcVars, vcVarsArguments());
    setFromMsvcToolchain();
}

void MsvcToolchainConfigWidget::discardImpl()
{
    setFromMsvcToolchain();
}

bool MsvcToolchainConfigWidget::isDirtyImpl() const
{
    return bundle().get(&MsvcToolchain::varsBat)
               != QDir::fromNativeSeparators(m_varsBatPathCombo->currentText())
           || bundle().get(&MsvcToolchain::varsBatArg) != vcVarsArguments()
           || bundle().get(&MsvcToolchain::targetAbi) != m_abiWidget->currentAbi();
}

void MsvcToolchainConfigWidget::makeReadOnlyImpl()
{
    m_varsBatPathCombo->setEnabled(false);
    m_varsBatArchCombo->setEnabled(false);
    m_varsBatArgumentsEdit->setEnabled(false);
    m_abiWidget->setEnabled(false);
}

void MsvcToolchainConfigWidget::setFromMsvcToolchain()
{
    QString args = bundle().get(&MsvcToolchain::varsBatArg);
    QStringList argList = args.split(' ');
    for (int i = 0; i < argList.count(); ++i) {
        if (m_varsBatArchCombo->findText(argList.at(i).trimmed()) != -1) {
            const QString arch = argList.takeAt(i);
            m_varsBatArchCombo->setCurrentText(arch);
            args = argList.join(QLatin1Char(' '));
            break;
        }
    }
    m_varsBatPathCombo->setCurrentText(
        QDir::toNativeSeparators(bundle().get(&MsvcToolchain::varsBat)));
    m_varsBatArgumentsEdit->setText(args);
    m_abiWidget->setAbis(bundle().supportedAbis(), bundle().targetAbi());
}

void MsvcToolchainConfigWidget::updateAbis()
{
    const QString normalizedVcVars = QDir::fromNativeSeparators(m_varsBatPathCombo->currentText());
    const MsvcToolchain::Platform platform = m_varsBatArchCombo->currentData().value<MsvcToolchain::Platform>();
    const Abi::Architecture arch = archForPlatform(platform);
    const unsigned char wordWidth = wordWidthForPlatform(platform);

    // Search the selected vcVars bat file in already detected MSVC compilers.
    // For each variant of MSVC found, add its supported ABIs to the ABI widget so the user can
    // choose one appropriately.
    Abis supportedAbis;
    Abi targetAbi;
    for (const MsvcToolchain *tc : std::as_const(g_availableMsvcToolchains)) {
        if (tc->varsBat() == normalizedVcVars && tc->targetAbi().wordWidth() == wordWidth
            && tc->targetAbi().architecture() == arch) {
            // We need to filter out duplicates as there might be multiple toolchains with
            // same abi (like x86, amd64_x86 for example).
            for (const Abi &abi : tc->supportedAbis()) {
                if (!supportedAbis.contains(abi))
                    supportedAbis.append(abi);
            }
            targetAbi = tc->targetAbi();
        }
    }

    // If we didn't find an exact match, try to find a fallback according to varsBat only.
    // This can happen when the toolchain does not support user-selected arch/wordWidth.
    if (!targetAbi.isValid()) {
        const MsvcToolchain *tc = Utils::findOrDefault(g_availableMsvcToolchains,
                                                       [normalizedVcVars](const MsvcToolchain *tc) {
                                                           return tc->varsBat() == normalizedVcVars;
                                                       });
        if (tc) {
            targetAbi = Abi(arch,
                            tc->targetAbi().os(),
                            tc->targetAbi().osFlavor(),
                            tc->targetAbi().binaryFormat(),
                            wordWidth);
        }
    }

    // Always set ABIs, even if none was found, to prevent stale data in the ABI widget.
    // In that case, a custom ABI will be selected according to targetAbi.
    m_abiWidget->setAbis(supportedAbis, targetAbi);

    emit dirty();
}

void MsvcToolchainConfigWidget::handleVcVarsChange(const QString &)
{
    updateAbis();
}

void MsvcToolchainConfigWidget::handleVcVarsArchChange(const QString &)
{
    // supportedAbi list in the widget only contains matching ABIs to whatever arch was selected.
    // We need to reupdate it from scratch with new arch parameters
    updateAbis();
}

QString MsvcToolchainConfigWidget::vcVarsArguments() const
{
    QString varsBatArg
            = m_varsBatArchCombo->currentText() == Tr::tr("<empty>")
            ? "" : m_varsBatArchCombo->currentText();
    if (!m_varsBatArgumentsEdit->text().isEmpty())
        varsBatArg += QLatin1Char(' ') + m_varsBatArgumentsEdit->text();
    return varsBatArg;
}

// --------------------------------------------------------------------------
// ClangClToolChainConfigWidget
// --------------------------------------------------------------------------

class ClangClToolchainConfigWidget final : public MsvcBasedToolchainConfigWidget
{
public:
    explicit ClangClToolchainConfigWidget(const ToolchainBundle &bundle)
        : MsvcBasedToolchainConfigWidget(bundle)
        , m_varsBatDisplayCombo(new QComboBox(this))
    {
        m_mainLayout->removeRow(m_mainLayout->rowCount() - 1);

        m_varsBatDisplayCombo->setObjectName("varsBatCombo");
        m_varsBatDisplayCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        m_mainLayout->addRow(Tr::tr("Initialization:"), m_varsBatDisplayCombo);
        setCommandVersionArguments(QStringList("--version"));
        addErrorLabel();
        setFromClangClToolchain();
    }

protected:
    void applyImpl() final;
    void discardImpl() final;
    bool isDirtyImpl() const override { return false; }
    void makeReadOnlyImpl() final;

private:
    void setFromClangClToolchain();

    QComboBox *m_varsBatDisplayCombo = nullptr;
};

void ClangClToolchainConfigWidget::setFromClangClToolchain()
{
    m_varsBatDisplayCombo->clear();
    m_varsBatDisplayCombo->addItem(msvcVarsToDisplay(bundle()));
    for (const MsvcToolchain *tc : std::as_const(g_availableMsvcToolchains)) {
        const QString varsToDisplay = msvcVarsToDisplay(*tc);
        if (m_varsBatDisplayCombo->findText(varsToDisplay) == -1)
            m_varsBatDisplayCombo->addItem(varsToDisplay);
    }
}

class ClangClInfo
{
public:
    FilePath filePath() const { return m_filePath; }
    QVersionNumber version() const { return m_version; }
    Abi defaultAbi() const { return m_defaultAbi; }

    static ClangClInfo getInfo(const FilePath &filePath);

private:
    FilePath m_filePath;
    QVersionNumber m_version;
    Abi m_defaultAbi;
};

static const MsvcToolchain *selectMsvcToolChain(const QString &displayedVarsBat,
                                                const FilePath &clangClPath)
{
    if (!displayedVarsBat.isEmpty()) {
        if (const auto tc = Utils::findOrDefault(g_availableMsvcToolchains,
                                                 [&displayedVarsBat](const MsvcToolchain *tc) {
                                                     return msvcVarsToDisplay(*tc)
                                                            == displayedVarsBat;
                                                 }))
            return tc;
    }

    QTC_CHECK(displayedVarsBat.isEmpty());
    const ClangClInfo clangClInfo = ClangClInfo::getInfo(clangClPath);
    QList<Abi::OSFlavor> flavors;
    if (clangClInfo.version().majorVersion() >= 6)
        flavors << Abi::WindowsMsvc2022Flavor << Abi::WindowsMsvc2019Flavor
                << Abi::WindowsMsvc2017Flavor;
    flavors << Abi::WindowsMsvc2015Flavor << Abi::WindowsMsvc2013Flavor;
    for (const Abi::OSFlavor flavor : std::as_const(flavors)) {
        if (const auto tc = Utils::findOrDefault(g_availableMsvcToolchains,
                                                 [&clangClInfo, flavor](const MsvcToolchain *tc) {
                                                     const Abi abi = tc->targetAbi();
                                                     return abi.osFlavor() == flavor
                                                            && abi.isCompatibleWith(
                                                                clangClInfo.defaultAbi())
                                                            && tc->hostPrefersToolchain();
                                                 })) {
            return tc;
        }
    }
    return nullptr;
}

static Toolchains detectClangClToolChainInPath(const FilePath &clangClPath,
                                               const Toolchains &alreadyKnown,
                                               const QString &displayedVarsBat,
                                               bool isDefault = false)
{
    Toolchains res;
    const MsvcToolchain *toolChain = selectMsvcToolChain(displayedVarsBat, clangClPath);

    if (!toolChain) {
        qWarning("Unable to find a suitable MSVC version for \"%s\".",
                 qPrintable(clangClPath.toUserOutput()));
        return res;
    }

    const Abi targetAbi = toolChain->targetAbi();
    const QString name = QString("%1LLVM %2 bit based on %3")
                             .arg(QLatin1String(isDefault ? "Default " : ""))
                             .arg(targetAbi.wordWidth())
                             .arg(Abi::toString(targetAbi.osFlavor()).toUpper());
    for (const Id language : {Id(Constants::C_LANGUAGE_ID), Id(Constants::CXX_LANGUAGE_ID)}) {
        if (Utils::findOrDefault(alreadyKnown, [&](Toolchain *tc) {
                if (tc->typeId() != Constants::CLANG_CL_TOOLCHAIN_TYPEID)
                    return false;
                if (tc->targetAbi() != targetAbi)
                    return false;
                if (tc->language() != language)
                    return false;
                return tc->compilerCommand().isSameExecutable(clangClPath);
            })) {
            continue;
        }

        auto cltc = new ClangClToolchain;
        cltc->setClangPath(clangClPath);
        cltc->setDisplayName(name);
        cltc->setDetectionSource(DetectionSource::FromSystem);
        cltc->setLanguage(language);
        cltc->setupVarsBat(toolChain->targetAbi(), toolChain->varsBat(), toolChain->varsBatArg());
        res << cltc;
    }
    return res;
}

void ClangClToolchainConfigWidget::applyImpl()
{
    const FilePath clangClPath = bundle().get(&ClangClToolchain::clangPath);
    if (clangClPath.fileName() != "clang-cl.exe") {
        bundle().set(&ClangClToolchain::resetVarsBat);
        setFromClangClToolchain();
        return;
    }

    const QString displayedVarsBat = m_varsBatDisplayCombo->currentText();
    Toolchains results = detectClangClToolChainInPath(clangClPath, {}, displayedVarsBat);
    const QList<ToolchainBundle> bundles
        = ToolchainBundle::collectBundles(results, ToolchainBundle::HandleMissing::NotApplicable);

    if (bundles.isEmpty()) {
        bundle().set(&ClangClToolchain::resetVarsBat);
    } else {
        const ToolchainBundle &b = bundles.first();
        bundle().set(
            &MsvcToolchain::setupVarsBat,
            b.targetAbi(),
            b.get(&MsvcToolchain::varsBat),
            b.get(&MsvcToolchain::varsBatArg));
    }
    qDeleteAll(results);
    setFromClangClToolchain();
}

void ClangClToolchainConfigWidget::discardImpl()
{
    setFromClangClToolchain();
}

void ClangClToolchainConfigWidget::makeReadOnlyImpl()
{
    m_varsBatDisplayCombo->setEnabled(false);
}

// --------------------------------------------------------------------------
// ClangClToolChain, piggy-backing on MSVC2015 and providing the compiler
// clang-cl.exe as a [to some extent] compatible drop-in replacement for cl.
// --------------------------------------------------------------------------

ClangClToolchain::ClangClToolchain()
    : MsvcToolchain(Constants::CLANG_CL_TOOLCHAIN_TYPEID)
{
    setDisplayName("clang-cl");
    setTypeDisplayName(Tr::tr("Clang"));
    setVersionFlagsAndParser(
        {"--version"}, [](const QString &output, const QString &) -> QVersionNumber {
            const QString marker = "clang version ";
            const int markerIndex = output.indexOf(marker);
            if (markerIndex == -1)
                return {};
            const int versionOffset = markerIndex + marker.size();
            const int newlineIndex = output.indexOf('\n', versionOffset);
            if (newlineIndex == -1)
                return {};
            return QVersionNumber::fromString(
                output.mid(versionOffset, newlineIndex - versionOffset).trimmed());
        });
}

bool ClangClToolchain::isValid() const
{
    const FilePath clang = clangPath();
    return MsvcToolchain::isValid() && clang.exists() && clang.fileName() == "clang-cl.exe";
}

void ClangClToolchain::addToEnvironment(Utils::Environment &env) const
{
    MsvcToolchain::addToEnvironment(env);
    env.prependOrSetPath(m_clangPath.parentDir()); // bin folder
}

Utils::FilePath ClangClToolchain::compilerCommand() const
{
    return m_clangPath;
}

QStringList ClangClToolchain::suggestedMkspecList() const
{
    const QString mkspec = "win32-clang-" + Abi::toString(targetAbi().osFlavor());
    return {mkspec, "win32-clang-msvc"};
}

QList<OutputLineParser *> ClangClToolchain::createOutputParsers() const
{
    return {new ClangClParser};
}

static Key llvmDirKey()
{
    return "ProjectExplorer.ClangClToolChain.LlvmDir";
}

void ClangClToolchain::toMap(Store &data) const
{
    MsvcToolchain::toMap(data);
    data.insert(llvmDirKey(), m_clangPath.toUrlishString());
}

void ClangClToolchain::fromMap(const Store &data)
{
    MsvcToolchain::fromMap(data);
    if (hasError())
        return;

    const QString clangPath = data.value(llvmDirKey()).toString();
    if (clangPath.isEmpty()) {
        reportError();
        return;
    }

    m_clangPath = FilePath::fromString(clangPath);
}

bool ClangClToolchain::operator==(const Toolchain &other) const
{
    if (!MsvcToolchain::operator==(other))
        return false;

    const auto *clangClTc = static_cast<const ClangClToolchain *>(&other);
    return m_clangPath == clangClTc->m_clangPath;
}

int ClangClToolchain::priority() const
{
    return MsvcToolchain::priority() - 1;
}

bool ClangClToolchain::canShareBundleImpl(const Toolchain &other) const
{
    return MsvcToolchain::canShareBundleImpl(other)
           && m_clangPath == static_cast<const ClangClToolchain &>(other).m_clangPath;
}

Macros ClangClToolchain::msvcPredefinedMacros(const QStringList &cxxflags,
                                              const Utils::Environment &env) const
{
    if (!cxxflags.contains("--driver-mode=g++"))
        return MsvcToolchain::msvcPredefinedMacros(cxxflags, env);

    Process cpp;
    cpp.setEnvironment(env);
    cpp.setWorkingDirectory(Utils::TemporaryDirectory::masterDirectoryFilePath());
    cpp.setCommand({compilerCommand(), {cxxflags, gccPredefinedMacrosOptions(language()), "-"}});
    cpp.runBlocking();
    if (cpp.result() != ProcessResult::FinishedWithSuccess) {
        // Show the warning but still parse the output.
        QTC_CHECK(false && "clang-cl exited with non-zero code.");
    }

    return Macro::toMacros(cpp.allRawOutput());
}

LanguageVersion ClangClToolchain::msvcLanguageVersion(const QStringList &cxxflags,
                                                      const Id &language,
                                                      const Macros &macros) const
{
    if (cxxflags.contains("--driver-mode=g++"))
        return Toolchain::languageVersion(language, macros);
    return MsvcToolchain::msvcLanguageVersion(cxxflags, language, macros);
}

ClangClToolchain::BuiltInHeaderPathsRunner ClangClToolchain::createBuiltInHeaderPathsRunner(
        const Environment &env) const
{
    {
        QMutexLocker locker(&m_headerPathsMutex);
        m_headerPathsPerEnv.clear();
    }

    return MsvcToolchain::createBuiltInHeaderPathsRunner(env);
}

// --------------------------------------------------------------------------
// MsvcToolchainFactory
// --------------------------------------------------------------------------

class MsvcToolchainFactory : public ToolchainFactory
{
public:
    MsvcToolchainFactory()
    {
        setDisplayName(Tr::tr("MSVC"));
        setSupportedToolchainType(Constants::MSVC_TOOLCHAIN_TYPEID);
        setSupportedLanguages({Constants::C_LANGUAGE_ID, Constants::CXX_LANGUAGE_ID});
        setToolchainConstructor([] { return new MsvcToolchain(Constants::MSVC_TOOLCHAIN_TYPEID); });
        setUserCreatable(true);
    }

    Toolchains autoDetect(const ToolchainDetector &detector) const final;

    std::unique_ptr<ToolchainConfigWidget> createConfigurationWidget(
        const ToolchainBundle &bundle) const override
    {
        return std::make_unique<MsvcToolchainConfigWidget>(bundle);
    }

    static QString vcVarsBatFor(const QString &basePath,
                                MsvcToolchain::Platform platform,
                                const QVersionNumber &v);
};

QString MsvcToolchainFactory::vcVarsBatFor(const QString &basePath,
                                           MsvcToolchain::Platform platform,
                                           const QVersionNumber &v)
{
    QString result;
    if (const MsvcPlatform *p = platformEntry(platform)) {
        result += basePath;
        // Starting with 15.0 (MSVC2017), the .bat are in one folder.
        if (v.majorVersion() <= 14)
            result += QLatin1String(p->prefix);
        result += QLatin1Char('/');
        result += QLatin1String(p->bat);
    }
    return result;
}

static Toolchains findOrCreateToolchains(const ToolchainDetector &detector,
                                         const QString &name,
                                         const Abi &abi,
                                         const QString &varsBat,
                                         const QString &varsBatArg)
{
    Toolchains res;
    for (auto language : {Id(Constants::C_LANGUAGE_ID), Id(Constants::CXX_LANGUAGE_ID)}) {
        Toolchain *tc = Utils::findOrDefault(detector.alreadyKnown, [&](Toolchain *tc) -> bool {
            if (tc->typeId() != Constants::MSVC_TOOLCHAIN_TYPEID)
                return false;
            if (tc->targetAbi() != abi)
                return false;
            if (tc->language() != language)
                return false;
            auto mtc = static_cast<MsvcToolchain *>(tc);
            return mtc->varsBat() == varsBat && mtc->varsBatArg() == varsBatArg;
        });
        if (tc) {
            res << tc;
        } else {
            auto mstc = new MsvcToolchain(Constants::MSVC_TOOLCHAIN_TYPEID);
            mstc->setupVarsBat(abi, varsBat, varsBatArg);
            mstc->setDisplayName(name);
            mstc->setLanguage(language);
            res << mstc;
        }
    }
    return res;
}

// Detect build tools introduced with MSVC2015
static void detectCppBuildTools2015(Toolchains *list)
{
    struct Entry
    {
        const char *postFix;
        const char *varsBatArg;
        Abi::Architecture architecture;
        Abi::BinaryFormat format;
        unsigned char wordSize;
    };

    const Entry entries[] = {{" (x86)", "x86", Abi::X86Architecture, Abi::PEFormat, 32},
                             {" (x64)", "amd64", Abi::X86Architecture, Abi::PEFormat, 64},
                             {" (x86_arm)", "x86_arm", Abi::ArmArchitecture, Abi::PEFormat, 32},
                             {" (x64_arm)", "amd64_arm", Abi::ArmArchitecture, Abi::PEFormat, 32},
                             {" (x86_arm64)", "x86_arm64", Abi::ArmArchitecture, Abi::PEFormat, 64},
                             {" (x64_arm64)", "amd64_arm64", Abi::ArmArchitecture, Abi::PEFormat, 64}};

    const QString name = "Microsoft Visual C++ Build Tools";
    const QString vcVarsBat = windowsProgramFilesDir() + '/' + name + "/vcbuildtools.bat";
    if (!QFileInfo(vcVarsBat).isFile())
        return;
    for (const Entry &e : entries) {
        const Abi abi(e.architecture,
                      Abi::WindowsOS,
                      Abi::WindowsMsvc2015Flavor,
                      e.format,
                      e.wordSize);
        for (auto language : {Id(Constants::C_LANGUAGE_ID), Id(Constants::CXX_LANGUAGE_ID)}) {
            auto tc = new MsvcToolchain(Constants::MSVC_TOOLCHAIN_TYPEID);
            tc->setupVarsBat(abi, vcVarsBat, QLatin1String(e.varsBatArg));
            tc->setDisplayName(name + QLatin1String(e.postFix));
            tc->setDetectionSource(DetectionSource::FromSystem);
            tc->setLanguage(language);
            list->append(tc);
        }
    }
}

Toolchains MsvcToolchainFactory::autoDetect(const ToolchainDetector &detector) const
{
    if (detector.device->type() != ProjectExplorer::Constants::DESKTOP_DEVICE_TYPE) {
        // FIXME currently no support for msvc toolchains on a device
        return {};
    }

    Toolchains results;

    // 1) Installed SDKs preferred over standalone Visual studio
    const QSettings
        sdkRegistry(QLatin1String(
                        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Microsoft SDKs\\Windows"),
                    QSettings::NativeFormat);
    const QString defaultSdkPath = sdkRegistry.value(QLatin1String("CurrentInstallFolder"))
                                       .toString();
    if (!defaultSdkPath.isEmpty()) {
        const QStringList groups = sdkRegistry.childGroups();
        for (const QString &sdkKey : groups) {
            const QString name = sdkRegistry.value(sdkKey + QLatin1String("/ProductName")).toString();
            const QString folder = sdkRegistry.value(sdkKey + QLatin1String("/InstallationFolder"))
                                       .toString();
            if (folder.isEmpty())
                continue;

            QDir dir(folder);
            if (!dir.cd(QLatin1String("bin")))
                continue;
            QFileInfo fi(dir, QLatin1String("SetEnv.cmd"));
            if (!fi.exists())
                continue;

            QList<Toolchain *> tmp;
            const QList<QPair<MsvcToolchain::Platform, QString>> platforms = {
                {MsvcToolchain::x86, "x86"},
                {MsvcToolchain::amd64, "x64"},
                {MsvcToolchain::ia64, "ia64"},
                {MsvcToolchain::arm64, "arm64"},
            };
            for (const auto &platform : platforms) {
                tmp.append(findOrCreateToolchains(detector,
                                                  generateDisplayName(name,
                                                                      MsvcToolchain::WindowsSDK,
                                                                      platform.first),
                                                  findAbiOfMsvc(MsvcToolchain::WindowsSDK,
                                                                platform.first,
                                                                sdkKey),
                                                  fi.absoluteFilePath(),
                                                  "/" + platform.second));
            }
            // Make sure the default is front.
            if (folder == defaultSdkPath)
                results = tmp + results;
            else
                results += tmp;
        } // for
    }

    // 2) Installed MSVCs
    // prioritized list.
    // x86_arm was put before amd64_arm as a workaround for auto detected windows phone
    // toolchains. As soon as windows phone builds support x64 cross builds, this change
    // can be reverted.
    const MsvcToolchain::Platform platforms[] = {MsvcToolchain::x86,
                                                 MsvcToolchain::amd64_x86,
                                                 MsvcToolchain::amd64,
                                                 MsvcToolchain::x86_amd64,
                                                 MsvcToolchain::arm,
                                                 MsvcToolchain::x86_arm,
                                                 MsvcToolchain::amd64_arm,
                                                 MsvcToolchain::x86_arm64,
                                                 MsvcToolchain::amd64_arm64,
                                                 MsvcToolchain::ia64,
                                                 MsvcToolchain::x86_ia64,
                                                 MsvcToolchain::arm64,
                                                 MsvcToolchain::arm64_x86,
                                                 MsvcToolchain::arm64_amd64};

    const QList<VisualStudioInstallation> studios = detectVisualStudio();
    for (const VisualStudioInstallation &i : studios) {
        for (MsvcToolchain::Platform platform : platforms) {
            const bool toolchainInstalled
                = QFileInfo(vcVarsBatFor(i.vcVarsPath, platform, i.version)).isFile();
            if (hostSupportsPlatform(platform) && toolchainInstalled) {
                results.append(
                    findOrCreateToolchains(detector,
                                           generateDisplayName(i.vsName, MsvcToolchain::VS, platform, i.displayName),
                                           findAbiOfMsvc(MsvcToolchain::VS, platform, i.vsName),
                                           i.vcVarsAll,
                                           platformName(platform)));
            }
        }
    }

    detectCppBuildTools2015(&results);

    for (Toolchain *tc : std::as_const(results))
        tc->setDetectionSource(DetectionSource::FromSystem);

    return results;
}

bool MsvcToolchain::operator==(const Toolchain &other) const
{
    if (!Toolchain::operator==(other))
        return false;

    const auto *msvcTc = dynamic_cast<const MsvcToolchain *>(&other);
    return targetAbi() == msvcTc->targetAbi() && m_vcvarsBat == msvcTc->m_vcvarsBat
           && m_varsBatArg == msvcTc->m_varsBatArg;
}

int MsvcToolchain::priority() const
{
    return hostPrefersToolchain() ? PriorityHigh : PriorityNormal;
}

void MsvcToolchain::cancelMsvcToolChainDetection()
{
    envModThreadPool()->clear();
}

std::optional<QString> MsvcToolchain::generateEnvironmentSettings(const Environment &env,
                                                                  const QString &batchFile,
                                                                  const QString &batchArgs,
                                                                  QMap<QString, QString> &envPairs)
{
    const QString marker = "####################";
    // Create a temporary file name for the output. Use a temporary file here
    // as I don't know another way to do this in Qt...

    // Create a batch file to create and save the env settings
    TempFileSaver saver(TemporaryDirectory::masterDirectoryPath() + "/XXXXXX.bat");

    QByteArray call = "call ";
    call += ProcessArgs::quoteArg(batchFile).toLocal8Bit();
    if (!batchArgs.isEmpty()) {
        call += ' ';
        call += batchArgs.toLocal8Bit();
    }

    if (HostOsInfo::isWindowsHost())
        saver.write("chcp 65001\r\n");
    saver.write("set VSCMD_SKIP_SENDTELEMETRY=1\r\n");
    saver.write("set CLINK_NOAUTORUN=1\r\n");
    saver.write("setlocal enableextensions\r\n");
    saver.write("if defined VCINSTALLDIR (\r\n");
    saver.write("  if not defined QTC_NO_MSVC_CLEAN_ENV (\r\n");
    saver.write("    call \"%VCINSTALLDIR%/Auxiliary/Build/vcvarsall.bat\" /clean_env\r\n");
    saver.write("  )\r\n");
    saver.write(")\r\n");
    saver.write(QByteArray(call + "\r\n"));
    saver.write(QByteArray("@echo " + marker.toLocal8Bit() + "\r\n"));
    saver.write("set\r\n");
    saver.write(QByteArray("@echo " + marker.toLocal8Bit() + "\r\n"));
    if (const Result<> &res = saver.finalize(); !res) {
        qWarning("%s: %s", Q_FUNC_INFO, qPrintable(res.error()));
        return {};
    }

    Process run;

    // As of WinSDK 7.1, there is logic preventing the path from being set
    // correctly if "ORIGINALPATH" is already set. That can cause problems
    // if Creator is launched within a session set up by setenv.cmd.
    Environment runEnv = env;
    runEnv.unset(QLatin1String("ORIGINALPATH"));
    run.setEnvironment(runEnv);
    FilePath cmdPath = FilePath::fromUserInput(qtcEnvironmentVariable("COMSPEC"));
    if (cmdPath.isEmpty())
        cmdPath = env.searchInPath(QLatin1String("cmd.exe"));
    // Windows SDK setup scripts require command line switches for environment expansion.
    CommandLine cmd(cmdPath, {"/D", "/E:ON", "/V:ON", "/c", saver.filePath().toUserOutput()});
    qCDebug(Log) << "readEnvironmentSetting: " << call << cmd.toUserOutput()
                 << " Env: " << runEnv.toStringList().size();
    run.setUtf8Codec();
    run.setCommand(cmd);
    run.runBlocking(1min);

    if (run.result() != ProcessResult::FinishedWithSuccess) {
        const QString message = run.exitMessage(Process::FailureMessageFormat::WithStdErr);
        qWarning().noquote() << message;
        QString command = QDir::toNativeSeparators(batchFile);
        if (!batchArgs.isEmpty())
            command += ' ' + batchArgs;
        return Tr::tr("Failed to retrieve MSVC Environment from \"%1\":\n%2")
            .arg(command, message);
    }

    // The SDK/MSVC scripts do not return exit codes != 0. Check on stdout.
    const QString stdOut = run.cleanedStdOut();

    //
    // Now parse the file to get the environment settings
    const int start = stdOut.indexOf(marker);
    if (start == -1) {
        qWarning("Could not find start marker in stdout output.");
        return {};
    }

    const int end = stdOut.indexOf(marker, start + 1);
    if (end == -1) {
        qWarning("Could not find end marker in stdout output.");
        return {};
    }

    const QString output = stdOut.mid(start, end - start);

    const QStringList lines = output.split(QLatin1String("\n"));
    for (const QString &line : lines) {
        const int pos = line.indexOf('=');
        if (pos > 0) {
            const QString varName = line.mid(0, pos);
            const QString varValue = line.mid(pos + 1);
            envPairs.insert(varName, varValue);
        }
    }

    return std::nullopt;
}

MsvcToolchain::WarningFlagAdder::WarningFlagAdder(const QString &flag, WarningFlags &flags)
    : m_flags(flags)
{
    if (flag.startsWith(QLatin1String("-wd"))) {
        m_doesEnable = false;
    } else if (flag.startsWith(QLatin1String("-w"))) {
        m_doesEnable = true;
    } else {
        m_triggered = true;
        return;
    }
    bool ok = false;
    if (m_doesEnable)
        m_warningCode = flag.mid(2).toInt(&ok);
    else
        m_warningCode = flag.mid(3).toInt(&ok);
    if (!ok)
        m_triggered = true;
}

void MsvcToolchain::WarningFlagAdder::operator()(int warningCode, WarningFlags flagsSet)
{
    if (m_triggered)
        return;
    if (warningCode == m_warningCode) {
        m_triggered = true;
        if (m_doesEnable)
            m_flags |= flagsSet;
        else
            m_flags &= ~flagsSet;
    }
}

bool MsvcToolchain::WarningFlagAdder::triggered() const
{
    return m_triggered;
}

// --------------------------------------------------------------------------
// ClangClToolchainFactory
// --------------------------------------------------------------------------

class ClangClToolchainFactory : public ToolchainFactory
{
public:
    ClangClToolchainFactory()
    {
        setDisplayName(Tr::tr("clang-cl"));
        setSupportedLanguages({Constants::C_LANGUAGE_ID, Constants::CXX_LANGUAGE_ID});
        setSupportedToolchainType(Constants::CLANG_CL_TOOLCHAIN_TYPEID);
        setToolchainConstructor([] { return new ClangClToolchain; });
        setUserCreatable(true);
    }

    Toolchains autoDetect(const ToolchainDetector &detector) const final;
    std::unique_ptr<ToolchainConfigWidget> createConfigurationWidget(
        const ToolchainBundle &bundle) const override
    {
        return std::make_unique<ClangClToolchainConfigWidget>(bundle);
    }
};

Toolchains ClangClToolchainFactory::autoDetect(const ToolchainDetector &detector) const
{
    if (detector.device->type() != ProjectExplorer::Constants::DESKTOP_DEVICE_TYPE) {
        // FIXME currently no support for msvc toolchains on a device
        return {};
    }
#ifdef Q_OS_WIN64
    const char registryNode[] = "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\LLVM\\LLVM";
#else
    const char registryNode[] = "HKEY_LOCAL_MACHINE\\SOFTWARE\\LLVM\\LLVM";
#endif

    Toolchains results;
    Toolchains known = detector.alreadyKnown;

    Result<FilePath> qtCreatorsClang = Core::ICore::clangExecutable(CLANG_BINDIR);
    if (qtCreatorsClang) {
        qtCreatorsClang = qtCreatorsClang->parentDir().pathAppended("clang-cl.exe");
        results.append(detectClangClToolChainInPath(*qtCreatorsClang,
                                                    detector.alreadyKnown, "", true));
        known.append(results);
    }

    const QSettings registry(QLatin1String(registryNode), QSettings::NativeFormat);
    if (registry.status() == QSettings::NoError) {
        const FilePath path = FilePath::fromUserInput(registry.value(QStringLiteral(".")).toString());
        const FilePath clangClPath = path / "bin/clang-cl.exe";
        if (!path.isEmpty()) {
            results.append(detectClangClToolChainInPath(clangClPath, known, ""));
            known.append(results);
        }
    }

    const Environment systemEnvironment = Environment::systemEnvironment();
    const FilePath clangClPath = systemEnvironment.searchInPath("clang-cl.exe");
    if (!clangClPath.isEmpty())
        results.append(detectClangClToolChainInPath(clangClPath, known, ""));

    return results;
}

void setupMsvcToolchain()
{
#ifdef Q_OS_WIN
    static MsvcToolchainFactory theMsvcToolChainFactory;
#endif
}

void setupClangClToolchain()
{
#ifdef Q_OS_WIN
    static ClangClToolchainFactory theClangClToolchainFactory;
#endif
}

ClangClInfo ClangClInfo::getInfo(const FilePath &filePath)
{
    QTC_ASSERT(!filePath.isEmpty(), return {});

    static const auto parser = [](const QString &stdOut, const QString &) {
        ClangClInfo info;
        static const QRegularExpression regexp("clang version (\\d+(\\.\\d+)+)");
        const QRegularExpressionMatch versionMatch = regexp.match(stdOut);
        if (versionMatch.hasMatch())
            info.m_version = QVersionNumber::fromString(versionMatch.captured(1));
        const QString targetKey = "Target:";
        int startOffset = stdOut.indexOf(targetKey);
        if (startOffset != -1) {
            startOffset += targetKey.length();
            const int endOffset = stdOut.indexOf('\n', startOffset);
            if (endOffset != -1) {
                const Abi detectedAbi = Abi::abiFromTargetTriplet(
                    stdOut.mid(startOffset, endOffset - startOffset).trimmed());

                // Abi::fromTargetTriple() always detects clang triples as msys,
                // but here in clang-cl context we know that's wrong.
                info.m_defaultAbi = Abi(detectedAbi.architecture(),
                                        detectedAbi.os(),
                                        Abi::UnknownFlavor,
                                        detectedAbi.binaryFormat(),
                                        detectedAbi.wordWidth());
            }
        }
        return info;
    };
    const auto info = DataFromProcess<ClangClInfo>::getData({{filePath, {"--version"}}, parser});
    return info ? *info : ClangClInfo();
}

} // namespace ProjectExplorer::Internal

Q_DECLARE_METATYPE(ProjectExplorer::Internal::MsvcToolchain::Platform)
