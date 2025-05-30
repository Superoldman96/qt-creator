import qbs 1.0

QtcPlugin {
    name: "QmlProjectManager"

    Depends { name: "Qt"; submodules: ["widgets", "network", "quickwidgets"] }
    Depends { name: "QmlJS" }
    Depends { name: "Utils" }

    Depends { name: "Core" }
    Depends { name: "ProjectExplorer" }
    Depends { name: "QmlDesignerBase" }
    Depends { name: "QtSupport" }
    Depends { name: "QmlJSEditor" }
    Depends { name: "TextEditor" }

    Group {
        name: "General"
        files: [
            "projectfilecontenttools.cpp", "projectfilecontenttools.h",
            "qdslandingpagetheme.cpp", "qdslandingpagetheme.h",
            "qdslandingpage.cpp", "qdslandingpage.h",
            "qmlmainfileaspect.cpp", "qmlmainfileaspect.h",
            "qmlmultilanguageaspect.cpp", "qmlmultilanguageaspect.h",
            "qmlproject.cpp", "qmlproject.h",
            "qmlproject.qrc",
            "qmlprojectconstants.h",
            "qmlprojectmanager_global.h", "qmlprojectmanagertr.h",
            "qmlprojectplugin.cpp", "qmlprojectplugin.h",
            "qmlprojectrunconfiguration.cpp", "qmlprojectrunconfiguration.h",
            project.ide_source_tree + "/src/share/3rdparty/studiofonts/studiofonts.qrc"
        ]
    }

    Group {
        name: "Build System"
        prefix: "buildsystem/"
        files: [
            "qmlbuildsystem.cpp", "qmlbuildsystem.h",
            "projectitem/filefilteritems.cpp", "projectitem/filefilteritems.h",
            "projectitem/qmlprojectitem.cpp", "projectitem/qmlprojectitem.h",
            "projectitem/converters.cpp", "projectitem/converters.h",
            "projectnode/qmlprojectnodes.cpp", "projectnode/qmlprojectnodes.h"
        ]
    }

    Group {
        name: "CMake Generator"
        prefix: "qmlprojectexporter/"
        files: [
            "pythongenerator.cpp", "pythongenerator.h",
            "resourcegenerator.cpp", "resourcegenerator.h",
            "cmakegenerator.cpp", "cmakegenerator.h",
            "cmakewriter.cpp", "cmakewriter.h",
            "cmakewriterlib.cpp", "cmakewriterlib.h",
            "cmakewriterv0.cpp", "cmakewriterv0.h",
            "cmakewriterv1.cpp", "cmakewriterv1.h",
            "exporter.cpp", "exporter.h",
            "filegenerator.cpp", "filegenerator.h",
            "filetypes.cpp", "filetypes.h",
            "boilerplate.qrc"
        ]
    }

    Group {
        name: "QML Project File Generator"
        prefix: "qmlprojectgen/"
        files: [
            "qmlprojectgenerator.cpp", "qmlprojectgenerator.h",
            "templates.qrc"
        ]
    }
}
