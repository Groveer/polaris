// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "balanceconfig.h"

#include <QFileSystemWatcher>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QVariantMap>

namespace polaris {

class BalanceMonitor : public QObject
{
    Q_OBJECT

public:
    explicit BalanceMonitor(QObject *parent = nullptr);
    void setEnabled(bool enabled);
    void refresh();
    QVariantMap data() const;

Q_SIGNALS:
    void changed();

private:
    bool reload();
    void fetch();
    void cancel();
    void finish(QNetworkReply *reply);
    void watchDirectory();

    BalanceConfig m_config;
    QTimer m_timer;
    QFileSystemWatcher m_watcher;
    QNetworkAccessManager m_network;
    QNetworkReply *m_reply = nullptr;
    QVariantMap m_snapshot;
    QString m_status;
    bool m_enabled = false;
    bool m_canRefresh = false;
};

} // namespace polaris
