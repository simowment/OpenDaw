#include "CommandGateway.h"

#include "ai/AiToolExecutor.h"
#include "ai/AiToolExtensions.h"
#include "ai/AiTypes.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <exception>

#ifndef OpenDaw_VERSION
#define OpenDaw_VERSION "dev"
#endif

namespace OpenDaw {

namespace {

const QString kJsonRpc = QStringLiteral("2.0");

QString serverInstructions()
{
    return QStringLiteral(
        "OpenDaw is a digital audio workstation; these tools edit the project that is open in it. "
        "Start with get_project_info and get_track_list: every track, clip, plugin and pattern has a "
        "stable id such as \"track:1043\", \"clip:2210\", \"plugin:877\" or \"pattern:3\". Pass these "
        "ids rather than names or indices; they survive renames, reordering and save/load. "
        "Positions are in beats unless a parameter says seconds. "
        "For multi-step edits use run_batch: the steps run in order as one undo step and, when "
        "atomic, are rolled back together if any step fails. "
        "To hear what you made, render with render_region and read the analysis it returns "
        "(loudness, peaks, spectrum) or pass the rendered file to the analysis tools, then adjust and "
        "render again. Failed calls return isError with structuredContent.error.code, e.g. "
        "NOT_FOUND, INVALID_ARGUMENT, BUSY or BATCH_FAILED.");
}

QJsonObject textContent(const QString& text)
{
    return QJsonObject{{"type", "text"}, {"text", text}};
}

QString compactJson(const QJsonObject& obj)
{
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// MCP CallToolResult from an executor result.
QJsonObject toCallToolResult(const AiToolResult& r)
{
    QJsonObject structured = r.structured;
    QString text;

    if (!structured.isEmpty()) {
        text = compactJson(structured);
    } else {
        // Results built without the structured helpers: keep the text as is
        // and derive a structured form from it when possible.
        text = r.content;
        if (r.isError) {
            structured = QJsonObject{{"error", QJsonObject{
                {"code", r.errorCode.isEmpty() ? AiErrorCode::Failed : r.errorCode},
                {"message", r.content}}}};
        } else {
            QJsonParseError pe{};
            const auto doc = QJsonDocument::fromJson(r.content.toUtf8(), &pe);
            if (pe.error == QJsonParseError::NoError && doc.isObject())
                structured = doc.object();
            else if (pe.error == QJsonParseError::NoError && doc.isArray())
                structured = QJsonObject{{"items", doc.array()}};
            else if (!r.content.isEmpty())
                structured = QJsonObject{{"message", r.content}};
        }
    }

    if (text.isEmpty())
        text = r.isError ? QStringLiteral("Tool failed.") : QStringLiteral("OK");

    QJsonObject result;
    result["content"] = QJsonArray{textContent(text)};
    if (!structured.isEmpty())
        result["structuredContent"] = structured;
    result["isError"] = r.isError;
    return result;
}

QJsonObject toolErrorResult(const QString& code, const QString& message)
{
    return toCallToolResult(aiErr(QString(), code, message));
}

} // namespace

CommandGateway::CommandGateway(AiToolExecutor& executor, QObject* parent)
    : QObject(parent)
    , executor_(executor)
{
}

QString CommandGateway::protocolVersion()
{
    return QStringLiteral("2025-06-18");
}

QStringList CommandGateway::supportedProtocolVersions()
{
    return {QStringLiteral("2025-06-18"), QStringLiteral("2025-03-26"), QStringLiteral("2024-11-05")};
}

void CommandGateway::setAllowDestructive(bool allow)
{
    allowDestructive_ = allow;
}

QJsonObject CommandGateway::makeError(const QJsonValue& id, int code, const QString& message,
                                      const QJsonValue& data)
{
    QJsonObject err{{"code", code}, {"message", message}};
    if (!data.isUndefined())
        err["data"] = data;
    QJsonObject resp{{"jsonrpc", kJsonRpc}, {"error", err}};
    resp["id"] = id.isUndefined() ? QJsonValue(QJsonValue::Null) : id;
    return resp;
}

QJsonObject CommandGateway::makeResult(const QJsonValue& id, const QJsonObject& result)
{
    return QJsonObject{{"jsonrpc", kJsonRpc}, {"id", id}, {"result", result}};
}

QJsonArray CommandGateway::handleBatch(const QJsonArray& messages)
{
    QJsonArray responses;
    if (messages.isEmpty()) {
        responses.append(makeError(QJsonValue::Null, InvalidRequest, QStringLiteral("Empty batch.")));
        return responses;
    }
    for (const auto& v : messages) {
        if (!v.isObject()) {
            responses.append(makeError(QJsonValue::Null, InvalidRequest,
                                       QStringLiteral("Batch entries must be JSON-RPC objects.")));
            continue;
        }
        const auto resp = handleMessage(v.toObject());
        if (!resp.isEmpty())
            responses.append(resp);
    }
    return responses;
}

QJsonObject CommandGateway::handleMessage(const QJsonObject& message)
{
    const bool hasId = message.contains("id");
    const QJsonValue id = hasId ? message.value("id") : QJsonValue(QJsonValue::Null);

    // A response from the client (we never send requests): nothing to answer.
    if (!message.contains("method") && (message.contains("result") || message.contains("error")))
        return {};

    if (hasId && !(id.isString() || id.isDouble() || id.isNull()))
        return makeError(QJsonValue::Null, InvalidRequest, QStringLiteral("\"id\" must be a string or a number."));

    if (message.contains("jsonrpc") && message.value("jsonrpc").toString() != kJsonRpc)
        return makeError(id, InvalidRequest, QStringLiteral("Only JSON-RPC 2.0 is supported."));

    const QJsonValue methodValue = message.value("method");
    if (!methodValue.isString() || methodValue.toString().isEmpty())
        return makeError(id, InvalidRequest, QStringLiteral("Missing \"method\"."));
    const QString method = methodValue.toString();

    const QJsonValue paramsValue = message.value("params");
    if (!paramsValue.isUndefined() && !paramsValue.isNull() && !paramsValue.isObject()) {
        if (!hasId) return {};
        return makeError(id, InvalidParams, QStringLiteral("\"params\" must be an object."));
    }
    const QJsonObject params = paramsValue.toObject();

    // Notifications never get a response. A tools/call sent as a notification
    // still runs (the caller just does not get the result).
    if (!hasId) {
        if (method == QLatin1String("tools/call") && params.value("name").isString())
            handleToolsCall(params);
        return {};
    }

    try {
        if (method == QLatin1String("initialize"))
            return makeResult(id, handleInitialize(params));
        if (method == QLatin1String("ping"))
            return makeResult(id, QJsonObject{});
        if (method == QLatin1String("tools/list"))
            return makeResult(id, handleToolsList());
        if (method == QLatin1String("tools/call")) {
            if (!params.value("name").isString() || params.value("name").toString().isEmpty())
                return makeError(id, InvalidParams, QStringLiteral("tools/call needs a string \"name\"."));
            const QJsonValue args = params.value("arguments");
            if (!args.isUndefined() && !args.isNull() && !args.isObject())
                return makeError(id, InvalidParams, QStringLiteral("\"arguments\" must be an object."));
            return makeResult(id, handleToolsCall(params));
        }
        // Capabilities we do not advertise, answered leniently with empty lists
        // because some clients probe them anyway.
        if (method == QLatin1String("resources/list"))
            return makeResult(id, QJsonObject{{"resources", QJsonArray{}}});
        if (method == QLatin1String("resources/templates/list"))
            return makeResult(id, QJsonObject{{"resourceTemplates", QJsonArray{}}});
        if (method == QLatin1String("prompts/list"))
            return makeResult(id, QJsonObject{{"prompts", QJsonArray{}}});
        if (method == QLatin1String("logging/setLevel"))
            return makeResult(id, QJsonObject{});
    } catch (const std::exception& e) {
        return makeError(id, InternalError, QStringLiteral("Internal error: %1").arg(QString::fromUtf8(e.what())));
    } catch (...) {
        return makeError(id, InternalError, QStringLiteral("Internal error."));
    }

    return makeError(id, MethodNotFound, QStringLiteral("Method not found: %1").arg(method));
}

QJsonObject CommandGateway::handleInitialize(const QJsonObject& params) const
{
    const QString requested = params.value("protocolVersion").toString();
    const QString version = supportedProtocolVersions().contains(requested) ? requested : protocolVersion();

    QJsonObject result;
    result["protocolVersion"] = version;
    result["capabilities"] = QJsonObject{{"tools", QJsonObject{{"listChanged", false}}}};
    result["serverInfo"] = QJsonObject{
        {"name", "opendaw"},
        {"title", "OpenDaw"},
        {"version", QString::fromUtf8(OpenDaw_VERSION)},
    };
    result["instructions"] = serverInstructions();
    return result;
}

QJsonObject CommandGateway::handleToolsList() const
{
    QJsonArray tools;
    const QJsonArray defs = executor_.toolDefinitions();
    for (const auto& v : defs) {
        const QJsonObject def = v.toObject();
        const QString name = def.value("name").toString();
        if (name.isEmpty())
            continue;

        QJsonObject schema = def.value("input_schema").toObject();
        if (schema.isEmpty())
            schema = def.value("inputSchema").toObject();
        if (!schema.contains("type"))
            schema["type"] = QStringLiteral("object");

        const AiToolFlags flags = executor_.toolFlags(name);
        const bool destructive = toolIsDestructive(name);

        QJsonObject tool;
        tool["name"] = name;
        if (def.contains("title"))
            tool["title"] = def.value("title");
        QString description = def.value("description").toString();
        if (destructive && !allowDestructive_)
            description += QStringLiteral(" (Disabled: destructive tools are turned off in OpenDaw's settings.)");
        tool["description"] = description;
        tool["inputSchema"] = schema;
        tool["annotations"] = QJsonObject{
            {"readOnlyHint", !flags.mutating},
            {"destructiveHint", destructive},
            {"idempotentHint", false},
            {"openWorldHint", false},
        };
        tools.append(tool);
    }
    // Everything fits in one page: no nextCursor, and any cursor is ignored.
    return QJsonObject{{"tools", tools}};
}

bool CommandGateway::toolIsDestructive(const QString& name) const
{
    return executor_.isDestructive(name) || executor_.toolFlags(name).destructive;
}

QJsonObject CommandGateway::handleToolsCall(const QJsonObject& params)
{
    const QString name = params.value("name").toString();
    const QJsonObject arguments = params.value("arguments").toObject();

    QElapsedTimer timer;
    timer.start();

    QJsonObject result;
    bool isError = true;

    if (!executor_.hasTool(name)) {
        result = toolErrorResult(AiErrorCode::UnknownTool,
                                 QStringLiteral("Unknown tool '%1'. Call tools/list for the available tools.").arg(name));
    } else if (!allowDestructive_ && (toolIsDestructive(name) || executor_.isDestructiveCall(name, arguments))) {
        result = toolErrorResult(AiErrorCode::NotAllowed,
                                 QStringLiteral("'%1' is destructive (or runs a destructive command) and destructive "
                                                "tools are disabled for agents in OpenDaw (setting "
                                                "agent/allowDestructive).").arg(name));
    } else {
        AiToolCall call;
        call.id = QStringLiteral("mcp-%1").arg(++callCounter_);
        call.name = name;
        call.input = arguments;

        AiToolResult r;
        try {
            r = executor_.execute(call);
        } catch (const std::exception& e) {
            r = aiErr(call.id, AiErrorCode::Internal,
                      QStringLiteral("'%1' threw an exception: %2").arg(name, QString::fromUtf8(e.what())));
        } catch (...) {
            r = aiErr(call.id, AiErrorCode::Internal, QStringLiteral("'%1' threw an exception.").arg(name));
        }
        isError = r.isError;
        result = toCallToolResult(r);
    }

    const qint64 elapsed = timer.elapsed();
    emit toolCalled(name, isError, elapsed);
    return result;
}

} // namespace OpenDaw
