import qbs 1.0

QtcPlugin {
    name: "ScxmlEditor"

    Depends { name: "Qt.widgets" }
    Depends { name: "Utils" }

    Depends { name: "Core" }
    Depends { name: "TextEditor" }
    Depends { name: "ProjectExplorer" }
    Depends { name: "QtSupport" }

    cpp.includePaths: base.concat([
        ".",
        common.prefix,
        outputpane.prefix,
        plugin_interface.prefix,
    ])

    files: [
        "scxmleditor_global.h",
        "scxmleditoricons.h",
        "scxmleditortr.h",
        "scxmleditorconstants.h",
        "scxmleditor.cpp",
        "scxmleditor.h",
        "scxmleditordocument.cpp",
        "scxmleditordocument.h",
        "scxmleditorplugin.cpp",
        "scxmltexteditor.cpp",
        "scxmltexteditor.h",
    ]

    Group {
        id: common
        name: "Common"
        prefix: "common/"
        files: [
            "colorpicker.cpp", "colorpicker.h",
            "colorsettings.cpp", "colorsettings.h",
            "colorthemedialog.cpp", "colorthemedialog.h",
            "colorthemes.cpp", "colorthemes.h",
            "colorthemeview.cpp", "colorthemeview.h",
            "colortoolbutton.cpp", "colortoolbutton.h",
            "dragshapebutton.cpp", "dragshapebutton.h",
            "graphicsview.cpp", "graphicsview.h",
            "magnifier.cpp", "magnifier.h",
            "mainwidget.cpp", "mainwidget.h",
            "movableframe.cpp", "movableframe.h",
            "navigator.cpp", "navigator.h",
            "navigatorgraphicsview.cpp", "navigatorgraphicsview.h",
            "navigatorslider.cpp", "navigatorslider.h",
            "search.cpp", "search.h",
            "searchmodel.cpp", "searchmodel.h",
            "shapegroupwidget.cpp", "shapegroupwidget.h",
            "shapestoolbox.cpp", "shapestoolbox.h",
            "sizegrip.cpp", "sizegrip.h",
            "stateproperties.cpp", "stateproperties.h",
            "stateview.cpp", "stateview.h",
            "statistics.cpp", "statistics.h",
            "statisticsdialog.cpp", "statisticsdialog.h",
            "structure.cpp", "structure.h",
            "structuremodel.cpp", "structuremodel.h",
            "treeview.h", "treeview.cpp",
        ]

        Group {
            name: "images"
            files: "images/*.png"
            fileTags: "qt.core.resource_data"
            Qt.core.resourceSourceBase: product.sourceDirectory + "/common"
        }
    }

    Group {
        id: outputpane
        name: "Output Pane"
        prefix: "outputpane/"
        files: [
            "errorwidget.cpp", "errorwidget.h",
            "outputpane.h",
            "outputtabwidget.cpp", "outputtabwidget.h",
            "tableview.cpp", "tableview.h",
            "warning.cpp", "warning.h",
            "warningmodel.cpp", "warningmodel.h",
        ]
    }

    Group {
        id: plugin_interface
        name: "Plugin Interface"
        prefix: "plugin_interface/"
        files: [
            "actionhandler.cpp", "actionhandler.h",
            "actionprovider.h",
            "attributeitemdelegate.cpp", "attributeitemdelegate.h",
            "attributeitemmodel.cpp", "attributeitemmodel.h",
            "baseitem.cpp", "baseitem.h",
            "connectableitem.cpp", "connectableitem.h",
            "cornergrabberitem.cpp", "cornergrabberitem.h",
            "eventitem.cpp", "eventitem.h",
            "finalstateitem.cpp", "finalstateitem.h",
            "genericscxmlplugin.cpp", "genericscxmlplugin.h",
            "graphicsitemprovider.h",
            "graphicsscene.cpp", "graphicsscene.h",
            "highlightitem.cpp", "highlightitem.h",
            "historyitem.cpp", "historyitem.h",
            "idwarningitem.cpp", "idwarningitem.h",
            "imageprovider.cpp", "imageprovider.h",
            "initialstateitem.cpp", "initialstateitem.h",
            "initialwarningitem.cpp", "initialwarningitem.h",
            "isceditor.h",
            "layoutitem.cpp", "layoutitem.h",
            "mytypes.h",
            "parallelitem.cpp", "parallelitem.h",
            "quicktransitionitem.cpp", "quicktransitionitem.h",
            "scattributeitemdelegate.cpp", "scattributeitemdelegate.h",
            "scattributeitemmodel.cpp", "scattributeitemmodel.h",
            "sceneutils.cpp", "sceneutils.h",
            "scgraphicsitemprovider.cpp", "scgraphicsitemprovider.h",
            "scshapeprovider.cpp", "scshapeprovider.h",
            "scutilsprovider.cpp", "scutilsprovider.h",
            "scxmldocument.cpp", "scxmldocument.h",
            "scxmlnamespace.cpp", "scxmlnamespace.h",
            "scxmltag.cpp", "scxmltag.h",
            "scxmltagutils.cpp", "scxmltagutils.h",
            "scxmltypes.h",
            "scxmluifactory.cpp", "scxmluifactory.h",
            "serializer.cpp", "serializer.h",
            "shapeprovider.cpp", "shapeprovider.h",
            "snapline.cpp", "snapline.h",
            "stateitem.cpp", "stateitem.h",
            "statewarningitem.cpp", "statewarningitem.h",
            "tagtextitem.cpp", "tagtextitem.h",
            "textitem.cpp", "textitem.h",
            "transitionitem.cpp", "transitionitem.h",
            "transitionwarningitem.cpp", "transitionwarningitem.h",
            "undocommands.cpp", "undocommands.h",
            "utilsprovider.cpp", "utilsprovider.h",
            "warningitem.cpp", "warningitem.h",
            "warningprovider.h",
        ]
    }
}
