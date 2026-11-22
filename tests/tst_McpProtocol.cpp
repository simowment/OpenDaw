// MCP / JSON-RPC behaviour of CommandGateway, then LocalApiServer end to end
// over a QLocalSocket, then (when built) the opendaw-mcp stdio bridge.
//
// The server lives in this thread, so nothing here may block: every wait
// pumps the event loop (QTRY_* / QTest::qWait) instead of waitFor*().

#include "TestHarness.h"
#include "engine/EditManager.h"

#include "ai/AiToolExtensions.h"

#include "api/CommandGateway.h"
#include "api/LocalApiServer.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QSet>
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

// Reads newline-delimited JSON from any QIODevice while pumping events.
class LineReader {
public:
    explicit LineReader(QIODevice& dev) : dev_(dev) {}

    // Next complete JSON line, or nullopt after timeoutMs.
    std::optional<QJsonDocument> next(int timeoutMs = 10000)
    {
        QElapsedTimer t;
        t.start();
        while (true) {
            buffer_ += dev_.readAll();
            const int nl = buffer_.indexOf('\n');
            if (nl >= 0) {
                const QByteArray l = buffer_.left(nl).trimmed();
                buffer_.remove(0, nl + 1);
                if (l.isEmpty())
                    continue;
                return QJsonDocument::fromJson(l);
            }
            if (t.elapsed() > timeoutMs)
                return std::nullopt;
            QTest::qWait(10);
        }
    }

    // Next message whose id equals `id`, skipping notifications and other
    // messages (the bridge may send notifications/tools/list_changed).
    std::optional<QJsonDocument> nextWithId(qint64 id, int timeoutMs = 20000)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            auto doc = next(int(timeoutMs - t.elapsed()));
            if (!doc)
                return std::nullopt;
            if (doc->isObject() && doc->object().value("id").isDouble()
                && doc->object().value("id").toInteger() == id)
                return doc;
        }
        return std::nullopt;
    }

    bool hasPendingData()
    {
        buffer_ += dev_.readAll();
        return !buffer_.trimmed().isEmpty();
    }

private:
    QIODevice& dev_;
    QByteArray buffer_;
};

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
    // Where the build puts the helpers ($<TARGET_FILE_DIR:OpenDaw>).
    if (const QFileInfo built(QDir(QString::fromUtf8(OPENDAW_TOOLS_DIR)).filePath(exe)); built.isExecutable())
        return built.absoluteFilePath();
#endif
    const QDir here(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        here.filePath(exe),
        here.filePath(QStringLiteral("OpenDaw_artefacts/Release/") + exe),
        here.filePath(QStringLiteral("OpenDaw_artefacts/Debug/") + exe),
        here.filePath(QStringLiteral("OpenDaw_artefacts/RelWithDebInfo/") + exe),
        here.filePath(QStringLiteral("OpenDaw_artefacts/") + exe),
        here.filePath(QStringLiteral("../OpenDaw_artefacts/Release/") + exe),
        here.filePath(QStringLiteral("../OpenDaw_artefacts/Debug/") + exe),
    };
    for (const auto& c : candidates)
        if (QFileInfo(c).isExecutable())
            return QFileInfo(c).absoluteFilePath();
    return {};
}

} // namespace

class TestMcpProtocol : public QObject {
    Q_OBJECT

private slots:
    void init() { harness().resetProject(); }

    // ── Gateway (direct) ─────────────────────────────────────────────────────

    void initializeResponse()
    {
        const QJsonObject resp = harness().rpc(QStringLiteral("initialize"), QJsonObject{
            {"protocolVersion", "2025-06-18"},
            {"capabilities", QJsonObject{}},
            {"clientInfo", QJsonObject{{"name", "OpenDawTests"}, {"version", "1"}}},
        });
        QCOMPARE(resp.value("jsonrpc").toString(), QStringLiteral("2.0"));
        QVERIFY2(resp.contains("result"), QJsonDocument(resp).toJson().constData());
        const QJsonObject r = resp.value("result").toObject();
        QCOMPARE(r.value("protocolVersion").toString(), QStringLiteral("2025-06-18"));
        QVERIFY(!r.value("serverInfo").toObject().value("name").toString().isEmpty());
        QVERIFY(r.value("serverInfo").toObject().contains("version"));
        QVERIFY(r.value("capabilities").toObject().value("tools").isObject());
    }

