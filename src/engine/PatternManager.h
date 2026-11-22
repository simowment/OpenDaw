#pragma once

// Pattern-based sequencing (channel rack + step sequencer + playlist).
//
// Model
//  - A *channel* is an instrument track: every MIDI track of the Edit, in
//    display order. The Channel Rack shows one row per channel.
//  - A *pattern* holds, for each channel, a list of notes (PatternNote: start
//    and length in steps, default 16 steps of a 1/16 note: stepsPerBeat = 4).
//    Notes are the source of truth. The step-sequencer row (PatternChannel::
//    steps) is a view of them: step i is on when a note starts exactly on i.
//    Step edits (setStep, toggleStep, setChannelSteps) add or remove 1-step
//    notes on those steps and keep every other note (long, off-grid, chords).
//    A note with note -1 plays the channel's root note.
//  - A *placement* puts a pattern on the playlist (the arrangement timeline)
//    at a beat. It is made of one MIDI clip per channel that has notes in the
//    pattern, on that channel's own track. Each such clip is tagged in its
//    ValueTree with OpenDaw_patternId / OpenDaw_placementId, so every
//    placement of a pattern is regenerated when the pattern is edited (linked
//    instances). Removing all clips of a placement removes the placement.
//  - Editing the notes of a placement clip by hand (piano roll, AI note
//    tools) writes them back into the pattern (beats -> steps, notes past the
//    pattern end clipped) and regenerates the pattern's other placements. The
//    write-back runs from the event loop right after the edit and joins the
//    Edit's current undo transaction, so one undo restores both. A placement
//    clip moved to another track, or edited after being split/trimmed at its
//    start, becomes an ordinary clip instead.
//
// Persistence: the pattern data is a child ValueTree "OpenDaw_PATTERNS" of the
// Edit state, written through the Edit's UndoManager so pattern edits undo and
// redo together with the clip changes they cause. Survives save/load. Older
// step-only data (CHANNEL.steps) is read as 1-step notes.
//
// Threading: message thread (= Qt GUI thread) only.
// Ownership: OpenDawApplication owns it (it outlives MainWindow rebuilds).

#include "AudioEngine.h"
#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <optional>

namespace OpenDaw {

class EditManager;

struct PatternStep {
    bool on = false;
    int velocity = 100;   // 1..127
    int note = -1;        // MIDI note 0..127, or -1 = the channel's root note
};

// A note of a pattern, in steps from the pattern start. Since the piano roll
// can edit pattern notes, notes are the source of truth; `steps` below is the
// step-sequencer view of them (a step is on when a note starts exactly on it).
struct PatternNote {
    double startStep = 0.0;    // 0 .. lengthSteps (fractional = off-grid)
    double lengthSteps = 1.0;  // > 0
    int note = -1;             // MIDI note, -1 = the channel's root note
    int velocity = 100;        // 1..127
};

struct PatternChannel {
    te::EditItemID trackId;
    QVector<PatternStep> steps; // size == Pattern::lengthSteps (derived from notes)
    QVector<PatternNote> notes; // every note of this channel in the pattern
};

struct Pattern {
    int id = 0;               // > 0, unique within the Edit, never reused
    QString name;
    QColor colour;
    int lengthSteps = 16;     // 1..256
    int stepsPerBeat = 4;     // 4 = sixteenth-note steps
    QVector<PatternChannel> channels; // only channels with at least one note

    double lengthBeats() const { return double(lengthSteps) / double(stepsPerBeat); }
    const PatternChannel* channel(te::EditItemID trackId) const {
        for (auto& c : channels) if (c.trackId == trackId) return &c;
        return nullptr;
    }
};

struct PatternPlacement {
    int placementId = 0;
    int patternId = 0;
    double startBeat = 0.0;
    QVector<te::EditItemID> clipIds; // one MIDI clip per channel used
};

class PatternManager : public QObject {
    Q_OBJECT
public:
    enum class Mode { Pattern, Song };
    Q_ENUM(Mode)

    explicit PatternManager(EditManager& editMgr, QObject* parent = nullptr);
    ~PatternManager() override;

    // ── Channels ────────────────────────────────────────────────────────────
    QVector<te::AudioTrack*> channels() const;         // MIDI tracks, display order
    int channelRootNote(te::EditItemID trackId) const; // default 60 (C4)
    void setChannelRootNote(te::EditItemID trackId, int note);

