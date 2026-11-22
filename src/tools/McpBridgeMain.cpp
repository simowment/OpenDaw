// opendaw-mcp: the MCP stdio server that agents (Claude Code, Codex, ...)
// launch. It relays newline-delimited JSON-RPC between stdin/stdout and the
// running OpenDaw's local agent socket (see src/api/LocalApiServer.h), after
// authenticating with the token from the discovery file.
//
// stdout carries only MCP messages, one per line; every log goes to stderr.
// When OpenDaw is not reachable the bridge still answers initialize, ping and
// tools/list (empty), and tools/call with an isError result explaining how to
// start OpenDaw. It retries the connection on every request, and once
// OpenDaw appears it sends notifications/tools/list_changed so clients reload
// the tool list.
//
// Connecting never blocks the event loop: an attempt (connect, verify the
// server, authenticate) runs asynchronously and is bounded by
// kAttemptTimeoutMs. Requests that arrive meanwhile are held and forwarded
// once connected, or answered offline when the attempt fails (requests other
// than initialize also wait for a --launch in progress, up to 60 s).
//
// Before the token is sent, the bridge checks who serves the socket: on
// Windows the pipe's server process must be the discovery file's pid and run
// as the current user (a pipe name freed by a crash can be re-created by any
// account); on POSIX the peer uid must be ours.
//
// Only QtCore and QtNetwork are used: the bridge links no engine code.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib") // OpenProcessToken, GetTokenInformation, EqualSid
#endif
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/un.h>
#endif
#endif

#ifndef OpenDaw_VERSION
#define OpenDaw_VERSION "dev"
#endif

