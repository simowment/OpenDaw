#include "AiToolExecutor.h"
#include <QDir>
#include <QFileInfo>
#include "AiToolDefs.h"
#include "AiIds.h"
#include "AiSchemaValidator.h"
#include "engine/instruments/BundledPlugins.h"
#include "engine/instruments/SoundFontInstrument.h"
#include <tracktion_engine/tracktion_engine.h>
#include <QCoreApplication>
#include <QDebug>
#include <QSet>
#include <QtConcurrent/QtConcurrent>
#include <cmath>
#include <algorithm>
#include <exception>

namespace OpenDaw {

static QString qstr(const juce::String& s)
{
    return QString::fromStdString(s.toStdString());
}

// Human-readable form of a reference value for error messages.
static QString describeRef(const QJsonValue& ref)
{
    if (ref.isString()) return QString("'%1'").arg(ref.toString());
    if (ref.isDouble()) return QString("index %1").arg(ref.toDouble());
    if (ref.isObject()) return QString::fromUtf8(QJsonDocument(ref.toObject()).toJson(QJsonDocument::Compact));
    if (ref.isArray()) return QString::fromUtf8(QJsonDocument(ref.toArray()).toJson(QJsonDocument::Compact));
    return QStringLiteral("(none)");
}

// The structured form of a plain result content string.
static QJsonObject structuredFromContent(const QString& content)
{
    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(content.toUtf8(), &pe);
    if (pe.error == QJsonParseError::NoError) {
        if (doc.isObject()) return doc.object();
        if (doc.isArray()) return QJsonObject{{"items", doc.array()}};
    }
    return QJsonObject{{"message", content}};
}

AiToolExecutor::AiToolExecutor(EditManager* editMgr, AudioEngine* audioEngine,
                               PluginScanner* pluginScanner, QObject* parent)
    : QObject(parent), editMgr_(editMgr), audioEngine_(audioEngine),
      pluginScanner_(pluginScanner), audioAnalysis_(editMgr)
{
    registerHandlers();
    registerBuiltInTools();
    registerExtensionTools();
}

// ── Tool table ──────────────────────────────────────────────────────────────

AiToolFlags AiToolExecutor::builtInFlags(const QString& name)
{
    // Read-only tools that do not follow the get_/list_/analyze_/measure_ naming.
    static const QSet<QString> readOnly = {
        "preview_mix_plan", "get_transport_state", "group_tracks_by_inferred_role"
    };
    static const QSet<QString> destructive = {
        "delete_track", "remove_effect_from_track",
        "clear_track_output", "clear_track_input",
        "set_track_output", "remove_send",
        "undo", "redo", "revert_last_mix_stage"
    };
    static const QSet<QString> notBatchable = {
        "play", "stop", "record", "set_position", "measure_mix_loudness", "save_project",
        "undo", "redo", "run_batch", "revert_last_mix_stage", "commit_last_mix_stage"
    };

    AiToolFlags f;
    f.mutating = !(name.startsWith("get_") || name.startsWith("list_") || name.startsWith("analyze_")
                   || name.startsWith("measure_") || readOnly.contains(name));
    f.destructive = destructive.contains(name);
    f.batchable = !notBatchable.contains(name);
    return f;
}

void AiToolExecutor::registerBuiltInTools()
{
    for (const auto& v : AiToolDefs::allTools()) {
        const QJsonObject def = v.toObject();
        const QString name = def["name"].toString();
        auto h = handlers_.constFind(name);
        if (h == handlers_.constEnd()) {
            qWarning() << "[AiToolExecutor] built-in tool has no handler:" << name;
            continue;
        }
        ToolEntry e;
        e.definition = def;
        e.inputSchema = def["input_schema"].toObject();
        e.handler = h.value();
        e.flags = builtInFlags(name);
        tools_.insert(name, e);
        toolOrder_.append(name);
    }
    for (auto it = handlers_.constBegin(); it != handlers_.constEnd(); ++it)
        if (!tools_.contains(it.key()))
            qWarning() << "[AiToolExecutor] handler has no tool definition:" << it.key();
}

void AiToolExecutor::registerExtensionTools()
{
    const auto providers = AiToolExtensionRegistry::instance().providers();
    for (const auto& provider : providers) {
        if (!provider) continue;
        QVector<AiToolExtension> extensions;
        try {
            extensions = provider(*this);
        } catch (const std::exception& e) {
            qWarning() << "[AiToolExecutor] tool provider failed:" << e.what();
            continue;
        } catch (...) {
            qWarning() << "[AiToolExecutor] tool provider failed";
            continue;
        }

        for (const auto& ext : extensions) {
            const QString name = ext.definition["name"].toString();
            if (name.isEmpty() || !ext.handler) {
                qWarning() << "[AiToolExecutor] skipping extension tool without name or handler:" << name;
                continue;
            }
            if (tools_.contains(name)) {
                qWarning() << "[AiToolExecutor] extension tool" << name
                           << "clashes with an existing tool; skipped";
                continue;
            }
            ToolEntry e;
            e.definition = ext.definition;
            e.inputSchema = ext.definition["input_schema"].toObject();
            if (e.inputSchema.isEmpty()) {
                e.inputSchema = QJsonObject{{"type", "object"}};
                e.definition["input_schema"] = e.inputSchema;
            }
            e.handler = ext.handler;
            e.flags = ext.flags;
            tools_.insert(name, e);
            toolOrder_.append(name);
        }
    }
}

QJsonArray AiToolExecutor::toolDefinitions() const
{
    QJsonArray arr;
    for (const auto& name : toolOrder_)
        arr.append(tools_.value(name).definition);
    return arr;
}

bool AiToolExecutor::hasTool(const QString& name) const
{
    return tools_.contains(name);
}

AiToolFlags AiToolExecutor::toolFlags(const QString& name) const
{
    auto it = tools_.constFind(name);
    return it != tools_.constEnd() ? it->flags : AiToolFlags{};
}

bool AiToolExecutor::isDestructive(const QString& toolName) const
{
    auto it = tools_.constFind(toolName);
    return it != tools_.constEnd() && it->flags.destructive;
}

bool AiToolExecutor::isDestructiveCall(const QString& toolName, const QJsonObject& input) const
{
    if (isDestructive(toolName)) return true;
    if (toolName == "run_batch") {
        for (const auto& cmd : input["commands"].toArray())
            if (isDestructive(cmd.toObject().value("tool").toString()))
                return true;
    }
    return false;
}

// ── Resolution helpers ──────────────────────────────────────────────────────

// ── Undoable parameter changes ──────────────────────────────────────────────
// Built-in plugin parameters are attached to a CachedValue that refers to the
// plugin state through the Edit's UndoManager (e.g. VolumeAndPanPlugin's
// volume.referTo(state, IDs::volume, um)). setParameter(v, sendNotification)
// writes that CachedValue (AutomatableParameter::setParameterValue ->
// AttachedFloatValue::setValue -> `value = v`), so the change is already
// recorded in the current transaction and undo restores it.
//
// Parameters without an attached value (external/VST plugin parameters) only
// write the plugin state back later and record nothing, so those go through
// SetParameterAction. The action keeps no pointer to the parameter or its
// plugin: the plugin can leave the Edit, be freed by the PluginCache and be
// re-created as a new object by a later undo. It stores the plugin's
// EditItemID and the parameter's ID and looks both up again each time, doing
// nothing when either is gone.
namespace {
// Reads AutomatableParameter's protected attachedValue (pointer-to-member
// formed through a derived class; never instantiated).
struct ParamAccess : te::AutomatableParameter {
    static bool hasAttachedValue(const te::AutomatableParameter& p)
    {
        return (p.*(&ParamAccess::attachedValue)) != nullptr;
    }
};

struct SetParameterAction : juce::UndoableAction {
    SetParameterAction(te::Edit& e, te::EditItemID plugin, juce::String param, float newValue, float oldValue)
        : edit(e), pluginID(plugin), paramID(std::move(param)), newVal(newValue), oldVal(oldValue) {}

    bool perform() override { apply(newVal); return true; }
    bool undo() override    { apply(oldVal); return true; }
    int getSizeInUnits() override { return 32; }

    void apply(float v) const
    {
        // The UndoManager belongs to the Edit, so the Edit outlives this action.
        if (auto plugin = edit.getPluginCache().getPluginFor(pluginID))
            if (auto param = plugin->getAutomatableParameterByID(paramID))
                param->setParameter(v, juce::sendNotification);
    }

