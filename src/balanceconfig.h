// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace polaris {

struct BalanceConfig {
    QString name = QStringLiteral("AIHub");
    QString url = QStringLiteral("https://aihub.top/v1/usage");
    QString auth = QStringLiteral("bearer");
    QString path = QStringLiteral("balance");
    QString unit = QStringLiteral("USD");
    QByteArray key;
    int minutes = 5;
};

QString balanceConfigPath();
BalanceConfig loadBalanceConfig();
QString validateBalanceConfig(const BalanceConfig &config);
QString saveBalanceConfig(const BalanceConfig &config);

} // namespace polaris