namespace {

// Keep in sync with LocalApiServer / CommandGateway (the bridge does not link them).
const QString kDefaultServerName = QStringLiteral("opendaw-agent-api");
const QString kProtocolVersion = QStringLiteral("2025-06-18");
const QStringList kSupportedVersions = {QStringLiteral("2025-06-18"), QStringLiteral("2025-03-26"),
                                        QStringLiteral("2024-11-05")};
const QString kAuthId = QStringLiteral("opendaw-mcp-bridge-auth");
constexpr int kLaunchWaitMs = 60000;
// One connection attempt (connect + server check + authenticate). The server
// answers authentication immediately, even during a long tool call.
constexpr int kAttemptTimeoutMs = 5000;
// Retry interval while a --launch is in progress and requests are waiting.
constexpr int kLaunchPollMs = 250;

QString defaultDiscoveryPath()
{
    const QString overridePath = qEnvironmentVariable("OPENDAW_AGENT_API_FILE");
    if (!overridePath.isEmpty())
        return QDir::cleanPath(overridePath);
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir::cleanPath(base + QStringLiteral("/OpenDaw/agent-api.json"));
}

void logMessage(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    const char* level = "info";
    switch (type) {
    case QtDebugMsg:    level = "debug"; break;
    case QtInfoMsg:     level = "info"; break;
    case QtWarningMsg:  level = "warning"; break;
    case QtCriticalMsg: level = "error"; break;
    case QtFatalMsg:    level = "fatal"; break;
    }
    std::fprintf(stderr, "[opendaw-mcp] %s: %s\n", level, msg.toLocal8Bit().constData());
    std::fflush(stderr);
}

void writeStdoutLine(const QByteArray& line)
{
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void writeJson(const QJsonObject& obj)
{
    writeStdoutLine(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void writeJson(const QJsonArray& arr)
{
    writeStdoutLine(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QJsonObject rpcResult(const QJsonValue& id, const QJsonObject& result)
{
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

QJsonObject rpcError(const QJsonValue& id, int code, const QString& message)
{
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
                       {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

QJsonObject toolError(const QString& code, const QString& message)
{
    return QJsonObject{
        {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", message}}}},
        {"structuredContent", QJsonObject{{"error", QJsonObject{{"code", code}, {"message", message}}}}},
        {"isError", true},
    };
}

QString idKey(const QJsonValue& id)
{
    if (id.isString()) return QStringLiteral("s:") + id.toString();
    if (id.isDouble()) return QStringLiteral("n:") + QString::number(id.toDouble(), 'g', 17);
    return QStringLiteral("null");
}

// ── Server identity ─────────────────────────────────────────────────────────

#ifdef _WIN32
struct HandleCloser {
    HANDLE h = nullptr;
    ~HandleCloser() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};

// The TOKEN_USER of a process token, or empty.
QByteArray tokenUser(HANDLE token)
{
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0)
        return {};
    QByteArray buf(int(size), '\0');
    if (!GetTokenInformation(token, TokenUser, buf.data(), size, &size))
        return {};
    return buf;
}

QByteArray processUser(HANDLE process)
{
    HandleCloser token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.h))
        return {};
    return tokenUser(token.h);
}
#endif

// True when the process serving `sock` may be given the token. expectedPid is
// the discovery file's pid (0 when the endpoint did not come from it).
bool verifyServerIdentity(QLocalSocket* sock, qint64 expectedPid, QString* why)
{
#ifdef _WIN32
    const HANDLE pipe = reinterpret_cast<HANDLE>(sock->socketDescriptor());
    ULONG serverPid = 0;
    if (!pipe || pipe == INVALID_HANDLE_VALUE || !GetNamedPipeServerProcessId(pipe, &serverPid)) {
        *why = QStringLiteral("cannot tell which process serves the pipe (error %1).").arg(GetLastError());
        return false;
    }
    if (expectedPid > 0 && qint64(serverPid) != expectedPid) {
        *why = QStringLiteral("the pipe is served by process %1, but the discovery file names OpenDaw pid %2.")
                   .arg(serverPid).arg(expectedPid);
        return false;
    }
    // The pid alone could have been reused; the owner must be us.
    HandleCloser proc;
    proc.h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, serverPid);
    if (!proc.h) {
        *why = QStringLiteral("cannot open the pipe's server process %1 (error %2).").arg(serverPid).arg(GetLastError());
        return false;
    }
    const QByteArray theirs = processUser(proc.h);
    const QByteArray ours = processUser(GetCurrentProcess());
    if (theirs.isEmpty() || ours.isEmpty()) {
        *why = QStringLiteral("cannot read the user of process %1 (error %2).").arg(serverPid).arg(GetLastError());
        return false;
    }
    const auto* a = reinterpret_cast<const TOKEN_USER*>(theirs.constData());
    const auto* b = reinterpret_cast<const TOKEN_USER*>(ours.constData());
    if (!EqualSid(a->User.Sid, b->User.Sid)) {
        *why = QStringLiteral("the pipe's server process %1 belongs to another user.").arg(serverPid);
        return false;
    }
    return true;
#else
    const int fd = int(sock->socketDescriptor());
    if (fd < 0) {
        *why = QStringLiteral("the socket has no descriptor.");
        return false;
    }
    uid_t peerUid = 0;
    qint64 peerPid = 0;
#if defined(__linux__)
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        *why = QStringLiteral("cannot read the socket peer's credentials (errno %1).").arg(errno);
        return false;
    }
    peerUid = cred.uid;
    peerPid = cred.pid;
#else
    gid_t peerGid = 0;
    if (getpeereid(fd, &peerUid, &peerGid) != 0) {
        *why = QStringLiteral("cannot read the socket peer's credentials (errno %1).").arg(errno);
        return false;
    }
#if defined(__APPLE__) && defined(LOCAL_PEERPID)
    pid_t pid = 0;
    socklen_t len = sizeof(pid);
    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) == 0)
        peerPid = pid;
#endif
#endif
    if (peerUid != getuid()) {
        *why = QStringLiteral("the socket is served by another user (uid %1).").arg(qint64(peerUid));
        return false;
    }
    // The uid is what matters on POSIX (the socket file is ours); a pid
    // mismatch is only reported (pid namespaces, sandboxes).
    if (expectedPid > 0 && peerPid > 0 && peerPid != expectedPid)
        qWarning() << "the socket is served by pid" << peerPid << "but the discovery file names pid" << expectedPid;
    return true;
#endif
}

struct Options {
    QString pipe;          // overrides the discovery file's server name
    QString discovery;     // discovery file path
    bool launch = false;   // start OpenDaw when it is not running
    int timeoutSec = 600;  // max wait for one response
};

class Bridge : public QObject {
    struct Pending {
        QJsonValue id;
        QString method;
        qint64 deadlineMs = 0;
    };
    // A stdin line that arrived while not connected.
    struct Held {
        QByteArray line;
        QJsonDocument doc;
        qint64 holdUntilMs = 0;
        bool initializeOnly = false;
    };

public:
    explicit Bridge(Options opt)
        : opt_(std::move(opt))
    {
        clock_.start();
        timeoutTimer_.setInterval(1000);
        connect(&timeoutTimer_, &QTimer::timeout, this, &Bridge::checkTimeouts);
        timeoutTimer_.start();
        retryTimer_.setInterval(2000);
        connect(&retryTimer_, &QTimer::timeout, this, &Bridge::retryInBackground);
        attemptTimer_.setSingleShot(true);
        connect(&attemptTimer_, &QTimer::timeout, this, [this] {
            failAttempt(attemptAuthSent_ ? QStringLiteral("OpenDaw did not answer the authentication request.")
                                         : QStringLiteral("OpenDaw is not reachable on '%1' (timed out).").arg(attemptName_));
        });
        holdTimer_.setInterval(100);
        connect(&holdTimer_, &QTimer::timeout, this, &Bridge::releaseExpiredHeld);
        launchPollTimer_.setSingleShot(true);
        launchPollTimer_.setInterval(kLaunchPollMs);
        connect(&launchPollTimer_, &QTimer::timeout, this, [this] {
            if (!isConnected())
                startAttempt();
        });
    }

    void onStdinLine(QByteArray line)
    {
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.trimmed().isEmpty())
            return;

        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
        if (pe.error != QJsonParseError::NoError || !(doc.isObject() || doc.isArray())) {
            writeJson(rpcError(QJsonValue::Null, -32700,
                               QStringLiteral("Parse error: %1").arg(pe.errorString())));
            return;
        }
        if (doc.isArray() && doc.array().isEmpty()) {
            writeJson(rpcError(QJsonValue::Null, -32600, QStringLiteral("Empty batch.")));
            return;
        }

        if (isConnected() && held_.isEmpty()) {
            forwardLine(line, doc);
            return;
        }

        // Not connected: hold the line while one bounded, asynchronous
        // attempt runs. initialize must answer quickly (clients time out the
        // handshake), so it never waits for a launch to finish.
        Held h;
        h.line = line;
        h.doc = doc;
        h.initializeOnly = doc.isObject() && doc.object().value("method").toString() == QLatin1String("initialize");
        const qint64 now = clock_.elapsed();
        h.holdUntilMs = now + kAttemptTimeoutMs + 1000;
        if (!h.initializeOnly && launchDeadlineMs_ > now)
            h.holdUntilMs = std::max(h.holdUntilMs, launchDeadlineMs_);
        held_.append(h);
        if (!holdTimer_.isActive())
            holdTimer_.start();

        if (isConnected())
            flushHeld(); // (held_ was not empty: keep the order)
        else if (!attempt_)
            startAttempt(); // may fail synchronously and answer offline
    }

    void onStdinClosed()
    {
        stdinClosed_ = true;
        quitIfDone();
    }

private:
    // ── Connection ──────────────────────────────────────────────────────────

    bool isConnected() const
    {
        return socket_ && socket_->state() == QLocalSocket::ConnectedState;
    }

    void forwardLine(const QByteArray& line, const QJsonDocument& doc)
    {
        trackRequests(doc);
        QByteArray data = line;
        data.append('\n');
        if (socket_->write(data) == data.size()) {
            socket_->flush();
            return;
        }
        qWarning() << "write to OpenDaw failed:" << socket_->errorString();
        untrackRequests(doc);
        dropConnection(QStringLiteral("write failed"));
        answerOffline(doc);
    }

    // Starts one asynchronous connection attempt (no-op when one is running).
    void startAttempt()
    {
        if (attempt_ || isConnected())
            return;

        QString name = opt_.pipe;
        QString token;
        qint64 expectedPid = 0;

        QFile f(opt_.discovery);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject info = QJsonDocument::fromJson(f.readAll()).object();
            token = info.value("token").toString();
            if (name.isEmpty()) {
#ifdef Q_OS_WIN
                name = info.value("serverName").toString();
#else
                // The absolute socket path does not depend on our TMPDIR.
                name = info.value("fullServerName").toString();
                if (name.isEmpty()) name = info.value("serverName").toString();
#endif
                // Only an endpoint taken from the file is bound to its pid.
                expectedPid = info.value("pid").toInteger();
            }
        } else if (name.isEmpty()) {
            attemptFailed(QStringLiteral("OpenDaw is not running (no discovery file at %1).")
                              .arg(QDir::toNativeSeparators(opt_.discovery)));
            return;
        }
        if (name.isEmpty())
            name = kDefaultServerName;

        auto* sock = new QLocalSocket(this);
        attempt_ = sock;
        attemptName_ = name;
        attemptToken_ = token;
        attemptPid_ = expectedPid;
        attemptBuf_.clear();
        attemptAuthSent_ = false;

        connect(sock, &QLocalSocket::connected, this, [this, sock] {
            if (sock == attempt_) onAttemptConnected();
        });
        connect(sock, &QLocalSocket::readyRead, this, [this, sock] {
            if (sock == attempt_) onAttemptReadyRead();
        });
        connect(sock, &QLocalSocket::errorOccurred, this, [this, sock](QLocalSocket::LocalSocketError) {
            if (sock != attempt_)
                return;
            failAttempt(attemptAuthSent_
                ? QStringLiteral("OpenDaw closed the connection during authentication (%1).").arg(sock->errorString())
                : QStringLiteral("OpenDaw is not reachable on '%1' (%2).").arg(attemptName_, sock->errorString()));
        });
        connect(sock, &QLocalSocket::disconnected, this, [this, sock] {
            if (sock == attempt_)
                failAttempt(QStringLiteral("OpenDaw closed the connection during authentication."));
        });

        attemptTimer_.start(kAttemptTimeoutMs);
        // Asynchronous on POSIX. On Windows it opens the pipe at once (and
        // waits at most a few seconds when every pipe instance is busy).
        // The handlers above may run (and end the attempt) inside this call,
        // so nothing touches `sock` after it.
        sock->connectToServer(name);
    }

    void onAttemptConnected()
    {
        QString why;
        if (!verifyServerIdentity(attempt_.data(), attemptPid_, &why)) {
            failAttempt(QStringLiteral("Refusing to send the token: %1 The discovery file %2 is probably stale "
                                       "(did OpenDaw crash?). Start OpenDaw again.")
                            .arg(why, QDir::toNativeSeparators(opt_.discovery)));
            return;
        }
        const QJsonObject auth{{"jsonrpc", "2.0"}, {"id", kAuthId}, {"method", "opendaw/authenticate"},
                               {"params", QJsonObject{{"token", attemptToken_}}}};
        QByteArray authLine = QJsonDocument(auth).toJson(QJsonDocument::Compact);
        authLine.append('\n');
        attemptAuthSent_ = true;
        attempt_->write(authLine);
        attempt_->flush();
    }

    void onAttemptReadyRead()
    {
        attemptBuf_.append(attempt_->readAll());
        const qsizetype nl = attemptBuf_.indexOf('\n');
        if (nl < 0)
            return;
        if (!attemptAuthSent_) {
            failAttempt(QStringLiteral("OpenDaw sent data before authentication."));
            return;
        }
        const QJsonObject reply = QJsonDocument::fromJson(attemptBuf_.left(nl)).object();
        if (!reply.value("result").toObject().value("ok").toBool()) {
            const QString why = reply.value("error").toObject().value("message").toString();
            failAttempt(QStringLiteral("OpenDaw refused the connection: %1. Restart opendaw-mcp after "
                                       "OpenDaw has started, or check %2.")
                            .arg(why.isEmpty() ? QStringLiteral("unknown error") : why,
                                 QDir::toNativeSeparators(opt_.discovery)));
            return;
        }
        attemptSucceeded(attemptBuf_.mid(nl + 1));
    }

    void attemptSucceeded(const QByteArray& leftover)
    {
        QLocalSocket* sock = attempt_.data();
        attempt_ = nullptr;
        attemptTimer_.stop();
        sock->disconnect(this);

        socket_ = sock;
        readBuf_ = leftover;
        connect(socket_, &QLocalSocket::readyRead, this, &Bridge::onSocketReadyRead);
        connect(socket_, &QLocalSocket::disconnected, this, [this] { dropConnection(QStringLiteral("OpenDaw closed the connection")); });
        connect(socket_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
            if (socket_ && socket_->state() != QLocalSocket::ConnectedState)
                dropConnection(socket_->errorString());
        });
        qInfo() << "connected to OpenDaw on" << attemptName_;
        lastError_.clear();
        launchDeadlineMs_ = -1;
        launchPollTimer_.stop();
        retryTimer_.stop();

        if (servedOfflineTools_) {
            servedOfflineTools_ = false;
            writeJson(QJsonObject{{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}});
        }
        flushHeld();
        if (!readBuf_.isEmpty())
            QTimer::singleShot(0, this, &Bridge::onSocketReadyRead);
    }

