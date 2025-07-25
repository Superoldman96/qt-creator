// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <utils/algorithm.h>
#include <utils/filepath.h>

#include <QDir>
#include <QString>

namespace ProjectExplorer {

enum class HeaderPathType {
    User,
    BuiltIn,
    System,
    Framework,
};

class HeaderPath
{
public:
    HeaderPath() = default;
    HeaderPath(const QString &path, HeaderPathType type)
        : path(Utils::FilePath::fromUserInput(path)), type(type)
    {}
    HeaderPath(const Utils::FilePath &path, HeaderPathType type)
        : path(path), type(type)
    {}

    bool operator==(const HeaderPath &other) const
    {
        return type == other.type && path == other.path;
    }

    bool operator!=(const HeaderPath &other) const
    {
        return !(*this == other);
    }

    static HeaderPath makeUser(const Utils::FilePath &fp)
    {
        return {fp, HeaderPathType::User};
    }
    template<typename F> static HeaderPath makeBuiltIn(const F &fp)
    {
        return {fp, HeaderPathType::BuiltIn};
    }
    template<typename F> static HeaderPath makeSystem(const F &fp)
    {
        return {fp, HeaderPathType::System};
    }
    template<typename F> static HeaderPath makeFramework(const F &fp)
    {
        return {fp, HeaderPathType::Framework};
    }

    friend auto qHash(const HeaderPath &key, uint seed = 0)
    {
        return ((qHash(key.path) << 2) | uint(key.type)) ^ seed;
    }

    Utils::FilePath path;
    HeaderPathType type = HeaderPathType::User;
};

using HeaderPaths = QList<HeaderPath>;
template<typename C> HeaderPaths toHeaderPaths(const C &list, HeaderPathType type)
{
    return Utils::transform<HeaderPaths>(list, [type](const auto &fp) {
        return HeaderPath(fp, type);
    });
}
template<typename C> HeaderPaths toUserHeaderPaths(const C &list)
{
    return toHeaderPaths(list, HeaderPathType::User);
}
template<typename C> HeaderPaths toBuiltInHeaderPaths(const C &list)
{
    return toHeaderPaths(list, HeaderPathType::BuiltIn);
}
template<typename C> HeaderPaths toFrameworkHeaderPaths(const C &list)
{
    return toHeaderPaths(list, HeaderPathType::Framework);
}

} // namespace ProjectExplorer
