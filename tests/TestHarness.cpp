#include "TestHarness.h"

#include "ai/AiIds.h"
#include "ai/AiToolExecutor.h"
#include "ai/AiToolExtensions.h"
#include "api/CommandGateway.h"
#include "api/LocalApiServer.h"
#include "api/PatternTools.h"
#include "api/RenderService.h"
#include "app/JuceQtBridge.h"
#include "engine/EditManager.h"
#include "engine/PatternManager.h"
#include "engine/PluginScanner.h"
#include "engine/instruments/BundledPlugins.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QQueue>
#include <QStandardPaths>
#include <QtTest>

namespace OpenDaw::Test {

namespace {
TestHarness* g_instance = nullptr;
}

std::vector<RegisteredTest>& registeredTests()
{
    static std::vector<RegisteredTest> tests;
    return tests;
}

bool registerTest(const char* name, std::function<std::unique_ptr<QObject>()> create)
{
    registeredTests().push_back({name, std::move(create)});
    return true;
}

TestHarness::TestHarness()
{
    Q_ASSERT(g_instance == nullptr);
    g_instance = this;
}

TestHarness::~TestHarness()
{
    shutdown();
    if (g_instance == this)
        g_instance = nullptr;
}

TestHarness& TestHarness::instance()
{
    Q_ASSERT_X(g_instance, "TestHarness::instance", "no harness: main() creates it");
    return *g_instance;
}

bool TestHarness::init(QString* error)
{
    auto fail = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };

    // Keep QStandardPaths (autosave folder, chat store, ...) away from the
    // user's real data. QSettings is separated by the organisation name main()
    // sets ("OpenDaw-tests").
    QStandardPaths::setTestModeEnabled(true);

    if (!tempDir_.isValid())
        return fail(QStringLiteral("cannot create a temporary directory"));

    // The constructing thread becomes the JUCE message thread: it must be the
    // Qt GUI thread, before any Tracktion object exists.
    juceInit_ = std::make_unique<juce::ScopedJuceInitialiser_GUI>();

    // On macOS it only points Surge XT at bundled content; harmless elsewhere.
    BundledPlugins::prepareEnvironment();

    bridge_ = std::make_unique<JuceQtBridge>();
    bridge_->start();

    // No setDefaultAudioDevice(): CI machines have no audio hardware, and
    // offline rendering does not need a device.
    audioEngine_ = std::make_unique<AudioEngine>();
    editManager_ = std::make_unique<EditManager>(*audioEngine_);
    pluginScanner_ = std::make_unique<PluginScanner>(*audioEngine_);
    patternManager_ = std::make_unique<PatternManager>(*editManager_);
    renderService_ = std::make_unique<RenderService>(*editManager_);

    if (!editManager_->edit())
        return fail(QStringLiteral("EditManager did not create an Edit"));

    // Same providers OpenDawApplication registers. Clear first so a provider
    // registered by anything else cannot point at objects we do not own.
    auto& registry = AiToolExtensionRegistry::instance();
    registry.clear();
    {
        auto* render = renderService_.get();
        auto* patterns = patternManager_.get();
        registry.addProvider([render](AiToolExecutor& ex) { return render->toolExtensions(ex); });
        registry.addProvider([patterns](AiToolExecutor& ex) { return patternToolExtensions(*patterns, ex); });
    }

    executor_ = std::make_unique<AiToolExecutor>(editManager_.get(), audioEngine_.get(), pluginScanner_.get());
    gateway_ = std::make_unique<CommandGateway>(*executor_);
    gateway_->setAllowDestructive(true);

    QString serverError;
    if (!startServer(&serverError))
        qWarning() << "[tests] LocalApiServer did not start:" << serverError
                   << "- socket tests will be skipped";

    initialised_ = true;
    pump(50);
    return true;
}

bool TestHarness::startServer(QString* error)
{
    serverName_ = QStringLiteral("opendaw-test-%1").arg(QCoreApplication::applicationPid());
    discoveryPath_ = QDir(tempDir_.path()).filePath(QStringLiteral("agent-api.json"));

    // LocalApiServer (and the opendaw-mcp bridge) read the discovery file
    // location from this variable; without it the server would overwrite the
    // real app's per-user discovery file.
    qputenv("OPENDAW_AGENT_API_FILE", QFile::encodeName(QDir::toNativeSeparators(discoveryPath_)));

    server_ = std::make_unique<LocalApiServer>(*gateway_);
    server_->setRequireToken(true);
    if (!server_->start(serverName_)) {
        if (error) *error = QStringLiteral("start() returned false for %1").arg(serverName_);
        server_.reset();
        return false;
    }
    // The server may decorate the name (e.g. "-<pid>" if taken); use its view.
    serverName_ = server_->serverName();
    discoveryPath_ = LocalApiServer::discoveryFilePath();
    return true;
}

void TestHarness::shutdown()
{
    if (!initialised_ && !juceInit_)
        return;
    initialised_ = false;

    if (server_)
        server_->stop();
    server_.reset();
    gateway_.reset();
    executor_.reset();
    AiToolExtensionRegistry::instance().clear();
    renderService_.reset();
    patternManager_.reset();
    pluginScanner_.reset();
    editManager_.reset();
    audioEngine_.reset();
    if (bridge_)
        bridge_->stop();
    bridge_.reset();
    juceInit_.reset();
}

