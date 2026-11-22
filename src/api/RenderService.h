#pragma once

// Agent-facing offline rendering: render a region of the song (the mix or
// only some tracks) to a WAV file, export stems, and measure audio files, so
// an agent can hear the effect of its edits as numbers:
//
//     change something -> render_region -> read "analysis" -> adjust
//
// Everything runs synchronously on the Qt GUI thread (= the JUCE message
// thread), where Tracktion builds its render graph inline. The UI does not
// repaint while a render runs. Each render claims EditManager's render flag
// (tryClaimRender) before it starts, so it fails with BUSY instead of racing
// a UI export on a worker thread, and holds a render session (the live
// playback graph is freed while the offline graph uses the plugins).

#include "ai/AiToolExtensions.h"
#include "engine/AudioEngine.h"
#include "engine/EditManager.h"
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

namespace OpenDaw {

class AiToolExecutor;

struct RenderRequest {
    // Range: beats or seconds, not both. Missing start: 0; missing end: the
    // end of the song.
    std::optional<double> startBeat, endBeat, startSeconds, endSeconds;
    // Empty: the whole mix as it sounds now (mutes and solos apply). Otherwise
    // only these tracks are heard; a bus stands for the unmuted tracks that
    // feed it. Buses and effect returns keep processing, so the file is these
    // tracks' share of the mix.
    QVector<te::EditItemID> tracks;
    bool includeMasterEffects = true;
    double sampleRate = 48000;   // 44100, 48000, 88200 or 96000
    int bitDepth = 24;           // 16, 24 or 32 (float)
    double tailSeconds = 0;      // 0..30, extra time for reverb/delay tails
    QString outputPath;          // .wav; empty: a new file in defaultRenderDir()
    bool analyze = true;         // add AudioFileAnalyzer's result as info["analysis"]
};

struct RenderResult {
    bool ok = false;
    QString errorCode;   // AiErrorCode::* when !ok
    QString error;
    QString path;
    QJsonObject info;
};

struct StemsRequest {
    StemMode mode = StemMode::Tracks;   // Tracks or Groups
    RenderRequest base;                 // range, format, analyze; tracks and outputPath are ignored
    QString folder;                     // empty: a new folder in defaultRenderDir()
};

class RenderService : public QObject {
    Q_OBJECT

public:
    explicit RenderService(EditManager& editMgr, QObject* parent = nullptr);

    // render_region, export_stems, analyze_audio_file, compare_audio_files.
    // The executor must outlive the returned handlers.
    QVector<AiToolExtension> toolExtensions(AiToolExecutor& executor);

    RenderResult renderRegion(const RenderRequest& request);

    // One result per planned stem (failed stems have ok == false). Returns an
    // empty vector and sets *error / *errorCode when nothing could start.
    QVector<RenderResult> exportStems(const StemsRequest& request, QString* error = nullptr,
                                      QString* errorCode = nullptr);

    // <temp>/OpenDaw/agent-renders, created on demand.
    static QString defaultRenderDir();

private:
    struct Range {
        double startSeconds = 0, endSeconds = 0, startBeat = 0, endBeat = 0;
    };
    struct Failure {
        QString code, message;
    };

    std::optional<Failure> checkReady() const;
    std::optional<Failure> checkFormat(const RenderRequest& request) const;
    std::optional<Failure> resolveRange(const RenderRequest& request, Range& range) const;
    // The non-bus tracks to leave audible for the requested tracks.
    std::optional<Failure> resolveSources(const QVector<te::EditItemID>& tracks,
                                          std::vector<te::EditItemID>& sources,
                                          QStringList& warnings) const;
    bool reachesMainMix(te::AudioTrack* track) const;
    // Renders `range` to `dest`; with a stem, only its sources are heard.
    RenderResult renderTo(const RenderRequest& request, const Range& range,
                          const EditManager::Stem* stem, const juce::File& dest);
    QJsonObject rangeJson(const Range& range) const;
    static void pruneOldRenders();

    EditManager& editMgr_;
};

} // namespace OpenDaw
