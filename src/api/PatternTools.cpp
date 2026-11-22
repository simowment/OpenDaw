#include "api/PatternTools.h"

#include "ai/AiIds.h"
#include "ai/AiToolExecutor.h"
#include "engine/EditManager.h"
#include "engine/PatternManager.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <optional>

namespace OpenDaw {

namespace {

// ── Schema helpers ──────────────────────────────────────────────────────────

QJsonObject makeTool(const QString& name, const QString& description,
                     const QJsonObject& properties, const QJsonArray& required = {})
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

QJsonObject prop(const QString& type, const QString& description)
{
    return QJsonObject{{"type", type}, {"description", description}};
}

QJsonObject propInteger(const QString& description, int min, int max)
{
    return QJsonObject{{"type", "integer"}, {"description", description}, {"minimum", min}, {"maximum", max}};
}

QJsonObject propNumberMin(const QString& description, double min)
{
    return QJsonObject{{"type", "number"}, {"description", description}, {"minimum", min}};
}

QJsonObject typeOnly(const QString& type) { return QJsonObject{{"type", type}}; }

QJsonObject patternRefSchema()
{
    return QJsonObject{
        {"description", "Pattern id like 'pattern:3' (preferred), the pattern number 3, or its exact name."},
        {"oneOf", QJsonArray{typeOnly("string"), typeOnly("integer")}}};
}

QJsonObject trackRefSchema()
{
    QJsonObject objProps;
    objProps["id"] = prop("string", "Track id like 'track:1043'.");
    objProps["index"] = prop("integer", "Zero-based track index.");
    objProps["name"] = prop("string", "Track name.");
    QJsonObject objType{{"type", "object"}, {"properties", objProps}, {"additionalProperties", false}};
    return QJsonObject{
        {"description", "The channel: a MIDI (instrument) track. Track id like 'track:1043' (preferred, "
                        "stable), or its name, or zero-based track index."},
        {"oneOf", QJsonArray{typeOnly("string"), typeOnly("integer"), objType}}};
}

// ── Conversions ─────────────────────────────────────────────────────────────

QString trackName(te::Track* t)
{
    return t ? QString::fromStdString(t->getName().toStdString()) : QString();
}

QString colourHex(const QColor& c) { return c.name(QColor::HexRgb); }

QString stepsString(const QVector<PatternStep>& steps, int stepsPerBeat)
{
    QString s;
    s.reserve(steps.size() + steps.size() / std::max(1, stepsPerBeat));
    for (int i = 0; i < steps.size(); ++i) {
        if (i > 0 && stepsPerBeat > 0 && i % stepsPerBeat == 0) s += QLatin1Char('|');
        const auto& st = steps[i];
        s += st.on ? (st.velocity >= 127 ? QLatin1Char('X') : QLatin1Char('x')) : QLatin1Char('.');
    }
    return s;
}

double barFromBeat(EditManager& em, double beat)
{
    auto* e = em.edit();
    if (!e) return 1.0;
    auto& ts = e->tempoSequence;
    const auto bb = ts.toBarsAndBeats(ts.toTime(tracktion::BeatPosition::fromBeats(beat)));
    const double frac = bb.numerator > 0 ? bb.beats.inBeats() / double(bb.numerator) : 0.0;
    return double(bb.bars) + frac + 1.0;
}

double beatFromBar(EditManager& em, double bar)
{
    auto* e = em.edit();
    const double zeroBased = std::max(0.0, bar - 1.0);
    if (!e) return zeroBased * 4.0;
    auto& ts = e->tempoSequence;
    tracktion::tempo::BarsAndBeats bb;
    bb.bars = int(std::floor(zeroBased));
    const double whole = ts.toBeats(bb).inBeats();
    const double frac = zeroBased - std::floor(zeroBased);
    if (frac <= 0.0) return whole;
    tracktion::tempo::BarsAndBeats next;
    next.bars = bb.bars + 1;
    return whole + (ts.toBeats(next).inBeats() - whole) * frac;
}

double roundBeat(double b) { return std::round(b * 10000.0) / 10000.0; }
double roundStep(double s) { return std::round(s * 10000.0) / 10000.0; }

QJsonArray notesJson(const QVector<PatternNote>& notes, int root)
{
    QJsonArray arr;
    for (const auto& n : notes) {
        QJsonObject o{
            {"start_step", roundStep(n.startStep)},
            {"length_steps", roundStep(n.lengthSteps)},
            {"note", n.note >= 0 ? n.note : root},
            {"velocity", n.velocity}};
        if (n.note < 0) o["root"] = true;
        arr.append(o);
    }
    return arr;
}

QJsonObject channelJson(PatternManager& pm, EditManager& em, const Pattern& p, const PatternChannel& ch)
{
    auto* t = em.findAudioTrack(ch.trackId);
    const int root = pm.channelRootNote(ch.trackId);
    QJsonArray steps;
    for (int i = 0; i < ch.steps.size(); ++i) {
        const auto& st = ch.steps[i];
        if (!st.on) continue;
        steps.append(QJsonObject{{"step", i}, {"velocity", st.velocity}, {"note", st.note >= 0 ? st.note : root}});
    }
    QJsonObject o{
        {"track_id", ids::make(ids::kTrack, ch.trackId)},
        {"track_name", trackName(t)},
        {"root_note", root},
        {"steps_string", stepsString(ch.steps, p.stepsPerBeat)},
        {"steps", steps},
        {"notes", notesJson(ch.notes, root)}};
    const QString sample = pm.channelSamplePath(ch.trackId);
    if (!sample.isEmpty()) o["sample"] = sample;
    return o;
}

QJsonObject sampleJson(const SampleInfo& s)
{
    return QJsonObject{
        {"path", s.path},
        {"name", s.name},
        {"folder", s.folder},
        {"relative_path", s.relativePath},
        {"root", s.root},
        {"size_bytes", double(s.size)}};
}

juce::File juceFile(const QString& path)
{
    return juce::File(juce::String::fromUTF8(QDir::toNativeSeparators(path).toUtf8().constData()));
}

// A sample given as an absolute path or as find_samples' relative_path.
std::optional<AiToolResult> resolveSampleArg(const QJsonObject& input, const QString& id, juce::File& out)
{
    const QString given = input.value("path").toString().trimmed();
    if (given.isEmpty())
        return aiErr(id, AiErrorCode::InvalidArgument, "path must not be empty.");
    const QString resolved = SampleLibrary::resolveSamplePath(given);
    if (resolved.isEmpty())
        return aiErr(id, AiErrorCode::NotFound,
                     QString("Sample '%1' not found. Use find_samples and pass its 'path'.").arg(given));
    out = juceFile(resolved);
    if (!PatternManager::isAudioSampleFile(out))
        return aiErr(id, AiErrorCode::InvalidArgument,
                     QString("'%1' is not a readable audio file (wav, aif, aiff, flac, ogg, mp3).").arg(resolved));
    return std::nullopt;
}

QJsonObject placementJson(EditManager& em, const PatternPlacement& pl, double lengthBeats)
{
    QJsonArray clips;
    for (auto& id : pl.clipIds) clips.append(ids::make(ids::kClip, id));
    return QJsonObject{
        {"placement_id", pl.placementId},
        {"pattern_id", ids::makePattern(pl.patternId)},
        {"start_beat", roundBeat(pl.startBeat)},
        {"start_bar", roundBeat(barFromBeat(em, pl.startBeat))},
        {"end_beat", roundBeat(pl.startBeat + lengthBeats)},
        {"clip_ids", clips}};
}

QJsonObject patternSummary(PatternManager& pm, const Pattern& p)
{
    return QJsonObject{
        {"id", ids::makePattern(p.id)},
        {"name", p.name},
        {"colour", colourHex(p.colour)},
        {"length_steps", p.lengthSteps},
        {"steps_per_beat", p.stepsPerBeat},
        {"length_beats", p.lengthBeats()},
        {"channel_count", int(p.channels.size())},
        {"placement_count", int(pm.placements(p.id).size())},
        {"is_current", pm.currentPattern() == p.id}};
}

QJsonObject patternDetail(PatternManager& pm, EditManager& em, const Pattern& p)
{
    QJsonArray channels;
    for (auto& ch : p.channels) channels.append(channelJson(pm, em, p, ch));
    QJsonArray placements;
    for (auto& pl : pm.placements(p.id)) placements.append(placementJson(em, pl, p.lengthBeats()));
    return QJsonObject{
        {"id", ids::makePattern(p.id)},
        {"name", p.name},
        {"colour", colourHex(p.colour)},
        {"length_steps", p.lengthSteps},
        {"steps_per_beat", p.stepsPerBeat},
        {"length_beats", p.lengthBeats()},
        {"is_current", pm.currentPattern() == p.id},
        {"channels", channels},
        {"placements", placements}};
}

// ── Resolution ──────────────────────────────────────────────────────────────

int resolvePattern(PatternManager& pm, const QJsonValue& v)
{
    if (v.isDouble()) {
        const double d = v.toDouble();
        const int id = int(d);
        if (double(id) != d) return 0;
        return pm.patternExists(id) ? id : 0;
    }
    if (!v.isString()) return 0;
    const QString s = v.toString().trimmed();
    if (auto id = ids::parsePattern(s)) return pm.patternExists(*id) ? *id : 0;
    bool ok = false;
    const int n = s.toInt(&ok);
    if (ok && pm.patternExists(n)) return n;
    for (auto& p : pm.patterns())
        if (p.name.compare(s, Qt::CaseInsensitive) == 0) return p.id;
    return 0;
}

QString refText(const QJsonValue& v)
{
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(v.toDouble());
    if (v.isObject()) {
        auto o = v.toObject();
        if (o.contains("id")) return o.value("id").toString();
        if (o.contains("name")) return o.value("name").toString();
        if (o.contains("index")) return QString::number(o.value("index").toInt());
    }
    return QStringLiteral("?");
}

// Common checks for every handler.
std::optional<AiToolResult> checkEditable(AiToolExecutor& ex, const QString& id, bool mutating)
{
    auto* em = ex.editManager();
    if (!em || !em->edit())
        return aiErr(id, AiErrorCode::Conflict, "No project is open.");
    if (mutating && em->isRenderInProgress())
        return aiErr(id, AiErrorCode::Busy, "A render/export is in progress. Try again when it finishes.");
    return std::nullopt;
}

struct PatternLookup {
    int id = 0;
    std::optional<AiToolResult> error;
};

PatternLookup lookupPattern(PatternManager& pm, const QJsonObject& input, const QString& toolUseId)
{
    PatternLookup r;
    const auto v = input.value("pattern");
    r.id = resolvePattern(pm, v);
    if (r.id == 0) {
        QJsonArray known;
        for (auto& p : pm.patterns()) known.append(ids::makePattern(p.id) + " (" + p.name + ")");
        r.error = aiErr(toolUseId, AiErrorCode::NotFound,
                        QString("Pattern '%1' not found. Use list_patterns.").arg(refText(v)),
                        QJsonObject{{"known_patterns", known}});
    }
    return r;
}

struct ChannelLookup {
    te::AudioTrack* track = nullptr;
    std::optional<AiToolResult> error;
};

ChannelLookup lookupChannel(AiToolExecutor& ex, const QJsonObject& input, const QString& toolUseId)
{
    ChannelLookup r;
    const auto v = input.value("track");
    r.track = ex.resolveTrack(v);
    auto* em = ex.editManager();
    if (!r.track) {
        r.error = aiErr(toolUseId, AiErrorCode::NotFound,
                        QString("Track '%1' not found. Use list_channels.").arg(refText(v)));
        return r;
    }
    if (!em->isMidiTrack(r.track) || em->isBusTrack(r.track)) {
        r.error = aiErr(toolUseId, AiErrorCode::InvalidArgument,
                        QString("Track '%1' is not a MIDI (instrument) track, so it is not a channel. "
                                "Create one with create_midi_track, or use list_channels.")
                            .arg(trackName(r.track)),
                        QJsonObject{{"track_id", ids::of(r.track)}});
        r.track = nullptr;
    }
    return r;
}

AiToolFlags readOnlyFlags()
{
    AiToolFlags f;
    f.mutating = false;
    f.destructive = false;
    f.batchable = true;
    return f;
}

AiToolFlags mutatingFlags(bool destructive = false)
{
    AiToolFlags f;
    f.mutating = true;
    f.destructive = destructive;
    f.batchable = true;
    return f;
}

const char* kModel =
    "OpenDaw has an FL-Studio-style pattern workflow. Channels = the MIDI (instrument) tracks (see "
    "list_channels); a channel's instrument (sampler, SoundFont, VST/VST3 synth) is its sound. A pattern "
    "holds notes per channel, positioned in steps (default 16 steps of a 1/16 note, 4 steps per beat = "
    "one 4/4 bar). The step-sequencer view of a channel is its steps: a step is 'on' when a note starts "
    "exactly on it; set_pattern_steps toggles 1-step notes playing the channel's root note (or the step's "
    "own note), set_pattern_notes writes real melodies/chords (any length, off-grid positions). Placing a "
    "pattern on the playlist (place_pattern) creates one linked MIDI clip per used channel at that "
    "position; every placement is rebuilt automatically when the pattern changes. Editing the notes of a "
    "placement clip (piano roll, MIDI note tools) writes them back into the pattern and updates its other "
    "placements; a placement clip moved to another track becomes an ordinary clip.";

const char* kSampleFlow =
    "Building a beat from drum samples: find_samples {query:'kick'} -> add_sample_channel {path:<a result's "
    "path>} (repeat for snare, hat, clap...) -> create_pattern -> set_pattern_steps {pattern, track:<the "
    "channel's track_id>, steps_string:'x...x...x...x...'} per channel -> place_pattern -> render_region to "
    "listen (its analysis tells you whether it is audible).";

} // namespace

QVector<AiToolExtension> patternToolExtensions(PatternManager& pm, AiToolExecutor& ex)
{
    QVector<AiToolExtension> tools;
    PatternManager* pmp = &pm;
    AiToolExecutor* exp = &ex;

    // ── list_patterns ───────────────────────────────────────────────────────
    {
        AiToolExtension t;
        t.definition = makeTool("list_patterns",
            QString(kModel) + " Lists every pattern: {patterns:[{id 'pattern:<n>', name, colour, length_steps, "
            "steps_per_beat, length_beats, channel_count, placement_count, is_current}], current_pattern, "
            "mode ('pattern' or 'song')}. Use get_pattern for the steps.",
            {});
        t.handler = [pmp, exp](const QJsonObject&, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, false)) return *e;
            QJsonArray list;
            for (auto& p : pmp->patterns()) list.append(patternSummary(*pmp, p));
            const int cur = pmp->currentPattern();
            return aiOk(id, QJsonObject{
                {"patterns", list},
                {"current_pattern", cur ? QJsonValue(ids::makePattern(cur)) : QJsonValue(QJsonValue::Null)},
                {"mode", pmp->mode() == PatternManager::Mode::Song ? "song" : "pattern"}});
        };
        t.flags = readOnlyFlags();
        tools.append(t);
    }

    // ── get_pattern ─────────────────────────────────────────────────────────
    {
        QJsonObject props;
        props["pattern"] = patternRefSchema();
        AiToolExtension t;
        t.definition = makeTool("get_pattern",
            "Returns one pattern: {id, name, colour, length_steps, steps_per_beat, length_beats, channels:"
            "[{track_id, track_name, root_note, sample?, steps_string, steps:[{step, velocity, note}], notes:"
            "[{start_step, length_steps, note, velocity, root?}]}], placements:[{placement_id, pattern_id, "
            "start_beat, start_bar, end_beat, clip_ids}]}. Only channels with at least one note are listed. "
            "notes is the full content (what plays); steps is the step-sequencer view (a step is on when a "
            "note starts exactly on it). root:true marks notes that follow the channel's root note. "
            "steps_string shows one character per step: 'x' on, 'X' accent (velocity 127), '.' off, with '|' "
            "between beats. Steps are 0-based.",
            props, {"pattern"});
        t.handler = [pmp, exp](const QJsonObject& input, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, false)) return *e;
            auto pl = lookupPattern(*pmp, input, id);
            if (pl.error) return *pl.error;
            auto p = pmp->pattern(pl.id);
            if (!p) return aiErr(id, AiErrorCode::NotFound, "Pattern not found.");
            return aiOk(id, patternDetail(*pmp, *exp->editManager(), *p));
        };
        t.flags = readOnlyFlags();
        tools.append(t);
    }

    // ── create_pattern ──────────────────────────────────────────────────────
    {
        QJsonObject props;
        props["name"] = prop("string", "Pattern name. Default 'Pattern <n>'.");
        props["length_steps"] = propInteger("Number of 1/16 steps. Default 16 (one 4/4 bar); 32 = two bars.", 1, 256);
        AiToolExtension t;
        t.definition = makeTool("create_pattern",
            QString(kModel) + " Creates an empty pattern, makes it the current one and returns it like "
            "get_pattern. Typical drum beat in one run_batch: create_pattern {name:'Beat'} -> "
            "set_pattern_steps {pattern:'pattern:1', track:'Kick', steps_string:'x...x...x...x...'} -> "
            "set_pattern_steps {... track:'Snare', steps_string:'....x.......x...'} -> set_pattern_steps "
            "{... track:'Hats', steps_string:'x.x.x.x.x.x.x.x.'} -> place_pattern {pattern:'pattern:1', "
            "start_bar:1} (repeat place_pattern without a position to append it bar after bar).",
            props);
        t.handler = [pmp, exp](const QJsonObject& input, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, true)) return *e;
            const QString name = input.value("name").toString().trimmed();
            if (input.contains("name") && name.isEmpty())
                return aiErr(id, AiErrorCode::InvalidArgument, "name must not be empty.");
            const int length = input.contains("length_steps") ? input.value("length_steps").toInt() : 16;
            PatternManager::ScopedExternalTransaction tx(*pmp);
            const int pid = pmp->createPattern(name);
            if (pid == 0) return aiErr(id, AiErrorCode::Failed, "Could not create the pattern.");
            if (length != 16 && !pmp->setPatternLength(pid, length))
                return aiErr(id, AiErrorCode::Failed, "Pattern created but its length could not be set.");
            pmp->setCurrentPattern(pid);
            auto p = pmp->pattern(pid);
            if (!p) return aiErr(id, AiErrorCode::Internal, "Pattern vanished after creation.");
            return aiOk(id, patternDetail(*pmp, *exp->editManager(), *p));
        };
        t.flags = mutatingFlags();
        tools.append(t);
    }

    // ── clone_pattern ───────────────────────────────────────────────────────
    {
        QJsonObject props;
        props["pattern"] = patternRefSchema();
        props["name"] = prop("string", "Name of the copy. Default '<name> (copy)'.");
        AiToolExtension t;
        t.definition = makeTool("clone_pattern",
            "Copies a pattern's steps into a new, independent pattern (not placed anywhere) and returns it "
            "like get_pattern. Use it for variations (fills, breakdowns): clone, edit the copy, place it.",
            props, {"pattern"});
        t.handler = [pmp, exp](const QJsonObject& input, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, true)) return *e;
            auto pl = lookupPattern(*pmp, input, id);
            if (pl.error) return *pl.error;
            const QString name = input.value("name").toString().trimmed();
            if (input.contains("name") && name.isEmpty())
                return aiErr(id, AiErrorCode::InvalidArgument, "name must not be empty.");
            PatternManager::ScopedExternalTransaction tx(*pmp);
            const int pid = pmp->clonePattern(pl.id);
            if (pid == 0) return aiErr(id, AiErrorCode::Failed, "Could not clone the pattern.");
            if (!name.isEmpty()) pmp->renamePattern(pid, name);
            auto p = pmp->pattern(pid);
            if (!p) return aiErr(id, AiErrorCode::Internal, "Pattern vanished after cloning.");
            return aiOk(id, patternDetail(*pmp, *exp->editManager(), *p));
        };
        t.flags = mutatingFlags();
        tools.append(t);
    }

    // ── delete_pattern ──────────────────────────────────────────────────────
    {
        QJsonObject props;
        props["pattern"] = patternRefSchema();
        AiToolExtension t;
        t.definition = makeTool("delete_pattern",
            "Deletes a pattern and every placement of it (their MIDI clips are removed from the playlist). "
            "Returns {deleted, removed_placements}.",
            props, {"pattern"});
        t.handler = [pmp, exp](const QJsonObject& input, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, true)) return *e;
            auto pl = lookupPattern(*pmp, input, id);
            if (pl.error) return *pl.error;
            const int placements = int(pmp->placements(pl.id).size());
            PatternManager::ScopedExternalTransaction tx(*pmp);
            if (!pmp->removePattern(pl.id))
                return aiErr(id, AiErrorCode::Failed, "Could not delete the pattern.");
            return aiOk(id, QJsonObject{{"deleted", ids::makePattern(pl.id)},
                                        {"removed_placements", placements}});
        };
        t.flags = mutatingFlags(true);
        tools.append(t);
    }

    // ── rename_pattern ──────────────────────────────────────────────────────
    {
        QJsonObject props;
        props["pattern"] = patternRefSchema();
        props["name"] = prop("string", "New name.");
        AiToolExtension t;
        t.definition = makeTool("rename_pattern",
            "Renames a pattern; its placed clips are renamed too. Returns the pattern summary.",
            props, {"pattern", "name"});
        t.handler = [pmp, exp](const QJsonObject& input, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, true)) return *e;
            auto pl = lookupPattern(*pmp, input, id);
            if (pl.error) return *pl.error;
            const QString name = input.value("name").toString().trimmed();
            if (name.isEmpty()) return aiErr(id, AiErrorCode::InvalidArgument, "name must not be empty.");
            PatternManager::ScopedExternalTransaction tx(*pmp);
            if (!pmp->renamePattern(pl.id, name))
                return aiErr(id, AiErrorCode::Failed, "Could not rename the pattern.");
            auto p = pmp->pattern(pl.id);
            if (!p) return aiErr(id, AiErrorCode::Internal, "Pattern vanished.");
            return aiOk(id, patternSummary(*pmp, *p));
        };
        t.flags = mutatingFlags();
        tools.append(t);
    }

    // ── set_pattern_length ──────────────────────────────────────────────────
    {
        QJsonObject props;
        props["pattern"] = patternRefSchema();
        props["length_steps"] = propInteger("New length in steps (16 = one 4/4 bar of 1/16 notes).", 1, 256);
        AiToolExtension t;
        t.definition = makeTool("set_pattern_length",
            "Changes a pattern's length. Shortening drops the steps past the end; lengthening adds empty "
            "steps. Every placement's clips get the new length. Returns the pattern like get_pattern.",
            props, {"pattern", "length_steps"});
        t.handler = [pmp, exp](const QJsonObject& input, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, true)) return *e;
            auto pl = lookupPattern(*pmp, input, id);
            if (pl.error) return *pl.error;
            const int length = input.value("length_steps").toInt();
            if (length < 1 || length > 256)
                return aiErr(id, AiErrorCode::InvalidArgument, "length_steps must be 1..256.");
            PatternManager::ScopedExternalTransaction tx(*pmp);
            if (!pmp->setPatternLength(pl.id, length))
                return aiErr(id, AiErrorCode::Failed, "Could not change the pattern length.");
            auto p = pmp->pattern(pl.id);
            if (!p) return aiErr(id, AiErrorCode::Internal, "Pattern vanished.");
            return aiOk(id, patternDetail(*pmp, *exp->editManager(), *p));
        };
        t.flags = mutatingFlags();
        tools.append(t);
    }

    // ── set_pattern_steps ───────────────────────────────────────────────────
    {
        QJsonObject stepProps;
        stepProps["step"] = QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 255},
                                        {"description", "0-based step index."}};
        stepProps["on"] = prop("boolean", "Default true. false turns the step off.");
        stepProps["velocity"] = propInteger("1..127. Default: unchanged (100 for a new step).", 1, 127);
        stepProps["note"] = propInteger("MIDI note for this step only (60 = C4). Default: unchanged "
                                        "(the channel's root note for a new step).", 0, 127);
        QJsonObject stepSchema{{"type", "object"}, {"properties", stepProps},
                               {"required", QJsonArray{"step"}}, {"additionalProperties", false}};

        QJsonObject props;
        props["pattern"] = patternRefSchema();
        props["track"] = trackRefSchema();
        props["steps_string"] = prop("string",
            "The whole row, one character per step: 'x' on (velocity 100), 'X' accent (velocity 127), "
            "'.' or '-' off. Spaces and '|' are ignored, so 'x...|x...|x...|x...' works. After removing "
            "them its length must equal the pattern's length_steps. Replaces the row; new steps play the "
            "root note. Notes that do not start on a step (off-grid) are kept: use set_pattern_notes to "
            "replace everything.");
        props["steps"] = QJsonObject{
            {"type", "array"}, {"items", stepSchema}, {"minItems", 1},
            {"description", "Individual steps to change, e.g. [{step:0}, {step:8, velocity:80}, "
                            "{step:14, note:62}]. Steps not listed keep their state unless clear_first."}};
        props["clear_first"] = prop("boolean", "With 'steps': turn the whole row off first. Default false.");
        AiToolExtension t;
        t.definition = makeTool("set_pattern_steps",
            QString(kModel) + " Sets one channel's steps in a pattern; every placement of the pattern "
            "is updated. Give exactly one of steps_string (whole row) or steps (individual steps). "
            "A step turned on gets a 1-step note, a step turned off loses the notes starting on it, a "
            "step that stays on keeps its note (and length). Returns {pattern_id, channel:{track_id, "
            "track_name, root_note, steps_string, steps, notes}, placements_updated}. The channel leaves "
            "the pattern when no note is left. For melodies, chords and long notes use set_pattern_notes.",
            props, {"pattern", "track"});
        t.handler = [pmp, exp](const QJsonObject& input, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, true)) return *e;
            const bool hasString = input.contains("steps_string");
            const bool hasArray = input.contains("steps");
            if (hasString == hasArray)
                return aiErr(id, AiErrorCode::InvalidArgument, "Give exactly one of steps_string or steps.");
            if (hasString && input.contains("clear_first"))
                return aiErr(id, AiErrorCode::InvalidArgument,
                             "clear_first only applies to 'steps'; steps_string always replaces the row.");

            auto pl = lookupPattern(*pmp, input, id);
            if (pl.error) return *pl.error;
            auto ch = lookupChannel(*exp, input, id);
            if (ch.error) return *ch.error;
            auto p = pmp->pattern(pl.id);
            if (!p) return aiErr(id, AiErrorCode::NotFound, "Pattern not found.");
            const int len = p->lengthSteps;
            const auto trackId = ch.track->itemID;

            QVector<PatternStep> row(len);
            if (hasString) {
                const QString raw = input.value("steps_string").toString();
                QString cleaned;
                for (const QChar c : raw) {
                    if (c.isSpace() || c == QLatin1Char('|')) continue;
                    cleaned += c;
                }
                if (cleaned.size() != len)
                    return aiErr(id, AiErrorCode::InvalidArgument,
                                 QString("steps_string has %1 steps but the pattern has %2 (length_steps). "
                                         "Use one character per step, or set_pattern_length first.")
                                     .arg(cleaned.size()).arg(len),
                                 QJsonObject{{"expected", len}, {"got", int(cleaned.size())}});
                for (int i = 0; i < len; ++i) {
                    const QChar c = cleaned[i];
                    if (c == QLatin1Char('x')) { row[i].on = true; row[i].velocity = 100; }
                    else if (c == QLatin1Char('X')) { row[i].on = true; row[i].velocity = 127; }
                    else if (c == QLatin1Char('.') || c == QLatin1Char('-')) { /* off */ }
                    else
                        return aiErr(id, AiErrorCode::InvalidArgument,
                                     QString("steps_string: unexpected character '%1' at step %2. Use 'x', "
                                             "'X', '.' or '-'.").arg(c).arg(i));
                }
            } else {
                const bool clearFirst = input.value("clear_first").toBool(false);
                if (!clearFirst)
                    if (auto* existing = p->channel(trackId)) row = existing->steps;
                row.resize(len);
                const auto arr = input.value("steps").toArray();
                for (int k = 0; k < arr.size(); ++k) {
                    const auto o = arr[k].toObject();
                    const int step = o.value("step").toInt(-1);
                    if (step < 0 || step >= len)
                        return aiErr(id, AiErrorCode::InvalidArgument,
                                     QString("steps[%1].step = %2 is outside the pattern (0..%3).")
                                         .arg(k).arg(step).arg(len - 1));
                    auto& st = row[step];
                    st.on = o.value("on").toBool(true);
                    if (o.contains("velocity")) st.velocity = std::clamp(o.value("velocity").toInt(), 1, 127);
                    if (o.contains("note")) st.note = std::clamp(o.value("note").toInt(), 0, 127);
                }
            }

            PatternManager::ScopedExternalTransaction tx(*pmp);
            if (!pmp->setChannelSteps(pl.id, trackId, row))
                return aiErr(id, AiErrorCode::Failed, "Could not write the steps.");
            auto after = pmp->pattern(pl.id);
            if (!after) return aiErr(id, AiErrorCode::Internal, "Pattern vanished.");

            QJsonObject channel;
            if (auto* c = after->channel(trackId)) {
                channel = channelJson(*pmp, *exp->editManager(), *after, *c);
            } else {
                PatternChannel empty;
                empty.trackId = trackId;
                empty.steps = QVector<PatternStep>(after->lengthSteps);
                channel = channelJson(*pmp, *exp->editManager(), *after, empty);
            }
            return aiOk(id, QJsonObject{{"pattern_id", ids::makePattern(pl.id)},
                                        {"channel", channel},
                                        {"placements_updated", int(pmp->placements(pl.id).size())}});
        };
        t.flags = mutatingFlags();
        tools.append(t);
    }

    // ── set_pattern_notes ───────────────────────────────────────────────────
    {
        QJsonObject noteProps;
        noteProps["start_step"] = QJsonObject{{"type", "number"}, {"minimum", 0},
            {"description", "Start, in steps from the pattern start (0-based; fractions allowed: 2.5 = "
                            "half way through step 2)."}};
        noteProps["length_steps"] = QJsonObject{{"type", "number"}, {"exclusiveMinimum", 0},
            {"description", "Length in steps (> 0; 1 = a 1/16 note at 4 steps per beat, 4 = a beat). A note "
                            "running past the pattern end is cut there."}};
        noteProps["note"] = propInteger("MIDI note (60 = C4). Default: the channel's root note (the note "
                                        "then follows set_channel_root_note).", 0, 127);
        noteProps["velocity"] = propInteger("1..127. Default 100.", 1, 127);
        QJsonObject noteSchema{{"type", "object"}, {"properties", noteProps},
                               {"required", QJsonArray{"start_step", "length_steps"}},
                               {"additionalProperties", false}};

        QJsonObject props;
        props["pattern"] = patternRefSchema();
        props["track"] = trackRefSchema();
        props["notes"] = QJsonObject{
            {"type", "array"}, {"items", noteSchema},
            {"description", "The notes, e.g. a C minor chord on beat 1 held for a beat and a melody note: "
                            "[{start_step:0, length_steps:4, note:60}, {start_step:0, length_steps:4, note:63}, "
                            "{start_step:0, length_steps:4, note:67}, {start_step:6, length_steps:2, note:70, "
                            "velocity:90}]. An empty array with replace:true removes the channel's notes."}};
        props["replace"] = prop("boolean",
            "Default true: the notes replace every note of the channel in this pattern. false: they are added; "
            "an existing note with the same start and pitch is replaced.");
        AiToolExtension t;
        t.definition = makeTool("set_pattern_notes",
            QString(kModel) + " Writes one channel's notes in a pattern (melodies, chords, bass lines, long "
            "or off-grid notes; set_pattern_steps is the drum-grid shortcut). Every placement of the pattern "
            "is updated. Notes starting at or after the pattern end are rejected; extend it first with "
            "set_pattern_length. Returns {pattern_id, channel:{track_id, track_name, root_note, steps_string, "
            "steps, notes}, placements_updated, clipped_notes}.",
            props, {"pattern", "track", "notes"});
        t.handler = [pmp, exp](const QJsonObject& input, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, true)) return *e;
            auto pl = lookupPattern(*pmp, input, id);
            if (pl.error) return *pl.error;
            auto ch = lookupChannel(*exp, input, id);
            if (ch.error) return *ch.error;
            auto p = pmp->pattern(pl.id);
            if (!p) return aiErr(id, AiErrorCode::NotFound, "Pattern not found.");
            const int len = p->lengthSteps;
            const auto trackId = ch.track->itemID;
            const bool replace = input.value("replace").toBool(true);
            const int root = pmp->channelRootNote(trackId);

            QVector<PatternNote> given;
            int clipped = 0;
            const auto arr = input.value("notes").toArray();
            for (int k = 0; k < arr.size(); ++k) {
                const auto o = arr[k].toObject();
                PatternNote n;
                n.startStep = o.value("start_step").toDouble(-1.0);
                n.lengthSteps = o.value("length_steps").toDouble(0.0);
                n.note = o.contains("note") ? std::clamp(o.value("note").toInt(), 0, 127) : -1;
                n.velocity = o.contains("velocity") ? std::clamp(o.value("velocity").toInt(), 1, 127) : 100;
                if (!std::isfinite(n.startStep) || n.startStep < 0.0 || n.startStep >= double(len))
                    return aiErr(id, AiErrorCode::InvalidArgument,
                                 QString("notes[%1].start_step = %2 is outside the pattern (0 to below %3). "
                                         "Use set_pattern_length first for a longer pattern.")
                                     .arg(k).arg(n.startStep).arg(len),
                                 QJsonObject{{"length_steps", len}});
                if (!std::isfinite(n.lengthSteps) || n.lengthSteps <= 0.0)
                    return aiErr(id, AiErrorCode::InvalidArgument,
                                 QString("notes[%1].length_steps must be greater than 0.").arg(k));
                if (n.startStep + n.lengthSteps > double(len) + 1.0e-9) {
                    n.lengthSteps = double(len) - n.startStep;
                    ++clipped;
                }
                given.push_back(n);
            }

            QVector<PatternNote> notes;
            if (replace) {
                notes = given;
            } else {
                notes = pmp->channelNotes(pl.id, trackId);
                auto pitchOf = [root](const PatternNote& n) { return n.note >= 0 ? n.note : root; };
                for (const auto& n : given) {
                    notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const PatternNote& e) {
                                    return std::abs(e.startStep - n.startStep) < 1.0e-6 && pitchOf(e) == pitchOf(n);
                                }),
                                notes.end());
                    notes.push_back(n);
                }
            }

            PatternManager::ScopedExternalTransaction tx(*pmp);
            if (!pmp->setChannelNotes(pl.id, trackId, notes))
                return aiErr(id, AiErrorCode::Failed, "Could not write the notes.");
            auto after = pmp->pattern(pl.id);
            if (!after) return aiErr(id, AiErrorCode::Internal, "Pattern vanished.");

            QJsonObject channel;
            if (auto* c = after->channel(trackId)) {
                channel = channelJson(*pmp, *exp->editManager(), *after, *c);
            } else {
                PatternChannel empty;
                empty.trackId = trackId;
                empty.steps = QVector<PatternStep>(after->lengthSteps);
                channel = channelJson(*pmp, *exp->editManager(), *after, empty);
            }
            return aiOk(id, QJsonObject{{"pattern_id", ids::makePattern(pl.id)},
                                        {"channel", channel},
                                        {"placements_updated", int(pmp->placements(pl.id).size())},
                                        {"clipped_notes", clipped}});
        };
        t.flags = mutatingFlags();
        tools.append(t);
    }

    // ── clear_pattern_channel ───────────────────────────────────────────────
    {
        QJsonObject props;
        props["pattern"] = patternRefSchema();
        props["track"] = trackRefSchema();
        AiToolExtension t;
        t.definition = makeTool("clear_pattern_channel",
            "Removes every note of one channel in a pattern (the channel's clips leave its placements). "
            "Returns {pattern_id, track_id, cleared}.",
            props, {"pattern", "track"});
        t.handler = [pmp, exp](const QJsonObject& input, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, true)) return *e;
            auto pl = lookupPattern(*pmp, input, id);
            if (pl.error) return *pl.error;
            auto ch = lookupChannel(*exp, input, id);
            if (ch.error) return *ch.error;
            auto p = pmp->pattern(pl.id);
            const bool had = p && p->channel(ch.track->itemID) != nullptr;
            PatternManager::ScopedExternalTransaction tx(*pmp);
            if (!pmp->clearChannel(pl.id, ch.track->itemID))
                return aiErr(id, AiErrorCode::Failed, "Could not clear the channel.");
            return aiOk(id, QJsonObject{{"pattern_id", ids::makePattern(pl.id)},
                                        {"track_id", ids::of(ch.track)},
                                        {"cleared", had}});
        };
        t.flags = mutatingFlags();
        tools.append(t);
    }

    // ── place_pattern ───────────────────────────────────────────────────────
    {
        QJsonObject props;
        props["pattern"] = patternRefSchema();
        props["start_bar"] = propNumberMin("1-based bar where the placement starts (1 = song start).", 1.0);
        props["start_beat"] = propNumberMin("Start position in beats (0 = song start). Alternative to start_bar.", 0.0);
        AiToolExtension t;
        t.definition = makeTool("place_pattern",
            "Puts a pattern on the playlist (arrangement): creates one linked MIDI clip per used channel, "
            "on that channel's track. Give start_bar or start_beat (not both); with neither, the pattern "
            "is appended right after the last placement of any pattern (beat 0 if there are none), so "
            "calling it N times lays the pattern out back to back. Placements of the same pattern stay "
            "linked: editing the pattern updates all of them. An empty pattern gets one empty clip on the "
            "first channel. Returns {placement_id, pattern_id, start_beat, start_bar, end_beat, clip_ids}.",
            props, {"pattern"});
        t.handler = [pmp, exp](const QJsonObject& input, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, true)) return *e;
            auto* em = exp->editManager();
            auto pl = lookupPattern(*pmp, input, id);
            if (pl.error) return *pl.error;
            if (input.contains("start_bar") && input.contains("start_beat"))
                return aiErr(id, AiErrorCode::InvalidArgument, "Give start_bar or start_beat, not both.");
            auto p = pmp->pattern(pl.id);
            if (!p) return aiErr(id, AiErrorCode::NotFound, "Pattern not found.");
            if (p->channels.isEmpty() && pmp->channels().isEmpty())
                return aiErr(id, AiErrorCode::Conflict,
                             "The project has no MIDI tracks to hold the pattern. Create one with create_midi_track.");

            double start = 0.0;
            if (input.contains("start_bar")) {
                start = beatFromBar(*em, input.value("start_bar").toDouble(1.0));
            } else if (input.contains("start_beat")) {
                start = std::max(0.0, input.value("start_beat").toDouble(0.0));
            } else {
                for (auto& other : pmp->placements(0)) {
                    auto op = pmp->pattern(other.patternId);
                    const double len = op ? op->lengthBeats() : 0.0;
                    start = std::max(start, other.startBeat + len);
                }
            }

            PatternManager::ScopedExternalTransaction tx(*pmp);
            const int placementId = pmp->placePattern(pl.id, start);
            if (placementId == 0) return aiErr(id, AiErrorCode::Failed, "Could not place the pattern.");
            for (auto& placed : pmp->placements(pl.id))
                if (placed.placementId == placementId)
                    return aiOk(id, placementJson(*em, placed, p->lengthBeats()));
            return aiErr(id, AiErrorCode::Internal, "The placement was created but cannot be found.");
        };
        t.flags = mutatingFlags();
        tools.append(t);
    }

    // ── remove_pattern_placement ────────────────────────────────────────────
    {
        QJsonObject props;
        props["placement_id"] = QJsonObject{{"type", "integer"}, {"minimum", 1},
                                            {"description", "placement_id from get_pattern or place_pattern."}};
        AiToolExtension t;
        t.definition = makeTool("remove_pattern_placement",
            "Removes one placement of a pattern from the playlist (deletes its MIDI clips). The pattern "
            "itself is kept. Returns {removed, pattern_id}.",
            props, {"placement_id"});
        t.handler = [pmp, exp](const QJsonObject& input, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, true)) return *e;
            const int placementId = input.value("placement_id").toInt(0);
            int patternId = 0;
            for (auto& pl : pmp->placements(0))
                if (pl.placementId == placementId) { patternId = pl.patternId; break; }
            if (patternId == 0)
                return aiErr(id, AiErrorCode::NotFound,
                             QString("Placement %1 not found. Use get_pattern to list placements.").arg(placementId));
            PatternManager::ScopedExternalTransaction tx(*pmp);
            if (!pmp->removePlacement(placementId))
                return aiErr(id, AiErrorCode::Failed, "Could not remove the placement.");
            return aiOk(id, QJsonObject{{"removed", placementId}, {"pattern_id", ids::makePattern(patternId)}});
        };
        t.flags = mutatingFlags(true);
        tools.append(t);
    }

    // ── list_channels ───────────────────────────────────────────────────────
    {
        AiToolExtension t;
        t.definition = makeTool("list_channels",
            "Lists the channels of the channel rack: every MIDI (instrument) track in display order, "
            "{channels:[{track_id, name, root_note, instrument, sample?}]}; 'sample' is the audio file of a "
            "sample channel. Each channel is one row of steps in every pattern. To add a channel: "
            "add_sample_channel for a drum/one-shot sample (see find_samples), or create a MIDI track "
            "(create_midi_track) and give it an instrument (set_track_instrument).",
            {});
        t.handler = [pmp, exp](const QJsonObject&, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, false)) return *e;
            auto* em = exp->editManager();
            QJsonArray list;
            for (auto* track : pmp->channels()) {
                auto* inst = em->getTrackInstrument(track);
                QJsonObject o{
                    {"track_id", ids::of(track)},
                    {"name", trackName(track)},
                    {"root_note", pmp->channelRootNote(track->itemID)},
                    {"instrument", inst ? QJsonValue(QString::fromStdString(inst->getName().toStdString()))
                                        : QJsonValue(QJsonValue::Null)}};
                const QString sample = pmp->channelSamplePath(track->itemID);
                if (!sample.isEmpty()) o["sample"] = sample;
                list.append(o);
            }
            return aiOk(id, QJsonObject{{"channels", list}});
        };
        t.flags = readOnlyFlags();
        tools.append(t);
    }

    // ── set_channel_root_note ───────────────────────────────────────────────
    {
        QJsonObject props;
        props["track"] = trackRefSchema();
        props["note"] = propInteger("MIDI note 0..127 that 'on' steps play (60 = C4, default). For a "
                                    "General MIDI drum kit: 36 kick, 38 snare, 42 closed hat, 46 open hat, "
                                    "39 clap.", 0, 127);
        AiToolExtension t;
        t.definition = makeTool("set_channel_root_note",
            "Sets the note a channel's steps play when they have no note of their own, in every pattern "
            "(placements are updated). Useful when several channels share a drum-kit instrument, one "
            "drum sound per channel. Returns {track_id, root_note}.",
            props, {"track", "note"});
        t.handler = [pmp, exp](const QJsonObject& input, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, true)) return *e;
            auto ch = lookupChannel(*exp, input, id);
            if (ch.error) return *ch.error;
            const int note = input.value("note").toInt(-1);
            if (note < 0 || note > 127) return aiErr(id, AiErrorCode::InvalidArgument, "note must be 0..127.");
            PatternManager::ScopedExternalTransaction tx(*pmp);
            pmp->setChannelRootNote(ch.track->itemID, note);
            return aiOk(id, QJsonObject{{"track_id", ids::of(ch.track)},
                                        {"root_note", pmp->channelRootNote(ch.track->itemID)}});
        };
        t.flags = mutatingFlags();
        tools.append(t);
    }

    // ── list_sample_folders ─────────────────────────────────────────────────
    {
        QJsonObject props;
        props["folder"] = prop("string",
            "A folder to look into: absolute, or relative to a sample root (e.g. 'Drums' or 'Drums/Kicks'). "
            "Omit it to list the sample roots.");
        AiToolExtension t;
        t.definition = makeTool("list_sample_folders",
            "Browses the sample library (one-shot drum and FX samples for sample channels). Without 'folder': "
            "the sample roots {roots:[{path, name, sample_count}]} - the folders listed in the settings, every "
            "FL Studio pack folder found on any drive (.../Image-Line/FL Studio*/Data/Patches/Packs) and the "
            "user's Music folder. With 'folder': {folder, samples_here, folders:[{path, name, relative_path, "
            "sample_count}]} - its sub-folders that contain samples, with recursive counts. Usually "
            "find_samples is faster: it searches every root at once. " + QString(kSampleFlow),
            props);
        t.handler = [](const QJsonObject& input, const QString& id) -> AiToolResult {
            const QString folder = input.value("folder").toString().trimmed();
            QString resolved, error;
            int direct = 0;
            const auto list = SampleLibrary::listFolders(folder, &resolved, &direct, &error);
            if (!error.isEmpty()) return aiErr(id, AiErrorCode::NotFound, error);
            QJsonArray arr;
            for (const auto& f : list) {
                QJsonObject o{{"path", f.path}, {"name", f.name}, {"sample_count", f.sampleCount}};
                if (!folder.isEmpty()) o["relative_path"] = f.relativePath;
                arr.append(o);
            }
            if (folder.isEmpty()) {
                QJsonObject out{{"roots", arr}};
                if (arr.isEmpty())
                    out["hint"] = "No sample folder found. The user can add folders to the 'samples/roots' "
                                  "setting, or pass an absolute path to find_samples {folder}.";
                return aiOk(id, out);
            }
            return aiOk(id, QJsonObject{{"folder", resolved}, {"samples_here", direct}, {"folders", arr}});
        };
        t.flags = readOnlyFlags();
        tools.append(t);
    }

    // ── find_samples ────────────────────────────────────────────────────────
    {
        QJsonObject props;
        props["query"] = prop("string",
            "Words that must all appear in the sample's path, case-insensitive, e.g. 'kick', 'snare tight', "
            "'open hat', '808'. Spaces, '-' and '_' are ignored inside words ('hihat' finds 'Hi Hats'). "
            "Omit it to list everything in 'folder'.");
        props["folder"] = prop("string",
            "Limit the search to this folder and below: absolute, or relative to a sample root (e.g. "
            "'Drums/Kicks'). Default: every sample root.");
        props["limit"] = propInteger("Maximum results, 1..200. Default 25.", 1, 200);
        AiToolExtension t;
        t.definition = makeTool("find_samples",
            "Searches the sample library (the folders list_sample_folders shows: FL Studio packs, the user's "
            "Music folder, configured folders) for wav/aif/flac/ogg/mp3 files. Returns {samples:[{path, name, "
            "folder, relative_path, root, size_bytes}], total_matches, truncated}; samples whose file name "
            "matches rank before those that only match a folder name. " + QString(kSampleFlow),
            props);
        t.handler = [](const QJsonObject& input, const QString& id) -> AiToolResult {
            const QString query = input.value("query").toString();
            const QString folder = input.value("folder").toString().trimmed();
            const int limit = input.contains("limit") ? input.value("limit").toInt(25) : 25;
            if (limit < 1 || limit > 200)
                return aiErr(id, AiErrorCode::InvalidArgument, "limit must be 1..200.");
            if (!folder.isEmpty()) {
                QString error;
                SampleLibrary::listFolders(folder, nullptr, nullptr, &error);
                if (!error.isEmpty()) return aiErr(id, AiErrorCode::NotFound, error);
            }
            int total = 0;
            const auto found = SampleLibrary::findSamples(query, folder, limit, &total);
            QJsonArray arr;
            for (const auto& s : found) arr.append(sampleJson(s));
            QJsonObject out{{"samples", arr}, {"total_matches", total}, {"truncated", total > int(found.size())}};
            if (found.isEmpty() && SampleLibrary::sampleRoots().isEmpty())
                out["hint"] = "No sample folder found. Pass an absolute folder, or ask the user where their "
                              "samples are.";
            return aiOk(id, out);
        };
        t.flags = readOnlyFlags();
        tools.append(t);
    }

    // ── add_sample_channel ──────────────────────────────────────────────────
    {
        QJsonObject props;
        props["path"] = prop("string",
            "The audio file: a 'path' from find_samples (absolute), or its relative_path.");
        props["name"] = prop("string", "Channel (track) name. Default: the file name without extension.");
        AiToolExtension t;
        t.definition = makeTool("add_sample_channel",
            "Adds a sample channel to the channel rack: a new MIDI track whose instrument is a sampler "
            "playing this one audio file as a one-shot (each 'on' step plays the whole sample at its "
            "original pitch; a higher root/step note plays it higher). Returns {track_id, name, sample, "
            "root_note}. Use the track_id as 'track' in set_pattern_steps. " + QString(kSampleFlow),
            props, {"path"});
        t.handler = [pmp, exp](const QJsonObject& input, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, true)) return *e;
            juce::File file;
            if (auto e = resolveSampleArg(input, id, file)) return *e;
            const QString name = input.value("name").toString().trimmed();
            if (input.contains("name") && name.isEmpty())
                return aiErr(id, AiErrorCode::InvalidArgument, "name must not be empty.");
            PatternManager::ScopedExternalTransaction tx(*pmp);
            QString error;
            auto* track = pmp->addSampleChannel(file, name, &error);
            if (!track)
                return aiErr(id, AiErrorCode::Failed,
                             error.isEmpty() ? QStringLiteral("Could not add the sample channel.") : error);
            return aiOk(id, QJsonObject{{"track_id", ids::of(track)},
                                        {"name", trackName(track)},
                                        {"sample", pmp->channelSamplePath(track->itemID)},
                                        {"root_note", pmp->channelRootNote(track->itemID)}});
        };
        t.flags = mutatingFlags();
        tools.append(t);
    }

    // ── set_channel_sample ──────────────────────────────────────────────────
    {
        QJsonObject props;
        props["track"] = trackRefSchema();
        props["path"] = prop("string",
            "The new audio file: a 'path' from find_samples (absolute), or its relative_path.");
        AiToolExtension t;
        t.definition = makeTool("set_channel_sample",
            "Swaps the sample a channel plays (e.g. try another kick) without touching its steps, patterns or "
            "mixer settings. A channel whose instrument is not a sampler gets a sampler instead. A channel "
            "still named after its old sample is renamed after the new one. Returns {track_id, name, sample}.",
            props, {"track", "path"});
        t.handler = [pmp, exp](const QJsonObject& input, const QString& id) -> AiToolResult {
            if (auto e = checkEditable(*exp, id, true)) return *e;
            auto ch = lookupChannel(*exp, input, id);
            if (ch.error) return *ch.error;
            juce::File file;
            if (auto e = resolveSampleArg(input, id, file)) return *e;
            PatternManager::ScopedExternalTransaction tx(*pmp);
            QString error;
            if (!pmp->setChannelSample(ch.track->itemID, file, &error))
                return aiErr(id, AiErrorCode::Failed,
                             error.isEmpty() ? QStringLiteral("Could not set the sample.") : error);
            return aiOk(id, QJsonObject{{"track_id", ids::of(ch.track)},
                                        {"name", trackName(ch.track)},
                                        {"sample", pmp->channelSamplePath(ch.track->itemID)}});
        };
        t.flags = mutatingFlags();
        tools.append(t);
    }

    return tools;
}

} // namespace OpenDaw
