// SPDX-License-Identifier: GPL-3.0-or-later
#include "balanceconfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace polaris {
namespace {
constexpr qint64 maxConfigBytes = 16384;
QString message(const char *text) { return QCoreApplication::translate("BalanceConfig", text); }
}

QString balanceConfigPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/org.deepin.ds.dock.polaris/balance.json");
}

QString validateBalanceConfig(const BalanceConfig &c)
{
    const QUrl url(c.url, QUrl::StrictMode);
    if (c.url.size() > 2048 || !url.isValid() || url.scheme() != QStringLiteral("https")
        || url.host().isEmpty() || url.authority().contains(u'@') || url.hasFragment())
        return message("请输入不含用户名、密码或片段的 HTTPS 接口地址。");
    static const QRegularExpression field(QStringLiteral("\\A[\\w-]+(?:\\.[\\w-]+)*\\z"));
    if (c.path.size() > 120 || !field.match(c.path).hasMatch())
        return message("请输入 JSON 余额字段，例如 data.balance。");
    static const QRegularExpression unsafe(QStringLiteral("[\\p{C}<>&]"));
    if (c.name.trimmed().isEmpty() || c.name.size() > 16 || c.unit.size() > 8
        || unsafe.match(c.name).hasMatch() || unsafe.match(c.unit).hasMatch())
        return message("平台名称最多 16 个字符，单位最多 8 个字符，请使用普通文本。");
    if (c.minutes < 5 || c.minutes > 60)
        return message("刷新间隔必须在 5 到 60 分钟之间。");
    if (c.auth == QStringLiteral("none"))
        return {};
    if (c.auth != QStringLiteral("bearer") && c.auth != QStringLiteral("x-api-key"))
        return message("请选择受支持的鉴权方式。");
    if (c.key.isEmpty())
        return message("请在控制中心配置 API Key。");
    if (c.key.size() > 4096 || c.key.startsWith("Bearer ")
        || std::any_of(c.key.cbegin(), c.key.cend(), [](unsigned char ch) { return ch <= 32 || ch >= 127; }))
        return message("请输入原始 API Key，不包含 Bearer 前缀、空格或换行。");
    return {};
}

BalanceConfig loadBalanceConfig()
{
    BalanceConfig c;
    const auto path = QFile::encodeName(balanceConfigPath());
    const int fd = ::open(path.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return c;
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid()
        || (info.st_mode & 0077) != 0 || info.st_size > maxConfigBytes) {
        ::close(fd);
        return c;
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return c;
    }
    const auto json = QJsonDocument::fromJson(file.read(maxConfigBytes + 1)).object();
    // Read the destination and credential as a pair. An incomplete file must
    // never use the default AIHub URL with a key meant for another platform.
    for (const auto *field : {"name", "url", "auth", "path", "unit", "key"}) {
        if (!json.value(QLatin1String(field)).isString())
            return {};
    }
    if (!json.value("minutes").isDouble())
        return {};
    c.name = json.value("name").toString();
    c.url = json.value("url").toString();
    c.auth = json.value("auth").toString();
    c.path = json.value("path").toString();
    c.unit = json.value("unit").toString();
    c.minutes = json.value("minutes").toInt();
    c.key = json.value("key").toString().toUtf8();
    return validateBalanceConfig(c).isEmpty() ? c : BalanceConfig{};
}

QString saveBalanceConfig(const BalanceConfig &c)
{
    if (const QString error = validateBalanceConfig(c); !error.isEmpty())
        return error;
    const auto path = balanceConfigPath();
    const QString error = message("无法安全保存 API 余额配置，请检查用户配置目录权限。");
    if (!QDir().mkpath(QFileInfo(path).absolutePath()) || QFileInfo(path).isSymLink())
        return error;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return error;
    const QByteArray data = QJsonDocument(QJsonObject{
        {"name", c.name}, {"url", c.url}, {"auth", c.auth}, {"path", c.path}, {"unit", c.unit},
        {"minutes", c.minutes}, {"key", c.auth == QStringLiteral("none") ? QString() : QString::fromLatin1(c.key)}
    }).toJson(QJsonDocument::Compact);
    if (file.write(data) != data.size() || !file.commit())
        return error;
    return {};
}

} // namespace polaris
