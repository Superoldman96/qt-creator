// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <tracing/traceeventtype.h>

#include <QDataStream>

namespace PerfProfiler::Internal {

class PerfEventType : public Timeline::TraceEventType
{
public:
    static const qint32 staticClassId = 0x70726674; // 'prft'

    enum Feature {
        ThreadStart,
        ThreadEnd,
        Command,
        LocationDefinition,
        SymbolDefinition,
        StringDefinition,
        LostDefinition,
        FeaturesDefinition,
        Error,
        Progress,
        TracePointFormat,
        AttributesDefinition,
        ContextSwitchDefinition,
        Sample,
        TracePointSample,
        InvalidFeature
    };

    static quint64 attributeFeatures()
    {
        return (1ull << Sample) | (1ull << TracePointSample) | (1ull << AttributesDefinition);
    }

    static quint64 metaFeatures()
    {
        return (1ull << ThreadStart) | (1ull << ThreadEnd) | (1ull << LostDefinition)
               | (1ull << ContextSwitchDefinition);
    }

    static quint64 locationFeatures()
    {
        return (1ull << LocationDefinition);
    }

    static quint64 allFeatures()
    {
        return attributeFeatures() | metaFeatures() | locationFeatures();
    }

    enum Type {
        TypeHardware       = 0,
        TypeSoftware       = 1,
        TypeTracepoint     = 2,
        TypeHardwareCache  = 3,
        TypeRaw            = 4,
        TypeBreakpoint     = 5,
        TypeMax
    };

    struct Attribute {
        quint64 config = std::numeric_limits<quint64>::max();
        quint64 frequencyOrPeriod = std::numeric_limits<quint64>::max();
        quint32 type = std::numeric_limits<quint32>::max();
        qint32 name = -1;
        bool usesFrequency = false;
    };

    struct Location {
        quint64 address = 0;
        qint32 file = -1;
        quint32 pid = 0;
        qint32 line = -1;
        qint32 column = -1;
        qint32 parentLocationId = -1;
    };

    struct Meta {
    };

    PerfEventType(Feature feature = InvalidFeature, const QString &displayName = QString()) :
        TraceEventType(staticClassId, feature, displayName)
    {
        if (isAttribute())
            m_attribute = Attribute();
        else if (isLocation())
            m_location = Location();
        else if (isMeta())
            m_meta = Meta();
        // else the memory stays uninitialized
    }

    bool isLocation() const
    {
        return feature() == LocationDefinition;
    }

    bool isAttribute() const
    {
        switch (feature()) {
        case Sample:
        case AttributesDefinition:
        case TracePointSample:
            return true;
        default:
            return false;
        }
    }

    bool isMeta() const {
        switch (feature()) {
        case ThreadStart:
        case ThreadEnd:
        case LostDefinition:
        case ContextSwitchDefinition:
        case InvalidFeature:
            return true;
        default:
            return false;
        }
    }

    const Attribute &attribute() const
    {
        static const Attribute empty = Attribute();
        return isAttribute() ? m_attribute : empty;
    }

    const Location &location() const
    {
        static const Location empty = Location();
        return isLocation() ? m_location : empty;
    }

    const Meta &meta() const
    {
        static const Meta empty = Meta();
        return isMeta() ? m_meta : empty;
    }

private:
    friend QDataStream &operator>>(QDataStream &stream, PerfEventType &eventType);
    friend QDataStream &operator<<(QDataStream &stream, const PerfEventType &eventType);

    union {
        Attribute m_attribute;
        Location m_location;
        Meta m_meta;
    };
};

inline QDataStream &operator>>(QDataStream &stream, PerfEventType::Attribute &attribute)
{
    return stream >> attribute.type >> attribute.config >> attribute.name
                  >> attribute.usesFrequency >> attribute.frequencyOrPeriod;
}

inline QDataStream &operator<<(QDataStream &stream, const PerfEventType::Attribute &attribute)
{
    return stream << attribute.type << attribute.config << attribute.name
                  << attribute.usesFrequency << attribute.frequencyOrPeriod;
}

inline QDataStream &operator>>(QDataStream &stream, PerfEventType::Location &location)
{
    return stream >> location.address >> location.file >> location.pid
                  >> location.line >> location.column >> location.parentLocationId;
}

inline QDataStream &operator<<(QDataStream &stream, const PerfEventType::Location &location)
{
    return stream << location.address << location.file << location.pid
                  << location.line << location.column << location.parentLocationId;
}

inline QDataStream &operator>>(QDataStream &stream, PerfEventType &eventType)
{
    if (eventType.isAttribute()) {
        stream >> eventType.m_attribute;
        eventType.setFeature(eventType.m_attribute.type == PerfEventType::TypeTracepoint
                             ? PerfEventType::TracePointSample : PerfEventType::Sample);
        return stream;
    } else if (eventType.isLocation()) {
        return stream >> eventType.m_location;
    } else {
        return stream;
    }
}

inline QDataStream &operator<<(QDataStream &stream, const PerfEventType &eventType)
{
    if (eventType.isAttribute())
        return stream << eventType.m_attribute;
    else if (eventType.isLocation())
        return stream << eventType.m_location;
    else
        return stream;
}

} // namespace PerfProfiler::Internal
