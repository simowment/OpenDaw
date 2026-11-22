// AudioFileAnalyzer on synthesized WAV files, then the render tools
// (render_region, export_stems) on a small project.

#include "TestHarness.h"

#include "ai/AiIds.h"
#include "ai/AiToolExtensions.h"
#include "engine/AudioFileAnalyzer.h"
#include "engine/EditManager.h"
#include "engine/instruments/SoundFontInstrument.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <functional>

using namespace OpenDaw;
using OpenDaw::Test::harness;
using OpenDaw::Test::TestHarness;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// Writes a stereo 24-bit WAV; gen(channel, sampleIndex) gives the sample.
bool writeWav(const juce::File& file, double seconds,
              const std::function<float(int, int)>& gen, int channels = 2)
{
    const int n = int(seconds * kSampleRate);
    juce::AudioBuffer<float> buffer(channels, n);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < n; ++i)
            buffer.setSample(c, i, gen(c, i));

    file.deleteFile();
    juce::WavAudioFormat wav;
    auto stream = std::unique_ptr<juce::OutputStream>(file.createOutputStream());
    if (!stream)
        return false;
    const auto opts = juce::AudioFormatWriterOptions{}
                          .withSampleRate(kSampleRate)
                          .withNumChannels(channels)
                          .withBitsPerSample(24);
    auto writer = wav.createWriterFor(stream, opts);
    if (!writer)
        return false;
    return writer->writeFromAudioSampleBuffer(buffer, 0, n);
}

juce::File toJuce(const QString& path)
{
    return juce::File(juce::String(QDir::toNativeSeparators(path).toUtf8().constData()));
}

// Every number anywhere in the JSON is finite (no NaN/inf leaks to agents).
bool allFinite(const QJsonValue& v, QString* where, const QString& path = QStringLiteral("$"))
{
    if (v.isDouble()) {
        if (!std::isfinite(v.toDouble())) {
            if (where) *where = path;
            return false;
        }
        return true;
    }
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            if (!allFinite(it.value(), where, path + "." + it.key()))
                return false;
    } else if (v.isArray()) {
        const QJsonArray a = v.toArray();
        for (int i = 0; i < a.size(); ++i)
            if (!allFinite(a[i], where, QStringLiteral("%1[%2]").arg(path).arg(i)))
                return false;
    }
    return true;
}

// The spectrum band that contains `hz`, or {}.
QJsonObject bandContaining(const QJsonObject& analysis, double hz)
{
    for (const auto& b : analysis.value("spectrum").toObject().value("bands").toArray()) {
        const QJsonObject band = b.toObject();
        if (hz >= band.value("low_hz").toDouble() && hz < band.value("high_hz").toDouble())
            return band;
    }
    return {};
}

QString dump(const QJsonObject& o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)).left(2000);
}

const QStringList kAnalysisKeys{
    "duration_seconds", "sample_rate", "channels", "peak_dbfs", "true_peak_dbtp", "rms_dbfs",
    "integrated_lufs", "clipped_samples", "silence_ratio", "stereo", "spectrum", "warnings",
};

} // namespace

class TestRenderAnalysis : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

    juce::File wavPath(const char* name) { return toJuce(dir_.filePath(QString::fromLatin1(name))); }

    QJsonObject analyzeFile(const juce::File& f)
    {
        QString error;
        const QJsonObject a = AudioFileAnalyzer::analyze(f, {}, &error);
        if (a.isEmpty())
            qWarning() << "analyze failed:" << error;
        return a;
    }