    te::Edit& edit;
    te::EditItemID pluginID;
    juce::String paramID;
    float newVal, oldVal;
};

// Used to push rolled-back batch changes off the redo side (see runBatch).
struct NoOpAction : juce::UndoableAction {
    bool perform() override { return true; }
    bool undo() override    { return true; }
    int getSizeInUnits() override { return 1; }
};

// EditManager::beginUndoGroup()/endUndoGroup(): while held, EditManager
// methods do not begin transactions of their own, so a tool (or a whole
// run_batch) is one undo step.
struct ScopedUndoGroup {
    explicit ScopedUndoGroup(EditManager* m) : mgr(m) { if (mgr) mgr->beginUndoGroup(); }
    ~ScopedUndoGroup() { if (mgr) mgr->endUndoGroup(); }
    ScopedUndoGroup(const ScopedUndoGroup&) = delete;
    ScopedUndoGroup& operator=(const ScopedUndoGroup&) = delete;
    EditManager* mgr;
};
} // namespace

static void setParamUndoable(juce::UndoManager* um, te::AutomatableParameter* p, float value)
{
    if (!p) return;
    auto* plugin = p->getPlugin();
    // Every plugin-owned parameter goes through the action, built-in ones too:
    // setParameter alone leaves no undo history even for parameters attached
    // to a CachedValue (it writes the state back asynchronously; verified by
    // TestToolExecutor::undoRevertsVolumeChange). Parameters not owned by a
    // plugin (macros, modifiers) cannot be looked up again by id, so they are
    // set directly.
    if (!um || !plugin) {
        p->setParameter(value, juce::sendNotification);
        return;
    }
    um->perform(new SetParameterAction(plugin->edit, plugin->itemID, p->paramID,
                                       value, p->getCurrentBaseValue()));
}

static void setParamUndoable(juce::UndoManager* um, const te::AutomatableParameter::Ptr& p, float value)
{
    setParamUndoable(um, p.get(), value);
}

juce::UndoManager* AiToolExecutor::undoManager() const
{
    return (editMgr_ && editMgr_->edit()) ? &editMgr_->edit()->getUndoManager() : nullptr;
}

te::AudioTrack* AiToolExecutor::resolveTrack(const QJsonValue& trackRef) const
{
    if (!editMgr_ || !editMgr_->edit()) return nullptr;

    auto byId = [this](const QString& s) -> te::AudioTrack* {
        const auto id = ids::parse(s.trimmed(), ids::kTrack);
        if (!id) return nullptr;
        auto* t = editMgr_->findAudioTrack(*id);
        return (t && editMgr_->getAudioTracks().contains(t)) ? t : nullptr;
    };
    auto byName = [this](const QString& name) -> te::AudioTrack* {
        for (auto* t : editMgr_->getAudioTracks())
            if (qstr(t->getName()).compare(name, Qt::CaseInsensitive) == 0)
                return t;
        return nullptr;
    };

    if (trackRef.isDouble()) {
        int idx = trackRef.toInt(-1);   // -1 (no match) for fractional/out-of-range, not track 0
        auto* track = editMgr_->getAudioTrack(idx);
        if (!track)
            qWarning() << "[resolveTrack] no track at index" << idx;
        return track;
    }

    if (trackRef.isObject()) {
        auto obj = trackRef.toObject();
        if (obj.contains("id"))
            return byId(obj["id"].toString());
        int idx = obj["index"].toInt(-1);
        QString name = obj["name"].toString();
        auto* track = (idx >= 0) ? editMgr_->getAudioTrack(idx) : nullptr;
        if (track && !name.isEmpty()) {
            auto trackName = qstr(track->getName());
            if (trackName.compare(name, Qt::CaseInsensitive) != 0)
                qWarning() << "[resolveTrack] index/name mismatch: idx=" << idx
                           << "expected=" << name << "actual=" << trackName;
        }
        if (track) return track;
        if (!name.isEmpty()) return byName(name);
        return nullptr;
    }

    if (trackRef.isString()) {
        const QString s = trackRef.toString();
        // A well-formed id is never treated as a name.
        if (ids::isIdOfKind(s.trimmed(), ids::kTrack))
            return byId(s);
        return byName(s);
    }
    return nullptr;
}

QVector<te::Plugin*> AiToolExecutor::getUserPlugins(te::AudioTrack* track) const
{
    QVector<te::Plugin*> result;
    if (!track) return result;
    for (auto* p : track->pluginList.getPlugins()) {
        if (EditManager::isInternalPlugin(p)) continue;
        result.append(p);
    }
    return result;
}

te::Plugin* AiToolExecutor::resolveEffect(te::AudioTrack* track, const QJsonValue& effectRef) const
{
    if (effectRef.isObject())
        return resolveEffect(track, effectRef.toObject().value("id"));

    auto plugins = getUserPlugins(track);
    if (plugins.isEmpty()) return nullptr;

    if (effectRef.isDouble()) {
        int idx = effectRef.toInt(-1);
        if (idx >= 0 && idx < plugins.size())
            return plugins[idx];
        return nullptr;
    }

    const QString name = effectRef.toString();
    if (ids::isIdOfKind(name.trimmed(), ids::kPlugin)) {
        const auto id = ids::parse(name.trimmed(), ids::kPlugin);
        for (auto* p : plugins)
            if (id && p->itemID == *id)
                return p;
        return nullptr;
    }
    for (auto* p : plugins) {
        if (qstr(p->getName()).compare(name, Qt::CaseInsensitive) == 0)
            return p;
    }
    return nullptr;
}

te::AutomatableParameter* AiToolExecutor::resolveParameter(te::Plugin* plugin, const QJsonValue& paramRef) const
{
    if (!plugin) return nullptr;
    auto params = plugin->getAutomatableParameters();

    if (paramRef.isDouble()) {
        int idx = paramRef.toInt(-1);
        if (idx >= 0 && idx < params.size())
            return params[idx];
        return nullptr;
    }

    QString name = paramRef.toString();
    for (auto* p : params) {
        if (qstr(p->getParameterName()).compare(name, Qt::CaseInsensitive) == 0)
            return p;
    }
    for (auto* p : params) {
        if (qstr(p->getParameterName()).contains(name, Qt::CaseInsensitive))
            return p;
    }
    return nullptr;
}

te::Clip* AiToolExecutor::resolveClipImpl(te::AudioTrack* track, const QJsonObject& input,
                                          QString* error, QString* code) const
{
    auto fail = [error, code](const QString& c, const QString& message) -> te::Clip* {
        if (error) *error = message;
        if (code) *code = c;
        return nullptr;
    };

    if (!editMgr_ || !editMgr_->edit())
        return fail(AiErrorCode::Conflict, "No project loaded.");

    if (input.contains("clip_id")) {
        const QString s = input["clip_id"].toString().trimmed();
        const auto cid = ids::parse(s, ids::kClip);
        if (!cid)
            return fail(AiErrorCode::InvalidArgument,
                        QString("clip_id must look like 'clip:2210', got '%1'.").arg(s));
        if (track) {
            for (auto* c : track->getClips())
                if (c->itemID == *cid)
                    return c;
            return fail(AiErrorCode::NotFound,
                        QString("Clip %1 not found on track '%2'. Call get_clips_on_track for clip ids.")
                            .arg(s, qstr(track->getName())));
        }
        auto* c = te::findClipForID(*editMgr_->edit(), *cid);
        if (!c || !dynamic_cast<te::AudioTrack*>(c->getTrack()))
            return fail(AiErrorCode::NotFound,
                        QString("Clip %1 not found. Call get_clips_on_track for clip ids.").arg(s));
        return c;
    }

    if (input.contains("clip_index")) {
        if (!track)
            return fail(AiErrorCode::InvalidArgument,
                        "clip_index needs a track: give track as well, or use clip_id.");
        const int idx = input["clip_index"].toInt(-1);
        if (idx < 0)
            return fail(AiErrorCode::InvalidArgument, "Invalid clip_index.");
        const auto& clips = track->getClips();
        if (idx >= clips.size())
            return fail(AiErrorCode::NotFound,
                        QString("clip_index %1 out of range (track has %2 clips).").arg(idx).arg(clips.size()));
        return clips[idx];
    }

    return fail(AiErrorCode::InvalidArgument, "Give clip_id (preferred) or track + clip_index.");
}

te::Clip* AiToolExecutor::resolveClip(te::AudioTrack* track, const QJsonObject& input, QString* error) const
{
    return resolveClipImpl(track, input, error, nullptr);
}

std::optional<AiToolResult> AiToolExecutor::resolveMidiClipTarget(const QJsonObject& input, const QString& id,
                                                                  te::AudioTrack*& track, te::MidiClip*& clip,
                                                                  int& clipIndex) const
{
    track = nullptr;
    clip = nullptr;
    clipIndex = -1;

    if (input.contains("track")) {
        track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
    }

    QString message, code;
    te::Clip* any = resolveClipImpl(track, input, &message, &code);
    if (!any)
        return err(id, code.isEmpty() ? AiErrorCode::Failed : code, message);

    if (!track) track = dynamic_cast<te::AudioTrack*>(any->getTrack());
    if (!track) return err(id, AiErrorCode::NotFound, "The clip's track was not found.");

    clipIndex = track->getClips().indexOf(any);
    clip = dynamic_cast<te::MidiClip*>(any);
    if (!clip) {
        const QString which = input.contains("clip_id") ? input["clip_id"].toString()
                                                        : QString("at index %1").arg(clipIndex);
        return err(id, AiErrorCode::InvalidArgument, QString("Clip %1 is not a MIDI clip.").arg(which));
    }
    return std::nullopt;
}

// ── Result helpers ──────────────────────────────────────────────────────────

AiToolResult AiToolExecutor::ok(const QString& toolUseId, const QJsonObject& data) const
{
    return aiOk(toolUseId, data);
}

AiToolResult AiToolExecutor::ok(const QString& toolUseId, const QJsonArray& data) const
{
    AiToolResult r;
    r.toolUseId = toolUseId;
    r.content = QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact));
    r.isError = false;
    r.structured = QJsonObject{{"items", data}};
    return r;
}

AiToolResult AiToolExecutor::ok(const QString& toolUseId, const QString& message) const
{
    return aiOkMessage(toolUseId, message);
}

AiToolResult AiToolExecutor::err(const QString& toolUseId, const QString& code, const QString& message,
                                 const QJsonObject& details) const
{
    return aiErr(toolUseId, code, message, details);
}

AiToolResult AiToolExecutor::trackNotFound(const QString& toolUseId, const QJsonValue& ref,
                                           const QString& field) const
{
    QJsonObject details{{"field", field}};
    if (ref.isUndefined() || ref.isNull())
        return err(toolUseId, AiErrorCode::InvalidArgument, QString("Missing %1.").arg(field), details);
    details["ref"] = ref;
    return err(toolUseId, AiErrorCode::NotFound,
               QString("Track not found: %1. Call get_track_list for track ids and names.").arg(describeRef(ref)),
               details);
}

AiToolResult AiToolExecutor::effectNotFound(const QString& toolUseId, const QJsonValue& ref) const
{
    QJsonObject details{{"field", "effect"}};
    if (!ref.isUndefined()) details["ref"] = ref;
    return err(toolUseId, AiErrorCode::NotFound,
               QString("Effect not found: %1. Call get_track_effects for effect ids and names.").arg(describeRef(ref)),
               details);
}

// ── Track info helper ───────────────────────────────────────────────────────

static QJsonObject trackToJson(EditManager* mgr, te::AudioTrack* t, int index)
{
    QJsonObject obj;
    obj["id"] = ids::of(t);
    obj["index"] = index;
    obj["name"] = qstr(t->getName());

    if (mgr->isBusTrack(t))
        obj["type"] = "bus";
    else if (mgr->isMidiTrack(t))
        obj["type"] = "midi";
    else
        obj["type"] = "audio";

    obj["muted"] = t->isMuted(false);
    obj["solo"] = t->isSolo(false);
    obj["armed"] = mgr->isTrackRecordEnabled(t);
    obj["mono"] = mgr->isTrackMono(t);

    for (auto* p : t->pluginList.getPlugins()) {
        if (auto* vp = dynamic_cast<te::VolumeAndPanPlugin*>(p)) {
            float volPos = vp->volParam->getCurrentValue();
            float dB = te::volumeFaderPositionToDB(volPos);
            obj["volume_db"] = static_cast<double>(dB);
            obj["pan"] = static_cast<double>(vp->pan.get());
            break;
        }
    }

    obj["input"] = mgr->getTrackInputName(t);
    obj["output"] = mgr->getTrackOutputName(t);
    if (mgr->isMidiTrack(t)) {
        auto* instrument = mgr->getTrackInstrument(t);
        obj["instrument"] = instrument ? qstr(instrument->getName()) : QString("none");
    }

    return obj;
}

static QJsonObject effectToJson(te::Plugin* p, int index)
{
    QJsonObject fxObj;
    fxObj["id"] = ids::of(p);
    fxObj["index"] = index;
    fxObj["name"] = qstr(p->getName());
    fxObj["enabled"] = p->isEnabled();

    QJsonArray paramsArr;
    for (auto* param : p->getAutomatableParameters()) {
        QJsonObject pObj;
        pObj["name"] = qstr(param->getParameterName());
        pObj["value"] = static_cast<double>(param->getCurrentValue());
        paramsArr.append(pObj);
    }
    fxObj["parameters"] = paramsArr;
    return fxObj;
}

