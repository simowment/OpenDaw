#include "AiToolDefs.h"
#include <QJsonObject>

namespace OpenDaw {

// Every input_schema is enforced by AiSchemaValidator before a handler runs:
// types, ranges, enums and required keys are checked, and unknown properties
// are rejected. Keep each schema in step with the keys its handler reads.

static QJsonObject makeTool(const QString& name, const QString& description,
                            const QJsonObject& properties,
                            const QJsonArray& required = {})
{
    QJsonObject inputSchema;
    inputSchema["type"] = "object";
    inputSchema["properties"] = properties;
    if (!required.isEmpty())
        inputSchema["required"] = required;
    inputSchema["additionalProperties"] = false;

    QJsonObject tool;
    tool["name"] = name;
    tool["description"] = description;
    tool["input_schema"] = inputSchema;
    return tool;
}

static QJsonObject prop(const QString& type, const QString& description)
{
    QJsonObject p;
    p["type"] = type;
    p["description"] = description;
    return p;
}

static QJsonObject propNumber(const QString& description, double min, double max)
{
    QJsonObject p;
    p["type"] = "number";
    p["description"] = description;
    p["minimum"] = min;
    p["maximum"] = max;
    return p;
}

static QJsonObject propInteger(const QString& description, int min, int max)
{
    QJsonObject p;
    p["type"] = "integer";
    p["description"] = description;
    p["minimum"] = min;
    p["maximum"] = max;
    return p;
}

static QJsonObject propEnum(const QString& description, const QStringList& values)
{
    QJsonObject p;
    p["type"] = "string";
    p["description"] = description;
    QJsonArray e;
    for (const auto& v : values) e.append(v);
    p["enum"] = e;
    return p;
}

static QJsonObject typeOnly(const QString& type)
{
    QJsonObject t;
    t["type"] = type;
    return t;
}

// A track reference: "track:<id>" (stable), a name, a zero-based index, or
// {"id"} / {"index","name"} objects.
static QJsonObject trackRefSchema(const QString& description)
{
    QJsonObject objProps;
    objProps["id"] = prop("string", "Track id like 'track:1043'.");
    objProps["index"] = prop("integer", "Zero-based track index.");
    objProps["name"] = prop("string", "Track name (checked against the index when both are given).");
    QJsonObject objType;
    objType["type"] = "object";
    objType["properties"] = objProps;
    objType["additionalProperties"] = false;

    QJsonObject p;
    p["description"] = description;
    p["oneOf"] = QJsonArray{typeOnly("string"), typeOnly("integer"), objType};
    return p;
}

static QJsonObject trackProp()
{
    return trackRefSchema("Track id like 'track:1043' (preferred, stable), or name, or zero-based index.");
}

static QJsonObject busProp(const QString& what)
{
    return trackRefSchema(what + " Bus track id like 'track:1043' (preferred, stable), or its name, "
                                 "or zero-based track index. It must be a bus track.");
}

static QJsonObject targetProp()
{
    return trackRefSchema("Track id like 'track:1043' (preferred, stable), track name or zero-based "
                          "index, or the literal string 'master'.");
}

static QJsonObject effectProp()
{
    QJsonObject p;
    p["description"] = "Effect id like 'plugin:877' (preferred, stable), effect name, or zero-based "
                       "index in the track's effect chain (as listed by get_track_effects).";
    p["oneOf"] = QJsonArray{typeOnly("string"), typeOnly("integer")};
    return p;
}

static QJsonObject parameterProp()
{
    QJsonObject p;
    p["description"] = "Parameter name (string) or zero-based index (integer).";
    p["oneOf"] = QJsonArray{typeOnly("string"), typeOnly("integer")};
    return p;
}

static QJsonObject clipIdProp()
{
    return prop("string", "Clip id like 'clip:2210' (preferred, stable), as returned by create_midi_clip "
                          "or get_clips_on_track. With clip_id, track may be omitted.");
}

static QJsonObject clipIndexProp()
{
    return propInteger("Zero-based index of the clip on the track (as returned by create_midi_clip or "
                       "get_clips_on_track). Needs track. Give clip_id or clip_index.", 0, 100000);
}

// Adds track + clip_id + clip_index to a clip tool's properties.
static void addClipTarget(QJsonObject& props)
{
    props["track"] = trackRefSchema("Track id like 'track:1043' (preferred, stable), or name, or "
                                    "zero-based index. Required with clip_index; optional with clip_id.");
    props["clip_id"] = clipIdProp();
    props["clip_index"] = clipIndexProp();
}

QJsonArray AiToolDefs::allTools()
{
    QJsonArray tools;

    // ── Track Management ────────────────────────────────────────────────────

    tools.append(makeTool("get_project_info",
        "Get an overview of the current project: track count, tempo, time signature, transport state, and a list of all tracks with basic info (each with its stable id).",
        {}));

    tools.append(makeTool("get_track_list",
        "Get a list of all tracks with their stable id ('track:<n>'), index, name, type (audio/midi/bus), mute, solo, volume (dB), and pan.",
        {}));

    {
        QJsonObject props;
        props["track"] = trackProp();
        tools.append(makeTool("get_track_info",
            "Get detailed information about a specific track: id, name, type, mute, solo, volume, pan, input, output, effects (with ids and parameters), sends, armed state, mono state.",
            props, {"track"}));
    }

    {
        QJsonObject props;
        props["name"] = prop("string", "Optional name for the new track.");
        tools.append(makeTool("create_audio_track",
            "Create a new audio track. Optionally set its name. Returns {id, index, name}.",
            props));
    }

    {
        QJsonObject props;
        props["name"] = prop("string", "Optional name for the new MIDI track.");
        tools.append(makeTool("create_midi_track",
            "Create a new MIDI track. Optionally set its name. It plays the built-in Grand Piano "
            "until you choose another sound with set_track_instrument. Returns {id, index, name}.",
            props));
    }

    tools.append(makeTool("list_instruments",
        "List the instruments a MIDI track can use: the built-in sounds (a full General MIDI set "
        "grouped by family, plus drum kits), the bundled Surge XT synth, and any VST instruments "
        "installed.",
        {}));

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["instrument"] = prop("string",
            "A built-in sound name (e.g. 'Grand Piano', 'Finger Bass', 'Slow Strings', 'Warm Pad', "
            "'Saw Lead'), a drum kit ('Standard 1', 'Room', 'Power', 'Electronic', '808/909', 'Jazz', "
            "'Brush', 'Orchestral', or just 'drums'), a General MIDI program number 1-128, 'Surge XT' "
            "(the bundled synth, for when the user wants to design or browse synth sounds "
            "themselves), or the name of an installed VST instrument.");
        props["follow_program_changes"] = prop("boolean",
            "Built-in instrument only: play like a General MIDI module, where program changes in the "
            "MIDI choose each channel's sound and channel 10 plays drums. For imported GM files.");
        tools.append(makeTool("set_track_instrument",
            "Choose the instrument a MIDI track plays. Give every MIDI part a fitting sound: drums on "
            "a drum kit, bass on a bass, and so on.",
            props, {"track", "instrument"}));
    }

