// EditManager / render fixes: signal batching (one tracksChanged per Add MIDI
// Track), undo groups (EditManager methods join the caller's transaction),
// the GUI-thread render claim and its BUSY path, render sessions (the live
// playback graph is freed during a render and given back after), NaN/Inf
// samples in loudness analysis, and render_region's beat unit.

#include "TestHarness.h"

#include "ai/AiIds.h"
#include "ai/AiToolExtensions.h"
#include "api/RenderService.h"
#include "engine/AudioFileAnalyzer.h"
#include "engine/EditManager.h"
#include "engine/LoudnessMeter.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <functional>
#include <limits>

using namespace OpenDaw;
using OpenDaw::Test::harness;
using OpenDaw::Test::TestHarness;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

juce::File toJuce(const QString& path)
{
    return juce::File(juce::String(QDir::toNativeSeparators(path).toUtf8().constData()));
}

QString dump(const QJsonObject& o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)).left(1500);
}

juce::AudioBuffer<float> sine(double seconds, float amplitude, int channels = 2)
{
    const int n = int(seconds * kRate);
    juce::AudioBuffer<float> buffer(channels, n);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < n; ++i)
            buffer.setSample(c, i, amplitude * float(std::sin(2.0 * kPi * 1000.0 * i / kRate)));
    return buffer;
}

bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& buffer, int bits)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    auto stream = std::unique_ptr<juce::OutputStream>(file.createOutputStream());
    if (!stream) return false;
    const auto opts = juce::AudioFormatWriterOptions{}
                          .withSampleRate(kRate)
                          .withNumChannels(buffer.getNumChannels())
                          .withBitsPerSample(bits);
    auto writer = wav.createWriterFor(stream, opts);
    if (!writer) return false;
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

bool hasWarningContaining(const QJsonObject& analysis, const QString& text)
{
    for (const auto& w : analysis.value("warnings").toArray())
        if (w.toString().contains(text, Qt::CaseInsensitive)) return true;
    return false;
}

} // namespace

class TestEngineFixes : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

    EditManager& em() { return harness().editManager(); }

