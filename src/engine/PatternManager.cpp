#include "PatternManager.h"
#include "EditManager.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace OpenDaw {

namespace {

// ── Persistent identifiers ──────────────────────────────────────────────────
// Edit state:   OpenDaw_PATTERNS { nextPatternId, nextPlacementId }
//                 PATTERN { id, name, colour (QRgb), lengthSteps, stepsPerBeat }
//                   CHANNEL { track (raw EditItemID) }
//                     NOTE { start, length (steps), note (-1 = root), velocity }
//                   Legacy CHANNEL { track, steps (encoded row) } without NOTE
//                   children is read as 1-step notes and rewritten as NOTEs on
//                   the next edit of that channel. A CHANNEL with neither is an
//                   empty channel kept for a piano-roll clip (editableClipFor).
// Clip state:   OpenDaw_patternId, OpenDaw_placementId,
//               OpenDaw_placementStart (beats: the placement start last written),
//               OpenDaw_patternNotes (signature of the notes PatternManager last
//               wrote; a clip whose notes no longer match was edited by hand),
//               OpenDaw_patternTrack (the channel the clip was made for; a clip
//               found on another track was moved there and is untagged)
// Track state:  OpenDaw_rootNote, OpenDaw_sample (absolute path of a sample channel's sound)
const juce::Identifier kPatternsId("OpenDaw_PATTERNS");
const juce::Identifier kPatternNode("PATTERN");
const juce::Identifier kChannelNode("CHANNEL");
const juce::Identifier kIdProp("id");
const juce::Identifier kNameProp("name");
const juce::Identifier kColourProp("colour");
const juce::Identifier kLengthProp("lengthSteps");
const juce::Identifier kStepsPerBeatProp("stepsPerBeat");
const juce::Identifier kTrackProp("track");
const juce::Identifier kStepsProp("steps");
const juce::Identifier kNextPatternIdProp("nextPatternId");
const juce::Identifier kNextPlacementIdProp("nextPlacementId");
const juce::Identifier kClipPatternIdProp("OpenDaw_patternId");
const juce::Identifier kClipPlacementIdProp("OpenDaw_placementId");
const juce::Identifier kClipPlacementStartProp("OpenDaw_placementStart");
const juce::Identifier kClipNotesSigProp("OpenDaw_patternNotes");
const juce::Identifier kClipTrackProp("OpenDaw_patternTrack");
const juce::Identifier kNoteNode("NOTE");
const juce::Identifier kNoteStartProp("start");
const juce::Identifier kNoteLengthProp("length");
const juce::Identifier kNoteNoteProp("note");
const juce::Identifier kNoteVelocityProp("velocity");
const juce::Identifier kRootNoteProp("OpenDaw_rootNote");
const juce::Identifier kSampleProp("OpenDaw_sample");

constexpr int kDefaultRootNote = 60;
constexpr int kDefaultVelocity = 100;
constexpr int kMaxSteps = 256;
constexpr double kBeatEps = 1.0e-4;
constexpr double kStepEps = 1.0e-6;   // a note starts "on" a step within this
constexpr double kMinNoteSteps = 1.0e-3;

QString toQ(const juce::String& s) { return QString::fromUtf8(s.toRawUTF8()); }
juce::String toJ(const QString& s) { return juce::String::fromUTF8(s.toUtf8().constData()); }

juce::String trackKey(te::EditItemID id) { return juce::String((juce::int64) id.getRawID()); }

te::EditItemID trackIdFromVar(const juce::var& v)
{
    const auto raw = v.toString().getLargeIntValue();
    return te::EditItemID::fromRawID(static_cast<uint64_t>(raw));
}

// Legacy step row encoding (read only): comma separated tokens, one per step.
//   "."           off, default velocity and root note
//   "v" / "v:n"   on, velocity v, note n (no ":n" = root note)
//   "~v" / "~v:n" off, but remembers velocity and note for when it is turned on
QVector<PatternStep> decodeSteps(const juce::String& encoded, int length)
{
    QVector<PatternStep> steps(length);
    juce::StringArray tokens;
    tokens.addTokens(encoded, ",", "");
    const int n = std::min(length, tokens.size());
    for (int i = 0; i < n; ++i) {
        auto t = tokens[i].trim();
        if (t.isEmpty() || t == ".") continue;
        PatternStep st;
        st.on = !t.startsWithChar('~');
        if (!st.on) t = t.substring(1);
        const int colon = t.indexOfChar(':');
        const auto velPart = colon >= 0 ? t.substring(0, colon) : t;
        st.velocity = std::clamp(velPart.getIntValue(), 1, 127);
        if (colon >= 0)
            st.note = std::clamp(t.substring(colon + 1).getIntValue(), 0, 127);
        steps[i] = st;
    }
    return steps;
}

// ── Notes ───────────────────────────────────────────────────────────────────

double roundSteps(double v) { return std::round(v * 1.0e6) / 1.0e6; }

bool startsOnStep(const PatternNote& n, int step) { return std::abs(n.startStep - double(step)) < kStepEps; }

bool noteLess(const PatternNote& a, const PatternNote& b)
{
    if (std::abs(a.startStep - b.startStep) > kStepEps) return a.startStep < b.startStep;
    if (a.note != b.note) return a.note < b.note;
    if (std::abs(a.lengthSteps - b.lengthSteps) > kStepEps) return a.lengthSteps < b.lengthSteps;
    return a.velocity < b.velocity;
}

// Clamps every field, drops notes outside 0..lengthSteps, cuts lengths at the
// pattern end and sorts by start (then note).
QVector<PatternNote> normalizeNotes(QVector<PatternNote> notes, int lengthSteps)
{
    QVector<PatternNote> out;
    out.reserve(notes.size());
    for (auto n : notes) {
        if (!std::isfinite(n.startStep) || !std::isfinite(n.lengthSteps)) continue;
        n.startStep = roundSteps(n.startStep);
        if (n.startStep < 0.0) {
            if (n.startStep > -kStepEps) n.startStep = 0.0;
            else continue;
        }
        if (n.startStep >= double(lengthSteps) - kStepEps) continue;
        n.lengthSteps = roundSteps(std::min(n.lengthSteps, double(lengthSteps) - n.startStep));
        if (n.lengthSteps < kMinNoteSteps) continue;
        n.velocity = std::clamp(n.velocity, 1, 127);
        n.note = n.note < 0 ? -1 : std::min(n.note, 127);
        out.push_back(n);
    }
    std::stable_sort(out.begin(), out.end(), noteLess);
    return out;
}

bool sameNotes(const QVector<PatternNote>& a, const QVector<PatternNote>& b)
{
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (std::abs(a[i].startStep - b[i].startStep) > kStepEps
            || std::abs(a[i].lengthSteps - b[i].lengthSteps) > kStepEps
            || a[i].note != b[i].note || a[i].velocity != b[i].velocity)
            return false;
    return true;
}

QVector<PatternNote> stepsToNotes(const QVector<PatternStep>& steps)
{
    QVector<PatternNote> notes;
    for (int i = 0; i < steps.size(); ++i)
        if (steps[i].on)
            notes.push_back({ double(i), 1.0, steps[i].note, std::clamp(steps[i].velocity, 1, 127) });
    return notes;
}

// The note a step shows: the first note starting on it that plays the root
// note, else the first one. -1 when none.
int stepNoteIndex(const QVector<PatternNote>& notes, int step)
{
    int first = -1;
    for (int k = 0; k < notes.size(); ++k) {
        if (!startsOnStep(notes[k], step)) continue;
        if (notes[k].note < 0) return k;
        if (first < 0) first = k;
    }
    return first;
}

QVector<PatternStep> notesToSteps(const QVector<PatternNote>& notes, int lengthSteps)
{
    QVector<PatternStep> steps(lengthSteps);
    for (int i = 0; i < lengthSteps; ++i) {
        const int k = stepNoteIndex(notes, i);
        if (k < 0) continue;
        steps[i].on = true;
        steps[i].velocity = notes[k].velocity;
        steps[i].note = notes[k].note;
    }
    return steps;
}

QColor colourFromVar(const juce::var& v, const QColor& fallback)
{
    if (v.isVoid()) return fallback;
    const auto raw = static_cast<juce::int64>(v);
    return QColor::fromRgba(static_cast<QRgb>(static_cast<quint32>(raw)));
}

double clipStartBeat(te::Clip& c)
{
    auto& ts = c.edit.tempoSequence;
    return ts.toBeats(c.getPosition().getStart()).inBeats();
}

double clipEndBeat(te::Clip& c)
{
    auto& ts = c.edit.tempoSequence;
    return ts.toBeats(c.getPosition().getEnd()).inBeats();
}

struct DesiredNote {
    int pitch;
    double start;
    double length;
    int velocity;
};

// Notes as one string, independent of note order:
// "pitch@start+length*velocity;" sorted, beats to 4 decimals.
juce::String signatureOf(const std::vector<DesiredNote>& notes)
{
    juce::StringArray parts;
    for (auto& n : notes)
        parts.add(juce::String(n.pitch) + "@" + juce::String(n.start, 4) + "+" + juce::String(n.length, 4)
                  + "*" + juce::String(n.velocity));
    parts.sort(false);
    return parts.joinIntoString(";");
}

juce::String notesSignature(te::MidiClip& mc)
{
    std::vector<DesiredNote> notes;
    for (auto* n : mc.getSequence().getNotes())
        notes.push_back({ n->getNoteNumber(), n->getStartBeat().inBeats(), n->getLengthBeats().inBeats(),
                          n->getVelocity() });
    return signatureOf(notes);
}

// The MIDI notes a channel's pattern notes become in a placement clip.
std::vector<DesiredNote> desiredNotes(const Pattern& p, const QVector<PatternNote>& notes, int root)
{
    std::vector<DesiredNote> out;
    const double stepLen = 1.0 / double(p.stepsPerBeat);
    for (const auto& n : notes) {
        const double end = std::min(n.startStep + n.lengthSteps, double(p.lengthSteps));
        if (end - n.startStep < kMinNoteSteps) continue;
        out.push_back({ std::clamp(n.note >= 0 ? n.note : root, 0, 127), n.startStep * stepLen,
                        (end - n.startStep) * stepLen, std::clamp(n.velocity, 1, 127) });
    }
    return out;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// PatternManager::Impl
// ═════════════════════════════════════════════════════════════════════════════

struct PatternManager::Impl : public juce::ValueTree::Listener {
    PatternManager& owner;
    EditManager& em;

    te::Edit* attachedEdit = nullptr;
    juce::ValueTree tree;              // the OpenDaw_PATTERNS node we listen to

    int suppressListener = 0;          // > 0 while we write the tree ourselves
    int externalTransactions = 0;
    bool inEditChangedHandler = false;

    // Placement start as last written/seen, to tell which clip the user moved.
    std::map<int, double> lastStart;
    QString placementSignature;

    // Deferred UI refresh
    bool flushScheduled = false;
    bool pendingEdit = false;
    std::set<uint64_t> pendingClips;

    // Deferred signals caused by undo/redo or other external tree changes
    bool externalFlushScheduled = false;
    bool externalPatternsDirty = false;
    std::set<int> externalDirtyPatterns;

    // Hand edits of placement clips: the whole Edit state is watched for note
    // changes in tagged clips (piano roll, AI note tools, undo/redo) and for
    // tagged clips arriving on a track; they are processed from the event
    // loop (writeBack: notes -> pattern, other placements regenerated).
    struct ClipWatcher : juce::ValueTree::Listener {
        Impl& impl;
        explicit ClipWatcher(Impl& i) : impl(i) {}
        void valueTreePropertyChanged(juce::ValueTree& t, const juce::Identifier&) override { impl.onClipTreeChanged(t); }
        void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override
        {
            if (child.hasType(te::IDs::MIDICLIP)) impl.onClipArrived(child);
            else impl.onClipTreeChanged(parent);
        }
        void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int) override { impl.onClipTreeChanged(parent); }
        void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override {}
        void valueTreeParentChanged(juce::ValueTree&) override {}
    };
    ClipWatcher clipWatcher { *this };
    juce::ValueTree watchedState;      // the attached Edit's state
    std::set<uint64_t> pendingWriteBack; // clips whose notes changed
    bool clipCheckPending = false;     // a tagged clip arrived somewhere
    bool clipCheckScheduled = false;

    Impl(PatternManager& o, EditManager& e) : owner(o), em(e) {}
    ~Impl() override { detach(); }

    // ── Edit access ─────────────────────────────────────────────────────────
    te::Edit* edit() const { return em.edit(); }
    juce::UndoManager* um() const { return edit() ? &edit()->getUndoManager() : nullptr; }

    juce::ValueTree root(bool create) const
    {
        auto* e = edit();
        if (!e) return {};
        auto t = e->state.getChildWithName(kPatternsId);
        if (!t.isValid() && create) {
            // Created without undo so undo never removes the node we listen to.
            t = juce::ValueTree(kPatternsId);
            e->state.appendChild(t, nullptr);
        }
        return t;
    }

    juce::ValueTree patternNode(int patternId) const
    {
        auto r = root(false);
        if (!r.isValid() || patternId <= 0) return {};
        for (auto child : r)
            if (child.hasType(kPatternNode) && int(child.getProperty(kIdProp, 0)) == patternId)
                return child;
        return {};
    }

    static juce::ValueTree channelNode(const juce::ValueTree& pnode, te::EditItemID trackId)
    {
        for (auto child : pnode)
            if (child.hasType(kChannelNode) && trackIdFromVar(child.getProperty(kTrackProp)) == trackId)
                return child;
        return {};
    }

    void beginTransaction(const char* name)
    {
        if (externalTransactions == 0)
            if (auto* u = um()) u->beginNewTransaction(juce::String(name));
    }

    bool canEdit() const { return edit() != nullptr && !em.isRenderInProgress(); }

    // ── Attach / detach ─────────────────────────────────────────────────────
    void attach()
    {
        auto* e = edit();
        if (e == attachedEdit && tree.isValid()) return;
        detach();
        attachedEdit = e;
        lastStart.clear();
        placementSignature.clear();
        if (e) {
            tree = root(true);
            tree.addListener(this);
            watchedState = e->state;
            watchedState.addListener(&clipWatcher);
        }
        auto list = owner.patterns();
        const int first = list.isEmpty() ? 0 : list.first().id;
        owner.currentPattern_ = first;
        seedLastStartFromClips();
        normalizePlacements();
        placementSignature = computeSignature();
        emit owner.patternsChanged();
        emit owner.placementsChanged();
        emit owner.currentPatternChanged(owner.currentPattern_);
    }

    void detach()
    {
        if (tree.isValid()) tree.removeListener(this);
        tree = {};
        if (watchedState.isValid()) watchedState.removeListener(&clipWatcher);
        watchedState = {};
        pendingWriteBack.clear();
        clipCheckPending = false;
        attachedEdit = nullptr;
        pendingEdit = false;
        pendingClips.clear();
        externalPatternsDirty = false;
        externalDirtyPatterns.clear();
        lastStart.clear();
    }

    // ── Reading ─────────────────────────────────────────────────────────────
    std::unordered_map<uint64_t, int> displayIndex() const
    {
        std::unordered_map<uint64_t, int> idx;
        int i = 0;
        for (auto* t : em.getAudioTracksInDisplayOrder())
            if (t) idx[t->itemID.getRawID()] = i++;
        return idx;
    }

    Pattern readPattern(const juce::ValueTree& node, const std::unordered_map<uint64_t, int>& order) const
    {
        Pattern p;
        p.id = int(node.getProperty(kIdProp, 0));
        p.name = toQ(node.getProperty(kNameProp, juce::String()).toString());
        p.lengthSteps = std::clamp(int(node.getProperty(kLengthProp, 16)), 1, kMaxSteps);
        p.stepsPerBeat = std::clamp(int(node.getProperty(kStepsPerBeatProp, 4)), 1, 64);
        const auto pal = PatternManager::palette();
        p.colour = colourFromVar(node.getProperty(kColourProp), pal[(std::max(p.id, 1) - 1) % pal.size()]);
        for (auto child : node) {
            if (!child.hasType(kChannelNode)) continue;
            const auto tid = trackIdFromVar(child.getProperty(kTrackProp));
            if (!tid.isValid() || order.find(tid.getRawID()) == order.end())
                continue; // track deleted: dropped lazily
            PatternChannel ch;
            ch.trackId = tid;
            ch.notes = readNotes(child, p.lengthSteps);
            if (ch.notes.isEmpty()) continue;
            ch.steps = notesToSteps(ch.notes, p.lengthSteps);
            p.channels.push_back(std::move(ch));
        }
        std::stable_sort(p.channels.begin(), p.channels.end(),
                         [&order](const PatternChannel& a, const PatternChannel& b) {
                             return order.at(a.trackId.getRawID()) < order.at(b.trackId.getRawID());
                         });
        return p;
    }

    // A channel's notes: its NOTE children, or the legacy step row as 1-step notes.
    static QVector<PatternNote> readNotes(const juce::ValueTree& ch, int lengthSteps)
    {
        QVector<PatternNote> notes;
        bool hasNoteNodes = false;
        for (auto n : ch) {
            if (!n.hasType(kNoteNode)) continue;
            hasNoteNodes = true;
            PatternNote pn;
            pn.startStep = double(n.getProperty(kNoteStartProp, 0.0));
            pn.lengthSteps = double(n.getProperty(kNoteLengthProp, 1.0));
            pn.note = int(n.getProperty(kNoteNoteProp, -1));
            pn.velocity = int(n.getProperty(kNoteVelocityProp, kDefaultVelocity));
            notes.push_back(pn);
        }
        if (!hasNoteNodes && ch.hasProperty(kStepsProp))
            notes = stepsToNotes(decodeSteps(ch.getProperty(kStepsProp).toString(), lengthSteps));
        return normalizeNotes(std::move(notes), lengthSteps);
    }

    // Every track with a CHANNEL node in the pattern, empty ones included.
    std::set<uint64_t> channelNodeTracks(const juce::ValueTree& pnode) const
    {
        std::set<uint64_t> out;
        for (auto child : pnode)
            if (child.hasType(kChannelNode))
                out.insert(trackIdFromVar(child.getProperty(kTrackProp)).getRawID());
        return out;
    }

    std::optional<Pattern> readPattern(int patternId) const
    {
        auto node = patternNode(patternId);
        if (!node.isValid()) return std::nullopt;
        return readPattern(node, displayIndex());
    }

    int allocatePatternId()
    {
        auto r = root(true);
        int next = std::max(1, int(r.getProperty(kNextPatternIdProp, 1)));
        for (auto child : r)
            if (child.hasType(kPatternNode))
                next = std::max(next, int(child.getProperty(kIdProp, 0)) + 1);
        r.setProperty(kNextPatternIdProp, next + 1, nullptr); // never undone: ids are never reused
        return next;
    }

    int allocatePlacementId()
    {
        auto r = root(true);
        int next = std::max(1, int(r.getProperty(kNextPlacementIdProp, 1)));
        for (auto& c : scanTagged())
            next = std::max(next, c.placementId + 1);
        r.setProperty(kNextPlacementIdProp, next + 1, nullptr);
        return next;
    }

    // ── Clip scanning ───────────────────────────────────────────────────────
    struct TaggedClip {
        te::MidiClip* clip = nullptr;
        te::AudioTrack* track = nullptr;
        int patternId = 0;
        int placementId = 0;
        double start = 0.0;
    };

    std::vector<TaggedClip> scanTagged() const
    {
        std::vector<TaggedClip> out;
        auto* e = edit();
        if (!e) return out;
        for (auto* t : te::getAudioTracks(*e)) {
            if (!t) continue;
            for (auto* c : t->getClips()) {
                auto* mc = dynamic_cast<te::MidiClip*>(c);
                if (!mc || !mc->state.hasProperty(kClipPlacementIdProp)) continue;
                TaggedClip tc;
                tc.clip = mc;
                tc.track = t;
                tc.patternId = int(mc->state.getProperty(kClipPatternIdProp, 0));
                tc.placementId = int(mc->state.getProperty(kClipPlacementIdProp, 0));
                if (tc.patternId <= 0 || tc.placementId <= 0) continue;
                tc.start = clipStartBeat(*mc);
                out.push_back(tc);
            }
        }
        return out;
    }

    std::map<int, std::vector<TaggedClip>> groupByPlacement(const std::vector<TaggedClip>& all, int patternId) const
    {
        std::map<int, std::vector<TaggedClip>> groups;
        for (auto& c : all)
            if (patternId == 0 || c.patternId == patternId)
                groups[c.placementId].push_back(c);
        return groups;
    }

    // The placement start last written is stored on every clip of the
    // placement, so a placement whose clips were dragged apart resolves to the
    // same start after a reload as before it.
    void seedLastStartFromClips()
    {
        for (auto& c : scanTagged()) {
            if (lastStart.count(c.placementId)) continue;
            const auto v = c.clip->state.getProperty(kClipPlacementStartProp);
            if (!v.isVoid()) lastStart[c.placementId] = std::max(0.0, double(v));
        }
    }

    // The placement start: the clip the user moved if one clip disagrees with
    // the last known start, otherwise that start; the earliest clip if unknown.
    double derivedStart(int placementId, const std::vector<TaggedClip>& clips) const
    {
        if (clips.empty()) return 0.0;
        auto it = lastStart.find(placementId);
        if (it != lastStart.end()) {
            for (auto& c : clips)
                if (std::abs(c.start - it->second) > kBeatEps)
                    return c.start;
            return it->second;
        }
        double s = clips.front().start;
        for (auto& c : clips) s = std::min(s, c.start);
        return s;
    }

    // Copies of pattern clips (copy/paste, duplicate, split) keep the tags.
    // When one placement has two clips on the same track, the extra ones become
    // placements of their own (written without undo: it is bookkeeping only).
    bool normalizePlacements()
    {
        auto* e = edit();
        if (!e) return false;
        auto groups = groupByPlacement(scanTagged(), 0);
        bool changed = false;
        for (auto& [pid, clips] : groups) {
            std::set<uint64_t> tracks;
            bool dup = false;
            for (auto& c : clips)
                if (!tracks.insert(c.track->itemID.getRawID()).second) { dup = true; break; }

            if (!dup) {
                bool same = true;
                for (auto& c : clips)
                    if (std::abs(c.start - clips.front().start) > kBeatEps) same = false;
                if (same) lastStart[pid] = clips.front().start;
                continue;
            }

            // Reference start: the remembered one if a clip still sits there, else the earliest.
            double ref = clips.front().start;
            for (auto& c : clips) ref = std::min(ref, c.start);
            if (auto it = lastStart.find(pid); it != lastStart.end())
                for (auto& c : clips)
                    if (std::abs(c.start - it->second) <= kBeatEps) { ref = it->second; break; }

            std::set<uint64_t> kept;
            std::vector<TaggedClip> extras;
            for (auto& c : clips) {
                const auto raw = c.track->itemID.getRawID();
                if (std::abs(c.start - ref) <= kBeatEps && kept.insert(raw).second) continue;
                extras.push_back(c);
            }
            std::stable_sort(extras.begin(), extras.end(),
                             [](const TaggedClip& a, const TaggedClip& b) { return a.start < b.start; });

            struct Cluster { double start; int id; std::set<uint64_t> tracks; };
            std::vector<Cluster> clusters;
            for (auto& c : extras) {
                const auto raw = c.track->itemID.getRawID();
                Cluster* target = nullptr;
                for (auto& cl : clusters)
                    if (std::abs(cl.start - c.start) <= kBeatEps && !cl.tracks.count(raw)) { target = &cl; break; }
                if (!target) {
                    clusters.push_back({ c.start, allocatePlacementId(), {} });
                    target = &clusters.back();
                    lastStart[target->id] = c.start;
                }
                target->tracks.insert(raw);
                ++suppressListener;
                c.clip->state.setProperty(kClipPlacementIdProp, target->id, nullptr);
                c.clip->state.setProperty(kClipPlacementStartProp, c.start, nullptr);
                --suppressListener;
                changed = true;
            }
            lastStart[pid] = ref;
        }
        return changed;
    }

    QString computeSignature() const
    {
        QString sig;
        auto groups = groupByPlacement(scanTagged(), 0);
        for (auto& [pid, clips] : groups) {
            sig += QString::number(pid) + QLatin1Char('/') + QString::number(clips.front().patternId)
                 + QLatin1Char('@') + QString::number(derivedStart(pid, clips), 'f', 4)
                 + QLatin1Char('#') + QString::number(int(clips.size())) + QLatin1Char(';');
        }
        return sig;
    }

    // ── Regeneration ────────────────────────────────────────────────────────
    struct RegenResult {
        bool structural = false; // clips created, deleted or moved
        bool removedClips = false;
    };

    void noteClipModified(te::MidiClip* mc)
    {
        if (mc) pendingClips.insert(mc->itemID.getRawID());
        pendingEdit = true;
    }

    // The MIDI notes a placement clip on `trackRaw` should hold.
    std::vector<DesiredNote> desiredFor(const Pattern& p, uint64_t trackRaw) const
    {
        for (auto& ch : p.channels)
            if (ch.trackId.getRawID() == trackRaw)
                return desiredNotes(p, ch.notes, owner.channelRootNote(ch.trackId));
        return {};
    }

    // A tagged clip whose notes differ from the signature PatternManager
    // stored when it last wrote them was edited by hand (piano roll, AI note
    // tools). A clip without a signature (older projects) is compared with
    // the notes the pattern would give it.
    bool isHandEdited(te::MidiClip& mc, const Pattern& p, te::AudioTrack& track) const
    {
        const auto sig = notesSignature(mc);
        const auto v = mc.state.getProperty(kClipNotesSigProp);
        if (!v.isVoid()) return v.toString() != sig;
        return signatureOf(desiredFor(p, track.itemID.getRawID())) != sig;
    }

    // Turns a pattern clip into an ordinary MIDI clip (undoable).
    void untag(te::MidiClip& mc, juce::UndoManager* u)
    {
        mc.state.removeProperty(kClipPatternIdProp, u);
        mc.state.removeProperty(kClipPlacementIdProp, u);
        mc.state.removeProperty(kClipPlacementStartProp, u);
        mc.state.removeProperty(kClipNotesSigProp, u);
        mc.state.removeProperty(kClipTrackProp, u);
        noteClipModified(&mc);
    }

    // Writes the tags and bookkeeping of a placement clip.
    void tagClip(te::MidiClip& mc, int patternId, int placementId, double start, te::AudioTrack& track,
                 juce::UndoManager* u)
    {
        if (int(mc.state.getProperty(kClipPatternIdProp, 0)) != patternId)
            mc.state.setProperty(kClipPatternIdProp, patternId, u);
        if (int(mc.state.getProperty(kClipPlacementIdProp, 0)) != placementId)
            mc.state.setProperty(kClipPlacementIdProp, placementId, u);
        // Bookkeeping that survives save/reload and undo: the placement
        // start (dragged clips snapped back after a reload), the channel it
        // belongs to (a clip moved to another track leaves the pattern) and
        // the notes as written (to recognise later hand edits).
        if (!mc.state.hasProperty(kClipPlacementStartProp)
            || std::abs(double(mc.state.getProperty(kClipPlacementStartProp)) - start) > 1.0e-9)
            mc.state.setProperty(kClipPlacementStartProp, start, u);
        const auto key = trackKey(track.itemID);
        if (mc.state.getProperty(kClipTrackProp).toString() != key)
            mc.state.setProperty(kClipTrackProp, key, u);
        const auto sig = notesSignature(mc);
        if (!mc.state.hasProperty(kClipNotesSigProp) || mc.state.getProperty(kClipNotesSigProp).toString() != sig)
            mc.state.setProperty(kClipNotesSigProp, sig, u);
    }

    // Rebuilds one placement's clips. `forcedStart` is used for new placements.
    // `skip` (the clip whose hand edit was just written back) is left exactly
    // as it is, so the write-back never fights the editor showing it.
    RegenResult regenerateGroup(const Pattern& p, int placementId, const std::vector<TaggedClip>& clips,
                                std::optional<double> forcedStart, te::MidiClip* skip = nullptr)
    {
        RegenResult res;
        auto* e = edit();
        auto* u = um();
        if (!e) return res;

        const double start = std::max(0.0, forcedStart ? *forcedStart : derivedStart(placementId, clips));
        const double len = p.lengthBeats();
        const auto clipName = toJ(p.name);
        const auto colour = juce::Colour::fromRGB(static_cast<juce::uint8>(p.colour.red()),
                                                  static_cast<juce::uint8>(p.colour.green()),
                                                  static_cast<juce::uint8>(p.colour.blue()));
        const auto pnode = patternNode(p.id);
        const auto channelTracks = channelNodeTracks(pnode);

        struct Target { te::AudioTrack* track; const PatternChannel* channel; };
        std::vector<Target> targets;
        std::set<uint64_t> targetTracks;
        for (auto& ch : p.channels)
            if (auto* t = em.findAudioTrack(ch.trackId)) {
                targets.push_back({ t, &ch });
                targetTracks.insert(t->itemID.getRawID());
            }

        // Existing clips, one per track (the skipped clip first); the rest are deleted.
        std::unordered_map<uint64_t, te::MidiClip*> byTrack;
        std::vector<te::MidiClip*> toDelete;
        for (auto& c : clips)
            if (c.clip == skip) byTrack[c.track->itemID.getRawID()] = c.clip;
        for (auto& c : clips) {
            if (c.clip == skip) continue;
            const auto raw = c.track->itemID.getRawID();
            if (byTrack.count(raw)) toDelete.push_back(c.clip);
            else byTrack[raw] = c.clip;
        }

        // An empty channel (kept for a piano-roll clip) keeps its existing clip.
        for (auto& [raw, mc] : byTrack)
            if (!targetTracks.count(raw) && channelTracks.count(raw))
                if (auto* t = dynamic_cast<te::AudioTrack*>(mc->getTrack())) {
                    targets.push_back({ t, nullptr });
                    targetTracks.insert(raw);
                }

        if (targets.empty()) {
            // An empty pattern keeps one empty placeholder clip, so the
            // placement stays on the playlist.
            te::AudioTrack* placeholderTrack = nullptr;
            if (!clips.empty()) placeholderTrack = clips.front().track;
            else if (forcedStart) {
                auto chans = owner.channels();
                if (!chans.isEmpty()) placeholderTrack = chans.first();
            }
            if (placeholderTrack)
                targets.push_back({ placeholderTrack, nullptr });
        }

        std::set<uint64_t> used;
        auto& ts = e->tempoSequence;
        const tracktion::TimeRange targetRange(ts.toTime(tracktion::BeatPosition::fromBeats(start)),
                                               ts.toTime(tracktion::BeatPosition::fromBeats(start + len)));

        for (auto& tg : targets) {
            const auto raw = tg.track->itemID.getRawID();
            used.insert(raw);
            te::MidiClip* mc = nullptr;
            if (auto it = byTrack.find(raw); it != byTrack.end()) mc = it->second;
            if (mc && mc == skip) continue;

            if (!mc) {
                auto ptr = tg.track->insertMIDIClip(clipName, targetRange, nullptr);
                mc = ptr.get();
                if (!mc) continue;
                mc->state.setProperty(kClipPatternIdProp, p.id, u);
                mc->state.setProperty(kClipPlacementIdProp, placementId, u);
                res.structural = true;
            } else {
                const auto pos = mc->getPosition();
                const bool moved = std::abs(clipStartBeat(*mc) - start) > 1.0e-6
                                || std::abs(clipEndBeat(*mc) - (start + len)) > 1.0e-6;
                if (moved || pos.getOffset() != tracktion::TimeDuration()) {
                    mc->setPosition(te::ClipPosition{ targetRange, tracktion::TimeDuration() });
                    if (moved) res.structural = true;
                    noteClipModified(mc);
                }
            }

            if (mc->getName() != clipName) { mc->setName(clipName); noteClipModified(mc); }
            if (mc->getColour() != colour) { mc->setColour(colour); noteClipModified(mc); }

            const std::vector<DesiredNote> desired =
                tg.channel ? desiredNotes(p, tg.channel->notes, owner.channelRootNote(tg.channel->trackId))
                           : std::vector<DesiredNote>{};

            auto& seq = mc->getSequence();
            const auto& existing = seq.getNotes();
            bool same = existing.size() == int(desired.size());
            if (same) {
                std::vector<DesiredNote> have;
                for (auto* n : existing)
                    have.push_back({ n->getNoteNumber(), n->getStartBeat().inBeats(),
                                     n->getLengthBeats().inBeats(), n->getVelocity() });
                auto cmp = [](const DesiredNote& a, const DesiredNote& b) {
                    if (std::abs(a.start - b.start) > 1.0e-6) return a.start < b.start;
                    if (a.pitch != b.pitch) return a.pitch < b.pitch;
                    if (std::abs(a.length - b.length) > 1.0e-6) return a.length < b.length;
                    return a.velocity < b.velocity;
                };
                std::sort(have.begin(), have.end(), cmp);
                auto want = desired;
                std::sort(want.begin(), want.end(), cmp);
                for (size_t i = 0; same && i < want.size(); ++i)
                    same = have[i].pitch == want[i].pitch
                        && std::abs(have[i].start - want[i].start) < 1.0e-6
                        && std::abs(have[i].length - want[i].length) < 1.0e-6
                        && have[i].velocity == want[i].velocity;
            }
            if (!same) {
                seq.removeAllNotes(u);
                for (auto& d : desired)
                    seq.addNote(d.pitch, tracktion::BeatPosition::fromBeats(d.start),
                                tracktion::BeatDuration::fromBeats(d.length), d.velocity, 0, u);
                noteClipModified(mc);
            }

            tagClip(*mc, p.id, placementId, start, *tg.track, u);
        }

        // Clips on channels the pattern no longer uses
        for (auto& [raw, mc] : byTrack)
            if (!used.count(raw) && mc != skip) toDelete.push_back(mc);
        for (auto* mc : toDelete) {
            pendingClips.erase(mc->itemID.getRawID());
            mc->removeFromParent();
            res.structural = true;
            res.removedClips = true;
        }

        lastStart[placementId] = start;
        if (res.structural) pendingEdit = true;
        return res;
    }

    RegenResult regenerate(int patternId, te::MidiClip* skip = nullptr)
    {
        RegenResult total;
        auto p = readPattern(patternId);
        if (!p) return total;
        normalizePlacements();
        auto groups = groupByPlacement(scanTagged(), patternId);
        for (auto& [pid, clips] : groups) {
            auto r = regenerateGroup(*p, pid, clips, std::nullopt, skip);
            total.structural |= r.structural;
            total.removedClips |= r.removedClips;
        }
        return total;
    }

    // ── Hand edits of placement clips ───────────────────────────────────────

    // A clip's notes as pattern notes: clip beats -> steps, notes outside the
    // pattern clipped. A note still at the start and pitch of a stored note
    // keeps that note's encoding (-1 = follows the channel's root note);
    // other notes store their pitch.
    QVector<PatternNote> notesFromClip(te::MidiClip& mc, const Pattern& p, int root,
                                       const QVector<PatternNote>& previous) const
    {
        const double offsetBeats = mc.getContentBeatAtTime(mc.getPosition().getStart()).inBeats();
        const double spb = double(p.stepsPerBeat);
        QVector<PatternNote> notes;
        for (auto* n : mc.getSequence().getNotes()) {
            double s = (n->getStartBeat().inBeats() - offsetBeats) * spb;
            double l = n->getLengthBeats().inBeats() * spb;
            if (s < 0.0) { l += s; s = 0.0; }   // starts before the clip content: keep the audible part
            const int pitch = std::clamp(n->getNoteNumber(), 0, 127);
            int note = pitch;
            for (const auto& prev : previous)
                if (std::abs(prev.startStep - s) < 1.0e-4 && (prev.note >= 0 ? prev.note : root) == pitch) {
                    note = prev.note;
                    break;
                }
            notes.push_back({ s, l, note, std::clamp(n->getVelocity(), 1, 127) });
        }
        return normalizeNotes(std::move(notes), p.lengthSteps);
    }

    // Replaces a channel's notes in the pattern node (undoable). An empty
    // list removes the channel unless keepEmpty (the channel then stays, with
    // no notes, so its piano-roll clip is kept). True when something changed.
    bool writeNotes(juce::ValueTree pnode, te::EditItemID trackId, QVector<PatternNote> notes, bool keepEmpty = false)
    {
        auto* u = um();
        const int len = std::clamp(int(pnode.getProperty(kLengthProp, 16)), 1, kMaxSteps);
        notes = normalizeNotes(std::move(notes), len);
        auto ch = channelNode(pnode, trackId);
        if (notes.isEmpty() && !keepEmpty) {
            if (!ch.isValid()) return false;
            pnode.removeChild(ch, u);
            return true;
        }
        if (!ch.isValid()) {
            ch = juce::ValueTree(kChannelNode);
            ch.setProperty(kTrackProp, trackKey(trackId), nullptr);
            pnode.appendChild(ch, u);
        } else {
            bool hasNoteNodes = false;
            for (auto n : ch) if (n.hasType(kNoteNode)) { hasNoteNodes = true; break; }
            if ((hasNoteNodes || !ch.hasProperty(kStepsProp)) && sameNotes(readNotes(ch, len), notes))
                return false;
        }
        if (ch.hasProperty(kStepsProp)) ch.removeProperty(kStepsProp, u); // legacy row: now NOTEs
        for (int i = ch.getNumChildren(); --i >= 0;)
            if (ch.getChild(i).hasType(kNoteNode)) ch.removeChild(i, u);
        for (const auto& n : notes) {
            juce::ValueTree nn(kNoteNode);
            nn.setProperty(kNoteStartProp, n.startStep, nullptr);
            nn.setProperty(kNoteLengthProp, n.lengthSteps, nullptr);
            nn.setProperty(kNoteNoteProp, n.note, nullptr);
            nn.setProperty(kNoteVelocityProp, n.velocity, nullptr);
            ch.appendChild(nn, u);
        }
        return true;
    }

    QVector<PatternNote> channelNotesOf(const juce::ValueTree& pnode, te::EditItemID trackId) const
    {
        const int len = std::clamp(int(pnode.getProperty(kLengthProp, 16)), 1, kMaxSteps);
        auto ch = channelNode(pnode, trackId);
        if (!ch.isValid()) return {};
        return readNotes(ch, len);
    }

    // A placement clip found on a track other than its channel's was moved
    // there (or copied there): it becomes an ordinary clip. Clips from before
    // the channel was recorded adopt their current track.
    bool untagMovedClips()
    {
        bool changed = false;
        for (auto& c : scanTagged()) {
            const auto stored = c.clip->state.getProperty(kClipTrackProp);
            if (stored.isVoid()) continue;
            if (trackIdFromVar(stored) == c.track->itemID) continue;
            untag(*c.clip, um());
            changed = true;
        }
        return changed;
    }

    // Writes hand-edited placement clips back into their patterns, one clip
    // per pattern (a clip in `preferred`, i.e. edited just now, if any), and
    // regenerates the pattern's other placements. A hand-edited clip that
    // was split or trimmed at its start (its content no longer starts at
    // the pattern start, or it is shorter than the pattern) becomes an
    // ordinary clip instead. Writes join the current undo transaction.
    // Call inside a WriteScope.
    void writeBackHandEdits(const std::set<uint64_t>& preferred, RegenResult& total, std::set<int>& changedPatterns)
    {
        std::map<int, std::optional<Pattern>> cache;
        std::map<int, std::vector<TaggedClip>> edited;
        for (auto& c : scanTagged()) {
            auto it = cache.find(c.patternId);
            if (it == cache.end()) it = cache.emplace(c.patternId, readPattern(c.patternId)).first;
            if (!it->second) continue;
            if (!isHandEdited(*c.clip, *it->second, *c.track)) continue;
            const double offsetBeats = c.clip->getContentBeatAtTime(c.clip->getPosition().getStart()).inBeats();
            const double lenBeats = clipEndBeat(*c.clip) - c.start;
            if (std::abs(offsetBeats) > kBeatEps || lenBeats < it->second->lengthBeats() - kBeatEps) {
                untag(*c.clip, um());
                total.structural = true;
                continue;
            }
            edited[c.patternId].push_back(c);
        }
        for (auto& [pid, clips] : edited) {
            const Pattern& p = *cache[pid];
            const TaggedClip* pick = &clips.front();
            for (auto& c : clips)
                if (preferred.count(c.clip->itemID.getRawID())) { pick = &c; break; }
            auto pnode = patternNode(pid);
            if (!pnode.isValid()) continue;
            const auto trackId = pick->track->itemID;
            const auto notes = notesFromClip(*pick->clip, p, owner.channelRootNote(trackId),
                                             channelNotesOf(pnode, trackId));
            // The channel stays (empty) when every note was deleted, so the
            // clip the piano roll shows is not removed under it.
            writeNotes(pnode, trackId, notes, /*keepEmpty*/ true);
            const auto sig = notesSignature(*pick->clip);
            if (pick->clip->state.getProperty(kClipNotesSigProp).toString() != sig || !pick->clip->state.hasProperty(kClipNotesSigProp))
                pick->clip->state.setProperty(kClipNotesSigProp, sig, um());
            const auto key = trackKey(trackId);
            if (pick->clip->state.getProperty(kClipTrackProp).toString() != key)
                pick->clip->state.setProperty(kClipTrackProp, key, um());
            auto r = regenerate(pid, pick->clip);
            total.structural |= r.structural;
            total.removedClips |= r.removedClips;
            changedPatterns.insert(pid);
        }
    }

    // Processes pending clip changes now: moved clips, then hand edits.
    // Used from the event loop and at the start of every mutator, so a
    // pattern edit always starts from the notes the user sees.
    void processClipChanges()
    {
        auto preferred = std::move(pendingWriteBack);
        pendingWriteBack.clear();
        clipCheckPending = false;
        if (!edit() || edit() != attachedEdit || !tree.isValid()) return;
        if (em.isRenderInProgress()) {
            // Nothing may change the Edit during a render: try again later.
            pendingWriteBack.insert(preferred.begin(), preferred.end());
            QPointer<PatternManager> guard(&owner);
            QTimer::singleShot(100, &owner, [this, guard] { if (guard) scheduleClipCheck(); });
            return;
        }
        RegenResult total;
        std::set<int> changed;
        {
            WriteScope ws(*this);
            if (untagMovedClips()) total.structural = true;
            writeBackHandEdits(preferred, total, changed);
        }
        if (!total.structural && !total.removedClips && changed.empty()) return;
        for (int id : changed) emit owner.patternChanged(id);
        pendingEdit = true;
        finish(total);
    }

    void scheduleClipCheck()
    {
        if (clipCheckScheduled) return;
        clipCheckScheduled = true;
        QPointer<PatternManager> guard(&owner);
        QTimer::singleShot(0, &owner, [this, guard] {
            if (!guard) return;
            clipCheckScheduled = false;
            processClipChanges();
        });
    }

    // From ClipWatcher: a note of some clip changed (t = NOTE or SEQUENCE).
    void onClipTreeChanged(const juce::ValueTree& t)
    {
        if (suppressListener > 0) return;
        juce::ValueTree n = t;
        if (n.hasType(te::IDs::NOTE)) n = n.getParent();
        if (!n.hasType(te::IDs::SEQUENCE)) return;
        n = n.getParent();
        if (n.hasType(te::IDs::TAKES)) n = n.getParent();
        if (!n.hasType(te::IDs::MIDICLIP) || !n.hasProperty(kClipPlacementIdProp)) return;
        pendingWriteBack.insert(te::EditItemID::fromID(n).getRawID());
        scheduleClipCheck();
    }

    // From ClipWatcher: a MIDI clip was added to a track (new, pasted, moved
    // between tracks, or restored by undo).
    void onClipArrived(const juce::ValueTree& clipState)
    {
        if (suppressListener > 0 || !clipState.hasProperty(kClipPlacementIdProp)) return;
        clipCheckPending = true;
        scheduleClipCheck();
    }

    // Ends a public mutation: placement signal and timeline refresh. Removing
    // clips refreshes synchronously so no view keeps a deleted clip.
    void finish(const RegenResult& r)
    {
        const auto sig = computeSignature();
        if (r.structural || sig != placementSignature) {
            placementSignature = sig;
            emit owner.placementsChanged();
        }
        if (r.removedClips) flush();
        else scheduleFlush();
    }

    // ── UI refresh ──────────────────────────────────────────────────────────
    void scheduleFlush()
    {
        if (!pendingEdit && pendingClips.empty()) return;
        if (flushScheduled) return;
        flushScheduled = true;
        QPointer<PatternManager> guard(&owner);
        QTimer::singleShot(0, &owner, [this, guard] {
            if (!guard) return;
            flushScheduled = false;
            flush();
        });
    }

    void flush()
    {
        if (edit() != attachedEdit || !edit()) {
            pendingEdit = false;
            pendingClips.clear();
            return;
        }
        const bool doEdit = pendingEdit || !pendingClips.empty();
        auto clipIds = std::move(pendingClips);
        pendingClips.clear();
        pendingEdit = false;

        if (doEdit) em.notifyEdited();
        // Tagged clips and clips untagged just now (hand-edited ones).
        for (auto raw : clipIds)
            if (auto* mc = dynamic_cast<te::MidiClip*>(te::findClipForID(*edit(), te::EditItemID::fromRawID(raw))))
                emit em.midiClipModified(mc);
    }

    // ── Edit changes from elsewhere (timeline edits, undo/redo) ─────────────
    void onEditChanged()
    {
        if (inEditChangedHandler) return;
        if (edit() != attachedEdit || !tree.isValid()) { attach(); return; }
        inEditChangedHandler = true;
        normalizePlacements();
        const auto sig = computeSignature();
        if (sig != placementSignature) {
            placementSignature = sig;
            emit owner.placementsChanged();
        }
        validateCurrentPattern();
        inEditChangedHandler = false;
    }

    void validateCurrentPattern()
    {
        if (owner.currentPattern_ != 0 && patternNode(owner.currentPattern_).isValid()) return;
        auto list = owner.patterns();
        const int next = list.isEmpty() ? 0 : list.first().id;
        if (next != owner.currentPattern_) {
            owner.currentPattern_ = next;
            emit owner.currentPatternChanged(next);
        }
    }

    // ── juce::ValueTree::Listener (undo/redo and other external writes) ─────
    int patternIdFor(const juce::ValueTree& t) const
    {
        // PATTERN, CHANNEL or NOTE: the enclosing PATTERN's id.
        juce::ValueTree n = t;
        for (int depth = 0; depth < 3 && n.isValid(); ++depth, n = n.getParent())
            if (n.hasType(kPatternNode)) return int(n.getProperty(kIdProp, 0));
        return 0;
    }

    void markExternal(const juce::ValueTree& t, bool structural)
    {
        if (suppressListener > 0) return;
        if (t.hasType(kPatternsId)) {
            if (structural) externalPatternsDirty = true;
            else return; // counters only
        } else if (t.hasType(kPatternNode)) {
            if (structural) externalPatternsDirty = true;
            externalDirtyPatterns.insert(int(t.getProperty(kIdProp, 0)));
        } else if (const int id = patternIdFor(t); id > 0) {
            externalDirtyPatterns.insert(id);
        } else {
            return;
        }
        if (externalFlushScheduled) return;
        externalFlushScheduled = true;
        QPointer<PatternManager> guard(&owner);
        QTimer::singleShot(0, &owner, [this, guard] {
            if (!guard) return;
            externalFlushScheduled = false;
            flushExternal();
        });
    }

    void flushExternal()
    {
        const bool patternsDirty = externalPatternsDirty;
        auto ids = std::move(externalDirtyPatterns);
        externalDirtyPatterns.clear();
        externalPatternsDirty = false;
        if (!patternsDirty && ids.empty()) return;
        if (patternsDirty) emit owner.patternsChanged();
        for (int id : ids) emit owner.patternChanged(id);
        validateCurrentPattern();
        const auto sig = computeSignature();
        if (sig != placementSignature) {
            placementSignature = sig;
            emit owner.placementsChanged();
        }
    }

    void valueTreePropertyChanged(juce::ValueTree& t, const juce::Identifier& prop) override
    {
        if (t.hasType(kPatternNode))
            markExternal(t, prop == kNameProp || prop == kColourProp || prop == kLengthProp
                             || prop == kStepsPerBeatProp || prop == kIdProp);
        else
            markExternal(t, false);
    }
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override
    {
        markExternal(child.hasType(kPatternNode) ? child : parent, child.hasType(kPatternNode));
    }
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int) override
    {
        if (child.hasType(kPatternNode)) markExternal(parent, true);
        else markExternal(parent, false);
    }
    void valueTreeChildOrderChanged(juce::ValueTree& parent, int, int) override
    {
        markExternal(parent, parent.hasType(kPatternsId));
    }

