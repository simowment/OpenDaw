#pragma once

// MCP (Model Context Protocol) server methods on top of AiToolExecutor.
//
// The gateway is transport-agnostic: it takes one parsed JSON-RPC 2.0 message
// (or a batch array) and returns the response. LocalApiServer feeds it lines
// read from local sockets; tests can call it directly.
//
// Supported methods: initialize, notifications/* (ignored), ping, tools/list,
// tools/call. Tool failures are reported as tool results with isError=true
// (as the MCP spec asks); JSON-RPC errors are used only for malformed
// requests. All calls run synchronously on the Qt GUI thread.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QStringList>

namespace OpenDaw {

class AiToolExecutor;

class CommandGateway : public QObject {
    Q_OBJECT
public:
    explicit CommandGateway(AiToolExecutor& executor, QObject* parent = nullptr);

    // One JSON-RPC message. Returns the response object, or {} for
    // notifications (messages without an "id") and for client responses.
    QJsonObject handleMessage(const QJsonObject& message);

    // A JSON-RPC batch. Returns the responses (notifications produce none).
    // An empty input array yields one Invalid Request error, which the
    // transport should send as a single object (JSON-RPC 2.0 section 6).
    QJsonArray handleBatch(const QJsonArray& messages);

    // Protocol version this server implements ("2025-06-18").
    static QString protocolVersion();
    // Every version we accept from a client; the requested one is echoed when
    // it is in this list, otherwise protocolVersion() is answered.
    static QStringList supportedProtocolVersions();

    // When false, destructive tools (and run_batch steps that use them)
    // return NOT_ALLOWED. Default true.
    void setAllowDestructive(bool allow);
    bool allowDestructive() const { return allowDestructive_; }

    // JSON-RPC error codes used by the gateway and the transports.
    enum ErrorCode {
        ParseError = -32700,
        InvalidRequest = -32600,
        MethodNotFound = -32601,
        InvalidParams = -32602,
        InternalError = -32603,
        Unauthenticated = -32001,
    };

    static QJsonObject makeError(const QJsonValue& id, int code, const QString& message,
                                 const QJsonValue& data = QJsonValue(QJsonValue::Undefined));
    static QJsonObject makeResult(const QJsonValue& id, const QJsonObject& result);

signals:
    void toolCalled(const QString& name, bool isError, qint64 elapsedMs);

private:
    QJsonObject handleInitialize(const QJsonObject& params) const;
    QJsonObject handleToolsList() const;
    QJsonObject handleToolsCall(const QJsonObject& params);
    bool toolIsDestructive(const QString& name) const;

    AiToolExecutor& executor_;
    bool allowDestructive_ = true;
    quint64 callCounter_ = 0;
};

} // namespace OpenDaw
