#include "MainWindow.h"
#include "ui/TypingKeyboard.h"
#include "ui/effects/VstSelectorDialog.h"
#include "ui/effects/InstrumentMenu.h"
#include "engine/instruments/BundledPlugins.h"
#include "ui/effects/PluginEditorWindow.h"
#include "ui/pianoroll/PianoRollEditor.h"
#include "ui/audioclip/AudioClipEditor.h"
#include "ui/sheetmusic/SheetMusicView.h"
#include "ui/dialogs/ExportDialog.h"
#include "ui/dialogs/AudioSettingsDialog.h"
#include "ui/video/VideoPlayerWidget.h"
#include "ui/SplashScreen.h"
#include "ui/looper/LooperWindow.h"
#include "ui/channelrack/ChannelRackView.h"
#include "ui/toolbar/DockTitleBar.h"
#include "ui/toolbar/PatternSelector.h"
#include "engine/PatternManager.h"
#include "api/LocalApiServer.h"
#include "utils/ThemeManager.h"
#include "utils/IconFont.h"
#include <QFileDialog>
#include <QAction>
#include <QLabel>
#include <QKeySequence>
#include <QStyle>
#include <QSettings>
#include <QFileInfo>
#include <QDir>
#include <QMessageBox>
#include <QApplication>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QShortcutEvent>
#include <QTabBar>
#include <QActionGroup>
#include <QCursor>
#include <QPainter>
#include <QFontMetrics>
#include <QTimer>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <optional>
#include <cmath>
#include <QDebug>

namespace OpenDaw {

bool ShortcutFilter::eventFilter(QObject* /*obj*/, QEvent* event)
{
    if (event->type() != QEvent::Shortcut) return false;

    auto* se = static_cast<QShortcutEvent*>(event);
    auto seq = se->key();
    if (seq.count() != 1) return false;

    auto combo = seq[0];
    auto mods = combo.keyboardModifiers();
    if (mods != Qt::NoModifier && mods != Qt::ShiftModifier) return false;
    // Function keys (the F5-F9 panel keys) never type text, so they always work.
    if (combo.key() >= Qt::Key_F1 && combo.key() <= Qt::Key_F35) return false;

    auto* focus = QApplication::focusWidget();
    if (qobject_cast<QLineEdit*>(focus) ||
        qobject_cast<QTextEdit*>(focus) ||
        qobject_cast<QPlainTextEdit*>(focus)) {
        return true;
    }
    if (auto* cb = qobject_cast<QComboBox*>(focus))
        if (cb->isEditable()) return true;
    return false;
}

MainWindow::MainWindow(OpenDawApplication& app, QWidget* parent)
    : QMainWindow(parent), app_(app), editMgr_(app.editManager())
{
    auto* shortcutFilter = new ShortcutFilter(this);
    QApplication::instance()->installEventFilter(shortcutFilter);

    setWindowTitle("OpenDaw");
    setAccessibleName("OpenDaw Main Window");
    resize(1280, 800);
    setMinimumSize(900, 600);

    applyGlobalStyle();
    qDebug() << "[MainWindow] style applied";

    // Transport bar at the top
    transportBar_ = new TransportBar(&editMgr_, &app_.patternManager(), &app_.patternPlayer(), this);
    setMenuWidget(nullptr);

    // The timeline is the playlist: patterns and clips are arranged on it.
    timelineView_ = new TimelineView(&editMgr_, this);
    qDebug() << "[MainWindow] TimelineView created";
    playlistPanel_ = createPlaylistPanel();
    setCentralWidget(playlistPanel_);
    qDebug() << "[MainWindow] central widget set";

    // Connect snap mode from transport to timeline
    connect(transportBar_, &TransportBar::snapModeRequested,
            this, [this](int mode) {
                timelineView_->snapper().setMode(static_cast<SnapMode>(mode));
            });

    // Connect loop toggle from transport bar to timeline view
    connect(transportBar_, &TransportBar::loopToggled,
            timelineView_, &TimelineView::onLoopToggled);

    connect(timelineView_, &TimelineView::instrumentSelectRequested,
            this, &MainWindow::onInstrumentSelectRequested);

    connect(timelineView_, &TimelineView::trackSelected,
            this, [this](te::AudioTrack* track) {
                if (effectChain_)
                    effectChain_->setTrack(track);
                if (mixerView_)
                    mixerView_->setSelectedTrack(track);
                if (channelRack_)
                    channelRack_->setSelectedTrack(track);
                onTrackSelectedForPianoRoll(track, false);
            });

    connect(timelineView_, &TimelineView::selectedClipsDeleted,
            this, &MainWindow::dropStaleEditorClips);
    connect(timelineView_, &TimelineView::splitHandsRequested,
            this, &MainWindow::onSplitPianoHands);
    // Removing tracks (or an AI batch) deletes clips without an editChanged.
    connect(&editMgr_, &EditManager::tracksChanged,
            this, &MainWindow::dropStaleEditorClips);

    // VST editor windows keep their plugin alive; close them before the
    // plugin's Edit goes away or once the plugin is removed from it.
    connect(&editMgr_, &EditManager::aboutToChangeEdit,
            this, []() { PluginEditorWindow::closeAll(); });
    connect(&editMgr_, &EditManager::tracksChanged,
            this, []() { PluginEditorWindow::closeOrphaned(); });
    connect(&editMgr_, &EditManager::editChanged,
            this, []() { PluginEditorWindow::closeOrphaned(); });

    // Browser full height on the left, effects/AI full height on the right;
    // the channel rack and the mixer/piano roll tabs sit under the playlist.
    setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

    qDebug() << "[MainWindow] creating docks";
    createDocks();
    qDebug() << "[MainWindow] docks done";
    qDebug() << "[MainWindow] creating menus";
    createMenus();
    qDebug() << "[MainWindow] menus done";
    qDebug() << "[MainWindow] creating toolbar";
    createToolBar();
    qDebug() << "[MainWindow] toolbar done";
    createStatusBar();
    connectPluginScanner();

    resizeDocks({channelRackDock_, mixerDock_}, {230, 300}, Qt::Vertical);
    resizeDocks({browserDock_}, {240}, Qt::Horizontal);

    // Flat title bars (small capitals, float and close buttons) on every panel.
    for (auto* dock : findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly))
        DockTitleBar::install(dock);

    // saveState()/restoreState() match docks and toolbars by object name.
    channelRackDock_->setObjectName("ChannelRackDock");
    mixerDock_->setObjectName("MixerDock");
    pianoRollDock_->setObjectName("PianoRollDock");
    audioClipDock_->setObjectName("AudioClipDock");
    routingDock_->setObjectName("RoutingDock");
    sheetMusicDock_->setObjectName("SheetMusicDock");
    videoPlayerDock_->setObjectName("VideoPlayerDock");
    browserDock_->setObjectName("BrowserDock");
    effectsDock_->setObjectName("EffectsDock");
    aiDock_->setObjectName("AiDock");
    transportToolBar_->setObjectName("TransportToolBar");
    mainToolBar_->setObjectName("MainToolBar");
    restoreWindowLayout();

    connect(&editMgr_, &EditManager::editChanged, this, &MainWindow::updateWindowTitle);
    connect(&editMgr_, &EditManager::editChanged, this, [this]() {
        auto* edit = editMgr_.edit();
        if (!edit) return;
        juce::File vf = edit->getVideoFile();
        QString vpath = vf.existsAsFile()
            ? QString::fromStdString(vf.getFullPathName().toStdString())
            : QString();

        if (timelineView_ && timelineView_->videoLane()) {
            auto* lane = timelineView_->videoLane();
            if (!vpath.isEmpty() && lane->videoFilePath() != vpath)
                lane->loadVideo(vpath);
        }

        if (videoPlayer_)
            videoPlayer_->setVideoFile(vpath);
    });
    updateWindowTitle();

    aiQuickPrompt_ = new AiQuickPrompt(this);
    connect(aiQuickPrompt_, &AiQuickPrompt::promptSubmitted,
            this, [this](const QString& text) {
                if (aiChatWidget_) {
                    aiChatWidget_->submitPrompt(text);
                    aiDock_->setVisible(true);
                    aiDock_->raise();
                }
            });
}

MainWindow::~MainWindow()
{
    PluginEditorWindow::closeAll();
}

void MainWindow::dropStaleEditorClips()
{
    if (pianoRoll_)
        pianoRoll_->dropStaleClips();
    if (audioClipEditor_ && audioClipEditor_->clip()
        && !editMgr_.isClipValid(audioClipEditor_->clip()))
        audioClipEditor_->setClip(nullptr, nullptr);
    if (sheetMusicView_ && sheetMusicView_->clip()
        && !editMgr_.isClipValid(sheetMusicView_->clip()))
        sheetMusicView_->setClip(nullptr, nullptr);
}

