// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "materialbrowserwidget.h"

#include "materialbrowsermodel.h"
#include "materialbrowsertexturesmodel.h"
#include "materialbrowserview.h"

#include <asset.h>
#include <assetimageprovider.h>
#include <bundlehelper.h>
#include <createtexture.h>
#include <designmodewidget.h>
#include <documentmanager.h>
#include <hdrimage.h>
#include <qmldesignerconstants.h>
#include <qmldesignerplugin.h>
#include <studioquickwidget.h>
#include <theme.h>
#include <utils3d.h>
#include <variantproperty.h>

#include <coreplugin/icore.h>

#include <utils/algorithm.h>
#include <utils/environment.h>
#include <utils/fileutils.h>
#include <utils/qtcassert.h>
#include <utils/stylehelper.h>

#include <QMimeData>
#include <QMouseEvent>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QShortcut>
#include <QVBoxLayout>

using namespace Core;

namespace QmlDesigner {

static QString propertyEditorResourcesPath()
{
#ifdef SHARE_QML_PATH
    if (Utils::qtcEnvironmentVariableIsSet("LOAD_QML_FROM_SOURCE"))
        return QLatin1String(SHARE_QML_PATH) + "/propertyEditorQmlSources";
#endif
    return Core::ICore::resourcePath("qmldesigner/propertyEditorQmlSources").toUrlishString();
}

class PreviewImageProvider : public QQuickImageProvider
{
    QHash<qint32, QPixmap> m_pixmaps;

public:
    PreviewImageProvider()
        : QQuickImageProvider(Pixmap) {}

    void setPixmap(const ModelNode &node, const QPixmap &pixmap)
    {
        m_pixmaps.insert(node.internalId(), pixmap);
    }

    QPixmap getPixmap(const ModelNode &node)
    {
        QTC_ASSERT(node, return {});

        return m_pixmaps.value(node.internalId());
    }

    void clearPixmapCache()
    {
        m_pixmaps.clear();
    }

    QPixmap requestPixmap(const QString &id,
                          QSize *size,
                          [[maybe_unused]] const QSize &requestedSize) override
    {
        static QPixmap defaultPreview = QPixmap::fromImage(
            QImage(":/propertyeditor/images/defaultmaterialpreview.png"));

        QPixmap pixmap{150, 150};

        qint32 internalId = id.toInt();
        if (m_pixmaps.contains(internalId))
            pixmap = m_pixmaps.value(internalId);
        else
            pixmap = defaultPreview;

        if (size)
            *size = pixmap.size();

        return pixmap;
    }
};

bool MaterialBrowserWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::FocusOut) {
        if (obj == m_quickWidget->quickWidget())
            QMetaObject::invokeMethod(m_quickWidget->rootObject(), "closeContextMenu");
    } else if (event->type() == QMouseEvent::MouseMove) {
        DesignDocument *document = QmlDesignerPlugin::instance()->currentDesignDocument();
        QTC_ASSERT(document, return false);
        Model *model = document->currentModel();
        QTC_ASSERT(model, return false);

        if (m_materialToDrag.isValid() || m_textureToDrag.isValid()) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            if ((me->globalPosition().toPoint() - m_dragStartPoint).manhattanLength() > 20) {
                bool isMaterial = m_materialToDrag.isValid();
                auto mimeData = std::make_unique<QMimeData>();
                QByteArray internalId;

                if (isMaterial) {
                    qint32 internalIdVal = m_materialToDrag.internalId();
                    internalId.setNum(internalIdVal);
                    m_materialToDrag = {};
                    m_textureToDrag = {};
                    mimeData->setData(Constants::MIME_TYPE_MATERIAL, internalId);
                    model->startDrag(std::move(mimeData),
                                     m_previewImageProvider->requestPixmap(
                                         QString::number(internalIdVal),
                                         nullptr,
                                         {128, 128}),
                                     this);
                } else {
                    QString sourcePropVal = m_textureToDrag.variantProperty("source").value().toString();
                    internalId.setNum(m_textureToDrag.internalId());
                    m_textureToDrag = {};
                    mimeData->setData(Constants::MIME_TYPE_TEXTURE, internalId);
                    QString iconPath = QLatin1String("%1/%2")
                                    .arg(DocumentManager::currentResourcePath().path(),
                                         sourcePropVal);

                    QPixmap pixmap;
                    const QString suffix = iconPath.split('.').last().toLower();
                    if (suffix == "hdr")
                        pixmap = HdrImage{iconPath}.toPixmap();
                    else if (suffix == "ktx")
                        pixmap = Utils::StyleHelper::dpiSpecificImageFile(
                            ":/propertyeditor/images/texture_ktx.png");
                    else
                        pixmap = Utils::StyleHelper::dpiSpecificImageFile(iconPath);
                    if (pixmap.isNull())
                        pixmap = Utils::StyleHelper::dpiSpecificImageFile(
                            ":/propertyeditor/images/texture_default.png");
                    model->startDrag(std::move(mimeData), pixmap.scaled({128, 128}), this);
                }
            }
        }
    } else if (event->type() == QMouseEvent::MouseButtonRelease) {
        m_materialToDrag = {};
        m_textureToDrag = {};

        setIsDragging(false);
    }

    return QObject::eventFilter(obj, event);
}

