// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "customwizardscriptgenerator.h"
#include "customwizard.h"
#include "customwizardparameters.h"

#include <utils/environment.h>
#include <utils/fileutils.h>
#include <utils/hostosinfo.h>
#include <utils/qtcprocess.h>
#include <utils/temporarydirectory.h>

#include <QFileInfo>
#include <QDebug>

using namespace Core;
using namespace Utils;

namespace ProjectExplorer::Internal {

// Parse helper: Determine the correct binary to run:
// Expand to full wizard path if it is relative and located
// in the wizard directory, else assume it can be found in path.
// On Windows, run non-exe files with 'cmd /c'.
QStringList fixGeneratorScript(const QString &configFile, QString binary)
{
    if (binary.isEmpty())
        return {};
    // Expand to full path if it is relative and in the wizard
    // directory, else assume it can be found in path.
    QFileInfo binaryInfo(binary);
    if (!binaryInfo.isAbsolute()) {
        QString fullPath = QFileInfo(configFile).absolutePath();
        fullPath += QLatin1Char('/');
        fullPath += binary;
        const QFileInfo fullPathInfo(fullPath);
        if (fullPathInfo.isFile()) {
            binary = fullPathInfo.absoluteFilePath();
            binaryInfo = fullPathInfo;
        }
    } // not absolute
    QStringList rc(binary);
    if (Utils::HostOsInfo::isWindowsHost()) { // Windows: Cannot run scripts by QProcess, do 'cmd /c'
        const QString extension = binaryInfo.suffix();
        if (!extension.isEmpty() && extension.compare(QLatin1String("exe"),
                                                      Qt::CaseInsensitive) != 0) {
            rc.push_front(QLatin1String("/C"));
            rc.push_front(qtcEnvironmentVariable("COMSPEC"));
            if (rc.front().isEmpty())
                rc.front() = QLatin1String("cmd.exe");
        }
    }
    return rc;
}

// Helper for running the optional generation script.
static Result<>
    runGenerationScriptHelper(const FilePath &workingDirectory,
                              const QStringList &script,
                              const QList<GeneratorScriptArgument> &argumentsIn,
                              bool dryRun,
                              const QMap<QString, QString> &fieldMap,
                              QString *stdOut /* = 0 */)
{
    Utils::Process process;
    const QString binary = script.front();
    QStringList arguments;
    const int binarySize = script.size();
    for (int i = 1; i < binarySize; i++)
        arguments.push_back(script.at(i));

    // Arguments: Prepend 'dryrun' and do field replacement
    if (dryRun)
        arguments.push_back(QLatin1String("--dry-run"));

    // Arguments: Prepend 'dryrun'. Do field replacement to actual
    // argument value to expand via temporary file if specified
    CustomWizardContext::TemporaryFilePtrList temporaryFiles;
    for (const GeneratorScriptArgument &argument : argumentsIn) {
        QString value = argument.value;
        const bool nonEmptyReplacements
                = argument.flags & GeneratorScriptArgument::WriteFile ?
                    CustomWizardContext::replaceFields(fieldMap, &value, &temporaryFiles) :
                    CustomWizardContext::replaceFields(fieldMap, &value);
        if (nonEmptyReplacements || !(argument.flags & GeneratorScriptArgument::OmitEmpty))
            arguments.push_back(value);
    }
    process.setWorkingDirectory(workingDirectory);
    const Utils::CommandLine cmd(FilePath::fromString(binary), arguments);
    if (CustomWizard::verbose())
        qDebug("In %s, running:\n%s\n", qPrintable(workingDirectory.toUserOutput()),
               qPrintable(cmd.toUserOutput()));
    process.setCommand(cmd);
    using namespace std::chrono_literals;
    process.runBlocking(30s, EventLoopMode::On);
    if (process.result() != Utils::ProcessResult::FinishedWithSuccess) {
        QString errorMessage = QString("Generator script failed: %1")
                                   .arg(process.exitMessage(
                                       Process::FailureMessageFormat::WithStdErr));
        return ResultError(errorMessage);
    }
    if (stdOut) {
        *stdOut = process.cleanedStdOut();
        if (CustomWizard::verbose())
            qDebug("Output: '%s'\n", qPrintable(*stdOut));
    }
    return ResultOk;
}

/*!
    Performs the first step in custom wizard script generation.

    Does a dry run of the generation script to get a list of files.
    \sa runCustomWizardGeneratorScript, ProjectExplorer::CustomWizard
*/

Result<QList<GeneratedFile>>
    dryRunCustomWizardGeneratorScript(const FilePath &targetPath,
                                      const QStringList &script,
                                      const QList<GeneratorScriptArgument> &arguments,
                                      const QMap<QString, QString> &fieldMap)
{
    // Run in temporary directory as the target path may not exist yet.
    QString stdOut;
    const Result<> res = runGenerationScriptHelper(TemporaryDirectory::masterDirectoryFilePath(),
                                                   script, arguments, true, fieldMap, &stdOut);
    if (!res)
        return ResultError(res.error());

    GeneratedFiles files;
    // Parse the output consisting of lines with ',' separated tokens.
    // (file name + attributes matching those of the <file> element)
    const QStringList lines = stdOut.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            GeneratedFile file;
            GeneratedFile::Attributes attributes = GeneratedFile::CustomGeneratorAttribute;
            const QStringList tokens = line.split(QLatin1Char(','));
            const int count = tokens.count();
            for (int i = 0; i < count; i++) {
                const QString &token = tokens.at(i);
                if (i) {
                    if (token == QLatin1String(customWizardFileOpenEditorAttributeC))
                        attributes |= GeneratedFile::OpenEditorAttribute;
                    else if (token == QLatin1String(customWizardFileOpenProjectAttributeC))
                        attributes |= GeneratedFile::OpenProjectAttribute;
                } else {
                    // Token 0 is file name. Wizard wants native names.
                    // Expand to full path if relative
                    file.setFilePath(targetPath.resolvePath(token));
                }
            }
            file.setAttributes(attributes);
            files.push_back(file);
        }
    }
    if (CustomWizard::verbose()) {
        QDebug nospace = qDebug().nospace();
        nospace << script << " generated:\n";
        for (const GeneratedFile &f : std::as_const(files))
            nospace << ' ' << f.filePath() << f.attributes() << '\n';
    }
    return files;
}

