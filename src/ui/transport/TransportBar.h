#pragma once

#include "engine/EditManager.h"
#include <QWidget>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QTimer>
#include <QComboBox>
#include <QToolButton>

namespace OpenDaw {

class PatternManager;
class PatternPlayer;
class PatternSelector;
class RotaryKnob;

// The single-row top strip: Pattern/Song mode, transport, the LCD counter,
// tempo / meter / snap, the pattern picker, master volume, loop / punch /
// metronome / count-in, MIDI panic and engine power.
//
// In Pattern mode Play loops the current pattern through PatternPlayer (the
// song transport stays stopped); in Song mode it drives the Edit's transport.
// Every piece of state lives in EditManager, PatternManager or QSettings, so a
// window rebuild (theme change) picks it up again.
class TransportBar : public QWidget {
    Q_OBJECT

public:
    // patterns / player may be null: the strip then behaves as Song mode only.
    TransportBar(EditManager* editMgr, PatternManager* patterns, PatternPlayer* player,
                 QWidget* parent = nullptr);

    QSize sizeHint() const override { return {1100, kBarHeight}; }

    PatternSelector* patternSelector() const { return patternSelector_; }

    static constexpr int kBarHeight = 42;

public slots:
    // Play/stop in the current mode (Space).
    void togglePlay();
    // Stops the pattern player and the song transport, playhead back to 0.
    void stopAll();
    // MIDI panic: stops the pattern player and the song transport, sends
    // all-notes-off and resets the play/record buttons.
    void panic();

signals:
    void snapModeRequested(int mode);
    void loopToggled(bool enabled);

private slots:
    void onPlay();
    void onStop();
    void onRecord();
    void onLoop();
    void onMetronome();
    void onCountIn();
    void onCountInModeChanged(int index);
    void onPanic();
    void onEngineToggle();
    void onBpmChanged(double bpm);
    void onTimeSigNumChanged(int num);
    void updatePosition();

private:
    void applyButtonStyle(QPushButton* btn, const QColor& activeColor);
    void updateEngineButtonStyle();
    void syncCountInToEngine();
    void buildPunchMenu();
    void buildModeToggle(QHBoxLayout* layout);
    void syncModeButtons();
    void syncMasterVolume();
    bool patternModeActive() const;   // Pattern mode with a current pattern
    bool patternPlaying() const;
    void stopSongTransport();

    EditManager* editMgr_;
    PatternManager* patterns_ = nullptr;
    PatternPlayer* player_ = nullptr;

    QPushButton* patternModeBtn_ = nullptr;
    QPushButton* songModeBtn_ = nullptr;
    QPushButton* rtzBtn_;
    QPushButton* playBtn_;
    QPushButton* stopBtn_;
    QPushButton* recordBtn_;
    QPushButton* loopBtn_;
    QToolButton* punchBtn_;
    QPushButton* metronomeBtn_;
    QPushButton* countInBtn_;
    QComboBox*   countInCombo_;
    QPushButton* panicBtn_;
    QPushButton* engineBtn_;
    QLabel*      positionLabel_;
    QLabel*      beatLabel_;
    QDoubleSpinBox* bpmSpin_;
    QSpinBox*    timeSigNumSpin_;
    QComboBox*   timeSigDenCombo_;
    QComboBox*   snapCombo_;
    PatternSelector* patternSelector_ = nullptr;
    RotaryKnob*  masterKnob_ = nullptr;

    QTimer positionTimer_;
    int masterSyncTick_ = 0;
    int lastPatternStep_ = -1;
    int patternStepsPerBeat_ = 4;     // of the playing pattern
    QString playingPatternName_;
    bool isRecording_ = false;
    bool& deferredLoopActivation_;   // lives in EditManager, so it survives a window rebuild
};

} // namespace OpenDaw
