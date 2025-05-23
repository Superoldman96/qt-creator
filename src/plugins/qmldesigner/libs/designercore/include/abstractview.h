// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "model.h"
#include "modelnode.h"
#include "qmldesignercorelib_global.h"
#include "widgetregistration.h"

#include <commondefines.h>

#include <utils/span.h>
#include <utils/uniqueobjectptr.h>

#include <QAction>
#include <QObject>
#include <QPointer>

QT_BEGIN_NAMESPACE
class QImage;
class QPixmap;
class QVector3D;
QT_END_NAMESPACE

namespace QmlDesigner {

class AbstractProperty;
class DocumentMessage;
class ExternalDependenciesInterface;
class NodeInstanceView;
class QmlModelState;
class QmlTimeline;
class RewriterTransaction;
class RewriterView;

namespace Internal {
class InternalNode;
using InternalNodePointer = std::shared_ptr<InternalNode>;
}

class QMLDESIGNERCORE_EXPORT AbstractViewAction : public QAction
{
    Q_OBJECT

public:
    AbstractViewAction(class AbstractView &view);

signals:
    void viewCheckedChanged(bool checkable, AbstractView &view);

private:
    AbstractView &m_view;
};

class QMLDESIGNERCORE_EXPORT AbstractView : public QObject
{
    Q_OBJECT

public:
    Q_FLAGS(PropertyChangeFlag PropertyChangeFlags)

    enum PropertyChangeFlag {
        NoAdditionalChanges = 0x0,
        PropertiesAdded = 0x1,
        EmptyPropertiesRemoved = 0x2
    };
    Q_DECLARE_FLAGS(PropertyChangeFlags, PropertyChangeFlag)

    AbstractView(ExternalDependenciesInterface &externalDependencies)
        : m_externalDependencies{externalDependencies}
        , m_action{Utils::makeUniqueObjectPtr<AbstractViewAction>(*this)}
    {}

    void setWidgetRegistration(WidgetRegistrationInterface *interface);
    virtual void registerWidgetInfo();
    ~AbstractView() override;

    Model *model() const { return m_model.data(); }
    bool isAttached() const;

    enum class Kind { Other, Rewriter, NodeInstance };

    Kind kind() const { return m_kind; }

    RewriterTransaction beginRewriterTransaction(const QByteArray &identifier);

    ModelNode createModelNode(const TypeName &typeName);

#
    ModelNode createModelNode(const TypeName &typeName,
                              int majorVersion,
                              int minorVersion,
                              const PropertyListType &propertyList = PropertyListType(),
                              const AuxiliaryDatas &auxPropertyList = {},
                              const QString &nodeSource = {},
                              ModelNode::NodeSourceType nodeSourceType = ModelNode::NodeWithoutSource,
                              const QString &behaviorPropertyName = {});

    ModelNode createModelNode(const TypeName &typeName,
                              const PropertyListType &propertyList,
                              const AuxiliaryDatas &auxPropertyList = {},
                              const QString &nodeSource = {},
                              ModelNode::NodeSourceType nodeSourceType = ModelNode::NodeWithoutSource,
                              const QString &behaviorPropertyName = {});

    ModelNode rootModelNode() const;
    ModelNode rootModelNode();

    void setSelectedModelNodes(const QList<ModelNode> &selectedNodeList);
    void setSelectedModelNode(const ModelNode &modelNode);

    void selectModelNode(const ModelNode &node);
    void deselectModelNode(const ModelNode &node);
    void clearSelectedModelNodes();
    bool hasSelectedModelNodes() const;
    bool hasSingleSelectedModelNode() const;
    bool isSelectedModelNode(const ModelNode &modelNode) const;

    QList<ModelNode> selectedModelNodes() const;
    ModelNode firstSelectedModelNode() const;
    ModelNode singleSelectedModelNode() const;

    ModelNode modelNodeForId(const QString &id);
    bool hasId(const QString &id) const;

    ModelNode modelNodeForInternalId(qint32 internalId) const;
    bool hasModelNodeForInternalId(qint32 internalId) const;

    QList<ModelNode> allModelNodes() const;
    QList<ModelNode> allModelNodesUnordered() const;
    QList<ModelNode> allModelNodesOfType(const NodeMetaInfo &typeName) const;

