#include "AudioFileAnalyzer.h"
#include "LoudnessMeter.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <vector>

namespace OpenDaw {

namespace {

constexpr double kFloorDb = -120.0;
constexpr int kMaxChannels = 32;
constexpr int kReadBlock = 8192;   // LoudnessMeter keeps up to 25 s of steps between drains

double finiteOr(double v, double fallback) { return std::isfinite(v) ? v : fallback; }

double roundTo(double v, int decimals)
{
    if (!std::isfinite(v)) return 0.0;
    const double scale = std::pow(10.0, decimals);
    return std::round(v * scale) / scale;
}

double ampToDb(double amplitude)
{
    if (!std::isfinite(amplitude) || amplitude <= 1e-6) return kFloorDb;
    return std::max(kFloorDb, 20.0 * std::log10(amplitude));
}

double powerToDb(double power)
{
    if (!std::isfinite(power) || power <= 1e-12) return kFloorDb;
    return std::max(kFloorDb, 10.0 * std::log10(power));
}

// LoudnessMeter reports silence as kSilence (-100); the analysis uses -120.
double loudnessOut(double v)
{
    if (!std::isfinite(v) || v <= LoudnessMeter::kSilence) return kFloorDb;
    return v;
}

// ── Radix-2 FFT ─────────────────────────────────────────────────────────────

class Fft {
public:
    explicit Fft(int size) : size_(size), twiddles_(size_t(size / 2)), reversed_(size_t(size))
    {
        int bits = 0;
        while ((1 << bits) < size) ++bits;
        for (int i = 0; i < size; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            reversed_[size_t(i)] = r;
        }
        const double pi = juce::MathConstants<double>::pi;
        for (int k = 0; k < size / 2; ++k)
            twiddles_[size_t(k)] = std::polar(1.0, -2.0 * pi * k / size);
    }

    int size() const { return size_; }

    void perform(std::vector<std::complex<double>>& x) const
    {
        const int n = size_;
        for (int i = 0; i < n; ++i) {
            const int r = reversed_[size_t(i)];
            if (i < r) std::swap(x[size_t(i)], x[size_t(r)]);
        }
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len / 2;
            const int step = n / len;
            for (int i = 0; i < n; i += len) {
                for (int j = 0; j < half; ++j) {
                    const auto u = x[size_t(i + j)];
                    const auto v = x[size_t(i + j + half)] * twiddles_[size_t(j * step)];
                    x[size_t(i + j)] = u + v;
                    x[size_t(i + j + half)] = u - v;
                }
            }
        }
    }

private:
    int size_;
    std::vector<std::complex<double>> twiddles_;
    std::vector<int> reversed_;
};

std::vector<double> hannWindow(int size)
{
    std::vector<double> w(static_cast<size_t>(size));
    const double pi = juce::MathConstants<double>::pi;
    for (int i = 0; i < size; ++i)
        w[size_t(i)] = 0.5 - 0.5 * std::cos(2.0 * pi * i / size);   // periodic Hann
    return w;
}

// Collects a mono signal into overlapping frames and hands each windowed,
// transformed frame to the subclass.
class FrameAnalyzer {
public:
    FrameAnalyzer(int size, int hop) : fft_(size), hop_(hop), window_(hannWindow(size)),
                                       spectrum_(size_t(size))
    {
        pending_.reserve(size_t(size + hop));
        for (double w : window_) { windowSum_ += w; windowSumSq_ += w * w; }
    }
    virtual ~FrameAnalyzer() = default;

    void push(double sample)
    {
        pending_.push_back(sample);
        if (int(pending_.size()) == fft_.size()) {
            runFrame(1.0);
            pending_.erase(pending_.begin(), pending_.begin() + hop_);
        }
    }

    int frames() const { return frames_; }

protected:
    virtual void frame(const std::vector<std::complex<double>>& spectrum,
                       const std::vector<double>& samples, double gain) = 0;

