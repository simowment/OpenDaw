#include "TransportBar.h"
#include "engine/PatternManager.h"
#include "ui/controls/RotaryKnob.h"
#include "ui/timeline/GridSnapper.h"
#include "ui/toolbar/PatternSelector.h"
#include "utils/ThemeManager.h"
#include "utils/IconFont.h"
#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QMenu>
#include <QActionGroup>
#include <QSettings>
#include <QSignalBlocker>
#include <functional>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace OpenDaw {

namespace {
// Master volume knob: linear in dB from -60 to +6, like the mixer's master fader.
double masterDbToKnob(float db)
{
    return std::clamp((double(db) + 60.0) / 66.0, 0.0, 1.0);
}
float knobToMasterDb(double v)
{
    return v <= 0.001 ? -100.0f : float(-60.0 + v * 66.0);
}
} // namespace

TransportBar::TransportBar(EditManager* editMgr, PatternManager* patterns,
                           PatternPlayer* player, QWidget* parent)
    : QWidget(parent), editMgr_(editMgr), patterns_(patterns), player_(player),
      deferredLoopActivation_(editMgr->loopActivationDeferred())
{
    setAccessibleName("Transport Bar");
    setFixedHeight(kBarHeight);

    auto& theme = ThemeManager::instance().current();
    setAutoFillBackground(true);
    QPalette pal;
    pal.setColor(QPalette::Window, theme.surface);
    setPalette(pal);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 3, 6, 3);
    layout->setSpacing(4);

    const QFont iconFont = icons::fontAudio(15);
    // Every button and box in the row shares one height, so they line up.
    constexpr int kRowHeight = 28;
    const QSize transportButtonSize(32, kRowHeight);

    auto makeTransportBtn = [&](const QChar& glyph, const QString& name,
                                const QColor& activeColor, bool checkable = false) {
        auto* btn = new QPushButton(glyph, this);
        btn->setAccessibleName(name);
        btn->setToolTip(name);
        btn->setFont(iconFont);
        btn->setCheckable(checkable);
        btn->setFixedSize(transportButtonSize);
        btn->setFocusPolicy(Qt::NoFocus);
        applyButtonStyle(btn, activeColor);
        return btn;
    };

    auto makeSeparator = [&]() {
        auto* sep = new QFrame(this);
        sep->setFixedSize(1, kRowHeight);
        sep->setStyleSheet(QString("QFrame { background: %1; border: none; }")
                               .arg(theme.separator.name()));
        return sep;
    };

    // ── Pattern | Song ──
    buildModeToggle(layout);
    layout->addWidget(makeSeparator());

    // ── Transport buttons ──
    rtzBtn_    = makeTransportBtn(icons::fa::Prev,   "Return to Start", theme.text);
    stopBtn_   = makeTransportBtn(icons::fa::Stop,   "Stop",   theme.transportStop);
    playBtn_   = makeTransportBtn(icons::fa::Play,
                                  "Play (Space): loops the current pattern in Pattern mode, "
                                  "plays the playlist in Song mode",
                                  theme.transportPlay, true);
    playBtn_->setAccessibleName("Play");
    recordBtn_ = makeTransportBtn(icons::fa::Record, "Record (R)", theme.transportRecord, true);
    loopBtn_   = makeTransportBtn(icons::fa::Loop,   "Loop", theme.accent, true);
    metronomeBtn_ = makeTransportBtn(icons::fa::Metronome, "Metronome", theme.accent, true);

    // The record glyph reads as "record" only in red.
    recordBtn_->setStyleSheet(recordBtn_->styleSheet()
        + QString("QPushButton { color: %1; } QPushButton:checked { color: white; }")
              .arg(theme.transportRecord.name()));

    // Punch: click toggles it, the arrow holds pre-roll and post-roll.
    punchBtn_ = new QToolButton(this);
    punchBtn_->setText("Punch");
    punchBtn_->setAccessibleName("Punch In/Out Recording");
    punchBtn_->setToolTip("Punch in/out: record only inside the range on the ruler.\n"
                          "Plays a pre-roll first and stops after the post-roll (set them "
                          "with the arrow).");
    punchBtn_->setCheckable(true);
    punchBtn_->setPopupMode(QToolButton::MenuButtonPopup);
    punchBtn_->setFixedSize(66, kRowHeight);
    punchBtn_->setFocusPolicy(Qt::NoFocus);
    {
        const QColor& active = theme.transportRecord;
        const QColor checkedText = active.lightnessF() > 0.6 ? QColor(20, 20, 22) : QColor(Qt::white);
        punchBtn_->setStyleSheet(
            QString("QToolButton { background: %1; color: %2; border: 1px solid %3; "
                    "border-radius: %10px; font-weight: bold; font-size: 10px; padding-right: 12px; }"
                    "QToolButton:hover { background: %4; border-color: %5; }"
                    "QToolButton:checked { background: %6; color: %8; border-color: %7; }"
                    "QToolButton::menu-button { border: none; width: 14px; }"
                    "QToolButton::menu-arrow { image: url(%9); width: 8px; height: 5px; }")
                .arg(theme.background.name(), theme.text.name(), theme.separator.name(),
                     theme.surfaceLight.name(), theme.border.name(),
                     active.name(), active.darker(130).name(), checkedText.name(),
                     ThemeManager::dropDownArrowPath(theme.text))
                .arg(theme.cornerRadius));
    }
    buildPunchMenu();
    punchBtn_->setChecked(editMgr_->isPunchEnabled());
    connect(punchBtn_, &QToolButton::toggled, this, [this](bool on) {
        if (!editMgr_ || !editMgr_->edit()) return;
        editMgr_->setPunchEnabled(on);
        // Refused while recording: show the real state.
        QSignalBlocker block(punchBtn_);
        punchBtn_->setChecked(editMgr_->isPunchEnabled());
    });
    connect(editMgr_, &EditManager::punchChanged, this, [this](bool on) {
        QSignalBlocker block(punchBtn_);
        punchBtn_->setChecked(on);
    });

    countInBtn_ = new QPushButton("Count-in", this);
    countInBtn_->setAccessibleName("Count-In Before Recording");
    countInBtn_->setToolTip("Count-in before recording");
    countInBtn_->setCheckable(true);
    countInBtn_->setFixedSize(62, kRowHeight);
    countInBtn_->setFocusPolicy(Qt::NoFocus);
    applyButtonStyle(countInBtn_, theme.accent);

    countInCombo_ = new QComboBox(this);
    countInCombo_->setAccessibleName("Count-In Length");
    countInCombo_->setToolTip("Count-in length");
    countInCombo_->addItem("1 Bar",  1);
    countInCombo_->addItem("2 Bars", 2);
    countInCombo_->addItem("1 Beat", 4);
    countInCombo_->addItem("2 Beats", 3);
    countInCombo_->setFixedSize(78, kRowHeight);
    countInCombo_->setFocusPolicy(Qt::NoFocus);
    countInCombo_->setEnabled(false);

    panicBtn_ = new QPushButton(this);
    panicBtn_->setAccessibleName("MIDI Panic - Stop All Notes");
    panicBtn_->setToolTip("MIDI panic: stop all notes and reset (Ctrl+Shift+P)");
    panicBtn_->setText("PANIC");
    panicBtn_->setFixedSize(48, kRowHeight);
    panicBtn_->setFocusPolicy(Qt::NoFocus);
    panicBtn_->setStyleSheet(
        QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                "border-radius: %5px; font-size: 9px; font-weight: bold; letter-spacing: 1px; "
                "padding: 0px; }"
                "QPushButton:hover { color: %4; border-color: %4; }"
                "QPushButton:pressed { background: %4; color: white; }")
            .arg(theme.background.name(), theme.textDim.name(),
                 theme.border.name(), theme.recordArm.name())
            .arg(theme.cornerRadius));

    engineBtn_ = new QPushButton(icons::fa::Powerswitch, this);
    engineBtn_->setAccessibleName("Audio Engine Toggle");
    engineBtn_->setFont(iconFont);
    engineBtn_->setCheckable(true);
    engineBtn_->setChecked(!editMgr_->isEngineSuspended());
    engineBtn_->setFixedSize(transportButtonSize);
    engineBtn_->setFocusPolicy(Qt::NoFocus);
    updateEngineButtonStyle();

    connect(rtzBtn_,       &QPushButton::clicked, this, [this]() {
        if (!editMgr_ || !editMgr_->edit()) return;
        editMgr_->transport().setPosition(tracktion::TimePosition::fromSeconds(0.0));
    });
    connect(stopBtn_,      &QPushButton::clicked, this, &TransportBar::onStop);
    connect(playBtn_,      &QPushButton::clicked, this, &TransportBar::togglePlay);
    connect(recordBtn_,    &QPushButton::clicked, this, &TransportBar::onRecord);
    connect(loopBtn_,      &QPushButton::clicked, this, &TransportBar::onLoop);
    connect(metronomeBtn_, &QPushButton::clicked, this, &TransportBar::onMetronome);
    connect(countInBtn_,   &QPushButton::clicked, this, &TransportBar::onCountIn);
    connect(countInCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &TransportBar::onCountInModeChanged);
    connect(panicBtn_,     &QPushButton::clicked, this, &TransportBar::onPanic);
    connect(engineBtn_,    &QPushButton::clicked, this, &TransportBar::onEngineToggle);

    layout->addWidget(rtzBtn_);
    layout->addWidget(stopBtn_);
    layout->addWidget(playBtn_);
    layout->addWidget(recordBtn_);
    layout->addWidget(makeSeparator());

    // ── Counter: bars|beats large, min:sec (or the playing pattern) underneath ──
    constexpr int kDisplayHeight = 34;
    auto* counter = new QFrame(this);
    counter->setObjectName("transportCounter");
    counter->setFixedSize(150, kDisplayHeight);
    counter->setStyleSheet(
        QString("QFrame#transportCounter { background: %1; border: 1px solid %2; "
                "border-radius: %3px; }")
            .arg(theme.lcdBackground.name(), theme.separator.name())
            .arg(theme.cornerRadius));
    auto* counterLayout = new QVBoxLayout(counter);
    counterLayout->setContentsMargins(8, 1, 8, 1);
    counterLayout->setSpacing(0);

    beatLabel_ = new QLabel("1 | 1 | 000", counter);
    beatLabel_->setAccessibleName("Position Bars Beats");
    beatLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    beatLabel_->setStyleSheet(
        QString("QLabel { color: %1; background: transparent; "
                "font-family: 'Consolas', 'Cascadia Mono', 'Menlo', monospace; "
                "font-size: 17px; font-weight: bold; }")
            .arg(theme.lcdText.name()));

    positionLabel_ = new QLabel("0:00.000", counter);
    positionLabel_->setAccessibleName("Position Time");
    positionLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    positionLabel_->setStyleSheet(
        QString("QLabel { color: %1; background: transparent; "
                "font-family: 'Consolas', 'Cascadia Mono', 'Menlo', monospace; font-size: 9px; }")
            .arg(theme.textDim.name()));

    counterLayout->addWidget(beatLabel_);
    counterLayout->addWidget(positionLabel_);
    layout->addWidget(counter);

    // ── Tempo, meter, grid: a second display panel beside the counter ──
    auto* info = new QFrame(this);
    info->setObjectName("transportInfo");
    info->setFixedHeight(kDisplayHeight);
    const QColor& lcd = theme.lcdText;
    auto lcdAlpha = [&](int a) {
        return QString("rgba(%1,%2,%3,%4)").arg(lcd.red()).arg(lcd.green()).arg(lcd.blue()).arg(a);
    };
    info->setStyleSheet(
        QString("QFrame#transportInfo { background: %1; border: 1px solid %2; border-radius: %9px; }"
                "QLabel { color: %3; background: transparent; font-size: 8px; "
                "  font-weight: bold; letter-spacing: 1px; }"
                "QLabel#meterSlash { font-size: 12px; letter-spacing: 0px; }"
                "QAbstractSpinBox, QComboBox { background: transparent; color: %4; "
                "  border: 1px solid transparent; border-radius: 2px; padding: 0px 3px; "
                "  min-height: 0px; font-family: 'Consolas', 'Cascadia Mono', 'Menlo', monospace; "
                "  font-size: 13px; font-weight: bold; }"
                "QAbstractSpinBox:hover, QComboBox:hover { background: %5; border-color: %6; }"
                "QAbstractSpinBox:focus { border-color: %7; }"
                "QComboBox::drop-down { border: none; width: 12px; }"
                "QComboBox::down-arrow { image: url(%8); width: 9px; height: 6px; }")
            .arg(theme.lcdBackground.name(), theme.separator.name(), lcdAlpha(150),
                 lcd.name(), lcdAlpha(28), lcdAlpha(60), theme.accent.name(),
                 ThemeManager::dropDownArrowPath(lcd))
            .arg(theme.cornerRadius));
    auto* infoLayout = new QGridLayout(info);
    infoLayout->setContentsMargins(6, 1, 6, 1);
    infoLayout->setHorizontalSpacing(8);
    infoLayout->setVerticalSpacing(0);

    constexpr int kValueHeight = 19;
    // Values are typed, scrolled or nudged with the arrow keys; spin buttons would not fit.
    const QString valueHint = "Type a value, or scroll over it to change it";

    bpmSpin_ = new QDoubleSpinBox(info);
    bpmSpin_->setAccessibleName("Tempo BPM");
    bpmSpin_->setToolTip("Tempo (BPM). " + valueHint);
    bpmSpin_->setRange(20.0, 300.0);
    bpmSpin_->setDecimals(1);
    bpmSpin_->setValue(editMgr_->getBpm());
    bpmSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    bpmSpin_->setFixedSize(54, kValueHeight);
    connect(bpmSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &TransportBar::onBpmChanged);

    timeSigNumSpin_ = new QSpinBox(info);
    timeSigNumSpin_->setAccessibleName("Time Signature Numerator");
    timeSigNumSpin_->setToolTip("Beats per bar. " + valueHint);
    timeSigNumSpin_->setRange(1, 16);
    timeSigNumSpin_->setValue(editMgr_->getTimeSigNumerator());
    timeSigNumSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    timeSigNumSpin_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    timeSigNumSpin_->setFixedSize(26, kValueHeight);
    connect(timeSigNumSpin_, qOverload<int>(&QSpinBox::valueChanged),
            this, &TransportBar::onTimeSigNumChanged);

    timeSigDenCombo_ = new QComboBox(info);
    timeSigDenCombo_->setAccessibleName("Time Signature Denominator");
    for (int d : {1, 2, 4, 8, 16})
        timeSigDenCombo_->addItem(QString::number(d), d);
    int denIdx = timeSigDenCombo_->findData(editMgr_->getTimeSigDenominator());
    if (denIdx >= 0) timeSigDenCombo_->setCurrentIndex(denIdx);
    timeSigDenCombo_->setToolTip("Beat unit");
    timeSigDenCombo_->setFixedSize(38, kValueHeight);
    connect(timeSigDenCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                if (!editMgr_ || !editMgr_->edit()) return;
                int den = timeSigDenCombo_->itemData(idx).toInt();
                editMgr_->setTimeSignature(timeSigNumSpin_->value(), den);
            });

    auto* meterLayout = new QHBoxLayout;
    meterLayout->setContentsMargins(0, 0, 0, 0);
    meterLayout->setSpacing(0);
    auto* slash = new QLabel("/", info);
    slash->setObjectName("meterSlash");
    meterLayout->addWidget(timeSigNumSpin_);
    meterLayout->addWidget(slash);
    meterLayout->addWidget(timeSigDenCombo_);

    snapCombo_ = new QComboBox(info);
    snapCombo_->setAccessibleName("Grid Snap Mode");
    snapCombo_->setToolTip("Snap to grid");
    snapCombo_->addItem("Off",  int(SnapMode::Off));
    snapCombo_->addItem("Bar",  int(SnapMode::Bar));
    snapCombo_->addItem("1/2",  int(SnapMode::HalfNote));
    snapCombo_->addItem("1/4",  int(SnapMode::Beat));
    snapCombo_->addItem("1/8",  int(SnapMode::HalfBeat));
    snapCombo_->addItem("1/16", int(SnapMode::QuarterBeat));
    snapCombo_->addItem("1/32", int(SnapMode::EighthBeat));
    snapCombo_->setCurrentIndex(3);
    snapCombo_->setFixedSize(54, kValueHeight);
    connect(snapCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                emit snapModeRequested(snapCombo_->itemData(idx).toInt());
            });

    // Captions sit over the value text (inset past the box's border and padding).
    auto addCaption = [&](const QString& text, int column) {
        auto* label = new QLabel(text, info);
        label->setContentsMargins(4, 0, 0, 0);
        infoLayout->addWidget(label, 0, column);
    };
    addCaption("TEMPO", 0);
    addCaption("METER", 1);
    addCaption("SNAP", 2);
    infoLayout->addWidget(bpmSpin_, 1, 0);
    infoLayout->addLayout(meterLayout, 1, 1);
    infoLayout->addWidget(snapCombo_, 1, 2);
    layout->addWidget(info);
    layout->addWidget(makeSeparator());

    // ── Pattern picker ──
    if (patterns_) {
        patternSelector_ = new PatternSelector(patterns_, this);
        layout->addWidget(patternSelector_);
        layout->addWidget(makeSeparator());
    }

    // ── Master volume ──
    masterKnob_ = new RotaryKnob(this);
    masterKnob_->setAccessibleName("Master Volume");
    masterKnob_->setRange(0.0, 1.0);
    masterKnob_->setFixedSize(kRowHeight + 2, kRowHeight + 2);
    syncMasterVolume();
    connect(masterKnob_, &RotaryKnob::valueChanged, this, [this](double v) {
        if (!editMgr_ || !editMgr_->edit()) return;
        if (auto masterVol = editMgr_->edit()->getMasterVolumePlugin())
            masterVol->setVolumeDb(knobToMasterDb(v));
        const float db = knobToMasterDb(v);
        masterKnob_->setToolTip(db <= -99.0f ? QString("Master volume: -inf dB")
                                             : QString("Master volume: %1 dB").arg(db, 0, 'f', 1));
    });
    layout->addWidget(masterKnob_);
    layout->addWidget(makeSeparator());

    layout->addWidget(loopBtn_);
    layout->addWidget(punchBtn_);
    layout->addWidget(metronomeBtn_);
    layout->addWidget(countInBtn_);
    layout->addWidget(countInCombo_);

    layout->addStretch();

    // ── Rarely used: MIDI panic, engine power ──
    layout->addWidget(panicBtn_);
    layout->addWidget(engineBtn_);

    // Restore metronome and count-in state from settings
    {
        QSettings settings;
        bool metroOn = settings.value("transport/metronomeEnabled", false).toBool();
        metronomeBtn_->setChecked(metroOn);
        if (editMgr_ && editMgr_->edit())
            editMgr_->edit()->clickTrackEnabled = metroOn;

        bool countInOn = settings.value("transport/countInEnabled", false).toBool();
        int countInMode = settings.value("transport/countInMode", 1).toInt();
        int comboIdx = countInCombo_->findData(countInMode);
        if (comboIdx >= 0)
            countInCombo_->setCurrentIndex(comboIdx);
        countInBtn_->setChecked(countInOn);
        countInCombo_->setEnabled(countInOn);
        syncCountInToEngine();
    }

    // A theme change rebuilds this bar mid-session; pick up a recording in progress.
    if (editMgr_->edit() && editMgr_->transport().isRecording()) {
        isRecording_ = true;
        recordBtn_->setChecked(true);
    }

    connect(editMgr_, &EditManager::editChanged, this, [this]() {
        if (!editMgr_ || !editMgr_->edit()) return;
        double bpm = editMgr_->getBpm();
        if (bpmSpin_->value() != bpm) {
            QSignalBlocker blocker(bpmSpin_);
            bpmSpin_->setValue(bpm);
        }
        int num = editMgr_->getTimeSigNumerator();
        if (timeSigNumSpin_->value() != num) {
            QSignalBlocker blocker(timeSigNumSpin_);
            timeSigNumSpin_->setValue(num);
        }
        int den = editMgr_->getTimeSigDenominator();
        int denIdx = timeSigDenCombo_->findText(QString::number(den));
        if (denIdx >= 0 && timeSigDenCombo_->currentIndex() != denIdx) {
            QSignalBlocker blocker(timeSigDenCombo_);
            timeSigDenCombo_->setCurrentIndex(denIdx);
        }
        syncMasterVolume();
    });

    // ── Pattern mode wiring ──
    if (patterns_) {
        connect(patterns_, &PatternManager::modeChanged, this, [this](PatternManager::Mode mode) {
            syncModeButtons();
            // One thing plays at a time: switching mode stops the other one.
            if (mode == PatternManager::Mode::Song && patternPlaying())
                player_->stop();
            if (mode == PatternManager::Mode::Pattern && editMgr_ && editMgr_->edit()
                && editMgr_->transport().isPlaying() && !editMgr_->transport().isRecording())
                stopSongTransport();
        });
        connect(patterns_, &PatternManager::currentPatternChanged, this, [this](int id) {
            // Follow the selection while a pattern loops.
            if (!patternPlaying()) return;
            if (id > 0) player_->play(id);
            else player_->stop();
        });
    }
    if (player_) {
        connect(player_, &PatternPlayer::stepChanged, this, [this](int step) {
            lastPatternStep_ = step;
            if (step >= 0 && patterns_ && player_) {
                if (auto pat = patterns_->pattern(player_->playingPattern())) {
                    patternStepsPerBeat_ = std::max(1, pat->stepsPerBeat);
                    playingPatternName_ = pat->name;
                }
            }
        });
        connect(player_, &PatternPlayer::playingChanged, this, [this](bool playing) {
            if (!playing) lastPatternStep_ = -1;
            updatePosition();
        });
    }
    syncModeButtons();

    connect(&positionTimer_, &QTimer::timeout, this, &TransportBar::updatePosition);
    positionTimer_.start(16);
}

