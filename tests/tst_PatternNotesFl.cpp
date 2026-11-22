// Pattern notes as the source of truth (FL-style channel rack + piano roll):
// notes round trip and survive save/reload (legacy step-only data is read as
// 1-step notes), step edits keep off-grid and long notes, hand edits of a
// placement clip are written back into the pattern and copied to its other
// placements (one undo restores both), editableClipFor, patternOfClip,
// set_pattern_notes / get_pattern, and the pattern player's note schedule.

#include "TestHarness.h"

#include "ai/AiIds.h"
#include "ai/AiToolExtensions.h"
#include "engine/EditManager.h"
#include "engine/PatternManager.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

using namespace OpenDaw;
using OpenDaw::Test::harness;
using OpenDaw::Test::TestHarness;

namespace {

const juce::Identifier kPlacementIdProp("OpenDaw_placementId");

PatternManager& pm() { return harness().patternManager(); }
EditManager& em() { return harness().editManager(); }

QString dump(const QJsonObject& o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)).left(1500);
}

juce::File toJuce(const QString& path)
{
    return juce::File(juce::String::fromUTF8(QDir::toNativeSeparators(path).toUtf8().constData()));
}

double beatOf(te::Clip* clip, bool end = false)
{
    const auto& pos = clip->getPosition();
    return harness().edit().tempoSequence.toBeats(end ? pos.getEnd() : pos.getStart()).inBeats();
}

te::MidiClip* midiClip(te::EditItemID id)
{
    return dynamic_cast<te::MidiClip*>(te::findClipForID(harness().edit(), id));
}

// "pitch@start+length" of a clip's notes in beats, sorted.
QString clipNotes(te::MidiClip* mc)
{
    if (!mc) return QStringLiteral("<no clip>");
    QStringList parts;
    for (auto* n : mc->getSequence().getNotes())
        parts << QStringLiteral("%1@%2+%3").arg(n->getNoteNumber())
                     .arg(n->getStartBeat().inBeats(), 0, 'f', 3)
                     .arg(n->getLengthBeats().inBeats(), 0, 'f', 3);
    parts.sort();
    return parts.join(QLatin1Char(' '));
}

// "start+length:note:velocity" of pattern notes, in steps, in stored order.
QString patternNotes(const QVector<PatternNote>& notes)
{
    QStringList parts;
    for (const auto& n : notes)
        parts << QStringLiteral("%1+%2:%3:%4").arg(n.startStep).arg(n.lengthSteps).arg(n.note).arg(n.velocity);
    return parts.join(QLatin1Char(' '));
}

te::AudioTrack* newChannel(const QString& name)
{
    const QJsonObject t = harness().call("create_midi_track", QJsonObject{{"name", name}});
    if (harness().lastCallFailed()) return nullptr;
    return harness().trackById(TestHarness::findId(t, ids::kTrack));
}

te::MidiClip* placementClipOn(int placementId, te::AudioTrack* track)
{
    for (const auto& pl : pm().placements(0))
        if (pl.placementId == placementId)
            for (const auto& cid : pl.clipIds)
                if (auto* mc = midiClip(cid); mc && mc->getTrack() == track) return mc;
    return nullptr;
}

} // namespace

class TestPatternNotesFl : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

