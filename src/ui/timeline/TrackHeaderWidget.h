#pragma once

#include "engine/EditManager.h"
#include "ui/controls/LevelMeter.h"
#include "ui/controls/RotaryKnob.h"
#include "utils/ThemeManager.h"
#include "ui/ViewRefreshSupport.h"
#include <QWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QFrame>
#include <QTimer>
#include <tracktion_engine/tracktion_engine.h>
#include <optional>
#include <vector>

namespace te = tracktion::engine;

namespace OpenDaw {

class TrackHeaderWidget : public QWidget {
    Q_OBJECT

public:
    // `routing` is the rebuild's shared input/output list (queried here when null).
    TrackHeaderWidget(te::AudioTrack* track, EditManager* editMgr,
                      QWidget* parent = nullptr,
                      const TrackRoutingChoices* routing = nullptr);
    ~TrackHeaderWidget() override;

    te::AudioTrack* track() const { return track_; }
    uint64_t trackId() const { return trackId_; }
    // False once the track object was destroyed (or after detach()).
    bool trackAlive() const { return track_ && trackRef_.get() != nullptr; }
    // The kind of track this header was built for (MIDI headers have an
    // instrument row, audio ones a takes button); a change means a new header.
    TrackWidgetKind kind() const { return kind_; }
    // Before deleteLater(): drop the track and the meter, stop the timer.
    void detach();
    void setTrackHeight(int h);
    void setSelected(bool sel);
    void setAutomationVisible(bool visible);
    void setTakesVisible(bool visible);
    void setCollapsed(bool collapsed);
    bool isCollapsed() const { return collapsed_; }
    void setIndented(bool indented);
    // Re-reads the track. With `routing` the input/output combo lists are
    // updated too (otherwise they only reselect). `anySoloed`: whether any
    // track is soloed, when the caller already knows (saves a track scan).
    void refresh(const TrackRoutingChoices* routing = nullptr,
                 std::optional<bool> anySoloed = std::nullopt);
    bool isSelected() const { return selected_; }
    QSize minimumSizeHint() const override;

signals:
    void trackSelected(te::AudioTrack* track);
    void effectInsertRequested(te::AudioTrack* track);
    void instrumentSelectRequested(te::AudioTrack* track);
    void splitHandsRequested(te::AudioTrack* track);
    void automationToggled(te::AudioTrack* track, bool visible);
    void takesToggled(te::AudioTrack* track, bool visible);
    void collapseToggled(te::AudioTrack* track, bool collapsed);
    void dragStarted(TrackHeaderWidget* header);
    void dragMoved(TrackHeaderWidget* header, int globalY);
    void dragFinished(TrackHeaderWidget* header);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void updateMeter();
    void updateSelectionStyle();
    void updateRowVisibility();
    void refreshNameStyle(std::optional<bool> anySoloed = std::nullopt);
    void updateInstrumentButton();
    void updateTakesButton();
    void dropLevelMeterClient();

    static constexpr int kLeftMargin = metrics::kTrackColourStrip + 6;
    static constexpr int kVMargin = 4;
    static constexpr int kRowSpacing = 3;
    static constexpr int kNameRowHeight = 20;
    static constexpr int kButtonRowHeight = 18;
    static constexpr int kDetailRowHeight = 20;
    void applyToggleStyle(QPushButton* btn, const QColor& activeColor);
    void updateMonoButtonVisual(bool mono);
    void updateFreezeButtonVisual(bool frozen);
    void populateInputCombo(const TrackRoutingChoices* routing);
    void onInputComboChanged(int index);
    void populateOutputCombo(const TrackRoutingChoices* routing);
    void onOutputComboChanged(int index);
    void onArmToggled(bool armed);
    void startRenameEdit();

    te::AudioTrack* track_;
    juce::WeakReference<te::Selectable> trackRef_;
    uint64_t trackId_ = 0;
    TrackWidgetKind kind_ = TrackWidgetKind::Audio;
    EditManager* editMgr_;
    int frozenShown_ = -1;   // freeze look last applied (-1: none yet)
    bool selected_ = false;
    bool collapsed_ = false;

    QComboBox* inputCombo_ = nullptr;
    QComboBox* outputCombo_ = nullptr;
    QLabel* nameLabel_;
    QLabel* typeLabel_ = nullptr;
    QWidget* nameRow_ = nullptr;
    QWidget* buttonRow_ = nullptr;
    std::vector<QWidget*> optionalRows_;
    QColor trackColour_;
    bool indented_ = false;
    QPushButton* instrumentBtn_ = nullptr;
    QPushButton* muteBtn_;
    QPushButton* soloBtn_;
    QPushButton* armBtn_;
    QPushButton* monoBtn_ = nullptr;
    QPushButton* freezeBtn_ = nullptr;
    QSlider* volumeSlider_;
    QSlider* panKnob_;   // pan, -100..100
    LevelMeter* meter_;
    QPushButton* collapseBtn_ = nullptr;
    QPushButton* automationBtn_ = nullptr;
    QPushButton* takesBtn_ = nullptr;
    QTimer meterTimer_;
    bool automationVisible_ = false;
    QWidget* gripHandle_ = nullptr;
    QPoint dragStartPos_;
    bool draggingToReorder_ = false;
    te::LevelMeasurer::Client meterClient_;
    te::LevelMeterPlugin* levelMeterPlugin_ = nullptr;
    juce::WeakReference<te::Selectable> levelMeterRef_;   // guards levelMeterPlugin_
    double lastTrackedPlayheadSecs_ = -1.0;
};

} // namespace OpenDaw
