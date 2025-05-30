// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <coreplugin/locator/ilocatorfilter.h>

namespace CppEditor::Internal {

class CppIncludesFilter final : public Core::ILocatorFilter
{
public:
    CppIncludesFilter();

private:
    Core::LocatorMatcherTasks matchers() final { return {m_cache.matcher()}; }
    Core::LocatorFileCache m_cache;
};

} // namespace CppEditor::Internal
