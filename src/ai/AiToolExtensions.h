#pragma once

// Contract shared by the tool executor, the agent gateway and every module
// that contributes tools (rendering, patterns, ...).
//
// A module contributes tools by registering a provider once at start-up:
//
//     AiToolExtensionRegistry::instance().addProvider(
//         [svc](AiToolExecutor& ex) { return svc->toolExtensions(ex); });
//
// Every AiToolExecutor (the in-app chat has one, the external agent gateway
// has another) calls each provider from its constructor and adds the returned
// tools next to the built-in ones. Handlers run synchronously on the Qt GUI
// thread, which is also the JUCE message thread.

#include "AiTypes.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <functional>

namespace OpenDaw {

class AiToolExecutor;

namespace AiErrorCode {
inline const QString InvalidArgument = QStringLiteral("INVALID_ARGUMENT"); // failed schema validation or bad value
inline const QString NotFound        = QStringLiteral("NOT_FOUND");        // track/clip/plugin/pattern/file id or name unknown
inline const QString Busy            = QStringLiteral("BUSY");             // render/export or recording in progress
inline const QString UnknownTool     = QStringLiteral("UNKNOWN_TOOL");
inline const QString NotAllowed      = QStringLiteral("NOT_ALLOWED");      // e.g. tool not allowed inside a batch
inline const QString Conflict        = QStringLiteral("CONFLICT");         // state does not allow the operation
inline const QString BatchFailed     = QStringLiteral("BATCH_FAILED");     // a step of an atomic batch failed; rolled back
inline const QString Failed          = QStringLiteral("FAILED");           // operation attempted and failed
inline const QString Internal        = QStringLiteral("INTERNAL");
} // namespace AiErrorCode

struct AiToolFlags {
    bool mutating = true;     // changes the project; wrapped in its own undo transaction
    bool destructive = false; // in-app chat asks the user first; MCP gets destructiveHint
    bool batchable = true;    // may appear inside run_batch (false for transport, render, save, undo...)
};

using AiToolHandler = std::function<AiToolResult(const QJsonObject& input, const QString& toolUseId)>;

struct AiToolExtension {
    // Anthropic tool format: {name, description, input_schema:{type:"object", properties, required}}.
    // The input is validated against input_schema before the handler runs
    // (types, required, enum, minimum/maximum, items, oneOf; unknown properties are rejected).
    QJsonObject definition;
    AiToolHandler handler;
    AiToolFlags flags;
};

class AiToolExtensionRegistry {
public:
    using Provider = std::function<QVector<AiToolExtension>(AiToolExecutor&)>;

    static AiToolExtensionRegistry& instance() {
        static AiToolExtensionRegistry registry;
        return registry;
    }

    void addProvider(Provider p) { providers_.push_back(std::move(p)); }
    const QVector<Provider>& providers() const { return providers_; }
    void clear() { providers_.clear(); } // tests

private:
    QVector<Provider> providers_;
};

// ── Result helpers usable from any handler ──────────────────────────────────

inline AiToolResult aiOk(const QString& toolUseId, const QJsonObject& data) {
    AiToolResult r;
    r.toolUseId = toolUseId;
    r.content = QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact));
    r.structured = data;
    return r;
}

inline AiToolResult aiOkMessage(const QString& toolUseId, const QString& message) {
    AiToolResult r;
    r.toolUseId = toolUseId;
    r.content = message;
    r.structured = QJsonObject{{"message", message}};
    return r;
}

inline AiToolResult aiErr(const QString& toolUseId, const QString& code, const QString& message,
                          const QJsonObject& details = {}) {
    AiToolResult r;
    r.toolUseId = toolUseId;
    r.isError = true;
    r.errorCode = code;
    QJsonObject e{{"code", code}, {"message", message}};
    if (!details.isEmpty()) e["details"] = details;
    r.structured = QJsonObject{{"error", e}};
    r.content = message; // the in-app chat shows the plain message
    return r;
}

} // namespace OpenDaw