    // A signal shorter than one frame: zero-pad it once so it still counts.
    void flushPartial()
    {
        if (frames_ > 0 || pending_.empty()) return;
        const double gain = double(fft_.size()) / double(pending_.size());
        pending_.resize(size_t(fft_.size()), 0.0);
        runFrame(gain);
        pending_.clear();
    }

    int size() const { return fft_.size(); }
    int hop() const { return hop_; }
    double windowSum() const { return windowSum_; }
    double windowSumSq() const { return windowSumSq_; }

private:
    void runFrame(double gain)
    {
        for (size_t i = 0; i < pending_.size(); ++i)
            spectrum_[i] = { pending_[i] * window_[i], 0.0 };
        fft_.perform(spectrum_);
        frame(spectrum_, pending_, gain);
        ++frames_;
    }

    Fft fft_;
    int hop_;
    std::vector<double> window_;
    std::vector<double> pending_;
    std::vector<std::complex<double>> spectrum_;
    double windowSum_ = 0.0;
    double windowSumSq_ = 0.0;
    int frames_ = 0;
};

// Averaged power spectrum (4096-point Hann, 50% overlap).
class SpectrumAnalyzer : public FrameAnalyzer {
public:
    SpectrumAnalyzer() : FrameAnalyzer(4096, 2048), power_(2049, 0.0) {}

    QJsonObject result(double sampleRate)
    {
        flushPartial();
        const int n = size();
        const int bins = n / 2;
        // Mean-square contribution of each bin (Parseval, one-sided), so the
        // bins add up to the signal's mean square and a band's energy reads
        // in dBFS like an RMS level.
        std::vector<double> ms(size_t(bins + 1), 0.0);
        if (frames() > 0) {
            const double norm = double(n) * windowSumSq() * double(frames());
            for (int k = 0; k <= bins; ++k)
                ms[size_t(k)] = power_[size_t(k)] * ((k == 0 || k == bins) ? 1.0 : 2.0) / norm;
        }
        const double binHz = sampleRate / n;

        double total = 0.0, weighted = 0.0;
        for (int k = 1; k <= bins; ++k) {
            total += ms[size_t(k)];
            weighted += ms[size_t(k)] * k * binHz;
        }
        const double totalDb = powerToDb(total);

        struct Band { const char* name; double low, high; };
        static const Band kBands[] = {
            { "sub", 20.0, 60.0 },          { "bass", 60.0, 250.0 },
            { "low_mid", 250.0, 500.0 },    { "mid", 500.0, 2000.0 },
            { "high_mid", 2000.0, 6000.0 }, { "presence", 6000.0, 12000.0 },
            { "air", 12000.0, 20000.0 },
        };
        QJsonArray bands;
        for (const auto& b : kBands) {
            double e = 0.0;
            for (int k = 1; k <= bins; ++k) {
                const double f = k * binHz;
                if (f >= b.low && f < b.high) e += ms[size_t(k)];
            }
            const double db = powerToDb(e);
            QJsonObject o;
            o["name"] = QString::fromLatin1(b.name);
            o["low_hz"] = b.low;
            o["high_hz"] = b.high;
            o["energy_db"] = roundTo(db, 2);
            // The band's share of the whole spectrum, independent of level.
            o["relative_db"] = roundTo(db > kFloorDb && totalDb > kFloorDb
                                           ? std::max(kFloorDb, db - totalDb) : kFloorDb, 2);
            bands.append(o);
        }

        double centroid = 0.0, rolloff = 0.0;
        if (total > 1e-12) {
            centroid = weighted / total;
            double cumulative = 0.0;
            for (int k = 1; k <= bins; ++k) {
                cumulative += ms[size_t(k)];
                if (cumulative >= 0.85 * total) { rolloff = k * binHz; break; }
            }
        }

        QJsonObject out;
        out["bands"] = bands;
        out["centroid_hz"] = roundTo(finiteOr(centroid, 0.0), 1);
        out["rolloff_85_hz"] = roundTo(finiteOr(rolloff, 0.0), 1);
        out["total_db"] = roundTo(totalDb, 2);
        out["fft_size"] = n;
        return out;
    }

protected:
    void frame(const std::vector<std::complex<double>>& spectrum,
               const std::vector<double>&, double gain) override
    {
        for (size_t k = 0; k < power_.size(); ++k)
            power_[k] += std::norm(spectrum[k]) * gain;
    }

private:
    std::vector<double> power_;
};

// Spectral flux of the log magnitude spectrum (1024-point Hann, hop 512),
// peak-picked against a moving-median threshold.
class OnsetDetector : public FrameAnalyzer {
public:
    OnsetDetector() : FrameAnalyzer(1024, 512), previous_(513, 0.0) {}

