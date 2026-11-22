#include "LocalApiServer.h"
#include "CommandGateway.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QWindow>

#include <utility>

#ifndef OpenDaw_VERSION
#define OpenDaw_VERSION "dev"
#endif

namespace OpenDaw {

namespace {

const QString kAuthMethod = QStringLiteral("opendaw/authenticate");

// The name to connect to for a discovery record: the socket path on Unix
// (independent of TMPDIR), the pipe name on Windows.
QString discoveryEndpoint(const QJsonObject& info)
{
#ifdef Q_OS_WIN
    return info.value("serverName").toString();
#else
    const QString full = info.value("fullServerName").toString();
    return full.isEmpty() ? info.value("serverName").toString() : full;
#endif
}

bool writeOwnerOnlyFile(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    const auto ownerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.setPermissions(ownerOnly); // the temporary file, before the token is written
    if (file.write(bytes) != bytes.size())
        return false;
    if (!file.commit())
        return false;
    QFile::setPermissions(path, ownerOnly);
    return true;
}

} // namespace

LocalApiServer::LocalApiServer(CommandGateway& gateway, QObject* parent)
    : QObject(parent)
    , gateway_(gateway)
{
    retryTimer_ = new QTimer(this);
    retryTimer_->setSingleShot(true);
    retryTimer_->setInterval(kDeferRetryMs);
    connect(retryTimer_, &QTimer::timeout, this, &LocalApiServer::processQueue);
}

LocalApiServer::~LocalApiServer()
{
    stop();
}

QString LocalApiServer::defaultServerName()
{
    return QStringLiteral("opendaw-agent-api");
}

QString LocalApiServer::discoveryFilePath()
{
    const QString overridePath = qEnvironmentVariable("OPENDAW_AGENT_API_FILE");
    if (!overridePath.isEmpty())
        return QDir::cleanPath(overridePath);
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir::cleanPath(base + QStringLiteral("/OpenDaw/agent-api.json"));
}

bool LocalApiServer::isListening() const
{
    return server_ && server_->isListening();
}

int LocalApiServer::clientCount() const
{
    return int(clients_.size());
}

QString LocalApiServer::serverName() const
{
    return server_ ? server_->serverName() : QString();
}

QString LocalApiServer::fullServerName() const
{
    return server_ ? server_->fullServerName() : QString();
}

QString LocalApiServer::token() const
{
    return token_;
}

void LocalApiServer::setRequireToken(bool require)
{
    requireToken_ = require;
}

void LocalApiServer::setUiBusyPredicate(std::function<bool()> predicate)
{
    uiBusyPredicate_ = std::move(predicate);
}

bool LocalApiServer::isUiBusy() const
{
    // The main loop is level 1 (QCoreApplication::exec). Anything deeper is a
    // QDialog::exec / QMessageBox::exec / QMenu::exec style loop whose caller
    // may hold raw pointers into the Edit.
    if (QThread::currentThread()->loopLevel() > 1)
        return true;
    // A modal window shown with open()/show() does not nest a loop, but its
    // callbacks can hold the same kind of pointers.
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) && QGuiApplication::modalWindow())
        return true;
    return uiBusyPredicate_ && uiBusyPredicate_();
}

void LocalApiServer::scheduleRetry()
{
    // No new readyRead will arrive to wake processQueue, so poll.
    if (retryTimer_ && !retryTimer_->isActive())
        retryTimer_->start();
}

QString LocalApiServer::generateToken()
{
    quint32 words[8];
    QRandomGenerator::system()->fillRange(words, 8);
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(words), sizeof(words)).toHex());
}

