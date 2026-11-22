#pragma once

#include "engine/AudioEngine.h"
#include <QString>
#include <memory>

namespace OpenDaw {

// Plays an audio file once on the audio device, outside the Edit (sample
// previews in the sample browser), like FileBrowserPanel's preview: a JUCE
// AudioTransportSource fed to an AudioSourcePlayer that is one of the device
// manager's audio callbacks for as long as the player exists.
// Message thread. Cheap to create; reuse one for a run of previews.
class SamplePreviewPlayer {
public:
    explicit SamplePreviewPlayer(juce::AudioDeviceManager& deviceManager);
    ~SamplePreviewPlayer();
    SamplePreviewPlayer(const SamplePreviewPlayer&) = delete;
    SamplePreviewPlayer& operator=(const SamplePreviewPlayer&) = delete;

    // Plays the file from the start (stopping what was playing). False when
    // it can't be read.
    bool play(const QString& path);
    void stop();
    bool isPlaying() const;
    QString currentFile() const { return current_; }

private:
    juce::AudioDeviceManager& deviceManager_;
    juce::AudioFormatManager formats_;
    juce::AudioTransportSource transport_;
    juce::AudioSourcePlayer player_;
    std::unique_ptr<juce::AudioFormatReaderSource> reader_;
    QString current_;
};

} // namespace OpenDaw