    QJsonObject result(double sampleRate, double analyzedSeconds)
    {
        std::vector<double> times;
        const int count = int(flux_.size());
        const double maxFlux = count > 0 ? *std::max_element(flux_.begin(), flux_.end()) : 0.0;
        if (count >= 3 && maxFlux > 1e-9) {
            std::vector<double> g(flux_.size());
            for (size_t i = 0; i < g.size(); ++i) g[i] = flux_[i] / maxFlux;

            constexpr int medianRadius = 10;   // ~0.1 s each side at 48 kHz
            constexpr int peakRadius = 3;
            const int minGap = std::max(1, int(std::ceil(0.05 * sampleRate / hop())));
            std::vector<double> window;
            window.reserve(2 * medianRadius + 1);
            int last = -minGap;
            for (int t = 1; t < count; ++t) {
                const int lo = std::max(0, t - medianRadius), hi = std::min(count - 1, t + medianRadius);
                window.assign(g.begin() + lo, g.begin() + hi + 1);
                auto mid = window.begin() + window.size() / 2;
                std::nth_element(window.begin(), mid, window.end());
                const double threshold = 1.5 * *mid + 0.06;
                if (g[size_t(t)] <= threshold) continue;
                if (rms_[size_t(t)] < 1e-3) continue;   // below -60 dBFS
                if (t - last < minGap) continue;
                bool isPeak = true;
                for (int j = std::max(0, t - peakRadius); j <= std::min(count - 1, t + peakRadius) && isPeak; ++j) {
                    if (j < t && g[size_t(j)] >= g[size_t(t)]) isPeak = false;
                    if (j > t && g[size_t(j)] > g[size_t(t)]) isPeak = false;
                }
                if (!isPeak) continue;
                last = t;
                // Between the two compared frames' centres.
                times.push_back((double(t) * hop() + (size() - hop()) / 2.0) / sampleRate);
            }
        }

        QJsonArray first;
        for (size_t i = 0; i < times.size() && i < 64; ++i)
            first.append(roundTo(times[i], 3));
        QJsonObject out;
        out["count"] = int(times.size());
        out["per_second"] = roundTo(analyzedSeconds > 0.0 ? times.size() / analyzedSeconds : 0.0, 2);
        out["times_seconds"] = first;
        return out;
    }

protected:
    void frame(const std::vector<std::complex<double>>& spectrum,
               const std::vector<double>& samples, double) override
    {
        const int bins = size() / 2;
        const double ampScale = 2.0 / windowSum();   // a full-scale sine peaks at 1
        double flux = 0.0;
        for (int k = 1; k <= bins; ++k) {
            const double mag = std::log1p(1000.0 * std::abs(spectrum[size_t(k)]) * ampScale);
            if (frames() > 0)
                flux += std::max(0.0, mag - previous_[size_t(k)]);
            previous_[size_t(k)] = mag;
        }
        double sq = 0.0;
        for (double s : samples) sq += s * s;
        flux_.push_back(flux / bins);
        rms_.push_back(std::sqrt(sq / double(samples.size())));
    }

private:
    std::vector<double> previous_;
    std::vector<double> flux_;
    std::vector<double> rms_;
};

// ── Streaming accumulator ───────────────────────────────────────────────────

class Analysis {
public:
    Analysis(double sampleRate, int numChannels, const AudioAnalysisOptions& options)
        : sampleRate_(sampleRate), numChannels_(numChannels),
          sum_(size_t(numChannels), 0.0), sumSq_(size_t(numChannels), 0.0),
          peak_(size_t(numChannels), 0.0)
    {
        meter_.prepare(sampleRate);
        silenceWindow_ = std::max(1, int(std::round(sampleRate * 0.05)));
        if (options.spectrum) spectrum_ = std::make_unique<SpectrumAnalyzer>();
        if (options.onsets) onsets_ = std::make_unique<OnsetDetector>();
    }