bool MainWindow::maybeSaveBeforeAction()
{
    if (!editMgr_.edit() || !editMgr_.hasUnsavedChanges()) {
        editMgr_.clearAutosave();
        return true;
    }

    auto answer = QMessageBox::question(
        this, "Unsaved Changes",
        "Save changes to the current project?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Save)
        onSaveProject();
    else
        editMgr_.clearAutosave();
    return true;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (maybeSaveBeforeAction()) {
        saveWindowLayout();
        if (aiChatWidget_) aiChatWidget_->saveChatSessions();
        PluginEditorWindow::closeAll();
        editMgr_.stopAutosave();
        editMgr_.clearAutosave();
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::updateWindowTitle()
{
    QString title = "OpenDaw";
    auto currentFile = editMgr_.currentFile();
    if (currentFile != juce::File()) {
        auto name = QString::fromStdString(currentFile.getFileNameWithoutExtension().toStdString());
        title = QString("OpenDaw - %1").arg(name);
    } else {
        title = "OpenDaw - Untitled";
    }

    if (editMgr_.hasUnsavedChanges())
        title = "* " + title;

    setWindowTitle(title);
}

void MainWindow::updateStatusBar()
{
    auto& dm = app_.audioEngine().deviceManager();
    auto* device = dm.deviceManager.getCurrentAudioDevice();

    if (device) {
        double sr = device->getCurrentSampleRate();
        int buf = device->getCurrentBufferSizeSamples();
        sampleRateLabel_->setText(QString("%1 Hz").arg(static_cast<int>(sr)));
        bufferLabel_->setText(QString("%1 samples").arg(buf));
    }

    int cpuPercent = static_cast<int>(dm.getCpuUsage() * 100.0f);
    cpuLabel_->setText(QString("CPU: %1%").arg(cpuPercent));
}

void MainWindow::applyGlobalStyle()
{
    auto& theme = ThemeManager::instance().current();

    QString hoverBorder = QColor(theme.border).lighter(140).name();
    // Corner rounding is a theme choice: Studio Orange is nearly square.
    const QString r = QString::number(theme.cornerRadius);
    const QString rSmall = QString::number(std::max(0, theme.cornerRadius - 1));
    const QString selection = theme.selection.name();
    const QString selectionText = ThemeManager::readableTextOn(theme.selection).name();

    QString globalSS = QString(
        "QMainWindow { background: %1; }"
        "QWidget { background: %1; color: %3; }"

        "QMenuBar { background: %2; color: %3; border-bottom: 1px solid %4; }"
        "QMenuBar::item { background: transparent; padding: 4px 10px; border-radius: " + r + "px; }"
        "QMenuBar::item:selected { background: " + selection + "; color: " + selectionText + "; }"
        "QMenu { background: %2; color: %3; border: 1px solid %4; border-radius: " + r + "px; "
        "  padding: 4px 0; }"
        "QMenu::item { padding: 5px 24px; }"
        "QMenu::item:selected { background: " + selection + "; color: " + selectionText + "; "
        "  border-radius: " + rSmall + "px; margin: 0 4px; }"
        "QMenu::item:disabled { color: %7; }"
        "QMenu::separator { height: 1px; background: %4; margin: 4px 8px; }"
        "QMenu::section { color: %7; font-size: 10px; font-weight: bold; padding: 4px 12px; }"

        "QToolBar { background: %2; border: none; border-bottom: 1px solid %8; "
        "  spacing: 2px; padding: 2px 6px; }"
        "QToolBar::separator { background: %8; width: 1px; margin: 4px 6px; }"
        "QToolButton { background: transparent; color: %3; border: 1px solid transparent; "
        "  border-radius: " + r + "px; padding: 3px 5px; }"
        "QToolButton:hover { background: %6; border: 1px solid %4; }"
        "QToolButton:pressed { background: %1; }"
        "QToolButton:checked { background: %9; border: 1px solid transparent; }"

        "QMainWindow::separator { background: %8; width: 3px; height: 3px; }"
        "QMainWindow::separator:hover { background: %5; }"

        "QDockWidget { background: %1; color: %3; font-size: 11px; }"
        "QDockWidget::title { background: " + theme.dockTitle.name() + "; padding: 4px 8px; "
        "  color: " + theme.dockTitleText.name() + "; border: none; border-bottom: 1px solid %8; "
        "  font-weight: bold; }"
        "QDockWidget > QWidget { background: %1; }"

        "QTabBar { background: " + theme.dockTitle.name() + "; font-size: 11px; }"
        "QTabBar::tab { background: " + theme.dockTitle.name() + "; color: %7; border: none; "
        "  border-top: 2px solid transparent; padding: 5px 14px; margin-right: 1px; }"
        "QTabBar::tab:selected { background: %1; color: %3; border-top: 2px solid %5; }"
        "QTabBar::tab:hover:!selected { background: %6; color: %3; }"

        "QStatusBar { background: %2; color: %7; border-top: 1px solid %4; }"

        "QScrollArea { background: %1; border: none; }"
        "QScrollArea > QWidget > QWidget { background: %1; }"
        "QScrollBar:horizontal { background: %1; height: 10px; border: none; }"
        "QScrollBar::handle:horizontal { background: %4; border-radius: " + rSmall + "px; min-width: 24px; }"
        "QScrollBar::handle:horizontal:hover { background: %6; }"
        "QScrollBar:vertical { background: %1; width: 10px; border: none; }"
        "QScrollBar::handle:vertical { background: %4; border-radius: " + rSmall + "px; min-height: 24px; }"
        "QScrollBar::handle:vertical:hover { background: %6; }"
        "QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background: %1; }"

        "QSpinBox, QDoubleSpinBox { background: %1; color: %3; border: 1px solid %4; "
        "  border-radius: " + r + "px; padding: 3px 6px; min-height: 20px; }"
        "QSpinBox:hover, QDoubleSpinBox:hover { border-color: " + hoverBorder + "; }"
        "QSpinBox::up-button, QDoubleSpinBox::up-button { width: 16px; }"
        "QSpinBox::down-button, QDoubleSpinBox::down-button { width: 16px; }"

        "QComboBox { background: %1; color: %3; border: 1px solid %4; "
        "  border-radius: " + r + "px; padding: 3px 8px; min-height: 20px; }"
        "QComboBox:hover { border-color: " + hoverBorder + "; }"
        "QComboBox::drop-down { border: none; width: 20px; }"
        "QComboBox::down-arrow { image: url(" + ThemeManager::dropDownArrowPath(theme.textDim)
            + "); width: 9px; height: 6px; }"
        "QComboBox::down-arrow:disabled { image: url("
            + ThemeManager::dropDownArrowPath(theme.border) + "); }"
        "QComboBox QAbstractItemView { background: %2; color: %3; "
        "  selection-background-color: " + selection + "; selection-color: " + selectionText + "; "
        "  border: 1px solid %4; border-radius: " + r + "px; }"

        "QLabel { background: transparent; }"
        "QFrame { background: %1; }"

        "QHeaderView { background: %2; color: %3; border: none; }"
        "QHeaderView::section { background: %2; color: %3; border: 1px solid %4; "
        "  padding: 3px 6px; }"

        "QGraphicsView { background: %1; border: none; }"
        "QSplitter::handle { background: %4; }"

        "QPushButton { background: %2; color: %3; border: 1px solid %4; "
        "  border-radius: " + r + "px; padding: 4px 10px; }"
        "QPushButton:hover { background: %6; border-color: " + hoverBorder + "; }"
        "QPushButton:pressed { background: %5; }"
        "QPushButton:disabled { color: %7; }"

        "QLineEdit { background: %1; color: %3; border: 1px solid %4; "
        "  border-radius: " + r + "px; padding: 3px 4px; }"
        "QLineEdit:focus { border-color: %5; }"
    ).arg(
        theme.background.name(),  // %1
        theme.surface.name(),     // %2
        theme.text.name(),        // %3
        theme.border.name(),      // %4
        theme.accent.name(),      // %5
        theme.surfaceLight.name(),// %6
        theme.textDim.name(),     // %7
        theme.separator.name(),   // %8
        QString("rgba(%1,%2,%3,45)").arg(theme.accent.red()).arg(theme.accent.green())
            .arg(theme.accent.blue())   // %9
    );

    setStyleSheet(globalSS);
    qApp->setStyleSheet(
        QString("QToolTip { background: %1; color: %2; border: 1px solid %3; "
                "padding: 3px; font-size: 11px; }")
            .arg(theme.surfaceLight.name(), theme.text.name(), theme.border.name()));
}

void MainWindow::showLooper(bool show)
{
    if (!looperWindow_) {
        if (!show) return;
        looperWindow_ = new LooperWindow(editMgr_, this);
        connect(looperWindow_, &LooperWindow::visibilityChanged, this, [this](bool visible) {
            if (looperAction_) looperAction_->setChecked(visible);
        });
    }
    if (show) {
        looperWindow_->show();
        looperWindow_->raise();
        looperWindow_->activateWindow();
    } else {
        looperWindow_->close();
    }
}

void MainWindow::createMenus()
{
    auto* menuBar = new QMenuBar(this);
    setMenuBar(menuBar);

    // File menu
    auto* fileMenu = menuBar->addMenu("&File");

    fileMenu->addAction("&New Project", QKeySequence::New, this,
        &MainWindow::onNewProject);

    fileMenu->addAction("&Open Project...", QKeySequence::Open, this,
        &MainWindow::onOpenProject);

    recentMenu_ = fileMenu->addMenu("Open &Recent");
    recentMenu_->setAccessibleName("Recent Projects");
    recentMenu_->setToolTipsVisible(true);
    connect(recentMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentMenu);
    rebuildRecentMenu();

    fileMenu->addAction("&Save", QKeySequence::Save, this,
        &MainWindow::onSaveProject);

    fileMenu->addAction("Save &As...", QKeySequence("Ctrl+Shift+S"), this,
        &MainWindow::onSaveProjectAs);

    fileMenu->addSeparator();
    fileMenu->addAction("&Export Audio...", QKeySequence("Ctrl+Shift+E"), this,
        &MainWindow::onExportAudio);

    fileMenu->addSeparator();
    fileMenu->addAction("Import &Audio...", QKeySequence("Ctrl+I"), this,
        &MainWindow::onImportAudio);
    fileMenu->addAction("Import &MIDI...", QKeySequence("Ctrl+Shift+I"), this,
        &MainWindow::onImportMidi);
    fileMenu->addAction("Import &Video...", this, [this]() {
        QString path = QFileDialog::getOpenFileName(
            this, "Import Video", {},
            "Video Files (*.mp4 *.mkv *.avi *.mov *.webm *.wmv);;All Files (*)");
        if (path.isEmpty()) return;

        if (timelineView_ && timelineView_->videoLane()) {
            if (timelineView_->videoLane()->loadVideo(path)) {
                if (!timelineView_->videoLane()->isVisible())
                    timelineView_->toggleVideoLane();
            }
        }

        if (videoPlayer_) {
            videoPlayer_->setVideoFile(path);
            if (videoPlayerDock_ && !videoPlayerDock_->isVisible()) {
                videoPlayerDock_->setVisible(true);
                videoPlayerDock_->raise();
            }
        }
    });

    fileMenu->addSeparator();
    fileMenu->addAction("&Quit", QKeySequence::Quit, this, &QMainWindow::close);

    // Edit menu
    auto* editMenu = menuBar->addMenu("&Edit");

    auto* undoMenuAction = editMenu->addAction("&Undo\tCtrl+Z", this, [this]() { editMgr_.undo(); });
    auto* redoMenuAction = editMenu->addAction("&Redo\tCtrl+Y", this, [this]() { editMgr_.redo(); });
    connect(editMenu, &QMenu::aboutToShow, this, [this, undoMenuAction, redoMenuAction]() {
        auto* edit = editMgr_.edit();
        undoMenuAction->setEnabled(edit && edit->getUndoManager().canUndo());
        redoMenuAction->setEnabled(edit && edit->getUndoManager().canRedo());
    });
    editMenu->addSeparator();
    editMenu->addAction("&Delete\tDel", this, [this]() {
        if (timelineView_) timelineView_->deleteSelection(rippleEditing());
    });
    editMenu->addAction("Ripple Delete\tShift+Del", this, [this]() {
        if (timelineView_) timelineView_->deleteSelection(true);
    });
    editMenu->addAction("Select &All Clips\tCtrl+A", this, [this]() {
        if (timelineView_) timelineView_->selectAllClips();
    });
    auto* afterPlayhead = editMenu->addAction("Select Clips After &Playhead",
        QKeySequence("Ctrl+Shift+End"), this, [this]() {
            if (timelineView_) timelineView_->selectClipsAfterPlayhead();
        });
    afterPlayhead->setShortcutContext(Qt::WindowShortcut);
    editMenu->addAction("Nudge Left\tAlt+Left", this, [this]() {
        if (timelineView_) timelineView_->nudgeSelection(-1, false);
    });
    editMenu->addAction("Nudge Right\tAlt+Right", this, [this]() {
        if (timelineView_) timelineView_->nudgeSelection(1, false);
    });

    // Edit tools: shared by this menu and the toolbar.
    auto* toolMenu = editMenu->addMenu("Edit T&ool");
    editToolGroup_ = new QActionGroup(this);
    editToolGroup_->setExclusive(true);
    const struct { EditTool tool; const char* label; const char* tip; } kTools[] = {
        { EditTool::Smart,  "Smart Tool\t1",
          "Smart tool (1): move clips, trim their edges, drag fades, drag the dB readout "
          "for clip gain, Alt+drag to slip audio inside a clip, drag empty space to select a range" },
        { EditTool::Range,  "Range Tool\t2",
          "Range tool (2): select a time range across tracks, then Delete, Shift+Delete "
          "(ripple), S (split), copy, cut or right-click to loop it" },
        { EditTool::Razor,  "Razor Tool\t3",
          "Razor tool (3): click a clip to cut it there; Shift+click cuts every track" },
        { EditTool::Pen,    "Pen Tool\t4", "Pen tool (4): drag on a MIDI track to draw a clip" },
        { EditTool::Eraser, "Eraser Tool\t5", "Eraser tool (5): click a clip to delete it" },
    };
    for (const auto& t : kTools) {
        auto* a = toolMenu->addAction(t.label);
        a->setCheckable(true);
        a->setChecked(t.tool == activeEditTool());
        a->setData(int(t.tool));
        a->setToolTip(t.tip);
        editToolGroup_->addAction(a);
    }
    connect(editToolGroup_, &QActionGroup::triggered, this, [this](QAction* a) {
        if (timelineView_) timelineView_->setEditTool(static_cast<EditTool>(a->data().toInt()));
    });
    if (timelineView_) {
        // Number keys in the timeline change the tool too.
        connect(timelineView_, &TimelineView::editToolChanged, this, [this](EditTool tool) {
            for (auto* a : editToolGroup_->actions())
                if (a->data().toInt() == int(tool))
                    a->setChecked(true);
        });
    }
    toolMenu->addSeparator();
    rippleAction_ = toolMenu->addAction("&Ripple Editing");
    rippleAction_->setCheckable(true);
    rippleAction_->setChecked(rippleEditing());
    rippleAction_->setToolTip("Ripple editing: deleting clips or a range closes the gap, "
                              "moving later clips on those tracks left");
    connect(rippleAction_, &QAction::toggled, this, [](bool on) { setRippleEditing(on); });
    editMenu->addSeparator();

    editMenu->addAction("Add Audio &Track", QKeySequence("Ctrl+T"), this,
        &MainWindow::onAddTrack);

    editMenu->addAction("Add &MIDI Track", QKeySequence("Ctrl+Shift+T"), this,
        &MainWindow::onAddMidiTrack);

    editMenu->addAction("&Remove Selected Track", this,
        &MainWindow::onRemoveTrack);
    editMenu->addAction("Split Piano &Hands (AI)...", this, [this]() {
        onSplitPianoHands(timelineView_ ? timelineView_->selectedTrack() : nullptr);
    });

    editMenu->addSeparator();
    editMenu->addAction("&Copy", QKeySequence::Copy, this, [this]() {
        if (timelineView_) timelineView_->copySelectedClips();
    });
    editMenu->addAction("Cu&t", QKeySequence::Cut, this, [this]() {
        if (timelineView_) timelineView_->cutSelectedClips();
    });
    editMenu->addAction("&Paste", QKeySequence::Paste, this, [this]() {
        if (timelineView_) timelineView_->pasteClips();
    });

    editMenu->addSeparator();
    addThemeMenu(editMenu);
    editMenu->addAction("AI &Preferences...", this, [this]() {
        if (aiChatWidget_)
            aiChatWidget_->openSettings();
    });
    editMenu->addSeparator();
    editMenu->addAction("&Audio Settings...", this, [this]() {
        AudioSettingsDialog dlg(app_.audioEngine(), this);
        dlg.exec();
        updateStatusBar();
    });
    editMenu->addSeparator();
    editMenu->addAction("Scan &VST Plugins...", this,
        &MainWindow::onScanVstPlugins);
    auto* clearBlacklistAction = editMenu->addAction("Clear Plugin &Blacklist and Rescan", this,
        &MainWindow::onClearBlacklistAndRescan);
    clearBlacklistAction->setToolTip("Forgets the plugins that crashed or hung a scan and "
                                     "scans every plugin again");
    editMenu->setToolTipsVisible(true);

    splitClipAction_ = new QAction(style()->standardIcon(QStyle::SP_ArrowRight),
                                   "Split Clip", this);
    splitClipAction_->setShortcut(QKeySequence(Qt::Key_S));
    splitClipAction_->setShortcutContext(Qt::WindowShortcut);
    splitClipAction_->setToolTip("Split selected clip at playhead (S)");
    connect(splitClipAction_, &QAction::triggered,
            this, &MainWindow::onSplitClipAtPlayhead);
    addAction(splitClipAction_);
    editMenu->addAction(splitClipAction_);

    // View menu
    auto* viewMenu = menuBar->addMenu("&View");

    // Panel keys: show and raise a panel, or hide it when it is already in front.
    viewMenu->addSection("Panels");
    auto addPanelKey = [this, viewMenu](const QString& text, const QKeySequence& key,
                                        std::function<void()> fn) {
        auto* a = viewMenu->addAction(text, this, fn);
        a->setShortcut(key);
        a->setShortcutContext(Qt::WindowShortcut);
        addAction(a);
        return a;
    };
    addPanelKey("Pla&ylist", QKeySequence(Qt::Key_F5), [this]() { focusPlaylist(); });
    addPanelKey("C&hannel Rack", QKeySequence(Qt::Key_F6),
                [this]() { togglePanel(channelRackDock_); });
    addPanelKey("Piano Ro&ll", QKeySequence(Qt::Key_F7), [this]() {
        // Opening it shows the selected channel in the current pattern
        // (unless it already shows that channel).
        if (!isPianoRollInFront()) {
            te::AudioTrack* track = editMgr_.findAudioTrack(selectedTrackId_);
            if (!track && timelineView_) track = timelineView_->selectedTrack();
            if (!track) {
                // Like FL Studio, some channel is always the target.
                const auto chans = app_.patternManager().channels();
                if (!chans.isEmpty() && !pianoRoll_->clip() && !pianoRoll_->isShowingPattern())
                    track = chans.first();
            }
            if (track && isPatternChannel(track) && !pianoRollShowsChannel(track))
                openPianoRollForChannel(track, false, false);
        }
        togglePanel(pianoRollDock_);
    });
    addPanelKey("Bro&wser", QKeySequence(Qt::Key_F8), [this]() { togglePanel(browserDock_); });
    addPanelKey("Mi&xer", QKeySequence(Qt::Key_F9), [this]() { togglePanel(mixerDock_); });
    viewMenu->addSeparator();

    viewMenu->addAction("Zoom &In", QKeySequence("Ctrl+="), timelineView_,
        &TimelineView::zoomIn);
    viewMenu->addAction("Zoom &Out", QKeySequence("Ctrl+-"), timelineView_,
        &TimelineView::zoomOut);
    // Track height (vertical zoom); Alt + mouse wheel over the playlist too.
    viewMenu->addAction("Taller &Tracks (or Alt+Wheel)", QKeySequence("Ctrl+Shift+Up"), timelineView_,
        &TimelineView::zoomVerticalIn);
    viewMenu->addAction("Shorter Trac&ks (or Alt+Wheel)", QKeySequence("Ctrl+Shift+Down"), timelineView_,
        &TimelineView::zoomVerticalOut);
    viewMenu->addSeparator();
    // A dock's toggleViewAction stays checked while the dock is open, even when
    // it sits behind another tab, so the ticks show what is actually open.
    auto addDockToggle = [this, viewMenu](QDockWidget* dock, const QString& text) {
        auto* action = dock->toggleViewAction();
        action->setText(text);
        connect(action, &QAction::triggered, dock, [dock](bool shown) {
            if (shown) dock->raise();
        });
        viewMenu->addAction(action);
        return action;
    };
    addDockToggle(channelRackDock_, "Show Channel Rac&k");
    addDockToggle(mixerDock_, "&Mixer");
    addDockToggle(pianoRollDock_, "&Piano Roll");
    addDockToggle(audioClipDock_, "Audio &Clip Editor");
    addDockToggle(routingDock_, "&Routing");
    addDockToggle(sheetMusicDock_, "&Sheet Music");
    addDockToggle(videoPlayerDock_, "Video &Player");
    viewMenu->addSeparator();
    addDockToggle(browserDock_, "&Browser");
    addDockToggle(effectsDock_, "&Effects");
    auto* aiToggle = addDockToggle(aiDock_, "&AI Assistant");
    connect(aiToggle, &QAction::triggered, this, [this](bool shown) {
        if (shown) aiChatWidget_->focusInput();
    });
    viewMenu->addSeparator();
    auto* markerLaneAction = viewMenu->addAction("Marker/&Tempo Lane", this, [this]() {
        if (timelineView_) timelineView_->toggleMarkerTempoLane();
    });
    markerLaneAction->setCheckable(true);
    auto* videoLaneAction = viewMenu->addAction("&Video Track", this, [this]() {
        if (timelineView_) timelineView_->toggleVideoLane();
    });
    videoLaneAction->setCheckable(true);
    connect(viewMenu, &QMenu::aboutToShow, this, [this, markerLaneAction, videoLaneAction]() {
        markerLaneAction->setChecked(timelineView_ && timelineView_->isMarkerTempoLaneVisible());
        videoLaneAction->setChecked(timelineView_ && timelineView_->isVideoLaneVisible());
    });
    viewMenu->addSeparator();
    looperAction_ = viewMenu->addAction("&Looper", QKeySequence("Ctrl+L"), this,
        [this](bool on) { showLooper(on); });
    looperAction_->setCheckable(true);
    looperAction_->setToolTip("Looper (Ctrl+L): loop pads that record onto the timeline");
    looperAction_->setShortcutContext(Qt::WindowShortcut);
    addAction(looperAction_);
    auto* quickPromptAction = viewMenu->addAction("AI &Quick Prompt",
        QKeySequence("Ctrl+Shift+Space"), this, [this]() {
            if (aiQuickPrompt_)
                aiQuickPrompt_->showCentered();
        });
    quickPromptAction->setShortcutContext(Qt::WindowShortcut);
    addAction(quickPromptAction);

    // Pattern menu
    auto* patternMenu = menuBar->addMenu("Pa&ttern");
    auto* modeGroup = new QActionGroup(patternMenu);
    modeGroup->setExclusive(true);
    auto* patternModeAction = patternMenu->addAction("&Pattern Mode", this, [this]() {
        app_.patternManager().setMode(PatternManager::Mode::Pattern);
    });
    patternModeAction->setCheckable(true);
    patternModeAction->setToolTip("Play loops the current pattern");
    modeGroup->addAction(patternModeAction);
    auto* songModeAction = patternMenu->addAction("&Song Mode", this, [this]() {
        app_.patternManager().setMode(PatternManager::Mode::Song);
    });
    songModeAction->setCheckable(true);
    songModeAction->setToolTip("Play plays the playlist");
    modeGroup->addAction(songModeAction);
    patternMenu->addSeparator();
    patternMenu->addAction("&New Pattern", this, [this]() {
        if (auto* sel = transportBar_ ? transportBar_->patternSelector() : nullptr) sel->newPattern();
    });
    auto* cloneAction = patternMenu->addAction("&Clone Pattern", this, [this]() {
        if (auto* sel = transportBar_ ? transportBar_->patternSelector() : nullptr)
            sel->cloneCurrentPattern();
    });
    auto* renameAction = patternMenu->addAction("&Rename Pattern...", this, [this]() {
        if (auto* sel = transportBar_ ? transportBar_->patternSelector() : nullptr)
            sel->renameCurrentPattern();
    });
    auto* deleteAction = patternMenu->addAction("&Delete Pattern...", this, [this]() {
        if (auto* sel = transportBar_ ? transportBar_->patternSelector() : nullptr)
            sel->deleteCurrentPattern();
    });
    patternMenu->addSeparator();
    auto* prevAction = patternMenu->addAction("P&revious Pattern", this, [this]() {
        auto& pm = app_.patternManager();
        const auto all = pm.patterns();
        for (int i = 1; i < all.size(); ++i)
            if (all[i].id == pm.currentPattern()) { pm.setCurrentPattern(all[i - 1].id); return; }
    });
    auto* nextAction = patternMenu->addAction("Ne&xt Pattern", this, [this]() {
        auto& pm = app_.patternManager();
        const auto all = pm.patterns();
        for (int i = 0; i + 1 < all.size(); ++i)
            if (all[i].id == pm.currentPattern()) { pm.setCurrentPattern(all[i + 1].id); return; }
    });
    patternMenu->addSeparator();
    placePatternAction_ = patternMenu->addAction(
        icons::glyphIcon(icons::materialIcons(18), icons::mi::PlaylistAdd,
                         ThemeManager::instance().current().accentLight, 18),
        "&Place Current Pattern in Playlist", this, [this]() { placeCurrentPattern(); });
    placePatternAction_->setShortcut(QKeySequence("Ctrl+P"));
    placePatternAction_->setShortcutContext(Qt::WindowShortcut);
    placePatternAction_->setToolTip("Place the current pattern in the playlist at the "
                                    "bar under the playhead (Ctrl+P)");
    addAction(placePatternAction_);
    connect(patternMenu, &QMenu::aboutToShow, this,
            [this, patternModeAction, songModeAction, cloneAction, renameAction, deleteAction,
             prevAction, nextAction]() {
        auto& pm = app_.patternManager();
        const bool pattern = pm.mode() == PatternManager::Mode::Pattern;
        patternModeAction->setChecked(pattern);
        songModeAction->setChecked(!pattern);
        const auto all = pm.patterns();
        const bool has = pm.currentPattern() > 0 && pm.pattern(pm.currentPattern()).has_value();
        cloneAction->setEnabled(has);
        renameAction->setEnabled(has);
        deleteAction->setEnabled(has);
        prevAction->setEnabled(has && !all.isEmpty() && all.first().id != pm.currentPattern());
        nextAction->setEnabled(has && !all.isEmpty() && all.last().id != pm.currentPattern());
    });

    // Transport menu
    auto* transportMenu = menuBar->addMenu("&Transport");

    // Typing keyboard to piano (FL style): the computer keyboard plays the
    // selected channel/track from anywhere in the window.
    typingKeyboard_ = new TypingKeyboard(editMgr_, this);
    typingKeyboard_->setTarget(selectedTrackId_);
    typingKeyboardAction_ = transportMenu->addAction("&Typing Keyboard to Piano");
    typingKeyboardAction_->setCheckable(true);
    typingKeyboardAction_->setChecked(typingKeyboard_->isEnabled());
    typingKeyboardAction_->setToolTip(
        "Typing keyboard to piano: play the selected channel with the computer keyboard "
        "(bottom row Z-M and top row Q-P, by key position; numpad +/- change the octave)");
    connect(typingKeyboardAction_, &QAction::toggled, typingKeyboard_, &TypingKeyboard::setEnabled);
    connect(typingKeyboard_, &TypingKeyboard::octaveChanged, this, [this](int base) {
        statusBar()->showMessage(QString("Typing keyboard: lower row starts at C%1").arg(base / 12 - 1), 2500);
    });
    transportMenu->addSeparator();

    // Space plays in the current mode: the current pattern in Pattern mode,
    // the playlist in Song mode.
    transportMenu->addAction("&Play / Pause", QKeySequence("Space"), this, [this]() {
        if (transportBar_) {
            transportBar_->togglePlay();
            return;
        }
        auto& t = editMgr_.transport();
        if (t.isPlaying()) t.stop(false, false);
        else t.play(false);
    });

    transportMenu->addAction("&Stop", this, [this]() {
        if (transportBar_) {
            transportBar_->stopAll();
            return;
        }
        editMgr_.transport().stop(false, false);
        editMgr_.transport().setPosition(tracktion::TimePosition::fromSeconds(0));
    });

    transportMenu->addAction("&Record", QKeySequence("R"), this, [this]() {
        editMgr_.toggleRecording();
    });
    auto* punchAction = transportMenu->addAction("P&unch In/Out", this, [this](bool on) {
        editMgr_.setPunchEnabled(on);
    });
    punchAction->setCheckable(true);
    connect(transportMenu, &QMenu::aboutToShow, this, [this, punchAction]() {
        punchAction->setChecked(editMgr_.isPunchEnabled());
    });

    transportMenu->addSeparator();
    auto* panicAction = transportMenu->addAction("MIDI &Panic",
        QKeySequence("Ctrl+Shift+P"), this, [this]() {
            // Same as the PANIC button: the pattern player must stop too, or
            // its next step starts the notes again.
            if (transportBar_) {
                transportBar_->panic();
                return;
            }
            app_.patternPlayer().stop();
            editMgr_.midiPanic();
        });
    panicAction->setShortcutContext(Qt::WindowShortcut);
    addAction(panicAction);

    // Help menu
    auto* helpMenu = menuBar->addMenu("&Help");
    auto* mcpAction = helpMenu->addAction("Copy &MCP Setup Command", this,
                                          [this]() { copyMcpSetupCommand(); });
    mcpAction->setToolTip("Copies the command line an MCP client (an AI agent) runs to "
                          "control OpenDaw");
    helpMenu->addSeparator();
    helpMenu->addAction("&About OpenDaw", this, [this]() {
        auto* about = new SplashScreen(false, this);
        about->setAttribute(Qt::WA_DeleteOnClose);
        about->finish();
        about->show();
    });
}

void MainWindow::createToolBar()
{
    auto& theme = ThemeManager::instance().current();
    const QColor ic = theme.text;
    const QColor icAccent = theme.accentLight;
    const int sz = 18;
    const QFont faFont = icons::fontAudio(sz);
    const QFont miFont = icons::materialIcons(sz);

    auto faIcon = [&](const QChar& g) { return icons::glyphIcon(faFont, g, ic, sz); };
    auto miIcon = [&](const QChar& g) { return icons::glyphIcon(miFont, g, ic, sz); };
    auto faIconAccent = [&](const QChar& g) { return icons::glyphIcon(faFont, g, icAccent, sz); };

    transportToolBar_ = addToolBar("Transport");
    transportToolBar_->setAccessibleName("Transport Toolbar");
    transportToolBar_->setMovable(false);
    transportToolBar_->setFloatable(false);
    transportToolBar_->setIconSize(QSize(sz, sz));
    transportToolBar_->addWidget(transportBar_);

    // No toolbar break: the tools share the transport strip's row and fold
    // into the toolbar's overflow menu when the window is narrow.
    mainToolBar_ = addToolBar("Tools");
    mainToolBar_->setAccessibleName("Tools Toolbar");
    mainToolBar_->setMovable(false);
    mainToolBar_->setFloatable(false);
    mainToolBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    mainToolBar_->setIconSize(QSize(sz, sz));

    auto* newProjectAction = mainToolBar_->addAction(
        miIcon(icons::mi::InsertDriveFile), "New Project",
        this, &MainWindow::onNewProject);
    newProjectAction->setToolTip("New Project");

    auto* openProjectAction = mainToolBar_->addAction(
        faIcon(icons::fa::Open), "Open Project",
        this, &MainWindow::onOpenProject);
    openProjectAction->setToolTip("Open Project");

    auto* saveProjectAction = mainToolBar_->addAction(
        faIcon(icons::fa::Save), "Save Project",
        this, &MainWindow::onSaveProject);
    saveProjectAction->setToolTip("Save Project");

    mainToolBar_->addSeparator();
    if (typingKeyboardAction_) {
        typingKeyboardAction_->setIcon(faIcon(icons::fa::Keyboard));
        mainToolBar_->addAction(typingKeyboardAction_);
        mainToolBar_->addSeparator();
    }

    auto* undoAction = mainToolBar_->addAction(
        faIcon(icons::fa::Undo), "Undo",
        this, [this]() { editMgr_.undo(); });
    undoAction->setToolTip("Undo (Ctrl+Z)");
    undoAction->setShortcut(QKeySequence::Undo);
    undoAction->setShortcutContext(Qt::WindowShortcut);
    addAction(undoAction);

    auto* redoAction = mainToolBar_->addAction(
        faIcon(icons::fa::Redo), "Redo",
        this, [this]() { editMgr_.redo(); });
    redoAction->setToolTip("Redo (Ctrl+Y)");
    redoAction->setShortcut(QKeySequence::Redo);
    redoAction->setShortcutContext(Qt::WindowShortcut);
    addAction(redoAction);

    mainToolBar_->addSeparator();

    auto* addAudioTrackAction = mainToolBar_->addAction(
        faIconAccent(icons::fa::Waveform), "Add Audio Track",
        this, &MainWindow::onAddTrack);
    addAudioTrackAction->setToolTip("Add Audio Track");

    auto* addMidiTrackAction = mainToolBar_->addAction(
        faIconAccent(icons::fa::Keyboard), "Add MIDI Track",
        this, &MainWindow::onAddMidiTrack);
    addMidiTrackAction->setToolTip("Add MIDI Track");

    auto* removeTrackAction = mainToolBar_->addAction(
        miIcon(icons::mi::Delete), "Remove Track",
        this, &MainWindow::onRemoveTrack);
    removeTrackAction->setToolTip("Remove Track");

    mainToolBar_->addSeparator();

    if (splitClipAction_) {
        splitClipAction_->setIcon(faIcon(icons::fa::Scissors));
        splitClipAction_->setText("Split Clip");
        mainToolBar_->addAction(splitClipAction_);
    }

    // The tools share their actions (and checked state) with Edit > Edit Tool.
    if (editToolGroup_) {
        for (auto* a : editToolGroup_->actions()) {
            switch (static_cast<EditTool>(a->data().toInt())) {
                case EditTool::Smart:  a->setIcon(faIcon(icons::fa::Pointer)); break;
                case EditTool::Range:  a->setIcon(miIcon(icons::mi::HighlightAlt)); break;
                case EditTool::Razor:  a->setIcon(miIcon(icons::mi::ContentCut)); break;
                case EditTool::Pen:    a->setIcon(faIcon(icons::fa::Pen)); break;
                case EditTool::Eraser: a->setIcon(faIcon(icons::fa::Eraser)); break;
            }
            mainToolBar_->addAction(a);
        }
    }
    if (rippleAction_) {
        rippleAction_->setIcon(miIcon(icons::mi::SwapHoriz));
        mainToolBar_->addAction(rippleAction_);
    }

    mainToolBar_->addSeparator();

    auto* zoomInAction = mainToolBar_->addAction(
        faIcon(icons::fa::ZoomIn), "Zoom In",
        timelineView_, &TimelineView::zoomIn);
    zoomInAction->setToolTip("Zoom In");

    auto* zoomOutAction = mainToolBar_->addAction(
        faIcon(icons::fa::ZoomOut), "Zoom Out",
        timelineView_, &TimelineView::zoomOut);
    zoomOutAction->setToolTip("Zoom Out");

    mainToolBar_->addSeparator();

    auto* scanPluginsAction = mainToolBar_->addAction(
        miIcon(icons::mi::Refresh), "Scan VST Plugins",
        this, &MainWindow::onScanVstPlugins);
    scanPluginsAction->setToolTip("Scan VST Plugins");

    mainToolBar_->addSeparator();

    if (placePatternAction_)
        mainToolBar_->addAction(placePatternAction_);
    if (channelRackDock_) {
        auto* rackToggle = channelRackDock_->toggleViewAction();
        rackToggle->setIcon(miIcon(icons::mi::GridOn));
        rackToggle->setToolTip("Toggle Channel Rack (F6)");
        mainToolBar_->addAction(rackToggle);
    }
    if (looperAction_) {
        looperAction_->setIcon(faIcon(icons::fa::Drumpad));
        mainToolBar_->addAction(looperAction_);
    }
    if (mixerDock_) {
        auto* mixerToggle = mixerDock_->toggleViewAction();
        mixerToggle->setIcon(faIcon(icons::fa::Speaker));
        mixerToggle->setToolTip("Toggle Mixer (F9)");
        mainToolBar_->addAction(mixerToggle);
    }
    if (browserDock_) {
        auto* browserToggle = browserDock_->toggleViewAction();
        browserToggle->setIcon(miIcon(icons::mi::Folder));
        browserToggle->setToolTip("Toggle Browser (F8)");
        mainToolBar_->addAction(browserToggle);
    }
    if (effectsDock_) {
        auto* effectsToggle = effectsDock_->toggleViewAction();
        effectsToggle->setIcon(miIcon(icons::mi::Tune));
        effectsToggle->setToolTip("Toggle Effects");
        mainToolBar_->addAction(effectsToggle);
    }
    if (pianoRollDock_) {
        auto* pianoRollToggle = pianoRollDock_->toggleViewAction();
        pianoRollToggle->setIcon(faIcon(icons::fa::Keyboard));
        pianoRollToggle->setToolTip("Toggle Piano Roll (F7)");
        mainToolBar_->addAction(pianoRollToggle);
    }
    if (audioClipDock_) {
        auto* audioClipToggle = audioClipDock_->toggleViewAction();
        audioClipToggle->setIcon(faIcon(icons::fa::Waveform));
        audioClipToggle->setToolTip("Toggle Audio Clip Editor");
        mainToolBar_->addAction(audioClipToggle);
    }
    if (routingDock_) {
        auto* routingToggle = routingDock_->toggleViewAction();
        routingToggle->setIcon(miIcon(icons::mi::Settings));
        routingToggle->setToolTip("Toggle Routing");
        mainToolBar_->addAction(routingToggle);
    }
    if (sheetMusicDock_) {
        auto* sheetMusicToggle = sheetMusicDock_->toggleViewAction();
        sheetMusicToggle->setIcon(miIcon(icons::mi::LibraryMusic));
        sheetMusicToggle->setToolTip("Toggle Sheet Music");
        mainToolBar_->addAction(sheetMusicToggle);
    }
    {
        auto* markerTempoToggle = new QAction(miIcon(icons::mi::ShowChart), "Toggle Marker/Tempo Lane", this);
        markerTempoToggle->setToolTip("Toggle Marker/Tempo Lane");
        markerTempoToggle->setCheckable(true);
        connect(markerTempoToggle, &QAction::toggled, this, [this](bool) {
            if (timelineView_) timelineView_->toggleMarkerTempoLane();
        });
        mainToolBar_->addAction(markerTempoToggle);
    }
    if (aiDock_) {
        auto* aiToggle = aiDock_->toggleViewAction();
        aiToggle->setIcon(miIcon(icons::mi::Chat));
        aiToggle->setToolTip("Toggle AI Assistant");
        mainToolBar_->addAction(aiToggle);
    }
}

void MainWindow::createDocks()
{
    // Channel Rack (bottom, above the mixer / piano roll tabs)
    channelRackDock_ = new QDockWidget("Channel Rack", this);
    channelRackDock_->setAccessibleName("Channel Rack Dock");
    channelRack_ = new ChannelRackView(&editMgr_, &app_.patternManager(),
                                       &app_.patternPlayer(), channelRackDock_);
    channelRackDock_->setWidget(channelRack_);
    addDockWidget(Qt::BottomDockWidgetArea, channelRackDock_);

    connect(channelRack_, &ChannelRackView::trackSelected,
            this, [this](te::AudioTrack* track) {
                if (timelineView_)
                    timelineView_->setSelectedTrack(track);
                if (effectChain_)
                    effectChain_->setTrack(track);
                if (mixerView_)
                    mixerView_->setSelectedTrack(track);
                // After the timeline's fan-out (which also lands here, as
                // not-from-the-rack): retargets a piano roll in front.
                onTrackSelectedForPianoRoll(track, true);
            });
    // Double-click on a channel / "Edit in Piano Roll": that channel of the
    // current pattern (string connection: the rack's owner adds the signal).
    if (channelRack_->metaObject()->indexOfSignal(
            QMetaObject::normalizedSignature("pianoRollRequested(te::AudioTrack*)").constData()) >= 0)
        connect(channelRack_, SIGNAL(pianoRollRequested(te::AudioTrack*)),
                this, SLOT(onChannelPianoRollRequested(te::AudioTrack*)));
    channelRack_->setPluginScanner(&app_.pluginScanner());
    connect(channelRack_, &ChannelRackView::scanPluginsRequested, this, &MainWindow::onScanVstPlugins);
    connect(channelRack_, &ChannelRackView::instrumentSelectRequested,
            this, &MainWindow::onInstrumentSelectRequested);
    connect(channelRack_, &ChannelRackView::placePatternRequested,
            this, &MainWindow::placeCurrentPattern);
    connect(channelRack_, &ChannelRackView::statusMessage,
            this, [this](const QString& text, int timeoutMs) {
                statusBar()->showMessage(text, timeoutMs);
            });

    qDebug() << "[MainWindow] creating mixer dock";
    // Mixer dock (bottom, under the channel rack)
    mixerDock_ = new QDockWidget("Mixer", this);
    mixerDock_->setAccessibleName("Mixer Dock");
    mixerView_ = new MixerView(&editMgr_, mixerDock_);
    mixerDock_->setWidget(mixerView_);
    addDockWidget(Qt::BottomDockWidgetArea, mixerDock_);
    splitDockWidget(channelRackDock_, mixerDock_, Qt::Vertical);

    connect(mixerView_, &MixerView::effectInsertRequested,
            this, &MainWindow::onEffectInsertRequested);

    connect(mixerView_, &MixerView::instrumentSelectRequested,
            this, &MainWindow::onInstrumentSelectRequested);

    connect(mixerView_, &MixerView::trackSelected,
            this, [this](te::AudioTrack* track) {
                if (timelineView_)
                    timelineView_->setSelectedTrack(track);
                if (effectChain_)
                    effectChain_->setTrack(track);
                if (channelRack_)
                    channelRack_->setSelectedTrack(track);
                onTrackSelectedForPianoRoll(track, false);
            });

    connect(mixerView_, &MixerView::masterSelected,
            this, [this]() {
                if (timelineView_)
                    timelineView_->clearTrackSelection();
                if (channelRack_)
                    channelRack_->setSelectedTrack(nullptr);
                if (effectChain_) {
                    effectChain_->setMasterMode();
                    effectsDock_->setVisible(true);
                    effectsDock_->raise();
                }
            });
    qDebug() << "[MainWindow] mixer dock done";

    qDebug() << "[MainWindow] creating piano roll dock";
    // Piano Roll dock (bottom, tabbed with mixer)
    pianoRollDock_ = new QDockWidget("Piano Roll", this);
    pianoRollDock_->setAccessibleName("Piano Roll Dock");
    pianoRoll_ = new PianoRollEditor(pianoRollDock_);
    pianoRollDock_->setWidget(pianoRoll_);
    addDockWidget(Qt::BottomDockWidgetArea, pianoRollDock_);

    connect(pianoRoll_, &PianoRollEditor::notesChanged,
            timelineView_, &TimelineView::rebuildClips);

    connect(pianoRoll_, &PianoRollEditor::notesChanged,
            this, [this]() {
                if (sheetMusicView_ && sheetMusicView_->clip())
                    sheetMusicView_->refresh();
            });

    // A pattern channel with no placement yet gets its clip on the first note.
    pianoRoll_->setPatternClipFactory([this](int patternId, te::EditItemID trackId) {
        return app_.patternManager().editableClipFor(patternId, trackId);
    });

    connect(&editMgr_, &EditManager::aboutToChangeEdit, this, [this]() {
        selectedTrackId_ = {};
        if (pianoRoll_)
            pianoRoll_->clear();
    });

    connect(&editMgr_, &EditManager::editChanged, this, [this]() {
        // Drops a deleted clip (a pattern channel stays, empty) and redraws.
        if (pianoRoll_)
            pianoRoll_->refresh();
    });

    {
        auto& pm = app_.patternManager();
        connect(&pm, &PatternManager::currentPatternChanged,
                this, &MainWindow::onCurrentPatternChangedForPianoRoll);
        connect(&pm, &PatternManager::patternsChanged,
                this, &MainWindow::refreshPianoRollPattern);
        connect(&editMgr_, &EditManager::tracksChanged,
                this, &MainWindow::refreshPianoRollPattern);
        // An empty pattern channel (no clip yet) picks up a clip as soon as
        // there is one, or as soon as the channel gets notes (steps clicked
        // in the rack): the piano roll always shows the pattern's notes.
        // Queued: these arrive in the middle of pattern edits.
        auto reloadEmptyChannel = [this](int patternId) {
            QTimer::singleShot(0, this, [this, patternId]() {
                if (!pianoRoll_ || !pianoRoll_->isShowingPattern() || pianoRoll_->clip()) return;
                const auto ctx = pianoRoll_->patternContext();
                if (patternId > 0 && ctx.patternId != patternId) return;
                auto* track = editMgr_.findAudioTrack(ctx.trackId);
                if (!track || !app_.patternManager().patternExists(ctx.patternId)) return;
                if (findPatternClip(ctx.patternId, track)
                    || !app_.patternManager().channelNotes(ctx.patternId, ctx.trackId).isEmpty())
                    openPianoRollForChannel(track, false, false);
            });
        };
        connect(&pm, &PatternManager::patternChanged, this, reloadEmptyChannel);
        connect(&pm, &PatternManager::placementsChanged,
                this, [reloadEmptyChannel]() { reloadEmptyChannel(0); });
    }

    connect(&editMgr_, &EditManager::midiClipModified,
            this, [this](te::MidiClip* clip) {
                if (pianoRoll_ && pianoRoll_->isShowingClip(clip))
                    pianoRoll_->refresh();
            });

    connect(&editMgr_, &EditManager::midiClipSelected,
            this, [this](te::MidiClip* clip) {
                if (pianoRollDock_ && pianoRollDock_->isVisible() && pianoRoll_ && clip
                    && !pianoRoll_->isShowingClip(clip))
                    showMidiClipInPianoRoll(clip);
            });
    qDebug() << "[MainWindow] piano roll dock done";

    qDebug() << "[MainWindow] creating audio clip dock";
    // Audio Clip Editor dock (bottom, tabbed with mixer and piano roll)
    audioClipDock_ = new QDockWidget("Audio Clip", this);
    audioClipDock_->setAccessibleName("Audio Clip Editor Dock");
    audioClipEditor_ = new AudioClipEditor(audioClipDock_);
    audioClipDock_->setWidget(audioClipEditor_);
    addDockWidget(Qt::BottomDockWidgetArea, audioClipDock_);

    connect(audioClipEditor_, &AudioClipEditor::clipModified,
            timelineView_, &TimelineView::rebuildClips);

    connect(&editMgr_, &EditManager::aboutToChangeEdit, this, [this]() {
        if (audioClipEditor_)
            audioClipEditor_->setClip(nullptr, nullptr);
    });

    connect(&editMgr_, &EditManager::editChanged, this, [this]() {
        if (audioClipEditor_ && audioClipEditor_->clip()) {
            if (editMgr_.isClipValid(audioClipEditor_->clip()))
                audioClipEditor_->refresh();
            else
                audioClipEditor_->setClip(nullptr, nullptr);
        }
    });

    connect(&editMgr_, &EditManager::audioClipModified,
            this, [this](te::WaveAudioClip* clip) {
                if (audioClipEditor_ && audioClipEditor_->clip() == clip)
                    audioClipEditor_->refresh();
            });

    connect(&editMgr_, &EditManager::audioClipSelected,
            this, [this](te::WaveAudioClip* clip) {
                if (audioClipDock_ && audioClipDock_->isVisible() && audioClipEditor_ && clip)
                    audioClipEditor_->setClip(clip, &editMgr_);
            });
    qDebug() << "[MainWindow] audio clip dock done";

    qDebug() << "[MainWindow] creating routing dock";
    // Routing view dock (bottom, tabbed with mixer and piano roll)
    routingDock_ = new QDockWidget("Routing", this);
    routingDock_->setAccessibleName("Routing Dock");
    routingView_ = new RoutingView(&editMgr_, routingDock_);
    routingDock_->setWidget(routingView_);
    addDockWidget(Qt::BottomDockWidgetArea, routingDock_);

    connect(routingView_, &RoutingView::trackSelected,
            this, [this](te::AudioTrack* track) {
                if (timelineView_)
                    timelineView_->setSelectedTrack(track);
                if (effectChain_)
                    effectChain_->setTrack(track);
                if (mixerView_)
                    mixerView_->setSelectedTrack(track);
                if (channelRack_)
                    channelRack_->setSelectedTrack(track);
                onTrackSelectedForPianoRoll(track, false);
                if (effectsDock_) {
                    effectsDock_->setVisible(true);
                    effectsDock_->raise();
                }
            });

    connect(routingView_, &RoutingView::audioSettingsRequested, this, [this]() {
        AudioSettingsDialog dlg(app_.audioEngine(), this);
        dlg.exec();
        updateStatusBar();
        emit editMgr_.routingChanged();   // the outputs on offer may have changed
    });

    connect(routingView_, &RoutingView::masterSelected,
            this, [this]() {
                if (timelineView_)
                    timelineView_->clearTrackSelection();
                if (effectChain_) {
                    effectChain_->setMasterMode();
                    effectsDock_->setVisible(true);
                    effectsDock_->raise();
                }
                if (mixerView_)
                    mixerView_->setMasterSelected();
            });
    qDebug() << "[MainWindow] routing dock done";

    qDebug() << "[MainWindow] creating sheet music dock";
    // Sheet Music dock (bottom, tabbed after routing)
    sheetMusicDock_ = new QDockWidget("Sheet Music", this);
    sheetMusicDock_->setAccessibleName("Sheet Music Dock");
    sheetMusicView_ = new SheetMusicView(sheetMusicDock_);
    sheetMusicDock_->setWidget(sheetMusicView_);
    addDockWidget(Qt::BottomDockWidgetArea, sheetMusicDock_);

    connect(&editMgr_, &EditManager::aboutToChangeEdit, this, [this]() {
        if (sheetMusicView_)
            sheetMusicView_->setClip(nullptr, nullptr);
    });

    connect(&editMgr_, &EditManager::editChanged, this, [this]() {
        if (sheetMusicView_ && sheetMusicView_->clip()) {
            if (editMgr_.isClipValid(sheetMusicView_->clip()))
                sheetMusicView_->refresh();
            else
                sheetMusicView_->setClip(nullptr, nullptr);
        }
    });

    connect(&editMgr_, &EditManager::midiClipModified,
            this, [this](te::MidiClip* clip) {
                if (sheetMusicView_ && sheetMusicView_->clip() == clip)
                    sheetMusicView_->refresh();
            });

    connect(&editMgr_, &EditManager::midiClipSelected,
            this, [this](te::MidiClip* clip) {
                if (sheetMusicView_ && clip)
                    sheetMusicView_->setClip(clip, &editMgr_);
            });
    qDebug() << "[MainWindow] sheet music dock done";

    // Video Player dock (bottom, tabbed with others)
    videoPlayerDock_ = new QDockWidget("Video", this);
    videoPlayerDock_->setAccessibleName("Video Player Dock");
    videoPlayer_ = new VideoPlayerWidget(&editMgr_, videoPlayerDock_);
    videoPlayerDock_->setWidget(videoPlayer_);
    addDockWidget(Qt::BottomDockWidgetArea, videoPlayerDock_);
    qDebug() << "[MainWindow] video player dock done";

    tabifyDockWidget(mixerDock_, pianoRollDock_);
    tabifyDockWidget(pianoRollDock_, audioClipDock_);
    tabifyDockWidget(audioClipDock_, routingDock_);
    tabifyDockWidget(routingDock_, sheetMusicDock_);
    tabifyDockWidget(sheetMusicDock_, videoPlayerDock_);
    mixerDock_->raise();

    connect(&editMgr_, &EditManager::midiClipDoubleClicked,
            this, &MainWindow::onMidiClipDoubleClicked);

    connect(&editMgr_, &EditManager::audioClipDoubleClicked,
            this, &MainWindow::onAudioClipDoubleClicked);

    qDebug() << "[MainWindow] creating browser dock";
    // File browser dock (left, full height)
    browserDock_ = new QDockWidget("Browser", this);
    browserDock_->setAccessibleName("File Browser Dock");
    fileBrowser_ = new FileBrowserPanel(
        app_.audioEngine().engine().getDeviceManager().deviceManager, browserDock_);
    browserDock_->setWidget(fileBrowser_);
    addDockWidget(Qt::LeftDockWidgetArea, browserDock_);
    // Double-clicking a MIDI file drops it in at the playhead; a sample becomes
    // a Channel Rack sample channel (Shift: at the playhead), see importBrowserFile.
    connect(fileBrowser_, &FileBrowserPanel::fileDoubleClicked,
            this, &MainWindow::importBrowserFile);
    qDebug() << "[MainWindow] browser dock done";

    qDebug() << "[MainWindow] creating effects dock";
    // Effects dock (right, tabbed with browser)
    effectsDock_ = new QDockWidget("Effects", this);
    effectsDock_->setAccessibleName("Effects Dock");
    effectChain_ = new EffectChainWidget(&editMgr_, effectsDock_);
    effectChain_->setPluginList(&app_.pluginScanner().getPluginList());
    effectsDock_->setWidget(effectChain_);
    addDockWidget(Qt::RightDockWidgetArea, effectsDock_);
    qDebug() << "[MainWindow] effects dock done";

    qDebug() << "[MainWindow] creating AI dock";
    // AI assistant dock (right, tabbed with browser and effects)
    aiDock_ = new QDockWidget("AI", this);
    aiDock_->setAccessibleName("AI Assistant Dock");
    aiChatWidget_ = new AiChatWidget(&editMgr_, &app_.audioEngine(),
                                     &app_.pluginScanner(), aiDock_);
    aiDock_->setWidget(aiChatWidget_);
    addDockWidget(Qt::RightDockWidgetArea, aiDock_);
    qDebug() << "[MainWindow] AI dock done";

    tabifyDockWidget(effectsDock_, aiDock_);
    effectsDock_->raise();
}

void MainWindow::createStatusBar()
{
    auto* status = statusBar();
    auto& theme = ThemeManager::instance().current();

    auto labelStyle = QString("color: %1; font-size: 10px; padding: 0 8px;")
                          .arg(theme.textDim.name());

    sampleRateLabel_ = new QLabel("-- Hz", this);
    sampleRateLabel_->setAccessibleName("Sample Rate");
    sampleRateLabel_->setStyleSheet(labelStyle);

    bufferLabel_ = new QLabel("-- samples", this);
    bufferLabel_->setAccessibleName("Buffer Size");
    bufferLabel_->setStyleSheet(labelStyle);

    cpuLabel_ = new QLabel("CPU: 0%", this);
    cpuLabel_->setAccessibleName("CPU Usage");
    cpuLabel_->setStyleSheet(labelStyle);

    if (auto* api = app_.agentApiServer()) {
        agentApiLabel_ = new QLabel(this);
        agentApiLabel_->setAccessibleName("Agent API Clients");
        agentApiLabel_->setToolTip("AI agents connected through the local agent API (MCP). "
                                   "Help > Copy MCP Setup Command sets up a client.");
        agentApiLabel_->setStyleSheet(labelStyle);
        status->addPermanentWidget(agentApiLabel_);
        connect(api, &LocalApiServer::clientCountChanged, this, &MainWindow::updateAgentApiLabel);
        updateAgentApiLabel(api->clientCount());
    }

    status->addPermanentWidget(sampleRateLabel_);
    status->addPermanentWidget(bufferLabel_);
    status->addPermanentWidget(cpuLabel_);

    status->showMessage("Ready");

    statusBarTimer_ = new QTimer(this);
    connect(statusBarTimer_, &QTimer::timeout, this, &MainWindow::updateStatusBar);
    statusBarTimer_->start(1000);
    updateStatusBar();
}

void MainWindow::onNewProject()
{
    if (!maybeSaveBeforeAction()) return;
    if (aiChatWidget_) aiChatWidget_->saveChatSessions();
    editMgr_.newEdit();
    if (aiChatWidget_) aiChatWidget_->loadChatSessions();
    updateWindowTitle();
}

void MainWindow::onOpenProject()
{
    if (!maybeSaveBeforeAction()) return;

    QSettings settings;
    QString startDir = settings.value("paths/lastFileDialogDir", QDir::homePath()).toString();
    if (startDir.isEmpty() || !QDir(startDir).exists())
        startDir = QDir::homePath();

    QString path = QFileDialog::getOpenFileName(
        this, "Open Project", startDir,
        "Tracktion Edit (*.tracktionedit);;All Files (*)");
    if (path.isEmpty()) return;

    loadFile(path);
}

void MainWindow::loadFile(const QString& path)
{
    QFileInfo fi(path);
    if (!fi.exists() || fi.suffix().toLower() != "tracktionedit")
        return;

    juce::File file(juce::String(fi.absoluteFilePath().toUtf8().constData()));
    const bool loaded = editMgr_.loadEdit(file);
    if (aiChatWidget_) aiChatWidget_->loadChatSessions();
    updateWindowTitle();

    QSettings settings;
    settings.setValue("paths/lastFileDialogDir", fi.absolutePath());

    if (loaded) {
        addRecentProject(fi.absoluteFilePath());
    } else {
        QMessageBox::warning(this, "Open Project",
            QString("Could not open \"%1\".").arg(fi.fileName()));
    }
}

// ── Themes ──────────────────────────────────────────────────────────────────

void MainWindow::addThemeMenu(QMenu* parent)
{
    auto* themeMenu = parent->addMenu("&Theme");
    themeMenu->setAccessibleName("Theme");
    auto* group = new QActionGroup(themeMenu);
    group->setExclusive(true);

    const QString currentName = ThemeManager::instance().current().name;
    for (const auto& theme : ThemeManager::builtInThemes()) {
        // A strip of the theme's own colours, so the list previews itself.
        QPixmap swatch(30, 14);
        swatch.fill(theme.background);
        {
            QPainter p(&swatch);
            p.fillRect(QRect(0, 0, 10, 14), theme.surface);
            p.fillRect(QRect(10, 0, 10, 14), theme.accent);
            p.fillRect(QRect(20, 0, 10, 14),
                       theme.trackColours.empty() ? theme.text : theme.trackColours[1]);
            p.setPen(theme.separator);
            p.drawRect(swatch.rect().adjusted(0, 0, -1, -1));
        }
        auto* action = themeMenu->addAction(QIcon(swatch), theme.name);
        action->setCheckable(true);
        action->setChecked(theme.name == currentName);
        group->addAction(action);

        const QString name = theme.name;
        connect(action, &QAction::triggered, this, [this, name]() {
            if (name == ThemeManager::instance().current().name) return;
            ThemeManager::instance().setCurrentByName(name);
            ThemeManager::saveChoice(name);
            // Every widget bakes its colours in when it is built, so rebuild the
            // window. Queued: this action belongs to the window being replaced.
            auto* app = &app_;
            QTimer::singleShot(0, app, [app]() { app->rebuildMainWindow(); });
        });
    }
}

void MainWindow::prepareForRebuild()
{
    saveWindowLayout();
    if (routingView_) routingView_->flushNodePositions();
    if (aiChatWidget_) aiChatWidget_->saveChatSessions();
}

void MainWindow::finishRebuild()
{
    if (aiChatWidget_) aiChatWidget_->loadChatSessions();
    statusBar()->showMessage(
        QString("Theme: %1").arg(ThemeManager::instance().current().name), 4000);
}

// ── Recent projects ─────────────────────────────────────────────────────────

static const char* kRecentProjectsKey = "recentProjects";
static const int kMaxRecentProjects = 10;

void MainWindow::addRecentProject(const QString& path)
{
    const QString full = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    QSettings settings;
    QStringList recent = settings.value(kRecentProjectsKey).toStringList();
    recent.removeIf([&](const QString& p) { return p.compare(full, Qt::CaseInsensitive) == 0; });
    recent.prepend(full);
    while (recent.size() > kMaxRecentProjects)
        recent.removeLast();
    settings.setValue(kRecentProjectsKey, recent);
}

void MainWindow::rebuildRecentMenu()
{
    if (!recentMenu_) return;
    recentMenu_->clear();

    const QStringList recent = QSettings().value(kRecentProjectsKey).toStringList();
    if (recent.isEmpty()) {
        recentMenu_->addAction("No recent projects")->setEnabled(false);
        return;
    }

    const QFontMetrics fm(recentMenu_->font());
    for (int i = 0; i < recent.size(); ++i) {
        const QString& path = recent[i];
        QFileInfo fi(path);
        QString name = fi.completeBaseName();
        name.replace('&', "&&");
        QString folder = fm.elidedText(QDir::toNativeSeparators(fi.absolutePath()),
                                       Qt::ElideMiddle, 360);
        folder.replace('&', "&&");
        QString text = (i < 9 ? QString("&%1  ").arg(i + 1) : QString("    "))
                       + name + "    " + folder;
        auto* action = recentMenu_->addAction(text, this, [this, path]() {
            openRecentProject(path);
        });
        action->setToolTip(path);
        if (!fi.exists())
            action->setText(text + "  (missing)");
    }
    recentMenu_->addSeparator();
    recentMenu_->addAction("&Clear Recent Projects", this, []() {
        QSettings().remove(kRecentProjectsKey);
    });
}

void MainWindow::openRecentProject(const QString& path)
{
    if (!QFileInfo::exists(path)) {
        auto answer = QMessageBox::question(this, "Open Recent",
            QString("\"%1\" no longer exists.\n\nRemove it from the recent projects list?")
                .arg(QDir::toNativeSeparators(path)));
        if (answer == QMessageBox::Yes) {
            QSettings settings;
            QStringList recent = settings.value(kRecentProjectsKey).toStringList();
            recent.removeIf([&](const QString& p) { return p.compare(path, Qt::CaseInsensitive) == 0; });
            settings.setValue(kRecentProjectsKey, recent);
        }
        return;
    }
    if (!maybeSaveBeforeAction()) return;
    loadFile(path);
}

// ── Import ──────────────────────────────────────────────────────────────────

static double playheadBeat(EditManager& editMgr)
{
    auto* edit = editMgr.edit();
    if (!edit) return 0.0;
    return std::max(0.0, edit->tempoSequence.toBeats(editMgr.transport().getPosition()).inBeats());
}

static QString importStartDir()
{
    QSettings settings;
    QString dir = settings.value("paths/lastImportDir").toString();
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = settings.value("paths/lastFileDialogDir", QDir::homePath()).toString();
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = QDir::homePath();
    return dir;
}

void MainWindow::onImportAudio()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, "Import Audio", importStartDir(),
        "Audio Files (*.wav *.aif *.aiff *.flac *.ogg *.mp3);;All Files (*)");
    if (paths.isEmpty() || !editMgr_.edit()) return;
    QSettings().setValue("paths/lastImportDir", QFileInfo(paths.first()).absolutePath());
    importAudioFiles(paths);
}

void MainWindow::importAudioFiles(const QStringList& paths)
{
    if (paths.isEmpty() || !editMgr_.edit()) return;

    // One file goes on the selected audio track; several get a track each.
    te::AudioTrack* selected = timelineView_ ? timelineView_->selectedTrack() : nullptr;
    const bool useSelected = paths.size() == 1 && selected && !editMgr_.isMidiTrack(selected)
                             && !editMgr_.isBusTrack(selected);
    const double beat = playheadBeat(editMgr_);

    for (const auto& path : paths) {
        te::AudioTrack* track = useSelected ? selected : editMgr_.addAudioTrack();
        if (!track) break;
        if (!useSelected)
            track->setName(juce::String(QFileInfo(path).completeBaseName().toUtf8().constData()));
        editMgr_.addAudioClipToTrack(*track, juce::File(juce::String(path.toUtf8().constData())), beat);
    }
    if (timelineView_) timelineView_->rebuildClips();
    statusBar()->showMessage(QString("Imported %1 audio file%2 at the playhead")
                                 .arg(paths.size()).arg(paths.size() == 1 ? "" : "s"), 5000);
}

void MainWindow::onImportMidi()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Import MIDI", importStartDir(),
        "MIDI Files (*.mid *.midi);;All Files (*)");
    if (path.isEmpty() || !editMgr_.edit()) return;
    QSettings().setValue("paths/lastImportDir", QFileInfo(path).absolutePath());
    importMidiFile(path);
}

