// Pattern sequencing through the pattern tools: steps, linked placements on
// the playlist, regeneration, undo, deleting a placement clip, save/reload.

#include "TestHarness.h"

#include "ai/AiIds.h"
#include "ai/AiToolExtensions.h"
#include "engine/EditManager.h"
#include "engine/PatternManager.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

using namespace OpenDaw;
using OpenDaw::Test::harness;
using OpenDaw::Test::TestHarness;

namespace {

QString dump(const QJsonObject& o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)).left(1500);
}

// steps_string without the cosmetic separators the tools may add.
QString bareSteps(QString s)
{
    s.remove(QLatin1Char('|'));
    s.remove(QLatin1Char(' '));
    return s;
}

double clipStartBeat(te::Clip* clip)
{
    return harness().edit().tempoSequence.toBeats(clip->getPosition().getStart()).inBeats();
}

PatternManager& pm() { return harness().patternManager(); }

// "<startBeat>:<notes> ..." for every placement of a pattern, in start
// order, counting the notes of all its MIDI clips. E.g. "0:4 8:4".
QString notesPerPlacement(int patternId)
{
    QStringList out;
    for (const auto& pl : pm().placements(patternId)) {
        int notes = 0;
        for (const auto& cid : pl.clipIds)
            if (auto* mc = dynamic_cast<te::MidiClip*>(te::findClipForID(harness().edit(), cid)))
                notes += mc->getSequence().getNotes().size();
        out << QStringLiteral("%1:%2").arg(pl.startBeat).arg(notes);
    }
    return out.join(QLatin1Char(' '));
}

} // namespace

class TestPatterns : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;
    QString kickId_;

    // A project with one MIDI channel ("Kick") and a 16-step pattern "Beat"
    // with four on-steps. Returns the pattern's numeric id (0 on failure).
    int setUpBeat()
    {
        const QJsonObject t = harness().call("create_midi_track", QJsonObject{{"name", "Kick"}});
        if (harness().lastCallFailed()) return 0;
        kickId_ = TestHarness::findId(t, ids::kTrack);

        const QJsonObject p = harness().call("create_pattern", QJsonObject{{"name", "Beat"}});
        if (harness().lastCallFailed()) return 0;
        const auto pid = ids::parsePattern(p.value("id").toString());
        if (!pid) return 0;

        harness().call("set_pattern_steps", QJsonObject{
            {"pattern", ids::makePattern(*pid)}, {"track", kickId_}, {"steps_string", "x...x...x...x..."}});
        if (harness().lastCallFailed()) return 0;
        return *pid;
    }

