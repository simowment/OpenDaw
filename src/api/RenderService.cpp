#include "RenderService.h"
#include "ai/AiIds.h"
#include "ai/AiToolExecutor.h"
#include "engine/AudioFileAnalyzer.h"
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <memory>

namespace OpenDaw {

namespace {

constexpr double kMaxRenderSeconds = 3600.0;
constexpr double kMaxTailSeconds = 30.0;
constexpr qint64 kPruneAgeSeconds = 2 * 24 * 3600;   // renders older than this are removed

juce::File toJuceFile(const QString& path)
{
    return juce::File(juce::String::fromUTF8(QDir::toNativeSeparators(path).toUtf8().constData()));
}

QString toQString(const juce::String& s)
{
    return QString::fromStdString(s.toStdString());
}

QString timestamp()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
}

bool isInsideDir(const QString& path, const QString& dir)
{
#ifdef Q_OS_WIN
    constexpr auto cs = Qt::CaseInsensitive;
#else
    constexpr auto cs = Qt::CaseSensitive;
#endif
    const QString p = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString d = QDir::cleanPath(QDir(dir).absolutePath());
    return p.startsWith(d + QLatin1Char('/'), cs);
}

// Relative paths are taken relative to the render folder.
QString absoluteInRenderDir(const QString& path)
{
    QFileInfo fi(path);
    if (fi.isAbsolute()) return QDir::cleanPath(fi.absoluteFilePath());
    return QDir::cleanPath(QDir(RenderService::defaultRenderDir()).absoluteFilePath(path));
}

double fileDurationSeconds(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader || reader->sampleRate <= 0.0) return 0.0;
    return double(reader->lengthInSamples) / reader->sampleRate;
}

double round3(double v) { return std::isfinite(v) ? std::round(v * 1000.0) / 1000.0 : 0.0; }

// Restores the mutes and solos applyStemMutes() changed, on every way out.
struct StemMuteGuard {
    EditManager& editMgr;
    bool active = false;
    void apply(const EditManager::Stem& stem) { editMgr.applyStemMutes(stem); active = true; }
    ~StemMuteGuard() { if (active) editMgr.restoreStemMutes(); }
};

// ── Tool schema helpers ─────────────────────────────────────────────────────

QJsonObject typeOnly(const QString& type) { return QJsonObject{{"type", type}}; }

QJsonObject prop(const QString& type, const QString& description)
{
    return QJsonObject{{"type", type}, {"description", description}};
}

QJsonObject numberProp(const QString& description, double min, double max)
{
    return QJsonObject{{"type", "number"}, {"description", description}, {"minimum", min}, {"maximum", max}};
}

QJsonObject trackRefSchema()
{
    QJsonObject objProps;
    objProps["id"] = prop("string", "Track id like 'track:1043'.");
    objProps["index"] = prop("integer", "Zero-based track index.");
    objProps["name"] = prop("string", "Track name.");
    QJsonObject objType{{"type", "object"}, {"properties", objProps}, {"additionalProperties", false}};
    return QJsonObject{
        {"description", "Track id like 'track:1043' (preferred, stable), or name, or zero-based index."},
        {"oneOf", QJsonArray{typeOnly("string"), typeOnly("integer"), objType}}};
}

void addRangeProps(QJsonObject& props)
{
    props["start_beat"] = numberProp("Range start in beats from the song start (0 = bar 1 beat 1), the same "
                                     "beats as create_midi_clip and the other tools: one beat is the time "
                                     "signature's denominator note (a quarter note in 4/4, an eighth note "
                                     "in 6/8). Use beats or seconds, not both. Default: 0.", 0.0, 1.0e6);
    props["end_beat"] = numberProp("Range end in beats (same unit as start_beat). Default: the end of the "
                                   "song.", 0.0, 1.0e6);
    props["start_seconds"] = numberProp("Range start in seconds. Default: 0.", 0.0, kMaxRenderSeconds);
    props["end_seconds"] = numberProp("Range end in seconds. Default: the end of the song.", 0.0, 1.0e5);
}

void addFormatProps(QJsonObject& props)
{
    props["sample_rate"] = QJsonObject{{"type", "integer"},
                                       {"description", "Sample rate in Hz. Default 48000."},
                                       {"enum", QJsonArray{44100, 48000, 88200, 96000}}};
    props["bit_depth"] = QJsonObject{{"type", "integer"},
                                     {"description", "Bits per sample; 32 writes float WAV. Default 24."},
                                     {"enum", QJsonArray{16, 24, 32}}};
    props["tail_seconds"] = numberProp("Extra seconds rendered after the range end so reverb and delay "
                                       "tails decay (the render stops early once the tail is silent). "
                                       "Default 0.", 0.0, kMaxTailSeconds);
}