bool LocalApiServer::tokenMatches(const QString& candidate) const
{
    // Constant-time comparison.
    const QByteArray a = candidate.toUtf8();
    const QByteArray b = token_.toUtf8();
    if (a.size() != b.size() || b.isEmpty())
        return false;
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

// ── Listening ───────────────────────────────────────────────────────────────

bool LocalApiServer::isServerAlive(const QString& name)
{
    QLocalSocket probe;
    probe.connectToServer(name);
    const bool alive = probe.waitForConnected(300);
    if (alive)
        probe.abort();
    return alive;
}

bool LocalApiServer::listenOn(const QString& name)
{
    // On Windows several servers can listen on the same pipe name (clients
    // then reach a random one), so check for a live owner explicitly.
    if (isServerAlive(name))
        return false;

    // Removes a stale socket file left by a crashed run (no-op on Windows).
    QLocalServer::removeServer(name);

    if (server_->listen(name))
        return true;

    qWarning() << "[agent-api] cannot listen on" << name << ":" << server_->errorString();
    return false;
}

bool LocalApiServer::start(const QString& serverName)
{
    if (server_)
        stop();

    token_ = generateToken();

    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    connect(server_, &QLocalServer::newConnection, this, &LocalApiServer::onNewConnection);

    const QString baseName = serverName.isEmpty() ? defaultServerName() : serverName;
    bool ok = listenOn(baseName);
    if (!ok) {
        const QString fallback = QStringLiteral("%1-%2").arg(baseName).arg(QCoreApplication::applicationPid());
        qWarning() << "[agent-api]" << baseName << "is in use; using" << fallback;
        ok = listenOn(fallback);
    }
    if (!ok) {
        delete server_;
        server_ = nullptr;
        token_.clear();
        return false;
    }

    // When another live instance owns the file, remember it so stop() can
    // hand discovery back to that instance.
    previousDiscovery_.clear();
    {
        QFile existing(discoveryFilePath());
        if (existing.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = existing.readAll();
            const QJsonObject other = QJsonDocument::fromJson(bytes).object();
            const QString endpoint = discoveryEndpoint(other);
            if (other.value("pid").toInteger() != static_cast<qint64>(QCoreApplication::applicationPid())
                && !endpoint.isEmpty() && endpoint != server_->serverName()
                && endpoint != server_->fullServerName() && isServerAlive(endpoint))
                previousDiscovery_ = bytes;
        }
    }

    if (!writeDiscoveryFile())
        qWarning() << "[agent-api] could not write the discovery file" << discoveryFilePath()
                   << "- agents must be given the pipe name and token another way";

    qDebug() << "[agent-api] listening on" << server_->fullServerName();
    emit started(server_->serverName());
    return true;
}

void LocalApiServer::stop()
{
    queue_.clear();
    if (retryTimer_)
        retryTimer_->stop();
    deferring_ = false;

    const auto sockets = clients_.keys();
    clients_.clear();
    for (QLocalSocket* s : sockets) {
        if (!s) continue;
        s->disconnect(this);
        s->abort();
        s->deleteLater();
    }

    if (server_) {
        server_->close();
        delete server_;
        server_ = nullptr;
        removeDiscoveryFile();
    }
    token_.clear();

    if (!sockets.isEmpty())
        emit clientCountChanged(0);
}

// ── Discovery file ──────────────────────────────────────────────────────────

bool LocalApiServer::writeDiscoveryFile() const
{
    QJsonObject info;
    info["name"] = QStringLiteral("opendaw");
    info["serverName"] = server_->serverName();
    info["fullServerName"] = server_->fullServerName();
    info["token"] = token_;
    info["requireToken"] = requireToken_;
    info["pid"] = static_cast<qint64>(QCoreApplication::applicationPid());
    info["protocolVersion"] = CommandGateway::protocolVersion();
    info["appVersion"] = QString::fromUtf8(OpenDaw_VERSION);
    info["platform"] = QSysInfo::productType();
    info["startedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    return writeOwnerOnlyFile(discoveryFilePath(), QJsonDocument(info).toJson(QJsonDocument::Indented));
}

void LocalApiServer::removeDiscoveryFile()
{
    const QString path = discoveryFilePath();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject info = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    // Another OpenDaw instance may have taken the file over; leave it alone.
    if (info.value("pid").toInteger() != static_cast<qint64>(QCoreApplication::applicationPid()))
        return;

    const QByteArray previous = std::exchange(previousDiscovery_, QByteArray());
    if (!previous.isEmpty() && isServerAlive(discoveryEndpoint(QJsonDocument::fromJson(previous).object()))
        && writeOwnerOnlyFile(path, previous))
        return; // the instance we displaced is still running: point agents back at it
    QFile::remove(path);
}

// ── Connections ─────────────────────────────────────────────────────────────

void LocalApiServer::onNewConnection()
{
    if (!server_) return;
    bool added = false;
    while (server_->hasPendingConnections()) {
        QLocalSocket* socket = server_->nextPendingConnection();
        if (!socket) break;

        Client c;
        c.socket = socket;
        c.authenticated = !requireToken_;
        clients_.insert(socket, c);
        added = true;

        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] { onDisconnected(socket); });

        if (requireToken_) {
            QPointer<QLocalSocket> guard(socket);
            QTimer::singleShot(kAuthTimeoutMs, this, [this, guard] {
                if (!guard) return;
                auto it = clients_.find(guard.data());
                if (it != clients_.end() && !it->authenticated) {
                    qWarning() << "[agent-api] closing a connection that did not authenticate in time";
                    guard->abort();
                }
            });
        }

        // Data may already be waiting.
        if (socket->bytesAvailable() > 0)
            QMetaObject::invokeMethod(this, [this, guard = QPointer<QLocalSocket>(socket)] {
                if (guard) onReadyRead(guard.data());
            }, Qt::QueuedConnection);
    }
    if (added)
        emit clientCountChanged(clientCount());
}