MaterialBrowserWidget::MaterialBrowserWidget(AsynchronousImageCache &imageCache,
                                             MaterialBrowserView *view)
    : m_materialBrowserView(view)
    , m_materialBrowserModel(new MaterialBrowserModel(view, this))
    , m_materialBrowserTexturesModel(new MaterialBrowserTexturesModel(view, this))
    , m_quickWidget(Utils::makeUniqueObjectPtr<StudioQuickWidget>(this))
    , m_previewImageProvider(new PreviewImageProvider())
    , m_bundleHelper(std::make_unique<BundleHelper>(view, this))
{
    QImage defaultImage;
    defaultImage.load(
        Utils::StyleHelper::dpiSpecificImageFile(":/propertyeditor/images/texture_default.png"));
    m_textureImageProvider = new AssetImageProvider(imageCache, defaultImage);

    setWindowTitle(tr("Material Browser", "Title of material browser widget"));
    setMinimumWidth(120);

    Core::Context context(Constants::qmlMaterialBrowserContextId);
    m_context = new Core::IContext(this);
    m_context->setContext(context);
    m_context->setWidget(this);

    m_quickWidget->quickWidget()->setObjectName(Constants::OBJECT_NAME_MATERIAL_BROWSER);
    m_quickWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_quickWidget->engine()->addImportPath(propertyEditorResourcesPath() + "/imports");
    m_quickWidget->setClearColor(Theme::getColor(Theme::Color::DSpanelBackground));

    m_quickWidget->engine()->addImageProvider("materialBrowser", m_previewImageProvider);
    m_quickWidget->engine()->addImageProvider("materialBrowserTex", m_textureImageProvider);

    Theme::setupTheme(m_quickWidget->engine());
    m_quickWidget->quickWidget()->installEventFilter(this);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins({});
    layout->setSpacing(0);
    layout->addWidget(m_quickWidget.get());

    updateSearch();

    setStyleSheet(Theme::replaceCssColors(
        Utils::FileUtils::fetchQrc(":/qmldesigner/stylesheet.css")));

    m_qmlSourceUpdateShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F8), this);
    connect(m_qmlSourceUpdateShortcut, &QShortcut::activated, this, &MaterialBrowserWidget::reloadQmlSource);

    connect(m_materialBrowserModel, &MaterialBrowserModel::isEmptyChanged, this, [this] {
        if (m_materialBrowserModel->isEmpty())
            focusMaterialSection(false);
    });

    connect(m_materialBrowserTexturesModel, &MaterialBrowserTexturesModel::isEmptyChanged, this, [this] {
        if (m_materialBrowserTexturesModel->isEmpty())
            focusMaterialSection(true);
    });

    QmlDesignerPlugin::trackWidgetFocusTime(this, Constants::EVENT_MATERIALBROWSER_TIME);

    auto map = m_quickWidget->registerPropertyMap("MaterialBrowserBackend");

    map->setProperties({
        {"rootView", QVariant::fromValue(this)},
        {"materialBrowserModel", QVariant::fromValue(m_materialBrowserModel.data())},
        {"materialBrowserTexturesModel", QVariant::fromValue(m_materialBrowserTexturesModel.data())},
    });

    reloadQmlSource();

    setFocusProxy(m_quickWidget->quickWidget());

    IContext::attach(this,
                     Context(Constants::qmlMaterialBrowserContextId, Constants::qtQuickToolsMenuContextId),
                     [this](const IContext::HelpCallback &callback) { contextHelp(callback); });
}

