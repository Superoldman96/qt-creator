// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "gradientpresetitem.h"

#include <utils/qtcassert.h>
#include <utils/algorithm.h>

#include <QDebug>
#include <QMetaEnum>
#include <QMetaObject>
#include <QRegularExpression>
#include <QVariant>

#include <algorithm>

GradientPresetItem::GradientPresetItem()
    : m_gradientVal(QGradient())
    , m_gradientID(Preset(0))
    , m_presetName(QString())
{}

GradientPresetItem::GradientPresetItem(const QGradient &value, const QString &name)
    : m_gradientVal(value)
    , m_gradientID(Preset(0))
    , m_presetName(name)
{}

GradientPresetItem::GradientPresetItem(const Preset value)
    : m_gradientVal(createGradientFromPreset(value))
    , m_gradientID(value)
    , m_presetName(getNameByPreset(value))
{}

GradientPresetItem::~GradientPresetItem() = default;

QVariant GradientPresetItem::getProperty(GradientPresetItem::Property id) const
{
    QVariant out;

    switch (id) {
    case objectNameRole:
        out.setValue(QString());
        break;
    case stopsPosListRole:
        out.setValue(stopsPosList());
        break;
    case stopsColorListRole:
        out.setValue(stopsColorList());
        break;
    case stopListSizeRole:
        out.setValue(stopListSize());
        break;
    case presetNameRole:
        out.setValue(presetName());
        break;
    case presetIDRole:
        out.setValue(presetID());
        break;
    default:
        qWarning() << "GradientPresetItem Property switch default case";
        break; //replace with assert before switch?
    }

    return out;
}

QGradient GradientPresetItem::gradientVal() const
{
    return m_gradientVal;
}

void GradientPresetItem::setGradient(const QGradient &value)
{
    m_gradientVal = value;
    m_gradientID = Preset(0);
    m_presetName = QString();
}

void GradientPresetItem::setGradient(const Preset value)
{
    m_gradientID = value;
    m_gradientVal = createGradientFromPreset(value);
    m_presetName = getNameByPreset(value);
}

QList<qreal> GradientPresetItem::stopsPosList() const
{
    const QList<QPair<qreal, QColor>> subres = m_gradientVal.stops();
    const QList<qreal> result = Utils::transform<QList<qreal>>(subres,
                                                               [](const QPair<qreal, QColor> &item) {
                                                                   return item.first;
                                                               });
    return result;
}

QList<QString> GradientPresetItem::stopsColorList() const
{
    const QList<QPair<qreal, QColor>> subres = m_gradientVal.stops();
    const QList<QString> result
        = Utils::transform<QList<QString>>(subres, [](const QPair<qreal, QColor> &item) {
              return item.second.name();
          });
    return result;
}

int GradientPresetItem::stopListSize() const
{
    return m_gradientVal.stops().size();
}

void GradientPresetItem::setPresetName(const QString &value)
{
    m_presetName = value;
}

QString GradientPresetItem::presetName() const
{
    return m_presetName;
}

int GradientPresetItem::presetID() const
{
    return static_cast<int>(m_gradientID);
}

QString GradientPresetItem::getNameByPreset(Preset value)
{
    const QMetaObject &metaObj = QGradient::staticMetaObject;
    const QMetaEnum metaEnum = metaObj.enumerator(metaObj.indexOfEnumerator("Preset"));

    if (!metaEnum.isValid())
        return QString("Custom");

    QString enumName = QString::fromUtf8(metaEnum.valueToKey(static_cast<int>(value)));

    const QStringList sl = enumName.split(QRegularExpression("(?=[A-Z])"), Qt::SkipEmptyParts);

    enumName.clear();
    std::for_each(sl.begin(), sl.end(), [&enumName](const QString &s) { enumName += (s + " "); });
    enumName.chop(1); //let's remove the last empty space

    return (enumName.isEmpty() ? QString("Custom") : enumName);
}

QGradient GradientPresetItem::createGradientFromPreset(Preset value)
{
    return QGradient(value);
}

QDebug &operator<<(QDebug &stream, const GradientPresetItem &gradient)
{
    stream << "\"stops:" << gradient.m_gradientVal.stops() << "\"";
    stream << "\"preset:" << gradient.m_gradientID << "\"";
    stream << "\"name:" << gradient.m_presetName << "\"";
    return stream;
}

QDataStream &operator<<(QDataStream &stream, const GradientPresetItem &gradient)
{
    stream << gradient.m_gradientVal.stops();

    stream << static_cast<int>(gradient.m_gradientID);
    stream << gradient.m_presetName;
    return stream;
}

QDataStream &operator>>(QDataStream &stream, GradientPresetItem &gradient)
{
    QGradientStops stops;
    stream >> stops;
    gradient.m_gradientVal.setStops(stops);

    int gradientID;
    stream >> gradientID;
    gradient.m_gradientID = static_cast<GradientPresetItem::Preset>(gradientID);

    stream >> gradient.m_presetName;
    return stream;
}
