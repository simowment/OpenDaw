// LocalApiServer / opendaw-mcp fixes:
//  - agent lines are never executed inside a nested event loop (a modal
//    dialog or a menu): they stay queued and run once the loop exits;
//  - authentication is answered immediately, even while other lines wait;
//  - the bridge checks who serves the pipe before it sends the token, and
//    never blocks its stdin handling on a connection attempt.
//
// The server lives in this thread, so every wait pumps the event loop. The
// test functions run outside any QEventLoop (loop level 0); the tests that
// need the app's situation run an outer QEventLoop (level 1, like
// QApplication::exec) and open a nested one inside it (level 2, like
// QDialog::exec).

#include "TestHarness.h"
#include "engine/EditManager.h"

#include "api/CommandGateway.h"
#include "api/LocalApiServer.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QThread>
#include <QTimer>
#include <QtTest>
#include <optional>

using namespace OpenDaw;
using OpenDaw::Test::harness;

namespace {

QByteArray line(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

QJsonObject request(qint64 id, const QString& method, const QJsonObject& params = {})
{
    QJsonObject msg{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.isEmpty())
        msg["params"] = params;
    return msg;
}

QJsonObject toolCall(qint64 id, const QString& tool, const QJsonObject& args = {})
{
    return request(id, QStringLiteral("tools/call"), QJsonObject{{"name", tool}, {"arguments", args}});
}

// Splits whatever a device has delivered into JSON lines.
class Lines {
public:
    void feed(const QByteArray& data)
    {
        buffer_ += data;
        for (;;) {
            const qsizetype nl = buffer_.indexOf('\n');
            if (nl < 0) break;
            const QByteArray l = buffer_.left(nl).trimmed();
            buffer_.remove(0, nl + 1);
            if (!l.isEmpty())
                docs_.append(QJsonDocument::fromJson(l));
        }
    }
    std::optional<QJsonObject> withId(qint64 id) const
    {
        for (const auto& d : docs_)
            if (d.isObject() && d.object().value("id").isDouble() && d.object().value("id").toInteger() == id)
                return d.object();
        return std::nullopt;
    }
    std::optional<QJsonObject> withId(const QString& id) const
    {
        for (const auto& d : docs_)
            if (d.isObject() && d.object().value("id").toString() == id)
                return d.object();
        return std::nullopt;
    }
    int count() const { return int(docs_.size()); }

private:
    QByteArray buffer_;
    QList<QJsonDocument> docs_;
};

// Pumps events (at the caller's loop level) until `id` arrives on `dev`.
std::optional<QJsonObject> waitForId(QIODevice& dev, Lines& lines, qint64 id, int timeoutMs = 15000)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        lines.feed(dev.readAll());
        if (auto r = lines.withId(id))
            return r;
        QTest::qWait(10);
    }
    lines.feed(dev.readAll());
    return lines.withId(id);
}

// An authenticated socket to the harness server, or nullptr.
std::unique_ptr<QLocalSocket> connectAuthenticated(LocalApiServer& server)
{
    auto sock = std::make_unique<QLocalSocket>();
    sock->connectToServer(harness().serverName());
    QElapsedTimer t;
    t.start();
    while (sock->state() != QLocalSocket::ConnectedState && t.elapsed() < 5000)
        QTest::qWait(10);
    if (sock->state() != QLocalSocket::ConnectedState)
        return nullptr;
    sock->write(line(request(0, QStringLiteral("opendaw/authenticate"), QJsonObject{{"token", server.token()}})));
    sock->flush();
    Lines lines;
    const auto auth = waitForId(*sock, lines, 0, 5000);
    if (!auth || !auth->value("result").toObject().value("ok").toBool())
        return nullptr;
    return sock;
}

