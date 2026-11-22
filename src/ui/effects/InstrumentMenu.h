#pragma once

#include "engine/AudioEngine.h"
#include <QPoint>
#include <functional>

class QMenu;

namespace OpenDaw {

class EditManager;

// The instrument picker for a MIDI track: the built-in sounds by family (and
// drum kits), General MIDI mode for the built-in instrument, a VST instrument,
// or none. `chooseVst` opens the VST instrument list; `chooseSurge` puts the
// bundled Surge XT synth on the track (null if it isn't shipped).
void showInstrumentMenu(EditManager& editMgr, te::AudioTrack* track,
                        const std::function<void()>& chooseVst,
                        const std::function<void()>& chooseSurge, const QPoint& globalPos);

// Adds the built-in General MIDI sounds to `menu`: one submenu per family
// (General MIDI sounds, then the variations) plus the drum kits. `isCurrent`
// (may be empty) ticks the current sound; `onPick(bank, program)` runs when
// one is chosen. When the SoundFont is missing, a disabled note is added
// instead and false is returned. Shared by the instrument menu and the
// Channel Rack's sound picker.
bool addBuiltInSoundMenus(QMenu& menu, const std::function<bool(int bank, int program)>& isCurrent,
                          const std::function<void(int bank, int program)>& onPick,
                          bool withHeading = true);

} // namespace OpenDaw
