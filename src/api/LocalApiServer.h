#pragma once

// Local IPC endpoint for external agents (MCP).
//
// Listens on a QLocalServer (a named pipe on Windows, a Unix domain socket in
// QDir::tempPath() elsewhere), restricted to the current user. Every
// connection speaks newline-delimited JSON-RPC 2.0: one UTF-8 JSON object
// (or batch array) per line, which is exactly the MCP stdio framing, so the
// opendaw-mcp bridge only has to relay lines.
//
// Security: a random token is generated per run and written, together with
// the server name, to discoveryFilePath() (a per-user file). When a token is
// required, the first message on a connection must be
//   {"jsonrpc":"2.0","id":0,"method":"opendaw/authenticate","params":{"token":"..."}}
// Anything else gets JSON-RPC error -32001 and the connection is closed.
//
// Threading: GUI thread only. Messages are handled one at a time in arrival
// order; a long tool call (a render) blocks until it finishes and lines that
// arrive meanwhile are queued.
//
// Nested event loops: queued lines are never executed while the UI is busy
// (isUiBusy(): a nested event loop such as QDialog::exec / QMenu::exec, a
// modal window, or the installed busy predicate), because UI code on the
// stack may hold raw pointers into the Edit. They stay queued and a 50 ms
// single-shot timer retries until the UI is idle again.
//
// Authentication is the exception: the first line of an unauthenticated
// connection is handled as soon as it arrives (even during a long tool call
// or a modal dialog), since it touches no project state.

#include <QByteArray>
#include <QHash>
#include <QJsonDocument>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QLocalServer;
class QTimer;
class QLocalSocket;

namespace OpenDaw {

class CommandGateway;

class LocalApiServer : public QObject {
    Q_OBJECT
public:
    explicit LocalApiServer(CommandGateway& gateway, QObject* parent = nullptr);
    ~LocalApiServer() override;

    // Starts listening. If another live OpenDaw already owns the name, listens
    // on "<name>-<pid>" instead. Writes the discovery file. False on failure.
    bool start(const QString& serverName = defaultServerName());
    // Closes every connection, stops listening and removes the discovery file
    // (only if it still describes this process).
    void stop();

    bool isListening() const;
    int clientCount() const;
    QString serverName() const;      // the name actually listened on
    QString fullServerName() const;  // pipe path (Windows) or socket file path (Unix)
    QString token() const;           // 64 hex characters; new each start()

    void setRequireToken(bool require); // default true
    bool requireToken() const { return requireToken_; }

    // Extra "the UI must not be re-entered now" check, OR-ed with the built-in
    // ones (nested event loop, QGuiApplication::modalWindow()). The GUI app
    // can install e.g. [] { return QApplication::activePopupWidget(); }.
    void setUiBusyPredicate(std::function<bool()> predicate);
    // True while queued agent lines must not run (see the header comment).
    bool isUiBusy() const;
    // Lines received but not yet handled (tests / diagnostics).
    int queuedLineCount() const { return int(queue_.size()); }

    static QString defaultServerName();   // "opendaw-agent-api"
    // <GenericConfigLocation>/OpenDaw/agent-api.json, i.e.
    //   Windows: %LOCALAPPDATA%\OpenDaw\agent-api.json
    //   macOS:   ~/Library/Preferences/OpenDaw/agent-api.json
    //   Linux:   $XDG_CONFIG_HOME (or ~/.config)/OpenDaw/agent-api.json
    // The environment variable OPENDAW_AGENT_API_FILE overrides it (the
    // bridge and the Python client honour the same variable).
    static QString discoveryFilePath();

    static constexpr qint64 kMaxLineBytes = 16 * 1024 * 1024;
    static constexpr int kAuthTimeoutMs = 15000;
    static constexpr int kDeferRetryMs = 50;

signals:
    void clientCountChanged(int count);
    void started(const QString& name);

private:
    struct Client {
        QPointer<QLocalSocket> socket;
        QByteArray buffer;
        bool authenticated = false;
        bool closing = false;
    };
    struct PendingLine {
        QPointer<QLocalSocket> socket;
        QByteArray line;
    };

    void onNewConnection();
    void onReadyRead(QLocalSocket* socket);
    void onDisconnected(QLocalSocket* socket);
    void processQueue();
    void processLine(QLocalSocket* socket, const QByteArray& line);
    // The first line of an unauthenticated connection. True when the client
    // is now authenticated; false when it was rejected (and is closing).
    bool handleAuthLine(QLocalSocket* socket, const QByteArray& line);
    void scheduleRetry();
    void send(QLocalSocket* socket, const QJsonDocument& doc);
    void rejectAndClose(QLocalSocket* socket, const QJsonValue& id, int code, const QString& message);

    bool listenOn(const QString& name);
    static bool isServerAlive(const QString& name);
    bool writeDiscoveryFile() const;
    void removeDiscoveryFile();
    static QString generateToken();
    bool tokenMatches(const QString& candidate) const;

    CommandGateway& gateway_;
    QLocalServer* server_ = nullptr;
    QHash<QLocalSocket*, Client> clients_;
    QList<PendingLine> queue_;
    bool processing_ = false;
    bool deferring_ = false;     // logged once per busy period
    QTimer* retryTimer_ = nullptr;
    std::function<bool()> uiBusyPredicate_;
    bool requireToken_ = true;
    QString token_;
    // The discovery file of another live instance that start() replaced
    // (when this one had to use "<name>-<pid>"); stop() puts it back.
    QByteArray previousDiscovery_;
};

} // namespace OpenDaw