    // At most kReadBlock samples per call (the loudness steps are drained here).
    void process(const float* const* channels, int numSamples)
    {
        if (numSamples <= 0) return;
        meter_.process(channels, std::min(numChannels_, 2), numSamples);
        meter_.drainSteps([this](const LoudnessMeter::Step& s) { gate_.add(s); });

        const bool stereo = numChannels_ >= 2;
        const double invCh = 1.0 / numChannels_;
        for (int i = 0; i < numSamples; ++i) {
            double frameSq = 0.0, mono = 0.0;
            for (int c = 0; c < numChannels_; ++c) {
                double x = channels[c][i];
                if (!std::isfinite(x)) { x = 0.0; ++nonFinite_; }
                const double a = std::abs(x);
                sum_[size_t(c)] += x;
                sumSq_[size_t(c)] += x * x;
                if (a > peak_[size_t(c)]) peak_[size_t(c)] = a;
                if (a >= 0.999) ++clipped_;
                frameSq += x * x;
                mono += x;
            }
            mono *= invCh;
            if (stereo) {
                double l = channels[0][i], r = channels[1][i];
                if (!std::isfinite(l)) l = 0.0;
                if (!std::isfinite(r)) r = 0.0;
                sLL_ += l * l; sRR_ += r * r; sLR_ += l * r;
                const double m = 0.5 * (l + r), s = 0.5 * (l - r);
                sMM_ += m * m; sSS_ += s * s;
            }
            windowSq_ += frameSq * invCh;
            if (++windowCount_ == silenceWindow_) closeSilenceWindow();
            if (spectrum_) spectrum_->push(mono);
            if (onsets_) onsets_->push(mono);
        }
        samples_ += numSamples;
    }