    // ── Writing helpers ─────────────────────────────────────────────────────
    struct WriteScope {
        Impl& impl;
        explicit WriteScope(Impl& i) : impl(i) { ++impl.suppressListener; }
        ~WriteScope() { --impl.suppressListener; }
    };

    // Removes channel rows whose track was deleted.
    void dropDeadChannels(juce::ValueTree pnode)
    {
        for (int i = pnode.getNumChildren(); --i >= 0;) {
            auto child = pnode.getChild(i);
            if (!child.hasType(kChannelNode)) continue;
            if (!em.findAudioTrack(trackIdFromVar(child.getProperty(kTrackProp))))
                pnode.removeChild(i, um());
        }
    }

    // The first bar line at or after the end of every clip in the Edit
    // (beat 0 in an empty song).
    double firstFreeBarBeat() const
    {
        auto* e = edit();
        if (!e) return 0.0;
        double end = 0.0;
        for (auto* t : te::getAudioTracks(*e))
            if (t)
                for (auto* c : t->getClips())
                    if (c) end = std::max(end, clipEndBeat(*c));
        if (end <= kBeatEps) return 0.0;
        auto& ts = e->tempoSequence;
        auto bb = ts.toBarsAndBeats(ts.toTime(tracktion::BeatPosition::fromBeats(end)));
        tracktion::tempo::BarsAndBeats bar;
        bar.bars = bb.bars + (bb.beats.inBeats() > kBeatEps ? 1 : 0);
        return ts.toBeats(bar).inBeats();
    }