void TransportBar::buildModeToggle(QHBoxLayout* layout)
{
    auto& theme = ThemeManager::instance().current();
    const QColor checkedText = ThemeManager::readableTextOn(theme.accent);
    const QString style =
        QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                "font-size: 10px; font-weight: bold; letter-spacing: 1px; padding: 0px; }"
                "QPushButton:hover:!checked { background: %4; color: %5; }"
                "QPushButton:checked { background: %6; color: %7; border-color: %8; }"
                "QPushButton:disabled { color: %3; }")
            .arg(theme.background.name(), theme.textDim.name(), theme.separator.name(),
                 theme.surfaceLight.name(), theme.text.name(), theme.accent.name(),
                 checkedText.name(), theme.accent.darker(130).name());
    const QString r = QString::number(theme.cornerRadius);

    patternModeBtn_ = new QPushButton("PAT", this);
    patternModeBtn_->setAccessibleName("Pattern Mode");
    patternModeBtn_->setToolTip("Pattern mode: Play loops the current pattern");
    songModeBtn_ = new QPushButton("SONG", this);
    songModeBtn_->setAccessibleName("Song Mode");
    songModeBtn_->setToolTip("Song mode: Play plays the playlist");
    for (auto* b : {patternModeBtn_, songModeBtn_}) {
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedSize(b == songModeBtn_ ? 46 : 40, 28);
    }
    // A segmented pair: rounded only on the outside corners.
    patternModeBtn_->setStyleSheet(style + "QPushButton { border-top-left-radius: " + r
                                   + "px; border-bottom-left-radius: " + r + "px; }");
    songModeBtn_->setStyleSheet(style + "QPushButton { border-top-right-radius: " + r
                                + "px; border-bottom-right-radius: " + r + "px; "
                                "border-left: none; }");

    auto* pair = new QHBoxLayout;
    pair->setContentsMargins(0, 0, 0, 0);
    pair->setSpacing(0);
    pair->addWidget(patternModeBtn_);
    pair->addWidget(songModeBtn_);
    layout->addLayout(pair);

    connect(patternModeBtn_, &QPushButton::clicked, this, [this]() {
        if (patterns_) patterns_->setMode(PatternManager::Mode::Pattern);
        syncModeButtons();
    });
    connect(songModeBtn_, &QPushButton::clicked, this, [this]() {
        if (patterns_) patterns_->setMode(PatternManager::Mode::Song);
        syncModeButtons();
    });
}