    QJsonObject result(double durationSeconds, bool truncated)
    {
        // A trailing partial window only counts when there is no full one.
        if (windows_ == 0 && windowCount_ > 0) closeSilenceWindow();

        const double analyzedSeconds = samples_ / sampleRate_;
        const double n = double(std::max<juce::int64>(1, samples_));

        double peak = 0.0, totalSq = 0.0;
        QJsonArray dc;
        for (int c = 0; c < numChannels_; ++c) {
            peak = std::max(peak, peak_[size_t(c)]);
            totalSq += sumSq_[size_t(c)];
            dc.append(roundTo(sum_[size_t(c)] / n, 5));
        }
        const double peakDb = ampToDb(peak);
        const double rmsDb = powerToDb(totalSq / (n * numChannels_));
        const double truePeakDb = std::max(peakDb, loudnessOut(meter_.truePeakDb()));
        const double crest = (peakDb > kFloorDb && rmsDb > kFloorDb) ? peakDb - rmsDb : 0.0;
        const double integrated = loudnessOut(gate_.integrated());
        const double lra = std::max(0.0, finiteOr(gate_.loudnessRange(), 0.0));
        const double silenceRatio = windows_ > 0 ? double(silentWindows_) / windows_ : 1.0;

        QJsonObject out;
        out["duration_seconds"] = roundTo(durationSeconds, 3);
        out["analyzed_seconds"] = roundTo(analyzedSeconds, 3);
        out["sample_rate"] = sampleRate_;
        out["channels"] = numChannels_;
        out["peak_dbfs"] = roundTo(peakDb, 2);
        out["true_peak_dbtp"] = roundTo(truePeakDb, 2);
        out["rms_dbfs"] = roundTo(rmsDb, 2);
        out["crest_factor_db"] = roundTo(crest, 2);
        out["integrated_lufs"] = roundTo(integrated, 2);
        out["loudness_range_lu"] = roundTo(lra, 2);
        out["max_short_term_lufs"] = roundTo(loudnessOut(meter_.maxShortTerm()), 2);
        out["max_momentary_lufs"] = roundTo(loudnessOut(meter_.maxMomentary()), 2);
        out["dc_offset"] = dc;
        out["clipped_samples"] = QJsonValue(qint64(clipped_));
        out["non_finite_samples"] = QJsonValue(qint64(nonFinite_));
        out["silence_ratio"] = roundTo(silenceRatio, 3);

        QJsonObject stereo;
        double correlation = 1.0;
        if (numChannels_ >= 2) {
            const double denom = std::sqrt(sLL_ * sRR_);
            correlation = denom > 1e-20 ? std::clamp(sLR_ / denom, -1.0, 1.0) : 1.0;
            double width = 0.0;
            if (sMM_ > 1e-20) width = std::min(10.0, sSS_ / sMM_);
            else if (sSS_ > 1e-20) width = 10.0;
            double balance = 0.0;
            if (sLL_ > 1e-20 || sRR_ > 1e-20)
                balance = std::clamp(10.0 * std::log10((sLL_ + 1e-20) / (sRR_ + 1e-20)), -120.0, 120.0);
            stereo["correlation"] = roundTo(correlation, 3);
            stereo["width"] = roundTo(width, 3);
            stereo["balance_db"] = roundTo(balance, 2);
            stereo["mono"] = false;
        } else {
            stereo["correlation"] = 1.0;
            stereo["width"] = 0.0;
            stereo["balance_db"] = 0.0;
            stereo["mono"] = true;
        }
        out["stereo"] = stereo;

        if (spectrum_) out["spectrum"] = spectrum_->result(sampleRate_);
        if (onsets_) out["onsets"] = onsets_->result(sampleRate_, analyzedSeconds);

        QJsonArray warnings;
        if (samples_ == 0 || peakDb <= -60.0)
            warnings.append(QStringLiteral("silent: nothing above -60 dBFS"));
        else if (peakDb < -24.0 || (integrated > kFloorDb && integrated < -36.0))
            warnings.append(QStringLiteral("very low level: peak %1 dBFS, integrated %2 LUFS")
                                .arg(peakDb, 0, 'f', 1).arg(integrated, 0, 'f', 1));
        if (clipped_ > 0)
            warnings.append(QStringLiteral("clipping: %1 samples at or above full scale").arg(qint64(clipped_)));
        if (truePeakDb > -1.0 && peakDb > -60.0)
            warnings.append(QStringLiteral("true peak above -1 dBTP (%1 dBTP): may clip after lossy encoding")
                                .arg(truePeakDb, 0, 'f', 2));
        if (integrated > -9.0)
            warnings.append(QStringLiteral("very loud: integrated %1 LUFS (streaming targets about -14)")
                                .arg(integrated, 0, 'f', 1));
        if (crest > 0.0 && crest < 6.0 && peakDb > -60.0)
            warnings.append(QStringLiteral("low crest factor (%1 dB): heavily compressed or limited")
                                .arg(crest, 0, 'f', 1));
        if (numChannels_ >= 2 && correlation < 0.0 && peakDb > -60.0)
            warnings.append(QStringLiteral("phase issues: correlation < 0 (%1), the mix may cancel in mono")
                                .arg(correlation, 0, 'f', 2));
        for (int c = 0; c < numChannels_; ++c) {
            const double offset = sum_[size_t(c)] / n;
            if (std::abs(offset) > 0.01)
                warnings.append(QStringLiteral("DC offset on channel %1: %2").arg(c + 1).arg(offset, 0, 'f', 4));
        }
        if (nonFinite_ > 0)
            warnings.append(QStringLiteral("file contains %1 non-finite (NaN/Inf) samples, measured as silence; "
                                           "a plugin is probably unstable").arg(qint64(nonFinite_)));
        if (truncated)
            warnings.append(QStringLiteral("only the first %1 s were analyzed").arg(analyzedSeconds, 0, 'f', 1));
        out["warnings"] = warnings;
        return out;
    }

private:
    void closeSilenceWindow()
    {
        ++windows_;
        if (windowSq_ / windowCount_ < 1e-6) ++silentWindows_;   // -60 dBFS RMS
        windowSq_ = 0.0;
        windowCount_ = 0;
    }