    void failAttempt(const QString& reason)
    {
        if (QLocalSocket* sock = attempt_.data()) {
            attempt_ = nullptr;
            sock->disconnect(this);
            sock->abort();
            sock->deleteLater(); // we may be inside one of its signals
        }
        attemptTimer_.stop();
        attemptFailed(reason);
    }

    void attemptFailed(const QString& reason)
    {
        lastError_ = reason;
        if (opt_.launch && !held_.isEmpty())
            maybeLaunch(); // extends the holds of waiting requests
        const qint64 now = clock_.elapsed();
        const bool launching = launchDeadlineMs_ > now;
        // Answer what cannot wait: everything unless a launch is in progress;
        // during a launch, initialize and requests whose hold expired.
        QList<Held> keep;
        const QList<Held> all = std::exchange(held_, {});
        for (const Held& h : all) {
            if (launching && !h.initializeOnly && h.holdUntilMs > now)
                keep.append(h);
            else
                answerOffline(h.doc);
        }
        held_ = keep + held_; // (answering cannot add lines, but keep order anyway)
        if (held_.isEmpty())
            holdTimer_.stop();
        if (launching && !held_.isEmpty() && !launchPollTimer_.isActive())
            launchPollTimer_.start();
        if (!isConnected() && (opt_.launch || servedOfflineTools_) && !retryTimer_.isActive())
            retryTimer_.start();
        quitIfDone();
    }