private slots:
    void initTestCase() { QVERIFY(dir_.isValid()); }
    void init() { harness().resetProject(); }

    // ── AudioFileAnalyzer ────────────────────────────────────────────────────

    void sineMinus6dB()
    {
        const auto f = wavPath("sine1k.wav");
        const float amp = float(std::pow(10.0, -6.0 / 20.0));
        QVERIFY(writeWav(f, 3.0, [amp](int, int i) {
            return amp * float(std::sin(2.0 * kPi * 1000.0 * i / kSampleRate));
        }));

        const QJsonObject a = analyzeFile(f);
        QVERIFY(!a.isEmpty());
        for (const auto& k : kAnalysisKeys)
            QVERIFY2(a.contains(k), qPrintable("missing " + k + " in " + dump(a)));

        QVERIFY2(std::abs(a.value("duration_seconds").toDouble() - 3.0) < 0.01, qPrintable(dump(a)));
        QCOMPARE(a.value("channels").toInt(), 2);
        QCOMPARE(a.value("sample_rate").toDouble(), kSampleRate);
        QVERIFY2(std::abs(a.value("peak_dbfs").toDouble() - -6.0) < 0.3, qPrintable(dump(a)));
        // RMS of a sine is 3 dB below its peak.
        QVERIFY2(std::abs(a.value("rms_dbfs").toDouble() - -9.0) < 0.5, qPrintable(dump(a)));
        QCOMPARE(a.value("clipped_samples").toDouble(), 0.0);
        QVERIFY(a.value("silence_ratio").toDouble() < 0.01);

        const QJsonObject stereo = a.value("stereo").toObject();
        QVERIFY2(stereo.value("correlation").toDouble() > 0.98, qPrintable(dump(stereo)));

        // The 1 kHz band holds nearly all the energy.
        const QJsonObject mid = bandContaining(a, 1000.0);
        QVERIFY2(!mid.isEmpty(), qPrintable(dump(a)));
        const double midDb = mid.value("energy_db").toDouble();
        for (const auto& b : a.value("spectrum").toObject().value("bands").toArray()) {
            const QJsonObject band = b.toObject();
            if (band.value("name") == mid.value("name"))
                continue;
            QVERIFY2(band.value("energy_db").toDouble() < midDb - 20.0,
                     qPrintable("band " + band.value("name").toString() + " too loud: " + dump(a)));
        }
        const double centroid = a.value("spectrum").toObject().value("centroid_hz").toDouble();
        QVERIFY2(centroid > 700.0 && centroid < 1500.0, qPrintable(dump(a.value("spectrum").toObject())));

        QString where;
        QVERIFY2(allFinite(a, &where), qPrintable("non-finite number at " + where));
    }

    void silence()
    {
        const auto f = wavPath("silence.wav");
        QVERIFY(writeWav(f, 2.0, [](int, int) { return 0.0f; }));

        const QJsonObject a = analyzeFile(f);
        QVERIFY(!a.isEmpty());
        QCOMPARE(a.value("silence_ratio").toDouble(), 1.0);
        QVERIFY(a.value("peak_dbfs").toDouble() <= -100.0);
        QCOMPARE(a.value("clipped_samples").toDouble(), 0.0);

        QString where;
        QVERIFY2(allFinite(a, &where), qPrintable("non-finite number at " + where + ": " + dump(a)));

        bool silentWarning = false;
        for (const auto& w : a.value("warnings").toArray())
            silentWarning |= w.toString().contains("silent", Qt::CaseInsensitive);
        QVERIFY2(silentWarning, qPrintable(dump(a)));
    }

    void clippedSquare()
    {
        const auto f = wavPath("square.wav");
        // 100 Hz square wave at full scale.
        QVERIFY(writeWav(f, 1.0, [](int, int i) {
            return (i / 240) % 2 == 0 ? 1.0f : -1.0f;
        }));

        const QJsonObject a = analyzeFile(f);
        QVERIFY(!a.isEmpty());
        QVERIFY2(a.value("clipped_samples").toDouble() > 0.0, qPrintable(dump(a)));
        QVERIFY(a.value("peak_dbfs").toDouble() > -0.1);

        bool clipWarning = false;
        for (const auto& w : a.value("warnings").toArray())
            clipWarning |= w.toString().contains("clip", Qt::CaseInsensitive);
        QVERIFY2(clipWarning, qPrintable(dump(a)));

        QString where;
        QVERIFY2(allFinite(a, &where), qPrintable("non-finite number at " + where));
    }

    void invertedStereo()
    {
        const auto f = wavPath("inverted.wav");
        QVERIFY(writeWav(f, 2.0, [](int c, int i) {
            const float s = 0.5f * float(std::sin(2.0 * kPi * 440.0 * i / kSampleRate));
            return c == 0 ? s : -s;
        }));

        const QJsonObject a = analyzeFile(f);
        QVERIFY(!a.isEmpty());
        QVERIFY2(a.value("stereo").toObject().value("correlation").toDouble() < -0.98, qPrintable(dump(a)));

        bool phaseWarning = false;
        for (const auto& w : a.value("warnings").toArray())
            phaseWarning |= w.toString().contains("phase", Qt::CaseInsensitive)
                            || w.toString().contains("correlation", Qt::CaseInsensitive);
        QVERIFY2(phaseWarning, qPrintable(dump(a)));
    }

    void missingFile()
    {
        QString error;
        const QJsonObject a = AudioFileAnalyzer::analyze(wavPath("does-not-exist.wav"), {}, &error);
        QVERIFY(a.isEmpty());
        QVERIFY(!error.isEmpty());
    }

    // ── Render tools ─────────────────────────────────────────────────────────

    void renderRegionMidiTrack()
    {
        const bool haveSoundFont = SoundFontLibrary::instance().isAvailable();

        OD_CALL_OK(tempo, "set_tempo", (QJsonObject{{"bpm", 120}}));
        OD_CALL_OK(trackOut, "create_midi_track", (QJsonObject{{"name", "Piano"}}));
        const QString trackId = TestHarness::findId(trackOut, ids::kTrack);
        QVERIFY(!trackId.isEmpty());
        // New MIDI tracks get the built-in piano when the SoundFont is there.
        if (haveSoundFont)
            QVERIFY(harness().editManager().getTrackInstrument(harness().trackById(trackId)) != nullptr);

        OD_CALL_OK(clipOut, "create_midi_clip",
                   (QJsonObject{{"track", trackId}, {"start_beat", 0}, {"length_beats", 8}}));
        const QString clipId = TestHarness::findId(clipOut, ids::kClip);
        QVERIFY(!clipId.isEmpty());
        QJsonArray notes;
        for (int bar = 0; bar < 2; ++bar)
            for (int n : {60, 64, 67})
                notes.append(QJsonObject{{"note", n}, {"start_beat", bar * 4}, {"length_beats", 3.5}, {"velocity", 110}});
        OD_CALL_OK(added, "add_midi_notes", (QJsonObject{{"clip_id", clipId}, {"notes", notes}}));

        // Only this track is heard: the file is written even when the track
        // is silent (no SoundFont), so the shape of the result is always checked.
        const QString outPath = QDir(dir_.path()).filePath("region.wav");
        OD_CALL_OK(r, "render_region", (QJsonObject{
            {"start_beat", 0}, {"end_beat", 8}, {"tracks", QJsonArray{trackId}},
            {"output_path", outPath}, {"tail_seconds", 1}}));

        const QString path = r.value("path").toString();
        QVERIFY2(QFileInfo::exists(path), qPrintable(dump(r)));
        QCOMPARE(QFileInfo(path).canonicalFilePath(), QFileInfo(outPath).canonicalFilePath());
        QVERIFY(QFileInfo(path).size() > 1000);

        // 2 bars of 4/4 at 120 bpm = 4 s, plus at most the 1 s tail.
        const double duration = r.value("duration_seconds").toDouble();
        QVERIFY2(duration >= 3.95 && duration <= 5.1, qPrintable(dump(r)));
        const QJsonObject range = r.value("range").toObject();
        QVERIFY(std::abs(range.value("start_seconds").toDouble()) < 1e-6);
        QVERIFY2(std::abs(range.value("end_seconds").toDouble() - 4.0) < 0.01, qPrintable(dump(range)));
        QVERIFY(std::abs(range.value("end_beat").toDouble() - 8.0) < 0.01);

        const QJsonObject a = r.value("analysis").toObject();
        for (const auto& k : kAnalysisKeys)
            QVERIFY2(a.contains(k), qPrintable("analysis lacks " + k + ": " + dump(r)));
        QString where;
        QVERIFY2(allFinite(r, &where), qPrintable("non-finite number at " + where));

        if (!haveSoundFont)
            QSKIP("built-in SoundFont not found next to the executable: audio content not checked");

        QVERIFY2(a.value("peak_dbfs").toDouble() > -40.0, qPrintable(dump(a)));
        QVERIFY2(a.value("silence_ratio").toDouble() < 0.5, qPrintable(dump(a)));
        QCOMPARE(a.value("clipped_samples").toDouble(), 0.0);

        // The whole mix renders too, and carries the same notes.
        const QString mixPath = QDir(dir_.path()).filePath("mix.wav");
        OD_CALL_OK(mix, "render_region", (QJsonObject{
            {"start_beat", 0}, {"end_beat", 8}, {"output_path", mixPath}}));
        QVERIFY(QFileInfo::exists(mix.value("path").toString()));
        QVERIFY(mix.value("analysis").toObject().value("peak_dbfs").toDouble() > -40.0);
        QVERIFY(std::abs(mix.value("duration_seconds").toDouble() - 4.0) < 0.05);
    }

    void renderRegionBadRange()
    {
        OD_CALL_ERR(equal, "render_region", (QJsonObject{{"start_beat", 4}, {"end_beat", 4}}));
        QCOMPARE(TestHarness::errorCode(equal), AiErrorCode::InvalidArgument);

        OD_CALL_ERR(reversed, "render_region", (QJsonObject{{"start_beat", 8}, {"end_beat", 2}}));
        QCOMPARE(TestHarness::errorCode(reversed), AiErrorCode::InvalidArgument);

        OD_CALL_ERR(mixed, "render_region", (QJsonObject{{"start_beat", 0}, {"end_seconds", 2}}));
        QCOMPARE(TestHarness::errorCode(mixed), AiErrorCode::InvalidArgument);

        OD_CALL_ERR(badRate, "render_region",
                    (QJsonObject{{"start_beat", 0}, {"end_beat", 4}, {"sample_rate", 12345}}));
        QCOMPARE(TestHarness::errorCode(badRate), AiErrorCode::InvalidArgument);

        OD_CALL_ERR(missing, "render_region",
                    (QJsonObject{{"start_beat", 0}, {"end_beat", 4}, {"tracks", QJsonArray{"track:987654321987"}}}));
        QCOMPARE(TestHarness::errorCode(missing), AiErrorCode::NotFound);
    }

    void renderIsNotBatchable()
    {
        OD_CALL_ERR(out, "run_batch", (QJsonObject{{"commands", QJsonArray{
            QJsonObject{{"tool", "render_region"}, {"input", QJsonObject{{"start_beat", 0}, {"end_beat", 4}}}}}}}));
        QVERIFY2(dump(out).contains(AiErrorCode::NotAllowed), qPrintable(dump(out)));
    }

    void exportStemsOnePerTrack()
    {
        // A fresh project has 4 empty audio tracks; add two MIDI tracks with
        // notes so the song has content.
        QStringList trackIds;
        for (const char* name : {"Stem A", "Stem B"}) {
            OD_CALL_OK(t, "create_midi_track", (QJsonObject{{"name", name}}));
            const QString id = TestHarness::findId(t, ids::kTrack);
            QVERIFY(!id.isEmpty());
            trackIds << id;
            OD_CALL_OK(c, "create_midi_clip", (QJsonObject{{"track", id}, {"start_beat", 0}, {"length_beats", 4}}));
            OD_CALL_OK(n, "add_midi_notes", (QJsonObject{
                {"clip_id", TestHarness::findId(c, ids::kClip)},
                {"notes", QJsonArray{QJsonObject{{"note", 48}, {"start_beat", 0}, {"length_beats", 2}}}}}));
        }

        const QString folder = QDir(dir_.path()).filePath("stems");
        OD_CALL_OK(out, "export_stems", (QJsonObject{
            {"mode", "tracks"}, {"folder", folder}, {"start_beat", 0}, {"end_beat", 4}}));

        // One stem per unmuted non-bus track; with no routing changes that is
        // every track of the project.
        const auto plan = harness().editManager().planStems(StemMode::Tracks);
        const QJsonArray stems = out.value("stems").toArray();
        QCOMPARE(int(stems.size()), int(plan.size()));
        QCOMPARE(int(stems.size()), harness().editManager().getAudioTracks().size());
        QCOMPARE(out.value("failed").toInt(), 0);

        QSet<QString> sources;
        for (const auto& v : stems) {
            const QJsonObject s = v.toObject();
            QVERIFY2(QFileInfo::exists(s.value("path").toString()), qPrintable(dump(s)));
            // 4 beats at the default 120 bpm.
            QVERIFY2(std::abs(s.value("duration_seconds").toDouble() - 2.0) < 0.05, qPrintable(dump(s)));
            QVERIFY(s.value("analysis").isObject());
            QCOMPARE(int(s.value("sources").toArray().size()), 1);
            sources.insert(s.value("sources").toArray().first().toString());
        }
        for (const auto& id : trackIds)
            QVERIFY2(sources.contains(id), qPrintable(id));

        // Stem mutes are restored afterwards.
        QVERIFY(!harness().editManager().stemMutesActive());
        for (auto* t : harness().editManager().getAudioTracks())
            QVERIFY(!t->isMuted(false));
    }
};

OD_REGISTER_TEST(TestRenderAnalysis)
#include "tst_RenderAnalysis.moc"