te::Edit& TestHarness::edit()
{
    Q_ASSERT(editManager_ && editManager_->edit());
    return *editManager_->edit();
}

QString TestHarness::makeTempSubdir(const QString& prefix)
{
    QDir root(tempDir_.path());
    const QString name = QStringLiteral("%1-%2").arg(prefix).arg(++subdirCounter_);
    root.mkpath(name);
    return root.filePath(name);
}

void TestHarness::pump(int ms)
{
    // QTest::qWait processes Qt events; JuceQtBridge's timer pumps JUCE's
    // message queue from inside them.
    QTest::qWait(ms);
}

// ── JSON-RPC ─────────────────────────────────────────────────────────────────

QJsonObject TestHarness::rpc(const QJsonObject& message)
{
    return gateway_->handleMessage(message);
}

QJsonObject TestHarness::rpc(const QString& method, const QJsonObject& params)
{
    QJsonObject msg{{"jsonrpc", "2.0"}, {"id", nextRpcId_++}, {"method", method}};
    if (!params.isEmpty())
        msg["params"] = params;
    return rpc(msg);
}

QJsonObject TestHarness::call(const QString& tool, const QJsonObject& args)
{
    lastFailed_ = true;
    lastText_.clear();
    lastRaw_ = {};

    const QJsonObject resp = rpc(QStringLiteral("tools/call"),
                                 QJsonObject{{"name", tool}, {"arguments", args}});
    if (resp.contains("error")) {
        lastText_ = QStringLiteral("JSON-RPC error: ")
                    + QString::fromUtf8(QJsonDocument(resp.value("error").toObject()).toJson(QJsonDocument::Compact));
        return QJsonObject{{"error", resp.value("error")}};
    }

    const QJsonObject result = resp.value("result").toObject();
    lastRaw_ = result;
    lastFailed_ = result.value("isError").toBool(false);

    QString text;
    for (const auto& c : result.value("content").toArray()) {
        const QJsonObject block = c.toObject();
        if (block.value("type").toString() == QLatin1String("text"))
            text += block.value("text").toString();
    }
    lastText_ = text;

    if (result.value("structuredContent").isObject())
        return result.value("structuredContent").toObject();

    const auto doc = QJsonDocument::fromJson(text.toUtf8());
    if (doc.isObject())
        return doc.object();
    if (doc.isArray())
        return QJsonObject{{"items", doc.array()}};
    return QJsonObject{{"message", text}};
}

void TestHarness::resetProject()
{
    if (!editManager_)
        return;
    if (auto* e = editManager_->edit(); e && e->getTransport().isPlaying())
        e->getTransport().stop(false, false);
    editManager_->newEdit();
    pump(20);
}

// ── Lookups ──────────────────────────────────────────────────────────────────

QString TestHarness::findId(const QJsonValue& root, const QString& kind)
{
    const QStringList preferred{QStringLiteral("id"), kind + QStringLiteral("_id")};
    QQueue<QJsonValue> queue;
    queue.enqueue(root);
    while (!queue.isEmpty()) {
        const QJsonValue v = queue.dequeue();
        if (v.isString()) {
            if (ids::isIdOfKind(v.toString(), kind))
                return v.toString();
        } else if (v.isObject()) {
            const QJsonObject o = v.toObject();
            for (const auto& key : preferred)
                if (ids::isIdOfKind(o.value(key).toString(), kind))
                    return o.value(key).toString();
            for (auto it = o.begin(); it != o.end(); ++it)
                queue.enqueue(it.value());
        } else if (v.isArray()) {
            for (const auto& item : v.toArray())
                queue.enqueue(item);
        }
    }
    return {};
}

QJsonValue TestHarness::findKey(const QJsonValue& root, const QStringList& keys)
{
    QQueue<QJsonValue> queue;
    queue.enqueue(root);
    while (!queue.isEmpty()) {
        const QJsonValue v = queue.dequeue();
        if (v.isObject()) {
            const QJsonObject o = v.toObject();
            for (const auto& key : keys)
                if (o.contains(key))
                    return o.value(key);
            for (auto it = o.begin(); it != o.end(); ++it)
                if (it.value().isObject() || it.value().isArray())
                    queue.enqueue(it.value());
        } else if (v.isArray()) {
            for (const auto& item : v.toArray())
                queue.enqueue(item);
        }
    }
    return QJsonValue(QJsonValue::Undefined);
}

QString TestHarness::errorCode(const QJsonObject& structured)
{
    const QJsonValue code = structured.value("error").toObject().value("code");
    if (code.isString())
        return code.toString();
    return {};
}

te::AudioTrack* TestHarness::trackById(const QString& id)
{
    const auto itemId = ids::parse(id, ids::kTrack);
    if (!itemId || !editManager_->edit())
        return nullptr;
    return te::findAudioTrackForID(*editManager_->edit(), *itemId);
}

te::Clip* TestHarness::clipById(const QString& id)
{
    const auto itemId = ids::parse(id, ids::kClip);
    if (!itemId || !editManager_->edit())
        return nullptr;
    return te::findClipForID(*editManager_->edit(), *itemId);
}

} // namespace OpenDaw::Test