void MainWindow::importMidiFile(const QString& path)
{
    if (path.isEmpty() || !editMgr_.edit()) return;

    te::AudioTrack* track = timelineView_ ? timelineView_->selectedTrack() : nullptr;
    if (!track || !editMgr_.isMidiTrack(track)) {
        track = editMgr_.addMidiTrack();
        if (!track) return;
        track->setName(juce::String(QFileInfo(path).completeBaseName().toUtf8().constData()));
    }
    if (!editMgr_.importMidiFileToTrack(*track, juce::File(juce::String(path.toUtf8().constData())),
                                        playheadBeat(editMgr_))) {
        QMessageBox::warning(this, "Import MIDI",
            QString("Could not read \"%1\" as a MIDI file.").arg(QFileInfo(path).fileName()));
        return;
    }
    if (timelineView_) timelineView_->rebuildClips();
    statusBar()->showMessage("Imported MIDI at the playhead", 5000);
}

void MainWindow::importBrowserFile(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "mid" || ext == "midi")
        importMidiFile(path);
    else if (ext == "wav" || ext == "mp3" || ext == "flac" || ext == "ogg"
             || ext == "aiff" || ext == "aif") {
        // With the Channel Rack open, a double-clicked sample becomes a sample
        // channel; Shift+double-click (or a closed rack) puts it in the
        // playlist at the playhead instead.
        const bool shift = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;
        if (channelRack_ && channelRackDock_ && channelRackDock_->isVisible() && !shift) {
            if (channelRack_->addSampleChannels({path}) > 0)
                channelRackDock_->raise();
            return;
        }
        importAudioFiles({path});
    }
}

