#pragma once

#include "engine/EditManager.h"
#include "ui/controls/VolumeFader.h"
#include "ui/controls/RotaryKnob.h"
#include "ui/controls/LevelMeter.h"
#include "ui/ViewRefreshSupport.h"
#include <QWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QPixmap>
#include <tracktion_engine/tracktion_engine.h>

namespace OpenDaw {

class ChannelStrip : public QWidget {
    Q_OBJECT

public:
    // `routing` is the rebuild's shared input/output list (queried here when null).
    ChannelStrip(te::AudioTrack* track, EditManager* editMgr,
                 QWidget* parent = nullptr,
                 const TrackRoutingChoices* routing = nullptr);
    ~ChannelStrip() override;

    static ChannelStrip* createMasterStrip(EditManager* editMgr,
                                           QWidget* parent = nullptr);

    QSize sizeHint() const override { return {92, 400}; }

    te::AudioTrack* track() const { return track_; }
    // False once the track object was destroyed (or after detach()): the
    // strip then never dereferences track_.
    bool trackAlive() const { return track_ && trackRef_.get() != nullptr; }
    // The track's EditItemID (kept after detach()).
    uint64_t trackId() const { return trackId_; }
    // The kind of track this strip was built for (MIDI strips have an
    // instrument row); MixerView recreates a strip whose track changed kind.
    TrackWidgetKind kind() const { return kind_; }
    // Re-reads the track. Without `routing` the input/output combos only
    // reselect the current entry; with it their item lists are updated too.
    void refresh(const TrackRoutingChoices* routing = nullptr);
    // Called before deleteLater(): stop touching the (possibly deleted) track.
    void detach();
    void setSelected(bool selected);
    bool isSelected() const { return selected_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

signals:
    void instrumentSelectRequested(te::AudioTrack* track);

private:
    static constexpr int kInstrumentRowHeight = 19;
    QColor stripColour() const;
    void applyNameStyle();
    void setupUI(const TrackRoutingChoices* routing);
    void setupMasterUI();
    void updateMeter();
    void reconnectLevelMeterSource();
    void updateSelectionStyle();
    void applyToggleStyle(QPushButton* btn, const QColor& activeColor);
    void updateMonoButtonVisual(bool mono);
    void updateAutoModeButton();
    void updateVolumeLabel();
    void startVolumeEdit();
    void populateInputCombo(const TrackRoutingChoices* routing);
    void onInputComboChanged(int index);
    void populateOutputCombo(const TrackRoutingChoices* routing);
    void updateInstrumentButton();
    void onOutputComboChanged(int index);
    void updateSendsButton();
    void onArmToggled(bool armed);
    void onMidiChannelChanged(int index);
    void startRenameEdit();

    te::AudioTrack* track_ = nullptr;
    juce::WeakReference<te::Selectable> trackRef_;
    uint64_t trackId_ = 0;
    TrackWidgetKind kind_ = TrackWidgetKind::Audio;
    EditManager* editMgr_;
    bool isMaster_ = false;
    bool selected_ = false;

    QComboBox* inputCombo_ = nullptr;
    QComboBox* outputCombo_ = nullptr;
    QPushButton* sendsBtn_ = nullptr;
    QComboBox* midiChannelCombo_ = nullptr;
    QLabel* nameLabel_ = nullptr;
    QPushButton* instrumentBtn_ = nullptr;
    VolumeFader* fader_ = nullptr;
    RotaryKnob* panKnob_ = nullptr;
    LevelMeter* meter_ = nullptr;
    QPushButton* muteBtn_ = nullptr;
    QPushButton* soloBtn_ = nullptr;
    QPushButton* armBtn_ = nullptr;
    QPushButton* monoBtn_ = nullptr;
    QPushButton* autoModeBtn_ = nullptr;
    QLabel* volumeLabel_ = nullptr;
    QLabel* frozenLabel_ = nullptr;

    QTimer meterTimer_;
    te::LevelMeasurer::Client meterClient_;
    te::LevelMeterPlugin* levelMeterPlugin_ = nullptr;
    // Guards levelMeterPlugin_: a strip now outlives tracksChanged, and the
    // meter plugin can go (plugin list rebuilt, track deleted) while it does.
    juce::WeakReference<te::Selectable> levelMeterRef_;
    void dropLevelMeterClient();
    double lastTrackedPlayheadSecs_ = -1.0;

};

} // namespace OpenDaw