private slots:
    void initTestCase() { QVERIFY(dir_.isValid()); }
    void init() { harness().resetProject(); }
    void cleanup()
    {
        // Never leave a claim behind for the next test.
        while (em().inRenderSession()) em().endRenderSession();
        if (em().isRenderInProgress()) em().releaseRender();
        while (em().inUndoGroup()) em().endUndoGroup();
    }

    // ── Signal batching ──────────────────────────────────────────────────────

    void addMidiTrackEmitsTracksChangedOnce()
    {
        QStringList order;
        auto c1 = connect(&em(), &EditManager::tracksChanged, this, [&order] { order << "tracks"; });
        auto c2 = connect(&em(), &EditManager::editChanged, this, [&order] { order << "edit"; });
        QSignalSpy tracks(&em(), &EditManager::tracksChanged);

        auto* track = em().addMidiTrack();
        disconnect(c1);
        disconnect(c2);

        QVERIFY(track != nullptr);
        QVERIFY(em().isMidiTrack(track));
        QCOMPARE(tracks.count(), 1);
        // tracksChanged comes before editChanged (views rebuild, then refresh).
        QVERIFY2(order.count("edit") <= 1, qPrintable(order.join(',')));
        QCOMPARE(order.first(), QStringLiteral("tracks"));
    }

    void nestedBatchDefersUntilOutermostEnd()
    {
        QSignalSpy tracks(&em(), &EditManager::tracksChanged);
        QSignalSpy routing(&em(), &EditManager::routingChanged);
        em().beginSignalBatch();
        em().addAudioTrack();
        {
            const EditManager::ScopedSignalBatch inner(em());
            em().addBusTrack();
        }
        QCOMPARE(tracks.count(), 0);
        QCOMPARE(routing.count(), 0);
        em().endSignalBatch();
        QCOMPARE(tracks.count(), 1);
        QCOMPARE(routing.count(), 1);

        // Nothing pending: ending another empty batch emits nothing.
        em().beginSignalBatch();
        em().endSignalBatch();
        QCOMPARE(tracks.count(), 1);
    }

    void deletionInsideBatchEmitsAtOnce()
    {
        auto* track = em().addAudioTrack();
        QVERIFY(track);
        QSignalSpy tracks(&em(), &EditManager::tracksChanged);
        {
            const EditManager::ScopedSignalBatch batch(em());
            em().removeTrack(track);
            // Views must drop the freed track now, not when the batch ends.
            QCOMPARE(tracks.count(), 1);
        }
    }

    // ── Undo groups ──────────────────────────────────────────────────────────

    void undoGroupMakesBusAndSendOneStep()
    {
        auto& um = harness().edit().getUndoManager();
        auto* src = em().getAudioTrack(0);
        QVERIFY(src);
        const int before = em().trackCount();

        um.beginNewTransaction("Agent batch");
        em().beginUndoGroup();
        auto* bus = em().addFxBus("Reverb");
        QVERIFY(bus);
        QVERIFY(em().addSend(*src, *bus, -6.0f, false) != nullptr);
        em().setSendMuted(*src, *bus, true);
        em().endUndoGroup();
        QVERIFY(!em().inUndoGroup());
        QCOMPARE(em().trackCount(), before + 1);
        QCOMPARE(int(em().getTrackSends(src).size()), 1);

        // One undo reverts the bus and the send together.
        em().undo();
        QCOMPARE(em().trackCount(), before);
        QVERIFY(em().getBusTracks().isEmpty());
        QCOMPARE(int(em().getTrackSends(em().getAudioTrack(0)).size()), 0);
    }

    void withoutGroupSendIsItsOwnStep()
    {
        // Control: the old behaviour, so the test above really tests the group.
        auto& um = harness().edit().getUndoManager();
        auto* src = em().getAudioTrack(0);
        const int before = em().trackCount();
        um.beginNewTransaction("Bus");
        auto* bus = em().addBusTrack();
        QVERIFY(bus);
        QVERIFY(em().addSend(*src, *bus) != nullptr);
        em().undo();
        QCOMPARE(em().trackCount(), before + 1);   // only "Add Send" was undone
        QCOMPARE(int(em().getTrackSends(em().getAudioTrack(0)).size()), 0);
    }

    // ── Render claim ─────────────────────────────────────────────────────────

    void renderClaimBusyPath()
    {
        QVERIFY(!em().isRenderInProgress());
        QVERIFY(em().tryClaimRender());
        QVERIFY(em().isRenderInProgress());
        QVERIFY(!em().tryClaimRender());
        {
            const EditManager::ScopedRenderClaim second(em());
            QVERIFY(!second.claimed());
        }
        QVERIFY(em().isRenderInProgress());   // a failed claim releases nothing

        // An unclaimed exportMix reports Busy and leaves lastExportError alone.
        const QString errorBefore = em().lastExportError();
        ExportSettings s;
        s.destFile = toJuce(QDir(dir_.path()).filePath("busy.wav"));
        auto outcome = EditManager::ExportOutcome::Ok;
        QVERIFY(!em().exportMix(s, nullptr, &outcome));
        QCOMPARE(int(outcome), int(EditManager::ExportOutcome::Busy));
        QCOMPARE(em().lastExportError(), errorBefore);
        QVERIFY(em().isRenderInProgress());

        // The agent tools answer BUSY.
        OD_CALL_ERR(r, "render_region", (QJsonObject{{"start_seconds", 0}, {"end_seconds", 1}}));
        QCOMPARE(TestHarness::errorCode(r), AiErrorCode::Busy);

        // A claimed exportMix neither claims nor releases (it fails here: the
        // project is empty, which is a Failed outcome, not Busy).
        s.renderClaimed = true;
        QVERIFY(!em().exportMix(s, nullptr, &outcome));
        QCOMPARE(int(outcome), int(EditManager::ExportOutcome::Failed));
        QVERIFY(!em().lastExportError().isEmpty());
        QVERIFY(em().isRenderInProgress());

        em().releaseRender();
        QVERIFY(!em().isRenderInProgress());
        QVERIFY(em().tryClaimRender());
        em().releaseRender();
    }

    void renderSessionNestsAndEnds()
    {
        auto& edit = harness().edit();
        QVERIFY(!edit.isRendering());
        em().beginRenderSession();
        QVERIFY(em().inRenderSession());
        QVERIFY(edit.isRendering());
        QVERIFY(!edit.getTransport().isPlayContextActive());
        {
            const EditManager::ScopedRenderSession inner(em());
            QVERIFY(edit.isRendering());
        }
        QVERIFY(edit.isRendering());
        em().endRenderSession();
        QVERIFY(!em().inRenderSession());
        QVERIFY(!edit.isRendering());
        em().endRenderSession();   // unbalanced end is ignored
        QVERIFY(!edit.isRendering());
    }

    // ── Render: claim/session released, beat unit ────────────────────────────

    void renderRegionReleasesAndUsesTracktionBeats()
    {
        const auto wav = toJuce(QDir(dir_.path()).filePath("tone.wav"));
        QVERIFY(writeWav(wav, sine(4.0, 0.5f), 24));
        auto* track = em().getAudioTrack(0);
        QVERIFY(track);
        em().addAudioClipToTrack(*track, wav, 0.0);
        em().setBpm(120.0);
        em().setTimeSignature(6, 8);
        TestHarness::pump(50);

        OD_CALL_OK(r, "render_region", (QJsonObject{{"start_beat", 0}, {"end_beat", 6},
                                                    {"analyze", false}}));
        QVERIFY(!em().isRenderInProgress());
        QVERIFY(!em().inRenderSession());
        QVERIFY(!harness().edit().isRendering());

        // start_beat/end_beat are the Edit's beats, the same as create_midi_clip
        // (BeatPosition): whatever unit the time signature gives them.
        const double expectedEnd = harness().edit().tempoSequence
                                       .toTime(tracktion::BeatPosition::fromBeats(6.0)).inSeconds();
        const QJsonObject range = r.value("range").toObject();
        QVERIFY2(std::abs(range.value("end_seconds").toDouble() - expectedEnd) < 0.002, qPrintable(dump(r)));
        QVERIFY2(std::abs(range.value("end_beat").toDouble() - 6.0) < 1e-6, qPrintable(dump(r)));

        // And the tool description says so instead of promising quarter notes.
        bool found = false;
        for (const auto& t : harness().renderService().toolExtensions(harness().executor())) {
            if (t.definition.value("name").toString() != QLatin1String("render_region")) continue;
            found = true;
            const QString d = t.definition.value("input_schema").toObject().value("properties").toObject()
                                  .value("start_beat").toObject().value("description").toString();
            QVERIFY2(d.contains("denominator"), qPrintable(d));
            QVERIFY2(!d.contains("quarter notes from"), qPrintable(d));
        }
        QVERIFY(found);
    }

    // ── NaN / Inf in loudness ────────────────────────────────────────────────

    void nanDoesNotPoisonLoudnessMeter()
    {
        auto buffer = sine(5.0, 0.5f);
        buffer.setSample(0, 1000, std::numeric_limits<float>::quiet_NaN());
        buffer.setSample(1, 2000, std::numeric_limits<float>::infinity());

        LoudnessMeter meter;
        meter.prepare(kRate);
        LoudnessGate gate;
        meter.process(buffer.getArrayOfReadPointers(), 2, buffer.getNumSamples());
        meter.drainSteps([&gate](const LoudnessMeter::Step& s) { gate.add(s); });
        QVERIFY(std::isfinite(gate.integrated()));
        QVERIFY2(gate.integrated() > -20.0, qPrintable(QString::number(gate.integrated())));
        QVERIFY(std::isfinite(meter.truePeakDb()));
        QVERIFY(meter.truePeakDb() < 0.0);

        const QJsonObject a = AudioFileAnalyzer::analyzeBuffer(buffer, kRate, {});
        QVERIFY2(a.value("integrated_lufs").toDouble() > -20.0, qPrintable(dump(a)));
        QVERIFY2(a.value("max_short_term_lufs").toDouble() > -20.0, qPrintable(dump(a)));
        QCOMPARE(a.value("non_finite_samples").toDouble(), 2.0);
        QVERIFY2(hasWarningContaining(a, "non-finite"), qPrintable(dump(a)));
    }

    void nanInFloatFileDoesNotGiveSilentLoudness()
    {
        auto buffer = sine(5.0, 0.5f);
        buffer.setSample(0, 4800, std::numeric_limits<float>::quiet_NaN());
        const auto file = toJuce(QDir(dir_.path()).filePath("nan.wav"));
        QVERIFY(writeWav(file, buffer, 32));

        QString error;
        const QJsonObject a = AudioFileAnalyzer::analyze(file, {}, &error);
        QVERIFY2(!a.isEmpty(), qPrintable(error));
        QVERIFY2(a.value("integrated_lufs").toDouble() > -20.0, qPrintable(dump(a)));
        QVERIFY2(a.value("max_short_term_lufs").toDouble() > -20.0, qPrintable(dump(a)));
        if (a.value("non_finite_samples").toDouble() > 0)
            QVERIFY2(hasWarningContaining(a, "non-finite"), qPrintable(dump(a)));
        else
            qWarning("the WAV reader replaced the NaN on read; only the loudness was checked");

        const auto fr = LoudnessMeter::analyzeFile(file);
        QVERIFY(fr.ok);
        QVERIFY2(fr.integrated > -20.0, qPrintable(QString::number(fr.integrated)));
    }

    void cleanAnalysisHasNoNonFiniteWarning()
    {
        const QJsonObject a = AudioFileAnalyzer::analyzeBuffer(sine(1.0, 0.5f), kRate, {});
        QCOMPARE(a.value("non_finite_samples").toDouble(), 0.0);
        QVERIFY(!hasWarningContaining(a, "non-finite"));
    }
};

OD_REGISTER_TEST(TestEngineFixes)
#include "tst_EngineFixes.moc"