    // ── Sample channels (one-shot drum/FX samples, like a sampler channel) ──
    // A sample channel is a MIDI track whose instrument is a Tracktion
    // SamplerPlugin holding one sound, mapped over every key with its root on
    // the channel's root note (60) and open-ended (plays to the end of the
    // file whatever the step length). One undo step each.
    // The track is named after the file (without extension) unless `name` is
    // given, and stores the absolute path in its OpenDaw_sample property.
    te::AudioTrack* addSampleChannel(const juce::File& sample, const QString& name = {},
                                     QString* error = nullptr);
    // Replaces the sound of a channel's sampler (the plugin is kept; a channel
    // without a sampler gets one, replacing its instrument). A track still
    // named after its old sample is renamed after the new one.
    bool setChannelSample(te::EditItemID trackId, const juce::File& sample, QString* error = nullptr);
    QString channelSamplePath(te::EditItemID trackId) const;  // empty unless a sample channel
    te::SamplerPlugin* channelSampler(te::EditItemID trackId) const; // null unless the instrument is a sampler
    static bool isAudioSampleFile(const juce::File& f);       // wav/aif/aiff/flac/ogg/mp3, readable

    // ── Patterns ────────────────────────────────────────────────────────────
    QVector<Pattern> patterns() const;
    std::optional<Pattern> pattern(int patternId) const;
    int createPattern(const QString& name = {});       // returns new id; "Pattern N" when empty
    int clonePattern(int patternId);                   // 0 on failure
    bool removePattern(int patternId);                 // also deletes its placements' clips
    bool renamePattern(int patternId, const QString& name);
    bool setPatternColour(int patternId, const QColor& colour);
    bool setPatternLength(int patternId, int lengthSteps);

    // note: -2 = leave unchanged, -1 = root note. velocity: -1 = leave unchanged.
    bool setStep(int patternId, te::EditItemID trackId, int step, bool on,
                 int velocity = -1, int note = -2);
    bool toggleStep(int patternId, te::EditItemID trackId, int step);
    bool clearChannel(int patternId, te::EditItemID trackId);

    // ── Playlist placements ─────────────────────────────────────────────────
    int placePattern(int patternId, double startBeat);  // returns placementId, 0 on failure
    bool removePlacement(int placementId);
    QVector<PatternPlacement> placements(int patternId = 0) const; // 0 = all
    // Rebuilds the MIDI clips of every placement of a pattern from its steps.
    void regeneratePlacements(int patternId);

    // ── Current pattern / mode (UI state; not saved with the project) ───────
    int currentPattern() const { return currentPattern_; } // 0 when there are none
    void setCurrentPattern(int patternId);
    Mode mode() const { return mode_; }
    void setMode(Mode m);

    // ── Convenience (additions to the shared contract) ──────────────────────
    // Sets a channel's whole step row in one step (one regeneration): a step
    // turned on gets a 1-step note, a step turned off loses the notes that
    // start on it, a step that stays on keeps its note (length included) with
    // the row's velocity/note. Notes that do not start on a step are kept.
    // The row is resized to the pattern length; the channel leaves the
    // pattern when no note is left.
    bool setChannelSteps(int patternId, te::EditItemID trackId, const QVector<PatternStep>& steps);