    // An empty placement clip for `track` (editableClipFor).
    te::MidiClip* addEmptyPlacementClip(const Pattern& p, int placementId, double start, te::AudioTrack& track)
    {
        auto* e = edit();
        if (!e) return nullptr;
        auto& ts = e->tempoSequence;
        const tracktion::TimeRange range(ts.toTime(tracktion::BeatPosition::fromBeats(start)),
                                         ts.toTime(tracktion::BeatPosition::fromBeats(start + p.lengthBeats())));
        auto ptr = track.insertMIDIClip(toJ(p.name), range, nullptr);
        auto* mc = ptr.get();
        if (!mc) return nullptr;
        mc->setColour(juce::Colour::fromRGB(static_cast<juce::uint8>(p.colour.red()),
                                            static_cast<juce::uint8>(p.colour.green()),
                                            static_cast<juce::uint8>(p.colour.blue())));
        tagClip(*mc, p.id, placementId, start, track, um());
        lastStart[placementId] = start;
        noteClipModified(mc);
        pendingEdit = true;
        return mc;
    }

    bool hasPlacements(int patternId) const
    {
        for (auto& c : scanTagged())
            if (c.patternId == patternId) return true;
        return false;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// PatternManager
// ═════════════════════════════════════════════════════════════════════════════

PatternManager::PatternManager(EditManager& editMgr, QObject* parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>(*this, editMgr))
    , editMgr_(editMgr)
{
    connect(&editMgr_, &EditManager::aboutToChangeEdit, this, [this] {
        impl_->detach();
    });
    connect(&editMgr_, &EditManager::editChanged, this, [this] { impl_->onEditChanged(); });
    connect(&editMgr_, &EditManager::tracksChanged, this, [this] {
        if (editMgr_.edit() != impl_->attachedEdit || !impl_->tree.isValid()) { impl_->attach(); return; }
        // Channels may have been added, removed or reordered.
        emit patternsChanged();
        if (currentPattern_ != 0) emit patternChanged(currentPattern_);
    });
    impl_->attach();
}

PatternManager::~PatternManager()
{
    impl_->detach();
}

QVector<QColor> PatternManager::palette()
{
    static const QVector<QColor> colours = {
        QColor(0xE5, 0x73, 0x73), // coral red
        QColor(0xFF, 0xA7, 0x4D), // orange
        QColor(0xFF, 0xD5, 0x4F), // amber
        QColor(0xAE, 0xD5, 0x81), // light green
        QColor(0x66, 0xBB, 0x6A), // green
        QColor(0x4D, 0xB6, 0xAC), // teal
        QColor(0x4F, 0xC3, 0xF7), // sky blue
        QColor(0x64, 0x8F, 0xE8), // blue
        QColor(0x95, 0x75, 0xCD), // violet
        QColor(0xBA, 0x68, 0xC8), // purple
        QColor(0xF0, 0x62, 0x92), // pink
        QColor(0xA1, 0x88, 0x7F), // warm grey-brown
    };
    return colours;
}

// ── Channels ────────────────────────────────────────────────────────────────

QVector<te::AudioTrack*> PatternManager::channels() const
{
    QVector<te::AudioTrack*> out;
    if (!editMgr_.edit()) return out;
    for (auto* t : editMgr_.getAudioTracksInDisplayOrder())
        if (t && editMgr_.isMidiTrack(t) && !editMgr_.isBusTrack(t))
            out.push_back(t);
    return out;
}

int PatternManager::channelRootNote(te::EditItemID trackId) const
{
    auto* t = editMgr_.findAudioTrack(trackId);
    if (!t) return kDefaultRootNote;
    return std::clamp(int(t->state.getProperty(kRootNoteProp, kDefaultRootNote)), 0, 127);
}

void PatternManager::setChannelRootNote(te::EditItemID trackId, int note)
{
    if (!impl_->canEdit()) return;
    auto* t = editMgr_.findAudioTrack(trackId);
    if (!t) return;
    note = std::clamp(note, 0, 127);
    if (channelRootNote(trackId) == note) return;

    impl_->processClipChanges();
    Impl::WriteScope ws(*impl_);
    impl_->beginTransaction("Set Channel Root Note");
    t->state.setProperty(kRootNoteProp, note, impl_->um());

    Impl::RegenResult total;
    std::vector<int> affected;
    for (auto& p : patterns()) {
        auto* ch = p.channel(trackId);
        if (!ch) continue;
        bool usesRoot = false;
        for (auto& n : ch->notes) if (n.note < 0) usesRoot = true;
        if (!usesRoot) continue;
        affected.push_back(p.id);
        auto r = impl_->regenerate(p.id);
        total.structural |= r.structural;
        total.removedClips |= r.removedClips;
    }
    for (int id : affected) emit patternChanged(id);
    if (currentPattern_ != 0 && std::find(affected.begin(), affected.end(), currentPattern_) == affected.end())
        emit patternChanged(currentPattern_);
    impl_->finish(total);
}

// ── Sample channels ─────────────────────────────────────────────────────────

namespace {

// The sampler loads its sounds in an AsyncUpdater callback. Running it now
// means a render or a live note straight after adding a sample is heard.
// (SamplerPlugin inherits AsyncUpdater privately; the C-style cast is the
// standard way to reach an inaccessible base.)
void loadSamplerSoundsNow(te::SamplerPlugin& sp)
{
    ((juce::AsyncUpdater*) &sp)->handleUpdateNowIfNeeded();
}

juce::ValueTree firstSound(te::SamplerPlugin& sp)
{
    return sp.state.getChildWithName(te::IDs::SOUND);
}

// One sound mapped over every key, root on `rootNote`, playing the whole
// file to its end whatever the note length (a one-shot). Every write goes
// through the plugin's (the Edit's) UndoManager.
bool configureSamplerSound(te::SamplerPlugin& sp, const juce::File& file, const juce::String& name,
                           int rootNote, juce::UndoManager* um, QString* error)
{
    // A sample channel holds exactly one sound: extra ones go.
    const auto first = firstSound(sp);
    for (int i = sp.state.getNumChildren(); --i >= 0;) {
        auto c = sp.state.getChild(i);
        if (c.hasType(te::IDs::SOUND) && c != first) sp.state.removeChild(c, um);
    }
    const auto path = file.getFullPathName();
    if (sp.getNumSounds() == 0) {
        const auto err = sp.addSound(path, name, 0.0, 0.0 /* whole file */, 0.0f);
        if (err.isNotEmpty()) {
            if (error) *error = QString::fromUtf8(err.toRawUTF8());
            return false;
        }
    } else {
        sp.setSoundMedia(0, path);
        sp.setSoundName(0, name);
        sp.setSoundExcerpt(0, 0.0, 0.0);
        sp.setSoundGains(0, 0.0f, 0.0f);
    }
    if (sp.getNumSounds() == 0) {
        if (error) *error = QStringLiteral("The sampler did not accept the sound.");
        return false;
    }
    sp.setSoundParams(0, rootNote, 0, 127);
    sp.setSoundOpenEnded(0, true);
    loadSamplerSoundsNow(sp);
    return true;
}

} // namespace

bool PatternManager::isAudioSampleFile(const juce::File& f)
{
    if (!f.existsAsFile() || !SampleLibrary::hasSampleExtension(toQ(f.getFileName())))
        return false;
    static juce::AudioFormatManager* formats = [] {
        auto* m = new juce::AudioFormatManager(); // intentionally never freed: used until exit
        m->registerBasicFormats();
        return m;
    }();
    std::unique_ptr<juce::AudioFormatReader> reader(formats->createReaderFor(f));
    return reader != nullptr && reader->lengthInSamples > 0;
}

te::SamplerPlugin* PatternManager::channelSampler(te::EditItemID trackId) const
{
    auto* t = editMgr_.findAudioTrack(trackId);
    if (!t) return nullptr;
    return dynamic_cast<te::SamplerPlugin*>(editMgr_.getTrackInstrument(t));
}

QString PatternManager::channelSamplePath(te::EditItemID trackId) const
{
    auto* sp = channelSampler(trackId);
    if (!sp) return {};
    const auto snd = firstSound(*sp);
    if (!snd.isValid()) return {};
    auto* t = editMgr_.findAudioTrack(trackId);
    const auto stored = t->state.getProperty(kSampleProp).toString();
    if (stored.isNotEmpty()) return QDir::fromNativeSeparators(toQ(stored));
    return QDir::fromNativeSeparators(toQ(snd.getProperty(te::IDs::source).toString()));
}

te::AudioTrack* PatternManager::addSampleChannel(const juce::File& sample, const QString& name, QString* error)
{
    auto fail = [error](const QString& why) -> te::AudioTrack* {
        if (error) *error = why;
        return nullptr;
    };
    auto* e = editMgr_.edit();
    if (!e) return fail(QStringLiteral("No project is open."));
    if (editMgr_.isRenderInProgress()) return fail(QStringLiteral("A render is in progress."));
    if (!isAudioSampleFile(sample))
        return fail(QStringLiteral("'%1' is not a readable audio file (wav, aif, aiff, flac, ogg, mp3).")
                        .arg(toQ(sample.getFullPathName())));

    auto* um = &e->getUndoManager();
    if (impl_->externalTransactions == 0 && !editMgr_.inUndoGroup())
        um->beginNewTransaction("Add Sample Channel");

    te::AudioTrack* track = nullptr;
    QString why;
    {
        // One refresh and one undo step for track + sampler + MIDI flag.
        EditManager::ScopedSignalBatch batch(editMgr_);
        editMgr_.beginUndoGroup();
        track = editMgr_.addAudioTrack();
        if (!track) {
            why = QStringLiteral("Could not create the track.");
        } else {
            const QString trimmed = name.trimmed();
            track->setName(trimmed.isEmpty() ? sample.getFileNameWithoutExtension() : toJ(trimmed));
            editMgr_.removeTrackInstrument(*track);
            auto plugin = e->getPluginCache().createNewPlugin(juce::String(te::SamplerPlugin::xmlTypeName), {});
            auto* sp = dynamic_cast<te::SamplerPlugin*>(plugin.get());
            if (!sp) {
                why = QStringLiteral("Could not create the sampler.");
            } else {
                track->pluginList.insertPlugin(plugin, 0, nullptr);
                if (configureSamplerSound(*sp, sample, sample.getFileNameWithoutExtension(), kDefaultRootNote, um, &why)) {
                    track->state.setProperty(kRootNoteProp, kDefaultRootNote, um);
                    track->state.setProperty(kSampleProp, sample.getFullPathName(), um);
                    editMgr_.markAsMidiTrack(track);
                } else if (why.isEmpty()) {
                    why = QStringLiteral("Could not load the sample.");
                }
            }
        }
        editMgr_.endUndoGroup();
    }
    if (!why.isEmpty()) {
        // Leave nothing half-built behind.
        if (track) editMgr_.removeTrack(track);
        return fail(why);
    }
    editMgr_.notifyEdited();
    return track;
}

bool PatternManager::setChannelSample(te::EditItemID trackId, const juce::File& sample, QString* error)
{
    auto fail = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };
    auto* e = editMgr_.edit();
    if (!e) return fail(QStringLiteral("No project is open."));
    if (editMgr_.isRenderInProgress()) return fail(QStringLiteral("A render is in progress."));
    auto* track = editMgr_.findAudioTrack(trackId);
    if (!track) return fail(QStringLiteral("Track not found."));
    if (!isAudioSampleFile(sample))
        return fail(QStringLiteral("'%1' is not a readable audio file (wav, aif, aiff, flac, ogg, mp3).")
                        .arg(toQ(sample.getFullPathName())));

    auto* um = &e->getUndoManager();
    if (impl_->externalTransactions == 0 && !editMgr_.inUndoGroup())
        um->beginNewTransaction("Set Channel Sample");

    const QString oldPath = channelSamplePath(trackId);
    QString why;
    {
        EditManager::ScopedSignalBatch batch(editMgr_);
        editMgr_.beginUndoGroup();
        auto* sp = channelSampler(trackId);
        if (!sp) {
            editMgr_.removeTrackInstrument(*track);
            auto plugin = e->getPluginCache().createNewPlugin(juce::String(te::SamplerPlugin::xmlTypeName), {});
            sp = dynamic_cast<te::SamplerPlugin*>(plugin.get());
            if (sp) track->pluginList.insertPlugin(plugin, 0, nullptr);
        }
        if (!sp) {
            why = QStringLiteral("Could not create the sampler.");
        } else if (!configureSamplerSound(*sp, sample, sample.getFileNameWithoutExtension(),
                                          channelRootNote(trackId), um, &why)) {
            if (why.isEmpty()) why = QStringLiteral("Could not load the sample.");
        } else {
            if (!oldPath.isEmpty()
                && track->getName() == juce::File(toJ(QDir::toNativeSeparators(oldPath))).getFileNameWithoutExtension())
                track->setName(sample.getFileNameWithoutExtension());
            track->state.setProperty(kSampleProp, sample.getFullPathName(), um);
            editMgr_.markAsMidiTrack(track);
        }
        editMgr_.endUndoGroup();
    }
    if (!why.isEmpty()) return fail(why);
    editMgr_.notifyEdited();
    return true;
}

// ── Sample library ──────────────────────────────────────────────────────────

namespace {

struct ScannedSample {
    QString path;      // absolute, '/' separators
    QString relative;  // relative to the scanned root
    QString lowered;   // relative, lower case (sort key)
    QString squashed;  // relative, lower case, letters and digits only
    QString fileLower;    // base name, lower case
    QString fileSquashed; // base name, letters and digits only
    QString parent;    // absolute folder
    qint64 size = 0;
};

using SampleList = std::vector<ScannedSample>;

struct ScanCache {
    QMutex mutex;
    struct Entry {
        qint64 scannedAtMs = 0;
        std::shared_ptr<const SampleList> samples;
    };
    std::map<QString, Entry> byRoot; // key: normalised root, lower case
    QStringList flRoots;             // auto-detected FL Studio pack folders
    qint64 flRootsAtMs = 0;
};

ScanCache& scanCache()
{
    static ScanCache c;
    return c;
}

constexpr qint64 kScanCacheMs = 60 * 1000;
constexpr int kMaxScannedFiles = 200000;

QString squash(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s)
        if (c.isLetterOrNumber()) out += c.toLower();
    return out;
}

QString normDir(const QString& dir)
{
    QString p = QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(dir).absoluteFilePath()));
    while (p.size() > 3 && p.endsWith(QLatin1Char('/'))) p.chop(1);
    return p;
}

