#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/platform/Socket.h"

#include <mkcontract/Live.h>

#include <functional>
#include <string>
#include <vector>

// Live connection (contract between tools, docs/CODING_MODEL.md "Tools talking to a running editor/game").
// Dev build game exes and the editor open a small HTTP server on 127.0.0.1, and MealKit tools (separate repos) attach to change values and read state.
//   connection info   <project>/build/live/<kind>-<pid>.json  {"protocol","kind","pid","port","token","project","engine","url"}
//   GET  /<token>/api/hello                  this server: contract version, kind, project, engine version, services/topics list
//   GET  /<token>/api/events?topics=a,b      status stream (Server-Sent Events: "event: <topic>" + "data: <JSON>")
//   *    /<token>/<service>/...              features added with addService (the editor's Penpot import is penpot/)
// Security: opened only on 127.0.0.1 with a random per-run token in the path (only in the connection info file). Browser Origins are
// accepted only from this server itself and origins a service allows (so web pages can't call the local server).
// Requests are handled in update() on the main thread: services modify engine state without locks. Shipping builds (MEALKIT_LIVE=0) don't open it.
namespace mealkit::live {

inline constexpr u32 kProtocol = mkcontract::live::kProtocol; // contract version (protocol in tool manifests), owned by MealKit-Contract
inline constexpr u16 kEditorPort = 27220; // editor: 10 ports from this one (so the Penpot plugin address uses the same port each run), games any port
inline constexpr const char* kInfoFolder = mkcontract::live::kInfoFolder; // "build/live"

struct Request {
    std::string method;
    std::string path;  // after the service name (/<token>/penpot/api/project → "api/project"), up to ?
    std::string query; // after ?
    std::string origin;
    std::string body;
};

struct Response {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
};

struct Service {
    std::string name; // path /<token>/<name>/...
    // Whether to accept a browser Origin (this server's own origin is always accepted). If empty, requests with an Origin are rejected
    std::function<bool(const std::string& origin)> allowOrigin;
    std::function<Response(const Request&)> handle;
};

struct ServerConfig {
    std::string kind;       // "editor", "game"
    std::string projectDir; // project folder to write the connection info file to (not written if empty)
    std::string projectName;
    u16 port = 0;      // 0: any port. Otherwise tries 10 from port, then any port
    std::string token; // random if empty (automated tests set it)
};

class Server {
public:
    Server() = default;
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(const ServerConfig& config);
    void stop();
    bool running() const { return m_listener.isOpen(); }
    u16 port() const { return m_listener.localPort(); }
    const std::string& token() const { return m_token; }
    std::string origin() const;  // http://127.0.0.1:<port>
    std::string baseUrl() const; // http://127.0.0.1:<port>/<token>/
    const std::string& infoPath() const { return m_infoPath; }

    void addService(Service service);
    void addTopic(const char* topic); // topic this server sends (topics in hello, visible even with no listeners)
    // Status stream: sends to connections listening to that topic. If building the JSON is expensive, check listening() first
    void publish(const char* topic, const std::string& json);
    bool listening(const char* topic) const;

    void update(); // every frame: accept connections, read requests, send responses/streams (non-blocking)

    // One request (public so it can be tested without sockets). fullPath is the whole /<token>/...
    Response handle(const std::string& method, const std::string& fullPath, const std::string& origin, const std::string& body,
                    std::string* allowOrigin = nullptr);

private:
    struct Connection {
        net::TcpStream stream;
        std::string in, out;
        size_t sent = 0;
        bool responded = false;
        bool streaming = false; // api/events: kept open after the response to send publishes
        std::vector<std::string> topics;
        u32 idleFrames = 0;
    };
    bool originAllowed(const std::string& origin, const Service* service) const;
    void writeInfo();

    ServerConfig m_config;
    std::string m_token, m_infoPath;
    net::TcpListener m_listener;
    std::vector<Connection> m_connections;
    std::vector<Service> m_services;
    std::vector<std::string> m_topics; // topics from addTopic/publish (hello)
};

// Small JSON helpers (building responses; mkcontract's, as engine functions so mealkit.dll exports them)
std::string jsonString(const std::string& s);
std::string jsonList(const std::vector<std::string>& items);

} // namespace mealkit::live
