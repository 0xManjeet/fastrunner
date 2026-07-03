#pragma once

#include <KRunner/AbstractRunner>
#include <KService>
#include <QJsonObject>
#include <QHash>
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

    std::vector<AppCacheItem> m_appCache;
    
    // Structure: m_history["c"]["google-chrome.desktop"] = 11
    QHash<QString, QHash<QString, int>> m_history;
    QString m_historyFilePath;
};