// ── Pattern workflow ────────────────────────────────────────────────────────

void MainWindow::placeCurrentPattern()
{
    auto& patterns = app_.patternManager();
    const int id = patterns.currentPattern();
    const auto pattern = id > 0 ? patterns.pattern(id) : std::nullopt;
    if (!pattern || !editMgr_.edit()) {
        statusBar()->showMessage("No pattern to place: make one in the Channel Rack (F6) first",
                                 5000);
        return;
    }

    // At the start of the bar under the playhead.
    const int beatsPerBar = std::max(1, editMgr_.getTimeSigNumerator());
    const double beat = playheadBeat(editMgr_);
    const double bar = std::floor(beat / beatsPerBar + 1e-6);
    const double startBeat = std::max(0.0, bar * beatsPerBar);

    if (patterns.placePattern(id, startBeat) <= 0) {
        statusBar()->showMessage(
            QString("\"%1\" has no steps yet, so there is nothing to place").arg(pattern->name),
            5000);
        return;
    }
    if (timelineView_) timelineView_->rebuildClips();
    statusBar()->showMessage(QString("Placed \"%1\" at bar %2")
                                 .arg(pattern->name).arg(int(bar) + 1), 4000);
}

QWidget* MainWindow::createPlaylistPanel()
{
    auto& theme = ThemeManager::instance().current();
    auto* panel = new QWidget(this);
    panel->setAccessibleName("Playlist");
    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // A title strip in the same style as the panels' title bars.
    auto* header = new QWidget(panel);
    header->setObjectName("playlistHeader");
    header->setAttribute(Qt::WA_StyledBackground, true);
    header->setFixedHeight(22);
    header->setStyleSheet(
        QString("QWidget#playlistHeader { background: %1; border-bottom: 1px solid %2; }"
                "QLabel { background: transparent; color: %3; }"
                "QToolButton { background: transparent; color: %3; border: none; "
                "  border-radius: %5px; padding: 0px 6px; font-size: 10px; }"
                "QToolButton:hover { background: %4; color: %6; }")
            .arg(theme.dockTitle.name(), theme.separator.name(), theme.dockTitleText.name(),
                 theme.surfaceLight.name())
            .arg(theme.cornerRadius)
            .arg(theme.text.name()));
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(8, 0, 4, 0);
    h->setSpacing(6);

    auto* title = new QLabel("Playlist", header);
    QFont f = title->font();
    f.setPixelSize(11);
    f.setBold(true);
    f.setCapitalization(QFont::SmallCaps);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    title->setFont(f);
    h->addWidget(title);

    auto* hint = new QLabel("Arrangement: patterns, audio and MIDI clips", header);
    QFont hf = hint->font();
    hf.setPixelSize(10);
    hint->setFont(hf);
    h->addWidget(hint);
    h->addStretch(1);

    auto* placeBtn = new QToolButton(header);
    placeBtn->setAccessibleName("Place Current Pattern");
    placeBtn->setText("Place pattern");
    placeBtn->setToolTip("Place the current pattern at the bar under the playhead (Ctrl+P)");
    placeBtn->setIcon(icons::glyphIcon(icons::materialIcons(14), icons::mi::PlaylistAdd,
                                       theme.accentLight, 14));
    placeBtn->setIconSize(QSize(14, 14));
    placeBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    placeBtn->setFocusPolicy(Qt::NoFocus);
    placeBtn->setFixedHeight(20);
    connect(placeBtn, &QToolButton::clicked, this, &MainWindow::placeCurrentPattern);
    h->addWidget(placeBtn);

    v->addWidget(header);
    timelineView_->setParent(panel);
    v->addWidget(timelineView_, 1);
    return panel;
}

