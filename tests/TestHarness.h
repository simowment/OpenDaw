#pragma once

// Process-wide fixture for OpenDawTests.
//
// main() creates one TestHarness after the QGuiApplication. It brings up the
// same object graph OpenDawApplication::initialize() does, minus the window
// and minus the audio device (no hardware in CI):
//
//   ScopedJuceInitialiser_GUI -> JuceQtBridge -> AudioEngine -> EditManager
//   -> PluginScanner -> PatternManager -> RenderService
//   -> AiToolExtensionRegistry providers (render + pattern tools)
//   -> AiToolExecutor -> CommandGateway -> LocalApiServer("opendaw-test-<pid>")
//
// Test classes reach it with OpenDaw::Test::harness() and register themselves
// with OD_REGISTER_TEST(ClassName) at the bottom of their .cpp.
//
// Isolation: organisation "OpenDaw-tests", QStandardPaths test mode, and every
// file a test writes goes under harness().tempPath().

#include "engine/AudioEngine.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <functional>
#include <memory>
#include <vector>

namespace OpenDaw {
class JuceQtBridge;
class EditManager;
class PluginScanner;
class PatternManager;
class RenderService;
class AiToolExecutor;
class CommandGateway;
class LocalApiServer;
}

namespace OpenDaw::Test {

// ── Test class registry ─────────────────────────────────────────────────────

struct RegisteredTest {
    const char* name;
    std::function<std::unique_ptr<QObject>()> create;
};

std::vector<RegisteredTest>& registeredTests();
bool registerTest(const char* name, std::function<std::unique_ptr<QObject>()> create);

#define OD_REGISTER_TEST(Class)                                                     \
    namespace {                                                                     \
    const bool od_registered_##Class = ::OpenDaw::Test::registerTest(              \
        #Class, [] { return std::unique_ptr<QObject>(new Class); });               \
    }

// ── Harness ─────────────────────────────────────────────────────────────────

class TestHarness {
public:
    TestHarness();
    ~TestHarness();

    TestHarness(const TestHarness&) = delete;
    TestHarness& operator=(const TestHarness&) = delete;

    // Builds the whole object graph. False (with a reason) when something
    // essential could not be created; main() then exits non-zero.
    bool init(QString* error);
    // Stops the socket server and destroys everything in reverse order.
    void shutdown();

    static TestHarness& instance();

    AudioEngine&    audioEngine()    { return *audioEngine_; }
    EditManager&    editManager()    { return *editManager_; }
    PluginScanner&  pluginScanner()  { return *pluginScanner_; }
    PatternManager& patternManager() { return *patternManager_; }
    RenderService&  renderService()  { return *renderService_; }
    AiToolExecutor& executor()       { return *executor_; }
    CommandGateway& gateway()        { return *gateway_; }
    LocalApiServer* server()         { return server_.get(); } // null if it failed to start

    te::Edit& edit();

    // Root of this run's scratch files (removed at exit).
    QString tempPath() const { return tempDir_.path(); }
    // A fresh, empty sub-directory of tempPath().
    QString makeTempSubdir(const QString& prefix);

    QString serverName() const { return serverName_; }
    QString discoveryFilePath() const { return discoveryPath_; }

    // ── JSON-RPC through CommandGateway::handleMessage ──────────────────────
    QJsonObject rpc(const QJsonObject& message);
    QJsonObject rpc(const QString& method, const QJsonObject& params = {});

    // tools/call. Returns structuredContent (falls back to the text content
    // parsed as JSON, or {"message": text}). lastCallFailed() tells whether
    // the result had isError (or the request itself failed).
    QJsonObject call(const QString& tool, const QJsonObject& args = {});
    bool lastCallFailed() const { return lastFailed_; }
    QString lastCallText() const { return lastText_; }      // text content or error description
    QJsonObject lastRawResult() const { return lastRaw_; }  // the whole JSON-RPC "result"

    // New empty project (4 tracks, like the app), fresh undo history.
    void resetProject();

    // Pumps Qt + JUCE events for ms milliseconds.
    static void pump(int ms);

    // ── Lookups ─────────────────────────────────────────────────────────────
    // First "<kind>:<n>" string in a result, breadth-first, preferring a key
    // named "id" / "<kind>_id" at each level. Empty when none.
    static QString findId(const QJsonValue& v, const QString& kind);
    // First value stored under any of the given keys, searched breadth-first
    // through objects and arrays. Undefined when none.
    static QJsonValue findKey(const QJsonValue& v, const QStringList& keys);
    // Error code of an error result ({"error":{"code":...}}), or "".
    static QString errorCode(const QJsonObject& structured);

    te::AudioTrack* trackById(const QString& id);
    te::Clip* clipById(const QString& id);

private:
    bool startServer(QString* error);

    QTemporaryDir tempDir_;
    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juceInit_;
    std::unique_ptr<JuceQtBridge>   bridge_;
    std::unique_ptr<AudioEngine>    audioEngine_;
    std::unique_ptr<EditManager>    editManager_;
    std::unique_ptr<PluginScanner>  pluginScanner_;
    std::unique_ptr<PatternManager> patternManager_;
    std::unique_ptr<RenderService>  renderService_;
    std::unique_ptr<AiToolExecutor> executor_;
    std::unique_ptr<CommandGateway> gateway_;
    std::unique_ptr<LocalApiServer> server_;

    QString serverName_;
    QString discoveryPath_;
    qint64 nextRpcId_ = 1;
    bool lastFailed_ = false;
    QString lastText_;
    QJsonObject lastRaw_;
    int subdirCounter_ = 0;
    bool initialised_ = false;
};

inline TestHarness& harness() { return TestHarness::instance(); }

// Calls a tool that must succeed; binds the structured result to `out`.
#define OD_CALL_OK(out, tool, args)                                                         \
    const QJsonObject out = ::OpenDaw::Test::harness().call(QStringLiteral(tool), args);   \
    QVERIFY2(!::OpenDaw::Test::harness().lastCallFailed(),                                  \
             qPrintable(QStringLiteral(tool " failed: ") + ::OpenDaw::Test::harness().lastCallText()))

// Calls a tool that must fail; binds the structured result to `out`.
#define OD_CALL_ERR(out, tool, args)                                                        \
    const QJsonObject out = ::OpenDaw::Test::harness().call(QStringLiteral(tool), args);   \
    QVERIFY2(::OpenDaw::Test::harness().lastCallFailed(),                                   \
             qPrintable(QStringLiteral(tool " unexpectedly succeeded: ") +                  \
                        ::OpenDaw::Test::harness().lastCallText()))

} // namespace OpenDaw::Test