void TransportBar::syncModeButtons()
{
    if (!patternModeBtn_ || !songModeBtn_) return;
    const bool havePatterns = patterns_ != nullptr && player_ != nullptr;
    patternModeBtn_->setEnabled(havePatterns);
    const bool pattern = havePatterns && patterns_->mode() == PatternManager::Mode::Pattern;
    QSignalBlocker b1(patternModeBtn_), b2(songModeBtn_);
    patternModeBtn_->setChecked(pattern);
    songModeBtn_->setChecked(!pattern);
}

bool TransportBar::patternModeActive() const
{
    if (!patterns_ || !player_) return false;
    if (patterns_->mode() != PatternManager::Mode::Pattern) return false;
    const int id = patterns_->currentPattern();
    return id > 0 && patterns_->pattern(id).has_value();
}

bool TransportBar::patternPlaying() const
{
    return player_ && player_->isPlaying();
}

void TransportBar::stopSongTransport()
{
    if (!editMgr_ || !editMgr_->edit()) return;
    auto& t = editMgr_->transport();
    t.stop(false, false);
    if (deferredLoopActivation_) {
        t.looping.setValue(true, nullptr);
        deferredLoopActivation_ = false;
    }
}

void TransportBar::togglePlay()
{
    if (!editMgr_ || !editMgr_->edit()) return;
    auto& t = editMgr_->transport();

    if (patternPlaying()) {
        player_->stop();
    } else if (t.isPlaying()) {
        // A playing song (started by the AI, the audio clip editor or the
        // looper) is stopped, even in Pattern mode: the toggle never starts
        // something else. onPlay handles the deferred loop and record state.
        onPlay();
    } else if (patternModeActive() && !t.isRecording()) {
        player_->play(patterns_->currentPattern());
    } else {
        onPlay();
    }
    playBtn_->setChecked(t.isPlaying() || patternPlaying());
}

