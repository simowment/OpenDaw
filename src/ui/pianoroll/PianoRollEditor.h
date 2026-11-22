#pragma once

#include "PianoKeyboard.h"
#include "NoteGrid.h"
#include "VelocityLane.h"
#include "CcLane.h"
#include "PianoRollRuler.h"
#include <QWidget>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QSplitter>
#include <QScrollArea>
#include <QVBoxLayout>
#include <tracktion_engine/tracktion_engine.h>
#include <vector>
#include <set>
#include <functional>

namespace te = tracktion::engine;

namespace OpenDaw {

class EditManager;

// The piano roll. It edits one MIDI clip (with its linked channel clips), or
// one channel of a pattern: then the header reads "Pattern: <name> — Channel:
// <channel>" in the pattern's colour, the editable region is the pattern
// length, and the clip edited is one placement's clip of that channel (the
// pattern manager copies the edits to every placement). A pattern channel
// with no placement yet shows empty; its clip is made by the clip factory
// when the first note is drawn or typed.
//
// Previews (piano keys, clicking/drawing/dragging notes, musical typing) play
// the loaded clip's track on the clip's MIDI channel; with no clip, the
// pattern channel's track, else the preview track (the selected channel).
class PianoRollEditor : public QWidget {
    Q_OBJECT

public:
    struct PatternContext {
        int patternId = 0;          // 0 = not showing a pattern
        QString patternName;
        QColor colour;
        te::EditItemID trackId;     // the channel (MIDI track)
        QString channelName;
        double lengthBeats = 0.0;   // the pattern length
    };

    explicit PianoRollEditor(QWidget* parent = nullptr);

    // An ordinary clip (or none); leaves pattern mode.
    void setClip(te::MidiClip* clip, EditManager* editMgr = nullptr);
    // One channel of a pattern. `clip` may be null (no placement yet).
    void setPatternClip(te::MidiClip* clip, EditManager* editMgr, const PatternContext& ctx);
    // Name, colour or length of the shown pattern changed (keeps the clip).
    void updatePatternContext(const PatternContext& ctx);
    const PatternContext& patternContext() const { return pattern_; }
    bool isShowingPattern() const { return pattern_.patternId > 0; }
    // Makes the clip of a pattern channel on demand (first note drawn/typed).
    void setPatternClipFactory(std::function<te::MidiClip*(int patternId, te::EditItemID trackId)> f) {
        patternClipFactory_ = std::move(f);
    }
    // Where previews go when no clip is loaded (the selected track/channel).
    void setPreviewTrack(te::AudioTrack* track);
    // The track previews currently play: the clip's, the pattern channel's
    // or the preview track. Null when none.
    te::AudioTrack* targetTrack() const;
    // Drops the clip, the pattern and the preview track (Edit about to go).
    void clear();

    void refresh();
    te::MidiClip* clip() const { return clip_; }
    bool isShowingClip(te::MidiClip* clip) const;
    void dropStaleClips();

signals:
    void notesChanged();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    QPushButton* makeIconButton(QWidget* parent, const QFont& font,
                                const QChar& glyph, const QString& tooltip,
                                const QString& accessible, int size = 24);

    void syncKeyboardScroll();
    void syncLaneScroll();
    void onSnapModeChanged(int index);
    void onDrawModeToggled(bool checked);
    void onEditModeToggled(bool checked);
    void onNotesChanged();
    void onCcComboChanged(int index);
    void onChannelComboChanged(int index);
    void setCcLaneVisible(bool visible);
    void updateCcHeaderLabel();
    QString ccDisplayName() const;

    void gatherLinkedClips();
    void rebuildChannelPanel();
    void setActiveChannel(int channelNumber);
    te::MidiClip* ensureClipForActiveChannel();
    void passClipsToChildren();
    void updateDrumNames();
    void loadClip(te::MidiClip* clip, EditManager* editMgr);
    void applyPreviewTarget();
    void updateHeader();

    te::MidiClip* clip_ = nullptr;
    EditManager* editMgr_ = nullptr;
    te::AudioTrack* track_ = nullptr;
    std::vector<te::MidiClip*> linkedClips_;
    int activeChannel_ = 1;
    std::set<int> hiddenChannels_;
    PatternContext pattern_;
    std::function<te::MidiClip*(int, te::EditItemID)> patternClipFactory_;
    te::SafeSelectable<te::AudioTrack> previewTrack_;

    QLabel* clipNameLabel_;
    QLabel* patternHintLabel_ = nullptr;
    QPushButton* drawModeBtn_ = nullptr;
    QPushButton* editModeBtn_ = nullptr;
    QPushButton* musicalTypingBtn_ = nullptr;
    QPushButton* stepRecordBtn_ = nullptr;
    QComboBox* snapCombo_;
    QComboBox* ccCombo_ = nullptr;
    PianoRollRuler* ruler_;
    PianoKeyboard* keyboard_;
    std::vector<QWidget*> keyColumnPanels_;   // match the keyboard's width
    NoteGrid* noteGrid_;
    VelocityLane* velocityLane_;
    CcLane* ccLane_ = nullptr;

    QScrollArea* channelScrollArea_ = nullptr;
    QWidget* channelPanel_ = nullptr;
    QVBoxLayout* channelPanelLayout_ = nullptr;

    QWidget* ccContainer_ = nullptr;
    QLabel* ccHeaderLabel_ = nullptr;
    QPushButton* ccCollapseBtn_ = nullptr;
    QPushButton* ccFreehandBtn_ = nullptr;
    QPushButton* ccLineBtn_ = nullptr;
};

} // namespace OpenDaw
