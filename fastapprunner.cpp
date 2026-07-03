#include "fastapprunner.h"
#include <KRunner/RunnerContext>
#include <KRunner/QueryMatch>
#include <KApplicationTrader>
#include <KIO/ApplicationLauncherJob>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDir>
#include <QDBusInterface>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusMetaType>
#include <QMetaType>
#include <algorithm>

K_PLUGIN_CLASS_WITH_JSON(FastAppRunner, "fastapprunner.json")

FastAppRunner::FastAppRunner(QObject *parent, const KPluginMetaData &metaData)
    : KRunner::AbstractRunner(parent, metaData)
{
    // Define where to save the launch frequency history
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);
    m_historyFilePath = configDir + "/fastapprunner_history.json";

    loadHistory();
    loadAppsIntoRAM();

    // Set priority high so it appears first
    // setPriority(KRunner::AbstractRunner::HighestPriority);
}

FastAppRunner::~FastAppRunner() = default;

void FastAppRunner::loadAppsIntoRAM()
{
    // Fetch all installed apps once. This prevents D-Bus/Disk overhead during typing.
    KService::List services = KApplicationTrader::query([](const KService::Ptr &service) {
        return !service->noDisplay() && !service->name().isEmpty();
    });

    for (const auto &service : services) {
        m_appCache.push_back({
            service->storageId(),
            service->name(),
            service->name().toLower(),
            service->icon(),
            service
        });
    }
}

void FastAppRunner::loadHistory()
{
    QFile file(m_historyFilePath);
    if (!file.open(QIODevice::ReadOnly)) return;

    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        QJsonObject appCounts = it.value().toObject();
        QHash<QString, int> counts;
        for (auto appIt = appCounts.begin(); appIt != appCounts.end(); ++appIt) {
            counts[appIt.key()] = appIt.value().toInt();
        }
        m_history[it.key()] = counts;
    }
}

void FastAppRunner::saveHistory()
{
    QJsonObject root;
    for (auto it = m_history.begin(); it != m_history.end(); ++it) {
        QJsonObject appCounts;
        for (auto appIt = it.value().begin(); appIt != it.value().end(); ++appIt) {
            appCounts[appIt.key()] = appIt.value();
        }
        root[it.key()] = appCounts;
    }

    QFile file(m_historyFilePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson());
    }
}

void FastAppRunner::match(KRunner::RunnerContext &context)
{
    QString query = context.query();
    if (query.length() < 1) return;

    QString lowerQuery = query.toLower();
    QList<KRunner::QueryMatch> matches;

    // --- Virtual desktop mode: "x/" prefix ---
    if (lowerQuery.startsWith(QLatin1String("x/"))) {
        ensureVirtualDesktopsCached();

        QString searchTerm = lowerQuery.mid(2); // strip "x/"

        for (int i = 0; i < m_vdNames.size(); ++i) {
            if (searchTerm.isEmpty() || m_vdLowerNames[i].contains(searchTerm)) {
                KRunner::QueryMatch match(this);
                match.setText(m_vdNames[i]);
                match.setIconName(QStringLiteral("desktop"));
                match.setData(QStringLiteral("vd:") + m_vdIds[i]);
                match.setId(m_vdIds[i]);
                // Full list when empty, high relevance when filtering
                match.setRelevance(searchTerm.isEmpty() ? 0.1 : 0.9);
                matches << match;
            }
        }

        context.addMatches(matches);
        return; // Do NOT fall through to app search
    }

    // --- Normal app search mode ---

    // Get the launch history for this exact string (e.g. "c" or "ch")
    QHash<QString, int> queryHistory = m_history.value(lowerQuery);

    for (const auto &app : m_appCache) {
        // Fast substring match
        if (app.lowerName.startsWith(lowerQuery) || app.lowerName.contains(lowerQuery)) {
            KRunner::QueryMatch match(this);
            match.setText(app.name);
            match.setIconName(app.icon);
            match.setData(app.id); // Save ID for the run() function
            match.setId(app.id);

            // Calculation based on EXACT query frequency
            int launchCount = queryHistory.value(app.id, 0);

            // Relevance determines sorting order (0.0 to 1.0).
            // Apps you've launched with this string get massive boosts.
            qreal relevance = 0.5; // Base relevance
            if (app.lowerName.startsWith(lowerQuery)) relevance += 0.2; // Bonus for starting with letter

            if (launchCount > 0) {
                // Add a boost based on count. Capped at +0.3 to keep it in 0-1 range.
                relevance += std::min(0.3, (launchCount * 0.05));
            }

            match.setRelevance(relevance);
            matches << match;
        }
    }

    context.addMatches(matches);
}

void FastAppRunner::run(const KRunner::RunnerContext &context, const KRunner::QueryMatch &action)
{
    QString data = action.data().toString();

    // --- Virtual desktop switch ---
    if (data.startsWith(QStringLiteral("vd:"))) {
        QString desktopId = data.mid(3);

        QDBusInterface props(QStringLiteral("org.kde.KWin"),
                             QStringLiteral("/VirtualDesktopManager"),
                             QStringLiteral("org.freedesktop.DBus.Properties"));
        props.call(QStringLiteral("Set"),
                   QStringLiteral("org.kde.KWin.VirtualDesktopManager"),
                   QStringLiteral("current"),
                   QVariant(desktopId));
        return;
    }

    // --- Normal app launch ---
    QString query = context.query().toLower();
    QString appId = data;

    // 1. Update frequency for this exact typed string
    m_history[query][appId]++;
    saveHistory();

    // 2. Launch the app
    KService::Ptr service = KService::serviceByStorageId(appId);
    if (service) {
        auto *job = new KIO::ApplicationLauncherJob(service);
        job->start();
    }
}

void FastAppRunner::ensureVirtualDesktopsCached()
{
    if (m_vdCached) return;

    // Register the D-Bus meta-type once (thread-safe static)
    static const bool registered = []() {
        qDBusRegisterMetaType<DesktopEntry>();
        return true;
    }();
    (void)registered;

    QDBusInterface props(QStringLiteral("org.kde.KWin"),
                         QStringLiteral("/VirtualDesktopManager"),
                         QStringLiteral("org.freedesktop.DBus.Properties"));

    QDBusMessage reply = props.call(QStringLiteral("Get"),
                                     QStringLiteral("org.kde.KWin.VirtualDesktopManager"),
                                     QStringLiteral("desktops"));

    if (reply.type() != QDBusMessage::ReplyMessage) {
        m_vdCached = true;
        return;
    }

    QVariant outer = reply.arguments().at(0);

    // outer contains QDBusVariant -> extract inner QDBusArgument
    QMetaType metaType = outer.metaType();
    void *ptr = metaType.create(outer.constData());
    QVariant inner = *static_cast<QVariant *>(ptr);
    metaType.destroy(ptr);

    QDBusArgument arg = inner.value<QDBusArgument>();
    if (arg.currentSignature() != QLatin1String("a(uss)")) {
        m_vdCached = true;
        return;
    }

    QList<DesktopEntry> desktops;
    arg >> desktops;

    for (const auto &d : desktops) {
        m_vdNames << d.name;
        m_vdLowerNames << d.name.toLower();
        m_vdIds << d.id;
    }

    m_vdCached = true;
}

#include "fastapprunner.moc"
