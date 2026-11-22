// Sample channels (sampler-based channel rack rows), the sample library
// (find_samples / list_sample_folders), and pattern fixes: hand-edited
// pattern clips are untagged instead of overwritten, a placement start
// survives save/reload, and the pattern player never plays during a render.

#include "TestHarness.h"

#include "ai/AiIds.h"
#include "ai/AiToolExtensions.h"
#include "engine/EditManager.h"
#include "engine/PatternManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <functional>

using namespace OpenDaw;
using OpenDaw::Test::harness;
using OpenDaw::Test::TestHarness;

namespace {

constexpr double kSampleRate = 44100.0;
constexpr double kPi = 3.14159265358979323846;

const juce::Identifier kPlacementIdProp("OpenDaw_placementId");

QString dump(const QJsonObject& o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)).left(1500);
}

juce::File toJuce(const QString& path)
{
    return juce::File(juce::String::fromUTF8(QDir::toNativeSeparators(path).toUtf8().constData()));
}

// A short "drum hit": a decaying 120 Hz sine, mono 16-bit.
bool writeHit(const QString& path, double seconds = 0.4)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    const auto file = toJuce(path);
    const int n = int(seconds * kSampleRate);
    juce::AudioBuffer<float> buffer(1, n);
    for (int i = 0; i < n; ++i) {
        const double t = double(i) / kSampleRate;
        buffer.setSample(0, i, float(0.8 * std::exp(-t * 8.0) * std::sin(2.0 * kPi * 120.0 * t)));
    }
    file.deleteFile();
    juce::WavAudioFormat wav;
    auto stream = std::unique_ptr<juce::OutputStream>(file.createOutputStream());
    if (!stream) return false;
    const auto opts = juce::AudioFormatWriterOptions{}
                          .withSampleRate(kSampleRate)
                          .withNumChannels(1)
                          .withBitsPerSample(16);
    auto writer = wav.createWriterFor(stream, opts);
    if (!writer) return false;
    return writer->writeFromAudioSampleBuffer(buffer, 0, n);
}

PatternManager& pm() { return harness().patternManager(); }
EditManager& em() { return harness().editManager(); }

QString samePathForm(const QString& p) { return QFileInfo(p).canonicalFilePath().toLower(); }

double clipStartBeat(te::Clip* clip)
{
    return harness().edit().tempoSequence.toBeats(clip->getPosition().getStart()).inBeats();
}

te::MidiClip* midiClip(te::EditItemID id)
{
    return dynamic_cast<te::MidiClip*>(te::findClipForID(harness().edit(), id));
}

// The sound source stored in a sampler's state.
QString samplerSource(te::SamplerPlugin* sp)
{
    if (!sp) return {};
    const auto snd = sp->state.getChildWithName(te::IDs::SOUND);
    return QString::fromUtf8(snd.getProperty(te::IDs::source).toString().toRawUTF8());
}

int patternIdOf(const QJsonObject& created)
{
    const auto pid = ids::parsePattern(created.value("id").toString());
    return pid ? *pid : 0;
}

// Restores QSettings "samples/roots" when a test ends.
struct RootsSetting {
    QVariant old;
    bool had = false;
    explicit RootsSetting(const QStringList& roots)
    {
        QSettings s;
        had = s.contains("samples/roots");
        old = s.value("samples/roots");
        s.setValue("samples/roots", roots);
        s.sync();
        SampleLibrary::invalidateCache();
    }
    ~RootsSetting()
    {
        QSettings s;
        if (had) s.setValue("samples/roots", old);
        else s.remove("samples/roots");
        s.sync();
        SampleLibrary::invalidateCache();
    }
};

} // namespace

class TestPatternFixes : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