QJsonObject makeTool(const QString& name, const QString& description, const QJsonObject& properties,
                     const QJsonArray& required = {})
{
    QJsonObject schema{{"type", "object"}, {"properties", properties}, {"additionalProperties", false}};
    if (!required.isEmpty()) schema["required"] = required;
    return QJsonObject{{"name", name}, {"description", description}, {"input_schema", schema}};
}

// Range and format keys shared by render_region and export_stems.
void readCommon(const QJsonObject& in, RenderRequest& r)
{
    if (in.contains("start_beat")) r.startBeat = in["start_beat"].toDouble();
    if (in.contains("end_beat")) r.endBeat = in["end_beat"].toDouble();
    if (in.contains("start_seconds")) r.startSeconds = in["start_seconds"].toDouble();
    if (in.contains("end_seconds")) r.endSeconds = in["end_seconds"].toDouble();
    if (in.contains("sample_rate")) r.sampleRate = in["sample_rate"].toDouble();
    if (in.contains("bit_depth")) r.bitDepth = in["bit_depth"].toInt();
    if (in.contains("tail_seconds")) r.tailSeconds = in["tail_seconds"].toDouble();
    if (in.contains("analyze")) r.analyze = in["analyze"].toBool(true);
}

// The headline numbers of an analysis, for compare_audio_files.
QJsonObject summaryOf(const QJsonObject& a)
{
    QJsonObject s;
    for (const char* key : { "duration_seconds", "integrated_lufs", "true_peak_dbtp", "peak_dbfs",
                             "rms_dbfs", "crest_factor_db", "loudness_range_lu", "max_short_term_lufs",
                             "silence_ratio", "clipped_samples" })
        s[QLatin1String(key)] = a.value(QLatin1String(key));
    const auto stereo = a.value("stereo").toObject();
    s["stereo_correlation"] = stereo.value("correlation");
    s["stereo_width"] = stereo.value("width");
    s["stereo_balance_db"] = stereo.value("balance_db");
    const auto spectrum = a.value("spectrum").toObject();
    s["centroid_hz"] = spectrum.value("centroid_hz");
    s["rolloff_85_hz"] = spectrum.value("rolloff_85_hz");
    QJsonObject bands;
    for (const auto& b : spectrum.value("bands").toArray()) {
        const auto o = b.toObject();
        bands[o.value("name").toString()] = o.value("energy_db");
    }
    s["band_energy_db"] = bands;
    s["warnings"] = a.value("warnings");
    return s;
}

double delta(const QJsonObject& a, const QJsonObject& b, const QString& key)
{
    return round3(b.value(key).toDouble() - a.value(key).toDouble());
}

} // namespace

// ── RenderService ───────────────────────────────────────────────────────────

RenderService::RenderService(EditManager& editMgr, QObject* parent)
    : QObject(parent), editMgr_(editMgr)
{
    pruneOldRenders();
}

QString RenderService::defaultRenderDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (base.isEmpty()) base = QDir::tempPath();
    const QString dir = QDir::cleanPath(base + QStringLiteral("/OpenDaw/agent-renders"));
    QDir().mkpath(dir);
    return dir;
}

void RenderService::pruneOldRenders()
{
    // Agents render often; keep the temp folder from growing without bound.
    const QString dir = defaultRenderDir();
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-kPruneAgeSeconds);
    QStringList old;
    QDirIterator it(dir, QStringList{QStringLiteral("*.wav")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (QFileInfo(path).lastModified() < cutoff)
            old << path;
    }
    for (const auto& path : old)
        QFile::remove(path);
    // Stem folders left empty.
    const auto subdirs = QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& sub : subdirs)
        QDir(dir).rmdir(sub);   // only succeeds when empty
}

std::optional<RenderService::Failure> RenderService::checkReady() const
{
    auto* edit = editMgr_.edit();
    if (!edit) return Failure{AiErrorCode::Conflict, QStringLiteral("No project is loaded.")};
    if (editMgr_.isRenderInProgress() || editMgr_.stemMutesActive())
        return Failure{AiErrorCode::Busy, QStringLiteral("Another render or export is running; try again when it has finished.")};
    if (edit->getTransport().isRecording())
        return Failure{AiErrorCode::Busy, QStringLiteral("Recording is in progress; stop recording before rendering.")};
    return std::nullopt;
}

std::optional<RenderService::Failure> RenderService::checkFormat(const RenderRequest& r) const
{
    static const double kRates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };
    if (std::find(std::begin(kRates), std::end(kRates), r.sampleRate) == std::end(kRates))
        return Failure{AiErrorCode::InvalidArgument,
                       QStringLiteral("Unsupported sample rate %1; use 44100, 48000, 88200 or 96000.").arg(r.sampleRate)};
    if (r.bitDepth != 16 && r.bitDepth != 24 && r.bitDepth != 32)
        return Failure{AiErrorCode::InvalidArgument,
                       QStringLiteral("Unsupported bit depth %1; use 16, 24 or 32.").arg(r.bitDepth)};
    if (!std::isfinite(r.tailSeconds) || r.tailSeconds < 0.0 || r.tailSeconds > kMaxTailSeconds)
        return Failure{AiErrorCode::InvalidArgument, QStringLiteral("tail_seconds must be between 0 and 30.")};
    return std::nullopt;
}