    virtual void modelAttached(Model *model);
    virtual void modelAboutToBeDetached(Model *model);

    virtual void refreshMetaInfos(const TypeIds &deletedTypeIds);
    using ExportedTypeNames = Storage::Info::ExportedTypeNames;
    virtual void exportedTypeNamesChanged(const ExportedTypeNames &added,
                                          const ExportedTypeNames &removed);

    virtual void nodeCreated(const ModelNode &createdNode);
    virtual void nodeAboutToBeRemoved(const ModelNode &removedNode);
    virtual void nodeRemoved(const ModelNode &removedNode, const NodeAbstractProperty &parentProperty,
                             PropertyChangeFlags propertyChange);
    virtual void nodeAboutToBeReparented(const ModelNode &node, const NodeAbstractProperty &newPropertyParent,
                                         const NodeAbstractProperty &oldPropertyParent, PropertyChangeFlags propertyChange);
    virtual void nodeReparented(const ModelNode &node, const NodeAbstractProperty &newPropertyParent,
                                const NodeAbstractProperty &oldPropertyParent, PropertyChangeFlags propertyChange);
    virtual void nodeIdChanged(const ModelNode &node, const QString &newId, const QString &oldId);
    virtual void propertiesAboutToBeRemoved(const QList<AbstractProperty> &propertyList);
    virtual void propertiesRemoved(const QList<AbstractProperty> &propertyList);
    virtual void variantPropertiesChanged(const QList<VariantProperty> &propertyList,
                                          PropertyChangeFlags propertyChange);
    virtual void bindingPropertiesAboutToBeChanged(const QList<BindingProperty> &propertyList);
    virtual void bindingPropertiesChanged(const QList<BindingProperty> &propertyList,
                                          PropertyChangeFlags propertyChange);
    virtual void signalHandlerPropertiesChanged(const QVector<SignalHandlerProperty> &propertyList,
                                                PropertyChangeFlags propertyChange);
    virtual void signalDeclarationPropertiesChanged(const QVector<SignalDeclarationProperty> &propertyList,
                                                    PropertyChangeFlags propertyChange);
    virtual void rootNodeTypeChanged(const QString &type, int majorVersion, int minorVersion);
    virtual void nodeTypeChanged(const ModelNode &node, const TypeName &type, int majorVersion, int minorVersion);

    virtual void instancePropertyChanged(const QList<QPair<ModelNode, PropertyName> > &propertyList);
    virtual void instanceErrorChanged(const QVector<ModelNode> &errorNodeList);
    virtual void instancesCompleted(const QVector<ModelNode> &completedNodeList);
    virtual void instanceInformationsChanged(const QMultiHash<ModelNode, InformationName> &informationChangeHash);
    virtual void instancesRenderImageChanged(const QVector<ModelNode> &nodeList);
    virtual void instancesPreviewImageChanged(const QVector<ModelNode> &nodeList);
    virtual void instancesChildrenChanged(const QVector<ModelNode> &nodeList);
    virtual void instancesToken(const QString &tokenName, int tokenNumber, const QVector<ModelNode> &nodeVector);

    virtual void nodeSourceChanged(const ModelNode &modelNode, const QString &newNodeSource);

    virtual void rewriterBeginTransaction();
    virtual void rewriterEndTransaction();

    virtual void currentStateChanged(const ModelNode &node); // base state is a invalid model node

    virtual void selectedNodesChanged(const QList<ModelNode> &selectedNodeList,
                                      const QList<ModelNode> &lastSelectedNodeList);

    virtual void fileUrlChanged(const QUrl &oldUrl, const QUrl &newUrl);

    virtual void nodeOrderChanged(const NodeListProperty &listProperty);
    virtual void nodeOrderChanged(const NodeListProperty &listProperty,
                                  const ModelNode &movedNode,
                                  int oldIndex);

    virtual void importsChanged(const Imports &addedImports, const Imports &removedImports);
    virtual void possibleImportsChanged(const Imports &possibleImports);
    virtual void usedImportsChanged(const Imports &usedImports);

    virtual void auxiliaryDataChanged(const ModelNode &node,
                                      AuxiliaryDataKeyView type,
                                      const QVariant &data);