void MainWindow::focusPlaylist()
{
    activateWindow();
    if (timelineView_) timelineView_->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::togglePanel(QDockWidget* dock)
{
    if (!dock) return;
    // In front: visible and not hidden behind another tab of the same group.
    const bool inFront = dock->isVisible() && !dock->visibleRegion().isEmpty();
    if (inFront) {
        dock->hide();
        return;
    }
    dock->show();
    dock->raise();
    if (dock->isFloating()) dock->activateWindow();
    if (auto* w = dock->widget()) w->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::copyMcpSetupCommand()
{
#ifdef Q_OS_WIN
    const QString exeName = "opendaw-mcp.exe";
#else
    const QString exeName = "opendaw-mcp";
#endif
    const QString path = QDir::toNativeSeparators(
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(exeName));
    const QString command = QString("\"%1\" --launch").arg(path);
    if (auto* clipboard = QGuiApplication::clipboard())
        clipboard->setText(command);
    if (QFileInfo::exists(path))
        statusBar()->showMessage("Copied the MCP command line: add it as a stdio MCP server in "
                                 "your agent's settings", 8000);
    else
        statusBar()->showMessage(QString("Copied the MCP command line, but %1 was not found next "
                                         "to OpenDaw: build the opendaw-mcp target").arg(exeName),
                                 10000);
}

void MainWindow::updateAgentApiLabel(int clients)
{
    if (!agentApiLabel_) return;
    agentApiLabel_->setText(QString("Agent API: %1 client%2")
                                .arg(clients).arg(clients == 1 ? "" : "s"));
}

// ── Window layout ───────────────────────────────────────────────────────────

// Bump when docks or toolbars are added/renamed so a stale layout is ignored.
// 2: browser on the left, Channel Rack dock, tools on the transport row.
static const int kWindowStateVersion = 2;

void MainWindow::restoreWindowLayout()
{
    QSettings settings;
    restoreGeometry(settings.value("window/geometry").toByteArray());
    restoreState(settings.value("window/state").toByteArray(), kWindowStateVersion);
    fixDockTabBars();
}

void MainWindow::saveWindowLayout()
{
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState(kWindowStateVersion));
}

void MainWindow::fixDockTabBars()
{
    // Dock tab bars elide by default ("Piano R..."); there is room for full names.
    // QMainWindow creates them lazily, so look again once the layout settles.
    auto apply = [this]() {
        for (auto* tabBar : findChildren<QTabBar*>(Qt::FindDirectChildrenOnly))
            tabBar->setElideMode(Qt::ElideNone);
    };
    apply();
    QTimer::singleShot(0, this, apply);
}

void MainWindow::onSaveProject()
{
    if (editMgr_.currentFile() == juce::File()) {
        onSaveProjectAs();
        return;
    }
    if (routingView_) routingView_->flushNodePositions();
    if (editMgr_.saveEdit())
        addRecentProject(QString::fromStdString(
            editMgr_.currentFile().getFullPathName().toStdString()));
    if (aiChatWidget_) aiChatWidget_->saveChatSessions();
    updateWindowTitle();
}

void MainWindow::onSaveProjectAs()
{
    QSettings settings;
    QString startDir = settings.value("paths/lastFileDialogDir", QDir::homePath()).toString();
    if (startDir.isEmpty() || !QDir(startDir).exists())
        startDir = QDir::homePath();

    QString path = QFileDialog::getSaveFileName(
        this, "Save Project", startDir,
        "Tracktion Edit (*.tracktionedit);;All Files (*)");
    if (path.isEmpty()) return;

    settings.setValue("paths/lastFileDialogDir", QFileInfo(path).absolutePath());

    if (routingView_) routingView_->flushNodePositions();
    juce::File file(juce::String(path.toUtf8().constData()));
    if (editMgr_.saveEditAs(file))
        addRecentProject(path);
    if (aiChatWidget_) aiChatWidget_->saveChatSessions();
    updateWindowTitle();
}

void MainWindow::onExportAudio()
{
    ExportDialog dlg(this);
    if (editMgr_.edit()) {
        auto& t = editMgr_.transport();
        const auto in = t.loopPoint1.get(), out = t.loopPoint2.get();
        const tracktion::TimeRange loop = out > in ? tracktion::TimeRange(in, out)
                                                   : tracktion::TimeRange();
        dlg.setRanges(loop, timelineView_ ? timelineView_->timeSelectionRange()
                                          : tracktion::TimeRange());
    }
    if (dlg.exec() != QDialog::Accepted) return;

    auto settings = dlg.settings();
    if (settings.destFile == juce::File()) {
        QMessageBox::warning(this, "Export Audio",
                             settings.stems == StemMode::None ? "Please select an output file."
                                                              : "Please select a folder for the stems.");
        return;
    }

    std::vector<EditManager::Stem> stems;
    if (settings.stems != StemMode::None) {
        if (!settings.destFile.isDirectory() && !settings.destFile.createDirectory()) {
            QMessageBox::warning(this, "Export Audio", "The stems folder can't be created.");
            return;
        }
        stems = editMgr_.planStems(settings.stems);
        if (stems.empty()) {
            QMessageBox::information(this, "Export Stems",
                "No track is playing into the mix, so there are no stems to export.");
            return;
        }
    }

    // Claim the render here, on the GUI thread, before any worker starts: the
    // event pumping below can dispatch agent calls, and they must see the
    // render as busy from the first slice on. Held for the whole export (every
    // stem too), so the stem mutes and the renders can't be interleaved with
    // agent edits.
    if (!editMgr_.tryClaimRender()) {
        QMessageBox::information(this, "Export Audio",
            "Another render (an export, freeze or AI analysis) is running. "
            "Try again when it has finished.");
        return;
    }
    struct RenderClaim {
        EditManager& em;
        bool held = true;
        void release() { if (held) { held = false; em.releaseRender(); } }
        ~RenderClaim() { release(); }
    } claim { editMgr_ };
    settings.renderClaimed = true;

    // A looping pattern plays live MIDI into every track, which the offline
    // render would pick up.
    app_.patternPlayer().stop();

    // Free the live playback context on this thread for the whole export, so
    // the offline graphs are the only thing running the Edit's plugins.
    // Declared after the claim: the session ends first, then the claim goes.
    std::optional<EditManager::ScopedRenderSession> renderSession;
    renderSession.emplace(editMgr_);

    ExportDialog progressDlg(this);
    progressDlg.setWindowTitle("Exporting...");
    progressDlg.setExporting(true);
    progressDlg.setModal(true);
    progressDlg.show();

    // Renders one file on a worker thread; the UI keeps painting meanwhile.
    auto renderOne = [this, &progressDlg](const ExportSettings& s, float base, float span) {
        bool ok = false;
        auto future = QtConcurrent::run([this, s, &progressDlg, &ok, base, span]() {
            ok = editMgr_.exportMix(s, [&progressDlg, base, span](float p) {
                QMetaObject::invokeMethod(&progressDlg, [&progressDlg, p, base, span]() {
                    progressDlg.setProgress(base + p * span);
                }, Qt::QueuedConnection);
            });
        });
        while (!future.isFinished())
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        return ok;
    };

    QStringList failed;
    if (stems.empty()) {
        if (!renderOne(settings, 0.0f, 1.0f))
            failed << QString::fromStdString(settings.destFile.getFileName().toStdString());
    } else {
        const QString ext = settings.format == ExportFormat::FLAC ? ".flac"
                          : settings.format == ExportFormat::OGG  ? ".ogg"
                          : settings.format == ExportFormat::MP3  ? ".mp3" : ".wav";
        const float span = 1.0f / float(stems.size());
        for (size_t i = 0; i < stems.size(); ++i) {
            QString name = settings.stemPattern;
            name.replace("{project}", editMgr_.projectName())
                .replace("{name}", stems[i].name)
                .replace("{n}", QString("%1").arg(i + 1, 2, 10, QChar('0')));
            const auto safe = EditManager::sanitizeForFilename(juce::String(name.toStdString()));
            auto s = settings;
            s.destFile = settings.destFile.getChildFile(safe + juce::String(ext.toStdString()));
            progressDlg.setStatus(QString("Stem %1 of %2: %3...").arg(i + 1).arg(stems.size())
                                      .arg(stems[i].name));
            editMgr_.applyStemMutes(stems[i]);
            const bool ok = renderOne(s, float(i) * span, span);
            editMgr_.restoreStemMutes();
            if (!ok) failed << stems[i].name;
        }
    }

    progressDlg.hide();
    // Done rendering: give playback and the agents the Edit back before the
    // report dialogs.
    renderSession.reset();
    claim.release();

    if (!stems.empty()) {
        const auto folder = QString::fromStdString(settings.destFile.getFullPathName().toStdString());
        if (failed.isEmpty()) {
            QMessageBox::information(this, "Export Complete",
                QString("Exported %1 stems to:\n%2").arg(stems.size()).arg(folder));
            statusBar()->showMessage("Stems exported", 5000);
        } else {
            QMessageBox::warning(this, "Export Stems",
                QString("These stems failed to export: %1\n\nThe others are in:\n%2")
                    .arg(failed.join(", "), folder));
        }
        return;
    }

    if (failed.isEmpty()) {
        auto fileName = QString::fromStdString(settings.destFile.getFileName().toStdString());
        QString report = QString("Mix exported to:\n%1").arg(fileName);
        const auto loud = editMgr_.lastExportLoudness();
        if (loud.ok && loud.integrated > -70.0) {
            report += QString("\n\nLoudness: %1 LUFS integrated, true peak %2 dBTP, range %3 LU.")
                          .arg(loud.integrated, 0, 'f', 1)
                          .arg(loud.truePeakDb, 0, 'f', 1)
                          .arg(loud.loudnessRange, 0, 'f', 1);
            if (loud.truePeakDb > -1.0)
                report += "\n\nThe true peak is above -1 dBTP, so it may clip when a "
                          "streaming service encodes it. A limiter on Master set to -1 dB fixes that.";
            if (loud.integrated > -13.0)
                report += QString("\n\nStreaming services turn mixes louder than -14 LUFS down, "
                                  "so this one will play about %1 dB quieter there.")
                              .arg(loud.integrated + 14.0, 0, 'f', 1);
        }
        QMessageBox::information(this, "Export Complete", report);
        statusBar()->showMessage("Export complete", 5000);
    } else {
        QMessageBox::critical(this, "Export Failed",
            "Failed to export the mix. Check the output path and try again.");
    }
}

void MainWindow::onAddTrack()
{
    editMgr_.addAudioTrack();
}

void MainWindow::onRemoveTrack()
{
    auto tracks = editMgr_.getAudioTracks();
    if (tracks.size() <= 1) return;

    te::Track* target = nullptr;
    if (timelineView_)
        target = timelineView_->selectedTrack();
    if (!target)
        target = tracks.getLast();

    auto answer = QMessageBox::question(
        this, "Remove Track",
        QString("Remove the selected track?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer == QMessageBox::Yes)
        editMgr_.removeTrack(target);
}

void MainWindow::onSplitPianoHands(te::AudioTrack* track)
{
    const QString title = "Split Piano Hands";
    if (!track) {
        QMessageBox::information(this, title, "Select the piano track to split first.");
        return;
    }
    if (!editMgr_.isMidiTrack(track)) {
        QMessageBox::information(this, title, "Hand splitting works on MIDI tracks.");
        return;
    }

    if (!handSplitter_) {
        handSplitter_ = new AiHandSplitter(&editMgr_, this);
        connect(handSplitter_, &AiHandSplitter::progress, this, [this](int done, int total) {
            if (!handSplitProgress_) return;
            // A single request has no meaningful progress; keep the busy bar.
            handSplitProgress_->setMaximum(total > 1 ? total : 0);
            handSplitProgress_->setValue(total > 1 ? done : 0);
            if (total > 1)
                handSplitProgress_->setLabelText(
                    QString("Analysing hand movement... (%1 of %2 sections)").arg(done).arg(total));
        });
        connect(handSplitter_, &AiHandSplitter::finished,
                this, &MainWindow::onHandSplitFinished);
        connect(handSplitter_, &AiHandSplitter::failed, this, [this](const QString& message) {
            closeHandSplitProgress();
            QMessageBox::warning(this, "Split Piano Hands", message);
        });
    }
    if (handSplitter_->isRunning()) {
        QMessageBox::information(this, title, "A hand split is already running.");
        return;
    }

    // Created before start(): start() reports progress straight away.
    handSplitProgress_ = new QProgressDialog("Analysing hand movement...", "Cancel", 0, 0, this);
    handSplitProgress_->setWindowTitle(title);
    handSplitProgress_->setAccessibleName("Split Piano Hands Progress");
    handSplitProgress_->setWindowModality(Qt::WindowModal);
    handSplitProgress_->setMinimumDuration(0);
    handSplitProgress_->setAutoClose(false);
    handSplitProgress_->setAutoReset(false);
    connect(handSplitProgress_, &QProgressDialog::canceled, this, [this]() {
        if (handSplitter_) handSplitter_->cancel();
        closeHandSplitProgress();
        statusBar()->showMessage("Hand split cancelled", 4000);
    });

    QString error;
    if (!handSplitter_->start(track, error)) {
        closeHandSplitProgress();
        QMessageBox::warning(this, title, error);
        return;
    }
    if (handSplitProgress_)
        handSplitProgress_->open();
}

void MainWindow::closeHandSplitProgress()
{
    if (!handSplitProgress_) return;
    // close() would emit canceled(); detach first.
    disconnect(handSplitProgress_, nullptr, this, nullptr);
    handSplitProgress_->hide();
    handSplitProgress_->deleteLater();
    handSplitProgress_ = nullptr;
}

void MainWindow::onHandSplitFinished(const QVector<bool>& isLeftHand, int fallbackNotes)
{
    closeHandSplitProgress();
    const QString title = "Split Piano Hands";

    // The Edit must not change while an export renders it; apply the split afterwards.
    if (editMgr_.isRenderInProgress()) {
        QTimer::singleShot(250, this, [this, isLeftHand, fallbackNotes]() {
            onHandSplitFinished(isLeftHand, fallbackNotes);
        });
        return;
    }

    te::AudioTrack* track = nullptr;
    for (auto* t : editMgr_.getAudioTracks())
        if (t->itemID == handSplitter_->trackId())
            track = t;
    if (!track || !handSplitter_->matchesTrack(track)) {
        QMessageBox::warning(this, title,
            "The track changed while it was being analysed, so nothing was split. "
            "Try again.");
        return;
    }

    const int leftCount = static_cast<int>(std::count(isLeftHand.begin(), isLeftHand.end(), true));
    const int rightCount = isLeftHand.size() - leftCount;
    if (leftCount == 0 || rightCount == 0) {
        QMessageBox::information(this, title,
            QString("Every note was assigned to the %1 hand, so there is nothing to split.")
                .arg(leftCount == 0 ? "right" : "left"));
        return;
    }

    std::vector<bool> leftHand(isLeftHand.begin(), isLeftHand.end());
    if (!editMgr_.splitTrackByHands(*track, leftHand)) {
        QMessageBox::warning(this, title, "The track could not be split.");
        return;
    }

    QString message = QString("Split into %1 right-hand and %2 left-hand notes")
                          .arg(rightCount).arg(leftCount);
    if (fallbackNotes > 0)
        message += QString(" (%1 guessed without AI)").arg(fallbackNotes);
    statusBar()->showMessage(message + ". Ctrl+Z to undo.", 10000);
}

void MainWindow::onSplitClipAtPlayhead()
{
    if (timelineView_)
        timelineView_->splitSelectedClipsAtPlayhead();
}

void MainWindow::onAddMidiTrack()
{
    editMgr_.addMidiTrack();
}

void MainWindow::connectPluginScanner()
{
    // Connected once per window (not per scan, which piled up one more
    // "scan complete" handler on every scan).
    auto& scanner = app_.pluginScanner();
    connect(&scanner, &PluginScanner::scanFinished, this, [this]() {
        auto& sc = app_.pluginScanner();
        const int count = sc.getPluginList().getNumTypes();
        QString text = QString("VST scan complete - %1 plugins found").arg(count);
        if (scanFailures_ > 0)
            text += QString(", %1 failed (%2 blacklisted; Edit > Clear Plugin Blacklist "
                            "and Rescan retries them)")
                        .arg(scanFailures_).arg(sc.blacklist().size());
        const int timeoutMs = scanFailures_ > 0 ? 15000 : 8000;
        scanFailures_ = 0;
        statusBar()->showMessage(text, timeoutMs);
    });
    connect(&scanner, &PluginScanner::pluginFailed, this,
            [this](const QString& file, const QString& reason) {
        ++scanFailures_;
        const QString name = QFileInfo(file).fileName();
        statusBar()->showMessage(reason.isEmpty()
                                     ? QString("Plugin failed to scan: %1").arg(name)
                                     : QString("Plugin failed to scan: %1 (%2)").arg(name, reason),
                                 10000);
        qWarning() << "[PluginScanner] failed:" << file << reason;
    });
}

void MainWindow::onScanVstPlugins()
{
    auto& scanner = app_.pluginScanner();
    if (scanner.isScanning()) {
        statusBar()->showMessage("A plugin scan is already running", 4000);
        return;
    }
    scanFailures_ = 0;
    statusBar()->showMessage("Scanning VST plugins...");
    scanner.startScan();
}

void MainWindow::onClearBlacklistAndRescan()
{
    auto& scanner = app_.pluginScanner();
    if (scanner.isScanning()) {
        statusBar()->showMessage("A plugin scan is already running", 4000);
        return;
    }
    const int count = scanner.blacklist().size();
    scanner.clearBlacklist();
    scanFailures_ = 0;
    statusBar()->showMessage(QString("Cleared %1 blacklisted plugin%2; rescanning all plugins...")
                                 .arg(count).arg(count == 1 ? "" : "s"));
    scanner.startScan(true);
}

void MainWindow::onEffectInsertRequested(te::AudioTrack* track, int)
{
    if (!track) return;
    effectChain_->setTrack(track);
    effectsDock_->setVisible(true);
    effectsDock_->raise();
}

void MainWindow::onInstrumentSelectRequested(te::AudioTrack* track)
{
    if (!track) return;

    auto chooseVst = [this, track]() {
        auto& pluginList = app_.pluginScanner().getPluginList();
        VstSelectorDialog dlg(pluginList, true, this);
        if (dlg.exec() == QDialog::Accepted && dlg.hasSelection())
            editMgr_.setTrackInstrument(*track, dlg.selectedPlugin());
    };
    std::function<void()> chooseSurge;
    if (BundledPlugins::hasSurgeXt()) {
        const auto trackId = track->itemID;
        chooseSurge = [this, trackId]() { chooseSurgeXt(trackId); };
    }
    showInstrumentMenu(editMgr_, track, chooseVst, chooseSurge, QCursor::pos());
}

namespace {

struct SurgeScanResult {
    PluginScanner::HelperOutcome outcome = PluginScanner::HelperOutcome::NotStarted;
    std::vector<juce::PluginDescription> found;
    QString error;
};

bool isSurgeDescription(const juce::PluginDescription& d, const juce::File& bundle)
{
    const juce::File f(d.fileOrIdentifier);
    return d.isInstrument && (f == bundle || f.isAChildOf(bundle));
}

} // namespace

void MainWindow::chooseSurgeXt(te::EditItemID trackId)
{
    const juce::File bundle = BundledPlugins::surgeXt();
    if (!bundle.exists()) return;

    // Runs on the GUI thread once the description is known.
    auto finish = [this, trackId](std::optional<juce::PluginDescription> desc) {
        if (!desc) {
            QMessageBox::warning(this, "Surge XT", "Surge XT couldn't be loaded.");
            return;
        }
        auto* track = editMgr_.findAudioTrack(trackId);
        if (!track) {
            statusBar()->showMessage("The track was removed before Surge XT was ready", 5000);
            return;
        }
        editMgr_.setTrackInstrument(*track, *desc);
        // Open its window: the presets are picked there.
        if (auto* ext = dynamic_cast<te::ExternalPlugin*>(editMgr_.getTrackInstrument(track)))
            PluginEditorWindow::showForPlugin(*ext);
    };

    auto& scanner = app_.pluginScanner();
    // Already scanned: no work.
    for (const auto& d : scanner.getPluginList().getTypes())
        if (isSurgeDescription(d, bundle)) {
            finish(d);
            return;
        }

    const juce::File helper = PluginScanner::helperExecutable();
    if (!helper.existsAsFile()) {
        // No helper: the plugin has to be loaded in this process, which must
        // happen on the message thread.
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const auto desc = BundledPlugins::surgeXtDescription(scanner.getPluginList());
        if (desc) scanner.saveCachedList();
        QApplication::restoreOverrideCursor();
        finish(desc);
        return;
    }

    if (surgeScanRunning_) {
        statusBar()->showMessage("Surge XT is still being scanned...", 4000);
        return;
    }
    // The first scan runs the helper (up to the helper time limit); keep the
    // window responsive meanwhile and carry on here when it is done.
    surgeScanRunning_ = true;
    setCursor(Qt::BusyCursor);
    statusBar()->showMessage("Preparing Surge XT (first use)...");

    auto* watcher = new QFutureWatcher<SurgeScanResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, finish, bundle]() {
        watcher->deleteLater();
        surgeScanRunning_ = false;
        unsetCursor();
        statusBar()->clearMessage();
        const SurgeScanResult r = watcher->result();

        auto& sc = app_.pluginScanner();
        auto& list = sc.getPluginList();
        std::optional<juce::PluginDescription> desc;
        if (r.outcome == PluginScanner::HelperOutcome::NotStarted) {
            // The helper wouldn't run: scan in process, as BundledPlugins does.
            QApplication::setOverrideCursor(Qt::WaitCursor);
            desc = BundledPlugins::surgeXtDescription(list);
            QApplication::restoreOverrideCursor();
        } else if (r.outcome == PluginScanner::HelperOutcome::Scanned) {
            for (const auto& d : r.found) {
                list.addType(d);
                if (!desc && isSurgeDescription(d, bundle)) desc = d;
            }
        } else {
            qWarning() << "[MainWindow] Surge XT failed to scan:" << r.error;
        }
        if (desc) sc.saveCachedList();
        finish(desc);
    });
    const juce::String file = bundle.getFullPathName();
    watcher->setFuture(QtConcurrent::run([helper, file]() {
        SurgeScanResult r;
        juce::OwnedArray<juce::PluginDescription> found;
        r.outcome = PluginScanner::scanFileWithHelper(
            helper, juce::VST3PluginFormat::getFormatName(), file, found, &r.error);
        for (auto* d : found) r.found.push_back(*d);
        return r;
    }));
}

void MainWindow::onMidiClipDoubleClicked(te::MidiClip* clip)
{
    if (!clip) return;
    pianoRollDock_->setVisible(true);
    pianoRollDock_->raise();
    showMidiClipInPianoRoll(clip);
}

// ── Piano roll <-> channels and patterns ────────────────────────────────────

static PianoRollEditor::PatternContext makePatternContext(const Pattern& p, te::AudioTrack& track)
{
    PianoRollEditor::PatternContext ctx;
    ctx.patternId = p.id;
    ctx.patternName = p.name;
    ctx.colour = p.colour;
    ctx.trackId = track.itemID;
    ctx.channelName = QString::fromStdString(track.getName().toStdString());
    ctx.lengthBeats = p.lengthBeats();
    return ctx;
}

void MainWindow::showMidiClipInPianoRoll(te::MidiClip* clip)
{
    if (!pianoRoll_ || !clip || !editMgr_.isClipValid(clip)) return;
    auto& pm = app_.patternManager();
    const int patternId = pm.patternOfClip(clip);
    auto* track = clip->getAudioTrack();
    if (patternId > 0 && track) {
        if (auto p = pm.pattern(patternId)) {
            pianoRoll_->setPatternClip(clip, &editMgr_, makePatternContext(*p, *track));
            return;
        }
    }
    pianoRoll_->setClip(clip, &editMgr_);
}

te::MidiClip* MainWindow::findPatternClip(int patternId, te::AudioTrack* track) const
{
    if (!track || patternId <= 0) return nullptr;
    auto placements = app_.patternManager().placements(patternId);
    std::sort(placements.begin(), placements.end(),
              [](const PatternPlacement& a, const PatternPlacement& b) {
                  return a.startBeat < b.startBeat;
              });
    for (const auto& placement : placements)
        for (const auto& clipId : placement.clipIds)
            if (auto* mc = dynamic_cast<te::MidiClip*>(track->findClipForID(clipId)))
                return mc;
    return nullptr;
}

void MainWindow::openPianoRollForChannel(te::AudioTrack* track, bool createClip, bool raiseDock)
{
    if (!pianoRoll_ || !track) return;
    auto& pm = app_.patternManager();
    int patternId = pm.currentPattern();
    if (patternId <= 0 || !pm.patternExists(patternId)) {
        // Like FL Studio there is always a pattern to put notes in.
        patternId = pm.createPattern();
        if (patternId <= 0) return;
        pm.setCurrentPattern(patternId);
    }
    auto p = pm.pattern(patternId);
    if (!p) return;

    // An existing placement clip is edited in place; otherwise the clip is
    // made now (explicit request, or the channel has notes to show) or by
    // the first note drawn.
    te::MidiClip* clip = findPatternClip(patternId, track);
    if (!createClip && !clip && !pm.channelNotes(patternId, track->itemID).isEmpty())
        createClip = true;
    if (clip || createClip) {
        if (auto* editable = pm.editableClipFor(patternId, track->itemID))
            clip = editable;
        if (!clip && createClip)
            statusBar()->showMessage("Could not make a clip for this channel", 5000);
    }
    if (clip && !editMgr_.isClipValid(clip)) clip = nullptr;

    pianoRoll_->setPreviewTrack(track);
    pianoRoll_->setPatternClip(clip, &editMgr_, makePatternContext(*p, *track));
    if (raiseDock && pianoRollDock_) {
        pianoRollDock_->show();
        pianoRollDock_->raise();
        if (pianoRollDock_->isFloating()) pianoRollDock_->activateWindow();
    }
}

void MainWindow::onChannelPianoRollRequested(te::AudioTrack* track)
{
    openPianoRollForChannel(track, true, true);
}

bool MainWindow::isPianoRollInFront() const
{
    return pianoRollDock_ && pianoRollDock_->isVisible()
        && !pianoRollDock_->visibleRegion().isEmpty();
}

bool MainWindow::isPatternChannel(te::AudioTrack* track) const
{
    return track && app_.patternManager().channels().contains(track);
}

bool MainWindow::pianoRollShowsChannel(te::AudioTrack* track) const
{
    if (!pianoRoll_ || !track) return false;
    if (pianoRoll_->isShowingPattern()) {
        const auto& ctx = pianoRoll_->patternContext();
        return ctx.trackId == track->itemID
            && ctx.patternId == app_.patternManager().currentPattern();
    }
    auto* clip = pianoRoll_->clip();
    return clip && editMgr_.isClipValid(clip) && clip->getAudioTrack() == track;
}

void MainWindow::onTrackSelectedForPianoRoll(te::AudioTrack* track, bool fromChannelRack)
{
    selectedTrackId_ = track ? track->itemID : te::EditItemID();
    if (typingKeyboard_) typingKeyboard_->setTarget(selectedTrackId_);
    if (!pianoRoll_) return;
    pianoRoll_->setPreviewTrack(track);
    // FL Studio: picking a channel while the piano roll is open shows it.
    // Queued: this runs inside the rack's own click handling.
    if (fromChannelRack && track && isPianoRollInFront() && isPatternChannel(track)
        && !pianoRollShowsChannel(track)) {
        const auto id = track->itemID;
        QTimer::singleShot(0, this, [this, id]() {
            auto* t = editMgr_.findAudioTrack(id);
            if (t && isPianoRollInFront() && isPatternChannel(t) && !pianoRollShowsChannel(t))
                openPianoRollForChannel(t, false, false);
        });
    }
}

void MainWindow::onCurrentPatternChangedForPianoRoll(int patternId)
{
    if (!pianoRoll_ || !pianoRoll_->isShowingPattern()) return;
    const auto ctx = pianoRoll_->patternContext();
    if (ctx.patternId == patternId) return;
    auto* track = editMgr_.findAudioTrack(ctx.trackId);
    if (patternId > 0 && track && app_.patternManager().patternExists(patternId))
        openPianoRollForChannel(track, false, false);   // same channel, new pattern
    else
        pianoRoll_->setClip(nullptr, &editMgr_);
}

void MainWindow::refreshPianoRollPattern()
{
    if (!pianoRoll_ || !pianoRoll_->isShowingPattern()) return;
    const auto ctx = pianoRoll_->patternContext();
    auto& pm = app_.patternManager();
    auto* track = editMgr_.findAudioTrack(ctx.trackId);
    if (!track) {
        pianoRoll_->dropStaleClips();            // the channel went
        return;
    }
    if (auto p = pm.pattern(ctx.patternId)) {
        pianoRoll_->updatePatternContext(makePatternContext(*p, *track));
        return;
    }
    // The pattern was removed (or undone): show the channel in the current one.
    const int current = pm.currentPattern();
    if (current > 0 && pm.patternExists(current))
        openPianoRollForChannel(track, false, false);
    else
        pianoRoll_->setClip(nullptr, &editMgr_);
}

void MainWindow::onAudioClipDoubleClicked(te::WaveAudioClip* clip)
{
    if (!clip) return;
    audioClipDock_->setVisible(true);
    audioClipDock_->raise();

    if (audioClipEditor_)
        audioClipEditor_->setClip(clip, &editMgr_);
}

} // namespace OpenDaw