std::optional<RenderService::Failure> RenderService::resolveRange(const RenderRequest& r, Range& range) const
{
    auto* edit = editMgr_.edit();
    if (!edit) return Failure{AiErrorCode::Conflict, QStringLiteral("No project is loaded.")};

    const bool beats = r.startBeat || r.endBeat;
    const bool seconds = r.startSeconds || r.endSeconds;
    if (beats && seconds)
        return Failure{AiErrorCode::InvalidArgument,
                       QStringLiteral("Give the range in beats or in seconds, not both.")};
    for (const auto& v : { r.startBeat, r.endBeat, r.startSeconds, r.endSeconds })
        if (v && (!std::isfinite(*v) || *v < 0.0))
            return Failure{AiErrorCode::InvalidArgument, QStringLiteral("Range values must be 0 or more.")};

    // Tracktion beats, like every other tool's beat positions: they follow
    // the time signature's denominator (an eighth note in x/8), not always
    // quarter notes.
    auto& tempo = edit->tempoSequence;
    auto beatToSeconds = [&tempo](double b) {
        return tempo.toTime(tracktion::BeatPosition::fromBeats(b)).inSeconds();
    };
    auto secondsToBeats = [&tempo](double s) {
        return tempo.toBeats(tracktion::TimePosition::fromSeconds(s)).inBeats();
    };

    const double songEnd = edit->getLength().inSeconds();
    double start = 0.0, end = songEnd;
    if (beats) {
        if (r.startBeat) start = beatToSeconds(*r.startBeat);
        if (r.endBeat) end = beatToSeconds(*r.endBeat);
    } else {
        if (r.startSeconds) start = *r.startSeconds;
        if (r.endSeconds) end = *r.endSeconds;
    }

    if (!r.endBeat && !r.endSeconds && songEnd <= 0.0)
        return Failure{AiErrorCode::InvalidArgument,
                       QStringLiteral("The song is empty; add clips or give an explicit range end.")};
    if (!(end > start))
        return Failure{AiErrorCode::InvalidArgument,
                       QStringLiteral("The range end (%1 s) must be after its start (%2 s).")
                           .arg(end, 0, 'f', 3).arg(start, 0, 'f', 3)};
    if (end - start > kMaxRenderSeconds)
        return Failure{AiErrorCode::InvalidArgument,
                       QStringLiteral("The range is longer than %1 seconds.").arg(kMaxRenderSeconds)};

    range.startSeconds = start;
    range.endSeconds = end;
    range.startBeat = beats && r.startBeat ? *r.startBeat : secondsToBeats(start);
    range.endBeat = beats && r.endBeat ? *r.endBeat : secondsToBeats(end);
    return std::nullopt;
}

bool RenderService::reachesMainMix(te::AudioTrack* track) const
{
    for (int guard = 0; track && guard < 64; ++guard) {
        if (editMgr_.isTrackOutputDisconnected(track) || !editMgr_.getTrackDirectOutputID(track).isEmpty())
            return false;
        auto* dest = editMgr_.getTrackOutputDestination(track);
        if (!dest) return true;
        track = dest;
    }
    return false;
}

std::optional<RenderService::Failure> RenderService::resolveSources(const QVector<te::EditItemID>& tracks,
                                                                     std::vector<te::EditItemID>& sources,
                                                                     QStringList& warnings) const
{
    auto add = [&sources](te::EditItemID id) {
        if (std::find(sources.begin(), sources.end(), id) == sources.end())
            sources.push_back(id);
    };
    for (const auto& id : tracks) {
        auto* track = editMgr_.findAudioTrack(id);
        if (!track)
            return Failure{AiErrorCode::NotFound,
                           QStringLiteral("No track with id %1.").arg(ids::make(ids::kTrack, id))};
        const QString ref = ids::of(track);
        if (editMgr_.isBusTrack(track)) {
            const auto feeders = editMgr_.busSources(track);
            if (feeders.empty())
                warnings << QStringLiteral("%1 is a bus with no unmuted tracks feeding it.").arg(ref);
            for (const auto& f : feeders) add(f);
            if (!reachesMainMix(track))
                warnings << QStringLiteral("%1 does not reach the main mix (disconnected or sent to another "
                                           "output), so it renders silent.").arg(ref);
        } else {
            if (track->isMuted(false))
                warnings << QStringLiteral("%1 is muted in the project; it is unmuted for this render.").arg(ref);
            if (!reachesMainMix(track))
                warnings << QStringLiteral("%1 does not reach the main mix (disconnected or sent to another "
                                           "output), so it renders silent.").arg(ref);
            add(track->itemID);
        }
    }
    if (sources.empty())
        return Failure{AiErrorCode::Conflict,
                       QStringLiteral("None of the requested tracks has audio that reaches the mix.")};
    return std::nullopt;
}

