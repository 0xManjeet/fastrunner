#pragma once

#include <KRunner/AbstractRunner>
#include <KService>
#include <QJsonObject>
#include <QHash>
#include <QStringList>
#include <QDBusArgument>
#include <QNetworkAccessManager>
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
    void loadAiConfig();
    void requestAiAnswer(KRunner::RunnerContext context, const QString &prompt);
    void cancelAiRequest();

    std::vector<AppCacheItem> m_appCache;

    // Structure: m_history["c"]["google-chrome.desktop"] = 11
    QHash<QString, QHash<QString, int>> m_history;
    QString m_historyFilePath;
    QString m_aiConfigFilePath;

    // Virtual desktop cache (lazy-loaded once on first "/" query)
    QStringList m_vdNames;
    QStringList m_vdLowerNames;
    QStringList m_vdIds;
    bool m_vdCached = false;

    // AI query (trigger: >$prompt//)
    QNetworkAccessManager m_nam;
    QNetworkReply *m_aiReply = nullptr;
    QString m_aiCacheQuery;
    QString m_aiCacheAnswer;
    QString m_aiApiKey;
    QString m_aiApiUrl = QStringLiteral("https://api.cerebras.ai/v1/chat/completions");
    QString m_aiModel = QStringLiteral("gemma-4-31b");
    QString m_aiSystemPrompt = QStringLiteral(
        "Answer very concisely in under 15 lines. Use plain text only — no markdown, "
        "no bullet lists with asterisks, no code fences, no headings.");
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
