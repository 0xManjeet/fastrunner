#include "fastapprunner.h"
#include <KRunner/RunnerContext>
#include <KRunner/QueryMatch>
#include <KApplicationTrader>
#include <KIO/ApplicationLauncherJob>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDir>
#include <QDBusInterface>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusMetaType>
#include <QMetaType>
#include <QDateTime>
#include <QClipboard>
#include <QGuiApplication>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QProcessEnvironment>
#include <algorithm>

K_PLUGIN_CLASS_WITH_JSON(FastAppRunner, "fastapprunner.json")

static const QString kAiPrefix = QStringLiteral(">");
static const QString kAiSuffix = QStringLiteral("/");

FastAppRunner::FastAppRunner(QObject *parent, const KPluginMetaData &metaData)
    : KRunner::AbstractRunner(parent, metaData)
{
    // Define where to save the launch frequency history
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);
    m_historyFilePath = configDir + QStringLiteral("/fastapprunner_history.json");
    m_aiConfigFilePath = configDir + QStringLiteral("/fastapprunner_ai.json");

    loadHistory();
    loadAppsIntoRAM();
    loadAiConfig();

    // Set priority high so it appears first
    // setPriority(KRunner::AbstractRunner::HighestPriority);
}

FastAppRunner::~FastAppRunner()
{
    cancelAiRequest();
}