bool samePath(const QString& a, const QString& b)
{
#ifdef Q_OS_WIN
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}

// `dir` is `root` or below it.
bool isUnder(const QString& dir, const QString& root)
{
    if (samePath(dir, root)) return true;
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
#ifdef Q_OS_WIN
    return dir.startsWith(prefix, Qt::CaseInsensitive);
#else
    return dir.startsWith(prefix);
#endif
}

QString relativeTo(const QString& path, const QString& root)
{
    if (samePath(path, root)) return {};
    return path.mid(root.size() + (root.endsWith(QLatin1Char('/')) ? 0 : 1));
}

std::shared_ptr<const SampleList> scanRoot(const QString& root)
{
    auto& cache = scanCache();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker lock(&cache.mutex);
        auto it = cache.byRoot.find(root.toLower());
        if (it != cache.byRoot.end() && now - it->second.scannedAtMs < kScanCacheMs)
            return it->second.samples;
    }

    auto list = std::make_shared<SampleList>();
    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot | QDir::Readable, QDirIterator::Subdirectories);
    int seen = 0;
    while (it.hasNext() && seen < kMaxScannedFiles) {
        it.next();
        ++seen;
        const QFileInfo fi = it.fileInfo();
        if (!SampleLibrary::hasSampleExtension(fi.fileName())) continue;
        ScannedSample s;
        s.path = QDir::fromNativeSeparators(fi.absoluteFilePath());
        s.relative = relativeTo(s.path, root);
        s.lowered = s.relative.toLower();
        s.squashed = squash(s.relative);
        s.fileLower = fi.completeBaseName().toLower();
        s.fileSquashed = squash(s.fileLower);
        s.parent = QDir::fromNativeSeparators(fi.absolutePath());
        s.size = fi.size();
        list->push_back(std::move(s));
    }
    std::sort(list->begin(), list->end(),
              [](const ScannedSample& a, const ScannedSample& b) { return a.lowered < b.lowered; });

    QMutexLocker lock(&cache.mutex);
    auto& entry = cache.byRoot[root.toLower()];
    entry.scannedAtMs = now;
    entry.samples = list;
    return entry.samples;
}