    // ── Notes (piano-roll level; steps are a view of these) ─────────────────
    QVector<PatternNote> channelNotes(int patternId, te::EditItemID trackId) const;
    // Replaces every note of a channel in a pattern (one undo step) and
    // regenerates all placements.
    bool setChannelNotes(int patternId, te::EditItemID trackId, const QVector<PatternNote>& notes);
    // The MIDI clip the piano roll should edit for this channel in this
    // pattern: the channel's clip in the pattern's first placement. When the
    // pattern has no placement yet, one is created at the first free bar after
    // the arrangement's end (bar 1 in an empty song); when the placement has
    // no clip on this channel, one (empty) is added and kept while the
    // channel has no notes. Hand edits to any placement clip are written back
    // into the pattern and copied to its other placements (linked instances,
    // like a pattern in a playlist). One undo step ("Edit Pattern in Piano
    // Roll") when it creates something. nullptr on failure (no such pattern,
    // the track is not a channel, a render is in progress).
    te::MidiClip* editableClipFor(int patternId, te::EditItemID trackId);
    // The pattern a clip belongs to (0 when it is an ordinary clip).
    int patternOfClip(const te::Clip* clip) const;
    bool patternExists(int patternId) const;
    // The 12 colours new patterns cycle through.
    static QVector<QColor> palette();
    // Timeline/piano-roll refreshes (EditManager::editChanged and
    // midiClipModified) are coalesced and sent from the event loop; this sends
    // the pending ones now.
    void flushEditNotifications();
    // While at least one is open, mutators do not start their own undo
    // transaction: the caller (e.g. the AI tool executor) owns it. Nestable.
    void beginExternalTransaction();
    void endExternalTransaction();
    struct ScopedExternalTransaction {
        explicit ScopedExternalTransaction(PatternManager& pm) : pm_(pm) { pm_.beginExternalTransaction(); }
        ~ScopedExternalTransaction() { pm_.endExternalTransaction(); }
        ScopedExternalTransaction(const ScopedExternalTransaction&) = delete;
        ScopedExternalTransaction& operator=(const ScopedExternalTransaction&) = delete;
    private:
        PatternManager& pm_;
    };

signals:
    void patternsChanged();               // list, names, colours or lengths changed
    void patternChanged(int patternId);   // steps of one pattern changed
    void placementsChanged();
    void currentPatternChanged(int patternId);
    void modeChanged(PatternManager::Mode mode);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    EditManager& editMgr_;
    int currentPattern_ = 0;
    Mode mode_ = Mode::Pattern;
};

// ── Sample library ──────────────────────────────────────────────────────────
// Finds one-shot samples on disk for sample channels. Roots, in order:
//  - QSettings "samples/roots" (QStringList of folders the user added),
//  - every FL Studio install's pack folder on every mounted drive
//    (<drive>/Program Files[ (x86)]/Image-Line/FL Studio*/Data/Patches/Packs),
//  - the user's Music folder.
// Only existing folders are returned, each once. Directory scans are cached
// for 60 s (invalidateCache() drops them now). Message thread or any thread:
// the cache is locked.
struct SampleInfo {
    QString path;          // absolute, '/' separators
    QString name;          // file name without extension
    QString folder;        // containing folder relative to the root ("" = root itself)
    QString relativePath;  // path relative to the root, e.g. "Drums/Kicks/Kick 01.wav"
    QString root;          // the root folder it was found under
    qint64 size = 0;       // bytes
};

struct SampleFolderInfo {
    QString path;          // absolute
    QString name;          // folder name
    QString relativePath;  // relative to its root ("" for a root)
    int sampleCount = 0;   // samples in it and below
};

class SampleLibrary {
public:
    static QStringList sampleRoots();
    // Case-insensitive: every word of `query` must appear in the relative path
    // (spaces, '-' and '_' are ignored when matching, so "hihat" finds
    // "Hi Hats/..."). File-name hits rank first. `folder` (absolute, or
    // relative to a root, e.g. "Drums/Kicks") limits the search to that
    // folder and below. limit is clamped to 1..1000. `totalMatches` gets the
    // number of matches before the limit.
    static QVector<SampleInfo> findSamples(const QString& query, const QString& folder = {},
                                           int limit = 25, int* totalMatches = nullptr);
    // No folder: the roots. Otherwise the immediate sub-folders of `folder`
    // that contain samples. `resolved` gets the folder searched, `direct` the
    // number of samples directly in it. Empty + error when `folder` is unknown.
    static QVector<SampleFolderInfo> listFolders(const QString& folder = {}, QString* resolved = nullptr,
                                                 int* direct = nullptr, QString* error = nullptr);
    // Resolves a sample given as an absolute path or a path relative to a
    // root (find_samples' relative_path). Empty when not found.
    static QString resolveSamplePath(const QString& pathOrRelative);
    static bool hasSampleExtension(const QString& fileName); // wav/aif/aiff/flac/ogg/mp3
    static void invalidateCache();
};

// Plays a pattern on its own (Pattern mode), looping, by sending live MIDI to
// each channel's instrument at the project tempo. The transport stays stopped.
// It does not play while a render is in progress (it stops, and play() is
// refused), so its live notes never leak into an export.
// One note as the pattern player plays it: note-on at startStep, note-off at
// endStep (start + length, clamped to the pattern end), in steps from the
// pattern start (fractional = off-grid, played at that exact time).
struct PatternPlayEvent {
    te::EditItemID trackId;
    int note = 60;        // resolved: the channel's root note for note -1
    int velocity = 100;
    double startStep = 0.0;
    double endStep = 1.0;
};

class PatternPlayer : public QObject {
    Q_OBJECT
public:
    PatternPlayer(PatternManager& patterns, EditManager& editMgr, QObject* parent = nullptr);
    ~PatternPlayer() override;

    // Every note of a pattern as the player schedules it, sorted by start.
    static QVector<PatternPlayEvent> scheduleNotes(const Pattern& p, const PatternManager& pm);

    void play(int patternId);
    void stop();
    bool isPlaying() const;
    int playingPattern() const;

signals:
    void stepChanged(int step);   // -1 when stopped
    void playingChanged(bool playing);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace OpenDaw