static QJsonObject trackToDetailedJson(EditManager* mgr, te::AudioTrack* t, int index)
{
    QJsonObject obj = trackToJson(mgr, t, index);

    QJsonArray effectsArr;
    int fxIndex = 0;
    for (auto* p : t->pluginList.getPlugins()) {
        if (EditManager::isInternalPlugin(p)) continue;
        effectsArr.append(effectToJson(p, fxIndex++));
    }
    obj["effects"] = effectsArr;

    obj["output"] = mgr->currentOutputKey(t);
    QJsonArray sendsArr;
    for (const auto& send : mgr->getTrackSends(t)) {
        QJsonObject s;
        s["bus"] = qstr(send.bus->getName());
        s["bus_id"] = ids::of(send.bus);
        s["level_db"] = std::round(send.gainDb * 10.0f) / 10.0f;
        s["pre_fader"] = send.preFader;
        s["muted"] = send.muted;
        sendsArr.append(s);
    }
    obj["sends"] = sendsArr;
    return obj;
}

// {"id","index","name"} for a newly created track.
static QJsonObject createdTrackJson(EditManager* mgr, te::AudioTrack* t)
{
    QJsonObject obj;
    obj["id"] = ids::of(t);
    obj["index"] = mgr->getAudioTracks().indexOf(t);
    obj["name"] = qstr(t->getName());
    return obj;
}

// ── Effect name → xmlTypeName ───────────────────────────────────────────────

static const char* resolveEffectXmlType(const QString& name)
{
    QString lower = name.toLower().trimmed();
    if (lower == "reverb")                         return te::ReverbPlugin::xmlTypeName;
    if (lower == "eq" || lower == "equaliser"
        || lower == "equalizer")                   return te::EqualiserPlugin::xmlTypeName;
    if (lower == "compressor")                     return te::CompressorPlugin::xmlTypeName;
    if (lower == "delay")                          return te::DelayPlugin::xmlTypeName;
    if (lower == "chorus")                         return te::ChorusPlugin::xmlTypeName;
    if (lower == "phaser")                         return te::PhaserPlugin::xmlTypeName;
    if (lower == "low pass filter" || lower == "lowpass"
        || lower == "low pass")                    return te::LowPassPlugin::xmlTypeName;
    if (lower == "pitch shift" || lower == "pitchshift") return te::PitchShiftPlugin::xmlTypeName;
    return nullptr;
}

static te::VolumeAndPanPlugin* findVolumePanPlugin(te::AudioTrack* track)
{
    if (!track) return nullptr;
    for (auto* p : track->pluginList.getPlugins()) {
        if (auto* vp = dynamic_cast<te::VolumeAndPanPlugin*>(p))
            return vp;
    }
    return nullptr;
}

static te::Plugin* ensureBuiltInEffect(EditManager* editMgr, te::AudioTrack* track, const QString& effectName)
{
    if (!editMgr || !editMgr->edit() || !track) return nullptr;
    const char* xmlType = resolveEffectXmlType(effectName);
    if (!xmlType) return nullptr;

    juce::String resolvedType(xmlType);
    for (auto* p : track->pluginList.getPlugins()) {
        if (p->getPluginType() == resolvedType)
            return p;
        if (dynamic_cast<te::ReverbPlugin*>(p) && resolvedType == te::ReverbPlugin::xmlTypeName) return p;
        if (dynamic_cast<te::EqualiserPlugin*>(p) && resolvedType == te::EqualiserPlugin::xmlTypeName) return p;
        if (dynamic_cast<te::CompressorPlugin*>(p) && resolvedType == te::CompressorPlugin::xmlTypeName) return p;
        if (dynamic_cast<te::DelayPlugin*>(p) && resolvedType == te::DelayPlugin::xmlTypeName) return p;
        if (dynamic_cast<te::ChorusPlugin*>(p) && resolvedType == te::ChorusPlugin::xmlTypeName) return p;
        if (dynamic_cast<te::PhaserPlugin*>(p) && resolvedType == te::PhaserPlugin::xmlTypeName) return p;
        if (dynamic_cast<te::LowPassPlugin*>(p) && resolvedType == te::LowPassPlugin::xmlTypeName) return p;
        if (dynamic_cast<te::PitchShiftPlugin*>(p) && resolvedType == te::PitchShiftPlugin::xmlTypeName) return p;
    }

    auto plugin = editMgr->edit()->getPluginCache().createNewPlugin(juce::String(xmlType), {});
    if (!plugin) return nullptr;
    auto* raw = plugin.get();
    track->pluginList.insertPlugin(plugin, EditManager::effectInsertIndex(track->pluginList), nullptr);
    return raw;
}

static QString inferTrackRole(const QString& trackName)
{
    return AiAudioAnalysis::inferRoleFromName(trackName);
}

// ── Handler registration ────────────────────────────────────────────────────