void MaterialBrowserWidget::updateMaterialPreview(const ModelNode &node, const QPixmap &pixmap)
{
    m_previewImageProvider->setPixmap(node, pixmap);
    int idx = m_materialBrowserModel->materialIndex(node);
    if (idx != -1)
        QMetaObject::invokeMethod(m_quickWidget->rootObject(), "refreshPreview", Q_ARG(QVariant, idx));
}

void MaterialBrowserWidget::deleteSelectedItems()
{
    m_materialBrowserView->executeInTransaction(__FUNCTION__, [this] {
        m_materialBrowserModel->deleteSelectedMaterials();
        m_materialBrowserTexturesModel->deleteSelectedTextures();
    });
}

QList<QToolButton *> MaterialBrowserWidget::createToolBarWidgets()
{
    return {};
}

void MaterialBrowserWidget::contextHelp(const Core::IContext::HelpCallback &callback) const
{
    if (m_materialBrowserView)
        QmlDesignerPlugin::contextHelp(callback, m_materialBrowserView->contextHelpId());
    else
        callback({});
}

void MaterialBrowserWidget::handleSearchFilterChanged(const QString &filterText)
{
    if (filterText != m_filterText) {
        m_filterText = filterText;
        updateSearch();
    }
}

void MaterialBrowserWidget::startDragMaterial(int index, const QPointF &mousePos)
{
    m_materialToDrag = m_materialBrowserModel->materialAt(index);
    m_dragStartPoint = mousePos.toPoint();

    setIsDragging(true);
}

void MaterialBrowserWidget::startDragTexture(int index, const QPointF &mousePos)
{
    m_textureToDrag = m_materialBrowserTexturesModel->textureAt(index);
    m_dragStartPoint = mousePos.toPoint();

    setIsDragging(true);
}

void MaterialBrowserWidget::acceptBundleMaterialDrop()
{
    m_materialBrowserView->emitCustomNotification("drop_bundle_material", {}, {}); // To ContentLibraryView
    if (m_materialBrowserView->model())
        m_materialBrowserView->model()->endDrag();
}

bool MaterialBrowserWidget::hasAcceptableAssets(const QList<QUrl> &urls)
{
    return Utils::anyOf(urls, [](const QUrl &url) {
        return Asset(url.toLocalFile()).isValidTextureSource();
    });
}

void MaterialBrowserWidget::acceptBundleTextureDrop()
{
    m_materialBrowserView->emitCustomNotification("drop_bundle_texture", {}, {}); // To ContentLibraryView
    if (m_materialBrowserView->model())
        m_materialBrowserView->model()->endDrag();
}

void MaterialBrowserWidget::acceptBundleTextureDropOnMaterial(int matIndex, const QUrl &bundleTexPath)
{
    ModelNode mat = m_materialBrowserModel->materialAt(matIndex);
    QTC_ASSERT(mat.isValid(), return);

    m_materialBrowserView->executeInTransaction(__FUNCTION__, [&] {
        ModelNode tex = CreateTexture(m_materialBrowserView).execute(bundleTexPath.toLocalFile());
        QTC_ASSERT(tex.isValid(), return);

        mat.model()->setSelectedModelNodes({mat});
        m_materialBrowserView->applyTextureToMaterial({mat}, tex);
    });

    if (m_materialBrowserView->model())
        m_materialBrowserView->model()->endDrag();
}

void MaterialBrowserWidget::acceptAssetsDrop(const QList<QUrl> &urls)
{
    QStringList assetPaths = Utils::transform(urls, &QUrl::toLocalFile);
    m_materialBrowserView->createTextures(assetPaths);
    if (m_materialBrowserView->model())
        m_materialBrowserView->model()->endDrag();
}

void MaterialBrowserWidget::acceptAssetsDropOnMaterial(int matIndex, const QList<QUrl> &urls)
{
    ModelNode mat = m_materialBrowserModel->materialAt(matIndex);
    QTC_ASSERT(mat.isValid(), return);

    QString imageSrc = Utils::findOrDefault(urls, [](const QUrl &url) {
                           return Asset(url.toLocalFile()).isValidTextureSource();
                       }).toLocalFile();

    m_materialBrowserView->executeInTransaction(__FUNCTION__, [&] {
        ModelNode tex = CreateTexture(m_materialBrowserView).execute(imageSrc);
        QTC_ASSERT(tex.isValid(), return);

        mat.model()->setSelectedModelNodes({mat});
        m_materialBrowserView->applyTextureToMaterial({mat}, tex);
    });

    if (m_materialBrowserView->model())
        m_materialBrowserView->model()->endDrag();
}

