// Copyright (C) 2016 Petar Perisin <petar.perisin@gmail.com>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <QDialog>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QGroupBox;
class QPushButton;
class QRadioButton;
QT_END_NAMESPACE

namespace Git::Internal {

class BranchCheckoutDialog : public QDialog
{
public:
    explicit BranchCheckoutDialog(QWidget *parent, const QString &currentBranch,
                                  const QString &nextBranch);
    ~BranchCheckoutDialog() override;

    void foundNoLocalChanges();
    void foundStashForNextBranch();

    bool makeStashOfCurrentBranch() const;
    bool moveLocalChangesToNextBranch() const;
    bool discardLocalChanges() const;
    bool popStashOfNextBranch() const;

    bool hasStashForNextBranch() const;
    bool hasLocalChanges() const;
    bool diffRequested() const;

private:
    void updatePopStashCheckBox(bool moveChangesChecked);

    bool m_foundStashForNextBranch = false;
    bool m_hasLocalChanges = true;
    bool m_diffRequested = false;

    QGroupBox *m_localChangesGroupBox = nullptr;
    QRadioButton *m_makeStashRadioButton = nullptr;
    QRadioButton *m_moveChangesRadioButton = nullptr;
    QRadioButton *m_discardChangesRadioButton = nullptr;
    QCheckBox *m_popStashCheckBox = nullptr;
    QPushButton *m_diffButton = nullptr;
};

} // Git::Internal
