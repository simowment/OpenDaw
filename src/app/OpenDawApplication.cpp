#include "OpenDawApplication.h"
#include "ui/MainWindow.h"
#include "engine/instruments/BundledPlugins.h"
#include "ui/dialogs/RecoveryDialog.h"
#include "utils/ThemeManager.h"
#include "engine/PatternManager.h"
#include "api/RenderService.h"
#include "api/PatternTools.h"
#include "api/CommandGateway.h"
#include "api/LocalApiServer.h"
#include "ai/AiToolExecutor.h"
#include "ai/AiToolExtensions.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QSettings>

namespace OpenDaw {

OpenDawApplication::OpenDawApplication(QObject* parent)
    : QObject(parent)
{
}

OpenDawApplication::~OpenDawApplication()
{
    // The window holds pointers into everything below; the agent API must stop
    // before the objects its executor uses go away.
    mainWindow_.reset();
    stopAgentApi();
}

bool OpenDawApplication::initialize()
{
    BundledPlugins::prepareEnvironment();

    qDebug() << "[init] creating JuceQtBridge";
    bridge_ = std::make_unique<JuceQtBridge>(this);
    bridge_->start();

    qDebug() << "[init] creating AudioEngine";
    audioEngine_ = std::make_unique<AudioEngine>();
    audioEngine_->setDefaultAudioDevice();
    audioEngine_->restoreSavedAudioSettings();

    qDebug() << "[init] creating EditManager";
    editManager_ = std::make_unique<EditManager>(*audioEngine_);
    qDebug() << "[init] EditManager created";
    pluginScanner_ = std::make_unique<PluginScanner>(*audioEngine_);

    qDebug() << "[init] creating pattern sequencer";
    patternManager_ = std::make_unique<PatternManager>(*editManager_);
    patternPlayer_ = std::make_unique<PatternPlayer>(*patternManager_, *editManager_);

    // Tool modules contribute their tools to every AiToolExecutor (the in-app
    // chat's and the external agent gateway's). Register before any executor
    // exists.
    renderService_ = std::make_unique<RenderService>(*editManager_);
    {
        auto* render = renderService_.get();
        auto* patterns = patternManager_.get();
        auto& registry = AiToolExtensionRegistry::instance();
        registry.addProvider([render](AiToolExecutor& ex) { return render->toolExtensions(ex); });
        registry.addProvider([patterns](AiToolExecutor& ex) { return patternToolExtensions(*patterns, ex); });
    }

    qDebug() << "[init] initialize complete";

    return true;
}

bool OpenDawApplication::agentApiWanted()
{
    if (qEnvironmentVariableIsSet("OPENDAW_AGENT_API"))
        return qEnvironmentVariableIntValue("OPENDAW_AGENT_API") != 0;
    if (QCoreApplication::arguments().contains(QStringLiteral("--no-agent-api")))
        return false;
    return QSettings().value(QStringLiteral("agent/apiEnabled"), true).toBool();
}

void OpenDawApplication::startAgentApi()
{
    if (agentApiServer_ || !agentApiWanted())
        return;

    qDebug() << "[init] starting agent API";
    agentExecutor_ = std::make_unique<AiToolExecutor>(editManager_.get(), audioEngine_.get(), pluginScanner_.get());
    agentGateway_ = std::make_unique<CommandGateway>(*agentExecutor_);
    agentGateway_->setAllowDestructive(
        QSettings().value(QStringLiteral("agent/allowDestructive"), true).toBool());
    agentApiServer_ = std::make_unique<LocalApiServer>(*agentGateway_);
    // Agent calls wait while a modal dialog or a popup (menu, combo list) is
    // up: the UI code that opened it may hold pointers into the Edit.
    agentApiServer_->setUiBusyPredicate([]() {
        return QApplication::activeModalWidget() != nullptr
            || QApplication::activePopupWidget() != nullptr;
    });
    if (!agentApiServer_->start()) {
        qWarning() << "[init] agent API failed to start";
        agentApiServer_.reset();
        agentGateway_.reset();
        agentExecutor_.reset();
        return;
    }
    qDebug() << "[init] agent API listening on" << agentApiServer_->serverName();
}

void OpenDawApplication::stopAgentApi()
{
    if (agentApiServer_)
        agentApiServer_->stop();
    agentApiServer_.reset();
    agentGateway_.reset();
    agentExecutor_.reset();
}

void OpenDawApplication::checkRecovery(QWidget* splashToHide)
{
    auto recoveryFiles = EditManager::findRecoveryFiles();
    if (recoveryFiles.isEmpty())
        return;

    // Put the splash back afterwards only if it was showing (the user may
    // have clicked it away).
    const bool splashWasVisible = splashToHide && splashToHide->isVisible();
    if (splashToHide)
        splashToHide->hide();

    RecoveryDialog dlg(recoveryFiles);
    if (dlg.exec() == QDialog::Accepted) {
        auto info = dlg.selectedRecovery();
        juce::File autosaveFile(juce::String(info.autosavePath.toUtf8().constData()));
        if (editManager_->loadEdit(autosaveFile)) {
            juce::File origFile;
            if (!info.originalPath.isEmpty())
                origFile = juce::File(juce::String(info.originalPath.toUtf8().constData()));
            editManager_->adoptRecoveredEdit(origFile);
        }
    }

    for (const auto& info : recoveryFiles) {
        QFile::remove(info.autosavePath);
        auto baseName = info.autosavePath.left(info.autosavePath.lastIndexOf('.'));
        QFile::remove(baseName + ".json");
    }

    if (splashWasVisible)
        splashToHide->show();
}

void OpenDawApplication::showMainWindow()
{
    // Start the agent API only now: after session recovery (which may replace
    // the Edit) and before the window, whose status bar shows the API state.
    startAgentApi();

    qDebug() << "[init] creating MainWindow";
    ThemeManager::instance().loadSaved();
    mainWindow_ = std::make_unique<MainWindow>(*this);
    qDebug() << "[init] MainWindow created, showing";
    mainWindow_->show();
    qDebug() << "[init] MainWindow shown";
}

void OpenDawApplication::rebuildMainWindow()
{
    qDebug() << "[theme] rebuilding main window";
    if (mainWindow_) {
        mainWindow_->prepareForRebuild();
        mainWindow_.reset();
    }
    mainWindow_ = std::make_unique<MainWindow>(*this);
    mainWindow_->show();
    mainWindow_->finishRebuild();
}

} // namespace OpenDaw