void AiToolExecutor::registerHandlers()
{
    using namespace AiErrorCode;

    handlers_["get_project_info"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit())
            return err(id, Conflict, "No project loaded.");

        QJsonObject info;
        auto tracks = editMgr_->getAudioTracks();
        info["track_count"] = tracks.size();
        info["tempo_bpm"] = editMgr_->getBpm();
        info["time_sig_numerator"] = editMgr_->getTimeSigNumerator();
        info["time_sig_denominator"] = editMgr_->getTimeSigDenominator();

        auto& transport = editMgr_->transport();
        info["playing"] = transport.isPlaying();
        info["recording"] = transport.isRecording();
        info["position_seconds"] = transport.getPosition().inSeconds();
        info["looping"] = transport.looping.get();

        QJsonArray trackList;
        for (int i = 0; i < tracks.size(); ++i)
            trackList.append(trackToJson(editMgr_, tracks[i], i));
        info["tracks"] = trackList;

        return ok(id, info);
    };

    handlers_["get_track_list"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        auto tracks = editMgr_->getAudioTracks();
        QJsonArray arr;
        for (int i = 0; i < tracks.size(); ++i)
            arr.append(trackToJson(editMgr_, tracks[i], i));
        return ok(id, arr);
    };

    handlers_["get_track_info"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        auto tracks = editMgr_->getAudioTracks();
        int idx = tracks.indexOf(track);
        return ok(id, trackToDetailedJson(editMgr_, track, idx));
    };

    handlers_["create_audio_track"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        auto* track = editMgr_->addAudioTrack();
        if (!track) return err(id, Failed, "Failed to create audio track.");
        if (input.contains("name"))
            track->setName(juce::String(input["name"].toString().toStdString()));
        notifyTracksChanged();
        return ok(id, createdTrackJson(editMgr_, track));
    };

    handlers_["create_midi_track"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        auto* track = editMgr_->addMidiTrack();
        if (!track) return err(id, Failed, "Failed to create MIDI track.");
        if (input.contains("name"))
            track->setName(juce::String(input["name"].toString().toStdString()));
        notifyTracksChanged();
        return ok(id, createdTrackJson(editMgr_, track));
    };

    handlers_["list_instruments"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        QJsonObject result;
        QJsonObject builtIn;
        for (const auto& preset : SoundFontLibrary::instance().presets()) {
            QJsonArray names = builtIn[preset.category].toArray();
            names.append(preset.name);
            builtIn[preset.category] = names;
        }
        result["built_in"] = builtIn;
        if (BundledPlugins::hasSurgeXt())
            result["synth"] = "Surge XT (bundled; set_track_instrument with \"Surge XT\". "
                              "Its presets are chosen by the user in its window.)";
        QJsonArray vsts;
        if (pluginScanner_)
            for (auto& desc : pluginScanner_->getPluginList().getTypes())
                if (desc.isInstrument)
                    vsts.append(qstr(desc.name));
        result["vst"] = vsts;
        return ok(id, result);
    };

    handlers_["set_track_instrument"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        const QString want = input["instrument"].toString().trimmed();
        const QString trackName = qstr(track->getName());

        if (auto* preset = SoundFontLibrary::instance().findByName(want)) {
            editMgr_->setTrackBuiltInInstrument(*track, preset->bank, preset->program);
            if (input.contains("follow_program_changes"))
                if (auto* sf = dynamic_cast<SoundFontPlugin*>(editMgr_->getTrackInstrument(track)))
                    sf->setFollowsProgramChanges(input["follow_program_changes"].toBool());
            notifyTracksChanged();
            return ok(id, QString("'%1' now plays the built-in %2 (%3).")
                          .arg(trackName, preset->name, preset->category));
        }
        if (pluginScanner_ && want.contains("surge", Qt::CaseInsensitive)) {
            if (auto desc = BundledPlugins::surgeXtDescription(pluginScanner_->getPluginList())) {
                pluginScanner_->saveCachedList();
                editMgr_->setTrackInstrument(*track, *desc);
                notifyTracksChanged();
                return ok(id, QString("'%1' now plays the Surge XT synth (its Init patch; "
                                      "presets are chosen in its window).").arg(trackName));
            }
        }
        if (pluginScanner_) {
            for (auto& desc : pluginScanner_->getPluginList().getTypes()) {
                if (!desc.isInstrument) continue;
                const QString name = qstr(desc.name);
                if (name.contains(want, Qt::CaseInsensitive)) {
                    editMgr_->setTrackInstrument(*track, desc);
                    notifyTracksChanged();
                    return ok(id, QString("'%1' now plays the VST instrument %2.").arg(trackName, name));
                }
            }
        }
        return err(id, NotFound,
                   QString("No instrument matches '%1'. Call list_instruments for the names.").arg(want),
                   {{"field", "instrument"}, {"ref", want}});
    };

    handlers_["create_bus_track"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        auto* track = editMgr_->addBusTrack();
        if (!track) return err(id, Failed, "Failed to create bus track.");
        if (input.contains("name"))
            track->setName(juce::String(input["name"].toString().toStdString()));
        notifyTracksChanged();
        return ok(id, createdTrackJson(editMgr_, track));
    };

    handlers_["delete_track"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        QString name = qstr(track->getName());
        QString trackId = ids::of(track);
        editMgr_->removeTrack(track);
        notifyTracksChanged();
        return ok(id, QString("Deleted track '%1' (%2).").arg(name, trackId));
    };

    handlers_["rename_track"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        QString oldName = qstr(track->getName());
        QString newName = input["new_name"].toString();
        track->setName(juce::String(newName.toStdString()));
        notifyEditChanged();
        return ok(id, QString("Renamed '%1' to '%2'.").arg(oldName, newName));
    };

    handlers_["set_track_mute"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        bool muted = input["muted"].toBool();
        track->setMute(muted);
        notifyEditChanged();
        return ok(id, QString("Track '%1' %2.")
                      .arg(qstr(track->getName()), muted ? "muted" : "unmuted"));
    };

    handlers_["set_track_solo"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        bool solo = input["solo"].toBool();
        track->setSolo(solo);
        notifyEditChanged();
        return ok(id, QString("Track '%1' solo %2.")
                      .arg(qstr(track->getName()), solo ? "on" : "off"));
    };

    handlers_["set_track_volume"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        double db = input["volume_db"].toDouble();
        if (auto* vp = findVolumePanPlugin(track)) {
            float pos = te::decibelsToVolumeFaderPosition(static_cast<float>(db));
            setParamUndoable(undoManager(), vp->volParam, pos);
            notifyEditChanged();
            return ok(id, QString("Track '%1' volume set to %2 dB.")
                          .arg(qstr(track->getName()))
                          .arg(db, 0, 'f', 1));
        }
        return err(id, Failed, "Volume plugin not found on track.");
    };

    handlers_["set_track_pan"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        double pan = input["pan"].toDouble();
        if (auto* vp = findVolumePanPlugin(track)) {
            vp->pan.setValue(static_cast<float>(pan), undoManager());
            notifyEditChanged();
            return ok(id, QString("Track '%1' pan set to %2.")
                          .arg(qstr(track->getName()))
                          .arg(pan, 0, 'f', 2));
        }
        return err(id, Failed, "Volume/pan plugin not found on track.");
    };

    handlers_["set_track_mono"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        bool mono = input["mono"].toBool();
        editMgr_->setTrackMono(*track, mono);
        notifyEditChanged();
        return ok(id, QString("Track '%1' set to %2.")
                      .arg(qstr(track->getName()), mono ? "mono" : "stereo"));
    };

    handlers_["set_track_record_enabled"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        bool enabled = input["enabled"].toBool();
        editMgr_->setTrackRecordEnabled(*track, enabled);
        notifyEditChanged();
        return ok(id, QString("Track '%1' record %2.")
                      .arg(qstr(track->getName()), enabled ? "armed" : "disarmed"));
    };

    // ── Routing ─────────────────────────────────────────────────────────────

    handlers_["get_available_inputs"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        if (!editMgr_) return err(id, Conflict, "No project loaded.");
        auto sources = editMgr_->getAvailableInputSources();
        QJsonArray arr;
        for (auto& src : sources) {
            QJsonObject obj;
            obj["device_name"] = qstr(src.deviceName);
            obj["display_name"] = src.displayName;
            arr.append(obj);
        }
        return ok(id, arr);
    };

    handlers_["assign_input_to_track"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        QString inputName = input["input_name"].toString();
        // Accept the device name or its display name; an unknown name would
        // only clear the track's input.
        juce::String deviceName;
        bool known = false;
        for (auto& src : editMgr_->getAvailableInputSources()) {
            if (qstr(src.deviceName) == inputName
                || src.displayName.compare(inputName, Qt::CaseInsensitive) == 0) {
                deviceName = src.deviceName;
                known = true;
                break;
            }
        }
        if (!known)
            return err(id, NotFound,
                       QString("Input '%1' not found. Call get_available_inputs for the names.").arg(inputName),
                       {{"field", "input_name"}, {"ref", inputName}});
        editMgr_->assignInputToTrack(*track, deviceName);
        notifyRoutingChanged();
        return ok(id, QString("Assigned input '%1' to track '%2'.")
                      .arg(inputName, qstr(track->getName())));
    };

    handlers_["clear_track_input"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        editMgr_->clearTrackInput(*track);
        notifyRoutingChanged();
        return ok(id, QString("Cleared input on track '%1'.").arg(qstr(track->getName())));
    };

    handlers_["set_track_output"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        QJsonValue dest = input["destination"];
        QString destName;
        if (dest.isString() && dest.toString().trimmed().compare("master", Qt::CaseInsensitive) == 0) {
            editMgr_->setTrackOutputToMaster(*track);
            destName = "master";
        } else {
            auto* destTrack = resolveTrack(dest);
            if (!destTrack) return trackNotFound(id, dest, "destination");
            if (destTrack == track)
                return err(id, InvalidArgument, "A track cannot output to itself.");
            editMgr_->setTrackOutputToTrack(*track, *destTrack);
            destName = qstr(destTrack->getName());
        }
        notifyRoutingChanged();
        return ok(id, QString("Set output of '%1' to '%2'.").arg(qstr(track->getName()), destName));
    };

    handlers_["clear_track_output"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        editMgr_->clearTrackOutput(*track);
        notifyRoutingChanged();
        return ok(id, QString("Disconnected output on track '%1'.").arg(qstr(track->getName())));
    };

    // Resolves input[field] to a bus track, or returns the error result.
    auto resolveBus = [this](const QJsonObject& input, const QString& field, const QString& id,
                             te::AudioTrack*& bus) -> std::optional<AiToolResult> {
        bus = resolveTrack(input[field]);
        if (!bus) {
            auto r = trackNotFound(id, input[field], field);
            if (r.errorCode == NotFound)
                r = err(id, NotFound,
                        QString("Bus not found: %1. Sends go to bus tracks; create one first.")
                            .arg(describeRef(input[field])),
                        {{"field", field}, {"ref", input[field]}});
            return r;
        }
        if (!editMgr_->isBusTrack(bus))
            return err(id, InvalidArgument,
                       QString("Track '%1' is not a bus track.").arg(qstr(bus->getName())),
                       {{"field", field}, {"ref", input[field]}});
        return std::nullopt;
    };

    handlers_["add_send"] = [this, resolveBus](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        te::AudioTrack* bus = nullptr;
        if (auto e = resolveBus(input, "bus", id, bus)) return *e;
        if (!editMgr_->canSendTo(track, bus))
            return err(id, Conflict, "That send would feed the bus back into itself.");
        const float db = static_cast<float>(input["level_db"].toDouble(-6.0));
        if (!editMgr_->addSend(*track, *bus, db, input["pre_fader"].toBool(false)))
            return err(id, Failed, "Could not add the send (all 32 aux buses may be in use).");
        notifyRoutingChanged();
        return ok(id, QString("Added a send from '%1' to '%2' at %3 dB.")
                      .arg(qstr(track->getName()), qstr(bus->getName()))
                      .arg(db, 0, 'f', 1));
    };

    auto hasSend = [this](te::AudioTrack* track, te::AudioTrack* bus) {
        for (const auto& s : editMgr_->getTrackSends(track))
            if (s.bus == bus) return true;
        return false;
    };

    handlers_["set_send"] = [this, resolveBus, hasSend](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        te::AudioTrack* bus = nullptr;
        if (auto e = resolveBus(input, "bus", id, bus)) return *e;
        if (!hasSend(track, bus))
            return err(id, NotFound, "That track has no send to that bus. Use add_send.");
        // No transaction of its own: execute()/run_batch already opened one.
        if (input.contains("level_db"))
            editMgr_->setSendGainDb(*track, *bus, static_cast<float>(input["level_db"].toDouble()));
        if (input.contains("pre_fader"))
            editMgr_->setSendPreFader(*track, *bus, input["pre_fader"].toBool());
        if (input.contains("muted"))
            editMgr_->setSendMuted(*track, *bus, input["muted"].toBool());
        notifyRoutingChanged();
        return ok(id, "Send updated.");
    };

    handlers_["remove_send"] = [this, resolveBus, hasSend](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        te::AudioTrack* bus = nullptr;
        if (auto e = resolveBus(input, "bus", id, bus)) return *e;
        if (!hasSend(track, bus))
            return err(id, NotFound, "That track has no send to that bus.");
        editMgr_->removeSend(*track, *bus);
        notifyRoutingChanged();
        return ok(id, "Send removed.");
    };

    // ── Effects ─────────────────────────────────────────────────────────────

    handlers_["list_available_effects"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        QJsonArray arr;
        QStringList builtIn = {"Reverb", "EQ", "Compressor", "Delay", "Chorus",
                               "Phaser", "Low Pass Filter", "Pitch Shift"};
        for (auto& name : builtIn) {
            QJsonObject obj;
            obj["name"] = name;
            obj["type"] = "built-in";
            arr.append(obj);
        }
        if (pluginScanner_) {
            auto& list = pluginScanner_->getPluginList();
            for (auto& desc : list.getTypes()) {
                if (desc.isInstrument) continue;
                QJsonObject obj;
                obj["name"] = qstr(desc.name);
                obj["manufacturer"] = qstr(desc.manufacturerName);
                obj["type"] = "vst";
                arr.append(obj);
            }
        }
        return ok(id, arr);
    };

    handlers_["get_track_effects"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);

        auto plugins = getUserPlugins(track);
        QJsonArray arr;
        for (int i = 0; i < plugins.size(); ++i)
            arr.append(effectToJson(plugins[i], i));
        return ok(id, arr);
    };

    handlers_["add_effect_to_track"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        if (!editMgr_->edit()) return err(id, Conflict, "No project loaded.");

        QString effectName = input["effect_name"].toString();
        const char* xmlType = resolveEffectXmlType(effectName);
        if (!xmlType)
            return err(id, NotFound,
                       QString("Unknown effect '%1'. Use list_available_effects to see options.").arg(effectName),
                       {{"field", "effect_name"}, {"ref", effectName}});

        auto& cache = editMgr_->edit()->getPluginCache();
        auto plugin = cache.createNewPlugin(juce::String(xmlType), {});
        if (!plugin) return err(id, Failed, "Failed to create effect plugin.");

        track->pluginList.insertPlugin(plugin, EditManager::effectInsertIndex(track->pluginList), nullptr);
        notifyEditChanged();

        QJsonObject out;
        out["id"] = ids::of(plugin.get());
        out["name"] = qstr(plugin->getName());
        out["index"] = getUserPlugins(track).indexOf(plugin.get());
        out["track_id"] = ids::of(track);
        out["message"] = QString("Added '%1' to track '%2'.").arg(effectName, qstr(track->getName()));
        return ok(id, out);
    };

    handlers_["remove_effect_from_track"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);

        auto* plugin = resolveEffect(track, input["effect"]);
        if (!plugin) return effectNotFound(id, input["effect"]);

        QString name = qstr(plugin->getName());
        plugin->deleteFromParent();
        notifyEditChanged();
        return ok(id, QString("Removed '%1' from track '%2'.").arg(name, qstr(track->getName())));
    };

    handlers_["set_effect_parameter"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        auto* plugin = resolveEffect(track, input["effect"]);
        if (!plugin) return effectNotFound(id, input["effect"]);
        auto* param = resolveParameter(plugin, input["parameter"]);
        if (!param)
            return err(id, NotFound,
                       QString("Parameter not found: %1. Call get_track_effects for parameter names.")
                           .arg(describeRef(input["parameter"])),
                       {{"field", "parameter"}, {"ref", input["parameter"]}});

        double value = input["value"].toDouble();
        setParamUndoable(undoManager(), param, static_cast<float>(value));
        notifyEditChanged();
        return ok(id, QString("Set '%1' on '%2' to %3.")
                      .arg(qstr(param->getParameterName()), qstr(plugin->getName()))
                      .arg(value, 0, 'f', 3));
    };

    handlers_["set_effect_bypass"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        auto* plugin = resolveEffect(track, input["effect"]);
        if (!plugin) return effectNotFound(id, input["effect"]);

        bool bypassed = input["bypassed"].toBool();
        plugin->setEnabled(!bypassed);
        notifyEditChanged();
        return ok(id, QString("'%1' %2.")
                      .arg(qstr(plugin->getName()), bypassed ? "bypassed" : "enabled"));
    };

    // ── Analysis / Mix Intelligence ────────────────────────────────────────

    handlers_["analyze_track_levels"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        const double windowSeconds = input.contains("window_seconds")
            ? input["window_seconds"].toDouble()
            : 2.0;
        return ok(id, audioAnalysis_.analyzeTrackLevels(track, windowSeconds));
    };

    handlers_["analyze_master_levels"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        const double windowSeconds = input.contains("window_seconds")
            ? input["window_seconds"].toDouble()
            : 4.0;
        return ok(id, audioAnalysis_.analyzeMasterLevels(windowSeconds));
    };

    handlers_["measure_mix_loudness"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        if (!editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        // Claim the render here, on the GUI thread, before the worker starts:
        // the events pumped below may dispatch other agent calls, and they
        // must already see the render as running (checkBusy).
        if (!editMgr_->tryClaimRender()) return err(id, Busy, "Another render is running.");

        auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                        .getNonexistentChildFile("OpenDaw-loudness", ".wav");
        ExportSettings s;
        s.destFile = temp;
        s.sampleRate = 48000.0;
        s.bitDepth = 24;
        s.format = ExportFormat::WAV;
        s.dither = false;
        s.renderClaimed = true;

        // Render on a worker thread while the UI keeps running.
        // The render session (frees the playback context so the render owns
        // the plugins) is opened and closed here, on the message thread.
        bool rendered = false;
        editMgr_->beginRenderSession();
        auto future = QtConcurrent::run([this, s, &rendered]() {
            rendered = editMgr_->exportMix(s, nullptr);
        });
        while (!future.isFinished())
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        editMgr_->endRenderSession();
        editMgr_->releaseRender();
        temp.deleteFile();
        if (!rendered) return err(id, Failed, "The mix couldn't be rendered (is every track muted?).");

        const auto r = editMgr_->lastExportLoudness();
        QJsonObject out;
        out["integrated_lufs"] = r.integrated;
        out["true_peak_dbtp"] = r.truePeakDb;
        out["loudness_range_lu"] = r.loudnessRange;
        out["max_short_term_lufs"] = r.maxShortTerm;
        out["duration_seconds"] = r.seconds;
        out["includes_master_effects"] = true;
        out["standard"] = "ITU-R BS.1770-4 / EBU R128, true peak 4x oversampled";
        return ok(id, out);
    };

    handlers_["analyze_frequency_balance"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        const auto target = input["target"];
        if (target.isString() && target.toString().trimmed().compare("master", Qt::CaseInsensitive) == 0)
            return ok(id, audioAnalysis_.analyzeMasterFrequencyBalance());
        auto* track = resolveTrack(target);
        if (!track) return trackNotFound(id, target, "target");
        return ok(id, audioAnalysis_.analyzeFrequencyBalance(track));
    };

    handlers_["analyze_stereo_image"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        const auto target = input["target"];
        if (target.isString() && target.toString().trimmed().compare("master", Qt::CaseInsensitive) == 0)
            return ok(id, audioAnalysis_.analyzeMasterStereoImage());
        auto* track = resolveTrack(target);
        if (!track) return trackNotFound(id, target, "target");
        return ok(id, audioAnalysis_.analyzeStereoImage(track));
    };

    handlers_["analyze_transients"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        return ok(id, audioAnalysis_.analyzeTransients(track));
    };

    handlers_["analyze_masking"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* a = resolveTrack(input["track_a"]);
        if (!a) return trackNotFound(id, input["track_a"], "track_a");
        auto* b = resolveTrack(input["track_b"]);
        if (!b) return trackNotFound(id, input["track_b"], "track_b");
        return ok(id, audioAnalysis_.analyzeMasking(a, b));
    };

    // ── Semantic Mix / Master Actions ───────────────────────────────────────

    handlers_["apply_mix_preset"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        const QString preset = input["preset_name"].toString().toLower().trimmed();

        if (preset == "vocal_clarity") {
            ensureBuiltInEffect(editMgr_, track, "EQ");
            auto* comp = ensureBuiltInEffect(editMgr_, track, "Compressor");
            if (comp) {
                auto params = comp->getAutomatableParameters();
                if (params.size() > 0) setParamUndoable(undoManager(), params[0], 0.40f);
                if (params.size() > 1) setParamUndoable(undoManager(), params[1], 0.55f);
            }
            if (auto* vp = findVolumePanPlugin(track))
                setParamUndoable(undoManager(), vp->volParam, te::decibelsToVolumeFaderPosition(-8.0f));
        } else if (preset == "drum_punch") {
            auto* comp = ensureBuiltInEffect(editMgr_, track, "Compressor");
            if (comp) {
                auto params = comp->getAutomatableParameters();
                if (params.size() > 0) setParamUndoable(undoManager(), params[0], 0.58f);
                if (params.size() > 1) setParamUndoable(undoManager(), params[1], 0.62f);
            }
        } else if (preset == "bass_control") {
            auto* comp = ensureBuiltInEffect(editMgr_, track, "Compressor");
            if (comp) {
                auto params = comp->getAutomatableParameters();
                if (params.size() > 0) setParamUndoable(undoManager(), params[0], 0.52f);
            }
            if (auto* vp = findVolumePanPlugin(track))
                vp->pan.setValue(0.0f, undoManager());
        } else {
            return err(id, InvalidArgument, QString("Unknown preset '%1'.").arg(preset));
        }

        notifyEditChanged();
        return ok(id, QString("Applied mix preset '%1' to '%2'.").arg(preset, qstr(track->getName())));
    };

    handlers_["set_track_target_peak"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        const double targetDb = input["dbfs"].toDouble(-10.0);
        auto analysis = audioAnalysis_.analyzeTrackLevels(track, 1.5);
        const double currentPeak = analysis["peak_dbfs"].toDouble(-120.0);
        if (currentPeak <= -100.0)
            return err(id, Conflict, "Track appears silent right now; play audio first for peak targeting.");

        const double delta = targetDb - currentPeak;
        auto* vp = findVolumePanPlugin(track);
        if (!vp) return err(id, Failed, "Volume plugin not found.");

        const double currentVolDb = te::volumeFaderPositionToDB(vp->volParam->getCurrentValue());
        const double nextDb = std::clamp(currentVolDb + delta, -60.0, 6.0);
        setParamUndoable(undoManager(), vp->volParam, te::decibelsToVolumeFaderPosition(static_cast<float>(nextDb)));
        notifyEditChanged();
        return ok(id, QString("Adjusted '%1' toward target peak %2 dBFS (current %3 dBFS, new volume %4 dB).")
                      .arg(qstr(track->getName()))
                      .arg(targetDb, 0, 'f', 1).arg(currentPeak, 0, 'f', 1).arg(nextDb, 0, 'f', 1));
    };

    handlers_["set_track_dynamic_goal"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        const QString goal = input["goal"].toString().toLower().trimmed();
        if (goal != "tighter" && goal != "more_open" && goal != "more_sustain")
            return err(id, InvalidArgument, QString("Unknown dynamic goal '%1'.").arg(goal));
        auto* comp = ensureBuiltInEffect(editMgr_, track, "Compressor");
        if (!comp) return err(id, Failed, "Could not create/find compressor.");
        auto params = comp->getAutomatableParameters();

        if (goal == "tighter") {
            if (params.size() > 0) setParamUndoable(undoManager(), params[0], 0.60f);
            if (params.size() > 1) setParamUndoable(undoManager(), params[1], 0.65f);
        } else if (goal == "more_open") {
            if (params.size() > 0) setParamUndoable(undoManager(), params[0], 0.35f);
            if (params.size() > 1) setParamUndoable(undoManager(), params[1], 0.40f);
        } else {
            if (params.size() > 0) setParamUndoable(undoManager(), params[0], 0.50f);
            if (params.size() > 2) setParamUndoable(undoManager(), params[2], 0.70f);
        }

        notifyEditChanged();
        return ok(id, QString("Applied dynamic goal '%1' to '%2'.").arg(goal, qstr(track->getName())));
    };

    handlers_["set_reverb_character"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        const QString style = input["style"].toString().toLower().trimmed();
        if (style != "ethereal" && style != "roomy" && style != "tight")
            return err(id, InvalidArgument, QString("Unknown reverb style '%1'.").arg(style));
        const double amount = std::clamp(input.contains("amount") ? input["amount"].toDouble() : 0.5, 0.0, 1.0);
        auto* reverb = ensureBuiltInEffect(editMgr_, track, "Reverb");
        if (!reverb) return err(id, Failed, "Could not create/find reverb.");
        auto params = reverb->getAutomatableParameters();

        if (style == "ethereal") {
            if (params.size() > 0) setParamUndoable(undoManager(), params[0], static_cast<float>(0.75 + amount * 0.2));
            if (params.size() > 2) setParamUndoable(undoManager(), params[2], static_cast<float>(0.55 + amount * 0.35));
        } else if (style == "roomy") {
            if (params.size() > 0) setParamUndoable(undoManager(), params[0], static_cast<float>(0.55 + amount * 0.25));
            if (params.size() > 2) setParamUndoable(undoManager(), params[2], static_cast<float>(0.40 + amount * 0.25));
        } else {
            if (params.size() > 0) setParamUndoable(undoManager(), params[0], static_cast<float>(0.20 + amount * 0.20));
            if (params.size() > 2) setParamUndoable(undoManager(), params[2], static_cast<float>(0.15 + amount * 0.20));
        }

        notifyEditChanged();
        return ok(id, QString("Set reverb on '%1' to style '%2' (amount %3).")
                      .arg(qstr(track->getName()), style)
                      .arg(amount, 0, 'f', 2));
    };

    handlers_["set_bus_glue"] = [this, resolveBus](const QJsonObject& input, const QString& id) -> AiToolResult {
        te::AudioTrack* bus = nullptr;
        if (auto e = resolveBus(input, "bus", id, bus)) return *e;
        const double intensity = std::clamp(input.contains("intensity") ? input["intensity"].toDouble() : 0.5, 0.0, 1.0);
        auto* comp = ensureBuiltInEffect(editMgr_, bus, "Compressor");
        if (!comp) return err(id, Failed, "Could not create/find compressor.");
        auto params = comp->getAutomatableParameters();
        if (params.size() > 0) setParamUndoable(undoManager(), params[0], static_cast<float>(0.40 + intensity * 0.30));
        if (params.size() > 1) setParamUndoable(undoManager(), params[1], static_cast<float>(0.35 + intensity * 0.25));
        notifyEditChanged();
        return ok(id, QString("Applied bus glue on '%1' with intensity %2.")
                      .arg(qstr(bus->getName()))
                      .arg(intensity, 0, 'f', 2));
    };

    handlers_["set_master_target"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        const QString profile = input["profile"].toString().toLower().trimmed();
        if (profile == "streaming_balanced") {
            editMgr_->edit()->setMasterVolumeSliderPos(0.72f);
        } else if (profile == "loud_pop") {
            editMgr_->edit()->setMasterVolumeSliderPos(0.82f);
        } else if (profile == "dynamic_film") {
            editMgr_->edit()->setMasterVolumeSliderPos(0.65f);
        } else {
            return err(id, InvalidArgument, QString("Unknown master profile '%1'.").arg(profile));
        }
        notifyEditChanged();
        return ok(id, QString("Set master target profile to '%1'.").arg(profile));
    };

    // ── Session Structure Helpers ────────────────────────────────────────────

    handlers_["group_tracks_by_inferred_role"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        QJsonObject out;
        QJsonArray drums, bass, vocals, music, fx;
        auto tracks = editMgr_->getAudioTracks();
        for (auto* t : tracks) {
            const QString name = qstr(t->getName());
            const QString role = inferTrackRole(name);
            if (role == "drums") drums.append(name);
            else if (role == "bass") bass.append(name);
            else if (role == "vocals") vocals.append(name);
            else if (role == "fx") fx.append(name);
            else music.append(name);
        }
        out["drums"] = drums;
        out["bass"] = bass;
        out["vocals"] = vocals;
        out["music"] = music;
        out["fx"] = fx;
        return ok(id, out);
    };

    handlers_["create_mix_buses_from_roles"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        QStringList targetBuses = {"Drums Bus", "Music Bus", "Vocals Bus", "FX Bus"};
        QVector<te::AudioTrack*> createdTracks;
        auto buses = editMgr_->getBusTracks();

        auto busExists = [&buses](const QString& busName) {
            for (auto* b : buses) {
                if (qstr(b->getName()).compare(busName, Qt::CaseInsensitive) == 0)
                    return true;
            }
            return false;
        };

        for (const auto& name : targetBuses) {
            if (!busExists(name)) {
                auto* bus = editMgr_->addBusTrack();
                if (bus) {
                    bus->setName(juce::String(name.toStdString()));
                    createdTracks.append(bus);
                }
            }
        }
        QJsonArray created;
        for (auto* bus : createdTracks)
            created.append(createdTrackJson(editMgr_, bus));
        notifyTracksChanged();
        return ok(id, created);
    };

    handlers_["auto_route_by_name_patterns"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        auto tracks = editMgr_->getNonBusAudioTracks();
        auto buses = editMgr_->getBusTracks();
        QMap<QString, te::AudioTrack*> busByRole;
        for (auto* b : buses) {
            const QString n = qstr(b->getName()).toLower();
            if (n.contains("drum")) busByRole["drums"] = b;
            else if (n.contains("vocal")) busByRole["vocals"] = b;
            else if (n.contains("fx")) busByRole["fx"] = b;
            else if (n.contains("music")) busByRole["music"] = b;
        }

        QJsonArray routed;
        for (auto* t : tracks) {
            const QString role = inferTrackRole(qstr(t->getName()));
            if (busByRole.contains(role)) {
                auto* bus = busByRole[role];
                editMgr_->setTrackOutputToTrack(*t, *bus);
                QJsonObject row;
                row["track"] = qstr(t->getName());
                row["track_id"] = ids::of(t);
                row["bus"] = qstr(bus->getName());
                row["bus_id"] = ids::of(bus);
                routed.append(row);
            }
        }
        notifyRoutingChanged();
        return ok(id, routed);
    };

    // ── Mix Plan / Checkpoint Helpers ────────────────────────────────────────

    handlers_["preview_mix_plan"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        const QString req = input["request"].toString().trimmed();
        QJsonObject out;
        out["request"] = req;
        out["stages"] = QJsonArray{
            "1) Gain staging and peak targets",
            "2) Balance and pan pass",
            "3) Corrective EQ and masking checks",
            "4) Compression/dynamics shaping",
            "5) Space effects (reverb/delay)",
            "6) Bus glue + master target",
            "7) Verify against analysis metrics"
        };
        out["note"] = "Preview only - no project changes applied.";
        return ok(id, out);
    };

    handlers_["commit_last_mix_stage"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        return ok(id, "Last mix stage marked as committed.");
    };

    handlers_["revert_last_mix_stage"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        auto* um = undoManager();
        if (!um) return err(id, Conflict, "No project loaded.");
        if (!um->canUndo()) return err(id, Conflict, "Nothing to revert.");
        editMgr_->undo();
        return ok(id, "Reverted last mix stage via undo.");
    };

    // ── Transport ───────────────────────────────────────────────────────────

    handlers_["play"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        editMgr_->transport().play(false);
        return ok(id, "Playback started.");
    };

    handlers_["stop"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        editMgr_->transport().stop(false, false);
        return ok(id, "Playback stopped.");
    };

    handlers_["record"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        bool anyArmed = false;
        for (auto* track : editMgr_->getAudioTracks()) {
            if (editMgr_->isTrackRecordEnabled(track)) {
                anyArmed = true;
                break;
            }
        }
        if (!anyArmed)
            return err(id, Conflict, "No tracks are armed for recording. Arm a track first.");
        editMgr_->startRecording();
        return ok(id, editMgr_->isPunchEnabled()
                          ? "Recording started in punch mode (records only inside the punch range)."
                          : "Recording started.");
    };

    handlers_["set_position"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        double secs = input["seconds"].toDouble();
        editMgr_->transport().setPosition(tracktion::TimePosition::fromSeconds(secs));
        return ok(id, QString("Position set to %1 seconds.").arg(secs, 0, 'f', 2));
    };

    handlers_["get_transport_state"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        auto& transport = editMgr_->transport();
        QJsonObject state;
        state["playing"] = transport.isPlaying();
        state["recording"] = transport.isRecording();
        state["position_seconds"] = transport.getPosition().inSeconds();
        state["looping"] = transport.looping.get();
        return ok(id, state);
    };

    handlers_["set_tempo"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        double bpm = input["bpm"].toDouble();
        if (bpm < 20.0 || bpm > 999.0)
            return err(id, InvalidArgument, "BPM must be between 20 and 999.");
        editMgr_->setBpm(bpm);
        notifyEditChanged();
        return ok(id, QString("Tempo set to %1 BPM.").arg(bpm, 0, 'f', 1));
    };

    handlers_["set_time_signature"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        int num = input["numerator"].toInt();
        int den = input["denominator"].toInt();
        if (num < 1 || num > 32)
            return err(id, InvalidArgument, "Numerator must be between 1 and 32.");
        if (den < 1 || den > 32 || (den & (den - 1)) != 0)
            return err(id, InvalidArgument, "Denominator must be a power of 2 (1, 2, 4, 8, 16, 32).");
        editMgr_->setTimeSignature(num, den);
        notifyEditChanged();
        return ok(id, QString("Time signature set to %1/%2.").arg(num).arg(den));
    };

    // ── Project ─────────────────────────────────────────────────────────────

    handlers_["save_project"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        if (!editMgr_ || !editMgr_->edit()) return err(id, Conflict, "No project loaded.");
        const QString path = input.value("path").toString().trimmed();
        if (!path.isEmpty()) {
            const QFileInfo fi(path);
            if (!fi.isAbsolute() || fi.suffix().compare("tracktionedit", Qt::CaseInsensitive) != 0)
                return err(id, InvalidArgument, "path must be an absolute file path ending in .tracktionedit.");
            if (!QDir().mkpath(fi.absolutePath()))
                return err(id, Failed, QString("Could not create the folder '%1'.").arg(fi.absolutePath()));
            const juce::File file(juce::String(QDir::toNativeSeparators(fi.absoluteFilePath()).toUtf8().constData()));
            if (editMgr_->saveEditAs(file)) {
                notifyEditChanged();
                return ok(id, QJsonObject{{"saved", true}, {"path", QDir::toNativeSeparators(fi.absoluteFilePath())}});
            }
            return err(id, Failed, "Failed to save the project to that path.");
        }
        if (editMgr_->saveEdit())
            return ok(id, "Project saved.");
        return err(id, Conflict, "The project has no file yet: call save_project with a path (.tracktionedit).");
    };

    handlers_["undo"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        auto* um = undoManager();
        if (!um) return err(id, Conflict, "No project loaded.");
        if (!um->canUndo()) return err(id, Conflict, "Nothing to undo.");
        editMgr_->undo();
        return ok(id, "Undo performed.");
    };

    handlers_["redo"] = [this](const QJsonObject&, const QString& id) -> AiToolResult {
        auto* um = undoManager();
        if (!um) return err(id, Conflict, "No project loaded.");
        if (!um->canRedo()) return err(id, Conflict, "Nothing to redo.");
        editMgr_->redo();
        return ok(id, "Redo performed.");
    };

    handlers_["setup_midi_channels"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        if (!editMgr_->isMidiTrack(track))
            return err(id, InvalidArgument, "Track is not a MIDI track.");

        te::MidiClip* refClip = nullptr;
        for (auto* clip : track->getClips()) {
            if (auto* mc = dynamic_cast<te::MidiClip*>(clip)) {
                refClip = mc;
                break;
            }
        }
        if (!refClip) {
            refClip = editMgr_->addMidiClipToTrack(*track, 0.0, 16.0);
            if (!refClip) return err(id, Failed, "Failed to create reference MIDI clip.");
        }

        QJsonArray channels = input["channels"].toArray();
        QJsonArray created;
        for (const auto& chVal : channels) {
            QJsonObject chObj = chVal.toObject();
            int ch = chObj["channel"].toInt(1);
            QString name = chObj["name"].toString();

            if (ch == refClip->getMidiChannel().getChannelNumber()) {
                if (!name.isEmpty())
                    editMgr_->setChannelName(*refClip, name);
                QJsonObject entry;
                entry["channel"] = ch;
                entry["name"] = name;
                entry["status"] = "existing";
                entry["clip_id"] = ids::of(refClip);
                created.append(entry);
                continue;
            }

            auto* newClip = editMgr_->addLinkedMidiChannel(*track, *refClip, ch, name);
            QJsonObject entry;
            entry["channel"] = ch;
            entry["name"] = name;
            entry["status"] = newClip ? "created" : "failed";
            if (newClip) entry["clip_id"] = ids::of(newClip);
            created.append(entry);
        }

        notifyEditChanged();
        return ok(id, created);
    };

    handlers_["set_channel_name"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);
        int ch = input["channel"].toInt(0);
        QString name = input["name"].toString();
        if (ch < 1 || ch > 16) return err(id, InvalidArgument, "Channel must be 1-16.");
        if (name.isEmpty()) return err(id, InvalidArgument, "Name cannot be empty.");

        for (auto* clip : track->getClips()) {
            if (auto* mc = dynamic_cast<te::MidiClip*>(clip)) {
                if (mc->getMidiChannel().getChannelNumber() == ch) {
                    editMgr_->setChannelName(*mc, name);
                    return ok(id, QString("Channel %1 renamed to '%2'.").arg(ch).arg(name));
                }
            }
        }
        return err(id, NotFound, QString("No MIDI clip on channel %1 found.").arg(ch));
    };

    handlers_["get_channel_names"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);

        QJsonArray result;
        for (auto* clip : track->getClips()) {
            if (auto* mc = dynamic_cast<te::MidiClip*>(clip)) {
                QJsonObject entry;
                entry["channel"] = mc->getMidiChannel().getChannelNumber();
                entry["name"] = editMgr_->getChannelName(mc);
                entry["clip_id"] = ids::of(mc);
                result.append(entry);
            }
        }
        return ok(id, result);
    };

    // ── MIDI Clip / Note Creation ────────────────────────────────────────────

    handlers_["create_midi_clip"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);

        double startBeat = input["start_beat"].toDouble();
        double lengthBeats = input["length_beats"].toDouble();
        if (lengthBeats <= 0.0) return err(id, InvalidArgument, "length_beats must be positive.");

        auto* clip = editMgr_->addMidiClipToTrack(*track, startBeat, lengthBeats);
        if (!clip) return err(id, Failed, "Failed to create MIDI clip.");

        if (input.contains("channel")) {
            int ch = std::clamp(input["channel"].toInt(1), 1, 16);
            clip->setMidiChannel(te::MidiChannel(ch));
        }

        if (input.contains("name") && !input["name"].toString().isEmpty())
            clip->setName(juce::String(input["name"].toString().toStdString()));

        if (!editMgr_->isMidiTrack(track))
            editMgr_->markAsMidiTrack(track);

        const int clipIndex = track->getClips().indexOf(clip);

        notifyEditChanged();
        notifyTracksChanged();

        QJsonObject result;
        result["id"] = ids::of(clip);
        result["clip_index"] = clipIndex;
        result["track_id"] = ids::of(track);
        result["name"] = qstr(clip->getName());
        result["start_beat"] = startBeat;
        result["length_beats"] = lengthBeats;
        result["channel"] = clip->getMidiChannel().getChannelNumber();
        return ok(id, result);
    };

    handlers_["add_midi_notes"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        te::AudioTrack* track = nullptr;
        te::MidiClip* midiClip = nullptr;
        int clipIdx = -1;
        if (auto e = resolveMidiClipTarget(input, id, track, midiClip, clipIdx)) return *e;

        QJsonArray notes = input["notes"].toArray();
        if (notes.isEmpty()) return err(id, InvalidArgument, "No notes provided.");

        auto& seq = midiClip->getSequence();
        auto* um = undoManager();
        int added = 0;

        for (const auto& noteVal : notes) {
            QJsonObject noteObj = noteVal.toObject();
            int noteNum = noteObj["note"].toInt(-1);
            double startBeat = noteObj["start_beat"].toDouble(0.0);
            double lenBeats = noteObj["length_beats"].toDouble(1.0);
            int velocity = noteObj.contains("velocity") ? noteObj["velocity"].toInt(100) : 100;
            int channel = noteObj.contains("channel") ? noteObj["channel"].toInt(1) : 1;

            if (noteNum < 0 || noteNum > 127) continue;
            velocity = std::clamp(velocity, 1, 127);
            channel = std::clamp(channel, 1, 16);
            if (lenBeats <= 0.0) lenBeats = 0.25;

            seq.addNote(noteNum,
                        tracktion::BeatPosition::fromBeats(startBeat),
                        tracktion::BeatDuration::fromBeats(lenBeats),
                        velocity, channel - 1, um);
            added++;
        }

        notifyEditChanged();
        if (added > 0)
            notifyMidiClipModified(midiClip);

        QJsonObject result;
        result["added"] = added;
        result["total_notes"] = seq.getNotes().size();
        result["clip_id"] = ids::of(midiClip);
        result["clip_index"] = clipIdx;
        return ok(id, result);
    };

    handlers_["get_clips_on_track"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return trackNotFound(id, input["track"]);

        auto& ts = editMgr_->edit()->tempoSequence;
        QJsonArray arr;
        int idx = 0;
        for (auto* clip : track->getClips()) {
            QJsonObject obj;
            obj["id"] = ids::of(clip);
            obj["clip_index"] = idx;
            obj["index"] = idx;
            ++idx;
            obj["name"] = qstr(clip->getName());

            double startBeat = ts.toBeats(clip->getPosition().getStart()).inBeats();
            double endBeat = ts.toBeats(clip->getPosition().getEnd()).inBeats();
            obj["start_beat"] = startBeat;
            obj["length_beats"] = endBeat - startBeat;

            if (auto* mc = dynamic_cast<te::MidiClip*>(clip)) {
                obj["type"] = "midi";
                obj["channel"] = mc->getMidiChannel().getChannelNumber();
                obj["note_count"] = mc->getSequence().getNotes().size();
            } else {
                obj["type"] = "audio";
            }

            arr.append(obj);
        }
        return ok(id, arr);
    };

    handlers_["clear_midi_notes"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        te::AudioTrack* track = nullptr;
        te::MidiClip* midiClip = nullptr;
        int clipIdx = -1;
        if (auto e = resolveMidiClipTarget(input, id, track, midiClip, clipIdx)) return *e;

        auto& seq = midiClip->getSequence();
        int removed = seq.getNotes().size();
        seq.removeAllNotes(undoManager());

        notifyEditChanged();
        notifyMidiClipModified(midiClip);

        return ok(id, QString("Removed %1 notes from clip '%2'.")
                      .arg(removed)
                      .arg(qstr(midiClip->getName())));
    };

    handlers_["set_clip_channel"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        te::AudioTrack* track = nullptr;
        te::MidiClip* midiClip = nullptr;
        int clipIdx = -1;
        if (auto e = resolveMidiClipTarget(input, id, track, midiClip, clipIdx)) return *e;

        int newChannel = std::clamp(input["channel"].toInt(1), 1, 16);
        int oldChannel = midiClip->getMidiChannel().getChannelNumber();
        midiClip->setMidiChannel(te::MidiChannel(newChannel));

        notifyEditChanged();
        notifyMidiClipModified(midiClip);

        return ok(id, QString("Moved clip '%1' from channel %2 to channel %3.")
                      .arg(qstr(midiClip->getName()))
                      .arg(oldChannel).arg(newChannel));
    };

    // ── MIDI Note Reading & Surgical Editing ─────────────────────────────────

    handlers_["get_midi_notes"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        te::AudioTrack* track = nullptr;
        te::MidiClip* midiClip = nullptr;
        int clipIdx = -1;
        if (auto e = resolveMidiClipTarget(input, id, track, midiClip, clipIdx)) return *e;

        const bool hasChannelFilter = input.contains("channel");
        const int channelFilter = hasChannelFilter ? input["channel"].toInt(0) : 0;
        const bool hasNoteMin = input.contains("note_min");
        const int noteMin = hasNoteMin ? input["note_min"].toInt(0) : 0;
        const bool hasNoteMax = input.contains("note_max");
        const int noteMax = hasNoteMax ? input["note_max"].toInt(127) : 127;
        const bool hasStartMin = input.contains("start_beat_min");
        const double startBeatMin = hasStartMin ? input["start_beat_min"].toDouble(0.0) : 0.0;
        const bool hasStartMax = input.contains("start_beat_max");
        const double startBeatMax = hasStartMax ? input["start_beat_max"].toDouble(100000.0) : 100000.0;

        auto& seq = midiClip->getSequence();
        QJsonArray notesArr;

        for (auto* note : seq.getNotes()) {
            int noteNum = note->getNoteNumber();
            double startBeat = note->getStartBeat().inBeats();
            int colour = note->getColour();
            int channel = colour + 1;

            if (hasChannelFilter && channel != channelFilter) continue;
            if ((hasNoteMin || hasNoteMax) && (noteNum < noteMin || noteNum > noteMax)) continue;
            if ((hasStartMin || hasStartMax) && (startBeat < startBeatMin || startBeat >= startBeatMax)) continue;

            QJsonObject noteObj;
            noteObj["note"] = noteNum;
            noteObj["start_beat"] = startBeat;
            noteObj["length_beats"] = note->getLengthBeats().inBeats();
            noteObj["velocity"] = note->getVelocity();
            noteObj["channel"] = channel;
            notesArr.append(noteObj);
        }

        QJsonObject result;
        result["clip_id"] = ids::of(midiClip);
        result["clip_index"] = clipIdx;
        result["clip_name"] = qstr(midiClip->getName());
        result["clip_channel"] = midiClip->getMidiChannel().getChannelNumber();
        result["total_notes_in_clip"] = seq.getNotes().size();
        result["returned_notes"] = notesArr.size();
        result["notes"] = notesArr;
        return ok(id, result);
    };

    handlers_["remove_midi_notes"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        te::AudioTrack* track = nullptr;
        te::MidiClip* midiClip = nullptr;
        int clipIdx = -1;
        if (auto e = resolveMidiClipTarget(input, id, track, midiClip, clipIdx)) return *e;

        const bool hasChannelFilter = input.contains("channel");
        const int channelFilter = hasChannelFilter ? input["channel"].toInt(0) : 0;
        const bool hasNoteMin = input.contains("note_min");
        const int noteMin = hasNoteMin ? input["note_min"].toInt(0) : 0;
        const bool hasNoteMax = input.contains("note_max");
        const int noteMax = hasNoteMax ? input["note_max"].toInt(127) : 127;
        const bool hasStartMin = input.contains("start_beat_min");
        const double startBeatMin = hasStartMin ? input["start_beat_min"].toDouble(0.0) : 0.0;
        const bool hasStartMax = input.contains("start_beat_max");
        const double startBeatMax = hasStartMax ? input["start_beat_max"].toDouble(100000.0) : 100000.0;

        if (!hasChannelFilter && !hasNoteMin && !hasNoteMax && !hasStartMin && !hasStartMax)
            return err(id, InvalidArgument,
                       "At least one filter (channel, note_min/note_max, or start_beat_min/start_beat_max) is required. Use clear_midi_notes to remove all notes.");

        auto& seq = midiClip->getSequence();
        QVector<te::MidiNote*> toRemove;

        for (auto* note : seq.getNotes()) {
            int noteNum = note->getNoteNumber();
            double startBeat = note->getStartBeat().inBeats();
            int colour = note->getColour();
            int channel = colour + 1;

            bool matches = true;
            if (hasChannelFilter && channel != channelFilter) matches = false;
            if ((hasNoteMin || hasNoteMax) && (noteNum < noteMin || noteNum > noteMax)) matches = false;
            if ((hasStartMin || hasStartMax) && (startBeat < startBeatMin || startBeat >= startBeatMax)) matches = false;

            if (matches)
                toRemove.append(note);
        }

        int removed = toRemove.size();
        auto* um = undoManager();
        for (auto* note : toRemove)
            seq.removeNote(*note, um);

        notifyEditChanged();
        if (removed > 0)
            notifyMidiClipModified(midiClip);

        QJsonObject result;
        result["removed"] = removed;
        result["remaining_notes"] = seq.getNotes().size();
        return ok(id, result);
    };

    // ── Batching ─────────────────────────────────────────────────────────────

    handlers_["run_batch"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        return runBatch(input, id);
    };
}