void LocalApiServer::onDisconnected(QLocalSocket* socket)
{
    if (clients_.remove(socket))
        emit clientCountChanged(clientCount());
    // Queued lines from this socket are skipped: their QPointer goes null.
    socket->deleteLater();
}

void LocalApiServer::onReadyRead(QLocalSocket* socket)
{
    auto it = clients_.find(socket);
    if (it == clients_.end())
        return;

    if (it->closing) {
        socket->readAll(); // rejected; whatever follows is dropped
        return;
    }
    it->buffer.append(socket->readAll());

    bool tooLong = false;
    for (;;) {
        const qsizetype nl = it->buffer.indexOf('\n');
        if (nl < 0) {
            if (it->buffer.size() > kMaxLineBytes)
                tooLong = true;
            break;
        }
        QByteArray line = it->buffer.left(nl);
        it->buffer.remove(0, nl + 1);
        if (line.size() > kMaxLineBytes) {
            tooLong = true;
            break;
        }
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.trimmed().isEmpty())
            continue;
        if (!it->authenticated) {
            // Authentication touches no project state, so it is answered now
            // rather than queued behind a long tool call or deferred while a
            // dialog is open (the client and our auth timer would give up).
            if (!handleAuthLine(socket, line))
                return; // rejected and closing: drop the rest of the chunk
            it = clients_.find(socket); // sending may have run signal handlers
            if (it == clients_.end())
                return;
            continue;
        }
        queue_.append(PendingLine{QPointer<QLocalSocket>(socket), line});
    }

    if (tooLong) {
        it->buffer.clear();
        rejectAndClose(socket, QJsonValue::Null, CommandGateway::InvalidRequest,
                       QStringLiteral("Message exceeds the 16 MB line limit."));
        return;
    }

    processQueue();
}

void LocalApiServer::processQueue()
{
    // A tool call can pump the event loop (renders, measurements); lines that
    // arrive meanwhile are appended to queue_ and handled by this loop.
    if (processing_ || queue_.isEmpty())
        return;
    processing_ = true;
    while (!queue_.isEmpty()) {
        // Never run agent lines inside a nested UI loop: the dialog's or
        // menu's caller can hold a te::Track* (or a note) the call would free.
        if (isUiBusy()) {
            if (!deferring_) {
                deferring_ = true;
                qDebug() << "[agent-api] UI is busy (dialog or menu open); deferring" << queue_.size()
                         << "agent request(s)";
            }
            scheduleRetry();
            break;
        }
        deferring_ = false;
        PendingLine p = queue_.takeFirst();
        if (!p.socket)
            continue;
        processLine(p.socket.data(), p.line);
    }
    processing_ = false;
}

void LocalApiServer::send(QLocalSocket* socket, const QJsonDocument& doc)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState)
        return;
    QByteArray data = doc.toJson(QJsonDocument::Compact);
    data.append('\n');
    socket->write(data);
    socket->flush();
}

