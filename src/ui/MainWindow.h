#pragma once

#include "app/OpenDawApplication.h"
#include "ui/timeline/TimelineView.h"
#include "ui/mixer/MixerView.h"
#include "ui/transport/TransportBar.h"
#include "ui/effects/EffectChainWidget.h"
#include "ui/browser/FileBrowserPanel.h"
#include "ui/routing/RoutingView.h"
#include "ai/AiChatWidget.h"
#include "ai/AiQuickPrompt.h"
#include "ai/AiHandSplitter.h"
#include <QProgressDialog>
#include <QMainWindow>
#include <QDockWidget>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QLabel>
#include <QTimer>

namespace OpenDaw {

class PianoRollEditor;
class AudioClipEditor;
class SheetMusicView;
class VideoPlayerWidget;
class LooperWindow;
class ChannelRackView;
class TypingKeyboard;

class ShortcutFilter : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(OpenDawApplication& app, QWidget* parent = nullptr);
    ~MainWindow() override;

    void loadFile(const QString& path);

    // Window rebuild (theme change): keep what lives in the window across it.
    void prepareForRebuild();
    void finishRebuild();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    bool maybeSaveBeforeAction();
    void addThemeMenu(QMenu* parent);
    void addRecentProject(const QString& path);
    void rebuildRecentMenu();
    void openRecentProject(const QString& path);
    void onImportAudio();
    void onImportMidi();
    void importAudioFiles(const QStringList& paths);
    void importMidiFile(const QString& path);
    void importBrowserFile(const QString& path);
    // Pattern workflow
    void placeCurrentPattern();
    void focusPlaylist();
    // F-key panel switching: shows and raises the panel, or hides it when it
    // is already in front.
    void togglePanel(QDockWidget* dock);
    void copyMcpSetupCommand();
    void updateAgentApiLabel(int clients);
    QWidget* createPlaylistPanel();
    void restoreWindowLayout();
    void saveWindowLayout();
    void fixDockTabBars();
    void dropStaleEditorClips();
    void updateWindowTitle();
    void updateStatusBar();
    void createMenus();
    void createToolBar();
    void createDocks();
    void createStatusBar();
    void applyGlobalStyle();

    void onNewProject();
    void onOpenProject();
    void onSaveProject();
    void onSaveProjectAs();
    void onExportAudio();
    void onAddTrack();
    void onAddMidiTrack();
    void onRemoveTrack();
    void onSplitClipAtPlayhead();
    void onScanVstPlugins();
    void onClearBlacklistAndRescan();
    void connectPluginScanner();
    // Picks Surge XT for a track; its first scan runs off the GUI thread.
    void chooseSurgeXt(te::EditItemID trackId);
    void onEffectInsertRequested(te::AudioTrack* track, int slotIndex);
    void onInstrumentSelectRequested(te::AudioTrack* track);
    void onMidiClipDoubleClicked(te::MidiClip* clip);
    // Piano roll <-> channels and patterns (FL Studio style).
    // Shows a clip; a pattern clip gets the "Pattern: ... Channel: ..." header.
    void showMidiClipInPianoRoll(te::MidiClip* clip);
    // Shows a channel of the current pattern ("Pattern 1" is made when the
    // Edit has none). createClip: make the placement clip now (an explicit
    // "edit in piano roll"); otherwise the clip is made by the first note.
    void openPianoRollForChannel(te::AudioTrack* track, bool createClip, bool raiseDock);
    // A track was selected anywhere: it becomes the piano roll's preview
    // target; from the Channel Rack it also retargets a piano roll in front.
    void onTrackSelectedForPianoRoll(te::AudioTrack* track, bool fromChannelRack);
    void refreshPianoRollPattern();
    void onCurrentPatternChangedForPianoRoll(int patternId);
    bool isPianoRollInFront() const;
    bool isPatternChannel(te::AudioTrack* track) const;
    bool pianoRollShowsChannel(te::AudioTrack* track) const;
    te::MidiClip* findPatternClip(int patternId, te::AudioTrack* track) const;
    void onAudioClipDoubleClicked(te::WaveAudioClip* clip);
    void onSplitPianoHands(te::AudioTrack* track);
    void onHandSplitFinished(const QVector<bool>& isLeftHand, int fallbackNotes);
    void closeHandSplitProgress();

private slots:
    // ChannelRackView::pianoRollRequested (double-click / "Edit in Piano Roll").
    void onChannelPianoRollRequested(te::AudioTrack* track);

private:

    OpenDawApplication& app_;
    EditManager& editMgr_;

    TransportBar* transportBar_       = nullptr;
    TimelineView* timelineView_       = nullptr;
    MixerView*    mixerView_          = nullptr;
    EffectChainWidget* effectChain_   = nullptr;
    FileBrowserPanel*  fileBrowser_   = nullptr;
    PianoRollEditor*   pianoRoll_     = nullptr;
    AudioClipEditor*   audioClipEditor_ = nullptr;
    RoutingView*       routingView_   = nullptr;
    SheetMusicView*    sheetMusicView_ = nullptr;
    AiChatWidget*      aiChatWidget_  = nullptr;
    AiQuickPrompt*     aiQuickPrompt_ = nullptr;
    AiHandSplitter*    handSplitter_ = nullptr;
    QMenu*             recentMenu_ = nullptr;
    QProgressDialog*   handSplitProgress_ = nullptr;
    VideoPlayerWidget* videoPlayer_   = nullptr;
    ChannelRackView*   channelRack_   = nullptr;
    QWidget*           playlistPanel_ = nullptr;

    QDockWidget* mixerDock_           = nullptr;
    QDockWidget* effectsDock_         = nullptr;
    QDockWidget* browserDock_         = nullptr;
    QDockWidget* pianoRollDock_       = nullptr;
    QDockWidget* audioClipDock_      = nullptr;
    QDockWidget* routingDock_         = nullptr;
    QDockWidget* sheetMusicDock_     = nullptr;
    QDockWidget* videoPlayerDock_    = nullptr;
    QDockWidget* aiDock_              = nullptr;
    QDockWidget* channelRackDock_     = nullptr;

    QToolBar* transportToolBar_       = nullptr;
    QToolBar* mainToolBar_            = nullptr;
    QAction* splitClipAction_         = nullptr;
    QActionGroup* editToolGroup_      = nullptr;
    QAction* rippleAction_            = nullptr;
    QAction* looperAction_            = nullptr;
    QAction* placePatternAction_      = nullptr;
    LooperWindow* looperWindow_       = nullptr;
    void showLooper(bool show);

    QLabel* sampleRateLabel_          = nullptr;
    QLabel* bufferLabel_              = nullptr;
    QLabel* cpuLabel_                 = nullptr;
    QLabel* agentApiLabel_            = nullptr;
    QTimer* statusBarTimer_           = nullptr;
    int scanFailures_                 = 0;     // plugins that failed in the running scan
    bool surgeScanRunning_            = false;
    TypingKeyboard* typingKeyboard_ = nullptr;   // computer keyboard -> selected channel
    QAction* typingKeyboardAction_ = nullptr;
    te::EditItemID selectedTrackId_;           // the last track selected anywhere
};

} // namespace OpenDaw