void TransportBar::stopAll()
{
    onStop();
}

void TransportBar::onPlay()
{
    if (!editMgr_ || !editMgr_->edit()) return;
    if (patternPlaying()) player_->stop();
    auto& t = editMgr_->transport();
    if (t.isPlaying()) {
        t.stop(false, false);
        if (deferredLoopActivation_) {
            t.looping.setValue(true, nullptr);
            deferredLoopActivation_ = false;
        }
        if (isRecording_) {
            isRecording_ = false;
            recordBtn_->setChecked(false);
        }
    } else {
        // If looping is on and playhead is before the loop region,
        // temporarily disable engine looping so playback starts from
        // the current position rather than jumping to loop start.
        if (t.looping.get()) {
            auto pos = t.getPosition();
            auto loopStart = t.loopPoint1.get();
            if (pos < loopStart) {
                t.looping.setValue(false, nullptr);
                deferredLoopActivation_ = true;
            }
        }
        t.play(false);
    }
    playBtn_->setChecked(t.isPlaying());
}

void TransportBar::onStop()
{
    if (patternPlaying()) player_->stop();
    if (!editMgr_ || !editMgr_->edit()) return;
    auto& t = editMgr_->transport();
    t.stop(false, false);
    if (deferredLoopActivation_) {
        t.looping.setValue(true, nullptr);
        deferredLoopActivation_ = false;
    }
    t.setPosition(tracktion::TimePosition::fromSeconds(0));
    playBtn_->setChecked(false);
    recordBtn_->setChecked(false);
    isRecording_ = false;
}