// ── run_batch ───────────────────────────────────────────────────────────────

AiToolResult AiToolExecutor::runBatch(const QJsonObject& input, const QString& id)
{
    using namespace AiErrorCode;

    if (inRunBatch_)
        return err(id, NotAllowed, "run_batch cannot be nested.");

    const QJsonArray commands = input["commands"].toArray();
    const bool atomic = input.contains("atomic") ? input["atomic"].toBool() : true;
    const bool stopOnError = atomic || (input.contains("stop_on_error") ? input["stop_on_error"].toBool() : true);

    // Validate every command before anything runs.
    struct Step { QString tool; QJsonObject input; ToolEntry entry; };
    QVector<Step> steps;
    steps.reserve(commands.size());
    for (int i = 0; i < commands.size(); ++i) {
        const QJsonObject cmd = commands[i].toObject();
        const QString tool = cmd["tool"].toString();
        const QJsonObject cmdInput = cmd["input"].toObject();
        QJsonObject where{{"index", i}, {"tool", tool}};

        auto it = tools_.constFind(tool);
        if (it == tools_.constEnd())
            return err(id, UnknownTool, QString("Command %1: unknown tool '%2'.").arg(i).arg(tool), where);
        if (tool == "run_batch" || !it->flags.batchable)
            return err(id, NotAllowed,
                       QString("Command %1: '%2' is not allowed inside run_batch. Call it on its own.").arg(i).arg(tool),
                       where);
        if (auto e = AiSchemaValidator::validate(QJsonValue(cmdInput), it->inputSchema,
                                                 QString("$.commands[%1].input").arg(i))) {
            where["path"] = e->path;
            return err(id, InvalidArgument, QString("Command %1 (%2): %3").arg(i).arg(tool, e->message), where);
        }
        steps.append(Step{tool, cmdInput, it.value()});
    }

    auto* um = undoManager();
    if (!um) return err(id, Conflict, "No project loaded.");

    // The whole batch is one undo transaction: the undo group stops
    // EditManager methods from opening transactions of their own. Something
    // outside EditManager might still begin one, so count them too and roll
    // all of them back.
    const int undoBefore = um->getUndoDescriptions().size();
    const juce::Time batchStart = juce::Time::getCurrentTime();
    // The user's redo entries are stashed by the batch's first change; JUCE
    // restores them only through undoCurrentTransactionOnly().
    const bool hadRedo = um->canRedo();
    um->beginNewTransaction("Agent batch");

    beginBatch();
    inRunBatch_ = true;

    QJsonArray results;
    int failures = 0;
    int failedIndex = -1;
    AiToolResult failed;
    {
        ScopedUndoGroup group(editMgr_);
        for (int i = 0; i < steps.size(); ++i) {
            AiToolCall sub;
            sub.id = QString("%1#%2").arg(id).arg(i);
            sub.name = steps[i].tool;
            sub.input = steps[i].input;
            const AiToolResult r = dispatch(steps[i].entry, sub);

            QJsonObject row;
            row["tool"] = sub.name;
            row["ok"] = !r.isError;
            if (r.isError)
                row["error"] = QJsonObject{{"code", r.errorCode}, {"message", r.content}};
            else
                row["result"] = r.structured;

            if (r.isError) {
                ++failures;
                if (failedIndex < 0) {
                    failedIndex = i;
                    failed = r;
                }
                if (atomic) break;        // reported as failed_index/error, results hold the steps before it
                results.append(row);
                if (stopOnError) break;
                continue;
            }
            results.append(row);
        }
    }

    inRunBatch_ = false;

    if (failedIndex >= 0 && atomic) {
        const int undoAfter = um->getUndoDescriptions().size();
        const int newTransactions = undoAfter - undoBefore;
        bool rolledBack = true;
        bool redoPreserved = true;
        int undone = 0;
        QJsonArray reverted;   // names of the undo transactions rolled back, for the agent to check
        auto undoOne = [&]() {
            const QString what = qstr(um->getUndoDescription());
            if (!um->undo()) return false;
            reverted.append(what);
            ++undone;
            return true;
        };

        const bool changedAnything = newTransactions > 0 || um->getNumActionsInCurrentTransaction() > 0;
        if (newTransactions <= 1 && um->getNumActionsInCurrentTransaction() > 0) {
            // The normal case: one transaction, still open.
            const QString what = qstr(um->getUndoDescription());
            if (hadRedo) {
                // Undoes the batch, drops it from the redo side and puts the
                // user's stashed redo entries back.
                rolledBack = um->undoCurrentTransactionOnly();
                if (rolledBack) { reverted.append(what); ++undone; }
            } else {
                // No user redo to restore (and JUCE's stash may hold stale
                // entries from long ago, so never restore it here).
                rolledBack = undoOne();
            }
        } else if (newTransactions > 0) {
            for (int i = 0; i < newTransactions; ++i)
                if (!undoOne()) { rolledBack = false; break; }
        }
        // If a long batch pushed old history out of the undo manager the count
        // above comes up short; transactions begun after the batch started
        // are the batch's own.
        while (rolledBack && um->canUndo() && um->getTimeOfUndoTransaction() > batchStart) {
            if (!undoOne()) { rolledBack = false; break; }
        }

        // Unless undoCurrentTransactionOnly() restored the user's redo, the
        // rolled-back changes now sit on the redo side, where redo would
        // re-apply the partial batch. Replace them with an empty entry: a
        // no-op action stashes them (out of reach) and is itself undone.
        if (changedAnything && um->canRedo() && !(hadRedo && undone == 1 && newTransactions <= 1)) {
            um->beginNewTransaction("Agent batch (rolled back)");
            if (um->perform(new NoOpAction()))
                um->undo();
            redoPreserved = !hadRedo;
        }

        // Tracks the rollback deleted are freed already: refresh the track and
        // routing views now, before anything emits editChanged (which would
        // make them read the stale tracks), whatever the batch depth.
        dirtyMidi_ = false;
        lastDirtyMidiClip_ = nullptr;
        if (editMgr_) {
            emit editMgr_->tracksChanged();
            emit editMgr_->routingChanged();
        }
        dirtyTracks_ = dirtyRouting_ = dirtyEdit_ = false;
        if (editMgr_) editMgr_->notifyEdited();
        endBatch();

        QJsonObject details;
        details["failed_index"] = failedIndex;
        details["failed_tool"] = steps[failedIndex].tool;
        details["error"] = QJsonObject{{"code", failed.errorCode}, {"message", failed.content}};
        details["rolled_back"] = rolledBack;
        details["undo_steps_reverted"] = undone;
        details["reverted_transactions"] = reverted;
        details["redo_history_preserved"] = redoPreserved;
        details["results"] = results;   // the commands that ran before the failing one
        return err(id, BatchFailed,
                   QString("Batch command %1 (%2) failed: %3 %4")
                       .arg(failedIndex).arg(steps[failedIndex].tool, failed.content,
                                             rolledBack ? QString("All batch changes were rolled back.")
                                                        : QString("Rollback was incomplete; check the project.")),
                   details);
    }

    endBatch();

    QJsonObject out;
    out["results"] = results;
    out["count"] = results.size();
    if (!atomic) {
        out["failed"] = failures;
        out["stopped_early"] = failedIndex >= 0 && stopOnError;
    }
    return ok(id, out);
}

