// Copyright (C) 2023 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0+ OR GPL-3.0 WITH Qt-GPL-exception-1.0

#include "copilotsettings.h"

#include "authwidget.h"
#include "copilotconstants.h"
#include "copilottr.h"

#include <coreplugin/coreconstants.h>
#include <coreplugin/dialogs/ioptionspage.h>

#include <projectexplorer/project.h>

#include <utils/algorithm.h>
#include <utils/layoutbuilder.h>
#include <utils/pathchooser.h>

#include <QToolTip>

using namespace Utils;

namespace Copilot {

static void initEnableAspect(BoolAspect &enableCopilot)
{
    enableCopilot.setSettingsKey(Constants::ENABLE_COPILOT);
    enableCopilot.setDisplayName(Tr::tr("Enable Copilot"));
    enableCopilot.setLabelText(Tr::tr("Enable Copilot"));
    enableCopilot.setToolTip(Tr::tr("Enables the Copilot integration."));
    enableCopilot.setDefaultValue(false);
}

CopilotSettings &settings()
{
    static CopilotSettings settings;
    return settings;
}

static const QString entryPointFileName = QStringLiteral("language-server.js");

CopilotSettings::CopilotSettings()
{
    setAutoApply(false);

    const FilePath nodeFromPath = FilePath("node").searchInPath();

    // From: https://github.com/github/copilot.vim/blob/release/README.md#getting-started
    const QStringList subLocations
        = {"dist/agent.js",
           "copilot/dist/agent.js",
           "dist/language-server.js",
           "copilot-language-server/dist/language-server.js"};

    const QString baseDir = "pack/github/start/copilot.vim";

    const FilePaths locations = {
        // Vim, Linux/macOS:
        FilePath::fromUserInput("~/.vim") / baseDir,

        // Neovim, Linux/macOS:
        FilePath::fromUserInput("~/.config/nvim") / baseDir,

        // Vim, Windows (PowerShell command):
        FilePath::fromUserInput("~/vimfiles") / baseDir,

        // Neovim, Windows (PowerShell command):
        FilePath::fromUserInput("~/AppData/Local/nvim") / baseDir,
    };

    FilePaths searchDirs;
    for (const auto &loc : locations) {
        for (const auto &subLocation : subLocations)
            searchDirs.append(loc / subLocation);
    }

    const FilePath distFromVim = findOrDefault(searchDirs, &FilePath::exists);

    nodeJsPath.setExpectedKind(PathChooser::ExistingCommand);
    nodeJsPath.setDefaultPathValue(nodeFromPath);
    nodeJsPath.setSettingsKey("Copilot.NodeJsPath");
    nodeJsPath.setLabelText(Tr::tr("Node.js path:"));
    nodeJsPath.setHistoryCompleter("Copilot.NodePath.History");
    nodeJsPath.setDisplayName(Tr::tr("Node.js Path"));
    //: %1 is the URL to nodejs
    nodeJsPath.setToolTip(Tr::tr("Select path to node.js executable. See %1 "
                                 "for installation instructions.")
                              .arg("https://nodejs.org/en/download/"));

    distPath.setExpectedKind(PathChooser::File);
    distPath.setDefaultPathValue(distFromVim);
    distPath.setSettingsKey("Copilot.DistPath");
    //: %1 is the filename of the copilot language server
    distPath.setLabelText(Tr::tr("Path to %1:").arg(entryPointFileName));
    distPath.setHistoryCompleter("Copilot.DistPath.History");
    //: %1 is the filename of the copilot language server
    distPath.setDisplayName(Tr::tr("%1 path").arg(entryPointFileName));
    //: %1 is the URL to copilot.vim getting started, %2 is the filename of the copilot language server
    distPath.setToolTip(Tr::tr("Select path to %2 in Copilot Neovim plugin. See "
                               "%1 for installation instructions.")
                            .arg("https://github.com/github/copilot.vim#getting-started")
                            .arg(entryPointFileName));

    autoComplete.setDisplayName(Tr::tr("Auto Request"));
    autoComplete.setSettingsKey("Copilot.Autocomplete");
    autoComplete.setLabelText(Tr::tr("Auto request"));
    autoComplete.setDefaultValue(true);
    autoComplete.setToolTip(Tr::tr("Automatically request suggestions for the current text cursor "
                                   "position after changes to the document."));

    proxy.setDisplayName(Tr::tr("Proxy"));
    proxy.setDisplayStyle(StringAspect::DisplayStyle::LineEditDisplay);
    proxy.setSettingsKey("Copilot.Proxy");
    proxy.setLabelText(Tr::tr("Proxy:"));
    proxy.setDefaultValue("");
    proxy.setPlaceHolderText("http://localhost:3128");
    proxy.setToolTip(Tr::tr("The proxy server to use for connections."));

    proxyRejectUnauthorized.setDisplayName(Tr::tr("Reject Unauthorized"));
    proxyRejectUnauthorized.setSettingsKey("Copilot.ProxyRejectUnauthorized");
    proxyRejectUnauthorized.setLabelText(Tr::tr("Reject unauthorized"));
    proxyRejectUnauthorized.setDefaultValue(true);
    proxyRejectUnauthorized.setToolTip(Tr::tr("Reject unauthorized certificates from the proxy "
                                              "server. Turning this off is a security risk."));

    githubEnterpriseUrl.setDisplayName(Tr::tr("GitHub Enterprise URL"));
    githubEnterpriseUrl.setDisplayStyle(StringAspect::DisplayStyle::LineEditDisplay);
    githubEnterpriseUrl.setSettingsKey("Copilot.GithubEnterpriseUrl");
    githubEnterpriseUrl.setLabelText(Tr::tr("GitHub Enterprise URL:"));
    githubEnterpriseUrl.setDefaultValue("");
    githubEnterpriseUrl.setToolTip(Tr::tr("The URL of your GitHub Enterprise server."));

    initEnableAspect(enableCopilot);

    readSettings();

    // TODO: As a workaround we set the enabler after reading the settings, as that does not signal
    // a change.
    nodeJsPath.setEnabler(&enableCopilot);
    distPath.setEnabler(&enableCopilot);
    autoComplete.setEnabler(&enableCopilot);
    githubEnterpriseUrl.setEnabler(&enableCopilot);

    proxy.setEnabler(&enableCopilot);
    proxyRejectUnauthorized.setEnabler(&enableCopilot);

    setLayouter([this] {
        using namespace Layouting;

        // clang-format off

        Label warningLabel {
            wordWrap(true),
            textInteractionFlags(
                Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard | Qt::TextSelectableByMouse),
            text(Tr::tr("Enabling %1 is subject to your agreement and abidance with your applicable "
                 "%1 terms. It is your responsibility to know and accept the requirements and "
                 "parameters of using tools like %1. This may include, but is not limited to, "
                 "ensuring you have the rights to allow %1 access to your code, as well as "
                 "understanding any implications of your use of %1 and suggestions produced "
                 "(like copyright, accuracy, etc.).")
                       .arg("Copilot")),
        };

        Label helpLabel {
            textFormat(Qt::MarkdownText),
            wordWrap(true),
            textInteractionFlags(
                Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard | Qt::TextSelectableByMouse),
            openExternalLinks(true),
            onLinkHovered(this, [](const QString &link) { QToolTip::showText(QCursor::pos(), link); }),
            text(Tr::tr(
                "The Copilot plugin requires node.js and the Copilot neovim plugin. "
                "If you install the neovim plugin as described in %1, "
                "the plugin will find the %3 file automatically.\n\n"
                "Otherwise you need to specify the path to the %2 "
                "file from the Copilot neovim plugin.",
                "Markdown text for the copilot instruction label")
                       .arg("[README.md](https://github.com/github/copilot.vim)")
                       .arg("[language-server.js](https://github.com/github/copilot.vim/tree/release/dist)")
                       .arg(entryPointFileName)),
        };

        return Column {
            Group {
                title(Tr::tr("Note")),
                Column {
                    warningLabel, br,
                    helpLabel, br,
                }
            },
            Form {
                new AuthWidget, br,
                enableCopilot, br,
                nodeJsPath, br,
                distPath, br,
                autoComplete, br,
                githubEnterpriseUrl, br,
                hr, br,
                proxy, br,
                proxyRejectUnauthorized, br,
            },
            st
        };
        // clang-format on
    });
}

CopilotProjectSettings::CopilotProjectSettings(ProjectExplorer::Project *project)
{
    setAutoApply(true);

    useGlobalSettings.setSettingsKey(Constants::COPILOT_USE_GLOBAL_SETTINGS);
    useGlobalSettings.setDefaultValue(true);

    initEnableAspect(enableCopilot);

    Store map = storeFromVariant(project->namedSettings(Constants::COPILOT_PROJECT_SETTINGS_ID));
    fromMap(map);

    enableCopilot.addOnChanged(this, [this, project] { save(project); });
    useGlobalSettings.addOnChanged(this, [this, project] { save(project); });
}

void CopilotProjectSettings::setUseGlobalSettings(bool useGlobal)
{
    useGlobalSettings.setValue(useGlobal);
}

bool CopilotProjectSettings::isEnabled() const
{
    if (useGlobalSettings())
        return settings().enableCopilot();
    return enableCopilot();
}

void CopilotProjectSettings::save(ProjectExplorer::Project *project)
{
    Store map;
    toMap(map);
    project->setNamedSettings(Constants::COPILOT_PROJECT_SETTINGS_ID, variantFromStore(map));

    // This triggers a restart of the Copilot language server.
    settings().apply();
}

class CopilotSettingsPage : public Core::IOptionsPage
{
public:
    CopilotSettingsPage()
    {
        setId(Constants::COPILOT_GENERAL_OPTIONS_ID);
        setDisplayName("Copilot");
        setCategory(Core::Constants::SETTINGS_CATEGORY_AI);
        setSettingsProvider([] { return &settings(); });
    }
};

const CopilotSettingsPage settingsPage;

} // namespace Copilot