    {
        QJsonObject props;
        props["name"] = prop("string", "Optional name for the new bus track.");
        tools.append(makeTool("create_bus_track",
            "Create a new bus track for submixing. Optionally set its name. Returns {id, index, name}.",
            props));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        tools.append(makeTool("delete_track",
            "Delete a track by id, name or index. This is a destructive action.",
            props, {"track"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["new_name"] = prop("string", "The new name for the track.");
        tools.append(makeTool("rename_track",
            "Rename a track. Its id does not change.",
            props, {"track", "new_name"}));
    }

    // ── Track Properties ────────────────────────────────────────────────────

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["muted"] = prop("boolean", "True to mute, false to unmute.");
        tools.append(makeTool("set_track_mute",
            "Mute or unmute a track.",
            props, {"track", "muted"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["solo"] = prop("boolean", "True to solo, false to unsolo.");
        tools.append(makeTool("set_track_solo",
            "Solo or unsolo a track.",
            props, {"track", "solo"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["volume_db"] = propNumber("Volume in decibels.", -60.0, 6.0);
        tools.append(makeTool("set_track_volume",
            "Set the volume of a track in decibels (-60 to +6 dB).",
            props, {"track", "volume_db"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["pan"] = propNumber("Pan position: -1.0 (full left) to 1.0 (full right), 0.0 center.", -1.0, 1.0);
        tools.append(makeTool("set_track_pan",
            "Set the pan position of a track.",
            props, {"track", "pan"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["mono"] = prop("boolean", "True for mono, false for stereo.");
        tools.append(makeTool("set_track_mono",
            "Set a track to mono or stereo mode.",
            props, {"track", "mono"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["enabled"] = prop("boolean", "True to arm for recording, false to disarm.");
        tools.append(makeTool("set_track_record_enabled",
            "Arm or disarm a track for recording.",
            props, {"track", "enabled"}));
    }

    // ── Routing ─────────────────────────────────────────────────────────────

    tools.append(makeTool("get_available_inputs",
        "List all available audio input devices that can be assigned to tracks.",
        {}));

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["input_name"] = prop("string", "The name of the input device to assign.");
        tools.append(makeTool("assign_input_to_track",
            "Route an audio input device to a track.",
            props, {"track", "input_name"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        tools.append(makeTool("clear_track_input",
            "Remove the input assignment from a track.",
            props, {"track"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["destination"] = trackRefSchema("Destination: \"master\", or a track id like 'track:1043' "
                                              "(preferred), track name or zero-based index to route to.");
        tools.append(makeTool("set_track_output",
            "Route a track's output to the master bus or to another track/bus.",
            props, {"track", "destination"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        tools.append(makeTool("clear_track_output",
            "Disconnect a track's output. This is a destructive action.",
            props, {"track"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["bus"] = busProp("The bus to send to.");
        props["level_db"] = propNumber("Send level in dB (default -6).", -60, 6);
        props["pre_fader"] = prop("boolean", "True for a pre-fader send, which ignores the track's fader. Default false (post-fader).");
        tools.append(makeTool("add_send",
            "Add an aux send: a copy of the track's signal goes into a bus at its own level, while the "
            "track's output stays where it is. Use for shared reverb/delay buses and parallel processing.",
            props, {"track", "bus"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["bus"] = busProp("The bus the send goes to.");
        props["level_db"] = propNumber("New send level in dB.", -60, 6);
        props["pre_fader"] = prop("boolean", "Pre-fader (true) or post-fader (false).");
        props["muted"] = prop("boolean", "Mute or unmute the send.");
        tools.append(makeTool("set_send",
            "Change an existing aux send: its level, pre/post-fader setting or mute.",
            props, {"track", "bus"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["bus"] = busProp("The bus the send goes to.");
        tools.append(makeTool("remove_send",
            "Remove an aux send from a track to a bus.",
            props, {"track", "bus"}));
    }

    // ── Effects ─────────────────────────────────────────────────────────────

    tools.append(makeTool("list_available_effects",
        "List all available effects: built-in (Reverb, EQ, Compressor, Delay, Chorus, Phaser, Low Pass Filter, Pitch Shift) and any installed VST plugins.",
        {}));

    {
        QJsonObject props;
        props["track"] = trackProp();
        tools.append(makeTool("get_track_effects",
            "List all effects on a track with their stable ids ('plugin:<n>'), index, parameters and current values.",
            props, {"track"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["effect_name"] = prop("string", "The name of the effect to add (e.g. 'Reverb', 'EQ', 'Compressor', 'Delay', 'Chorus', 'Phaser', 'Low Pass Filter', 'Pitch Shift').");
        tools.append(makeTool("add_effect_to_track",
            "Add a built-in effect to a track. Returns the new effect's id.",
            props, {"track", "effect_name"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["effect"] = effectProp();
        tools.append(makeTool("remove_effect_from_track",
            "Remove an effect from a track. This is a destructive action.",
            props, {"track", "effect"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["effect"] = effectProp();
        props["parameter"] = parameterProp();
        props["value"] = propNumber("Normalized parameter value from 0.0 to 1.0.", 0.0, 1.0);
        tools.append(makeTool("set_effect_parameter",
            "Set a parameter on an effect. Values are normalized 0.0 to 1.0. Use get_track_effects first to see parameter names and current values.",
            props, {"track", "effect", "parameter", "value"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["effect"] = effectProp();
        props["bypassed"] = prop("boolean", "True to bypass (disable), false to enable.");
        tools.append(makeTool("set_effect_bypass",
            "Bypass or enable an effect on a track.",
            props, {"track", "effect", "bypassed"}));
    }

    // ── Transport ───────────────────────────────────────────────────────────

    tools.append(makeTool("play",
        "Start playback.",
        {}));

    tools.append(makeTool("stop",
        "Stop playback.",
        {}));

    tools.append(makeTool("record",
        "Start recording on all armed tracks.",
        {}));

    {
        QJsonObject props;
        props["seconds"] = propNumber("Position in seconds to seek to.", 0.0, 999999.0);
        tools.append(makeTool("set_position",
            "Seek the transport to a specific position in seconds.",
            props, {"seconds"}));
    }

    tools.append(makeTool("get_transport_state",
        "Get the current transport state: playing/stopped/recording, position in seconds, and loop status.",
        {}));

    // ── Analysis / Mix Intelligence ─────────────────────────────────────────

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["window_seconds"] = propNumber("Analysis window length in seconds (recommended: 1-10). Default 2.", 0.1, 120.0);
        tools.append(makeTool("analyze_track_levels",
            "Analyze a track's current levels and return peak, true peak estimate, RMS estimate, and crest factor.",
            props, {"track"}));
    }

    {
        QJsonObject props;
        props["window_seconds"] = propNumber("Analysis window length in seconds (recommended: 1-30). Default 4.", 0.1, 300.0);
        tools.append(makeTool("analyze_master_levels",
            "Read the master output right now: peak, per-track peaks, and the live loudness meter "
            "(momentary, short-term and integrated LUFS, loudness range, true peak) over what has "
            "played since the meter was last reset. Integrated values only mean something after "
            "the song has played; for an exact figure without playing, use measure_mix_loudness.",
            props));
    }

    tools.append(makeTool("measure_mix_loudness",
        "Render the whole mix offline (faster than real time, no playback needed) and measure it "
        "exactly to ITU-R BS.1770 / EBU R128: integrated loudness (LUFS), true peak (dBTP), "
        "loudness range (LU) and the loudest 3 seconds. Use it to check a mix against a "
        "delivery target such as -14 LUFS for streaming with true peak at or below -1 dBTP.",
        QJsonObject()));

    {
        QJsonObject props;
        props["target"] = targetProp();
        tools.append(makeTool("analyze_frequency_balance",
            "Estimate low/mid/high spectral energy balance and tilt for a track or master.",
            props, {"target"}));
    }

    {
        QJsonObject props;
        props["target"] = targetProp();
        tools.append(makeTool("analyze_stereo_image",
            "Estimate stereo width and mono compatibility for a track or master.",
            props, {"target"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        tools.append(makeTool("analyze_transients",
            "Estimate transient density and attack sharpness for a track.",
            props, {"track"}));
    }

    {
        QJsonObject props;
        props["track_a"] = trackProp();
        props["track_b"] = trackProp();
        tools.append(makeTool("analyze_masking",
            "Estimate frequency masking/overlap risk between two tracks.",
            props, {"track_a", "track_b"}));
    }

    // ── Semantic Mix / Master Actions ───────────────────────────────────────

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["preset_name"] = propEnum("Preset intent name.",
                                        {"vocal_clarity", "drum_punch", "bass_control"});
        tools.append(makeTool("apply_mix_preset",
            "Apply a semantic mix preset to a track by adjusting effects and level/dynamics-related settings.",
            props, {"track", "preset_name"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["dbfs"] = propNumber("Desired peak target in dBFS (negative value).", -40.0, 0.0);
        tools.append(makeTool("set_track_target_peak",
            "Adjust a track's volume toward a target peak in dBFS. The track must be playing audio.",
            props, {"track", "dbfs"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["goal"] = propEnum("Dynamic goal.", {"tighter", "more_open", "more_sustain"});
        tools.append(makeTool("set_track_dynamic_goal",
            "Apply dynamic-shaping behavior for a track by tuning compressor-style settings.",
            props, {"track", "goal"}));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["style"] = propEnum("Reverb character style.", {"ethereal", "roomy", "tight"});
        props["amount"] = propNumber("Wetness/intensity amount from 0.0 to 1.0. Default 0.5.", 0.0, 1.0);
        tools.append(makeTool("set_reverb_character",
            "Set reverb behavior on a track using style and amount semantics.",
            props, {"track", "style"}));
    }

    {
        QJsonObject props;
        props["bus"] = busProp("The bus to glue.");
        props["intensity"] = propNumber("Glue intensity from 0.0 to 1.0. Default 0.5.", 0.0, 1.0);
        tools.append(makeTool("set_bus_glue",
            "Apply gentle bus compression-style glue to a bus track.",
            props, {"bus"}));
    }

    {
        QJsonObject props;
        props["profile"] = propEnum("Master profile.", {"streaming_balanced", "loud_pop", "dynamic_film"});
        tools.append(makeTool("set_master_target",
            "Apply master output target behavior/profile guidance.",
            props, {"profile"}));
    }

    // ── Session Structure Helpers ────────────────────────────────────────────

    tools.append(makeTool("create_mix_buses_from_roles",
        "Create common mix buses (Drums Bus, Music Bus, Vocals Bus, FX Bus) that do not exist yet and "
        "return the created buses as [{id, index, name}].",
        {}));

    tools.append(makeTool("auto_route_by_name_patterns",
        "Route tracks to buses using inferred naming patterns (e.g., kick/snare/toms to drums bus).",
        {}));

    tools.append(makeTool("group_tracks_by_inferred_role",
        "Return track grouping suggestions by inferred role (drums, bass, vocals, music, fx, etc.).",
        {}));

    // ── Mix Plan / Checkpoint Helpers ────────────────────────────────────────

    {
        QJsonObject props;
        props["request"] = prop("string", "Original user mix/master request.");
        tools.append(makeTool("preview_mix_plan",
            "Generate and return a staged mix/master action plan without changing project state.",
            props, {"request"}));
    }

    tools.append(makeTool("commit_last_mix_stage",
        "Mark the most recent mix stage as committed/checkpointed for workflow tracking.",
        {}));

    tools.append(makeTool("revert_last_mix_stage",
        "Revert one mix stage by undoing recent changes.",
        {}));

    {
        QJsonObject props;
        props["bpm"] = propNumber("Tempo in beats per minute.", 20.0, 999.0);
        tools.append(makeTool("set_tempo",
            "Set the project tempo in BPM.",
            props, {"bpm"}));
    }

    {
        QJsonObject props;
        props["numerator"] = propInteger("Time signature numerator (e.g. 4).", 1, 32);
        QJsonObject den = propInteger("Time signature denominator: 1, 2, 4, 8, 16 or 32.", 1, 32);
        den["enum"] = QJsonArray{1, 2, 4, 8, 16, 32};
        props["denominator"] = den;
        tools.append(makeTool("set_time_signature",
            "Set the project time signature.",
            props, {"numerator", "denominator"}));
    }

    // ── Project ─────────────────────────────────────────────────────────────

    tools.append(makeTool("save_project",
        "Save the current project to disk. Pass path (an absolute .tracktionedit file path) to save "
        "under a new name (Save As) - required the first time an untitled project is saved; the "
        "folder is created if needed and the project then keeps that path.",
        {{"path", prop("string", "Absolute path ending in .tracktionedit (optional; Save As).")}}));

    tools.append(makeTool("undo",
        "Undo the last action.",
        {}));

    tools.append(makeTool("redo",
        "Redo the last undone action.",
        {}));

    {
        QJsonObject channelObj;
        channelObj["type"] = "object";
        QJsonObject channelProps;
        channelProps["channel"] = propInteger("MIDI channel number (1-16).", 1, 16);
        channelProps["name"] = prop("string", "Display name for this channel (e.g. 'Violin 1').");
        channelObj["properties"] = channelProps;
        channelObj["required"] = QJsonArray({"channel", "name"});
        channelObj["additionalProperties"] = false;

        QJsonObject channelsArr;
        channelsArr["type"] = "array";
        channelsArr["items"] = channelObj;
        channelsArr["minItems"] = 1;
        channelsArr["maxItems"] = 16;
        channelsArr["description"] = "Array of channel definitions to create.";

        QJsonObject props;
        props["track"] = trackProp();
        props["channels"] = channelsArr;

        tools.append(makeTool("setup_midi_channels",
            "Create multiple linked MIDI channel clips on a track for multi-timbral instruments. "
            "Creates co-located MIDI clips (same position/length) each on a different channel, "
            "with display names (e.g. 'Violin 1', 'Viola'). If no MIDI clip exists on the track, "
            "one is created first. Returns each channel's clip id.",
            props,
            QJsonArray({"track", "channels"})));
    }

    tools.append(makeTool("set_channel_name",
        "Set the display name of a MIDI channel clip on a track.",
        {{"track", trackProp()},
         {"channel", propInteger("MIDI channel number (1-16).", 1, 16)},
         {"name", prop("string", "New display name.")}},
        QJsonArray({"track", "channel", "name"})));

    tools.append(makeTool("get_channel_names",
        "Get all MIDI channel display names for clips on a track.",
        {{"track", trackProp()}},
        QJsonArray({"track"})));

    // ── MIDI Clip / Note Creation ────────────────────────────────────────────

    {
        QJsonObject props;
        props["track"] = trackProp();
        props["start_beat"] = propNumber("Start position in beats (0 = bar 1 beat 1).", 0.0, 100000.0);
        props["length_beats"] = propNumber("Clip length in beats (must be positive).", 0.001, 100000.0);
        props["channel"] = propInteger("MIDI channel for the clip (1-16). Default is 1. "
            "IMPORTANT: this determines which channel lane the clip appears on. "
            "For GM drums, use channel 10.", 1, 16);
        props["name"] = prop("string", "Optional clip name.");
        tools.append(makeTool("create_midi_clip",
            "Create an empty MIDI clip on a track at the given beat position and length. "
            "Returns the clip's stable id (use it as clip_id with add_midi_notes) and its clip_index. "
            "The track will be marked as a MIDI track if it isn't already. "
            "Set 'channel' to control which MIDI channel lane the clip belongs to "
            "(e.g. channel 10 for GM drums). The clip channel determines the lane/color; "
            "note-level channels in add_midi_notes set the colour index within the clip.",
            props, QJsonArray({"track", "start_beat", "length_beats"})));
    }

    {
        QJsonObject noteObj;
        noteObj["type"] = "object";
        QJsonObject noteProps;
        noteProps["note"] = propInteger("MIDI note number (0-127). Middle C = 60.", 0, 127);
        noteProps["start_beat"] = propNumber("Start position in beats relative to the clip start.", 0.0, 100000.0);
        noteProps["length_beats"] = propNumber("Note duration in beats.", 0.0, 100000.0);
        noteProps["velocity"] = propInteger("Note velocity (1-127). Default 100.", 1, 127);
        noteProps["channel"] = propInteger("MIDI channel (1-16). Default 1.", 1, 16);
        noteObj["properties"] = noteProps;
        noteObj["required"] = QJsonArray({"note", "start_beat", "length_beats"});
        noteObj["additionalProperties"] = false;

        QJsonObject notesArr;
        notesArr["type"] = "array";
        notesArr["items"] = noteObj;
        notesArr["minItems"] = 1;
        notesArr["description"] = "Array of MIDI notes to add.";

        QJsonObject props;
        addClipTarget(props);
        props["notes"] = notesArr;
        tools.append(makeTool("add_midi_notes",
            "Add one or more MIDI notes to an existing MIDI clip, chosen by clip_id (preferred) or "
            "track + clip_index. "
            "Note positions are relative to the clip start (beat 0 = clip start). "
            "Default velocity is 100, default channel is 1. A length of 0 becomes 0.25 beats. "
            "You can add many notes in a single call for efficiency.",
            props, QJsonArray({"notes"})));
    }

    {
        QJsonObject props;
        props["track"] = trackProp();
        tools.append(makeTool("get_clips_on_track",
            "List all clips on a track with their stable id ('clip:<n>'), clip_index, name, type (audio/midi), "
            "start beat, length in beats, and for MIDI clips the channel and note count.",
            props, QJsonArray({"track"})));
    }

    {
        QJsonObject props;
        addClipTarget(props);
        tools.append(makeTool("clear_midi_notes",
            "Remove all MIDI notes from a clip (clip_id, or track + clip_index). Useful before re-populating a clip.",
            props));
    }

    {
        QJsonObject props;
        addClipTarget(props);
        props["channel"] = propInteger("New MIDI channel for the clip (1-16).", 1, 16);
        tools.append(makeTool("set_clip_channel",
            "Change the MIDI channel assignment of a clip (clip_id, or track + clip_index). This controls "
            "which channel lane the clip appears on and its display color. For example, use channel 10 for GM drums. "
            "This does NOT change per-note data inside the clip — only the clip-level channel.",
            props, QJsonArray({"channel"})));
    }

    // ── MIDI Note Reading & Surgical Editing ─────────────────────────────────

    {
        QJsonObject props;
        addClipTarget(props);
        props["channel"] = propInteger("Optional: only return notes on this MIDI channel (1-16).", 1, 16);
        props["note_min"] = propInteger("Optional: minimum MIDI note number (0-127) to include.", 0, 127);
        props["note_max"] = propInteger("Optional: maximum MIDI note number (0-127) to include.", 0, 127);
        props["start_beat_min"] = propNumber("Optional: only include notes starting at or after this beat.", 0.0, 100000.0);
        props["start_beat_max"] = propNumber("Optional: only include notes starting before this beat.", 0.0, 100000.0);
        tools.append(makeTool("get_midi_notes",
            "Read all MIDI notes from a clip (clip_id, or track + clip_index). Returns an array of notes with note number, "
            "start_beat (relative to clip start), length_beats, velocity, and channel. "
            "Use optional filters to narrow results by channel, note range, or beat range. "
            "Essential for inspecting existing MIDI data before modifying it.",
            props));
    }

    {
        QJsonObject props;
        addClipTarget(props);
        props["channel"] = propInteger("Optional: only remove notes on this MIDI channel (1-16).", 1, 16);
        props["note_min"] = propInteger("Optional: minimum MIDI note number (0-127) to match.", 0, 127);
        props["note_max"] = propInteger("Optional: maximum MIDI note number (0-127) to match.", 0, 127);
        props["start_beat_min"] = propNumber("Optional: only match notes starting at or after this beat.", 0.0, 100000.0);
        props["start_beat_max"] = propNumber("Optional: only match notes starting before this beat.", 0.0, 100000.0);
        tools.append(makeTool("remove_midi_notes",
            "Remove MIDI notes from a clip (clip_id, or track + clip_index) that match the given filter criteria. "
            "At least one filter (channel, note range, or beat range) must be provided "
            "to prevent accidental deletion of all notes. Use clear_midi_notes to remove everything.",
            props));
    }

    // ── Batching ─────────────────────────────────────────────────────────────

    {
        QJsonObject cmdProps;
        cmdProps["tool"] = prop("string", "Name of the tool to run, e.g. 'set_track_volume'.");
        QJsonObject cmdInput;
        cmdInput["type"] = "object";
        cmdInput["description"] = "That tool's input, exactly as the tool itself takes it.";
        cmdProps["input"] = cmdInput;
        QJsonObject cmdObj;
        cmdObj["type"] = "object";
        cmdObj["properties"] = cmdProps;
        cmdObj["required"] = QJsonArray({"tool"});
        cmdObj["additionalProperties"] = false;

        QJsonObject commands;
        commands["type"] = "array";
        commands["items"] = cmdObj;
        commands["minItems"] = 1;
        commands["maxItems"] = 200;
        commands["description"] = "Commands to run in order.";

        QJsonObject props;
        props["commands"] = commands;
        props["atomic"] = prop("boolean",
            "Default true: if any command fails, every change the batch made is rolled back and the "
            "batch fails with BATCH_FAILED.");
        props["stop_on_error"] = prop("boolean",
            "Only for atomic=false. Default true: stop at the first failing command (earlier changes "
            "stay). False: run every command and report each result.");
        tools.append(makeTool("run_batch",
            "Run several tool commands in one call and one undo step ('Agent batch'). Every command's "
            "input is validated before anything runs. Transport, render, save, undo/redo and run_batch "
            "itself are not allowed inside a batch. Rollback (atomic) covers project state recorded in "
            "the undo history (tracks, clips, notes, effects, routing, mix settings); it cannot undo "
            "things outside the project such as files written to disk. Returns "
            "{results:[{tool, ok, result|error}], count}. A failed atomic batch returns a BATCH_FAILED "
            "error whose details hold failed_index, rolled_back, reverted_transactions and "
            "redo_history_preserved (whether the user's redo steps survived the rollback).",
            props, QJsonArray({"commands"})));
    }

    return tools;
}

} // namespace OpenDaw