void TransportBar::onRecord()
{
    if (!editMgr_ || !editMgr_->edit()) return;
    // Recording runs the song transport; a looping pattern would play over it.
    if (patternPlaying()) player_->stop();
    auto& t = editMgr_->transport();
    editMgr_->toggleRecording();
    isRecording_ = t.isRecording();
    recordBtn_->setChecked(isRecording_);
    playBtn_->setChecked(t.isPlaying());
}

void TransportBar::buildPunchMenu()
{
    auto* menu = new QMenu(punchBtn_);
    menu->setAccessibleName("Punch Options");
    auto addRollMenu = [&](const QString& title, int current, std::function<void(int)> apply) {
        auto* sub = menu->addMenu(title);
        auto* group = new QActionGroup(sub);
        for (int bars : {0, 1, 2, 4}) {
            auto* a = sub->addAction(bars == 0 ? QString("Off")
                                               : QString("%1 Bar%2").arg(bars).arg(bars == 1 ? "" : "s"));
            a->setCheckable(true);
            a->setChecked(bars == current);
            group->addAction(a);
            connect(a, &QAction::triggered, this, [apply, bars]() { apply(bars); });
        }
    };
    addRollMenu("Pre-roll", editMgr_->preRollBars(),
                [this](int bars) { editMgr_->setPreRollBars(bars); });
    addRollMenu("Post-roll", editMgr_->postRollBars(),
                [this](int bars) { editMgr_->setPostRollBars(bars); });
    punchBtn_->setMenu(menu);
}