    // Forwards (or, if the connection broke, answers offline) every held line.
    void flushHeld()
    {
        while (!held_.isEmpty()) {
            const Held h = held_.takeFirst();
            if (isConnected())
                forwardLine(h.line, h.doc);
            else
                answerOffline(h.doc);
        }
        holdTimer_.stop();
        quitIfDone();
    }

    void releaseExpiredHeld()
    {
        const qint64 now = clock_.elapsed();
        bool answered = false;
        while (!held_.isEmpty() && held_.first().holdUntilMs <= now) {
            const Held h = held_.takeFirst();
            if (!h.initializeOnly && lastLaunchMs_ >= 0 && launchDeadlineMs_ >= 0 && launchDeadlineMs_ <= now)
                lastError_ = QStringLiteral("OpenDaw did not become reachable within %1 s.").arg(kLaunchWaitMs / 1000);
            answerOffline(h.doc);
            answered = true;
        }
        if (held_.isEmpty())
            holdTimer_.stop();
        if (answered)
            quitIfDone();
    }

    void dropConnection(const QString& reason)
    {
        if (!socket_)
            return;
        qWarning() << "disconnected from OpenDaw:" << reason;
        QLocalSocket* s = socket_;
        socket_ = nullptr;
        s->disconnect(this);
        s->abort();
        s->deleteLater();
        readBuf_.clear();
        failPending(QStringLiteral("The connection to OpenDaw was lost (%1). Retry the call; if OpenDaw "
                                   "was closed, start it again.").arg(reason));
    }