    virtual void customNotification(const AbstractView *view,
                                    const QString &identifier,
                                    const QList<ModelNode> &nodeList,
                                    const QList<QVariant> &data);

    virtual void customNotification(const CustomNotificationPackage &);

    virtual void scriptFunctionsChanged(const ModelNode &node, const QStringList &scriptFunctionList);

    virtual void documentMessagesChanged(const QList<DocumentMessage> &errors, const QList<DocumentMessage> &warnings);

    virtual void currentTimelineChanged(const ModelNode &node);

    virtual void renderImage3DChanged(const QImage &image);
    virtual void updateActiveScene3D(const QVariantMap &sceneState);
    virtual void updateImport3DSupport(const QVariantMap &supportMap);
    virtual void nodeAtPosReady(const ModelNode &modelNode, const QVector3D &pos3d);
    virtual void modelNodePreviewPixmapChanged(const ModelNode &node,
                                               const QPixmap &pixmap,
                                               const QByteArray &requestId);

    virtual void view3DAction(View3DActionType type, const QVariant &value);

    virtual void dragStarted(QMimeData *mimeData);
    virtual void dragEnded();

    void changeRootNodeType(const TypeName &type, int majorVersion, int minorVersion);

    void emitCustomNotification(const QString &identifier,
                                Utils::span<const ModelNode> nodes = {},
                                const QList<QVariant> &data = {})
    {
        if (isAttached())
            model()->emitCustomNotification(this, identifier, nodes, data);
    }

    const AbstractView *nodeInstanceView() const;
    RewriterView *rewriterView() const;

    void setCurrentStateNode(const ModelNode &node);
    ModelNode currentStateNode() const;
    ModelNode currentTimelineNode() const;

    int majorQtQuickVersion() const;
    int minorQtQuickVersion() const;

    void resetView();
    void resetPuppet();

    virtual bool hasWidget() const;
    virtual WidgetInfo widgetInfo();
    virtual void disableWidget();
    virtual void enableWidget();

    QString contextHelpId() const;

    using OperationBlock = std::function<void()>;
    bool executeInTransaction(const QByteArray &identifier, const OperationBlock &lambda);

    bool isEnabled() const
    {
#ifdef DETACH_DISABLED_VIEWS
        return m_visible && (hasWidget() ? m_action->isChecked() : true);
#else
        return m_visible;
#endif
    }

    void setVisibility(bool visible) { m_visible = visible; }

    AbstractViewAction *action() const { return m_action.get(); }

    ExternalDependenciesInterface &externalDependencies() const { return m_externalDependencies; }

    bool isBlockingNotifications() const { return m_isBlockingNotifications; }

    class NotificationBlocker
    {
    public:
        NotificationBlocker(AbstractView *view)
            : m_view{view}
        {
            m_view->m_isBlockingNotifications = true;
        }

        ~NotificationBlocker() { m_view->m_isBlockingNotifications = false; }

    private:
        AbstractView *m_view;
    };

protected:
    void setModel(Model *model);
    void removeModel();

    static WidgetInfo createWidgetInfo(
        QWidget *widget = nullptr,
        const QString &uniqueId = QString(),
        WidgetInfo::PlacementHint placementHint = WidgetInfo::NoPane,
        const QString &tabName = QString(),
        const QString &feedbackDisplayName = QString(),
        DesignerWidgetFlags widgetFlags = DesignerWidgetFlags::DisableOnError);

    void setKind(Kind kind) { m_kind = kind; }
private:
    QList<ModelNode> toModelNodeList(Utils::span<const Internal::InternalNodePointer> nodeList) const;

    QPointer<Model> m_model;
    ExternalDependenciesInterface &m_externalDependencies;
    Utils::UniqueObjectPtr<AbstractViewAction> m_action;
    bool m_visible = true;
    bool m_isBlockingNotifications = false;
    Kind m_kind = Kind::Other;
    WidgetRegistrationInterface *m_widgetRegistration = nullptr;
};

QMLDESIGNERCORE_EXPORT QList<ModelNode> toModelNodeList(
    Utils::span<const Internal::InternalNodePointer> nodeList,
    Model *model,
    AbstractView *view = nullptr);
} // namespace QmlDesigner
