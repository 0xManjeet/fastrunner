#pragma once

#include <KRunner/AbstractRunner>
#include <KService>
#include <QJsonObject>
#include <QHash>
#include <QStringList>
#include <QDBusArgument>
#include <vector>

// Struct to cache apps in RAM for ultra-fast lookup
struct AppCacheItem {
    QString id;
    QString name;
    QString lowerName;
    QString icon;
    KService::Ptr service;
};

class FastAppRunner : public KRunner::AbstractRunner
{
    Q_OBJECT

public:
    FastAppRunner(QObject *parent, const KPluginMetaData &metaData);
    ~FastAppRunner() override;

    void match(KRunner::RunnerContext &context) override;
    void run(const KRunner::RunnerContext &context, const KRunner::QueryMatch &action) override;

private:
    void loadAppsIntoRAM();
    void loadHistory();
    void saveHistory();
    void ensureVirtualDesktopsCached();

    std::vector<AppCacheItem> m_appCache;

    // Structure: m_history["c"]["google-chrome.desktop"] = 11
    QHash<QString, QHash<QString, int>> m_history;
    QString m_historyFilePath;

    // Virtual desktop cache (lazy-loaded once on first "x/" query)
    QStringList m_vdNames;
    QStringList m_vdLowerNames;
    QStringList m_vdIds;
    bool m_vdCached = false;
};

// D-Bus struct matching the a(uss) signature from KWin's desktops property
struct DesktopEntry {
    quint32 position;
    QString id;
    QString name;
};
Q_DECLARE_METATYPE(DesktopEntry)

inline QDBusArgument &operator<<(QDBusArgument &arg, const DesktopEntry &entry)
{
    arg.beginStructure();
    arg << entry.position << entry.id << entry.name;
    arg.endStructure();
    return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg, DesktopEntry &entry)
{
    arg.beginStructure();
    arg >> entry.position >> entry.id >> entry.name;
    arg.endStructure();
    return arg;
}