QJsonObject RenderService::rangeJson(const Range& range) const
{
    return QJsonObject{{"start_seconds", round3(range.startSeconds)},
                       {"end_seconds", round3(range.endSeconds)},
                       {"start_beat", round3(range.startBeat)},
                       {"end_beat", round3(range.endBeat)}};
}

RenderResult RenderService::renderTo(const RenderRequest& request, const Range& range,
                                     const EditManager::Stem* stem, const juce::File& dest)
{
    RenderResult result;
    result.path = toQString(dest.getFullPathName());

    ExportSettings s;
    s.destFile = dest;
    s.sampleRate = request.sampleRate;
    s.bitDepth = request.bitDepth;
    s.format = ExportFormat::WAV;
    s.normalize = false;
    s.dither = true;   // only applied at 16 bit
    s.useMasterPlugins = request.includeMasterEffects;
    s.range = tracktion::TimeRange(tracktion::TimePosition::fromSeconds(range.startSeconds),
                                   tracktion::TimePosition::fromSeconds(range.endSeconds));
    s.tailSeconds = request.tailSeconds;
    // A part of the mix may be silent over the range; that is still a result.
    s.stems = stem ? StemMode::Tracks : StemMode::None;

    // The caller (renderRegion / exportStems) holds the render claim and a
    // render session on this (the GUI) thread.
    s.renderClaimed = true;

    bool ok = false;
    auto outcome = EditManager::ExportOutcome::Failed;
    {
        StemMuteGuard mutes { editMgr_ };
        if (stem) mutes.apply(*stem);
        // Synchronous on the message thread: Tracktion's callBlocking runs inline.
        ok = editMgr_.exportMix(s, nullptr, &outcome);
    }

    if (outcome == EditManager::ExportOutcome::Busy) {
        result.errorCode = AiErrorCode::Busy;
        result.error = QStringLiteral("Another render or export is running; try again when it has finished.");
        return result;
    }
    if (!ok || !dest.existsAsFile()) {
        result.errorCode = AiErrorCode::Failed;
        const QString why = editMgr_.lastExportError();
        result.error = QStringLiteral("The render failed: %1.")
                           .arg(why.isEmpty() ? QStringLiteral("no file was written") : why);
        dest.deleteFile();
        return result;
    }

    QJsonObject info;
    info["path"] = result.path;
    info["sample_rate"] = request.sampleRate;
    info["bit_depth"] = request.bitDepth;
    info["range"] = rangeJson(range);
    info["include_master_effects"] = request.includeMasterEffects;
    info["tail_seconds"] = request.tailSeconds;

    if (request.analyze) {
        QString error;
        auto analysis = AudioFileAnalyzer::analyze(dest, {}, &error);
        if (!analysis.isEmpty()) {
            info["duration_seconds"] = analysis.value("duration_seconds");
            info["analysis"] = analysis;
        } else {
            info["duration_seconds"] = round3(fileDurationSeconds(dest));
            info["analysis_error"] = error;
        }
    } else {
        info["duration_seconds"] = round3(fileDurationSeconds(dest));
    }

    result.ok = true;
    result.info = info;
    return result;
}

RenderResult RenderService::renderRegion(const RenderRequest& request)
{
    RenderResult result;
    auto fail = [&result](const Failure& f) {
        result.ok = false;
        result.errorCode = f.code;
        result.error = f.message;
        return result;
    };

    if (auto f = checkReady()) return fail(*f);
    if (auto f = checkFormat(request)) return fail(*f);
    Range range;
    if (auto f = resolveRange(request, range)) return fail(*f);

    std::vector<te::EditItemID> sources;
    QStringList warnings;
    if (!request.tracks.isEmpty())
        if (auto f = resolveSources(request.tracks, sources, warnings)) return fail(*f);

    // Output file.
    juce::File dest;
    if (request.outputPath.trimmed().isEmpty()) {
        dest = toJuceFile(defaultRenderDir())
                   .getNonexistentChildFile(juce::String::fromUTF8((QStringLiteral("render-") + timestamp()).toUtf8().constData()),
                                            ".wav", false);
    } else {
        const QString path = absoluteInRenderDir(request.outputPath.trimmed());
        if (!path.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive))
            return fail({AiErrorCode::InvalidArgument, QStringLiteral("output_path must end in .wav.")});
        if (QFileInfo(path).isDir())
            return fail({AiErrorCode::InvalidArgument, QStringLiteral("output_path is a folder.")});
        if (QFileInfo::exists(path) && !isInsideDir(path, defaultRenderDir()))
            return fail({AiErrorCode::Conflict,
                         QStringLiteral("%1 already exists; choose a new file name (files are only "
                                        "overwritten inside %2).").arg(path, defaultRenderDir())});
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return fail({AiErrorCode::Failed, QStringLiteral("Couldn't create the folder for %1.").arg(path)});
        dest = toJuceFile(path);
        if (dest.existsAsFile() && !dest.deleteFile())
            return fail({AiErrorCode::Failed, QStringLiteral("Couldn't replace %1.").arg(path)});
    }

    std::optional<EditManager::Stem> stem;
    if (!sources.empty())
        stem = EditManager::Stem{QStringLiteral("selection"), sources};

    // Claimed here on the GUI thread before anything renders, so a worker
    // export starting meanwhile sees it (and this sees theirs); the live
    // playback graph is freed for the whole render.
    const EditManager::ScopedRenderClaim claim(editMgr_);
    if (!claim.claimed())
        return fail({AiErrorCode::Busy, QStringLiteral("Another render or export is running; try again when it has finished.")});
    const EditManager::ScopedRenderSession session(editMgr_);

    result = renderTo(request, range, stem ? &*stem : nullptr, dest);
    if (!result.ok) return result;

    if (request.tracks.isEmpty()) {
        result.info["tracks"] = QStringLiteral("mix");
    } else {
        QJsonArray requested, heard;
        for (const auto& id : request.tracks) requested.append(ids::make(ids::kTrack, id));
        for (const auto& id : sources) heard.append(ids::make(ids::kTrack, id));
        result.info["tracks"] = requested;
        result.info["sources"] = heard;
    }
    if (!warnings.isEmpty())
        result.info["render_warnings"] = QJsonArray::fromStringList(warnings);
    return result;
}