/*!
    Performs the second step in custom wizard script generation by actually
    creating the files.

    In addition to the <file> elements
    that define template files in which macros are replaced, it is possible to have
    a custom wizard call a generation script (specified in the \a generatorscript
    attribute of the <files> element) which actually creates files.
    The command line of the script must follow the convention
    \code
    script [--dry-run] [options]
    \endcode

    Options containing field placeholders are configured in the XML files
    and will be passed with them replaced by their values.

    As \QC needs to know the file names before it actually creates the files to
    do overwrite checking etc., this is  2-step process:
    \list
    \li Determine file names and attributes: The script is called with the
      \c --dry-run option and the field values. It then prints the relative path
      names it intends to create followed by comma-separated attributes
     matching those of the \c <file> element, for example:
     \c myclass.cpp,openeditor
    \li The script is called with the parameters only in the working directory
    and then actually creates the files. If that involves directories, the script
    should create those, too.
    \endlist

    \sa dryRunCustomWizardGeneratorScript, ProjectExplorer::CustomWizard
 */

Result<> runCustomWizardGeneratorScript(const FilePath &targetPath,
                                        const QStringList &script,
                                        const QList<GeneratorScriptArgument> &arguments,
                                        const QMap<QString, QString> &fieldMap)
{
    return runGenerationScriptHelper(targetPath, script, arguments, false, fieldMap, nullptr);
}

} // namespace ProjectExplorer::Internal