private slots:
    void initTestCase() { QVERIFY(dir_.isValid()); }
    void init() { harness().resetProject(); }

    // ── Sample channels ─────────────────────────────────────────────────────

    void sampleChannelFromWav()
    {
        const QString wavPath = QDir(dir_.path()).filePath("Kick Punchy.wav");
        QVERIFY(writeHit(wavPath));
        QVERIFY(PatternManager::isAudioSampleFile(toJuce(wavPath)));
        QVERIFY(!PatternManager::isAudioSampleFile(toJuce(QDir(dir_.path()).filePath("missing.wav"))));

        const int tracksBefore = em().trackCount();
        OD_CALL_OK(added, "add_sample_channel", (QJsonObject{{"path", wavPath}}));
        const QString trackId = added.value("track_id").toString();
        QVERIFY2(!trackId.isEmpty(), qPrintable(dump(added)));
        QCOMPARE(added.value("name").toString(), QStringLiteral("Kick Punchy"));
        QCOMPARE(samePathForm(added.value("sample").toString()), samePathForm(wavPath));
        QCOMPARE(added.value("root_note").toInt(), 60);
        QCOMPARE(em().trackCount(), tracksBefore + 1);

        auto* track = harness().trackById(trackId);
        QVERIFY(track);
        QVERIFY(em().isMidiTrack(track));
        auto* sp = pm().channelSampler(track->itemID);
        QVERIFY2(sp, "the instrument is not a SamplerPlugin");
        QVERIFY(sp->isSynth());
        QCOMPARE(sp->getNumSounds(), 1);
        QCOMPARE(samePathForm(samplerSource(sp)), samePathForm(wavPath));
        QCOMPARE(sp->getKeyNote(0), 60);
        QCOMPARE(sp->getMinKey(0), 0);
        QCOMPARE(sp->getMaxKey(0), 127);
        QVERIFY(sp->isSoundOpenEnded(0));
        QVERIFY(pm().channels().contains(track));

        // list_channels shows the sample.
        OD_CALL_OK(chans, "list_channels", QJsonObject{});
        bool listed = false;
        for (const auto& c : chans.value("channels").toArray())
            if (c.toObject().value("track_id").toString() == trackId)
                listed = samePathForm(c.toObject().value("sample").toString()) == samePathForm(wavPath);
        QVERIFY2(listed, qPrintable(dump(chans)));

        // Swap the sample: the plugin stays, the name follows the file.
        const QString snarePath = QDir(dir_.path()).filePath("Snare Tight.wav");
        QVERIFY(writeHit(snarePath, 0.2));
        OD_CALL_OK(swapped, "set_channel_sample", (QJsonObject{{"track", trackId}, {"path", snarePath}}));
        QCOMPARE(pm().channelSampler(track->itemID), sp);
        QCOMPARE(sp->getNumSounds(), 1);
        QCOMPARE(samePathForm(samplerSource(sp)), samePathForm(snarePath));
        QCOMPARE(swapped.value("name").toString(), QStringLiteral("Snare Tight"));

        // Survives save/reload as a sampler-driven MIDI channel.
        const QString projPath = QDir(dir_.path()).filePath("samples.tracktionedit");
        QVERIFY(em().saveEditAs(toJuce(projPath)));
        harness().resetProject();
        QVERIFY(!harness().trackById(trackId));
        QVERIFY(em().loadEdit(toJuce(projPath)));
        TestHarness::pump(100);

        auto* reloaded = harness().trackById(trackId);
        QVERIFY2(reloaded, "the sample channel's track id changed on reload");
        QVERIFY(em().isMidiTrack(reloaded));
        QVERIFY(pm().channels().contains(reloaded));
        auto* sp2 = pm().channelSampler(reloaded->itemID);
        QVERIFY(sp2);
        QCOMPARE(sp2->getNumSounds(), 1);
        QCOMPARE(samePathForm(samplerSource(sp2)), samePathForm(snarePath));
        QCOMPARE(samePathForm(pm().channelSamplePath(reloaded->itemID)), samePathForm(snarePath));
        QVERIFY(sp2->isSoundOpenEnded(0));
    }

    void addSampleChannelIsOneUndoStep()
    {
        const QString wavPath = QDir(dir_.path()).filePath("Clap.wav");
        QVERIFY(writeHit(wavPath, 0.15));
        const int before = em().trackCount();
        OD_CALL_OK(added, "add_sample_channel", (QJsonObject{{"path", wavPath}, {"name", "My Clap"}}));
        QCOMPARE(added.value("name").toString(), QStringLiteral("My Clap"));
        QCOMPARE(em().trackCount(), before + 1);
        OD_CALL_OK(undone, "undo", QJsonObject{});
        TestHarness::pump(20);
        QCOMPARE(em().trackCount(), before);

        OD_CALL_ERR(bad, "add_sample_channel",
                    (QJsonObject{{"path", QDir(dir_.path()).filePath("nope.wav")}}));
        QCOMPARE(TestHarness::errorCode(bad), AiErrorCode::NotFound);
        QCOMPARE(em().trackCount(), before);
    }

    void sampleChannelRendersAudio()
    {
        const QString wavPath = QDir(dir_.path()).filePath("Render Kick.wav");
        QVERIFY(writeHit(wavPath));
        OD_CALL_OK(added, "add_sample_channel", (QJsonObject{{"path", wavPath}}));
        const QString trackId = added.value("track_id").toString();

        OD_CALL_OK(created, "create_pattern", (QJsonObject{{"name", "Kicks"}}));
        const int pid = patternIdOf(created);
        QVERIFY(pid > 0);
        OD_CALL_OK(steps, "set_pattern_steps", (QJsonObject{
            {"pattern", ids::makePattern(pid)}, {"track", trackId}, {"steps_string", "x...x...x...x..."}}));
        QCOMPARE(samePathForm(steps.value("channel").toObject().value("sample").toString()), samePathForm(wavPath));
        OD_CALL_OK(placed, "place_pattern", (QJsonObject{{"pattern", ids::makePattern(pid)}, {"start_bar", 1}}));
        TestHarness::pump(50);

        OD_CALL_OK(pat, "get_pattern", (QJsonObject{{"pattern", ids::makePattern(pid)}}));
        const auto chans = pat.value("channels").toArray();
        QCOMPARE(int(chans.size()), 1);
        QCOMPARE(samePathForm(chans.first().toObject().value("sample").toString()), samePathForm(wavPath));

        const QString outPath = QDir(dir_.path()).filePath("sample-render.wav");
        OD_CALL_OK(r, "render_region", (QJsonObject{
            {"start_beat", 0}, {"end_beat", 4}, {"tracks", QJsonArray{trackId}},
            {"output_path", outPath}, {"tail_seconds", 0.5}}));
        const QJsonObject a = r.value("analysis").toObject();
        QVERIFY2(a.contains("peak_dbfs"), qPrintable(dump(r)));
        QVERIFY2(a.value("peak_dbfs").toDouble() > -60.0, qPrintable(dump(a)));
    }

    // ── Sample library ──────────────────────────────────────────────────────

    void findSamplesInConfiguredRoot()
    {
        const QString root = harness().makeTempSubdir("samplelib");
        QVERIFY(writeHit(root + "/Drums/Kicks/ZqxKick Hard.wav", 0.05));
        QVERIFY(writeHit(root + "/Drums/Kicks/zqx other.wav", 0.05));
        QVERIFY(writeHit(root + "/Drums/Snares/Zqx Snare 01.wav", 0.05));
        QVERIFY(writeHit(root + "/Hi Hats/zqx_closed.wav", 0.05));
        {
            QFile txt(root + "/Drums/zqx readme.txt");
            QVERIFY(txt.open(QIODevice::WriteOnly));
            txt.write("not audio");
        }
        RootsSetting setting({root});

        const QStringList roots = SampleLibrary::sampleRoots();
        bool hasRoot = false;
        for (const auto& r : roots)
            if (samePathForm(r) == samePathForm(root)) hasRoot = true;
        QVERIFY2(hasRoot, qPrintable(roots.join(", ")));

        // All words must match; file-name hits rank before folder-only hits.
        OD_CALL_OK(kick, "find_samples", (QJsonObject{{"query", "zqx kick"}}));
        const auto kicks = kick.value("samples").toArray();
        QCOMPARE(int(kicks.size()), 2);
        QCOMPARE(kicks[0].toObject().value("relative_path").toString(), QStringLiteral("Drums/Kicks/ZqxKick Hard.wav"));
        QCOMPARE(kicks[0].toObject().value("name").toString(), QStringLiteral("ZqxKick Hard"));
        QCOMPARE(kicks[0].toObject().value("folder").toString(), QStringLiteral("Drums/Kicks"));
        QVERIFY(kicks[0].toObject().value("size_bytes").toDouble() > 44);
        QCOMPARE(kicks[1].toObject().value("relative_path").toString(), QStringLiteral("Drums/Kicks/zqx other.wav"));
        QCOMPARE(kick.value("total_matches").toInt(), 2);

        // Case-insensitive, separators ignored ("hihat" finds "Hi Hats").
        OD_CALL_OK(hat, "find_samples", (QJsonObject{{"query", "HIHAT zqx"}}));
        QCOMPARE(int(hat.value("samples").toArray().size()), 1);

        // Folder filter (relative to the root), non-audio files ignored.
        OD_CALL_OK(drums, "find_samples", (QJsonObject{{"query", "zqx"}, {"folder", "Drums"}}));
        QCOMPARE(int(drums.value("samples").toArray().size()), 3);
        OD_CALL_OK(limited, "find_samples", (QJsonObject{{"query", "zqx"}, {"folder", root}, {"limit", 2}}));
        QCOMPARE(int(limited.value("samples").toArray().size()), 2);
        QCOMPARE(limited.value("total_matches").toInt(), 4);
        QVERIFY(limited.value("truncated").toBool());
        OD_CALL_ERR(noFolder, "find_samples", (QJsonObject{{"query", "zqx"}, {"folder", "No Such Folder Zqx"}}));
        QCOMPARE(TestHarness::errorCode(noFolder), AiErrorCode::NotFound);

        // Browsing.
        OD_CALL_OK(top, "list_sample_folders", QJsonObject{});
        bool rootListed = false;
        for (const auto& r : top.value("roots").toArray())
            if (samePathForm(r.toObject().value("path").toString()) == samePathForm(root))
                rootListed = r.toObject().value("sample_count").toInt() == 4;
        QVERIFY2(rootListed, qPrintable(dump(top)));
        OD_CALL_OK(sub, "list_sample_folders", (QJsonObject{{"folder", root}}));
        QMap<QString, int> counts;
        for (const auto& f : sub.value("folders").toArray())
            counts[f.toObject().value("name").toString()] = f.toObject().value("sample_count").toInt();
        QCOMPARE(counts.value("Drums"), 3);
        QCOMPARE(counts.value("Hi Hats"), 1);
        QCOMPARE(sub.value("samples_here").toInt(), 0);
        OD_CALL_OK(kicksDir, "list_sample_folders", (QJsonObject{{"folder", "Drums/Kicks"}}));
        QCOMPARE(kicksDir.value("samples_here").toInt(), 2);

        // A relative_path from find_samples is accepted by add_sample_channel.
        OD_CALL_OK(added, "add_sample_channel", (QJsonObject{{"path", "Drums/Snares/Zqx Snare 01.wav"}}));
        QCOMPARE(added.value("name").toString(), QStringLiteral("Zqx Snare 01"));
    }

    // ── Finding 13: hand-edited pattern clips are detached, not wiped ───────

    // Hand edits (piano roll) to a pattern clip are written back into the
    // pattern (FL-style linked pattern instances): the clip stays a pattern
    // clip, and later step edits keep the hand-made off-grid note.
    void handEditedPatternClipWritesBack()
    {
        OD_CALL_OK(t, "create_midi_track", (QJsonObject{{"name", "Kick"}}));
        const QString kickId = TestHarness::findId(t, ids::kTrack);
        OD_CALL_OK(created, "create_pattern", (QJsonObject{{"name", "Beat"}}));
        const int pid = patternIdOf(created);
        QVERIFY(pid > 0);
        const QString pattern = ids::makePattern(pid);
        OD_CALL_OK(s1, "set_pattern_steps",
                   (QJsonObject{{"pattern", pattern}, {"track", kickId}, {"steps_string", "x...x...x...x..."}}));
        OD_CALL_OK(placed, "place_pattern", (QJsonObject{{"pattern", pattern}, {"start_bar", 1}}));
        TestHarness::pump(20);

        auto pls = pm().placements(pid);
        QCOMPARE(int(pls.size()), 1);
        QCOMPARE(int(pls.first().clipIds.size()), 1);
        const auto editedId = pls.first().clipIds.first();
        auto* mc = midiClip(editedId);
        QVERIFY(mc);
        QCOMPARE(mc->getSequence().getNotes().size(), 4);

        // A hand edit (piano roll): one extra off-grid note.
        auto& um = harness().edit().getUndoManager();
        um.beginNewTransaction("Hand edit");
        mc->getSequence().addNote(67, tracktion::BeatPosition::fromBeats(1.37), tracktion::BeatDuration::fromBeats(0.5),
                                  90, 0, &um);
        TestHarness::pump(100);
        pm().flushEditNotifications();

        const auto kickTrack = *ids::parse(kickId, ids::kTrack);
        QCOMPARE(int(pm().channelNotes(pid, kickTrack).size()), 5);

        // A later step edit keeps the hand-made note, in the same clip.
        OD_CALL_OK(s2, "set_pattern_steps",
                   (QJsonObject{{"pattern", pattern}, {"track", kickId}, {"steps_string", "x.x.x.x.x.x.x.x."}}));
        TestHarness::pump(20);

        mc = midiClip(editedId);
        QVERIFY2(mc, "the hand-edited clip was deleted");
        QVERIFY2(mc->state.hasProperty(kPlacementIdProp), "the hand-edited clip left the pattern");
        QCOMPARE(mc->getSequence().getNotes().size(), 9);
        pls = pm().placements(pid);
        QCOMPARE(int(pls.size()), 1);
        QCOMPARE(pls.first().clipIds.first(), editedId);
    }

    // ── Finding 17: placement start persists across save/reload ─────────────

    void draggedPlacementClipKeepsItsStartAfterReload()
    {
        OD_CALL_OK(k, "create_midi_track", (QJsonObject{{"name", "Kick"}}));
        OD_CALL_OK(s, "create_midi_track", (QJsonObject{{"name", "Snare"}}));
        const QString kickId = TestHarness::findId(k, ids::kTrack);
        const QString snareId = TestHarness::findId(s, ids::kTrack);
        OD_CALL_OK(created, "create_pattern", (QJsonObject{{"name", "Beat"}}));
        const int pid = patternIdOf(created);
        const QString pattern = ids::makePattern(pid);
        OD_CALL_OK(a, "set_pattern_steps",
                   (QJsonObject{{"pattern", pattern}, {"track", kickId}, {"steps_string", "x...x...x...x..."}}));
        OD_CALL_OK(b, "set_pattern_steps",
                   (QJsonObject{{"pattern", pattern}, {"track", snareId}, {"steps_string", "....x.......x..."}}));
        OD_CALL_OK(placed, "place_pattern", (QJsonObject{{"pattern", pattern}, {"start_beat", 0}}));
        TestHarness::pump(20);

        // Drag only the Kick clip to beat 8.
        const auto kickTrack = *ids::parse(kickId, ids::kTrack);
        te::MidiClip* kickClip = nullptr;
        const auto placedNow = pm().placements(pid);   // keep the list alive while iterating
        QVERIFY(!placedNow.isEmpty());
        for (const auto& cid : placedNow.first().clipIds)
            if (auto* mc = midiClip(cid); mc && mc->getTrack()->itemID == kickTrack) kickClip = mc;
        QVERIFY(kickClip);
        auto& ts = harness().edit().tempoSequence;
        harness().edit().getUndoManager().beginNewTransaction("Drag");
        kickClip->setPosition(te::ClipPosition{
            tracktion::TimeRange(ts.toTime(tracktion::BeatPosition::fromBeats(8.0)),
                                 ts.toTime(tracktion::BeatPosition::fromBeats(12.0))),
            tracktion::TimeDuration()});
        em().notifyEdited();
        TestHarness::pump(20);

        const QString projPath = QDir(dir_.path()).filePath("dragged.tracktionedit");
        QVERIFY(em().saveEditAs(toJuce(projPath)));
        harness().resetProject();
        QVERIFY(em().loadEdit(toJuce(projPath)));
        TestHarness::pump(50);

        QCOMPARE(int(pm().placements(pid).size()), 1);
        QVERIFY2(std::abs(pm().placements(pid).first().startBeat - 8.0) < 1e-6,
                 qPrintable(QString::number(pm().placements(pid).first().startBeat)));

        // The next pattern edit moves the Snare to the dragged position, not
        // the Kick back to 0.
        OD_CALL_OK(c, "set_pattern_steps",
                   (QJsonObject{{"pattern", pattern}, {"track", kickId}, {"steps_string", "x.x.x.x.x.x.x.x."}}));
        TestHarness::pump(20);
        const auto pls = pm().placements(pid);
        QCOMPARE(int(pls.size()), 1);
        QCOMPARE(int(pls.first().clipIds.size()), 2);
        for (const auto& cid : pls.first().clipIds) {
            auto* mc = midiClip(cid);
            QVERIFY(mc);
            QVERIFY2(std::abs(clipStartBeat(mc) - 8.0) < 1e-6, qPrintable(QString::number(clipStartBeat(mc))));
        }
    }

    // ── Finding 14: no pattern playback during a render ─────────────────────

    void patternPlayerStopsAndRefusesDuringRender()
    {
        OD_CALL_OK(t, "create_midi_track", (QJsonObject{{"name", "Kick"}}));
        const QString kickId = TestHarness::findId(t, ids::kTrack);
        OD_CALL_OK(created, "create_pattern", (QJsonObject{{"name", "Loop"}}));
        const int pid = patternIdOf(created);
        OD_CALL_OK(s, "set_pattern_steps", (QJsonObject{
            {"pattern", ids::makePattern(pid)}, {"track", kickId}, {"steps_string", "x...x...x...x..."}}));

        PatternPlayer player(pm(), em());
        player.play(pid);
        QVERIFY(player.isPlaying());

        QVERIFY(em().tryClaimRender());
        TestHarness::pump(30);
        const bool stillPlaying = player.isPlaying();
        player.play(pid);
        const bool startedDuringRender = player.isPlaying();
        em().releaseRender();
        QVERIFY2(!stillPlaying, "the pattern player kept playing during a render");
        QVERIFY2(!startedDuringRender, "play() started the pattern player during a render");

        player.play(pid);
        QVERIFY(player.isPlaying());
        player.stop();
    }
};

OD_REGISTER_TEST(TestPatternFixes)
#include "tst_PatternFixes.moc"