    void initializeUnknownVersionFallsBack()
    {
        const QJsonObject resp = harness().rpc(QStringLiteral("initialize"),
                                               QJsonObject{{"protocolVersion", "1999-01-01"}});
        const QString v = resp.value("result").toObject().value("protocolVersion").toString();
        QCOMPARE(v, CommandGateway::protocolVersion());
    }

    void initializedNotificationHasNoResponse()
    {
        const QJsonObject resp = harness().rpc(QJsonObject{
            {"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        QVERIFY2(resp.isEmpty(), QJsonDocument(resp).toJson().constData());
    }

    void ping()
    {
        const QJsonObject resp = harness().rpc(request(77, QStringLiteral("ping")));
        QCOMPARE(resp.value("id").toInteger(), qint64(77));
        QVERIFY(resp.value("result").isObject());
        QVERIFY(resp.value("result").toObject().isEmpty());
        QVERIFY(!resp.contains("error"));
    }

    void stringIdEchoed()
    {
        QJsonObject msg = request(0, QStringLiteral("ping"));
        msg["id"] = QStringLiteral("abc-1");
        const QJsonObject resp = harness().rpc(msg);
        QCOMPARE(resp.value("id").toString(), QStringLiteral("abc-1"));
    }

    void toolsListAnnotations()
    {
        const QJsonObject resp = harness().rpc(QStringLiteral("tools/list"));
        const QJsonArray tools = resp.value("result").toObject().value("tools").toArray();
        QVERIFY(tools.size() >= 69);

        QHash<QString, QJsonObject> byName;
        for (const auto& v : tools) {
            const QJsonObject t = v.toObject();
            const QString name = t.value("name").toString();
            QVERIFY(!name.isEmpty());
            QVERIFY2(!byName.contains(name), qPrintable("duplicate tool " + name));
            QVERIFY2(!t.value("description").toString().isEmpty(), qPrintable(name));
            QCOMPARE(t.value("inputSchema").toObject().value("type").toString(), QStringLiteral("object"));
            const QJsonObject ann = t.value("annotations").toObject();
            QVERIFY2(ann.value("readOnlyHint").isBool(), qPrintable(name));
            QVERIFY2(ann.value("destructiveHint").isBool(), qPrintable(name));
            byName.insert(name, t);
        }

        QVERIFY(byName.contains("get_project_info"));
        QCOMPARE(byName["get_project_info"].value("annotations").toObject().value("readOnlyHint").toBool(), true);
        QCOMPARE(byName["get_project_info"].value("annotations").toObject().value("destructiveHint").toBool(), false);
        QVERIFY(byName.contains("delete_track"));
        QCOMPARE(byName["delete_track"].value("annotations").toObject().value("destructiveHint").toBool(), true);
        QCOMPARE(byName["delete_track"].value("annotations").toObject().value("readOnlyHint").toBool(), false);
        QCOMPARE(byName["set_track_mute"].value("annotations").toObject().value("readOnlyHint").toBool(), false);
    }

    void toolsCallUnknownTool()
    {
        const QJsonObject resp = harness().rpc(request(5, QStringLiteral("tools/call"),
            QJsonObject{{"name", "no_such_tool"}, {"arguments", QJsonObject{}}}));
        QVERIFY2(!resp.contains("error"), "unknown tool must be a tool result, not a JSON-RPC error");
        const QJsonObject r = resp.value("result").toObject();
        QCOMPARE(r.value("isError").toBool(), true);
        QCOMPARE(r.value("structuredContent").toObject().value("error").toObject().value("code").toString(),
                 AiErrorCode::UnknownTool);
        QVERIFY(!r.value("content").toArray().isEmpty());
    }

    void toolsCallResultShape()
    {
        const QJsonObject resp = harness().rpc(request(6, QStringLiteral("tools/call"),
            QJsonObject{{"name", "get_project_info"}}));
        const QJsonObject r = resp.value("result").toObject();
        QCOMPARE(r.value("isError").toBool(), false);
        const QJsonArray content = r.value("content").toArray();
        QVERIFY(!content.isEmpty());
        QCOMPARE(content.first().toObject().value("type").toString(), QStringLiteral("text"));
        QVERIFY(r.value("structuredContent").isObject());
        QVERIFY(!r.value("structuredContent").toObject().isEmpty());
    }

    void malformedRequests_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::newRow("no method") << QByteArray(R"({"jsonrpc":"2.0","id":1})");
        QTest::newRow("empty method") << QByteArray(R"({"jsonrpc":"2.0","id":1,"method":""})");
        QTest::newRow("method not string") << QByteArray(R"({"jsonrpc":"2.0","id":1,"method":5})");
        QTest::newRow("wrong jsonrpc") << QByteArray(R"({"jsonrpc":"1.0","id":1,"method":"ping"})");
        QTest::newRow("object id") << QByteArray(R"({"jsonrpc":"2.0","id":{"a":1},"method":"ping"})");
    }

    void malformedRequests()
    {
        QFETCH(QByteArray, json);
        const QJsonObject resp = harness().rpc(QJsonDocument::fromJson(json).object());
        QVERIFY2(resp.contains("error"), QJsonDocument(resp).toJson().constData());
        QCOMPARE(resp.value("error").toObject().value("code").toInt(), int(CommandGateway::InvalidRequest));
    }

    void invalidParams()
    {
        const QJsonObject resp = harness().rpc(request(9, QStringLiteral("tools/call"),
            QJsonObject{{"arguments", QJsonObject{}}}));
        QCOMPARE(resp.value("error").toObject().value("code").toInt(), int(CommandGateway::InvalidParams));
    }

    void unknownMethod()
    {
        const QJsonObject resp = harness().rpc(request(10, QStringLiteral("tools/explode")));
        QCOMPARE(resp.value("error").toObject().value("code").toInt(), int(CommandGateway::MethodNotFound));
        QCOMPARE(resp.value("id").toInteger(), qint64(10));
    }

    void jsonRpcBatch()
    {
        const QJsonArray batch{
            request(1, QStringLiteral("ping")),
            QJsonObject{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},
            request(2, QStringLiteral("tools/list")),
            request(3, QStringLiteral("nope")),
            42,
        };
        const QJsonArray out = harness().gateway().handleBatch(batch);
        QCOMPARE(int(out.size()), 4); // the notification produces none
        QSet<qint64> ids;
        int methodNotFound = 0, invalid = 0;
        for (const auto& v : out) {
            const QJsonObject r = v.toObject();
            if (r.value("id").isDouble())
                ids.insert(r.value("id").toInteger());
            const int code = r.value("error").toObject().value("code").toInt();
            if (code == CommandGateway::MethodNotFound) ++methodNotFound;
            if (code == CommandGateway::InvalidRequest) ++invalid;
        }
        QVERIFY(ids.contains(1) && ids.contains(2) && ids.contains(3));
        QCOMPARE(methodNotFound, 1);
        QCOMPARE(invalid, 1);

        const QJsonArray empty = harness().gateway().handleBatch(QJsonArray{});
        QCOMPARE(int(empty.size()), 1);
        QCOMPARE(empty.first().toObject().value("error").toObject().value("code").toInt(),
                 int(CommandGateway::InvalidRequest));
    }

    void destructiveToolsCanBeDisabled()
    {
        auto& gw = harness().gateway();
        gw.setAllowDestructive(false);
        const QJsonObject s = harness().call(QStringLiteral("delete_track"), QJsonObject{{"track", 0}});
        const bool failed = harness().lastCallFailed();
        gw.setAllowDestructive(true);
        QVERIFY(failed);
        QCOMPARE(Test::TestHarness::errorCode(s), AiErrorCode::NotAllowed);
        QCOMPARE(harness().editManager().getAudioTracks().size(), 4); // nothing deleted
    }

    // ── LocalApiServer over a socket ─────────────────────────────────────────

    void socketRejectsUnauthenticated()
    {
        auto* server = harness().server();
        if (!server)
            QSKIP("LocalApiServer did not start");
        QVERIFY(server->isListening());
        QVERIFY(server->requireToken());

        QLocalSocket sock;
        sock.connectToServer(harness().serverName());
        QTRY_COMPARE_WITH_TIMEOUT(sock.state(), QLocalSocket::ConnectedState, 5000);

        sock.write(line(request(1, QStringLiteral("tools/list"))));
        LineReader reader(sock);
        const auto resp = reader.next();
        QVERIFY2(resp.has_value(), "no answer to an unauthenticated request");
        QCOMPARE(resp->object().value("error").toObject().value("code").toInt(),
                 int(CommandGateway::Unauthenticated));
        QTRY_COMPARE_WITH_TIMEOUT(sock.state(), QLocalSocket::UnconnectedState, 5000);
    }

    void socketRejectsWrongToken()
    {
        auto* server = harness().server();
        if (!server)
            QSKIP("LocalApiServer did not start");

        QLocalSocket sock;
        sock.connectToServer(harness().serverName());
        QTRY_COMPARE_WITH_TIMEOUT(sock.state(), QLocalSocket::ConnectedState, 5000);
        sock.write(line(request(0, QStringLiteral("opendaw/authenticate"),
                                QJsonObject{{"token", QString(64, QLatin1Char('0'))}})));
        LineReader reader(sock);
        const auto resp = reader.next();
        QVERIFY(resp.has_value());
        QCOMPARE(resp->object().value("error").toObject().value("code").toInt(),
                 int(CommandGateway::Unauthenticated));
        QTRY_COMPARE_WITH_TIMEOUT(sock.state(), QLocalSocket::UnconnectedState, 5000);
    }

    void socketAuthenticatedSession()
    {
        auto* server = harness().server();
        if (!server)
            QSKIP("LocalApiServer did not start");
        QCOMPARE(int(server->token().size()), 64);

        QLocalSocket sock;
        sock.connectToServer(harness().serverName());
        QTRY_COMPARE_WITH_TIMEOUT(sock.state(), QLocalSocket::ConnectedState, 5000);
        LineReader reader(sock);

        sock.write(line(request(0, QStringLiteral("opendaw/authenticate"),
                                QJsonObject{{"token", server->token()}})));
        const auto auth = reader.next();
        QVERIFY2(auth.has_value(), "no answer to authenticate");
        QVERIFY2(!auth->object().contains("error"), auth->toJson().constData());
        QCOMPARE(auth->object().value("id").toInteger(), qint64(0));
        QTRY_VERIFY_WITH_TIMEOUT(server->clientCount() >= 1, 5000);

        // tools/list
        sock.write(line(request(1, QStringLiteral("tools/list"))));
        const auto list = reader.next(15000);
        QVERIFY(list.has_value());
        QCOMPARE(list->object().value("id").toInteger(), qint64(1));
        QVERIFY(list->object().value("result").toObject().value("tools").toArray().size() >= 69);

        // A line delivered in two writes is answered once, after the newline.
        const QByteArray full = line(request(2, QStringLiteral("ping")));
        const int half = full.size() / 2;
        sock.write(full.left(half));
        sock.flush();
        QTest::qWait(150);
        QVERIFY2(!reader.hasPendingData(), "server answered a partial line");
        sock.write(full.mid(half));
        const auto pong = reader.next();
        QVERIFY(pong.has_value());
        QCOMPARE(pong->object().value("id").toInteger(), qint64(2));
        QVERIFY(pong->object().value("result").isObject());

        // Two messages in one write: two answers, in order.
        sock.write(line(request(3, QStringLiteral("ping"))) + line(request(4, QStringLiteral("ping"))));
        const auto a = reader.next();
        const auto b = reader.next();
        QVERIFY(a.has_value() && b.has_value());
        QCOMPARE(a->object().value("id").toInteger(), qint64(3));
        QCOMPARE(b->object().value("id").toInteger(), qint64(4));

        // A tool call over the socket.
        sock.write(line(request(5, QStringLiteral("tools/call"),
                                QJsonObject{{"name", "get_project_info"}, {"arguments", QJsonObject{}}})));
        const auto info = reader.next(15000);
        QVERIFY(info.has_value());
        QCOMPARE(info->object().value("result").toObject().value("isError").toBool(), false);

        // A batch array on one line gets an array back.
        const QJsonArray batch{request(6, QStringLiteral("ping")), request(7, QStringLiteral("ping"))};
        sock.write(QJsonDocument(batch).toJson(QJsonDocument::Compact) + '\n');
        const auto batchResp = reader.next();
        QVERIFY(batchResp.has_value());
        QVERIFY(batchResp->isArray());
        QCOMPARE(int(batchResp->array().size()), 2);

        // Garbage is a parse error, and the connection stays usable.
        sock.write("{not json\n");
        const auto parseErr = reader.next();
        QVERIFY(parseErr.has_value());
        QCOMPARE(parseErr->object().value("error").toObject().value("code").toInt(),
                 int(CommandGateway::ParseError));
        sock.write(line(request(8, QStringLiteral("ping"))));
        const auto after = reader.next();
        QVERIFY(after.has_value());
        QCOMPARE(after->object().value("id").toInteger(), qint64(8));

        sock.disconnectFromServer();
    }

    void discoveryFileDescribesServer()
    {
        auto* server = harness().server();
        if (!server)
            QSKIP("LocalApiServer did not start");
        const QString path = LocalApiServer::discoveryFilePath();
        QVERIFY2(QDir::fromNativeSeparators(path).startsWith(QDir::fromNativeSeparators(harness().tempPath()),
                                                             Qt::CaseInsensitive),
                 qPrintable("discovery file outside the test dir: " + path));
        QFile f(path);
        QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(path));
        const QByteArray data = f.readAll();
        QVERIFY(data.contains(server->serverName().toUtf8()));
        QVERIFY(data.contains(server->token().toUtf8()));
    }

    // ── opendaw-mcp stdio bridge ─────────────────────────────────────────────

    void stdioBridge()
    {
        auto* server = harness().server();
        if (!server)
            QSKIP("LocalApiServer did not start");
        const QString exe = bridgeExecutable();
        if (exe.isEmpty())
            QSKIP("opendaw-mcp not found next to the test executable (set OPENDAW_MCP_BRIDGE)");

        QProcess proc;
        proc.setProgram(exe);
        proc.setArguments({QStringLiteral("--pipe"), harness().serverName(),
                           QStringLiteral("--discovery"), LocalApiServer::discoveryFilePath()});
        proc.setProcessChannelMode(QProcess::SeparateChannels);
        proc.start();
        // waitForStarted only waits for the OS to create the process; it does
        // not need our event loop.
        QVERIFY2(proc.waitForStarted(10000), qPrintable(proc.errorString()));

        LineReader reader(proc);
        proc.write(line(request(1, QStringLiteral("initialize"), QJsonObject{
            {"protocolVersion", "2025-06-18"},
            {"capabilities", QJsonObject{}},
            {"clientInfo", QJsonObject{{"name", "OpenDawTests"}, {"version", "1"}}}})));
        const auto init = reader.nextWithId(1, 20000);
        QVERIFY2(init.has_value(), proc.readAllStandardError().constData());
        QVERIFY2(init->object().contains("result"), init->toJson().constData());
        QVERIFY(!init->object().value("result").toObject().value("protocolVersion").toString().isEmpty());

        proc.write(line(QJsonObject{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));
        proc.write(line(request(2, QStringLiteral("tools/call"),
                                QJsonObject{{"name", "get_project_info"}, {"arguments", QJsonObject{}}})));
        const auto info = reader.nextWithId(2, 20000);
        QVERIFY2(info.has_value(), proc.readAllStandardError().constData());
        QCOMPARE(info->object().value("id").toInteger(), qint64(2));
        const QJsonObject r = info->object().value("result").toObject();
        QCOMPARE(r.value("isError").toBool(), false);
        QVERIFY(Test::TestHarness::findKey(r.value("structuredContent"), {"track_count", "tracks"}).isDouble()
                || Test::TestHarness::findKey(r.value("structuredContent"), {"tracks"}).isArray());

        // Closing stdin ends the bridge.
        proc.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(proc.state() == QProcess::NotRunning, 10000);
    }
};

OD_REGISTER_TEST(TestMcpProtocol)
#include "tst_McpProtocol.moc"
