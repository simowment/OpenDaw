#pragma once

// "Typing keyboard to piano" (FL Studio style): while enabled, the computer
// keyboard plays the selected channel/track from anywhere in the window.
//
//   lower row  Z S X D C V G B H N J M ,   -> one octave from C (octave base)
//   upper row  Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P -> the octave above
//
// Keys are matched by physical position (Windows scan codes), so the layout
// works the same on AZERTY/QWERTZ keyboards. Numpad +/- shift the octave.
// Keys with Ctrl/Alt held, and every key while a text field, a modal dialog or
// the piano roll's own musical typing has focus, are left alone; the mapped
// keys otherwise take precedence over single-key shortcuts (S, R...), as in FL.
// Space is not mapped, so it still starts/stops playback.

#include "engine/AudioEngine.h"
#include <QHash>
#include <QObject>

class QKeyEvent;

namespace OpenDaw {

class EditManager;

class TypingKeyboard : public QObject {
    Q_OBJECT
public:
    explicit TypingKeyboard(EditManager& editMgr, QObject* parent = nullptr);
    ~TypingKeyboard() override;

    void setEnabled(bool on);
    bool isEnabled() const { return enabled_; }

    // The track whose instrument plays. Invalid id = the first MIDI track.
    void setTarget(te::EditItemID trackId);

    int octaveBase() const { return octaveBase_; }   // MIDI note of the lower row's C
    void setOctaveBase(int note);

    void allNotesOff();

signals:
    void enabledChanged(bool on);
    void octaveChanged(int baseNote);
    void notePlayed(int note, bool on);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    int noteForKey(const QKeyEvent* e) const;   // -1 when not a piano key
    static bool isOctaveKey(const QKeyEvent* e, int* direction);
    bool shouldHandle(const QKeyEvent* e) const;
    te::AudioTrack* targetTrack() const;
    void send(te::AudioTrack* t, int note, bool on);

    EditManager& editMgr_;
    bool enabled_ = true;
    te::EditItemID target_;
    int octaveBase_ = 48;               // C3 on the lower row, C4 on the upper
    QHash<quint32, int> held_;          // physical key -> sounding note
    te::EditItemID heldTrack_;          // where the held notes were sent
};

} // namespace OpenDaw