void TransportBar::onLoop()
{
    if (!editMgr_ || !editMgr_->edit()) return;
    auto& t = editMgr_->transport();
    bool enabling = !t.looping.get();

    if (enabling) {
        // Loop and punch share the range and can't both be on.
        if (editMgr_->isPunchEnabled()) {
            if (t.isRecording()) return;
            editMgr_->setPunchEnabled(false);
        }
        editMgr_->ensureLoopRange();
    }

    t.looping.setValue(enabling, nullptr);
    loopBtn_->setChecked(t.looping.get());
    emit loopToggled(t.looping.get());
}

void TransportBar::onMetronome()
{
    if (!editMgr_ || !editMgr_->edit()) return;
    bool on = metronomeBtn_->isChecked();
    editMgr_->edit()->clickTrackEnabled = on;
    QSettings settings;
    settings.setValue("transport/metronomeEnabled", on);
}

void TransportBar::onCountIn()
{
    bool on = countInBtn_->isChecked();
    countInCombo_->setEnabled(on);
    syncCountInToEngine();
    QSettings settings;
    settings.setValue("transport/countInEnabled", on);
}

void TransportBar::onCountInModeChanged(int /*index*/)
{
    syncCountInToEngine();
    QSettings settings;
    settings.setValue("transport/countInMode", countInCombo_->currentData().toInt());
}

