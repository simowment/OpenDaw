#pragma once

// Agent tools for the FL-Studio-style pattern workflow (channel rack, step
// sequencer, playlist placements) on top of PatternManager:
//
//   list_patterns, get_pattern, create_pattern, clone_pattern, delete_pattern,
//   rename_pattern, set_pattern_length, set_pattern_steps,
//   clear_pattern_channel, place_pattern, remove_pattern_placement,
//   list_channels, set_channel_root_note,
//   list_sample_folders, find_samples, add_sample_channel, set_channel_sample
//
// Patterns are addressed as "pattern:<n>" (or the number, or the exact name);
// channels are MIDI tracks, addressed like any track ("track:<id>", name or
// index) and resolved through AiToolExecutor::resolveTrack.
//
// Register once at start-up (the PatternManager must outlive every executor):
//
//     AiToolExtensionRegistry::instance().addProvider(
//         [&pm](AiToolExecutor& ex) { return patternToolExtensions(pm, ex); });

#include "ai/AiToolExtensions.h"
#include <QVector>

namespace OpenDaw {

class PatternManager;
class AiToolExecutor;

// The executor and the PatternManager must outlive the returned handlers.
QVector<AiToolExtension> patternToolExtensions(PatternManager& pm, AiToolExecutor& ex);

} // namespace OpenDaw