// Resolves `folder` (absolute, or relative to a root) to existing folders,
// each paired with the root whose scan covers it.
std::vector<std::pair<QString, QString>> resolveFolder(const QString& folder, const QStringList& roots)
{
    std::vector<std::pair<QString, QString>> out; // {dir, root}
    const QString f = QDir::fromNativeSeparators(folder.trimmed());
    if (f.isEmpty()) {
        for (auto& r : roots) out.push_back({ r, r });
        return out;
    }
    if (QDir::isAbsolutePath(f)) {
        if (!QFileInfo(f).isDir()) return out;
        const QString dir = normDir(f);
        for (auto& r : roots)
            if (isUnder(dir, r)) { out.push_back({ dir, r }); return out; }
        out.push_back({ dir, dir }); // outside every root: scanned on its own
        return out;
    }
    for (auto& r : roots) {
        const QString d = normDir(r + QLatin1Char('/') + f);
        if (QFileInfo(d).isDir() && isUnder(d, r)) out.push_back({ d, r });
    }
    return out;
}

// True when every query word is in the path; sets the rank.
bool matchSample(const ScannedSample& s, const QStringList& words, const QString& wholeQuery, int& score)
{
    score = 0;
    for (const auto& w : words) {
        const bool inFile = s.fileSquashed.contains(w);
        if (!inFile && !s.squashed.contains(w)) return false;
        score += inFile ? 100 : 10;
        if (inFile && s.fileSquashed.startsWith(w)) score += 20;
    }
    if (!wholeQuery.isEmpty() && s.fileSquashed == wholeQuery) score += 50;
    // Shallower paths first among equals.
    score -= int(s.relative.count(QLatin1Char('/')));
    return true;
}

} // namespace