// ── Signal batching ─────────────────────────────────────────────────────────

void AiToolExecutor::beginBatch()
{
    if (batchDepth_++ > 0) return;
    dirtyEdit_ = false;
    dirtyTracks_ = false;
    dirtyRouting_ = false;
    dirtyMidi_ = false;
    lastDirtyMidiClip_ = nullptr;
}

void AiToolExecutor::endBatch()
{
    if (batchDepth_ <= 0) return;
    if (--batchDepth_ > 0) return;
    if (editMgr_) {
        if (dirtyTracks_)  emit editMgr_->tracksChanged();
        else if (dirtyEdit_) emit editMgr_->editChanged();
        if (dirtyRouting_) emit editMgr_->routingChanged();
        if (dirtyMidi_ && lastDirtyMidiClip_)
            emit editMgr_->midiClipModified(lastDirtyMidiClip_);
    }
    dirtyEdit_ = dirtyTracks_ = dirtyRouting_ = dirtyMidi_ = false;
    lastDirtyMidiClip_ = nullptr;
}

void AiToolExecutor::notifyEditChanged()
{
    if (batchDepth_ > 0) { dirtyEdit_ = true; return; }
    if (editMgr_) emit editMgr_->editChanged();
}

void AiToolExecutor::notifyTracksChanged()
{
    if (batchDepth_ > 0) { dirtyTracks_ = true; return; }
    if (editMgr_) emit editMgr_->tracksChanged();
}

