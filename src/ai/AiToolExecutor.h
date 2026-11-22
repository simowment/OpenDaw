#pragma once

#include "AiTypes.h"
#include "AiToolExtensions.h"
#include "AiAudioAnalysis.h"
#include "engine/AudioEngine.h"
#include "engine/EditManager.h"
#include "engine/PluginScanner.h"
#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QStringList>
#include <functional>
#include <optional>

namespace OpenDaw {

// Runs agent tools against the project. Built-in tools come from
// AiToolDefs::allTools() plus registerHandlers(); every provider in
// AiToolExtensionRegistry adds more when the executor is constructed.
// All calls run synchronously on the Qt GUI thread (= JUCE message thread).
class AiToolExecutor : public QObject {
    Q_OBJECT
public:
    explicit AiToolExecutor(EditManager* editMgr, AudioEngine* audioEngine,
                            PluginScanner* pluginScanner, QObject* parent = nullptr);

    // Validates the input against the tool's schema, then dispatches.
    // Errors carry errorCode (AiErrorCode::*); successes carry structured.
    AiToolResult execute(const AiToolCall& call);

    // Built-in tools followed by extension tools, Anthropic format
    // {name, description, input_schema}.
    QJsonArray toolDefinitions() const;
    bool hasTool(const QString& name) const;
    AiToolFlags toolFlags(const QString& name) const;
    bool isDestructive(const QString& toolName) const;
    // Like isDestructive(), but also looks inside a run_batch call's commands.
    bool isDestructiveCall(const QString& toolName, const QJsonObject& input) const;

    // Defers UI refresh signals until the matching endBatch(). Nests.
    void beginBatch();
    void endBatch();

    // Accepts "track:<id>", {"id":"track:<id>"}, a name, an index, or {"index","name"}.
    // A well-formed id that matches no track returns nullptr (never a name lookup).
    te::AudioTrack* resolveTrack(const QJsonValue& ref) const;
    // Uses input["clip_id"] ("clip:<id>") or input["clip_index"] (needs track).
    // With a clip_id and a null track the whole edit is searched.
    te::Clip* resolveClip(te::AudioTrack* track, const QJsonObject& input, QString* error = nullptr) const;
    // "plugin:<id>", an effect name or an index into the track's user effects.
    te::Plugin* resolveEffect(te::AudioTrack* track, const QJsonValue& ref) const;

    EditManager* editManager() const { return editMgr_; }
    AudioEngine* audioEngine() const { return audioEngine_; }
    PluginScanner* pluginScanner() const { return pluginScanner_; }

    // UI refresh (deferred while batching).
    void notifyEditChanged();
    void notifyTracksChanged();
    void notifyRoutingChanged();
    void notifyMidiClipModified(te::MidiClip* clip);

private:
    struct ToolEntry {
        QJsonObject definition;
        QJsonObject inputSchema;
        AiToolHandler handler;
        AiToolFlags flags;
    };

    void registerHandlers();
    void registerBuiltInTools();
    void registerExtensionTools();
    static AiToolFlags builtInFlags(const QString& name);

    // Runs the handler and fills errorCode/structured if the handler left them empty.
    AiToolResult dispatch(const ToolEntry& entry, const AiToolCall& call);
    std::optional<AiToolResult> checkBusy(const QString& toolName, const AiToolFlags& flags,
                                          const QString& toolUseId) const;
    AiToolResult runBatch(const QJsonObject& input, const QString& toolUseId);

    te::AutomatableParameter* resolveParameter(te::Plugin* plugin, const QJsonValue& paramRef) const;
    QVector<te::Plugin*> getUserPlugins(te::AudioTrack* track) const;
    te::Clip* resolveClipImpl(te::AudioTrack* track, const QJsonObject& input,
                              QString* error, QString* code) const;
    // Resolves track + clip_id/clip_index to a MIDI clip; returns the error result on failure.
    std::optional<AiToolResult> resolveMidiClipTarget(const QJsonObject& input, const QString& toolUseId,
                                                      te::AudioTrack*& track, te::MidiClip*& clip,
                                                      int& clipIndex) const;
    juce::UndoManager* undoManager() const;

    AiToolResult ok(const QString& toolUseId, const QJsonObject& data) const;
    AiToolResult ok(const QString& toolUseId, const QJsonArray& data) const;
    AiToolResult ok(const QString& toolUseId, const QString& message) const;
    AiToolResult err(const QString& toolUseId, const QString& code, const QString& message,
                     const QJsonObject& details = {}) const;
    AiToolResult trackNotFound(const QString& toolUseId, const QJsonValue& ref,
                               const QString& field = QStringLiteral("track")) const;
    AiToolResult effectNotFound(const QString& toolUseId, const QJsonValue& ref) const;

    using Handler = AiToolHandler;
    QMap<QString, Handler> handlers_;      // built-in handlers, filled by registerHandlers()
    QMap<QString, ToolEntry> tools_;       // every callable tool
    QStringList toolOrder_;                // definition order: built-ins, then extensions

    EditManager* editMgr_;
    AudioEngine* audioEngine_;
    PluginScanner* pluginScanner_;
    AiAudioAnalysis audioAnalysis_;

    int batchDepth_ = 0;
    bool inRunBatch_ = false;
    bool dirtyEdit_ = false;
    bool dirtyTracks_ = false;
    bool dirtyRouting_ = false;
    bool dirtyMidi_ = false;
    te::MidiClip* lastDirtyMidiClip_ = nullptr;
};

} // namespace OpenDaw