    double sampleRate_;
    int numChannels_;
    LoudnessMeter meter_;
    LoudnessGate gate_;
    std::vector<double> sum_, sumSq_, peak_;
    juce::int64 clipped_ = 0;
    juce::int64 nonFinite_ = 0;   // NaN/Inf samples, counted per channel and replaced by 0
    juce::int64 samples_ = 0;
    double sLL_ = 0, sRR_ = 0, sLR_ = 0, sMM_ = 0, sSS_ = 0;
    int silenceWindow_ = 2400;
    int windowCount_ = 0;
    double windowSq_ = 0.0;
    juce::int64 windows_ = 0, silentWindows_ = 0;
    std::unique_ptr<SpectrumAnalyzer> spectrum_;
    std::unique_ptr<OnsetDetector> onsets_;
};

juce::int64 sampleLimit(const AudioAnalysisOptions& options, double sampleRate, juce::int64 length)
{
    if (options.maxSeconds <= 0.0) return length;
    const double limit = std::floor(options.maxSeconds * sampleRate);
    return limit < double(length) ? juce::int64(limit) : length;
}

} // namespace

QJsonObject AudioFileAnalyzer::analyze(const juce::File& file, const AudioAnalysisOptions& options,
                                       QString* error)
{
    auto setError = [error](const QString& e) { if (error) *error = e; };
    if (!file.existsAsFile()) {
        setError(QStringLiteral("file not found"));
        return {};
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader || reader->sampleRate <= 0.0 || reader->numChannels == 0 || reader->lengthInSamples < 0) {
        setError(QStringLiteral("not a readable audio file (WAV, FLAC, OGG or AIFF)"));
        return {};
    }

    const double sampleRate = reader->sampleRate;
    const int numChannels = int(std::min<unsigned int>(reader->numChannels, kMaxChannels));
    const juce::int64 length = reader->lengthInSamples;
    const juce::int64 limit = sampleLimit(options, sampleRate, length);

    Analysis analysis(sampleRate, numChannels, options);
    juce::AudioBuffer<float> buffer(numChannels, kReadBlock);
    for (juce::int64 pos = 0; pos < limit; pos += kReadBlock) {
        const int n = int(std::min<juce::int64>(kReadBlock, limit - pos));
        buffer.clear();
        if (!reader->read(buffer.getArrayOfWritePointers(), numChannels, pos, n)) {
            setError(QStringLiteral("read error at sample %1").arg(qint64(pos)));
            return {};
        }
        analysis.process(buffer.getArrayOfReadPointers(), n);
    }

    auto out = analysis.result(double(length) / sampleRate, limit < length);
    out["path"] = QString::fromStdString(file.getFullPathName().toStdString());
    out["format"] = QString::fromStdString(reader->getFormatName().toStdString());
    out["bits_per_sample"] = int(reader->bitsPerSample);
    if (error) error->clear();
    return out;
}

QJsonObject AudioFileAnalyzer::analyzeBuffer(const juce::AudioBuffer<float>& buffer, double sampleRate,
                                             const AudioAnalysisOptions& options)
{
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int numChannels = std::clamp(buffer.getNumChannels(), 1, kMaxChannels);
    const juce::int64 length = buffer.getNumChannels() > 0 ? buffer.getNumSamples() : 0;
    const juce::int64 limit = sampleLimit(options, rate, length);

    Analysis analysis(rate, numChannels, options);
    const float* pointers[kMaxChannels] {};
    for (juce::int64 pos = 0; pos < limit; pos += kReadBlock) {
        const int n = int(std::min<juce::int64>(kReadBlock, limit - pos));
        for (int c = 0; c < numChannels; ++c)
            pointers[c] = buffer.getReadPointer(c, int(pos));
        analysis.process(pointers, n);
    }
    return analysis.result(double(length) / rate, limit < length);
}

} // namespace OpenDaw
