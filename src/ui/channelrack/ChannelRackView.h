#pragma once

#include "engine/EditManager.h"
#include "ChannelRow.h"
#include "ChannelSound.h"
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QMimeData;

class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace OpenDaw {

class PatternManager;
class PatternPlayer;
class PluginScanner;
class SoundPicker;
class StepRuler;

// The Channel Rack (FL Studio style): one row per channel with a step
// sequencer for the current pattern. A channel is a sound (a sample, a
// built-in instrument or a VST/VST3 plugin such as Surge XT) that plays the
// pattern's notes; the rack shows the sound, never the track behind it.
// Steps, patterns and the current pattern live in PatternManager, so the rack
// can be rebuilt (theme change) at any time; it only keeps the row widgets.
//
// "+" opens the sound picker (SoundPicker): each pick makes a new channel
// playing that sound; a row's right-click "Replace sound" uses the same
// picker and keeps the channel's notes. Clicking a channel's name auditions
// it; double-clicking opens the plugin's window (VST/Surge) or the piano roll
// on the channel's notes in the current pattern.
//
// Audio files dropped on the rack (from the browser or the system file
// manager) become sample channels; dropped on a channel row, the first file
// replaces that channel's sound.
class ChannelRackView : public QWidget {
    Q_OBJECT

public:
    ChannelRackView(EditManager* editMgr, PatternManager* patterns, PatternPlayer* player,
                    QWidget* parent = nullptr);

    // Highlights a channel (from the timeline / mixer selection); null clears.
    void setSelectedTrack(te::AudioTrack* track);
    // The plugin list for the sound picker's "Plugins" menu (and for
    // recording a Surge XT scan). Without it the menu says it's unavailable.
    void setPluginScanner(PluginScanner* scanner);

    // Adds one sample channel per audio file (others are skipped) and selects
    // the last one. Returns how many were added; reports through statusMessage.
    int addSampleChannels(const QStringList& paths);
    // Gives an existing channel a new sample (one undo step).
    bool replaceChannelSample(te::EditItemID trackId, const QString& path);
    // The "Add channel" menu (the sound picker): each pick adds a channel.
    void showAddChannelMenu(const QPoint& globalPos);
    // Adds the channel(s) for a picked sound and selects the (last) new one.
    void addChannelWithSound(const SoundChoice& choice);
    // Replaces a channel's sound, keeping its notes (one undo step).
    bool replaceChannelSound(te::EditItemID trackId, const SoundChoice& choice);
    // The local audio files in a drag (by extension; not opened).
    static QStringList audioFilesIn(const QMimeData* mime);

signals:
    void trackSelected(te::AudioTrack* track);
    // Legacy (the rack picks sounds itself now); no longer emitted.
    void instrumentSelectRequested(te::AudioTrack* track);
    // "Scan for plugins..." in the sound picker.
    void scanPluginsRequested();
    void placePatternRequested();
    void statusMessage(const QString& text, int timeoutMs);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void buildHeader(QVBoxLayout* layout);
    QWidget* buildEmptyState(QWidget* parent);
    void scheduleRebuild();
    void rebuildRows();
    // commitPending: send step edits still waiting in the rows first (not when
    // the Edit is about to be replaced).
    void clearRows(bool commitPending = true);
    ChannelRow* rowAtGlobal(const QPoint& globalPos) const;
    void setDropRow(ChannelRow* row);
    void chooseSampleFiles();
    void updateEmptyState();
    void refreshSteps();
    void refreshControls();
    void refreshHeader();
    void updatePlayingStep(int step);
    // The current pattern, creating "Pattern 1" when the Edit has none yet.
    int ensureCurrentPattern();

    void onStepsEdited(te::EditItemID trackId, const QVector<PatternStep>& steps);
    void selectChannel(te::EditItemID trackId);
    // Double-click on a channel: the plugin's window, else the piano roll.
    void openChannel(te::EditItemID trackId);
    void openPianoRoll(te::EditItemID trackId);
    bool openPluginWindow(te::EditItemID trackId);
    void showChannelMenu(te::EditItemID trackId, const QPoint& globalPos);
    void renameChannel(te::EditItemID trackId);
    void cloneChannel(te::EditItemID trackId);
    void deleteChannel(te::EditItemID trackId);
    void selectNewChannel(te::AudioTrack* track);

    EditManager* editMgr_;
    PatternManager* patterns_;
    PatternPlayer* player_;

    QLabel* patternChip_ = nullptr;
    QLabel* patternLabel_ = nullptr;
    QComboBox* lengthCombo_ = nullptr;
    QPushButton* placeBtn_ = nullptr;
    QToolButton* addChannelBtn_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* content_ = nullptr;
    QVBoxLayout* rowsLayout_ = nullptr;
    StepRuler* ruler_ = nullptr;
    QWidget* rulerRow_ = nullptr;
    QWidget* emptyState_ = nullptr;
    QPushButton* emptyAddSampleBtn_ = nullptr;
    QPushButton* emptyAddInstrumentBtn_ = nullptr;
    QPushButton* addRowBtn_ = nullptr;
    QWidget* addRowWidget_ = nullptr;
    SoundPicker* picker_ = nullptr;

    QVector<QPointer<ChannelRow>> rows_;
    QPointer<ChannelRow> dropRow_;
    te::EditItemID selectedId_;
    bool rebuildQueued_ = false;
};

} // namespace OpenDaw