void MaterialBrowserWidget::acceptTextureDropOnMaterial(int matIndex, const QString &texId)
{
    ModelNode mat = m_materialBrowserModel->materialAt(matIndex);
    ModelNode tex = m_materialBrowserView->modelNodeForInternalId(texId.toInt());

    if (mat.isValid() && tex.isValid()) {
        mat.model()->setSelectedModelNodes({mat});
        m_materialBrowserView->applyTextureToMaterial({mat}, tex);
    }

    if (m_materialBrowserView->model())
        m_materialBrowserView->model()->endDrag();
}

void MaterialBrowserWidget::focusMaterialSection(bool focusMatSec)
{
    if (focusMatSec != m_materialSectionFocused) {
        m_materialSectionFocused = focusMatSec;
        emit materialSectionFocusedChanged();
    }
}

void MaterialBrowserWidget::addMaterialToContentLibrary(int idx)
{
    ModelNode mat = m_materialBrowserModel->materialAt(idx);
    QTC_ASSERT(mat, return);

    QmlDesignerPlugin::instance()->mainWidget()->showDockWidget("ContentLibrary");
    m_materialBrowserView->emitCustomNotification("add_material_to_content_lib", {mat},
                                                  {m_previewImageProvider->getPixmap(mat)}); // to ContentLibrary
}

void MaterialBrowserWidget::importMaterial()
{
    m_bundleHelper->importBundleToProject();
}

void MaterialBrowserWidget::exportMaterial(int idx)
{
    ModelNode mat = m_materialBrowserModel->materialAt(idx);
    QTC_ASSERT(mat, return);

    m_bundleHelper->exportBundle({mat}, m_previewImageProvider->getPixmap(mat));
    m_materialBrowserModel->updateMaterialComponent(idx);
}

void MaterialBrowserWidget::addQtQuick3D()
{
    Utils3D::addQuick3DImportAndView3D(m_materialBrowserView.data());
}

void MaterialBrowserWidget::openPropertyEditor()
{
    QmlDesignerPlugin::instance()->mainWidget()->showDockWidget("Properties", true);
}

QString MaterialBrowserWidget::qmlSourcesPath()
{
#ifdef SHARE_QML_PATH
    if (Utils::qtcEnvironmentVariableIsSet("LOAD_QML_FROM_SOURCE"))
        return QLatin1String(SHARE_QML_PATH) + "/materialBrowserQmlSource";
#endif
    return Core::ICore::resourcePath("qmldesigner/materialBrowserQmlSource").toUrlishString();
}

void MaterialBrowserWidget::clearSearchFilter()
{
    QMetaObject::invokeMethod(m_quickWidget->rootObject(), "clearSearchFilter");
}

void MaterialBrowserWidget::reloadQmlSource()
{
    const QString materialBrowserQmlPath = qmlSourcesPath() + "/MaterialBrowser.qml";

    QTC_ASSERT(QFileInfo::exists(materialBrowserQmlPath), return);

    m_quickWidget->setSource(QUrl::fromLocalFile(materialBrowserQmlPath));
}

void MaterialBrowserWidget::updateSearch()
{
    m_materialBrowserModel->setSearchText(m_filterText);
    m_materialBrowserTexturesModel->setSearchText(m_filterText);
    m_quickWidget->update();
}

void MaterialBrowserWidget::setIsDragging(bool val)
{
    if (m_isDragging != val) {
        m_isDragging = val;
        emit isDraggingChanged();
    }
}

StudioQuickWidget *MaterialBrowserWidget::quickWidget() const
{
    return m_quickWidget.get();
}

void MaterialBrowserWidget::clearPreviewCache()
{
    m_previewImageProvider->clearPixmapCache();
}

QSize MaterialBrowserWidget::sizeHint() const
{
    return {420, 420};
}

QPointer<MaterialBrowserModel> MaterialBrowserWidget::materialBrowserModel() const
{
    return m_materialBrowserModel;
}

QPointer<MaterialBrowserTexturesModel> MaterialBrowserWidget::materialBrowserTexturesModel() const
{
    return m_materialBrowserTexturesModel;
}

} // namespace QmlDesigner