void LocalApiServer::rejectAndClose(QLocalSocket* socket, const QJsonValue& id, int code, const QString& message)
{
    auto it = clients_.find(socket);
    if (it != clients_.end())
        it->closing = true;
    send(socket, QJsonDocument(CommandGateway::makeError(id, code, message)));
    // Waits for the pending write before closing.
    socket->disconnectFromServer();
}

bool LocalApiServer::handleAuthLine(QLocalSocket* socket, const QByteArray& line)
{
    auto it = clients_.find(socket);
    if (it == clients_.end() || it->closing)
        return false;
    if (it->authenticated)
        return true;

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    const bool parsed = parseError.error == QJsonParseError::NoError;
    const QJsonObject msg = doc.object();
    const QJsonValue id = msg.contains("id") ? msg.value("id") : QJsonValue(QJsonValue::Null);
    if (!parsed || !doc.isObject() || msg.value("method").toString() != kAuthMethod) {
        rejectAndClose(socket, id, CommandGateway::Unauthenticated,
                       QStringLiteral("unauthenticated: send opendaw/authenticate with the token from %1 first")
                           .arg(QDir::toNativeSeparators(discoveryFilePath())));
        return false;
    }
    if (!tokenMatches(msg.value("params").toObject().value("token").toString())) {
        qWarning() << "[agent-api] rejected a connection with a wrong token";
        rejectAndClose(socket, id, CommandGateway::Unauthenticated,
                       QStringLiteral("unauthenticated: invalid token"));
        return false;
    }
    // Set before sending: send() may run signal handlers that touch clients_.
    it->authenticated = true;
    send(socket, QJsonDocument(CommandGateway::makeResult(id, QJsonObject{
        {"ok", true}, {"serverName", serverName()}, {"protocolVersion", CommandGateway::protocolVersion()}})));
    return true;
}

void LocalApiServer::processLine(QLocalSocket* socket, const QByteArray& line)
{
    auto it = clients_.find(socket);
    if (it == clients_.end() || it->closing)
        return;

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    const bool parsed = parseError.error == QJsonParseError::NoError;

    // ── Authentication ──────────────────────────────────────────────────────
    // (Normally done in onReadyRead; kept here so no path can skip it.)
    if (!it->authenticated) {
        handleAuthLine(socket, line);
        return;
    }

    if (!parsed) {
        send(socket, QJsonDocument(CommandGateway::makeError(
            QJsonValue::Null, CommandGateway::ParseError,
            QStringLiteral("Parse error: %1").arg(parseError.errorString()))));
        return;
    }

    // Authenticating again on an authenticated connection is harmless.
    if (doc.isObject() && doc.object().value("method").toString() == kAuthMethod) {
        const QJsonObject msg = doc.object();
        if (msg.contains("id"))
            send(socket, QJsonDocument(CommandGateway::makeResult(msg.value("id"), QJsonObject{
                {"ok", true}, {"serverName", serverName()}, {"protocolVersion", CommandGateway::protocolVersion()}})));
        return;
    }

    // The gateway may run a long tool call that pumps events; the client may
    // disconnect (and the socket be deleted) meanwhile.
    QPointer<QLocalSocket> guard(socket);

    if (doc.isObject()) {
        const QJsonObject response = gateway_.handleMessage(doc.object());
        if (guard && !response.isEmpty())
            send(guard.data(), QJsonDocument(response));
        return;
    }

    if (doc.isArray()) {
        const QJsonArray batch = doc.array();
        if (batch.isEmpty()) {
            send(socket, QJsonDocument(CommandGateway::makeError(
                QJsonValue::Null, CommandGateway::InvalidRequest, QStringLiteral("Empty batch."))));
            return;
        }
        const QJsonArray responses = gateway_.handleBatch(batch);
        if (guard && !responses.isEmpty())
            send(guard.data(), QJsonDocument(responses));
        return;
    }

    send(socket, QJsonDocument(CommandGateway::makeError(
        QJsonValue::Null, CommandGateway::InvalidRequest, QStringLiteral("Expected a JSON-RPC object or batch array."))));
}

} // namespace OpenDaw
