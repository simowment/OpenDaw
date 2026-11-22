#include "ChannelStrip.h"
#include "SendsPanel.h"
#include "utils/ThemeManager.h"
#include "utils/IconFont.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QToolTip>
#include <QPointer>
#include <algorithm>
#include <cmath>
#include <optional>

namespace {
// Restyling is expensive (Qt re-polishes the widget): skip it when unchanged.
void setStyleSheetIfChanged(QWidget* w, const QString& css)
{
    if (w && w->styleSheet() != css)
        w->setStyleSheet(css);
}

// The theme's corner rounding, as a style sheet declaration.
QString radiusCss(int max = 3)
{
    const int r = std::min(max, OpenDaw::ThemeManager::instance().current().cornerRadius);
    return QString("border-radius: %1px; ").arg(std::max(0, r));
}

float dbToNormalized(float db) {
    constexpr float minDb = -60.0f;
    constexpr float maxDb = 6.0f;
    if (db <= minDb) return 0.0f;
    if (db >= maxDb) return 1.0f;
    return (db - minDb) / (maxDb - minDb);
}
}

namespace OpenDaw {

ChannelStrip::ChannelStrip(te::AudioTrack* track, EditManager* editMgr,
                           QWidget* parent, const TrackRoutingChoices* routing)
    : QWidget(parent), track_(track),
      trackId_(track ? track->itemID.getRawID() : 0),
      kind_(track && editMgr ? trackWidgetKind(*editMgr, track) : TrackWidgetKind::Audio),
      editMgr_(editMgr)
{
    trackRef_ = track;
    setAccessibleName("Channel Strip");
    setupUI(routing);

    reconnectLevelMeterSource();

    if (editMgr_) {
        connect(editMgr_, &EditManager::aboutToChangeEdit, this, [this]() {
            dropLevelMeterClient();
        });
        // Track strips re-resolve their meter in refresh(), which MixerView
        // calls only after checking the strip's track still exists. Reading
        // track_ straight from editChanged could touch a deleted track.
        connect(editMgr_, &EditManager::editChanged, this, [this]() {
            if (isMaster_)
                reconnectLevelMeterSource();
        });
    }

    connect(&meterTimer_, &QTimer::timeout, this, &ChannelStrip::updateMeter);
    meterTimer_.start(30);
}

ChannelStrip::~ChannelStrip()
{
    meterTimer_.stop();
    dropLevelMeterClient();
}

void ChannelStrip::detach()
{
    meterTimer_.stop();
    dropLevelMeterClient();
    track_ = nullptr;
    trackRef_ = nullptr;
}

void ChannelStrip::dropLevelMeterClient()
{
    // The plugin may already be gone with its track: only a live one still
    // holds our client.
    if (levelMeterPlugin_ && levelMeterRef_.get() != nullptr)
        levelMeterPlugin_->measurer.removeClient(meterClient_);
    levelMeterPlugin_ = nullptr;
    levelMeterRef_ = nullptr;
}

QColor ChannelStrip::stripColour() const
{
    if (isMaster_ || !trackAlive() || !editMgr_)
        return ThemeManager::instance().current().textDim;
    return editMgr_->getTrackColour(track_);
}

void ChannelStrip::paintEvent(QPaintEvent*)
{
    auto& theme = ThemeManager::instance().current();
    QPainter p(this);

    // Nearly flat: a faint top-to-bottom shade only.
    QLinearGradient bg(0, 0, 0, height());
    const QColor base = selected_ ? theme.surfaceLight : theme.surface;
    bg.setColorAt(0.0, base.lighter(103));
    bg.setColorAt(1.0, base.darker(104));
    p.fillRect(rect(), bg);

    // Track colour band along the top, separator on the right.
    p.fillRect(QRect(0, 0, width(), 4), stripColour());
    p.fillRect(QRect(width() - 1, 0, 1, height()), theme.separator);
    if (selected_) {
        p.setPen(QPen(theme.accent, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(1, 1, width() - 3, height() - 2));
    }
}

void ChannelStrip::applyNameStyle()
{
    if (!nameLabel_) return;
    const QColor c = stripColour();
    const auto& theme = ThemeManager::instance().current();
    const QColor bg = isMaster_ ? theme.surfaceLight : c.darker(150);
    setStyleSheetIfChanged(nameLabel_,
        QString("QLabel { color: %1; font-size: 11px; font-weight: bold; "
                "background: %2; border-left: 3px solid %3; " + radiusCss(2) + "padding: 3px; }")
            .arg(ThemeManager::readableTextOn(bg).name(), bg.name(), c.name()));
}

ChannelStrip* ChannelStrip::createMasterStrip(EditManager* editMgr,
                                               QWidget* parent)
{
    auto* strip = new ChannelStrip(nullptr, editMgr, parent);
    strip->isMaster_ = true;
    strip->reconnectLevelMeterSource();
    strip->setupMasterUI();
    return strip;
}

void ChannelStrip::setupUI(const TrackRoutingChoices* routing)
{
    auto& theme = ThemeManager::instance().current();

    // A strip built on its own (not by MixerView) queries the devices itself.
    std::optional<TrackRoutingChoices> ownRouting;
    if (!routing && track_ && editMgr_) {
        ownRouting = TrackRoutingChoices::query(*editMgr_);
        routing = &*ownRouting;
    }
    setObjectName("channelStrip");
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(92);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setAutoFillBackground(false);


    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setSpacing(3);

    // ── Top section: input routing, name, instrument, pan ──

    const bool isMidi = track_ && kind_ == TrackWidgetKind::Midi;

    inputCombo_ = new QComboBox(this);
    inputCombo_->setAccessibleName("Input Source");
    inputCombo_->setToolTip("Select audio input source");
    inputCombo_->setFixedHeight(19);
    inputCombo_->setStyleSheet(
        QString("QComboBox { background: " + theme.background.name() + "; color: %1; border: 1px solid " + theme.border.name() + "; "
                "border-radius: 3px; font-size: 9px; padding: 2px 3px; }"
                "QComboBox:hover { border: 1px solid %2; }"
                "QComboBox::drop-down { width: 14px; }"
                "QComboBox QAbstractItemView { background: %3; color: %1; "
                "selection-background-color: %4; font-size: 9px; }")
            .arg(theme.text.name(), theme.accent.name(),
                 theme.surface.name(), theme.surfaceLight.name()));
    populateInputCombo(routing);
    connect(inputCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ChannelStrip::onInputComboChanged);
    layout->addWidget(inputCombo_);

    outputCombo_ = new QComboBox(this);
    outputCombo_->setAccessibleName("Output Destination");
    outputCombo_->setToolTip("Select output destination");
    outputCombo_->setFixedHeight(19);
    outputCombo_->setStyleSheet(
        QString("QComboBox { background: " + theme.background.name() + "; color: %1; border: 1px solid " + theme.border.name() + "; "
                "border-radius: 3px; font-size: 9px; padding: 2px 3px; }"
                "QComboBox:hover { border: 1px solid %2; }"
                "QComboBox::drop-down { width: 14px; }"
                "QComboBox QAbstractItemView { background: %3; color: %1; "
                "selection-background-color: %4; font-size: 9px; }")
            .arg(theme.text.name(), theme.accent.name(),
                 theme.surface.name(), theme.surfaceLight.name()));
    populateOutputCombo(routing);
    connect(outputCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ChannelStrip::onOutputComboChanged);
    layout->addWidget(outputCombo_);

    sendsBtn_ = new QPushButton(this);
    sendsBtn_->setAccessibleName("Sends");
    sendsBtn_->setToolTip("Aux sends: copy this track into a bus, such as a shared reverb");
    sendsBtn_->setFixedHeight(19);
    sendsBtn_->setStyleSheet(
        QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                "border-radius: 3px; font-size: 9px; padding: 0 3px; text-align: left; }"
                "QPushButton:hover { border: 1px solid %4; }")
            .arg(theme.background.name(), theme.text.name(),
                 theme.border.name(), theme.accent.name()));
    connect(sendsBtn_, &QPushButton::clicked, this, [this]() {
        // Parented to the window: a routing change rebuilds the mixer's strips.
        if (trackAlive())
            SendsPanel::showFor(editMgr_, track_,
                                sendsBtn_->mapToGlobal(QPoint(0, sendsBtn_->height())),
                                window());
    });
    updateSendsButton();
    layout->addWidget(sendsBtn_);

    nameLabel_ = new QLabel(this);
    nameLabel_->setAccessibleName("Track Name");
    nameLabel_->setToolTip("Double-click to rename");
    if (trackAlive())
        nameLabel_->setText(QString::fromStdString(track_->getName().toStdString()));
    nameLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    nameLabel_->setMinimumWidth(10);
    nameLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    applyNameStyle();
    nameLabel_->installEventFilter(this);
    layout->addWidget(nameLabel_);

    if (isMidi) {
        auto* instrumentRow = new QHBoxLayout();
        instrumentRow->setSpacing(2);
        instrumentBtn_ = new QPushButton(this);
        instrumentBtn_->setAccessibleName("Select Instrument");
        updateInstrumentButton();
        instrumentBtn_->setFixedHeight(kInstrumentRowHeight);
        instrumentBtn_->setMinimumWidth(10);
        instrumentBtn_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        instrumentBtn_->setStyleSheet(
            QString("QPushButton { background: " + theme.background.name() + "; color: %1; "
                    "border: 1px solid " + theme.border.name() + "; "
                    "border-radius: 3px; font-size: 10px; padding: 0px 4px; text-align: left; }"
                    "QPushButton:hover { background: " + theme.surfaceLight.name() + "; }")
                .arg(theme.text.name()));
        connect(instrumentBtn_, &QPushButton::clicked, this, [this]() {
            if (trackAlive()) emit instrumentSelectRequested(track_);
        });
        instrumentRow->addWidget(instrumentBtn_, 1);

        midiChannelCombo_ = new QComboBox(this);
        midiChannelCombo_->setAccessibleName("MIDI Channel");
        midiChannelCombo_->setToolTip("MIDI output channel (1-16)");
        midiChannelCombo_->setFixedHeight(kInstrumentRowHeight);
        midiChannelCombo_->setFixedWidth(34);
        for (int ch = 1; ch <= 16; ++ch)
            midiChannelCombo_->addItem(QString::number(ch), ch);
        midiChannelCombo_->setStyleSheet(
            QString("QComboBox { background: " + theme.background.name() + "; color: %1; "
                    "border: 1px solid " + theme.border.name() + "; "
                    "border-radius: 3px; font-size: 10px; padding: 0px 2px; }"
                    "QComboBox:hover { border: 1px solid %2; }"
                    "QComboBox::drop-down { width: 12px; }"
                    "QComboBox QAbstractItemView { background: %3; color: %1; "
                    "selection-background-color: %4; font-size: 10px; }")
                .arg(theme.text.name(), theme.accent.name(),
                     theme.surface.name(), theme.surfaceLight.name()));

        int currentCh = 1;
        for (auto* clip : track_->getClips()) {
            if (auto* mc = dynamic_cast<te::MidiClip*>(clip)) {
                currentCh = mc->getMidiChannel().getChannelNumber();
                break;
            }
        }
        midiChannelCombo_->setCurrentIndex(std::clamp(currentCh, 1, 16) - 1);

        connect(midiChannelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &ChannelStrip::onMidiChannelChanged);
        instrumentRow->addWidget(midiChannelCombo_);
        layout->addLayout(instrumentRow);
    } else {
        layout->addSpacing(kInstrumentRowHeight + layout->spacing());
    }

    panKnob_ = new RotaryKnob(this);
    panKnob_->setAccessibleName("Pan");
    panKnob_->setRange(-1.0, 1.0);
    panKnob_->setValue(0.0);
    panKnob_->setLabel(QString());
    panKnob_->setFixedSize(34, 34);
    auto* panRow = new QHBoxLayout();
    panRow->addStretch();
    panRow->addWidget(panKnob_);
    panRow->addStretch();
    layout->addLayout(panRow);

    connect(panKnob_, &RotaryKnob::valueChanged, this, [this](double v) {
        if (isMaster_) {
            if (editMgr_ && editMgr_->edit())
                editMgr_->edit()->setMasterPanPos(float(v));
            return;
        }

        if (!trackAlive()) return;
        for (auto* plugin : track_->pluginList.getPlugins()) {
            if (auto* volPan = dynamic_cast<te::VolumeAndPanPlugin*>(plugin)) {
                volPan->panParam->setParameter(float(v), juce::sendNotification);
                break;
            }
        }
    });

    // ── Middle section: fader + meter (stretches to fill) ──

    auto* faderRow = new QHBoxLayout();
    faderRow->setSpacing(2);

    meter_ = new LevelMeter(this);
    meter_->setAccessibleName("Level Meter");
    meter_->setFixedWidth(14);
    meter_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    faderRow->addWidget(meter_);

    fader_ = new VolumeFader(this);
    fader_->setAccessibleName("Volume Fader");
    fader_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    faderRow->addWidget(fader_);

    layout->addLayout(faderRow, 1);

    volumeLabel_ = new QLabel("0.0", this);
    volumeLabel_->setAccessibleName("Volume Level dB");
    volumeLabel_->setToolTip("Volume in dB \xe2\x80\x93 double-click to type exact value");
    volumeLabel_->setAlignment(Qt::AlignCenter);
    volumeLabel_->setFixedHeight(18);
    volumeLabel_->setStyleSheet(
        QString("QLabel { color: %1; font-size: 9px; "
                "font-family: 'Consolas', 'Courier New', monospace; "
                "background: " + theme.lcdBackground.name() + "; border: 1px solid " + theme.border.name() + "; "
                "border-radius: 3px; padding: 1px 3px; }")
            .arg(theme.lcdText.name()));
    volumeLabel_->installEventFilter(this);
    layout->addWidget(volumeLabel_);
    updateVolumeLabel();

    connect(fader_, &VolumeFader::valueChanged, this, [this](double v) {
        updateVolumeLabel();
        if (isMaster_) {
            // The fader is linear in dB (-60..+6), unlike Tracktion's slider position.
            if (editMgr_ && editMgr_->edit())
                if (auto masterVol = editMgr_->edit()->getMasterVolumePlugin())
                    masterVol->setVolumeDb(v <= 0.001 ? -100.0f : float(-60.0 + v * 66.0));
            return;
        }

        if (!trackAlive()) return;
        for (auto* plugin : track_->pluginList.getPlugins()) {
            if (auto* volPan = dynamic_cast<te::VolumeAndPanPlugin*>(plugin)) {
                if (v <= 0.001) {
                    volPan->volParam->setParameter(0.0f, juce::sendNotification);
                } else {
                    double db = -60.0 + v * 66.0;
                    volPan->volParam->setParameter(te::decibelsToVolumeFaderPosition(float(db)), juce::sendNotification);
                }
                break;
            }
        }
    });

    // ── Bottom section: M / S / R buttons ──

    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(2);

    muteBtn_ = new QPushButton("M", this);
    muteBtn_->setAccessibleName("Mute");
    muteBtn_->setCheckable(true);
    muteBtn_->setFixedHeight(20);
    muteBtn_->setText("M");
    muteBtn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    applyToggleStyle(muteBtn_, theme.muteButton);
    connect(muteBtn_, &QPushButton::toggled, this, [this](bool m) {
        if (trackAlive()) track_->setMute(m);
    });

    soloBtn_ = new QPushButton(this);
    soloBtn_->setAccessibleName("Solo");
    soloBtn_->setCheckable(true);
    soloBtn_->setFixedHeight(20);
    soloBtn_->setText("S");
    soloBtn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    applyToggleStyle(soloBtn_, theme.soloButton);
    connect(soloBtn_, &QPushButton::toggled, this, [this](bool s) {
        if (trackAlive()) track_->setSolo(s);
    });

    armBtn_ = new QPushButton(this);
    armBtn_->setAccessibleName("Record Arm");
    armBtn_->setCheckable(true);
    armBtn_->setFixedHeight(20);
    armBtn_->setText("R");
    armBtn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    applyToggleStyle(armBtn_, theme.recordArm);
    connect(armBtn_, &QPushButton::toggled, this, &ChannelStrip::onArmToggled);

    monoBtn_ = new QPushButton(this);
    monoBtn_->setAccessibleName("Mono Or Stereo");
    monoBtn_->setCheckable(true);
    monoBtn_->setFixedHeight(20);
    monoBtn_->setFont(icons::fontAudio(11));
    monoBtn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    applyToggleStyle(monoBtn_, theme.accent);
    connect(monoBtn_, &QPushButton::toggled, this, [this](bool mono) {
        if (!trackAlive() || !editMgr_) return;
        editMgr_->setTrackMono(*track_, mono);
        updateMonoButtonVisual(mono);
    });

    btnRow->addWidget(muteBtn_);
    btnRow->addWidget(soloBtn_);
    btnRow->addWidget(armBtn_);
    if (!isMaster_)
        btnRow->addWidget(monoBtn_);
    layout->addLayout(btnRow);

    // Automation mode button (Off -> Read -> Write cycle)
    autoModeBtn_ = new QPushButton("OFF", this);
    autoModeBtn_->setAccessibleName("Automation Mode");
    autoModeBtn_->setToolTip("Automation mode (click to cycle):\n"
                              "READ - Play existing automation\n"
                              "TOUCH - Record only while moving a control\n"
                              "LATCH - Record from first touch, hold last value\n"
                              "WRITE - Record all params from playback start");
    autoModeBtn_->setFixedHeight(16);
    autoModeBtn_->setStyleSheet(
        QString("QPushButton { background: " + theme.background.name() + "; color: %1; "
                "border: 1px solid " + theme.border.name() + "; "
                "border-radius: 3px; font-size: 7px; font-weight: bold; padding: 0px 2px; }"
                "QPushButton:hover { border: 1px solid %2; }")
            .arg(theme.textDim.name(), theme.accent.name()));
    connect(autoModeBtn_, &QPushButton::clicked, this, [this]() {
        if (!editMgr_ || !editMgr_->edit()) return;

        te::Track* modeTrack = trackAlive() ? track_ : nullptr;
        if (isMaster_)
            modeTrack = editMgr_->edit()->getMasterTrack();
        if (!modeTrack) return;

        auto currentMode = modeTrack->automationMode.get();
        te::AutomationMode newMode;
        if (currentMode == te::AutomationMode::read)
            newMode = te::AutomationMode::touch;
        else if (currentMode == te::AutomationMode::touch)
            newMode = te::AutomationMode::latch;
        else if (currentMode == te::AutomationMode::latch)
            newMode = te::AutomationMode::write;
        else
            newMode = te::AutomationMode::read;
        modeTrack->automationMode = newMode;

        auto& arm = editMgr_->edit()->getAutomationRecordManager();
        bool anyWriting = (newMode != te::AutomationMode::read);
        arm.setReadingAutomation(true);
        arm.setWritingAutomation(anyWriting);
        updateAutoModeButton();
    });
    layout->addWidget(autoModeBtn_);
    updateAutoModeButton();

    if (track_ && editMgr_ && monoBtn_) {
        const bool mono = editMgr_->isTrackMono(track_);
        {
            QSignalBlocker block(monoBtn_);
            monoBtn_->setChecked(mono);
        }
        updateMonoButtonVisual(mono);
    }

    // Mono, like the rest of the strip, is re-read by refresh(): MixerView
    // calls it for every surviving strip on tracksChanged.

    frozenLabel_ = new QLabel("FROZEN", this);
    frozenLabel_->setAccessibleName("Track Frozen Indicator");
    frozenLabel_->setAlignment(Qt::AlignCenter);
    frozenLabel_->setStyleSheet(
        "QLabel { color: #fff; font-size: 8px; font-weight: bold; "
        "background: rgba(0,150,200,180); border-radius: 2px; padding: 1px 4px; }");
    frozenLabel_->setFixedHeight(16);
    bool isFrozen = track_ && editMgr_ && editMgr_->isTrackFrozen(track_);
    frozenLabel_->setVisible(isFrozen);
    layout->addWidget(frozenLabel_);

    if (track_ && editMgr_) {
        connect(editMgr_, &EditManager::trackFreezeStateChanged, this, [this](te::AudioTrack* t) {
            if (t != track_ || !frozenLabel_ || !editMgr_) return;
            frozenLabel_->setVisible(editMgr_->isTrackFrozen(track_));
        });
    }

    updateSelectionStyle();
}

void ChannelStrip::setupMasterUI()
{
    if (nameLabel_)
        nameLabel_->setText("Master");
    applyNameStyle();
    update();

    if (editMgr_ && editMgr_->edit()) {
        if (auto masterVol = editMgr_->edit()->getMasterVolumePlugin()) {
            // Showing the saved level must not write it back (it would round -60 dB to mute).
            if (fader_) {
                QSignalBlocker block(fader_);
                fader_->setValue(dbToNormalized(masterVol->getVolumeDb()));
            }
            if (panKnob_) {
                QSignalBlocker block(panKnob_);
                panKnob_->setValue(masterVol->getPan());
            }
            updateVolumeLabel();
        }
    }

    if (inputCombo_)
        inputCombo_->setVisible(false);
    if (outputCombo_)
        outputCombo_->setVisible(false);
    if (sendsBtn_)
        sendsBtn_->setVisible(false);
    if (armBtn_)
        armBtn_->setVisible(false);
    if (monoBtn_)
        monoBtn_->setVisible(false);
    if (muteBtn_)
        muteBtn_->setVisible(false);
    if (soloBtn_)
        soloBtn_->setVisible(false);
}

void ChannelStrip::setSelected(bool selected)
{
    if (selected_ == selected)
        return;
    selected_ = selected;
    updateSelectionStyle();
}

void ChannelStrip::refresh(const TrackRoutingChoices* routing)
{
    if (isMaster_) {
        if (editMgr_ && editMgr_->edit()) {
            if (auto masterVol = editMgr_->edit()->getMasterVolumePlugin()) {
                if (fader_) {
                    QSignalBlocker block(fader_);
                    fader_->setValue(dbToNormalized(masterVol->getVolumeDb()));
                }
                if (panKnob_) {
                    QSignalBlocker block(panKnob_);
                    panKnob_->setValue(masterVol->getPan());
                }
            }
        }
        updateVolumeLabel();
        return;
    }

    if (!trackAlive()) return;
    const QString name = QString::fromStdString(track_->getName().toStdString());
    if (nameLabel_->text() != name)
        nameLabel_->setText(name);
    applyNameStyle();
    updateSendsButton();
    update();
    {
        QSignalBlocker block(muteBtn_);
        muteBtn_->setChecked(track_->isMuted(false));
    }
    {
        QSignalBlocker block(soloBtn_);
        soloBtn_->setChecked(track_->isSolo(false));
    }
    if (editMgr_ && monoBtn_) {
        const bool mono = editMgr_->isTrackMono(track_);
        QSignalBlocker block(monoBtn_);
        monoBtn_->setChecked(mono);
        updateMonoButtonVisual(mono);
    }
    if (editMgr_ && armBtn_) {
        QSignalBlocker block(armBtn_);
        armBtn_->setChecked(editMgr_->isTrackRecordEnabled(track_));
    }

    for (auto* plugin : track_->pluginList.getPlugins()) {
        if (auto* vp = dynamic_cast<te::VolumeAndPanPlugin*>(plugin)) {
            if (fader_) {
                float dbVal = te::volumeFaderPositionToDB(vp->volParam->getCurrentValue());
                double norm = (dbVal + 60.0) / 66.0;
                QSignalBlocker block(fader_);
                fader_->setValue(norm);
            }
            if (panKnob_) {
                QSignalBlocker block(panKnob_);
                panKnob_->setValue(vp->pan.get());
            }
            break;
        }
    }
    updateVolumeLabel();
    updateAutoModeButton();

    populateInputCombo(routing);
    populateOutputCombo(routing);
    updateInstrumentButton();
    if (frozenLabel_ && editMgr_) {
        const bool frozen = editMgr_->isTrackFrozen(track_);
        if (frozenLabel_->isVisibleTo(this) != frozen)
            frozenLabel_->setVisible(frozen);
    }
    // The track's plugin list may have changed (or the track been replaced
    // by an undo): follow its current meter.
    if (track_->getLevelMeterPlugin() != levelMeterPlugin_ || levelMeterRef_.get() == nullptr)
        reconnectLevelMeterSource();
    if (midiChannelCombo_ && trackAlive()) {
        int ch = 1;
        for (auto* clip : track_->getClips()) {
            if (auto* mc = dynamic_cast<te::MidiClip*>(clip)) {
                ch = mc->getMidiChannel().getChannelNumber();
                break;
            }
        }
        const int idx = std::clamp(ch, 1, 16) - 1;
        if (midiChannelCombo_->currentIndex() != idx) {
            QSignalBlocker block(midiChannelCombo_);
            midiChannelCombo_->setCurrentIndex(idx);
        }
    }
}

void ChannelStrip::updateInstrumentButton()
{
    if (!instrumentBtn_ || !trackAlive() || !editMgr_) return;
    auto* instrument = editMgr_->getTrackInstrument(track_);
    const QString instrName = instrument
        ? QString::fromStdString(instrument->getName().toStdString())
        : "No Instrument";
    if (instrumentBtn_->text() != instrName) {
        instrumentBtn_->setText(instrName);
        instrumentBtn_->setToolTip(instrName + "\nClick to change the instrument");
    }
}

void ChannelStrip::updateMeter()
{
    if (!editMgr_ || !editMgr_->edit())
        return;

    if (levelMeterPlugin_) {
        auto levelL = meterClient_.getAndClearAudioLevel(0);
        auto levelR = meterClient_.getAndClearAudioLevel(1);

        if (isMaster_) {
            if (auto masterVol = editMgr_->edit()->getMasterVolumePlugin()) {
                const float masterDb = masterVol->getVolumeDb();
                levelL.dB += masterDb;
                levelR.dB += masterDb;
            }
        }

        meter_->setLevel(dbToNormalized(levelL.dB), dbToNormalized(levelR.dB));
    }

    // Only track automation values to controls when playing, or when the
    // playhead has been scrubbed to a new position. When stopped and
    // stationary, don't fight with the user's manual fader/knob drags.
    bool isPlaying = editMgr_->transport().isPlaying();
    double currentSecs = editMgr_->transport().getPosition().inSeconds();
    bool playheadMoved = std::abs(currentSecs - lastTrackedPlayheadSecs_) > 0.001;
    bool shouldTrack = isPlaying || playheadMoved;
    lastTrackedPlayheadSecs_ = currentSecs;

    if (!shouldTrack) return;

    if (isMaster_) {
        if (auto masterVol = editMgr_->edit()->getMasterVolumePlugin()) {
            auto* mt = editMgr_->edit()->getMasterTrack();
            auto mode = mt ? mt->automationMode.get() : te::AutomationMode::read;
            bool isWriting = isPlaying && (mode == te::AutomationMode::write);
            if (fader_ && !isWriting) {
                QSignalBlocker block(fader_);
                fader_->setValue(dbToNormalized(masterVol->getVolumeDb()));
            }
            if (panKnob_ && !isWriting) {
                QSignalBlocker block(panKnob_);
                panKnob_->setValue(masterVol->getPan());
            }
            updateVolumeLabel();
        }
        return;
    }

    if (!trackAlive()) return;

    auto mode = track_->automationMode.get();
    bool isWriting = isPlaying && (mode == te::AutomationMode::write);

    for (auto* plugin : track_->pluginList.getPlugins()) {
        if (auto* vp = dynamic_cast<te::VolumeAndPanPlugin*>(plugin)) {
            if (fader_ && !isWriting) {
                float dbVal = te::volumeFaderPositionToDB(vp->volParam->getCurrentValue());
                double norm = (dbVal + 60.0) / 66.0;
                QSignalBlocker block(fader_);
                fader_->setValue(std::clamp(norm, 0.0, 1.0));
            }
            if (panKnob_ && !isWriting) {
                QSignalBlocker block(panKnob_);
                panKnob_->setValue(vp->panParam->getCurrentValue());
            }
            updateVolumeLabel();
            break;
        }
    }
}

void ChannelStrip::reconnectLevelMeterSource()
{
    dropLevelMeterClient();

    if (!editMgr_ || !editMgr_->edit())
        return;

    if (isMaster_) {
        auto masterMeters = editMgr_->edit()
                                ->getMasterPluginList()
                                .getPluginsOfType<te::LevelMeterPlugin>();
        levelMeterPlugin_ = masterMeters.isEmpty() ? nullptr : masterMeters.getLast();
    } else if (trackAlive()) {
        levelMeterPlugin_ = track_->getLevelMeterPlugin();
    }

    if (levelMeterPlugin_) {
        levelMeterRef_ = levelMeterPlugin_;
        levelMeterPlugin_->measurer.addClient(meterClient_);
    }
}

void ChannelStrip::applyToggleStyle(QPushButton* btn, const QColor& activeColor)
{
    auto& theme = ThemeManager::instance().current();
    btn->setStyleSheet(
        QString("QPushButton { background: %4; color: %1; border: 1px solid %5; "
                + radiusCss() + "font-weight: bold; font-size: 11px; padding: 1px 2px; }"
                "QPushButton:hover { background: %6; color: %7; }"
                "QPushButton:checked { background: %2; color: #111; border-color: %3; }")
            .arg(theme.textDim.name(), activeColor.name(),
                 activeColor.darker(130).name(), theme.background.name(),
                 theme.border.name(), theme.surfaceLight.name(), theme.text.name()));
}

void ChannelStrip::updateMonoButtonVisual(bool mono)
{
    if (!monoBtn_)
        return;

    monoBtn_->setText(QString(mono ? icons::fa::Mono : icons::fa::Stereo));
    monoBtn_->setToolTip(mono ? "Track is mono" : "Track is stereo");
}

void ChannelStrip::updateAutoModeButton()
{
    if (!autoModeBtn_) return;
    auto& theme = ThemeManager::instance().current();

    auto mode = te::AutomationMode::read;
    if (trackAlive()) {
        mode = track_->automationMode.get();
    } else if (isMaster_ && editMgr_ && editMgr_->edit()) {
        if (auto* mt = editMgr_->edit()->getMasterTrack())
            mode = mt->automationMode.get();
    }

    QString text;
    QColor bg, fg;
    switch (mode) {
        case te::AutomationMode::write:
            text = "WRITE";
            bg = theme.recordArm;
            fg = QColor(Qt::white);
            break;
        case te::AutomationMode::latch:
            text = "LATCH";
            bg = theme.muteButton;
            fg = QColor(Qt::white);
            break;
        case te::AutomationMode::touch:
            text = "TOUCH";
            bg = theme.soloButton.darker(130);
            fg = QColor(Qt::white);
            break;
        case te::AutomationMode::read:
        default:
            text = "READ";
            bg = theme.meterGreen.darker(140);
            fg = theme.isLight ? QColor(Qt::white) : theme.meterGreen;
            break;
    }

    if (autoModeBtn_->text() != text)
        autoModeBtn_->setText(text);
    setStyleSheetIfChanged(autoModeBtn_,
        QString("QPushButton { background: rgba(%1,%2,%3,%4); color: %5; "
                "border: 1px solid " + theme.border.name() + "; "
                "border-radius: 3px; font-size: 7px; font-weight: bold; padding: 0px 2px; }"
                "QPushButton:hover { border: 1px solid %6; }")
            .arg(bg.red()).arg(bg.green()).arg(bg.blue()).arg(180)
            .arg(fg.name(), theme.accent.name()));
}

void ChannelStrip::populateInputCombo(const TrackRoutingChoices* routing)
{
    // The master strip's combos are hidden; track strips only.
    if (!inputCombo_ || !editMgr_ || !trackAlive()) return;

    if (routing)
        setComboItemsIfChanged(inputCombo_,
                               routing->inputItemsFor(kind_ == TrackWidgetKind::Midi));

    const QString currentInput = editMgr_->getTrackInputName(track_);
    if (currentInput.isEmpty()) {
        if (inputCombo_->currentIndex() != 0) {
            QSignalBlocker block(inputCombo_);
            inputCombo_->setCurrentIndex(0);
        }
    } else {
        selectComboData(inputCombo_, currentInput, "midi:" + currentInput);
    }
}

void ChannelStrip::onInputComboChanged(int index)
{
    if (!trackAlive() || !editMgr_ || index < 0) return;

    QString deviceName = inputCombo_->itemData(index).toString();
    if (deviceName.isEmpty()) {
        editMgr_->clearTrackInput(*track_);
        if (armBtn_) {
            QSignalBlocker block(armBtn_);
            armBtn_->setChecked(false);
        }
    } else if (deviceName.startsWith("midi:")) {
        editMgr_->assignMidiInputToTrack(*track_,
            juce::String(deviceName.mid(5).toStdString()));
    } else {
        editMgr_->assignInputToTrack(*track_,
            juce::String(deviceName.toStdString()));
    }
}

void ChannelStrip::populateOutputCombo(const TrackRoutingChoices* routing)
{
    if (!outputCombo_ || !editMgr_ || !trackAlive()) return;

    if (routing)
        setComboItemsIfChanged(outputCombo_,
                               toComboItems(routing->outputChoicesFor(*editMgr_, track_)));
    selectComboData(outputCombo_, editMgr_->currentOutputKey(track_));
}

void ChannelStrip::onOutputComboChanged(int index)
{
    if (!trackAlive() || !editMgr_ || index < 0) return;
    editMgr_->setTrackOutputByKey(*track_, outputCombo_->itemData(index).toString());
}

void ChannelStrip::onArmToggled(bool armed)
{
    if (!trackAlive() || !editMgr_) return;

    QString currentInput = editMgr_->getTrackInputName(track_);
    if (armed && currentInput.isEmpty()) {
        QSignalBlocker block(armBtn_);
        armBtn_->setChecked(false);
        QToolTip::showText(armBtn_->mapToGlobal(QPoint(0, -30)),
                           "Select an input source first",
                           armBtn_, {}, 2500);
        return;
    }

    editMgr_->setTrackRecordEnabled(*track_, armed);
}

void ChannelStrip::onMidiChannelChanged(int index)
{
    if (!trackAlive() || index < 0) return;
    int ch = midiChannelCombo_->itemData(index).toInt();
    te::MidiChannel midiCh(ch);
    for (auto* clip : track_->getClips()) {
        if (auto* mc = dynamic_cast<te::MidiClip*>(clip))
            mc->setMidiChannel(midiCh);
    }
}

void ChannelStrip::updateVolumeLabel()
{
    if (!volumeLabel_ || !fader_) return;
    double db = fader_->valueDb();
    if (fader_->value() <= 0.001)
        volumeLabel_->setText(QString::fromUtf8("-\xe2\x88\x9e dB"));
    else
        volumeLabel_->setText(QString("%1 dB").arg(db, 0, 'f', 1));
}

void ChannelStrip::startVolumeEdit()
{
    if (!fader_) return;

    auto* edit = new QLineEdit(this);
    edit->setAccessibleName("Volume dB Input");
    double db = fader_->valueDb();
    edit->setText(fader_->value() <= 0.001 ? "-60" : QString::number(db, 'f', 1));
    edit->selectAll();
    edit->setGeometry(volumeLabel_->geometry());
    edit->setAlignment(Qt::AlignCenter);
    edit->setStyleSheet(
        "QLineEdit { background: #222; color: #eee; border: 1px solid #888; "
        "font-size: 9px; font-family: monospace; padding: 0px 2px; border-radius: 2px; }");
    edit->show();
    edit->setFocus();
    volumeLabel_->hide();

    auto* faderPtr = fader_;
    auto* volLabel = volumeLabel_;

    connect(edit, &QLineEdit::editingFinished, edit, [edit, faderPtr, volLabel]() {
        edit->hide();
        edit->deleteLater();
        if (volLabel) volLabel->show();

        bool ok = false;
        double db = edit->text().trimmed().remove("dB", Qt::CaseInsensitive).trimmed().toDouble(&ok);
        if (!ok) return;
        db = std::clamp(db, faderPtr->valueDb() - 60.0, 6.0);
        db = std::clamp(db, -60.0, 6.0);
        double norm = (db + 60.0) / 66.0;
        faderPtr->setValue(norm);
    });
}

bool ChannelStrip::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == nameLabel_ && event->type() == QEvent::MouseButtonDblClick && track_) {
        startRenameEdit();
        return true;
    }
    if (obj == volumeLabel_ && event->type() == QEvent::MouseButtonDblClick) {
        startVolumeEdit();
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

void ChannelStrip::startRenameEdit()
{
    if (!trackAlive() || !editMgr_) return;

    auto* edit = new QLineEdit(this);
    edit->setAccessibleName("Rename Track");
    edit->setText(QString::fromStdString(track_->getName().toStdString()));
    edit->selectAll();
    edit->setGeometry(nameLabel_->geometry());
    edit->setAlignment(Qt::AlignCenter);
    edit->setStyleSheet(
        "QLineEdit { background: #222; color: #eee; border: 1px solid #888; "
        "font-size: 10px; font-weight: bold; padding: 1px 2px; border-radius: 2px; }");
    edit->show();
    edit->setFocus();
    nameLabel_->hide();

    auto trackId = trackId_;
    auto* mgr = editMgr_;
    QPointer<QLabel> nameLbl = nameLabel_;

    connect(edit, &QLineEdit::editingFinished, edit, [edit, trackId, mgr, nameLbl]() {
        QString newName = edit->text().trimmed();
        edit->hide();
        edit->deleteLater();
        // The strip survives the rename now (it is no longer rebuilt), so
        // bring its label back here.
        if (nameLbl) {
            if (!newName.isEmpty())
                nameLbl->setText(newName);
            nameLbl->show();
        }
        if (!newName.isEmpty() && mgr) {
            QTimer::singleShot(0, mgr, [trackId, mgr, newName]() {
                for (auto* t : mgr->getAudioTracks()) {
                    if (t->itemID.getRawID() == trackId) {
                        t->setName(juce::String(newName.toStdString()));
                        emit mgr->tracksChanged();
                        return;
                    }
                }
            });
        }
    });
}

void ChannelStrip::updateSelectionStyle()
{
    update();
}

void ChannelStrip::updateSendsButton()
{
    if (!sendsBtn_ || !trackAlive() || !editMgr_) return;
    const auto sends = editMgr_->getTrackSends(track_);
    if (sends.empty()) {
        sendsBtn_->setText(QString::fromUtf8("Sends \u25BE"));
        return;
    }
    QStringList names;
    for (const auto& send : sends)
        names << QString::fromStdString(send.bus->getName().toStdString());
    sendsBtn_->setText(QString::fromUtf8("\u2192 ") + names.join(", "));
    sendsBtn_->setToolTip("Sends to " + names.join(", ") + ". Click to change their levels.");
}

} // namespace OpenDaw