private slots:
    void initTestCase() { QVERIFY(dir_.isValid()); }
    void init() { harness().resetProject(); }

    void createAndSetSteps()
    {
        const int pid = setUpBeat();
        QVERIFY2(pid > 0, qPrintable(harness().lastCallText()));
        QVERIFY(!kickId_.isEmpty());

        OD_CALL_OK(got, "get_pattern", (QJsonObject{{"pattern", ids::makePattern(pid)}}));
        QCOMPARE(got.value("id").toString(), ids::makePattern(pid));
        QCOMPARE(got.value("name").toString(), QStringLiteral("Beat"));
        QCOMPARE(got.value("length_steps").toInt(), 16);
        const QJsonArray channels = got.value("channels").toArray();
        QCOMPARE(int(channels.size()), 1);
        const QJsonObject ch = channels.first().toObject();
        QCOMPARE(ch.value("track_id").toString(), kickId_);
        QCOMPARE(bareSteps(ch.value("steps_string").toString()), QStringLiteral("x...x...x...x..."));
        QList<int> on;
        for (const auto& s : ch.value("steps").toArray())
            on << s.toObject().value("step").toInt();
        QCOMPARE(on, (QList<int>{0, 4, 8, 12}));

        // The model agrees.
        const auto p = pm().pattern(pid);
        QVERIFY(p);
        const auto* row = p->channel(*ids::parse(kickId_, ids::kTrack));
        QVERIFY(row);
        QCOMPARE(int(row->steps.size()), 16);
        QVERIFY(row->steps[0].on && row->steps[4].on && !row->steps[1].on);

        // Lookup by name and by number work too.
        OD_CALL_OK(byName, "get_pattern", (QJsonObject{{"pattern", "Beat"}}));
        QCOMPARE(byName.value("id").toString(), ids::makePattern(pid));
        OD_CALL_OK(byNumber, "get_pattern", (QJsonObject{{"pattern", pid}}));
        QCOMPARE(byNumber.value("id").toString(), ids::makePattern(pid));
    }

    void badInput()
    {
        const int pid = setUpBeat();
        QVERIFY(pid > 0);

        OD_CALL_ERR(missing, "get_pattern", (QJsonObject{{"pattern", "pattern:9999"}}));
        QCOMPARE(TestHarness::errorCode(missing), AiErrorCode::NotFound);

        OD_CALL_ERR(tooShort, "set_pattern_steps", (QJsonObject{
            {"pattern", ids::makePattern(pid)}, {"track", kickId_}, {"steps_string", "x...x..."}}));
        QCOMPARE(TestHarness::errorCode(tooShort), AiErrorCode::InvalidArgument);

        // A default (audio) track is not a channel.
        const QString audioId = ids::of(harness().editManager().getAudioTrack(0));
        OD_CALL_ERR(notChannel, "set_pattern_steps", (QJsonObject{
            {"pattern", ids::makePattern(pid)}, {"track", audioId}, {"steps_string", "x...x...x...x..."}}));
        QCOMPARE(TestHarness::errorCode(notChannel), AiErrorCode::InvalidArgument);

        // The failed calls changed nothing.
        QCOMPARE(bareSteps(harness().call("get_pattern", QJsonObject{{"pattern", pid}})
                               .value("channels").toArray().first().toObject().value("steps_string").toString()),
                 QStringLiteral("x...x...x...x..."));
    }

    void placementsAreLinkedAndUndoable()
    {
        const int pid = setUpBeat();
        QVERIFY(pid > 0);
        const QString pattern = ids::makePattern(pid);
        auto* kick = harness().trackById(kickId_);
        QVERIFY(kick);

        OD_CALL_OK(first, "place_pattern", (QJsonObject{{"pattern", pattern}, {"start_bar", 1}}));
        OD_CALL_OK(second, "place_pattern", (QJsonObject{{"pattern", pattern}, {"start_bar", 3}}));
        QVERIFY(std::abs(first.value("start_beat").toDouble() - 0.0) < 1e-6);
        QVERIFY2(std::abs(second.value("start_beat").toDouble() - 8.0) < 1e-6, qPrintable(dump(second)));
        QVERIFY(first.value("placement_id").toInt() != second.value("placement_id").toInt());

        // One MIDI clip per placement on the channel's own track, 4 notes each.
        for (const QJsonObject& placed : {first, second}) {
            const QJsonArray clipIds = placed.value("clip_ids").toArray();
            QCOMPARE(int(clipIds.size()), 1);
            auto* clip = dynamic_cast<te::MidiClip*>(harness().clipById(clipIds.first().toString()));
            QVERIFY2(clip, qPrintable(dump(placed)));
            QCOMPARE(clip->getTrack(), static_cast<te::Track*>(kick));
            QCOMPARE(clip->getSequence().getNotes().size(), 4);
            QVERIFY(std::abs(clipStartBeat(clip) - placed.value("start_beat").toDouble()) < 1e-6);
        }
        QCOMPARE(int(pm().placements(pid).size()), 2);
        QCOMPARE(notesPerPlacement(pid), QStringLiteral("0:4 8:4"));

        // Editing the pattern regenerates both placements.
        OD_CALL_OK(edited, "set_pattern_steps", (QJsonObject{
            {"pattern", pattern}, {"track", kickId_}, {"steps_string", "x.x.x.x.x.x.x.x."}}));
        QTRY_COMPARE_WITH_TIMEOUT(notesPerPlacement(pid), QStringLiteral("0:8 8:8"), 3000);
        QCOMPARE(edited.value("placements_updated").toInt(), 2);

        // One undo step restores the previous steps in the pattern and both clips.
        OD_CALL_OK(undone, "undo", QJsonObject{});
        QTRY_COMPARE_WITH_TIMEOUT(notesPerPlacement(pid), QStringLiteral("0:4 8:4"), 3000);
        QCOMPARE(bareSteps(harness().call("get_pattern", QJsonObject{{"pattern", pattern}})
                               .value("channels").toArray().first().toObject().value("steps_string").toString()),
                 QStringLiteral("x...x...x...x..."));

        OD_CALL_OK(redone, "redo", QJsonObject{});
        QTRY_COMPARE_WITH_TIMEOUT(notesPerPlacement(pid), QStringLiteral("0:8 8:8"), 3000);
    }

    void deletingClipRemovesPlacement()
    {
        const int pid = setUpBeat();
        QVERIFY(pid > 0);
        const QString pattern = ids::makePattern(pid);
        OD_CALL_OK(a, "place_pattern", (QJsonObject{{"pattern", pattern}, {"start_bar", 1}}));
        OD_CALL_OK(b, "place_pattern", (QJsonObject{{"pattern", pattern}, {"start_bar", 3}}));
        QCOMPARE(int(pm().placements(pid).size()), 2);

        // Delete the second placement's clip the way the timeline does.
        const auto placements = pm().placements(pid);
        const auto& victim = placements.last();
        QVERIFY(std::abs(victim.startBeat - 8.0) < 1e-6);
        QCOMPARE(int(victim.clipIds.size()), 1);
        auto* clip = te::findClipForID(harness().edit(), victim.clipIds.first());
        QVERIFY(clip);
        harness().editManager().removeClip(clip);
        TestHarness::pump(20);

        QTRY_COMPARE_WITH_TIMEOUT(int(pm().placements(pid).size()), 1, 3000);
        QVERIFY(std::abs(pm().placements(pid).first().startBeat) < 1e-6);

        // Editing the pattern regenerates only the remaining placement; the
        // deleted one does not come back.
        OD_CALL_OK(edited, "set_pattern_steps", (QJsonObject{
            {"pattern", pattern}, {"track", kickId_}, {"steps_string", "xxxx............"}}));
        QTRY_COMPARE_WITH_TIMEOUT(notesPerPlacement(pid), QStringLiteral("0:4"), 3000);
        int clipsOnKick = 0;
        for (auto* c : harness().trackById(kickId_)->getClips())
            if (dynamic_cast<te::MidiClip*>(c)) ++clipsOnKick;
        QCOMPARE(clipsOnKick, 1);
    }

    void saveAndReloadKeepsPatterns()
    {
        const int pid = setUpBeat();
        QVERIFY(pid > 0);
        const QString pattern = ids::makePattern(pid);
        OD_CALL_OK(a, "place_pattern", (QJsonObject{{"pattern", pattern}, {"start_bar", 1}}));
        OD_CALL_OK(b, "place_pattern", (QJsonObject{{"pattern", pattern}, {"start_bar", 3}}));
        OD_CALL_OK(second, "create_pattern", (QJsonObject{{"name", "Fill"}, {"length_steps", 32}}));
        const auto kickItem = *ids::parse(kickId_, ids::kTrack);

        const juce::File file(juce::String(QDir::toNativeSeparators(dir_.filePath("patterns.tracktionedit"))
                                               .toUtf8().constData()));
        QVERIFY(harness().editManager().saveEditAs(file));
        QVERIFY(file.existsAsFile());

        // Start over, then load the saved project.
        harness().resetProject();
        QVERIFY(pm().patterns().isEmpty());
        QVERIFY(harness().editManager().loadEdit(file));
        TestHarness::pump(50);

        const auto patterns = pm().patterns();
        QCOMPARE(int(patterns.size()), 2);
        const auto beat = pm().pattern(pid);
        QVERIFY(beat);
        QCOMPARE(beat->name, QStringLiteral("Beat"));
        QCOMPARE(beat->lengthSteps, 16);
        const auto* row = beat->channel(kickItem);
        QVERIFY2(row, "the Kick channel row was not restored (track id changed?)");
        QList<int> on;
        for (int i = 0; i < row->steps.size(); ++i)
            if (row->steps[i].on) on << i;
        QCOMPARE(on, (QList<int>{0, 4, 8, 12}));

        bool haveFill = false;
        for (const auto& p : patterns)
            if (p.name == QLatin1String("Fill")) haveFill = p.lengthSteps == 32;
        QVERIFY(haveFill);

        // The placements (tagged clips) came back and are still linked.
        QCOMPARE(notesPerPlacement(pid), QStringLiteral("0:4 8:4"));
        OD_CALL_OK(edited, "set_pattern_steps", (QJsonObject{
            {"pattern", pattern}, {"track", kickId_}, {"steps_string", "x.x.x.x.x.x.x.x."}}));
        QTRY_COMPARE_WITH_TIMEOUT(notesPerPlacement(pid), QStringLiteral("0:8 8:8"), 3000);

        // New patterns after a reload never reuse an id.
        OD_CALL_OK(third, "create_pattern", (QJsonObject{{"name", "After"}}));
        const auto thirdId = ids::parsePattern(third.value("id").toString());
        QVERIFY(thirdId);
        for (const auto& p : patterns)
            QVERIFY(*thirdId != p.id);
    }
};

OD_REGISTER_TEST(TestPatterns)
#include "tst_Patterns.moc"
