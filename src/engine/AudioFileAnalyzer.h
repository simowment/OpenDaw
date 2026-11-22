#pragma once

#include <tracktion_engine/tracktion_engine.h>
#include <QJsonObject>
#include <QString>

namespace OpenDaw {

struct AudioAnalysisOptions {
    bool spectrum = true;      // 7-band energy, spectral centroid and roll-off
    bool onsets = true;        // spectral-flux onset detection
    double maxSeconds = 600.0; // only the first maxSeconds are measured (<= 0: no limit)
};

// Offline measurement of rendered audio, for agents checking their changes:
// levels (peak, true peak, RMS, crest factor), loudness to ITU-R BS.1770 /
// EBU R128 (integrated, LRA, max short-term), DC offset, clipping, silence,
// stereo correlation/width/balance, a 7-band spectrum and onsets.
//
// Pure computation: callable from any thread, no GUI. Every number in the
// result is finite; levels in dB bottom out at -120 for silence.
//
// Keys: duration_seconds, analyzed_seconds, sample_rate, channels, peak_dbfs,
// true_peak_dbtp, rms_dbfs, crest_factor_db, integrated_lufs,
// loudness_range_lu, max_short_term_lufs, max_momentary_lufs, dc_offset[],
// clipped_samples, silence_ratio, stereo{correlation, width, balance_db, mono},
// spectrum{bands[{name, low_hz, high_hz, energy_db, relative_db}],
// centroid_hz, rolloff_85_hz}, onsets{count, per_second, times_seconds},
// warnings[].
class AudioFileAnalyzer {
public:
    // WAV, FLAC, OGG and AIFF (JUCE's basic formats). Returns an empty object
    // and sets *error when the file is missing or can't be decoded.
    static QJsonObject analyze(const juce::File& file,
                               const AudioAnalysisOptions& options = {},
                               QString* error = nullptr);

    static QJsonObject analyzeBuffer(const juce::AudioBuffer<float>& buffer,
                                     double sampleRate,
                                     const AudioAnalysisOptions& options = {});
};

} // namespace OpenDaw