    void retryInBackground()
    {
        if (isConnected()) {
            retryTimer_.stop();
            return;
        }
        if (!servedOfflineTools_ && launchDeadlineMs_ < clock_.elapsed()) {
            retryTimer_.stop();
            return;
        }
        startAttempt(); // asynchronous: never stalls stdin handling
    }

    QString openDawExecutable() const
    {
        const QString overridePath = qEnvironmentVariable("OPENDAW_EXE");
        if (!overridePath.isEmpty())
            return overridePath;
        const QDir dir(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
        return dir.filePath(QStringLiteral("OpenDaw.exe"));
#else
        return dir.filePath(QStringLiteral("OpenDaw"));
#endif
    }

    void maybeLaunch()
    {
        const qint64 now = clock_.elapsed();
        if (launchDeadlineMs_ > now)
            return; // a launch is in progress
        if (lastLaunchMs_ >= 0 && now - lastLaunchMs_ < kLaunchWaitMs + 30000)
            return; // launched recently and it did not come up; do not spawn copies

        const QString exe = openDawExecutable();
        if (!QFileInfo::exists(exe)) {
            lastError_ = QStringLiteral("OpenDaw is not running and %1 was not found to launch it.").arg(exe);
            qWarning().noquote() << lastError_;
            lastLaunchMs_ = now;
            return;
        }

#ifdef _WIN32
        // Keep the MCP client's stdio pipes out of the DAW process, so the
        // client sees EOF when the bridge exits.
        for (DWORD which : {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
            HANDLE h = GetStdHandle(which);
            if (h && h != INVALID_HANDLE_VALUE)
                SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
        }
#endif
        QProcess proc;
        proc.setProgram(exe);
        proc.setWorkingDirectory(QFileInfo(exe).absolutePath());
        proc.setStandardInputFile(QProcess::nullDevice());
        proc.setStandardOutputFile(QProcess::nullDevice());
        proc.setStandardErrorFile(QProcess::nullDevice());
        qint64 pid = 0;
        if (!proc.startDetached(&pid)) {
            lastError_ = QStringLiteral("Could not launch %1: %2").arg(exe, proc.errorString());
            qWarning().noquote() << lastError_;
            lastLaunchMs_ = now;
            return;
        }
        qInfo() << "launched" << exe << "pid" << pid << "- waiting for it to start...";
        lastLaunchMs_ = now;
        launchDeadlineMs_ = now + kLaunchWaitMs;
        lastError_ = QStringLiteral("OpenDaw is starting; retry in a few seconds.");
        // Waiting requests (except initialize) wait for the launch.
        for (Held& h : held_)
            if (!h.initializeOnly)
                h.holdUntilMs = std::max(h.holdUntilMs, launchDeadlineMs_);
    }

    // ── Server -> client ───────────────────────────────────────────────────

    void onSocketReadyRead()
    {
        if (!socket_)
            return;
        readBuf_.append(socket_->readAll());
        for (;;) {
            const qsizetype nl = readBuf_.indexOf('\n');
            if (nl < 0) break;
            QByteArray line = readBuf_.left(nl);
            readBuf_.remove(0, nl + 1);
            if (line.endsWith('\r')) line.chop(1);
            if (!line.trimmed().isEmpty())
                handleServerLine(line);
        }
        quitIfDone();
    }

    void handleServerLine(const QByteArray& line)
    {
        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
        if (pe.error != QJsonParseError::NoError) {
            qWarning() << "dropping an unparseable line from OpenDaw:" << pe.errorString();
            return;
        }
        if (doc.isObject()) {
            QJsonObject msg = doc.object();
            if (msg.contains("id") && !msg.contains("method")) {
                if (msg.value("id").toString() == kAuthId)
                    return;
                if (msg.value("id").isNull()) {
                    writeJson(msg); // an error for a request OpenDaw could not read
                    return;
                }
                const auto it = pending_.find(idKey(msg.value("id")));
                if (it == pending_.end()) {
                    qWarning() << "dropping a response for an unknown or timed-out request";
                    return;
                }
                const QString method = it->method;
                pending_.erase(it);
                if (method == QLatin1String("initialize"))
                    msg = patchInitialize(msg);
                writeJson(msg);
                return;
            }
            writeJson(msg); // a notification from OpenDaw
            return;
        }
        if (doc.isArray()) {
            QJsonArray out;
            for (const auto& v : doc.array()) {
                QJsonObject msg = v.toObject();
                const auto it = pending_.find(idKey(msg.value("id")));
                if (it != pending_.end()) {
                    if (it->method == QLatin1String("initialize"))
                        msg = patchInitialize(msg);
                    pending_.erase(it);
                }
                out.append(msg);
            }
            writeJson(out);
        }
    }

    // The bridge can announce tool list changes (when OpenDaw comes up after
    // an empty offline list), so advertise listChanged.
    static QJsonObject patchInitialize(QJsonObject msg)
    {
        QJsonObject result = msg.value("result").toObject();
        if (result.isEmpty())
            return msg;
        QJsonObject caps = result.value("capabilities").toObject();
        QJsonObject tools = caps.value("tools").toObject();
        tools["listChanged"] = true;
        caps["tools"] = tools;
        result["capabilities"] = caps;
        msg["result"] = result;
        return msg;
    }

    // ── Request bookkeeping ────────────────────────────────────────────────

    void trackOne(const QJsonObject& msg)
    {
        if (!msg.contains("id") || !msg.value("method").isString())
            return;
        Pending p;
        p.id = msg.value("id");
        p.method = msg.value("method").toString();
        p.deadlineMs = clock_.elapsed() + qint64(opt_.timeoutSec) * 1000;
        pending_.insert(idKey(p.id), p);
    }

    void trackRequests(const QJsonDocument& doc)
    {
        if (doc.isObject()) trackOne(doc.object());
        else for (const auto& v : doc.array()) trackOne(v.toObject());
    }

    void untrackRequests(const QJsonDocument& doc)
    {
        auto drop = [this](const QJsonObject& m) { if (m.contains("id")) pending_.remove(idKey(m.value("id"))); };
        if (doc.isObject()) drop(doc.object());
        else for (const auto& v : doc.array()) drop(v.toObject());
    }

    void answerFailed(const Pending& p, const QString& message)
    {
        if (p.method == QLatin1String("tools/call"))
            writeJson(rpcResult(p.id, toolError(QStringLiteral("UNAVAILABLE"), message)));
        else
            writeJson(rpcError(p.id, -32000, message));
    }

    void failPending(const QString& message)
    {
        const auto all = pending_.values();
        pending_.clear();
        for (const auto& p : all)
            answerFailed(p, message);
        quitIfDone();
    }

    void checkTimeouts()
    {
        const qint64 now = clock_.elapsed();
        QList<Pending> expired;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->deadlineMs <= now) {
                expired.append(*it);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& p : expired)
            answerFailed(p, QStringLiteral("OpenDaw did not answer within %1 s (it may still be working, "
                                           "e.g. on a long render).").arg(opt_.timeoutSec));
        if (!expired.isEmpty())
            quitIfDone();
    }

    void quitIfDone()
    {
        if (stdinClosed_ && pending_.isEmpty() && held_.isEmpty())
            QCoreApplication::quit();
    }

    // ── Offline answers ────────────────────────────────────────────────────

    QString offlineReason() const
    {
        QString why = lastError_.isEmpty() ? QStringLiteral("OpenDaw is not running.") : lastError_;
        if (!opt_.launch)
            why += QStringLiteral(" Start OpenDaw (or configure opendaw-mcp with --launch so it starts OpenDaw "
                                  "itself) and call the tool again.");
        return why;
    }

    void answerOffline(const QJsonDocument& doc)
    {
        if (doc.isObject()) {
            const QJsonObject resp = offlineResponse(doc.object());
            if (!resp.isEmpty()) writeJson(resp);
            return;
        }
        QJsonArray out;
        for (const auto& v : doc.array()) {
            const QJsonObject resp = offlineResponse(v.toObject());
            if (!resp.isEmpty()) out.append(resp);
        }
        if (!out.isEmpty()) writeJson(out);
    }

    QJsonObject offlineResponse(const QJsonObject& msg)
    {
        if (!msg.contains("id"))
            return {}; // notification
        const QJsonValue id = msg.value("id");
        const QString method = msg.value("method").toString();

        if (method == QLatin1String("initialize")) {
            const QString requested = msg.value("params").toObject().value("protocolVersion").toString();
            QJsonObject result;
            result["protocolVersion"] = kSupportedVersions.contains(requested) ? requested : kProtocolVersion;
            result["capabilities"] = QJsonObject{{"tools", QJsonObject{{"listChanged", true}}}};
            result["serverInfo"] = QJsonObject{{"name", "opendaw"}, {"title", "OpenDaw"},
                                               {"version", QString::fromUtf8(OpenDaw_VERSION)}};
            result["instructions"] = QStringLiteral(
                "OpenDaw (a digital audio workstation) is not reachable right now: %1 Its tools appear "
                "once it is running (a tools/list_changed notification is sent). When available: use the "
                "ids from get_track_list (\"track:<n>\", \"clip:<n>\", \"plugin:<n>\", \"pattern:<n>\"), "
                "run_batch for multi-step atomic edits, and render_region plus the analysis tools to "
                "listen to the result.").arg(offlineReason());
            return rpcResult(id, result);
        }
        if (method == QLatin1String("ping"))
            return rpcResult(id, QJsonObject{});
        if (method == QLatin1String("tools/list")) {
            servedOfflineTools_ = true;
            if (!retryTimer_.isActive())
                retryTimer_.start();
            return rpcResult(id, QJsonObject{{"tools", QJsonArray{}}});
        }
        if (method == QLatin1String("tools/call"))
            return rpcResult(id, toolError(QStringLiteral("UNAVAILABLE"), offlineReason()));
        if (method == QLatin1String("resources/list"))
            return rpcResult(id, QJsonObject{{"resources", QJsonArray{}}});
        if (method == QLatin1String("resources/templates/list"))
            return rpcResult(id, QJsonObject{{"resourceTemplates", QJsonArray{}}});
        if (method == QLatin1String("prompts/list"))
            return rpcResult(id, QJsonObject{{"prompts", QJsonArray{}}});
        if (method == QLatin1String("logging/setLevel"))
            return rpcResult(id, QJsonObject{});
        if (method.isEmpty())
            return rpcError(id, -32600, QStringLiteral("Missing \"method\"."));
        return rpcError(id, -32601, QStringLiteral("Method not found: %1").arg(method));
    }

    Options opt_;
    QPointer<QLocalSocket> socket_;
    QByteArray readBuf_;
    QHash<QString, Pending> pending_;
    QElapsedTimer clock_;
    QTimer timeoutTimer_;
    QTimer retryTimer_;
    // The connection attempt in progress (connect, verify, authenticate).
    QPointer<QLocalSocket> attempt_;
    QString attemptName_;
    QString attemptToken_;
    qint64 attemptPid_ = 0;
    QByteArray attemptBuf_;
    bool attemptAuthSent_ = false;
    QTimer attemptTimer_;
    QTimer launchPollTimer_;
    QList<Held> held_;
    QTimer holdTimer_;
    QString lastError_;
    qint64 launchDeadlineMs_ = -1;
    qint64 lastLaunchMs_ = -1;
    bool servedOfflineTools_ = false;
    bool stdinClosed_ = false;
};

} // namespace