void AiToolExecutor::notifyRoutingChanged()
{
    if (batchDepth_ > 0) { dirtyRouting_ = true; return; }
    if (editMgr_) emit editMgr_->routingChanged();
}

void AiToolExecutor::notifyMidiClipModified(te::MidiClip* clip)
{
    if (batchDepth_ > 0) { dirtyMidi_ = true; lastDirtyMidiClip_ = clip; return; }
    if (editMgr_) emit editMgr_->midiClipModified(clip);
}

// ── Public API ──────────────────────────────────────────────────────────────

std::optional<AiToolResult> AiToolExecutor::checkBusy(const QString& toolName, const AiToolFlags& flags,
                                                      const QString& toolUseId) const
{
    // An export renders the Edit on a worker thread; changing it now could crash.
    if (editMgr_ && editMgr_->isRenderInProgress())
        return err(toolUseId, AiErrorCode::Busy, "A render/export is in progress. Try again when it finishes.");
    if (editMgr_ && editMgr_->edit() && editMgr_->transport().isRecording()) {
        const bool renders = toolName == "measure_mix_loudness";
        if ((flags.mutating || renders) && toolName != "stop")
            return err(toolUseId, AiErrorCode::Busy,
                       "Recording is in progress. Call 'stop' first, then try again.");
    }
    return std::nullopt;
}