bool SampleLibrary::hasSampleExtension(const QString& fileName)
{
    static const QStringList exts = { "wav", "aif", "aiff", "flac", "ogg", "mp3" };
    return exts.contains(QFileInfo(fileName).suffix().toLower());
}

void SampleLibrary::invalidateCache()
{
    auto& cache = scanCache();
    QMutexLocker lock(&cache.mutex);
    cache.byRoot.clear();
    cache.flRoots.clear();
    cache.flRootsAtMs = 0;
}

QStringList SampleLibrary::sampleRoots()
{
    QStringList candidates;
    QSettings settings;
    for (const auto& r : settings.value(QStringLiteral("samples/roots")).toStringList())
        if (!r.trimmed().isEmpty()) candidates << r.trimmed();

    // FL Studio pack folders on every mounted drive (newest version first).
    // Probing the drives is cached like the scans.
    {
        auto& cache = scanCache();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QMutexLocker lock(&cache.mutex);
        if (cache.flRootsAtMs == 0 || now - cache.flRootsAtMs >= kScanCacheMs) {
            QStringList found;
            QStringList driveRoots;
            // Only ready volumes: probing an empty optical or a disconnected drive can block.
            for (const auto& vol : QStorageInfo::mountedVolumes())
                if (vol.isValid() && vol.isReady()) driveRoots << QDir::fromNativeSeparators(vol.rootPath());
            if (driveRoots.isEmpty())
                for (const auto& d : QDir::drives()) driveRoots << QDir::fromNativeSeparators(d.absolutePath());
            driveRoots.removeDuplicates();
            for (const auto& drive : driveRoots) {
                for (const char* pf : { "Program Files", "Program Files (x86)" }) {
                    const QDir il(QDir(drive).filePath(QString::fromLatin1(pf) + QStringLiteral("/Image-Line")));
                    if (!il.exists()) continue;
                    for (const auto& fl : il.entryList({ QStringLiteral("FL Studio*") },
                                                       QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed)) {
                        const QString packs = il.filePath(fl + QStringLiteral("/Data/Patches/Packs"));
                        if (QFileInfo(packs).isDir()) found << packs;
                    }
                }
            }
            cache.flRoots = found;
            cache.flRootsAtMs = now;
        }
        candidates << cache.flRoots;
    }

    const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (!music.isEmpty()) candidates << music;

    QStringList out;
    for (const auto& c : candidates) {
        if (!QFileInfo(c).isDir()) continue;
        const QString n = normDir(c);
        bool dup = false;
        for (const auto& o : out)
            if (samePath(o, n)) { dup = true; break; }
        if (!dup) out << n;
    }
    return out;
}

QVector<SampleInfo> SampleLibrary::findSamples(const QString& query, const QString& folder, int limit,
                                               int* totalMatches)
{
    limit = std::clamp(limit, 1, 1000);
    if (totalMatches) *totalMatches = 0;
    const auto dirs = resolveFolder(folder, sampleRoots());

    QStringList words;
    for (const auto& w : query.split(QRegularExpression(QStringLiteral("[\\s/\\\\]+")), Qt::SkipEmptyParts))
        if (const auto sq = squash(w); !sq.isEmpty()) words << sq;
    const QString wholeQuery = squash(query);

    struct Hit { int score; const ScannedSample* s; QString root; };
    std::vector<Hit> hits;
    std::vector<std::shared_ptr<const SampleList>> keepAlive;
    std::set<QString> seenPaths;

    for (const auto& [dir, root] : dirs) {
        auto list = scanRoot(root);
        keepAlive.push_back(list);
        const bool whole = samePath(dir, root);
        for (const auto& s : *list) {
            if (!whole && !isUnder(s.parent, dir)) continue;
            int score = 0;
            if (!matchSample(s, words, wholeQuery, score)) continue;
            if (!seenPaths.insert(s.path.toLower()).second) continue; // nested roots
            hits.push_back({ score, &s, root });
        }
    }

    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.s->lowered < b.s->lowered;
    });
    if (totalMatches) *totalMatches = int(hits.size());

    QVector<SampleInfo> out;
    for (const auto& h : hits) {
        if (out.size() >= limit) break;
        SampleInfo info;
        info.path = h.s->path;
        info.name = QFileInfo(h.s->path).completeBaseName();
        info.relativePath = h.s->relative;
        const int cut = h.s->relative.lastIndexOf(QLatin1Char('/'));
        info.folder = cut < 0 ? QString() : h.s->relative.left(cut);
        info.root = h.root;
        info.size = h.s->size;
        out.push_back(std::move(info));
    }
    return out;
}

QVector<SampleFolderInfo> SampleLibrary::listFolders(const QString& folder, QString* resolved, int* direct,
                                                     QString* error)
{
    QVector<SampleFolderInfo> out;
    if (resolved) resolved->clear();
    if (direct) *direct = 0;
    const QStringList roots = sampleRoots();

    if (folder.trimmed().isEmpty()) {
        for (const auto& r : roots) {
            SampleFolderInfo fi;
            fi.path = r;
            fi.name = QFileInfo(r).fileName().isEmpty() ? r : QFileInfo(r).fileName();
            fi.sampleCount = int(scanRoot(r)->size());
            out.push_back(fi);
        }
        return out;
    }

    const auto dirs = resolveFolder(folder, roots);
    if (dirs.empty()) {
        if (error)
            *error = QStringLiteral("Folder '%1' not found. Give an absolute path or a path relative to a sample "
                                    "root (see list_sample_folders).").arg(folder);
        return out;
    }
    const auto& [dir, root] = dirs.front();
    if (resolved) *resolved = dir;
    auto list = scanRoot(root);

    std::map<QString, SampleFolderInfo> children; // key: lower-case child name
    const QString dirRel = relativeTo(dir, root);
    for (const auto& s : *list) {
        if (!isUnder(s.parent, dir)) continue;
        if (samePath(s.parent, dir)) {
            if (direct) ++*direct;
            continue;
        }
        const QString childName = relativeTo(s.parent, dir).section(QLatin1Char('/'), 0, 0);
        auto& c = children[childName.toLower()];
        if (c.path.isEmpty()) {
            c.name = childName;
            c.path = dir + (dir.endsWith(QLatin1Char('/')) ? QString() : QStringLiteral("/")) + childName;
            c.relativePath = dirRel.isEmpty() ? childName : dirRel + QLatin1Char('/') + childName;
        }
        ++c.sampleCount;
    }
    for (auto& [k, v] : children) out.push_back(v);
    return out;
}

QString SampleLibrary::resolveSamplePath(const QString& pathOrRelative)
{
    const QString p = QDir::fromNativeSeparators(pathOrRelative.trimmed());
    if (p.isEmpty()) return {};
    if (QDir::isAbsolutePath(p))
        return QFileInfo(p).isFile() ? QDir::cleanPath(p) : QString();
    for (const auto& r : sampleRoots()) {
        const QString full = QDir::cleanPath(r + QLatin1Char('/') + p);
        if (isUnder(full, r) && QFileInfo(full).isFile()) return full;
    }
    return {};
}

// ── Patterns ────────────────────────────────────────────────────────────────

QVector<Pattern> PatternManager::patterns() const
{
    QVector<Pattern> out;
    auto r = impl_->root(false);
    if (!r.isValid()) return out;
    const auto order = impl_->displayIndex();
    for (auto child : r)
        if (child.hasType(kPatternNode) && int(child.getProperty(kIdProp, 0)) > 0)
            out.push_back(impl_->readPattern(child, order));
    return out;
}

std::optional<Pattern> PatternManager::pattern(int patternId) const
{
    return impl_->readPattern(patternId);
}

bool PatternManager::patternExists(int patternId) const
{
    return impl_->patternNode(patternId).isValid();
}

