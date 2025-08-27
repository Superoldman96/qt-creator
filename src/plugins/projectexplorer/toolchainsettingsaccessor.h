// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <utils/settingsaccessor.h>

#include <QList>

namespace ProjectExplorer {

class Toolchain;

namespace Internal {

class ToolchainSettingsAccessor : public Utils::UpgradingSettingsAccessor
{
public:
    ToolchainSettingsAccessor();

    QList<Toolchain *> restoreToolchains() const;

    void saveToolchains(const QList<Toolchain *> &toolchains);

private:
    QList<Toolchain *> toolChains(const Utils::Store &data) const;
};

QObject *createToolchainSettingsTest();

} // namespace Internal
} // namespace ProjectExplorer