AiToolResult AiToolExecutor::dispatch(const ToolEntry& entry, const AiToolCall& call)
{
    AiToolResult r;
    try {
        r = entry.handler(call.input, call.id);
    } catch (const std::exception& e) {
        return err(call.id, AiErrorCode::Internal,
                   QString("Tool '%1' failed: %2").arg(call.name, QString::fromUtf8(e.what())));
    } catch (...) {
        return err(call.id, AiErrorCode::Internal, QString("Tool '%1' failed.").arg(call.name));
    }

    if (r.toolUseId.isEmpty()) r.toolUseId = call.id;
    if (r.isError) {
        if (r.errorCode.isEmpty()) r.errorCode = AiErrorCode::Failed;
        if (r.structured.isEmpty())
            r.structured = QJsonObject{{"error", QJsonObject{{"code", r.errorCode}, {"message", r.content}}}};
    } else if (r.structured.isEmpty()) {
        r.structured = structuredFromContent(r.content);
    }
    return r;
}

AiToolResult AiToolExecutor::execute(const AiToolCall& call)
{
    auto it = tools_.constFind(call.name);
    if (it == tools_.constEnd())
        return err(call.id, AiErrorCode::UnknownTool, QString("Unknown tool '%1'.").arg(call.name),
                   {{"tool", call.name}});
    const ToolEntry entry = it.value();

    if (auto busy = checkBusy(call.name, entry.flags, call.id))
        return *busy;

    if (auto e = AiSchemaValidator::validate(QJsonValue(call.input), entry.inputSchema))
        return err(call.id, AiErrorCode::InvalidArgument, e->message, {{"path", e->path}});

    // One undo step per mutating tool. undo/redo must not open one, and
    // run_batch opens its own.
    // The undo group keeps EditManager methods the handler calls from
    // splitting the step with transactions of their own. (Pattern tools also
    // hold PatternManager's external transaction themselves.)
    static const QSet<QString> ownUndoHandling = {"undo", "redo", "revert_last_mix_stage", "run_batch"};
    if (entry.flags.mutating && !inRunBatch_ && !ownUndoHandling.contains(call.name)) {
        if (auto* um = undoManager()) {
            um->beginNewTransaction(juce::String(call.name.toStdString()));
            ScopedUndoGroup group(editMgr_);
            return dispatch(entry, call);
        }
    }

    return dispatch(entry, call);
}

} // namespace OpenDaw