void FastAppRunner::loadAiConfig()
{
    // 1) Environment variable wins (matches curl example)
    const QString envKey = QProcessEnvironment::systemEnvironment()
                               .value(QStringLiteral("CEREBRAS_API_KEY"));
    if (!envKey.isEmpty()) {
        m_aiApiKey = envKey;
    }

    // 2) Optional config file overrides URL/model/prompt; fills key if env unset
    QFile file(m_aiConfigFilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (m_aiApiKey.isEmpty()) {
        m_aiApiKey = root.value(QStringLiteral("apiKey")).toString();
    }
    const QString url = root.value(QStringLiteral("apiUrl")).toString();
    if (!url.isEmpty()) {
        m_aiApiUrl = url;
    }
    const QString model = root.value(QStringLiteral("model")).toString();
    if (!model.isEmpty()) {
        m_aiModel = model;
    }
    const QString systemPrompt = root.value(QStringLiteral("systemPrompt")).toString();
    if (!systemPrompt.isEmpty()) {
        m_aiSystemPrompt = systemPrompt;
    }
}

void FastAppRunner::cancelAiRequest()
{
    if (m_aiReply) {
        m_aiReply->disconnect(this);
        m_aiReply->abort();
        m_aiReply->deleteLater();
        m_aiReply = nullptr;
    }
}

void FastAppRunner::requestAiAnswer(KRunner::RunnerContext context, const QString &prompt)
{
    if (m_aiApiKey.isEmpty()) {
        m_aiCacheQuery = context.query();
        m_aiCacheAnswer = QStringLiteral(
            "No API key. Set CEREBRAS_API_KEY or put apiKey in:\n") + m_aiConfigFilePath;
        return;
    }

    // Already have a fresh answer for this exact query
    if (m_aiCacheQuery == context.query() && !m_aiCacheAnswer.isEmpty()) {
        return;
    }

    // Request already in flight for this query
    if (m_aiReply && m_aiCacheQuery == context.query()) {
        return;
    }

    cancelAiRequest();
    m_aiCacheQuery = context.query();
    m_aiCacheAnswer.clear();

    QJsonArray messages;
    messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("system")},
        {QStringLiteral("content"), m_aiSystemPrompt},
    });
    messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("content"), prompt},
    });

    QJsonObject body{
        {QStringLiteral("model"), m_aiModel},
        {QStringLiteral("stream"), false},
        {QStringLiteral("max_tokens"), 1024},
        {QStringLiteral("temperature"), 1},
        {QStringLiteral("top_p"), 0.95},
        {QStringLiteral("messages"), messages},
    };

    QNetworkRequest req{QUrl(m_aiApiUrl)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization",
                     QByteArray("Bearer ") + m_aiApiKey.toUtf8());
    // Avoid Qt caching any responses
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);

    m_aiReply = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));

    // Capture context by value so we can check isValid() when the reply finishes.
    // mutable: addMatch() is non-const on the captured copy.
    connect(m_aiReply, &QNetworkReply::finished, this, [this, context]() mutable {
        QNetworkReply *reply = m_aiReply;
        m_aiReply = nullptr;
        if (!reply) {
            return;
        }

        const QString query = context.query();
        QString answer;

        if (reply->error() != QNetworkReply::NoError) {
            answer = QStringLiteral("AI error: ") + reply->errorString();
            // Surface HTTP body when available (auth/rate-limit messages)
            const QByteArray errBody = reply->readAll();
            if (!errBody.isEmpty()) {
                const QJsonObject errJson = QJsonDocument::fromJson(errBody).object();
                const QString msg = errJson.value(QStringLiteral("error")).toObject()
                                        .value(QStringLiteral("message")).toString();
                if (!msg.isEmpty()) {
                    answer = QStringLiteral("AI error: ") + msg;
                }
            }
        } else {
            const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
            const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
            if (!choices.isEmpty()) {
                answer = choices.at(0).toObject()
                             .value(QStringLiteral("message")).toObject()
                             .value(QStringLiteral("content")).toString()
                             .trimmed();
            }
            if (answer.isEmpty()) {
                answer = QStringLiteral("Empty response from AI");
            }
        }

        reply->deleteLater();

        // Only cache / re-add if this is still the active query
        if (!context.isValid()) {
            return;
        }

        m_aiCacheQuery = query;
        m_aiCacheAnswer = answer;

        KRunner::QueryMatch match(this);
        match.setText(answer);
        match.setMultiLine(true);
        match.setSubtext(QStringLiteral("Copy answer to clipboard"));
        match.setIconName(QStringLiteral("dialog-messages"));
        match.setData(QStringLiteral("ai:") + answer);
        match.setId(QStringLiteral("ai-answer"));
        match.setRelevance(1.0);
        context.addMatch(match);
    });
}

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

    // --- AI mode: >prompt/ ---
    // Starts with ">". When closed with trailing "/", fire the API request.
    // Note: checked before virtual-desktop mode ("/...") so ">" never collides.
    if (query.startsWith(kAiPrefix)) {
        const bool complete = query.endsWith(kAiSuffix) && query.length() > kAiPrefix.size() + kAiSuffix.size();

        if (!complete) {
            KRunner::QueryMatch match(this);
            match.setText(QStringLiteral("AI: type your question, end with /"));
            match.setSubtext(QStringLiteral("Example: >capital of France/"));
            match.setIconName(QStringLiteral("dialog-messages"));
            match.setData(QStringLiteral("ai-hint"));
            match.setId(QStringLiteral("ai-hint"));
            match.setRelevance(1.0);
            matches << match;
            context.addMatches(matches);
            return;
        }

        const QString prompt = query.mid(kAiPrefix.size(),
                                         query.size() - kAiPrefix.size() - kAiSuffix.size())
                                   .trimmed();
        if (prompt.isEmpty()) {
            KRunner::QueryMatch match(this);
            match.setText(QStringLiteral("AI: empty question"));
            match.setIconName(QStringLiteral("dialog-warning"));
            match.setData(QStringLiteral("ai-hint"));
            match.setId(QStringLiteral("ai-empty"));
            match.setRelevance(1.0);
            matches << match;
            context.addMatches(matches);
            return;
        }

        // Cached answer for this exact query
        if (m_aiCacheQuery == query && !m_aiCacheAnswer.isEmpty()) {
            KRunner::QueryMatch match(this);
            match.setText(m_aiCacheAnswer);
            match.setMultiLine(true);
            match.setSubtext(QStringLiteral("Copy answer to clipboard"));
            match.setIconName(QStringLiteral("dialog-messages"));
            match.setData(QStringLiteral("ai:") + m_aiCacheAnswer);
            match.setId(QStringLiteral("ai-answer"));
            match.setRelevance(1.0);
            matches << match;
            context.addMatches(matches);
            return;
        }

        // Kick off (or reuse in-flight) request; show loading row immediately
        requestAiAnswer(context, prompt);

        KRunner::QueryMatch match(this);
        if (m_aiApiKey.isEmpty() && !m_aiCacheAnswer.isEmpty()) {
            match.setText(m_aiCacheAnswer);
            match.setMultiLine(true);
            match.setSubtext(QStringLiteral("Configure API key"));
            match.setIconName(QStringLiteral("dialog-warning"));
            match.setData(QStringLiteral("ai:") + m_aiCacheAnswer);
        } else {
            match.setText(QStringLiteral("Asking AI…"));
            match.setSubtext(prompt);
            match.setIconName(QStringLiteral("view-refresh"));
            match.setData(QStringLiteral("ai-loading"));
        }
        match.setId(QStringLiteral("ai-pending"));
        match.setRelevance(1.0);
        matches << match;
        context.addMatches(matches);
        return;
    }

    // --- Virtual desktop mode: "/" prefix ---
    if (lowerQuery.startsWith(QLatin1String("/"))) {
        ensureVirtualDesktopsCached();

        QString searchTerm = lowerQuery.mid(1); // strip "/"

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

    // --- Date/time: "date" shows current date in dd MMM yy, h:mm AM/PM ---
    // Match while typing d / da / dat / date
    if (QStringLiteral("date").startsWith(lowerQuery)) {
        const QString formatted = QDateTime::currentDateTime()
            .toString(QStringLiteral("dd MMM yy, h:mm AP"));

        KRunner::QueryMatch match(this);
        match.setText(formatted);
        match.setSubtext(QStringLiteral("Copy date to clipboard"));
        match.setIconName(QStringLiteral("clock"));
        match.setData(QStringLiteral("date:") + formatted);
        match.setId(QStringLiteral("current-date"));
        match.setRelevance(lowerQuery == QLatin1String("date") ? 1.0 : 0.85);
        matches << match;

        // Exact "date": only show the formatted date, not app matches
        if (lowerQuery == QLatin1String("date")) {
            context.addMatches(matches);
            return;
        }
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

        QDBusInterface kwin(QStringLiteral("org.kde.KWin"),
                              QStringLiteral("/VirtualDesktopManager"),
                              QStringLiteral("org.kde.KWin.VirtualDesktopManager"));
        kwin.setProperty("current", QVariant(desktopId));
        return;
    }

    // --- Copy formatted date to clipboard ---
    if (data.startsWith(QStringLiteral("date:"))) {
        const QString text = data.mid(5);
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(text);
        }
        return;
    }

    // --- Copy AI answer to clipboard ---
    if (data.startsWith(QStringLiteral("ai:"))) {
        const QString text = data.mid(3);
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(text);
        }
        return;
    }
    if (data == QStringLiteral("ai-hint") || data == QStringLiteral("ai-loading")) {
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
