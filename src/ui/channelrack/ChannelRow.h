#pragma once

#include "ChannelSound.h"
#include "engine/EditManager.h"
#include "engine/PatternManager.h"
#include <QWidget>
#include <functional>

namespace OpenDaw {

class RotaryKnob;
class StepRow;

// The round lamp at the start of a channel row: lit while the channel plays,
// dark when it is muted. Click toggles the mute.
class ChannelLed : public QWidget {
public:
    explicit ChannelLed(QWidget* parent = nullptr);
    void setLit(bool lit);
    bool isLit() const { return lit_; }
    std::function<void()> onClicked;

    QSize sizeHint() const override { return {14, 14}; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    bool lit_ = true;
};

// The channel name button, FL Studio style: it shows the channel's sound
// (a badge for the kind of source, then the name). Click auditions the sound
// and selects the channel, double-click opens it (the plugin's window, or the
// piano roll), right-click shows the channel menu.
class ChannelNameButton : public QWidget {
public:
    explicit ChannelNameButton(QWidget* parent = nullptr);
    void setText(const QString& text);
    // The badge before the name: waveform = sample, plug = VST/VST3 (Surge),
    // piano = built-in instrument, '?' = no instrument yet.
    void setKind(ChannelSoundKind kind);
    ChannelSoundKind kind() const { return kind_; }
    void setColour(const QColor& colour);
    void setSelected(bool selected);
    std::function<void()> onClicked;
    std::function<void()> onDoubleClicked;
    std::function<void(const QPoint& globalPos)> onContextMenu;

    QSize sizeHint() const override { return {kWidth, 22}; }
    static constexpr int kWidth = 132;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QString text_;
    ChannelSoundKind kind_ = ChannelSoundKind::None;
    QColor colour_;
    bool selected_ = false;
    bool hover_ = false;
};

// One channel of the rack: mute lamp, pan and volume knobs, name button and
// the step row. It refers to its track by id and looks the track up on every
// use, so a track removed behind its back is simply ignored.
class ChannelRow : public QWidget {
    Q_OBJECT

public:
    // patterns may be null (no sample channel support then).
    ChannelRow(EditManager* editMgr, PatternManager* patterns, te::AudioTrack* track,
               QWidget* parent = nullptr);

    te::EditItemID trackId() const { return trackId_; }
    StepRow* steps() const { return steps_; }
    // The sample file of a sample channel, or empty.
    QString samplePath() const { return samplePath_; }
    ChannelSoundKind soundKind() const { return kind_; }

    // Name, sound, colour, mute and knob positions, from the track.
    void refreshControls();
    void setSelected(bool selected);
    // Outline shown while audio files are dragged over the row.
    void setDropHighlight(bool on);
    // Plays the channel's root note for a moment (auditions its sound).
    void previewSound();

    static constexpr int kPreviewMs = 300;

    // Width of everything left of the step row (the ruler lines up with it).
    static constexpr int kLeftWidth = 4 + 14 + 6 + 24 + 2 + 24 + 6 + ChannelNameButton::kWidth + 8;
    static constexpr int kHeight = 28;

signals:
    void selectRequested(te::EditItemID trackId);
    // Double-click on the name: open the channel (plugin window / piano roll).
    void openRequested(te::EditItemID trackId);
    void menuRequested(te::EditItemID trackId, const QPoint& globalPos);
    void stepsEdited(te::EditItemID trackId, const QVector<PatternStep>& steps);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    te::AudioTrack* track() const;

    EditManager* editMgr_;
    PatternManager* patterns_ = nullptr;
    te::EditItemID trackId_;
    QString samplePath_;
    ChannelSoundKind kind_ = ChannelSoundKind::None;
    bool selected_ = false;
    bool dropHighlight_ = false;

    ChannelLed* led_ = nullptr;
    RotaryKnob* panKnob_ = nullptr;
    RotaryKnob* volumeKnob_ = nullptr;
    ChannelNameButton* nameBtn_ = nullptr;
    StepRow* steps_ = nullptr;
};

} // namespace OpenDaw