int main(int argc, char* argv[])
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    qInstallMessageHandler(logMessage);

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("opendaw-mcp"));
    QCoreApplication::setApplicationVersion(QString::fromUtf8(OpenDaw_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "MCP stdio server for OpenDaw. Relays MCP JSON-RPC between stdin/stdout and a running OpenDaw."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption pipeOpt(QStringLiteral("pipe"),
        QStringLiteral("Local server name (or socket path) to connect to, instead of the one in the discovery file."),
        QStringLiteral("name"));
    const QCommandLineOption discoveryOpt(QStringLiteral("discovery"),
        QStringLiteral("Path of the discovery file written by OpenDaw."), QStringLiteral("path"),
        defaultDiscoveryPath());
    const QCommandLineOption launchOpt(QStringLiteral("launch"),
        QStringLiteral("Start OpenDaw (found next to this program, or $OPENDAW_EXE) when it is not running, "
                       "and wait up to 60 s for it."));
    const QCommandLineOption timeoutOpt(QStringLiteral("timeout"),
        QStringLiteral("Seconds to wait for OpenDaw to answer one request (default 600)."),
        QStringLiteral("seconds"), QStringLiteral("600"));
    parser.addOption(pipeOpt);
    parser.addOption(discoveryOpt);
    parser.addOption(launchOpt);
    parser.addOption(timeoutOpt);
    parser.process(app);

    Options opt;
    opt.pipe = parser.value(pipeOpt);
    opt.discovery = parser.value(discoveryOpt);
    opt.launch = parser.isSet(launchOpt);
    bool ok = false;
    const int timeout = parser.value(timeoutOpt).toInt(&ok);
    opt.timeoutSec = (ok && timeout > 0) ? timeout : 600;

    Bridge bridge(opt);
    QPointer<Bridge> target(&bridge);

    // stdin is read on its own thread (blocking reads) and each line is
    // posted to the main thread in order.
    std::thread reader([target] {
        std::string line;
        while (std::getline(std::cin, line)) {
            const QByteArray bytes = QByteArray::fromStdString(line);
            QMetaObject::invokeMethod(QCoreApplication::instance(), [target, bytes] {
                if (target) target->onStdinLine(bytes);
            }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [target] {
            if (target) target->onStdinClosed();
        }, Qt::QueuedConnection);
    });

    const int rc = app.exec();

    // The reader has normally finished (stdin closed). If not, it is blocked
    // in a read that cannot be interrupted portably; let process exit end it.
    if (reader.joinable())
        reader.detach();
    std::fflush(stdout);
    return rc;
}