int PatternManager::createPattern(const QString& name)
{
    if (!impl_->canEdit()) return 0;
    impl_->attach();
    Impl::WriteScope ws(*impl_);
    impl_->beginTransaction("New Pattern");

    const int id = impl_->allocatePatternId();
    const auto pal = palette();
    const QColor colour = pal[(id - 1) % pal.size()];
    const QString trimmed = name.trimmed();

    juce::ValueTree node(kPatternNode);
    node.setProperty(kIdProp, id, nullptr);
    node.setProperty(kNameProp, toJ(trimmed.isEmpty() ? QStringLiteral("Pattern %1").arg(id) : trimmed), nullptr);
    node.setProperty(kColourProp, (juce::int64) colour.rgba(), nullptr);
    node.setProperty(kLengthProp, 16, nullptr);
    node.setProperty(kStepsPerBeatProp, 4, nullptr);
    impl_->root(true).appendChild(node, impl_->um());

    emit patternsChanged();
    if (currentPattern_ == 0) setCurrentPattern(id);
    impl_->pendingEdit = true;
    impl_->scheduleFlush();
    return id;
}

int PatternManager::clonePattern(int patternId)
{
    if (!impl_->canEdit()) return 0;
    if (!impl_->patternNode(patternId).isValid()) return 0;
    impl_->processClipChanges(); // clone the notes the user sees
    auto src = impl_->patternNode(patternId);
    if (!src.isValid()) return 0;
    Impl::WriteScope ws(*impl_);
    impl_->beginTransaction("Clone Pattern");

    const int id = impl_->allocatePatternId();
    auto copy = src.createCopy();
    copy.setProperty(kIdProp, id, nullptr);
    copy.setProperty(kNameProp, src.getProperty(kNameProp).toString() + " (copy)", nullptr);
    const auto pal = palette();
    copy.setProperty(kColourProp, (juce::int64) pal[(id - 1) % pal.size()].rgba(), nullptr);
    auto r = impl_->root(true);
    r.addChild(copy, r.indexOf(src) + 1, impl_->um());
    impl_->dropDeadChannels(copy);

    emit patternsChanged();
    impl_->pendingEdit = true;
    impl_->scheduleFlush();
    return id;
}

bool PatternManager::removePattern(int patternId)
{
    if (!impl_->canEdit()) return false;
    auto node = impl_->patternNode(patternId);
    if (!node.isValid()) return false;
    Impl::WriteScope ws(*impl_);
    impl_->beginTransaction("Delete Pattern");

    Impl::RegenResult res;
    for (auto& c : impl_->scanTagged()) {
        if (c.patternId != patternId) continue;
        impl_->lastStart.erase(c.placementId);
        impl_->pendingClips.erase(c.clip->itemID.getRawID());
        c.clip->removeFromParent();
        res.structural = res.removedClips = true;
    }
    impl_->root(true).removeChild(node, impl_->um());

    if (currentPattern_ == patternId) {
        auto list = patterns();
        currentPattern_ = list.isEmpty() ? 0 : list.first().id;
        emit currentPatternChanged(currentPattern_);
    }
    emit patternsChanged();
    impl_->pendingEdit = true;
    impl_->finish(res);
    return true;
}

bool PatternManager::renamePattern(int patternId, const QString& name)
{
    if (!impl_->canEdit()) return false;
    auto node = impl_->patternNode(patternId);
    const QString trimmed = name.trimmed();
    if (!node.isValid() || trimmed.isEmpty()) return false;
    impl_->processClipChanges();
    Impl::WriteScope ws(*impl_);
    impl_->beginTransaction("Rename Pattern");
    node.setProperty(kNameProp, toJ(trimmed), impl_->um());
    auto r = impl_->regenerate(patternId);
    emit patternsChanged();
    impl_->pendingEdit = true;
    impl_->finish(r);
    return true;
}

bool PatternManager::setPatternColour(int patternId, const QColor& colour)
{
    if (!impl_->canEdit() || !colour.isValid()) return false;
    auto node = impl_->patternNode(patternId);
    if (!node.isValid()) return false;
    impl_->processClipChanges();
    Impl::WriteScope ws(*impl_);
    impl_->beginTransaction("Pattern Colour");
    node.setProperty(kColourProp, (juce::int64) colour.rgba(), impl_->um());
    auto r = impl_->regenerate(patternId);
    emit patternsChanged();
    impl_->pendingEdit = true;
    impl_->finish(r);
    return true;
}

bool PatternManager::setPatternLength(int patternId, int lengthSteps)
{
    if (!impl_->canEdit()) return false;
    if (lengthSteps < 1 || lengthSteps > kMaxSteps) return false;
    if (!impl_->patternNode(patternId).isValid()) return false;
    impl_->processClipChanges();
    auto node = impl_->patternNode(patternId);
    if (!node.isValid()) return false;
    Impl::WriteScope ws(*impl_);
    impl_->beginTransaction("Pattern Length");

    const int oldLen = std::clamp(int(node.getProperty(kLengthProp, 16)), 1, kMaxSteps);
    // Read the notes at the old length, then store them cut at the new end.
    struct Row { te::EditItemID track; QVector<PatternNote> notes; };
    std::vector<Row> rows;
    impl_->dropDeadChannels(node);
    for (auto child : node)
        if (child.hasType(kChannelNode))
            rows.push_back({ trackIdFromVar(child.getProperty(kTrackProp)), Impl::readNotes(child, oldLen) });
    node.setProperty(kLengthProp, lengthSteps, impl_->um());
    for (auto& row : rows)
        impl_->writeNotes(node, row.track, row.notes, /*keepEmpty*/ row.notes.isEmpty());

    auto r = impl_->regenerate(patternId);
    emit patternsChanged();
    emit patternChanged(patternId);
    impl_->pendingEdit = true;
    impl_->finish(r);
    return true;
}

bool PatternManager::setStep(int patternId, te::EditItemID trackId, int step, bool on,
                             int velocity, int note)
{
    if (!impl_->canEdit()) return false;
    auto node = impl_->patternNode(patternId);
    if (!node.isValid() || !editMgr_.findAudioTrack(trackId)) return false;
    const int len = std::clamp(int(node.getProperty(kLengthProp, 16)), 1, kMaxSteps);
    if (step < 0 || step >= len) return false;
    impl_->processClipChanges();
    node = impl_->patternNode(patternId);
    if (!node.isValid()) return false;

    const auto notes = impl_->channelNotesOf(node, trackId);
    auto updated = notes;
    const int k = stepNoteIndex(updated, step);
    if (!on) {
        if (k < 0) return true; // nothing to do
        updated.erase(std::remove_if(updated.begin(), updated.end(),
                                     [step](const PatternNote& n) { return startsOnStep(n, step); }),
                      updated.end());
    } else if (k < 0) {
        updated.push_back({ double(step), 1.0, note >= 0 ? std::min(note, 127) : -1,
                            velocity >= 0 ? std::clamp(velocity, 1, 127) : kDefaultVelocity });
    } else {
        auto& n = updated[k];
        if (velocity >= 0) n.velocity = std::clamp(velocity, 1, 127);
        if (note == -1) n.note = -1;
        else if (note >= 0) n.note = std::min(note, 127);
    }
    if (sameNotes(normalizeNotes(updated, len), notes)) return true; // nothing to do

    Impl::WriteScope ws(*impl_);
    impl_->beginTransaction("Edit Step");
    impl_->dropDeadChannels(node);
    impl_->writeNotes(node, trackId, updated);
    auto r = impl_->regenerate(patternId);
    emit patternChanged(patternId);
    impl_->pendingEdit = true;
    impl_->finish(r);
    return true;
}

bool PatternManager::toggleStep(int patternId, te::EditItemID trackId, int step)
{
    auto node = impl_->patternNode(patternId);
    if (!node.isValid()) return false;
    const int len = std::clamp(int(node.getProperty(kLengthProp, 16)), 1, kMaxSteps);
    if (step < 0 || step >= len) return false;
    const auto notes = impl_->channelNotesOf(node, trackId);
    return setStep(patternId, trackId, step, stepNoteIndex(notes, step) < 0);
}

bool PatternManager::clearChannel(int patternId, te::EditItemID trackId)
{
    if (!impl_->canEdit()) return false;
    if (!impl_->patternNode(patternId).isValid()) return false;
    impl_->processClipChanges();
    auto node = impl_->patternNode(patternId);
    if (!node.isValid()) return false;
    auto ch = Impl::channelNode(node, trackId);
    if (!ch.isValid()) return true;
    Impl::WriteScope ws(*impl_);
    impl_->beginTransaction("Clear Channel");
    node.removeChild(ch, impl_->um());
    auto r = impl_->regenerate(patternId);
    emit patternChanged(patternId);
    impl_->pendingEdit = true;
    impl_->finish(r);
    return true;
}

bool PatternManager::setChannelSteps(int patternId, te::EditItemID trackId, const QVector<PatternStep>& steps)
{
    if (!impl_->canEdit()) return false;
    auto node = impl_->patternNode(patternId);
    if (!node.isValid() || !editMgr_.findAudioTrack(trackId)) return false;
    impl_->processClipChanges();
    node = impl_->patternNode(patternId);
    if (!node.isValid()) return false;
    const int len = std::clamp(int(node.getProperty(kLengthProp, 16)), 1, kMaxSteps);
    QVector<PatternStep> row = steps;
    row.resize(len);
    for (auto& s : row) {
        s.velocity = std::clamp(s.velocity, 1, 127);
        if (s.note > 127) s.note = 127;
        if (s.note < -1) s.note = -1;
    }

    // Notes on a step follow the row; every other note is kept.
    const auto notes = impl_->channelNotesOf(node, trackId);
    QVector<PatternNote> updated;
    for (const auto& n : notes) {
        const int nearest = int(std::lround(n.startStep));
        if (startsOnStep(n, nearest) && nearest >= 0 && nearest < len && !row[nearest].on) continue;
        updated.push_back(n);
    }
    for (int i = 0; i < len; ++i) {
        if (!row[i].on) continue;
        const int k = stepNoteIndex(updated, i);
        if (k < 0) {
            updated.push_back({ double(i), 1.0, row[i].note, row[i].velocity });
        } else {
            updated[k].velocity = row[i].velocity;
            updated[k].note = row[i].note;
        }
    }

    Impl::WriteScope ws(*impl_);
    impl_->beginTransaction("Edit Steps");
    impl_->dropDeadChannels(node);
    impl_->writeNotes(node, trackId, updated);
    auto r = impl_->regenerate(patternId);
    emit patternChanged(patternId);
    impl_->pendingEdit = true;
    impl_->finish(r);
    return true;
}

// ── Notes ───────────────────────────────────────────────────────────────────

QVector<PatternNote> PatternManager::channelNotes(int patternId, te::EditItemID trackId) const
{
    auto node = impl_->patternNode(patternId);
    if (!node.isValid()) return {};
    return impl_->channelNotesOf(node, trackId);
}

bool PatternManager::setChannelNotes(int patternId, te::EditItemID trackId, const QVector<PatternNote>& notes)
{
    if (!impl_->canEdit()) return false;
    auto node = impl_->patternNode(patternId);
    if (!node.isValid() || !editMgr_.findAudioTrack(trackId)) return false;
    impl_->processClipChanges();
    node = impl_->patternNode(patternId);
    if (!node.isValid()) return false;
    const int len = std::clamp(int(node.getProperty(kLengthProp, 16)), 1, kMaxSteps);
    const bool exists = Impl::channelNode(node, trackId).isValid();
    const auto wanted = normalizeNotes(notes, len);
    if (exists && !wanted.isEmpty() && sameNotes(wanted, impl_->channelNotesOf(node, trackId)))
        return true; // nothing to do
    if (!exists && wanted.isEmpty()) return true;

    Impl::WriteScope ws(*impl_);
    impl_->beginTransaction("Edit Pattern Notes");
    impl_->dropDeadChannels(node);
    impl_->writeNotes(node, trackId, wanted);
    auto r = impl_->regenerate(patternId);
    emit patternChanged(patternId);
    impl_->pendingEdit = true;
    impl_->finish(r);
    return true;
}

te::MidiClip* PatternManager::editableClipFor(int patternId, te::EditItemID trackId)
{
    if (!impl_->canEdit() || !impl_->patternNode(patternId).isValid()) return nullptr;
    auto* track = editMgr_.findAudioTrack(trackId);
    if (!track || !editMgr_.isMidiTrack(track) || editMgr_.isBusTrack(track)) return nullptr;
    impl_->processClipChanges();
    auto node = impl_->patternNode(patternId);
    if (!node.isValid()) return nullptr;

    auto clipOnTrack = [&](int placementId) -> te::MidiClip* {
        for (auto& c : impl_->scanTagged())
            if (c.placementId == placementId && c.track == track) return c.clip;
        return nullptr;
    };

    auto existing = placements(patternId);
    if (!existing.isEmpty())
        if (auto* mc = clipOnTrack(existing.first().placementId)) return mc;

    // Something must be created: one undo step.
    Impl::WriteScope ws(*impl_);
    impl_->beginTransaction("Edit Pattern in Piano Roll");
    Impl::RegenResult r;
    r.structural = true;
    // The channel stays in the pattern while it has no notes, so the clip
    // the piano roll edits is kept by later regenerations.
    if (!Impl::channelNode(node, trackId).isValid())
        impl_->writeNotes(node, trackId, {}, /*keepEmpty*/ true);
    auto p = impl_->readPattern(patternId);
    if (!p) return nullptr;

    int placementId = 0;
    double start = 0.0;
    if (existing.isEmpty()) {
        impl_->normalizePlacements();
        placementId = impl_->allocatePlacementId();
        start = impl_->firstFreeBarBeat();
        if (!p->channels.isEmpty()) {
            auto g = impl_->regenerateGroup(*p, placementId, {}, start);
            r.removedClips |= g.removedClips;
        }
    } else {
        placementId = existing.first().placementId;
        start = existing.first().startBeat;
    }
    te::MidiClip* mc = clipOnTrack(placementId);
    if (!mc) mc = impl_->addEmptyPlacementClip(*p, placementId, start, *track);
    emit patternChanged(patternId);
    impl_->pendingEdit = true;
    impl_->finish(r);
    return mc;
}