QString bridgeExecutable()
{
    const QString env = qEnvironmentVariable("OPENDAW_MCP_BRIDGE");
    if (!env.isEmpty())
        return QFileInfo(env).isExecutable() ? env : QString();
#ifdef Q_OS_WIN
    const QString exe = QStringLiteral("opendaw-mcp.exe");
#else
    const QString exe = QStringLiteral("opendaw-mcp");
#endif
#ifdef OPENDAW_TOOLS_DIR
    if (const QFileInfo built(QDir(QString::fromUtf8(OPENDAW_TOOLS_DIR)).filePath(exe)); built.isExecutable())
        return built.absoluteFilePath();
#endif
    const QDir here(QCoreApplication::applicationDirPath());
    for (const QString& c : {here.filePath(exe), here.filePath(QStringLiteral("OpenDaw_artefacts/Debug/") + exe),
                             here.filePath(QStringLiteral("OpenDaw_artefacts/Release/") + exe),
                             here.filePath(QStringLiteral("OpenDaw_artefacts/") + exe)})
        if (QFileInfo(c).isExecutable())
            return QFileInfo(c).absoluteFilePath();
    return {};
}

bool writeDiscovery(const QString& path, const QJsonObject& info)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(info).toJson());
    return true;
}

QJsonObject harnessDiscovery()
{
    QFile f(harness().discoveryFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

QString toolText(const QJsonObject& response)
{
    QString text;
    for (const auto& c : response.value("result").toObject().value("content").toArray())
        text += c.toObject().value("text").toString();
    return text;
}

} // namespace

class TestGatewayFixes : public QObject {
    Q_OBJECT

private slots:
    void init() { harness().resetProject(); }

    // ── Finding 5: nested event loops ────────────────────────────────────────

    void notBusyAtTopLevel()
    {
        auto* server = harness().server();
        if (!server)
            QSKIP("LocalApiServer did not start");
        QVERIFY(!server->isUiBusy());

        bool busyInOuter = true;
        bool busyInNested = false;
        QEventLoop outer;
        QTimer::singleShot(0, &outer, [&] {
            busyInOuter = server->isUiBusy(); // level 1: the main loop
            QEventLoop nested;
            QTimer::singleShot(0, &nested, [&] { busyInNested = server->isUiBusy(); nested.quit(); });
            nested.exec();
            outer.quit();
        });
        outer.exec();
        QVERIFY2(!busyInOuter, "the main loop itself must not count as busy");
        QVERIFY2(busyInNested, "a nested event loop must count as busy");

        // The installable predicate.
        bool flag = true;
        server->setUiBusyPredicate([&flag] { return flag; });
        QVERIFY(server->isUiBusy());
        flag = false;
        QVERIFY(!server->isUiBusy());
        server->setUiBusyPredicate({});
    }

    void toolCallDeferredDuringNestedLoop()
    {
        auto* server = harness().server();
        if (!server)
            QSKIP("LocalApiServer did not start");
        auto sock = connectAuthenticated(*server);
        QVERIFY2(sock, "could not authenticate");

        const int before = harness().editManager().getAudioTracks().size();
        int tracksInside = -1;
        int queuedInside = -1;
        bool answeredInside = true;
        int tracksAfter = -1;
        Lines lines;

        QElapsedTimer waited;
        QEventLoop outer; // stands in for QApplication::exec (loop level 1)
        QTimer::singleShot(0, &outer, [&] {
            // Like QDialog::exec: a nested loop while an agent call arrives.
            QEventLoop nested;
            sock->write(line(toolCall(10, QStringLiteral("create_midi_track"),
                                      QJsonObject{{"name", "Deferred"}})));
            sock->flush();
            QTimer::singleShot(700, &nested, [&] {
                tracksInside = harness().editManager().getAudioTracks().size();
                queuedInside = server->queuedLineCount();
                lines.feed(sock->readAll());
                answeredInside = lines.withId(10).has_value();
                nested.quit();
            });
            nested.exec();

            // Back in the main loop: the retry timer runs the call.
            auto* poll = new QTimer(&outer);
            waited.start();
            QObject::connect(poll, &QTimer::timeout, &outer, [&, poll] {
                lines.feed(sock->readAll());
                if (lines.withId(10) || waited.elapsed() > 10000) {
                    poll->stop();
                    tracksAfter = harness().editManager().getAudioTracks().size();
                    outer.quit();
                }
            });
            poll->start(20);
        });
        outer.exec();

        QCOMPARE(tracksInside, before);   // nothing ran inside the nested loop
        QVERIFY2(queuedInside >= 1, "the call should have been waiting in the queue");
        QVERIFY2(!answeredInside, "the call was answered inside the nested loop");

        const auto resp = lines.withId(10);
        QVERIFY2(resp.has_value(), "the deferred call was never run");
        QCOMPARE(resp->value("result").toObject().value("isError").toBool(), false);
        QCOMPARE(tracksAfter, before + 1);
        QCOMPARE(server->queuedLineCount(), 0);
        sock->disconnectFromServer();
    }

    // ── Finding 6: authentication is not queued ──────────────────────────────

    void authenticationAnsweredWhileCallsWait()
    {
        auto* server = harness().server();
        if (!server)
            QSKIP("LocalApiServer did not start");
        auto a = connectAuthenticated(*server);
        QVERIFY2(a, "could not authenticate client A");

        const int before = harness().editManager().getAudioTracks().size();
        QLocalSocket b;
        Lines aLines;
        Lines bLines;
        bool bAuthInside = false;
        bool bPingInside = true;
        bool aAnsweredInside = true;
        int tracksInside = -1;
        QElapsedTimer waited;

        QEventLoop outer;
        QTimer::singleShot(0, &outer, [&] {
            QEventLoop nested;
            // A's mutating call waits (the UI is busy)...
            a->write(line(toolCall(20, QStringLiteral("create_midi_track"), QJsonObject{{"name", "Queued"}})));
            a->flush();
            // ...while B connects and authenticates; its ping (same chunk as
            // the token) must wait like any other request.
            b.connectToServer(harness().serverName());
            QTimer::singleShot(150, &nested, [&] {
                b.write(line(request(0, QStringLiteral("opendaw/authenticate"),
                                     QJsonObject{{"token", server->token()}}))
                        + line(request(21, QStringLiteral("ping"))));
                b.flush();
            });
            QTimer::singleShot(900, &nested, [&] {
                bLines.feed(b.readAll());
                aLines.feed(a->readAll());
                const auto auth = bLines.withId(0);
                bAuthInside = auth && auth->value("result").toObject().value("ok").toBool();
                bPingInside = bLines.withId(21).has_value();
                aAnsweredInside = aLines.withId(20).has_value();
                tracksInside = harness().editManager().getAudioTracks().size();
                nested.quit();
            });
            nested.exec();

            auto* poll = new QTimer(&outer);
            waited.start();
            QObject::connect(poll, &QTimer::timeout, &outer, [&, poll] {
                aLines.feed(a->readAll());
                bLines.feed(b.readAll());
                if ((aLines.withId(20) && bLines.withId(21)) || waited.elapsed() > 10000) {
                    poll->stop();
                    outer.quit();
                }
            });
            poll->start(20);
        });
        outer.exec();

        QVERIFY2(bAuthInside, "authentication was queued behind the waiting call");
        QVERIFY2(!bPingInside, "a request after the token ran inside the nested loop");
        QVERIFY2(!aAnsweredInside, "A's call ran inside the nested loop");
        QCOMPARE(tracksInside, before);
        QVERIFY2(aLines.withId(20).has_value(), "A's call never ran");
        QVERIFY2(bLines.withId(21).has_value(), "B's ping never ran");
        QCOMPARE(harness().editManager().getAudioTracks().size(), before + 1);
        a->disconnectFromServer();
        b.disconnectFromServer();
    }

    void wrongTokenRejectedAtOnceAndRestDropped()
    {
        auto* server = harness().server();
        if (!server)
            QSKIP("LocalApiServer did not start");
        const int before = harness().editManager().getAudioTracks().size();
        QLocalSocket sock;
        sock.connectToServer(harness().serverName());
        QTRY_COMPARE_WITH_TIMEOUT(sock.state(), QLocalSocket::ConnectedState, 5000);
        sock.write(line(request(0, QStringLiteral("opendaw/authenticate"), QJsonObject{{"token", "nope"}}))
                   + line(toolCall(1, QStringLiteral("create_midi_track"))));
        sock.flush();
        Lines lines;
        const auto rej = waitForId(sock, lines, 0, 5000);
        QVERIFY(rej.has_value());
        QCOMPARE(rej->value("error").toObject().value("code").toInt(), int(CommandGateway::Unauthenticated));
        QTRY_COMPARE_WITH_TIMEOUT(sock.state(), QLocalSocket::UnconnectedState, 5000);
        QTest::qWait(100);
        QCOMPARE(harness().editManager().getAudioTracks().size(), before);
        QVERIFY(!lines.withId(1).has_value());
    }

    // ── Finding 8 + non-blocking bridge ──────────────────────────────────────

    void bridgeConnectsThroughDiscoveryFile()
    {
        // No --pipe: the endpoint and pid come from the discovery file, so
        // the server identity check (pid + user) runs against the real server.
        if (!harness().server())
            QSKIP("LocalApiServer did not start");
        const QString exe = bridgeExecutable();
        if (exe.isEmpty())
            QSKIP("opendaw-mcp not found (set OPENDAW_MCP_BRIDGE)");

        QProcess proc;
        proc.setProgram(exe);
        proc.setArguments({QStringLiteral("--discovery"), harness().discoveryFilePath()});
        proc.start();
        QVERIFY2(proc.waitForStarted(10000), qPrintable(proc.errorString()));
        Lines lines;
        proc.write(line(toolCall(1, QStringLiteral("get_project_info"))));
        const auto resp = waitForId(proc, lines, 1, 20000);
        QVERIFY2(resp.has_value(), proc.readAllStandardError().constData());
        QVERIFY2(!resp->value("result").toObject().value("isError").toBool(),
                 qPrintable(toolText(*resp) + " / " + QString::fromLocal8Bit(proc.readAllStandardError())));
        proc.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(proc.state() == QProcess::NotRunning, 10000);
    }

    void bridgeRefusesStaleDiscoveryPid()
    {
#ifndef Q_OS_WIN
        QSKIP("the pid check is enforced on Windows (POSIX checks the peer uid)");
#else
        auto* server = harness().server();
        if (!server)
            QSKIP("LocalApiServer did not start");
        const QString exe = bridgeExecutable();
        if (exe.isEmpty())
            QSKIP("opendaw-mcp not found (set OPENDAW_MCP_BRIDGE)");

        // A discovery file left by a "crashed" OpenDaw whose pipe name is now
        // served by another process (here: this test process, but with a pid
        // that does not match the file).
        QJsonObject info = harnessDiscovery();
        QVERIFY(!info.isEmpty());
        info["pid"] = qint64(QCoreApplication::applicationPid()) + 4;
        const QString path = QDir(harness().makeTempSubdir(QStringLiteral("stale"))).filePath("agent-api.json");
        QVERIFY(writeDiscovery(path, info));

        const int clientsBefore = server->clientCount();
        QProcess proc;
        proc.setProgram(exe);
        proc.setArguments({QStringLiteral("--discovery"), path});
        proc.start();
        QVERIFY2(proc.waitForStarted(10000), qPrintable(proc.errorString()));
        Lines lines;
        proc.write(line(toolCall(1, QStringLiteral("get_project_info"))));
        const auto resp = waitForId(proc, lines, 1, 20000);
        QVERIFY2(resp.has_value(), proc.readAllStandardError().constData());
        QVERIFY(resp->value("result").toObject().value("isError").toBool());
        QVERIFY2(toolText(*resp).contains(QStringLiteral("Refusing to send the token")), qPrintable(toolText(*resp)));
        // It never authenticated (the connection was dropped before the token).
        QTRY_COMPARE_WITH_TIMEOUT(server->clientCount(), clientsBefore, 5000);
        proc.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(proc.state() == QProcess::NotRunning, 10000);
#endif
    }

    void bridgeAnswersQuicklyWhenOffline()
    {
        const QString exe = bridgeExecutable();
        if (exe.isEmpty())
            QSKIP("opendaw-mcp not found (set OPENDAW_MCP_BRIDGE)");
        const QString path = QDir(harness().makeTempSubdir(QStringLiteral("none"))).filePath("missing.json");

        QProcess proc;
        proc.setProgram(exe);
        proc.setArguments({QStringLiteral("--discovery"), path});
        proc.start();
        QVERIFY2(proc.waitForStarted(10000), qPrintable(proc.errorString()));
        QElapsedTimer t;
        t.start();
        Lines lines;
        proc.write(line(request(1, QStringLiteral("initialize"), QJsonObject{{"protocolVersion", "2025-06-18"}}))
                   + line(request(2, QStringLiteral("ping"))) + line(request(3, QStringLiteral("tools/list"))));
        const auto list = waitForId(proc, lines, 3, 10000);
        QVERIFY2(list.has_value(), proc.readAllStandardError().constData());
        QVERIFY(lines.withId(1).has_value() && lines.withId(2).has_value());
        QVERIFY2(t.elapsed() < 3000, qPrintable(QStringLiteral("offline answers took %1 ms").arg(t.elapsed())));
        proc.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(proc.state() == QProcess::NotRunning, 10000);
    }

    void bridgeAttemptIsBoundedWhenServerIsSilent()
    {
        // A server that accepts but never answers the token: the attempt
        // must end (bounded) and the waiting request be answered offline.
        const QString exe = bridgeExecutable();
        if (exe.isEmpty())
            QSKIP("opendaw-mcp not found (set OPENDAW_MCP_BRIDGE)");

        QLocalServer silent;
        silent.setSocketOptions(QLocalServer::UserAccessOption);
        const QString name = QStringLiteral("opendaw-silent-%1").arg(QCoreApplication::applicationPid());
        QLocalServer::removeServer(name);
        QVERIFY2(silent.listen(name), qPrintable(silent.errorString()));
        QList<QLocalSocket*> accepted;
        connect(&silent, &QLocalServer::newConnection, &silent, [&] {
            while (silent.hasPendingConnections())
                accepted.append(silent.nextPendingConnection());
        });

        QJsonObject info{{"serverName", silent.serverName()}, {"fullServerName", silent.fullServerName()},
                         {"token", "0123"}, {"pid", qint64(QCoreApplication::applicationPid())}};
        const QString path = QDir(harness().makeTempSubdir(QStringLiteral("silent"))).filePath("agent-api.json");
        QVERIFY(writeDiscovery(path, info));

        QProcess proc;
        proc.setProgram(exe);
        proc.setArguments({QStringLiteral("--discovery"), path});
        proc.start();
        QVERIFY2(proc.waitForStarted(10000), qPrintable(proc.errorString()));
        QElapsedTimer t;
        t.start();
        Lines lines;
        proc.write(line(toolCall(1, QStringLiteral("get_project_info"))));
        const auto resp = waitForId(proc, lines, 1, 15000);
        QVERIFY2(resp.has_value(), proc.readAllStandardError().constData());
        QVERIFY(resp->value("result").toObject().value("isError").toBool());
        QVERIFY2(t.elapsed() < 9000, qPrintable(QStringLiteral("took %1 ms").arg(t.elapsed())));
        QVERIFY2(!accepted.isEmpty(), "the bridge never connected to the silent server");
        proc.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(proc.state() == QProcess::NotRunning, 10000);
        silent.close();
    }
};

OD_REGISTER_TEST(TestGatewayFixes)
#include "tst_GatewayFixes.moc"