QVector<RenderResult> RenderService::exportStems(const StemsRequest& request, QString* error,
                                                 QString* errorCode)
{
    auto fail = [error, errorCode](const Failure& f) {
        if (error) *error = f.message;
        if (errorCode) *errorCode = f.code;
        return QVector<RenderResult>{};
    };
    if (error) error->clear();
    if (errorCode) errorCode->clear();

    if (request.mode == StemMode::None)
        return fail({AiErrorCode::InvalidArgument, QStringLiteral("Stem mode must be tracks or groups.")});
    if (auto f = checkReady()) return fail(*f);
    if (auto f = checkFormat(request.base)) return fail(*f);
    Range range;
    if (auto f = resolveRange(request.base, range)) return fail(*f);

    const auto plan = editMgr_.planStems(request.mode);
    if (plan.empty())
        return fail({AiErrorCode::Conflict,
                     QStringLiteral("There are no unmuted tracks that reach the mix, so there is nothing to export.")});

    QString folder = request.folder.trimmed().isEmpty()
        ? QDir(defaultRenderDir()).filePath(QStringLiteral("stems-") + timestamp())
        : absoluteInRenderDir(request.folder.trimmed());
    folder = QDir::cleanPath(folder);
    if (QFileInfo(folder).exists() && !QFileInfo(folder).isDir())
        return fail({AiErrorCode::InvalidArgument, QStringLiteral("%1 is a file, not a folder.").arg(folder)});
    if (!QDir().mkpath(folder))
        return fail({AiErrorCode::Failed, QStringLiteral("Couldn't create the folder %1.").arg(folder)});
    const bool mayOverwrite = isInsideDir(QDir(folder).filePath(QStringLiteral("x.wav")), defaultRenderDir());
    const auto folderFile = toJuceFile(folder);

    // Claimed here on the GUI thread before anything renders, so a worker
    // export starting meanwhile sees it (and this sees theirs); the live
    // playback graph is freed for the whole render.
    const EditManager::ScopedRenderClaim claim(editMgr_);
    if (!claim.claimed())
        return fail({AiErrorCode::Busy, QStringLiteral("Another render or export is running; try again when it has finished.")});
    const EditManager::ScopedRenderSession session(editMgr_);

    QVector<RenderResult> results;
    for (size_t i = 0; i < plan.size(); ++i) {
        const auto& stem = plan[i];
        const auto baseName = EditManager::sanitizeForFilename(juce::String::fromUTF8(
            QStringLiteral("%1 - %2").arg(int(i + 1), 2, 10, QLatin1Char('0')).arg(stem.name).toUtf8().constData()));
        auto dest = folderFile.getChildFile(baseName + ".wav");
        if (dest.existsAsFile()) {
            if (mayOverwrite) dest.deleteFile();
            else dest = folderFile.getNonexistentChildFile(baseName, ".wav", false);
        }

        auto r = renderTo(request.base, range, &stem, dest);
        QJsonArray sources;
        for (const auto& id : stem.sources) sources.append(ids::make(ids::kTrack, id));
        r.info["name"] = stem.name;
        r.info["sources"] = sources;
        results.push_back(r);
    }
    return results;
}

// ── Tools ───────────────────────────────────────────────────────────────────

