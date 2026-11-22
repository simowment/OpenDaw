#include "SamplePreviewPlayer.h"
#include <QDir>

namespace OpenDaw {

SamplePreviewPlayer::SamplePreviewPlayer(juce::AudioDeviceManager& deviceManager)
    : deviceManager_(deviceManager)
{
    formats_.registerBasicFormats();
    player_.setSource(&transport_);
    deviceManager_.addAudioCallback(&player_);
}

SamplePreviewPlayer::~SamplePreviewPlayer()
{
    // Off the device first, so the audio thread never sees a dying source.
    deviceManager_.removeAudioCallback(&player_);
    transport_.stop();
    transport_.setSource(nullptr);
    player_.setSource(nullptr);
    reader_.reset();
}

bool SamplePreviewPlayer::play(const QString& path)
{
    transport_.stop();
    transport_.setSource(nullptr);
    reader_.reset();
    current_.clear();

    const juce::File file(juce::String::fromUTF8(QDir::toNativeSeparators(path).toUtf8().constData()));
    auto* reader = formats_.createReaderFor(file);
    if (!reader) return false;
    reader_ = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    transport_.setSource(reader_.get(), 0, nullptr, reader->sampleRate);
    transport_.setPosition(0.0);
    transport_.start();
    current_ = path;
    return true;
}

void SamplePreviewPlayer::stop()
{
    transport_.stop();
}

bool SamplePreviewPlayer::isPlaying() const
{
    return transport_.isPlaying();
}

} // namespace OpenDaw
