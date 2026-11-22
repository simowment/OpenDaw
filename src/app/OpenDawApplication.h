#pragma once

#include "engine/AudioEngine.h"
#include "engine/EditManager.h"
#include "engine/PluginScanner.h"
#include "JuceQtBridge.h"
#include <QObject>
#include <QWidget>
#include <memory>

namespace OpenDaw {

class MainWindow;
class PatternManager;
class PatternPlayer;
class RenderService;
class AiToolExecutor;
class CommandGateway;
class LocalApiServer;

class OpenDawApplication : public QObject {
    Q_OBJECT

public:
    explicit OpenDawApplication(QObject* parent = nullptr);
    ~OpenDawApplication() override;

    bool initialize();
    void checkRecovery(QWidget* splashToHide = nullptr);
    void showMainWindow();
    // Recreates the main window (e.g. after a theme change). The project, engine
    // and playback live outside it and carry on. Must not be called from inside
    // the old window's event handling; queue it.
    void rebuildMainWindow();

    AudioEngine&  audioEngine()  { return *audioEngine_; }
    EditManager&  editManager()  { return *editManager_; }
    PluginScanner& pluginScanner() { return *pluginScanner_; }
    MainWindow*   mainWindow()   { return mainWindow_.get(); }

    // Pattern sequencing (Channel Rack / step sequencer / playlist patterns).
    PatternManager& patternManager() { return *patternManager_; }
    PatternPlayer&  patternPlayer()  { return *patternPlayer_; }

    // External agent API (MCP over a local socket). May be null if disabled.
    LocalApiServer* agentApiServer() { return agentApiServer_.get(); }
    // Settings "agent/apiEnabled" (default on), env OPENDAW_AGENT_API=0|1,
    // or --no-agent-api on the command line.
    static bool agentApiWanted();
    void startAgentApi();
    void stopAgentApi();

private:
    std::unique_ptr<JuceQtBridge>   bridge_;
    std::unique_ptr<AudioEngine>    audioEngine_;
    std::unique_ptr<EditManager>    editManager_;
    std::unique_ptr<PluginScanner>  pluginScanner_;
    std::unique_ptr<PatternManager> patternManager_;
    std::unique_ptr<PatternPlayer>  patternPlayer_;
    std::unique_ptr<RenderService>  renderService_;
    std::unique_ptr<AiToolExecutor> agentExecutor_;
    std::unique_ptr<CommandGateway> agentGateway_;
    std::unique_ptr<LocalApiServer> agentApiServer_;
    std::unique_ptr<MainWindow>     mainWindow_;
};

} // namespace OpenDaw