void TransportBar::syncCountInToEngine()
{
    if (!editMgr_ || !editMgr_->edit()) return;
    if (countInBtn_->isChecked()) {
        int mode = countInCombo_->currentData().toInt();
        editMgr_->edit()->setCountInMode(
            static_cast<te::Edit::CountIn>(mode));
    } else {
        editMgr_->edit()->setCountInMode(te::Edit::CountIn::none);
    }
}

void TransportBar::panic()
{
    onPanic();
}

void TransportBar::onPanic()
{
    if (!editMgr_) return;
    if (patternPlaying()) player_->stop();
    editMgr_->midiPanic();
    playBtn_->setChecked(false);
    recordBtn_->setChecked(false);
    isRecording_ = false;
}

void TransportBar::onEngineToggle()
{
    if (!editMgr_) return;

    bool turningOn = engineBtn_->isChecked();

    if (turningOn) {
        auto& theme = ThemeManager::instance().current();
        auto* busyDlg = new QDialog(window());
        busyDlg->setWindowTitle("Audio Engine");
        busyDlg->setAccessibleName("Audio Engine Initializing");
        busyDlg->setModal(true);
        busyDlg->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
        busyDlg->setFixedSize(320, 100);

        auto* label = new QLabel("Initializing audio engine...", busyDlg);
        label->setAccessibleName("Engine Status");
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(
            QString("QLabel { color: %1; font-size: 12px; font-weight: bold; }")
                .arg(theme.text.name()));
        auto* dlgLayout = new QVBoxLayout(busyDlg);
        dlgLayout->addWidget(label);

        QTimer::singleShot(50, busyDlg, [this, busyDlg]() {
            editMgr_->resumeEngine();
            busyDlg->accept();
        });

        busyDlg->exec();
        delete busyDlg;
    } else {
        if (patternPlaying()) player_->stop();
        editMgr_->suspendEngine();
    }

    updateEngineButtonStyle();
    playBtn_->setChecked(false);
    recordBtn_->setChecked(false);
    isRecording_ = false;
}

void TransportBar::updateEngineButtonStyle()
{
    auto& theme = ThemeManager::instance().current();
    const bool active = engineBtn_->isChecked();
    engineBtn_->setToolTip(active ? "Audio engine on: click to turn it off"
                                  : "Audio engine off: click to turn it on");
    engineBtn_->setStyleSheet(
        QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                "border-radius: %5px; padding: 0px; }"
                "QPushButton:hover { background: %4; }")
            .arg(theme.background.name(),
                 (active ? theme.meterGreen : theme.textDim).name(),
                 theme.border.name(), theme.surfaceLight.name())
            .arg(theme.cornerRadius));
}

void TransportBar::onBpmChanged(double bpm)
{
    if (!editMgr_ || !editMgr_->edit()) return;
    auto& ts = editMgr_->edit()->tempoSequence;
    auto& transport = editMgr_->transport();
    double beatBefore = ts.toBeats(transport.getPosition()).inBeats();
    editMgr_->setBpm(bpm);
    auto newTime = ts.toTime(tracktion::BeatPosition::fromBeats(beatBefore));
    transport.setPosition(newTime);
}

void TransportBar::onTimeSigNumChanged(int num)
{
    if (!editMgr_ || !editMgr_->edit()) return;
    int den = timeSigDenCombo_->currentData().toInt();
    editMgr_->setTimeSignature(num, den);
}

