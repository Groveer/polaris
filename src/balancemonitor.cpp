// SPDX-License-Identifier: GPL-3.0-or-later
#include "balancemonitor.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkReply>
#include <cmath>
#include <optional>
#include <utility>

namespace polaris {
namespace {
constexpr qint64 maxResponseBytes = 1024 * 1024;

std::optional<double> number(const QJsonValue &value)
{
    bool ok = value.isDouble();
    const double n = ok ? value.toDouble() : value.toString().toDouble(&ok);
    if (!ok || !std::isfinite(n))
        return std::nullopt;
    return n;
}

QString amount(double value, const QString &unit, int decimals)
{
    QString result = QLocale().toString(value, 'f', decimals);
    if (decimals > 2) {
        if (const auto dot = result.indexOf(QLocale().decimalPoint()); dot >= 0) {
            const auto zero = QLocale().zeroDigit();
            while (result.size() > dot + QLocale().decimalPoint().size() + 2 && result.endsWith(zero))
                result.chop(zero.size());
        }
    }
    return unit.isEmpty() ? result : result + u' ' + unit;
}

QString compactAmount(double value, const QString &unit)
{
    QString suffix;
    if (std::abs(value) >= 1e9)
        return (unit == QStringLiteral("USD") ? QStringLiteral("$") : QString()) + QString::number(value, 'g', 3);
    if (std::abs(value) >= 1e6) {
        value /= 1e6;
        suffix = QStringLiteral("M");
    } else if (std::abs(value) >= 1e3) {
        value /= 1e3;
        suffix = QStringLiteral("k");
    }
    return (unit == QStringLiteral("USD") ? QStringLiteral("$") : QString())
           + QString::number(value, 'f', 2) + suffix;
}
} // namespace

BalanceMonitor::BalanceMonitor(QObject *parent)
    : QObject(parent)
{
    m_timer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &BalanceMonitor::refresh);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        if (m_enabled && reload())
            fetch();
    });
}

void BalanceMonitor::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (!enabled) {
        m_timer.stop();
        const auto paths = m_watcher.directories();
        if (!paths.isEmpty())
            m_watcher.removePaths(paths);
        cancel();
        m_snapshot.clear();
        m_status.clear();
        m_config.key.clear();
        m_canRefresh = false;
        Q_EMIT changed();
        return;
    }
    QDir().mkpath(QFileInfo(balanceConfigPath()).absolutePath());
    refresh();
    if (!m_timer.isActive())
        m_timer.start(m_config.minutes * 60 * 1000);
}

void BalanceMonitor::watchDirectory()
{
    const auto directory = QFileInfo(balanceConfigPath()).absolutePath();
    if (!m_watcher.directories().contains(directory) && QFileInfo(directory).isDir())
        m_watcher.addPath(directory);
}

bool BalanceMonitor::reload()
{
    watchDirectory();
    auto config = loadBalanceConfig();
    const bool identityChanged = m_config.url != config.url || m_config.auth != config.auth
        || m_config.key != config.key || m_config.path != config.path || m_config.unit != config.unit;
    const bool nameChanged = m_config.name != config.name;
    const bool intervalChanged = m_config.minutes != config.minutes;
    const QString error = validateBalanceConfig(config);
    const bool valid = error.isEmpty();
    if (identityChanged || !valid) {
        cancel();
        m_snapshot.clear();
    }
    m_config = std::move(config);
    if (intervalChanged)
        m_timer.start(qBound(5, m_config.minutes, 60) * 60 * 1000);
    if (identityChanged || m_canRefresh != valid || !valid)
        m_status = error;
    m_canRefresh = valid;
    if (identityChanged || nameChanged || !valid)
        Q_EMIT changed();
    return identityChanged;
}

void BalanceMonitor::refresh()
{
    if (!m_enabled)
        return;
    reload();
    fetch();
}

void BalanceMonitor::fetch()
{
    if (!m_canRefresh || m_reply)
        return;
    QNetworkRequest request{QUrl(m_config.url, QUrl::StrictMode)};
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    if (m_config.auth == QStringLiteral("bearer"))
        request.setRawHeader("Authorization", "Bearer " + m_config.key);
    else if (m_config.auth == QStringLiteral("x-api-key"))
        request.setRawHeader("x-api-key", m_config.key);
    auto *reply = m_network.get(request);
    m_reply = reply;
    reply->setReadBufferSize(maxResponseBytes + 1);
    connect(reply, &QIODevice::readyRead, this, [reply] {
        if (reply->bytesAvailable() > maxResponseBytes) {
            reply->setProperty("tooLarge", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finish(reply); });
    QTimer::singleShot(15000, reply, [reply] {
        if (reply->isRunning()) {
            reply->setProperty("timedOut", true);
            reply->abort();
        }
    });
    m_timer.start(m_config.minutes * 60 * 1000);
    Q_EMIT changed();
}

void BalanceMonitor::cancel()
{
    if (auto *reply = std::exchange(m_reply, nullptr)) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void BalanceMonitor::finish(QNetworkReply *reply)
{
    m_reply = nullptr;
    reply->deleteLater();
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto fail = [this](const QString &message) {
        m_status = m_snapshot.isEmpty() ? message : tr("余额未更新：%1").arg(message);
        Q_EMIT changed();
    };
    if (reply->property("tooLarge").toBool() || reply->bytesAvailable() > maxResponseBytes)
        return fail(tr("接口响应过大。"));
    if (reply->property("timedOut").toBool())
        return fail(tr("请求超时。"));
    if (http == 401 || http == 403)
        return fail(tr("接口拒绝访问（HTTP %1），请检查 API Key 和平台访问限制。").arg(http));
    if (http >= 300)
        return fail(tr("余额接口返回 HTTP %1。").arg(http));
    if (reply->error() != QNetworkReply::NoError)
        return fail(tr("无法连接余额接口。"));
    const auto document = QJsonDocument::fromJson(reply->readAll());
    if (!document.isObject())
        return fail(tr("余额接口未返回 JSON 对象。"));
    const auto object = document.object();
    if (object.value("isValid") == QJsonValue(false))
        return fail(tr("接口报告 API Key 无效。"));
    QJsonValue field(object);
    for (const auto &part : m_config.path.split(u'.'))
        field = field.toObject().value(part);
    const auto balance = number(field);
    if (!balance)
        return fail(tr("未找到数字形式的余额，请检查 JSON 字段设置。"));

    m_snapshot = {{"compact", compactAmount(*balance, m_config.unit)},
                  {"balance", amount(*balance, m_config.unit, 8)},
                  {"updated", QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat)}};
    const auto usage = object.value("usage").toObject();
    for (const auto &period : {QStringLiteral("today"), QStringLiteral("total")}) {
        const auto stats = usage.value(period).toObject();
        if (auto value = number(stats.value("actual_cost")))
            m_snapshot.insert(period + QStringLiteral("Cost"), amount(*value, m_config.unit, 8));
        if (auto value = number(stats.value("requests")))
            m_snapshot.insert(period + QStringLiteral("Requests"), QLocale().toString(*value, 'f', 0));
        if (auto value = number(stats.value("total_tokens")))
            m_snapshot.insert(period + QStringLiteral("Tokens"), QLocale().toString(*value, 'f', 0));
    }
    m_status.clear();
    Q_EMIT changed();
}

QVariantMap BalanceMonitor::data() const
{
    auto result = m_snapshot;
    result.insert("name", m_config.name);
    result.insert("status", m_status);
    result.insert("busy", m_reply != nullptr);
    result.insert("canRefresh", m_canRefresh);
    return result;
}
} // namespace polaris
