#pragma once

// What a Channel Rack channel *sounds like*, and the edits that change it.
//
// A channel is a MIDI track whose instrument is its sound source:
//  - Sample:  a te::SamplerPlugin holding one file (PatternManager sample channel)
//  - BuiltIn: the built-in General MIDI instrument (SoundFontPlugin)
//  - Plugin:  a VST/VST3 instrument (te::ExternalPlugin, incl. bundled Surge XT)
//  - None:    no instrument yet
//
// The rack names a channel after its sound (FL Studio style): a channel's
// track is named after the sample file or the instrument when it is created,
// and a track still carrying a default name ("Track 3") is shown under its
// sound's name. Nothing here uses widgets, so the integration tests can
// drive it (tests/tst_ChannelRackFl.cpp).
//
// Every mutator is one undo step (it opens its own transaction unless an
// EditManager undo group is open) and sends one round of EditManager
// signals (ScopedSignalBatch). Message thread only.

#include "engine/AudioEngine.h"
#include <QString>
#include <QStringList>

namespace OpenDaw {

class EditManager;
class PatternManager;

enum class ChannelSoundKind { None, Sample, BuiltIn, Plugin };

// A sound picked in the sound picker (SoundPicker) or dropped on the rack.
struct SoundChoice {
    enum class Kind { None, Samples, BuiltIn, Plugin };
    Kind kind = Kind::None;
    QStringList samples;               // Samples: one channel per file
    int bank = 0, program = 0;         // BuiltIn
    juce::PluginDescription plugin;    // Plugin
    QString name;                      // the sound's own name (menu text)

    bool isValid() const;
    static SoundChoice fromSamples(const QStringList& paths);
    static SoundChoice builtIn(int bank, int program);   // name from the SoundFont library
    static SoundChoice fromPlugin(const juce::PluginDescription& desc);
};

namespace channelsound {

ChannelSoundKind kind(EditManager& em, PatternManager* pm, te::AudioTrack* track);
// The name of the channel's sound: the sample's file name (no extension),
// the built-in preset ("Grand Piano") or the plugin ("Surge XT"). Empty
// without an instrument.
QString soundName(EditManager& em, PatternManager* pm, te::AudioTrack* track);
// What the rack shows: the track name, unless it is a default name
// (isDefaultName), then the sound's name, else "Channel N".
QString displayName(EditManager& em, PatternManager* pm, te::AudioTrack* track);
// "Track 3", "Audio 1", "MIDI 2", "Channel 4", "" and the like.
bool isDefaultName(const QString& name);
QString builtInPresetName(int bank, int program);

// A new channel playing `choice` (BuiltIn or Plugin), named after it.
// One undo step. Null (and *error) on failure: nothing is left behind.
te::AudioTrack* addInstrumentChannel(EditManager& em, const SoundChoice& choice,
                                     QString* error = nullptr);
// New sample channels, one per file; returns those added (failures in *errors).
QVector<te::AudioTrack*> addSampleChannels(PatternManager& pm, const QStringList& paths,
                                           QStringList* errors = nullptr);
// Gives a channel a new sound and keeps its notes in every pattern (the track
// stays the same). Samples: the first file. The channel is renamed after the
// new sound when its name was the old sound's name or a default name.
bool replaceSound(EditManager& em, PatternManager& pm, te::EditItemID trackId,
                  const SoundChoice& choice, QString* error = nullptr);
// A copy of the channel (same instrument and settings, mix, colour, root note,
// its notes in every pattern), added as the last channel and named
// "<name> #2" (#3, ...). One undo step.
te::AudioTrack* cloneChannel(EditManager& em, PatternManager& pm, te::EditItemID trackId,
                             QString* error = nullptr);
// One undo step; false when the name is empty or the track is gone.
bool renameChannel(EditManager& em, te::EditItemID trackId, const QString& name);
// "<base> #2", "#3", ...: the first one no channel is called yet.
QString uniqueCloneName(EditManager& em, const QString& base);

// ── Recent samples (QSettings "channelRack/recentSamples") ──────────────────
constexpr int kMaxRecentSamples = 10;
// Most recent first; files that no longer exist are left out.
QStringList recentSamples();
// Moves the files to the front (most recent = the last of `paths`).
void rememberSamples(const QStringList& paths);

} // namespace channelsound
} // namespace OpenDaw