QVector<AiToolExtension> RenderService::toolExtensions(AiToolExecutor& executor)
{
    QVector<AiToolExtension> tools;
    AiToolFlags flags;
    flags.mutating = false;
    flags.destructive = false;
    flags.batchable = false;

    // render_region
    {
        QJsonObject props;
        addRangeProps(props);
        props["tracks"] = QJsonObject{
            {"type", "array"},
            {"description", "Only these tracks are heard (ids like 'track:1043' preferred). A bus stands "
                            "for the unmuted tracks feeding it. Buses and effect returns keep processing, "
                            "so the file is exactly these tracks' share of the mix. Omit for the full mix "
                            "as it sounds now (current mutes and solos apply)."},
            {"items", trackRefSchema()},
            {"minItems", 1}};
        props["include_master_effects"] = prop("boolean", "Run the Master track's effects (EQ, limiter...). "
                                                          "Default true.");
        addFormatProps(props);
        props["output_path"] = prop("string", "Where to write the .wav. Relative paths go in the render "
                                              "folder. Default: a new file in the render folder. Existing "
                                              "files outside the render folder are never overwritten.");
        props["analyze"] = prop("boolean", "Measure the file and return the analysis. Default true.");

        AiToolExtension t;
        t.definition = makeTool("render_region",
            "Render part of the song offline (faster than real time, no playback needed) to a WAV file "
            "and measure it, so you can hear the result of your edits as numbers. Use it in a loop: make "
            "a change, render_region the affected bars (only the tracks you touched, or the full mix), "
            "read 'analysis' (integrated_lufs, peak_dbfs, true_peak_dbtp, rms_dbfs, crest_factor_db, "
            "stereo correlation/width, 7-band spectrum energy, onsets, warnings), adjust, and render "
            "again. Keep the same range between renders and use compare_audio_files on the two paths "
            "to see what changed. Ranges in beats follow the tempo map. Add tail_seconds when the "
            "range ends on reverb or delay. Returns {path, duration_seconds, sample_rate, bit_depth, "
            "range:{start_seconds, end_seconds, start_beat, end_beat}, tracks: [ids] or \"mix\", "
            "analysis}. Fails with BUSY while another render or a recording runs.",
            props);
        t.flags = flags;
        t.handler = [this, &executor](const QJsonObject& in, const QString& id) -> AiToolResult {
            RenderRequest r;
            readCommon(in, r);
            if (in.contains("include_master_effects"))
                r.includeMasterEffects = in["include_master_effects"].toBool(true);
            r.outputPath = in["output_path"].toString();
            for (const QJsonValue ref : in["tracks"].toArray()) {
                auto* track = executor.resolveTrack(ref);
                if (!track)
                    return aiErr(id, AiErrorCode::NotFound,
                                 QStringLiteral("Track not found: %1. Use get_track_list for ids.")
                                     .arg(ref.isString() ? ref.toString()
                                                         : QString::fromUtf8(QJsonDocument(QJsonArray{ref})
                                                                                 .toJson(QJsonDocument::Compact))),
                                 QJsonObject{{"track", ref}});
                if (!r.tracks.contains(track->itemID)) r.tracks.push_back(track->itemID);
            }
            const auto result = renderRegion(r);
            if (!result.ok) return aiErr(id, result.errorCode, result.error);
            return aiOk(id, result.info);
        };
        tools.push_back(t);
    }

    // export_stems
    {
        QJsonObject props;
        props["mode"] = QJsonObject{
            {"type", "string"},
            {"description", "'tracks': one file per unmuted track. 'groups': one file per bus with the "
                            "tracks routed into it; tracks going straight to Master get their own file."},
            {"enum", QJsonArray{"tracks", "groups"}}};
        props["folder"] = prop("string", "Folder for the stem files (created if needed; relative paths go in "
                                         "the render folder). Default: a new folder in the render folder.");
        addRangeProps(props);
        addFormatProps(props);
        props["include_master_effects"] = prop("boolean", "Run the Master track's effects on each stem. "
                                                          "Default false, so the stems add up to the mix "
                                                          "before mastering.");
        props["analyze"] = prop("boolean", "Measure each stem. Default true.");

        AiToolExtension t;
        t.definition = makeTool("export_stems",
            "Render every audible track (mode 'tracks') or every bus group (mode 'groups') to its own WAV "
            "over the same range, with bus processing and effect returns kept so the stems add up to "
            "the mix. Each stem comes with its analysis, which shows which part is loudest, brightest, "
            "clipping or out of phase, e.g. to find what masks the vocal in the 2-6 kHz band or which "
            "track carries the sub energy. Returns {folder, stems:[{name, path, sources:[track ids], "
            "analysis}]}. Takes one render per stem, so use a short range for quick checks.",
            props, QJsonArray{"mode"});
        t.flags = flags;
        t.handler = [this](const QJsonObject& in, const QString& id) -> AiToolResult {
            StemsRequest req;
            req.mode = in["mode"].toString() == QLatin1String("groups") ? StemMode::Groups : StemMode::Tracks;
            req.folder = in["folder"].toString();
            req.base.includeMasterEffects = in["include_master_effects"].toBool(false);
            readCommon(in, req.base);

            QString error, code;
            const auto results = exportStems(req, &error, &code);
            if (results.isEmpty())
                return aiErr(id, code.isEmpty() ? AiErrorCode::Failed : code, error);

            QJsonArray stems;
            int failed = 0;
            QString folder;
            QJsonObject range;
            for (const auto& r : results) {
                QJsonObject s;
                s["name"] = r.info.value("name");
                s["path"] = r.path;
                s["sources"] = r.info.value("sources");
                if (r.ok) {
                    s["duration_seconds"] = r.info.value("duration_seconds");
                    if (r.info.contains("analysis")) s["analysis"] = r.info.value("analysis");
                    if (range.isEmpty()) range = r.info.value("range").toObject();
                } else {
                    ++failed;
                    s.remove("path");
                    s["error"] = r.error;
                    s["error_code"] = r.errorCode;
                }
                if (folder.isEmpty()) folder = QFileInfo(r.path).absolutePath();
                stems.append(s);
            }
            if (failed == results.size())
                return aiErr(id, AiErrorCode::Failed,
                             QStringLiteral("No stem could be rendered: %1").arg(results.front().error),
                             QJsonObject{{"stems", stems}});

            QJsonObject out;
            out["folder"] = folder;
            out["mode"] = in["mode"].toString();
            out["range"] = range;
            out["sample_rate"] = req.base.sampleRate;
            out["bit_depth"] = req.base.bitDepth;
            out["include_master_effects"] = req.base.includeMasterEffects;
            out["stems"] = stems;
            out["failed"] = failed;
            return aiOk(id, out);
        };
        tools.push_back(t);
    }

    // analyze_audio_file
    {
        QJsonObject props;
        props["path"] = prop("string", "Audio file to measure (WAV, FLAC, OGG or AIFF), e.g. a path returned "
                                       "by render_region. Relative paths are looked up in the render folder.");
        props["spectrum"] = prop("boolean", "Include the 7-band spectrum, centroid and roll-off. Default true.");
        props["onsets"] = prop("boolean", "Include onset (note/hit) detection. Default true.");

        AiToolExtension t;
        t.definition = makeTool("analyze_audio_file",
            "Measure an audio file offline: duration, peak_dbfs, true_peak_dbtp, rms_dbfs, "
            "crest_factor_db, integrated_lufs, loudness_range_lu and max_short_term_lufs "
            "(ITU-R BS.1770 / EBU R128), dc_offset per channel, clipped_samples, silence_ratio, "
            "stereo {correlation -1..1, width = side/mid energy, balance_db = L-R}, spectrum {bands sub "
            "20-60, bass 60-250, low_mid 250-500, mid 500-2k, high_mid 2k-6k, presence 6k-12k, air "
            "12k-20k Hz with energy_db (dBFS) and relative_db (share of the total), centroid_hz, "
            "rolloff_85_hz}, onsets {count, per_second, times_seconds} and plain-language warnings. "
            "Levels bottom out at -120 dB for silence. Use it on imported audio or on earlier renders.",
            props, QJsonArray{"path"});
        t.flags = flags;
        t.handler = [](const QJsonObject& in, const QString& id) -> AiToolResult {
            const QString path = absoluteInRenderDir(in["path"].toString());
            if (!QFileInfo(path).isFile())
                return aiErr(id, AiErrorCode::NotFound, QStringLiteral("No file at %1.").arg(path));
            AudioAnalysisOptions options;
            options.spectrum = in["spectrum"].toBool(true);
            options.onsets = in["onsets"].toBool(true);
            QString error;
            const auto analysis = AudioFileAnalyzer::analyze(toJuceFile(path), options, &error);
            if (analysis.isEmpty())
                return aiErr(id, AiErrorCode::InvalidArgument,
                             QStringLiteral("Couldn't analyze %1: %2.").arg(path, error));
            return aiOk(id, analysis);
        };
        tools.push_back(t);
    }

    // compare_audio_files
    {
        QJsonObject props;
        props["path_a"] = prop("string", "The reference file, usually the render from before your change.");
        props["path_b"] = prop("string", "The file to compare, usually the render after your change.");

        AiToolExtension t;
        t.definition = makeTool("compare_audio_files",
            "Compare two audio files, typically two render_region results over the same range before "
            "and after a change, to check whether the change helped. Returns {a:{summary}, b:{summary}, "
            "delta:{integrated_lufs, peak_dbfs, true_peak_dbtp, rms_dbfs, crest_factor_db, "
            "loudness_range_lu, stereo_correlation, stereo_width, centroid_hz, band_energy_db:{band: "
            "dB}, band_relative_db:{band: dB}}, changes:[plain-language notes]}. Every delta is b minus "
            "a: positive means b is louder, brighter or wider. band_relative_db ignores the overall "
            "level change, so it shows tonal balance shifts on their own.",
            props, QJsonArray{"path_a", "path_b"});
        t.flags = flags;
        t.handler = [](const QJsonObject& in, const QString& id) -> AiToolResult {
            AudioAnalysisOptions options;
            options.onsets = false;
            QJsonObject analyses[2];
            const QString keys[2] = { QStringLiteral("path_a"), QStringLiteral("path_b") };
            for (int i = 0; i < 2; ++i) {
                const QString path = absoluteInRenderDir(in[keys[i]].toString());
                if (!QFileInfo(path).isFile())
                    return aiErr(id, AiErrorCode::NotFound, QStringLiteral("No file at %1 (%2).").arg(path, keys[i]));
                QString error;
                analyses[i] = AudioFileAnalyzer::analyze(toJuceFile(path), options, &error);
                if (analyses[i].isEmpty())
                    return aiErr(id, AiErrorCode::InvalidArgument,
                                 QStringLiteral("Couldn't analyze %1: %2.").arg(path, error));
                analyses[i]["path"] = path;
            }
            const auto a = summaryOf(analyses[0]);
            const auto b = summaryOf(analyses[1]);

            QJsonObject d;
            for (const char* key : { "duration_seconds", "integrated_lufs", "peak_dbfs", "true_peak_dbtp",
                                     "rms_dbfs", "crest_factor_db", "loudness_range_lu", "max_short_term_lufs",
                                     "stereo_correlation", "stereo_width", "stereo_balance_db",
                                     "centroid_hz", "rolloff_85_hz", "silence_ratio" })
                d[QLatin1String(key)] = delta(a, b, QLatin1String(key));
            d["clipped_samples"] = b.value("clipped_samples").toDouble() - a.value("clipped_samples").toDouble();

            QJsonObject bandAbs, bandRel;
            QStringList changes;
            const auto bandsA = analyses[0].value("spectrum").toObject().value("bands").toArray();
            const auto bandsB = analyses[1].value("spectrum").toObject().value("bands").toArray();
            for (int i = 0; i < bandsA.size() && i < bandsB.size(); ++i) {
                const auto ba = bandsA[i].toObject(), bb = bandsB[i].toObject();
                const QString name = ba.value("name").toString();
                bandAbs[name] = round3(bb.value("energy_db").toDouble() - ba.value("energy_db").toDouble());
                const double rel = round3(bb.value("relative_db").toDouble() - ba.value("relative_db").toDouble());
                bandRel[name] = rel;
                if (std::abs(rel) >= 1.0)
                    changes << QStringLiteral("%1 band %2 by %3 dB relative to the whole spectrum")
                                   .arg(name, rel > 0 ? QStringLiteral("up") : QStringLiteral("down"))
                                   .arg(std::abs(rel), 0, 'f', 1);
            }
            d["band_energy_db"] = bandAbs;
            d["band_relative_db"] = bandRel;

            auto note = [&changes, &d](const char* key, double threshold, const QString& up,
                                       const QString& down, const QString& unit) {
                const double v = d.value(QLatin1String(key)).toDouble();
                if (std::abs(v) >= threshold)
                    changes << QStringLiteral("%1 %2 by %3 %4").arg(QString::fromLatin1(key), v > 0 ? up : down)
                                   .arg(std::abs(v), 0, 'f', 2).arg(unit);
            };
            note("integrated_lufs", 0.3, QStringLiteral("up (b is louder)"), QStringLiteral("down (b is quieter)"), QStringLiteral("LU"));
            note("true_peak_dbtp", 0.3, QStringLiteral("up"), QStringLiteral("down"), QStringLiteral("dB"));
            note("crest_factor_db", 0.5, QStringLiteral("up (more dynamic)"), QStringLiteral("down (more compressed)"), QStringLiteral("dB"));
            note("loudness_range_lu", 0.5, QStringLiteral("up"), QStringLiteral("down"), QStringLiteral("LU"));
            note("stereo_width", 0.05, QStringLiteral("up (wider)"), QStringLiteral("down (narrower)"), QString());
            note("stereo_correlation", 0.1, QStringLiteral("up (more mono-compatible)"), QStringLiteral("down (less mono-compatible)"), QString());
            note("centroid_hz", 100.0, QStringLiteral("up (brighter)"), QStringLiteral("down (darker)"), QStringLiteral("Hz"));
            if (d.value("clipped_samples").toDouble() > 0)
                changes << QStringLiteral("b has more clipped samples");
            if (std::abs(d.value("duration_seconds").toDouble()) > 0.05)
                changes << QStringLiteral("the files differ in length; compare renders of the same range");
            if (changes.isEmpty())
                changes << QStringLiteral("no notable difference");

            QJsonObject out;
            out["a"] = a;
            out["b"] = b;
            out["delta"] = d;
            out["changes"] = QJsonArray::fromStringList(changes);
            out["path_a"] = analyses[0].value("path");
            out["path_b"] = analyses[1].value("path");
            return aiOk(id, out);
        };
        tools.push_back(t);
    }

    return tools;
}

} // namespace OpenDaw