private slots:
    void initTestCase() { QVERIFY(dir_.isValid()); }
    void init() { harness().resetProject(); }

    // ── Notes round trip, save/reload, legacy data ──────────────────────────

    void notesRoundTripAndSaveReload()
    {
        auto* keys = newChannel("Keys");
        QVERIFY(keys);
        const int pid = pm().createPattern("Chords");
        QVERIFY(pid > 0);

        const QVector<PatternNote> notes = {
            { 0.0, 4.0, 60, 100 }, { 0.0, 4.0, 63, 90 }, { 2.5, 0.5, 70, 80 }, { 6.0, 2.0, -1, 110 },
            { 14.0, 8.0, 67, 100 }, // runs past the end: cut at step 16
        };
        QVERIFY(pm().setChannelNotes(pid, keys->itemID, notes));
        const auto got = pm().channelNotes(pid, keys->itemID);
        QCOMPARE(patternNotes(got), QStringLiteral("0+4:60:100 0+4:63:90 2.5+0.5:70:80 6+2:-1:110 14+2:67:100"));

        // The step view: steps 0, 6 and 14 are on (2.5 is off-grid).
        auto p = pm().pattern(pid);
        QVERIFY(p);
        const auto* ch = p->channel(keys->itemID);
        QVERIFY(ch);
        QCOMPARE(int(ch->steps.size()), 16);
        QList<int> on;
        for (int i = 0; i < ch->steps.size(); ++i)
            if (ch->steps[i].on) on << i;
        QCOMPARE(on, (QList<int>{0, 6, 14}));
        QCOMPARE(ch->steps[6].note, -1);
        QCOMPARE(ch->steps[6].velocity, 110);

        // Placement clips carry the real lengths and positions.
        const int placement = pm().placePattern(pid, 0.0);
        QVERIFY(placement > 0);
        auto* mc = placementClipOn(placement, keys);
        QVERIFY(mc);
        QCOMPARE(clipNotes(mc), QStringLiteral("60@0.000+1.000 60@1.500+0.500 63@0.000+1.000 67@3.500+0.500 "
                                               "70@0.625+0.125"));

        // Save / reload.
        const QString keysId = ids::make(ids::kTrack, keys->itemID);
        const QString path = QDir(dir_.path()).filePath("notes.tracktionedit");
        QVERIFY(em().saveEditAs(toJuce(path)));
        harness().resetProject();
        QVERIFY(em().loadEdit(toJuce(path)));
        TestHarness::pump(50);
        auto* reloaded = harness().trackById(keysId);
        QVERIFY(reloaded);
        QCOMPARE(patternNotes(pm().channelNotes(pid, reloaded->itemID)),
                 QStringLiteral("0+4:60:100 0+4:63:90 2.5+0.5:70:80 6+2:-1:110 14+2:67:100"));
        QCOMPARE(int(pm().placements(pid).size()), 1);
    }

    void legacyStepRowsAreReadAsNotes()
    {
        auto* kick = newChannel("Kick");
        QVERIFY(kick);
        // A project saved before notes existed: CHANNEL { track, steps }.
        auto& state = harness().edit().state;
        auto root = state.getChildWithName("OpenDaw_PATTERNS");
        if (!root.isValid()) {
            root = juce::ValueTree("OpenDaw_PATTERNS");
            state.appendChild(root, nullptr);
        }
        juce::ValueTree pat("PATTERN");
        pat.setProperty("id", 77, nullptr);
        pat.setProperty("name", "Old", nullptr);
        pat.setProperty("lengthSteps", 16, nullptr);
        pat.setProperty("stepsPerBeat", 4, nullptr);
        juce::ValueTree ch("CHANNEL");
        ch.setProperty("track", juce::String((juce::int64) kick->itemID.getRawID()), nullptr);
        ch.setProperty("steps", "100,.,.,.,127:62,.,~90,.,80,.,.,.,.,.,.,.", nullptr);
        pat.appendChild(ch, nullptr);
        root.appendChild(pat, nullptr);
        TestHarness::pump(20);

        QCOMPARE(patternNotes(pm().channelNotes(77, kick->itemID)),
                 QStringLiteral("0+1:-1:100 4+1:62:127 8+1:-1:80"));

        // Survives save/reload as it was (still step data until edited).
        const QString kickId = ids::make(ids::kTrack, kick->itemID);
        const QString path = QDir(dir_.path()).filePath("legacy.tracktionedit");
        QVERIFY(em().saveEditAs(toJuce(path)));
        harness().resetProject();
        QVERIFY(em().loadEdit(toJuce(path)));
        TestHarness::pump(50);
        auto* k2 = harness().trackById(kickId);
        QVERIFY(k2);
        QCOMPARE(patternNotes(pm().channelNotes(77, k2->itemID)),
                 QStringLiteral("0+1:-1:100 4+1:62:127 8+1:-1:80"));

        // The first edit rewrites the channel as NOTE children.
        QVERIFY(pm().setStep(77, k2->itemID, 12, true));
        QCOMPARE(patternNotes(pm().channelNotes(77, k2->itemID)),
                 QStringLiteral("0+1:-1:100 4+1:62:127 8+1:-1:80 12+1:-1:100"));
        auto node = harness().edit().state.getChildWithName("OpenDaw_PATTERNS");
        juce::ValueTree chan;
        for (auto p : node)
            if (int(p.getProperty("id")) == 77) chan = p.getChildWithName("CHANNEL");
        QVERIFY(chan.isValid());
        QVERIFY(!chan.hasProperty("steps"));
        QCOMPARE(chan.getNumChildren(), 4);
    }

    // ── Step edits keep every other note ────────────────────────────────────

    void stepEditsPreserveOffGridAndLongNotes()
    {
        auto* bass = newChannel("Bass");
        QVERIFY(bass);
        const int pid = pm().createPattern("Bass");
        QVERIFY(pm().setChannelNotes(pid, bass->itemID, {
            { 0.0, 4.0, -1, 100 },   // long, on step 0
            { 2.5, 0.5, 64, 90 },    // off-grid
            { 8.0, 1.0, -1, 100 },
        }));

        QVERIFY(pm().setStep(pid, bass->itemID, 4, true, 70));
        QCOMPARE(patternNotes(pm().channelNotes(pid, bass->itemID)),
                 QStringLiteral("0+4:-1:100 2.5+0.5:64:90 4+1:-1:70 8+1:-1:100"));

        // Changing the velocity of an on step keeps the note's length.
        QVERIFY(pm().setStep(pid, bass->itemID, 0, true, 50));
        QCOMPARE(patternNotes(pm().channelNotes(pid, bass->itemID)),
                 QStringLiteral("0+4:-1:50 2.5+0.5:64:90 4+1:-1:70 8+1:-1:100"));

        QVERIFY(pm().setStep(pid, bass->itemID, 0, false));
        QVERIFY(pm().toggleStep(pid, bass->itemID, 8));
        QCOMPARE(patternNotes(pm().channelNotes(pid, bass->itemID)),
                 QStringLiteral("2.5+0.5:64:90 4+1:-1:70"));

        // A whole row: step 4 stays (velocity follows the row), 12 is added,
        // the off-grid note stays.
        QVector<PatternStep> row(16);
        row[4] = { true, 100, -1 };
        row[12] = { true, 127, 62 };
        QVERIFY(pm().setChannelSteps(pid, bass->itemID, row));
        QCOMPARE(patternNotes(pm().channelNotes(pid, bass->itemID)),
                 QStringLiteral("2.5+0.5:64:90 4+1:-1:100 12+1:62:127"));

        // An all-off row keeps the off-grid note, so the channel stays.
        QVERIFY(pm().setChannelSteps(pid, bass->itemID, QVector<PatternStep>(16)));
        QCOMPARE(patternNotes(pm().channelNotes(pid, bass->itemID)), QStringLiteral("2.5+0.5:64:90"));
        QVERIFY(pm().pattern(pid)->channel(bass->itemID));

        QVERIFY(pm().clearChannel(pid, bass->itemID));
        QVERIFY(pm().channelNotes(pid, bass->itemID).isEmpty());
        QVERIFY(!pm().pattern(pid)->channel(bass->itemID));
    }

    void shorteningCutsNotes()
    {
        auto* lead = newChannel("Lead");
        const int pid = pm().createPattern("Lead");
        QVERIFY(pm().setChannelNotes(pid, lead->itemID, { { 2.0, 10.0, 72, 100 }, { 9.0, 1.0, 74, 100 } }));
        QVERIFY(pm().setPatternLength(pid, 8));
        QCOMPARE(patternNotes(pm().channelNotes(pid, lead->itemID)), QStringLiteral("2+6:72:100"));
    }

    // ── Tools ───────────────────────────────────────────────────────────────

    void setPatternNotesTool()
    {
        auto* keys = newChannel("Keys");
        QVERIFY(keys);
        const QString trackId = ids::make(ids::kTrack, keys->itemID);
        OD_CALL_OK(created, "create_pattern", (QJsonObject{{"name", "Melody"}}));
        const QString pattern = created.value("id").toString();

        OD_CALL_OK(set, "set_pattern_notes", (QJsonObject{
            {"pattern", pattern}, {"track", trackId},
            {"notes", QJsonArray{
                QJsonObject{{"start_step", 0}, {"length_steps", 4}, {"note", 60}},
                QJsonObject{{"start_step", 1.5}, {"length_steps", 0.5}, {"note", 67}, {"velocity", 90}},
                QJsonObject{{"start_step", 12}, {"length_steps", 8}}}}}));
        QCOMPARE(set.value("clipped_notes").toInt(), 1);
        const auto notes = set.value("channel").toObject().value("notes").toArray();
        QCOMPARE(int(notes.size()), 3);
        QCOMPARE(notes[1].toObject().value("start_step").toDouble(), 1.5);
        QCOMPARE(notes[2].toObject().value("length_steps").toDouble(), 4.0);
        QCOMPARE(notes[2].toObject().value("note").toInt(), 60);       // root note, resolved
        QVERIFY(notes[2].toObject().value("root").toBool());

        // Merge: same start + pitch is replaced, the rest is added.
        OD_CALL_OK(merged, "set_pattern_notes", (QJsonObject{
            {"pattern", pattern}, {"track", trackId}, {"replace", false},
            {"notes", QJsonArray{
                QJsonObject{{"start_step", 0}, {"length_steps", 2}, {"note", 60}, {"velocity", 50}},
                QJsonObject{{"start_step", 8}, {"length_steps", 1}, {"note", 64}}}}}));
        Q_UNUSED(merged);
        QCOMPARE(patternNotes(pm().channelNotes(*ids::parsePattern(pattern), keys->itemID)),
                 QStringLiteral("0+2:60:50 1.5+0.5:67:90 8+1:64:100 12+4:-1:100"));

        OD_CALL_OK(got, "get_pattern", (QJsonObject{{"pattern", pattern}}));
        const auto ch = got.value("channels").toArray().first().toObject();
        QCOMPARE(int(ch.value("notes").toArray().size()), 4);
        QVERIFY2(ch.contains("steps_string"), qPrintable(dump(got)));

        OD_CALL_ERR(outside, "set_pattern_notes", (QJsonObject{
            {"pattern", pattern}, {"track", trackId},
            {"notes", QJsonArray{QJsonObject{{"start_step", 16}, {"length_steps", 1}}}}}));
        QCOMPARE(TestHarness::errorCode(outside), AiErrorCode::InvalidArgument);
        OD_CALL_ERR(zeroLen, "set_pattern_notes", (QJsonObject{
            {"pattern", pattern}, {"track", trackId},
            {"notes", QJsonArray{QJsonObject{{"start_step", 1}, {"length_steps", 0}}}}}));
        Q_UNUSED(zeroLen);

        // One undo step per call.
        OD_CALL_OK(undone, "undo", QJsonObject{});
        TestHarness::pump(20);
        QCOMPARE(patternNotes(pm().channelNotes(*ids::parsePattern(pattern), keys->itemID)),
                 QStringLiteral("0+4:60:100 1.5+0.5:67:90 12+4:-1:100"));
    }

    // ── Write-back of hand edits ────────────────────────────────────────────

    void handEditWritesBackAndUpdatesOtherPlacements()
    {
        auto* kick = newChannel("Kick");
        QVERIFY(kick);
        const int pid = pm().createPattern("Beat");
        QVERIFY(pm().setChannelSteps(pid, kick->itemID, [] {
            QVector<PatternStep> r(16);
            for (int i : {0, 4, 8, 12}) r[i].on = true;
            return r;
        }()));
        const int a = pm().placePattern(pid, 0.0);
        const int b = pm().placePattern(pid, 8.0);
        QVERIFY(a > 0 && b > 0);
        TestHarness::pump(20);
        auto* clipA = placementClipOn(a, kick);
        auto* clipB = placementClipOn(b, kick);
        QVERIFY(clipA && clipB);
        const auto idA = clipA->itemID, idB = clipB->itemID;
        const QString before = clipNotes(clipB);
        QCOMPARE(before, QStringLiteral("60@0.000+0.250 60@1.000+0.250 60@2.000+0.250 60@3.000+0.250"));

        // A piano-roll edit of placement A: an off-grid G and a longer first note.
        auto& um = harness().edit().getUndoManager();
        um.beginNewTransaction("Hand edit");
        clipA->getSequence().addNote(67, tracktion::BeatPosition::fromBeats(1.375),
                                     tracktion::BeatDuration::fromBeats(0.5), 90, 0, &um);
        auto* first = clipA->getSequence().getNotes().getFirst();
        for (auto* n : clipA->getSequence().getNotes())
            if (n->getStartBeat().inBeats() < 1.0e-6) first = n;
        first->setStartAndLength(tracktion::BeatPosition(), tracktion::BeatDuration::fromBeats(1.0), &um);
        TestHarness::pump(30);

        // The pattern now holds the clip's notes...
        QCOMPARE(patternNotes(pm().channelNotes(pid, kick->itemID)),
                 QStringLiteral("0+4:-1:100 4+1:-1:100 5.5+2:67:90 8+1:-1:100 12+1:-1:100"));
        // ...the other placement was regenerated with them, the edited clip is
        // untouched and still a pattern clip.
        clipA = midiClip(idA);
        clipB = midiClip(idB);
        QVERIFY2(clipA && clipB, "a placement clip was replaced");
        QVERIFY(clipA->state.hasProperty(kPlacementIdProp));
        QCOMPARE(pm().patternOfClip(clipA), pid);
        QCOMPARE(clipNotes(clipB), clipNotes(clipA));
        QCOMPARE(clipNotes(clipB),
                 QStringLiteral("60@0.000+1.000 60@1.000+0.250 60@2.000+0.250 60@3.000+0.250 67@1.375+0.500"));
        QCOMPARE(int(pm().placements(pid).size()), 2);

        // No loop: nothing changes any more.
        const int undoCount = um.getUndoDescriptions().size();
        TestHarness::pump(30);
        QCOMPARE(um.getUndoDescriptions().size(), undoCount);
        QCOMPARE(um.getUndoDescriptions()[0], juce::String("Hand edit")); // most recent first

        // One undo restores the clip, the pattern and the other placement.
        QVERIFY(um.undo());
        TestHarness::pump(30);
        QCOMPARE(patternNotes(pm().channelNotes(pid, kick->itemID)),
                 QStringLiteral("0+1:-1:100 4+1:-1:100 8+1:-1:100 12+1:-1:100"));
        QCOMPARE(clipNotes(midiClip(idA)), before);
        QCOMPARE(clipNotes(midiClip(idB)), before);
        QVERIFY(um.canRedo());

        // Redo brings it all back.
        QVERIFY(um.redo());
        TestHarness::pump(30);
        QCOMPARE(pm().channelNotes(pid, kick->itemID).size(), 5);
        QCOMPARE(clipNotes(midiClip(idB)), clipNotes(midiClip(idA)));

        // A step click after the hand edit keeps the hand-made notes.
        QVERIFY(pm().setStep(pid, kick->itemID, 14, true));
        TestHarness::pump(20);
        QCOMPARE(midiClip(idA)->getSequence().getNotes().size(), 6);
        QCOMPARE(midiClip(idB)->getSequence().getNotes().size(), 6);
    }

    void notesPastThePatternEndAreClipped()
    {
        auto* kick = newChannel("Kick");
        const int pid = pm().createPattern("Short");
        QVERIFY(pm().setStep(pid, kick->itemID, 0, true));
        const int a = pm().placePattern(pid, 0.0);
        const int b = pm().placePattern(pid, 4.0);
        TestHarness::pump(20);
        auto* clipA = placementClipOn(a, kick);
        QVERIFY(clipA);
        auto& um = harness().edit().getUndoManager();
        um.beginNewTransaction("Hand edit");
        clipA->getSequence().addNote(62, tracktion::BeatPosition::fromBeats(3.5),
                                     tracktion::BeatDuration::fromBeats(2.0), 100, 0, &um); // past the end
        clipA->getSequence().addNote(64, tracktion::BeatPosition::fromBeats(5.0),
                                     tracktion::BeatDuration::fromBeats(1.0), 100, 0, &um); // outside
        TestHarness::pump(30);
        QCOMPARE(patternNotes(pm().channelNotes(pid, kick->itemID)), QStringLiteral("0+1:-1:100 14+2:62:100"));
        QCOMPARE(clipNotes(placementClipOn(b, kick)), QStringLiteral("60@0.000+0.250 62@3.500+0.500"));
        // The edited clip keeps what the user drew.
        QCOMPARE(midiClip(clipA->itemID)->getSequence().getNotes().size(), 3);
    }

    void clipMovedToAnotherTrackLeavesThePattern()
    {
        auto* kick = newChannel("Kick");
        auto* other = newChannel("Other");
        const int pid = pm().createPattern("Beat");
        QVERIFY(pm().setStep(pid, kick->itemID, 0, true));
        const int a = pm().placePattern(pid, 0.0);
        TestHarness::pump(20);
        auto* clip = placementClipOn(a, kick);
        QVERIFY(clip);
        const auto id = clip->itemID;
        harness().edit().getUndoManager().beginNewTransaction("Move");
        QVERIFY(clip->moveTo(*other));
        em().notifyEdited();
        TestHarness::pump(30);
        auto* moved = midiClip(id);
        QVERIFY(moved);
        QCOMPARE(moved->getTrack(), static_cast<te::Track*>(other));
        QVERIFY2(!moved->state.hasProperty(kPlacementIdProp), "a clip moved to another track is still a pattern clip");
        QCOMPARE(pm().patternOfClip(moved), 0);
        // The pattern is unchanged.
        QCOMPARE(patternNotes(pm().channelNotes(pid, kick->itemID)), QStringLiteral("0+1:-1:100"));
    }

    // ── editableClipFor / patternOfClip ─────────────────────────────────────

    void editableClipForCreatesPlacementAtBarOne()
    {
        auto* keys = newChannel("Keys");
        QVERIFY(keys);
        const int pid = pm().createPattern("Melody");
        auto& um = harness().edit().getUndoManager();
        const int undoBefore = um.getUndoDescriptions().size();

        auto* mc = pm().editableClipFor(pid, keys->itemID);
        QVERIFY(mc);
        QCOMPARE(mc->getTrack(), static_cast<te::Track*>(keys));
        QVERIFY(std::abs(beatOf(mc)) < 1e-6);
        QVERIFY(std::abs(beatOf(mc, true) - 4.0) < 1e-6);
        QCOMPARE(pm().patternOfClip(mc), pid);
        QCOMPARE(int(pm().placements(pid).size()), 1);
        QCOMPARE(um.getUndoDescriptions().size(), undoBefore + 1);
        QCOMPARE(um.getUndoDescriptions()[0], juce::String("Edit Pattern in Piano Roll")); // most recent first

        // The same clip again, nothing new created.
        const auto id = mc->itemID;
        QCOMPARE(pm().editableClipFor(pid, keys->itemID), mc);
        QCOMPARE(um.getUndoDescriptions().size(), undoBefore + 1);

        // The empty clip survives an unrelated edit of the pattern.
        auto* hat = newChannel("Hat");
        QVERIFY(pm().setStep(pid, hat->itemID, 2, true));
        TestHarness::pump(20);
        QCOMPARE(midiClip(id), mc);

        // Notes drawn into it land in the pattern.
        um.beginNewTransaction("Draw");
        mc->getSequence().addNote(64, tracktion::BeatPosition::fromBeats(1.0),
                                  tracktion::BeatDuration::fromBeats(1.0), 100, 0, &um);
        TestHarness::pump(30);
        QCOMPARE(patternNotes(pm().channelNotes(pid, keys->itemID)), QStringLiteral("4+4:64:100"));

        // An ordinary clip belongs to no pattern.
        auto ordinary = keys->insertMIDIClip("plain", tracktion::TimeRange(tracktion::TimePosition::fromSeconds(20.0),
                                                                           tracktion::TimePosition::fromSeconds(21.0)),
                                             nullptr);
        QVERIFY(ordinary);
        QCOMPARE(pm().patternOfClip(ordinary.get()), 0);
        QCOMPARE(pm().patternOfClip(nullptr), 0);

        // Not a channel: refused.
        QVERIFY(!pm().editableClipFor(pid, te::EditItemID::fromRawID(987654321)));
    }

    void editableClipForAppendsAfterTheSong()
    {
        auto* kick = newChannel("Kick");
        auto* keys = newChannel("Keys");
        const int beat = pm().createPattern("Beat");
        QVERIFY(pm().setStep(beat, kick->itemID, 0, true));
        QVERIFY(pm().placePattern(beat, 0.0) > 0);
        QVERIFY(pm().placePattern(beat, 2.0) > 0); // song ends at beat 6, inside bar 2

        const int melody = pm().createPattern("Melody");
        QVERIFY(pm().setStep(melody, kick->itemID, 4, true));
        auto* mc = pm().editableClipFor(melody, keys->itemID);
        QVERIFY(mc);
        QCOMPARE(mc->getTrack(), static_cast<te::Track*>(keys));
        QVERIFY2(std::abs(beatOf(mc) - 8.0) < 1e-6, qPrintable(QString::number(beatOf(mc))));
        // The placement also holds the pattern's other channels.
        const auto pls = pm().placements(melody);
        QCOMPARE(int(pls.size()), 1);
        QCOMPARE(int(pls.first().clipIds.size()), 2);
        QVERIFY(std::abs(pls.first().startBeat - 8.0) < 1e-6);

        // Undo removes the whole placement in one step.
        QVERIFY(harness().edit().getUndoManager().undo());
        TestHarness::pump(20);
        QVERIFY(pm().placements(melody).isEmpty());
    }

    // ── Pattern player schedule ─────────────────────────────────────────────

    void playerSchedulesNoteLengths()
    {
        auto* lead = newChannel("Lead");
        auto* drum = newChannel("Drum");
        pm().setChannelRootNote(drum->itemID, 36);
        const int pid = pm().createPattern("Play");
        QVERIFY(pm().setChannelNotes(pid, lead->itemID, {
            { 0.0, 2.0, 72, 100 }, { 3.5, 1.0, 64, 80 }, { 15.0, 4.0, 67, 100 } }));
        QVERIFY(pm().setStep(pid, drum->itemID, 4, true));

        auto p = pm().pattern(pid);
        QVERIFY(p);
        const auto events = PatternPlayer::scheduleNotes(*p, pm());
        QCOMPARE(int(events.size()), 4);
        QCOMPARE(events[0].note, 72);
        QCOMPARE(events[0].startStep, 0.0);
        QCOMPARE(events[0].endStep, 2.0);
        QCOMPARE(events[1].note, 64);
        QCOMPARE(events[1].startStep, 3.5);      // off-grid, not quantized
        QCOMPARE(events[1].endStep, 4.5);
        QCOMPARE(events[1].velocity, 80);
        QCOMPARE(events[2].note, 36);            // root note resolved
        QCOMPARE(events[2].trackId, drum->itemID);
        QCOMPARE(events[2].startStep, 4.0);
        QCOMPARE(events[2].endStep, 5.0);
        QCOMPARE(events[3].startStep, 15.0);
        QCOMPARE(events[3].endStep, 16.0);       // clamped to the pattern end

        // The player runs with notes (no audio device: just no crash, steps advance).
        PatternPlayer player(pm(), em());
        int lastStep = -2;
        QObject::connect(&player, &PatternPlayer::stepChanged, [&lastStep](int s) { lastStep = s; });
        player.play(pid);
        QVERIFY(player.isPlaying());
        TestHarness::pump(150);
        QVERIFY(lastStep >= 0);
        player.stop();
        QCOMPARE(lastStep, -1);
    }
};

OD_REGISTER_TEST(TestPatternNotesFl)
#include "tst_PatternNotesFl.moc"
