// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "cppeditor_global.h"

#include "cppcodestylesettings.h"

#include <QObject>

namespace CppEditor {

// This class is meant to go away.

class CPPEDITOR_EXPORT CppToolsSettings : public QObject
{
public:
    CppToolsSettings();
    ~CppToolsSettings() override;

    static CppCodeStylePreferences *cppCodeStyle();
};

} // namespace CppEditor
