// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "modelnode.h"
#include "qmldesigner_global.h"

#include <QObject>
#include <QQmlPropertyMap>
#include <QtQml>

namespace QmlDesigner {

class QmlObjectNode;
class PropertyEditorValue;

class PropertyEditorSubSelectionWrapper : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QQmlPropertyMap *properties READ properties NOTIFY propertiesChanged)

signals:
    void propertiesChanged();

public:
    QQmlPropertyMap *properties();
    PropertyEditorSubSelectionWrapper(const ModelNode &modelNode);
    ModelNode modelNode() const;

    Q_INVOKABLE void deleteModelNode();

    void setValueFromModel(PropertyNameView name, const QVariant &value);
    void resetValue(PropertyNameView name);

    bool isRelevantModelNode(const ModelNode &modelNode) const;

private:
    void changeValue(const QString &name);
    void changeExpression(const QString &propertyName);
    void createPropertyEditorValue(const QmlObjectNode &qmlObjectNode,
                                   PropertyNameView name,
                                   const QVariant &value);
    void exportPropertyAsAlias(const QString &name);
    void removeAliasExport(const QString &name);
    bool locked() const;

    ModelNode m_modelNode;
    QQmlPropertyMap m_valuesPropertyMap;
    bool m_locked = false;
    void removePropertyFromModel(PropertyNameView propertyName);
    void commitVariantValueToModel(PropertyNameView propertyName, const QVariant &value);
    AbstractView *view() const;
};

class PropertyEditorNodeWrapper : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool exists READ exists NOTIFY existsChanged)
    Q_PROPERTY(QQmlPropertyMap *properties READ properties NOTIFY propertiesChanged)
    Q_PROPERTY(QString type READ type NOTIFY typeChanged)

public:
    PropertyEditorNodeWrapper(QObject *parent = nullptr);
    PropertyEditorNodeWrapper(PropertyEditorValue *parent);

    bool exists() const;
    QString type() const;
    QQmlPropertyMap *properties();
    ModelNode parentModelNode() const;
    PropertyNameView propertyName() const;

public slots:
    void add(const QString &type = QString());
    void remove();
    void changeValue(const QString &propertyName);
    void update();

signals:
    void existsChanged();
    void propertiesChanged();
    void typeChanged();

private:
    void setup();

    ModelNode m_modelNode;
    QQmlPropertyMap m_valuesPropertyMap;
    PropertyEditorValue *m_editorValue = nullptr;
};

class QMLDESIGNER_EXPORT PropertyEditorValue : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariant value READ value WRITE setValueWithEmit NOTIFY valueChangedQml)
    Q_PROPERTY(QVariant enumeration READ enumeration NOTIFY valueChangedQml)
    Q_PROPERTY(QString expression READ expression WRITE setExpressionWithEmit NOTIFY expressionChanged FINAL)
    Q_PROPERTY(QString valueToString READ valueToString NOTIFY valueChangedQml FINAL)
    Q_PROPERTY(bool isInModel READ isInModel NOTIFY isExplicitChanged FINAL)
    Q_PROPERTY(bool isInSubState READ isInSubState NOTIFY isExplicitChanged FINAL)
    Q_PROPERTY(bool isBound READ isBound NOTIFY isBoundChanged FINAL)
    Q_PROPERTY(bool isValid READ isValid NOTIFY isValidChanged FINAL)
    Q_PROPERTY(bool isTranslated READ isTranslated NOTIFY expressionChanged FINAL)
    Q_PROPERTY(bool hasActiveDrag READ hasActiveDrag WRITE setHasActiveDrag NOTIFY hasActiveDragChanged FINAL)

    Q_PROPERTY(bool isIdList READ isIdList NOTIFY expressionChanged FINAL)
    Q_PROPERTY(QStringList expressionAsList READ getExpressionAsList NOTIFY expressionChanged FINAL)

    Q_PROPERTY(QString name READ nameAsQString CONSTANT FINAL)
    Q_PROPERTY(PropertyEditorNodeWrapper *complexNode READ complexNode NOTIFY complexNodeChanged FINAL)

    Q_PROPERTY(bool isAvailable READ isAvailable NOTIFY isBoundChanged)

public:
    PropertyEditorValue(QObject *parent = nullptr);

    QVariant value() const;
    void setValueWithEmit(const QVariant &value);
    void setValue(const QVariant &value);

    QString enumeration() const;

    QString expression() const;
    void setExpressionWithEmit(const QString &expression);
    void setExpression(const QString &expression);

    QString valueToString() const;

    bool isInSubState() const;

    bool isInModel() const;

    bool isBound() const;
    bool isValid() const;

    void setIsValid(bool valid);

    bool isTranslated() const;

    bool hasActiveDrag() const;
    void setHasActiveDrag(bool val);

    bool isAvailable() const;

    PropertyNameView name() const;
    QString nameAsQString() const;
    void setName(PropertyNameView name);

    ModelNode modelNode() const;
    void setModelNode(const ModelNode &modelNode);

    PropertyEditorNodeWrapper *complexNode();

    static void registerDeclarativeTypes();

    Q_INVOKABLE void exportPropertyAsAlias();
    Q_INVOKABLE bool hasPropertyAlias() const;
    Q_INVOKABLE bool isAttachedProperty() const;
    Q_INVOKABLE void removeAliasExport();

    Q_INVOKABLE QString getTranslationContext() const;

    bool isIdList() const;

    Q_INVOKABLE QStringList getExpressionAsList() const;
    Q_INVOKABLE QVector<double> getExpressionAsVector() const;
    Q_INVOKABLE bool idListAdd(const QString &value);
    Q_INVOKABLE bool idListRemove(int idx);
    Q_INVOKABLE bool idListReplace(int idx, const QString &value);
    Q_INVOKABLE void commitDrop(const QString &dropData);
    Q_INVOKABLE void editMaterial(int idx);

    Q_INVOKABLE void setForceBound(bool b);

    Q_INVOKABLE void insertKeyframe();

public slots:
    void resetValue();
    void setEnumeration(const QString &scope, const QString &name);

signals:
    void valueChanged(const QString &name, const QVariant &);
    void valueChangedQml();

    void expressionChanged(const QString &name); // HACK - We use the same notifer for the backend and frontend.
                                                 // If name is empty the signal is used for QML.

    void expressionChangedQml();

    void exportPropertyAsAliasRequested(const QString &name);
    void removeAliasExportRequested(const QString &name);

    void modelStateChanged();
    void modelNodeChanged();
    void complexNodeChanged();
    void isBoundChanged();
    void isValidChanged();
    void isExplicitChanged();
    void hasActiveDragChanged();
    void dropCommitted(QString dropData);

private:
    QStringList generateStringList(const QString &string) const;
    QString generateString(const QStringList &stringList) const;

    ModelNode m_modelNode;
    QVariant m_value;
    QString m_expression;
    Utils::SmallString m_name;
    bool m_isInSubState = false;
    bool m_isInModel = false;
    bool m_isBound = false;
    bool m_hasActiveDrag = false;
    bool m_isValid = false; // if the property value belongs to a non-existing complexProperty it is invalid
    PropertyEditorNodeWrapper *m_complexNode;
    bool m_forceBound = false;
};

} // namespace QmlDesigner

QML_DECLARE_TYPE(QmlDesigner::PropertyEditorValue)
QML_DECLARE_TYPE(QmlDesigner::PropertyEditorNodeWrapper)