int PatternManager::patternOfClip(const te::Clip* clip) const
{
    if (!clip || !dynamic_cast<const te::MidiClip*>(clip)) return 0;
    if (!clip->state.hasProperty(kClipPlacementIdProp)) return 0;
    const int id = int(clip->state.getProperty(kClipPatternIdProp, 0));
    return id > 0 && impl_->patternNode(id).isValid() ? id : 0;
}

// ── Placements ──────────────────────────────────────────────────────────────

int PatternManager::placePattern(int patternId, double startBeat)
{
    if (!impl_->canEdit() || !impl_->patternNode(patternId).isValid()) return 0;
    impl_->processClipChanges();
    auto p = pattern(patternId);
    if (!p) return 0;
    if (p->channels.isEmpty() && channels().isEmpty()) return 0; // nowhere to put a clip
    Impl::WriteScope ws(*impl_);
    impl_->beginTransaction("Place Pattern");
    impl_->normalizePlacements();
    const int placementId = impl_->allocatePlacementId();
    auto r = impl_->regenerateGroup(*p, placementId, {}, std::max(0.0, startBeat));
    bool created = false;
    for (auto& c : impl_->scanTagged())
        if (c.placementId == placementId) { created = true; break; }
    r.structural = true;
    impl_->finish(r);
    return created ? placementId : 0;
}

bool PatternManager::removePlacement(int placementId)
{
    if (!impl_->canEdit() || placementId <= 0) return false;
    std::vector<te::MidiClip*> clips;
    for (auto& c : impl_->scanTagged())
        if (c.placementId == placementId) clips.push_back(c.clip);
    if (clips.empty()) return false;
    Impl::WriteScope ws(*impl_);
    impl_->beginTransaction("Remove Pattern Placement");
    for (auto* mc : clips) {
        impl_->pendingClips.erase(mc->itemID.getRawID());
        mc->removeFromParent();
    }
    impl_->lastStart.erase(placementId);
    Impl::RegenResult r;
    r.structural = r.removedClips = true;
    impl_->pendingEdit = true;
    impl_->finish(r);
    return true;
}

QVector<PatternPlacement> PatternManager::placements(int patternId) const
{
    QVector<PatternPlacement> out;
    auto groups = impl_->groupByPlacement(impl_->scanTagged(), patternId);
    for (auto& [pid, clips] : groups) {
        if (clips.empty()) continue;
        const int patId = clips.front().patternId;
        if (!impl_->patternNode(patId).isValid()) continue;
        PatternPlacement pl;
        pl.placementId = pid;
        pl.patternId = patId;
        pl.startBeat = impl_->derivedStart(pid, clips);
        for (auto& c : clips) pl.clipIds.push_back(c.clip->itemID);
        out.push_back(std::move(pl));
    }
    std::stable_sort(out.begin(), out.end(), [](const PatternPlacement& a, const PatternPlacement& b) {
        if (std::abs(a.startBeat - b.startBeat) > kBeatEps) return a.startBeat < b.startBeat;
        return a.placementId < b.placementId;
    });
    return out;
}

void PatternManager::regeneratePlacements(int patternId)
{
    if (!impl_->canEdit() || !impl_->patternNode(patternId).isValid()) return;
    impl_->processClipChanges();
    Impl::WriteScope ws(*impl_);
    auto r = impl_->regenerate(patternId);
    impl_->finish(r);
}

// ── Current pattern / mode ──────────────────────────────────────────────────

void PatternManager::setCurrentPattern(int patternId)
{
    if (patternId != 0 && !impl_->patternNode(patternId).isValid()) return;
    if (patternId == currentPattern_) return;
    currentPattern_ = patternId;
    emit currentPatternChanged(patternId);
}

void PatternManager::setMode(Mode m)
{
    if (m == mode_) return;
    mode_ = m;
    emit modeChanged(m);
}

void PatternManager::flushEditNotifications()
{
    impl_->processClipChanges();
    impl_->flush();
}

void PatternManager::beginExternalTransaction()
{
    ++impl_->externalTransactions;
}

void PatternManager::endExternalTransaction()
{
    if (impl_->externalTransactions > 0) --impl_->externalTransactions;
}

// ═════════════════════════════════════════════════════════════════════════════
// PatternPlayer
// ═════════════════════════════════════════════════════════════════════════════

QVector<PatternPlayEvent> PatternPlayer::scheduleNotes(const Pattern& p, const PatternManager& pm)
{
    QVector<PatternPlayEvent> out;
    const double len = double(std::max(1, p.lengthSteps));
    for (const auto& ch : p.channels) {
        const int root = pm.channelRootNote(ch.trackId);
        for (const auto& n : ch.notes) {
            if (n.startStep < 0.0 || n.startStep >= len) continue;
            PatternPlayEvent ev;
            ev.trackId = ch.trackId;
            ev.note = std::clamp(n.note >= 0 ? n.note : root, 0, 127);
            ev.velocity = std::clamp(n.velocity, 1, 127);
            ev.startStep = n.startStep;
            ev.endStep = std::min(n.startStep + std::max(n.lengthSteps, kMinNoteSteps), len);
            out.push_back(ev);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const PatternPlayEvent& a, const PatternPlayEvent& b) {
        return a.startStep < b.startStep;
    });
    return out;
}

struct PatternPlayer::Impl {
    PatternPlayer& owner;
    PatternManager& pm;
    EditManager& em;

    QTimer timer;
    QElapsedTimer clock;
    int patternId = 0;

    // Play position in steps since play() (not wrapped: loop n covers
    // [n * length, (n + 1) * length)). Advanced by elapsed time at the
    // current tempo, so tempo changes apply at once.
    double posSteps = 0.0;
    double lastMs = 0.0;
    int lastStep = -1;

    // The pattern as scheduled; rebuilt when it changes.
    bool dirty = true;
    QVector<PatternPlayEvent> events;
    int lengthSteps = 16;
    int stepsPerBeat = 4;

    struct Sounding {
        uint64_t track;
        int note;
        double offPos; // in posSteps units
    };
    std::vector<Sounding> sounding;

    Impl(PatternPlayer& o, PatternManager& p, EditManager& e) : owner(o), pm(p), em(e)
    {
        timer.setTimerType(Qt::PreciseTimer);
        timer.setInterval(2);
    }

    double nowMs() const { return double(clock.nsecsElapsed()) / 1.0e6; }

    void send(uint64_t trackRaw, const juce::MidiMessage& msg)
    {
        if (!em.edit()) return;
        if (auto* t = em.findAudioTrack(te::EditItemID::fromRawID(trackRaw)))
            t->injectLiveMidiMessage(msg, 0);
    }

    void noteOff(const Sounding& s) { send(s.track, juce::MidiMessage::noteOff(1, s.note)); }

    void allNotesOff()
    {
        auto notes = std::move(sounding);
        sounding.clear();
        for (auto& s : notes) noteOff(s);
    }

    void releaseUpTo(double pos)
    {
        for (size_t i = 0; i < sounding.size();) {
            if (sounding[i].offPos <= pos) {
                noteOff(sounding[i]);
                sounding.erase(sounding.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    bool rebuild()
    {
        auto p = pm.pattern(patternId);
        if (!p) return false;
        events = PatternPlayer::scheduleNotes(*p, pm);
        lengthSteps = std::max(1, p->lengthSteps);
        stepsPerBeat = std::max(1, p->stepsPerBeat);
        dirty = false;
        return true;
    }

    void noteOn(const PatternPlayEvent& ev, double offPos)
    {
        const auto raw = ev.trackId.getRawID();
        for (size_t i = 0; i < sounding.size(); ++i) {
            if (sounding[i].track == raw && sounding[i].note == ev.note) {
                noteOff(sounding[i]);
                sounding.erase(sounding.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        send(raw, juce::MidiMessage::noteOn(1, ev.note, static_cast<juce::uint8>(ev.velocity)));
        sounding.push_back({ raw, ev.note, offPos });
    }

    void tick()
    {
        if (patternId == 0) return;
        // Never during a render: injected live notes would be mixed into it.
        if (!em.edit() || em.transport().isPlaying() || em.isRenderInProgress()) { owner.stop(); return; }
        if (dirty && !rebuild()) { owner.stop(); return; }

        const double now = nowMs();
        double dt = now - lastMs;
        lastMs = now;
        // After a stall (modal dialog, heavy load) carry on from where it was
        // instead of catching up with a burst of notes.
        if (dt > 250.0 || dt < 0.0) dt = 0.0;

        const double bpm = std::clamp(em.getBpm(), 1.0, 999.0);
        const double stepMs = 60000.0 / bpm / double(stepsPerBeat);
        const double from = posSteps;
        const double to = posSteps + dt / stepMs;
        const double len = double(lengthSteps);

        releaseUpTo(to);
        // Note-ons in [from, to), loop by loop, at their exact (off-grid) positions.
        const auto firstLoop = static_cast<long long>(std::floor(from / len));
        const auto lastLoop = static_cast<long long>(std::floor(to / len));
        for (long long loop = firstLoop; loop <= lastLoop; ++loop) {
            const double base = double(loop) * len;
            for (const auto& ev : events) {
                const double at = base + ev.startStep;
                if (at < from) continue;
                if (at >= to) break; // sorted by start
                noteOn(ev, base + ev.endStep);
            }
        }
        posSteps = to;

        double wrapped = std::fmod(std::max(0.0, to), len);
        const int step = std::clamp(int(std::floor(wrapped)), 0, lengthSteps - 1);
        if (step != lastStep) {
            lastStep = step;
            emit owner.stepChanged(step);
        }
    }
};

PatternPlayer::PatternPlayer(PatternManager& patterns, EditManager& editMgr, QObject* parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>(*this, patterns, editMgr))
{
    connect(&impl_->timer, &QTimer::timeout, this, [this] { impl_->tick(); });
    connect(&editMgr, &EditManager::aboutToChangeEdit, this, [this] { stop(); });
    connect(&editMgr, &EditManager::tracksChanged, this, [this] { impl_->dirty = true; });
    connect(&patterns, &PatternManager::patternsChanged, this, [this] {
        impl_->dirty = true;
        if (impl_->patternId != 0 && !impl_->pm.patternExists(impl_->patternId)) stop();
    });
    connect(&patterns, &PatternManager::patternChanged, this, [this](int id) {
        if (id == impl_->patternId) impl_->dirty = true;
    });
}

PatternPlayer::~PatternPlayer()
{
    impl_->timer.stop();
    impl_->allNotesOff();
}

void PatternPlayer::play(int patternId)
{
    if (!impl_->em.edit() || !impl_->pm.patternExists(patternId)) return;
    if (impl_->em.isRenderInProgress()) { stop(); return; } // refused while rendering
    if (impl_->em.transport().isPlaying())
        impl_->em.transport().stop(false, false);

    const bool wasPlaying = isPlaying();
    if (wasPlaying && impl_->patternId == patternId) return;

    impl_->patternId = patternId;
    impl_->dirty = true;
    if (!wasPlaying) {
        // Start from the first step now; switching patterns while playing keeps the beat.
        impl_->allNotesOff();
        impl_->clock.start();
        impl_->lastMs = 0.0;
        impl_->posSteps = -1.0e-9; // so notes on step 0 play on the first tick
        impl_->lastStep = -1;
        impl_->timer.start();
        emit playingChanged(true);
    }
    impl_->tick();
}

void PatternPlayer::stop()
{
    if (impl_->patternId == 0 && !impl_->timer.isActive()) return;
    impl_->timer.stop();
    impl_->allNotesOff();
    impl_->patternId = 0;
    impl_->posSteps = 0.0;
    impl_->lastStep = -1;
    emit stepChanged(-1);
    emit playingChanged(false);
}

bool PatternPlayer::isPlaying() const
{
    return impl_->patternId != 0 && impl_->timer.isActive();
}

int PatternPlayer::playingPattern() const
{
    return isPlaying() ? impl_->patternId : 0;
}

} // namespace OpenDaw