void TransportBar::syncMasterVolume()
{
    if (!masterKnob_ || !editMgr_ || !editMgr_->edit()) return;
    auto masterVol = editMgr_->edit()->getMasterVolumePlugin();
    if (!masterVol) return;
    const float db = masterVol->getVolumeDb();
    const double v = masterDbToKnob(db);
    // Showing the level must not write it back (it would round -60 dB to mute).
    if (std::abs(masterKnob_->value() - v) > 0.002) {
        QSignalBlocker block(masterKnob_);
        masterKnob_->setValue(v);
    }
    masterKnob_->setToolTip(db <= -99.0f ? QString("Master volume: -inf dB")
                                         : QString("Master volume: %1 dB").arg(db, 0, 'f', 1));
}

void TransportBar::updatePosition()
{
    if (!editMgr_ || !editMgr_->edit()) return;

    // The mixer's master fader (or the AI) may move the master level too.
    if (++masterSyncTick_ >= 15) {
        masterSyncTick_ = 0;
        syncMasterVolume();
    }

    if (editMgr_->isEngineSuspended()) {
        positionLabel_->setText("engine off");
        beatLabel_->setText("- | - | ---");
        return;
    }

    auto& transport = editMgr_->transport();

    if (patternPlaying()) {
        // The counter follows the looping pattern; the song playhead is parked.
        const int step = std::max(0, lastPatternStep_);
        const double beat = double(step) / double(std::max(1, patternStepsPerBeat_));
        const int beatsPerBar = std::max(1, editMgr_->getTimeSigNumerator());
        const int bar = int(beat / beatsPerBar) + 1;
        const int beatInBar = int(std::fmod(beat, double(beatsPerBar))) + 1;
        const int ticks = int(std::fmod(beat, 1.0) * 960);
        beatLabel_->setText(QString("%1 | %2 | %3").arg(bar).arg(beatInBar)
                                .arg(ticks, 3, 10, QChar('0')));
        positionLabel_->setText(QString("PAT  %1").arg(playingPatternName_));
        playBtn_->setChecked(true);
        isRecording_ = transport.isRecording();
        recordBtn_->setChecked(isRecording_);
        loopBtn_->setChecked(transport.looping.get() || deferredLoopActivation_);
        return;
    }

    auto pos = transport.getPosition();
    double secs = std::max(0.0, pos.inSeconds());

    int mins = int(secs) / 60;
    int wholeSecs = int(secs) % 60;
    int millis = int((secs - std::floor(secs)) * 1000);
    positionLabel_->setText(
        QString("%1:%2.%3")
            .arg(mins)
            .arg(wholeSecs, 2, 10, QChar('0'))
            .arg(millis, 3, 10, QChar('0')));

    auto& ts2 = editMgr_->edit()->tempoSequence;
    double beat = std::max(0.0, ts2.toBeats(pos).inBeats());
    int beatsPerBar = editMgr_->getTimeSigNumerator();
    int bar = int(beat / beatsPerBar) + 1;
    int beatInBar = int(std::fmod(beat, beatsPerBar)) + 1;
    int ticks = int(std::fmod(beat, 1.0) * 960);   // 960 ticks per quarter note

    beatLabel_->setText(
        QString("%1 | %2 | %3")
            .arg(bar)
            .arg(beatInBar)
            .arg(ticks, 3, 10, QChar('0')));

    playBtn_->setChecked(transport.isPlaying());
    // Recording also stops on its own (post-roll, the R key, the AI).
    isRecording_ = transport.isRecording();
    recordBtn_->setChecked(isRecording_);

    // Re-enable looping once the playhead reaches the loop region
    if (deferredLoopActivation_ && transport.isPlaying()) {
        auto loopStart = transport.loopPoint1.get();
        if (pos >= loopStart) {
            transport.looping.setValue(true, nullptr);
            deferredLoopActivation_ = false;
        }
    }

    loopBtn_->setChecked(transport.looping.get() || deferredLoopActivation_);
}

void TransportBar::applyButtonStyle(QPushButton* btn, const QColor& activeColor)
{
    auto& theme = ThemeManager::instance().current();
    const QColor checkedText = activeColor.lightnessF() > 0.6 ? QColor(20, 20, 22) : QColor(Qt::white);
    btn->setStyleSheet(
        QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                "border-radius: %9px; font-weight: bold; font-size: 10px; padding: 0px; }"
                "QPushButton:hover { background: %4; border-color: %5; }"
                "QPushButton:checked { background: %6; color: %8; border-color: %7; }"
                "QPushButton:pressed { background: %4; }")
            .arg(theme.background.name(), theme.text.name(), theme.separator.name(),
                 theme.surfaceLight.name(), theme.border.name(),
                 activeColor.name(), activeColor.darker(130).name(), checkedText.name())
            .arg(theme.cornerRadius));
}

} // namespace OpenDaw
