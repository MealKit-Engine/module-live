#include "mealkit/live/Server.h"

#include "mealkit/core/Log.h"
#include "mealkit/core/Version.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/System.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>

namespace mealkit::live {
namespace {

constexpr size_t kMaxRequest = 64u << 20; // max request size received (including baked PNGs and sources from Penpot import)
constexpr size_t kMaxStreamBacklog = 4u << 20; // connections not reading their stream are closed when this much piles up
constexpr u32 kIdleFrames = 1800;         // request connections quiet for this many frames are closed (except streams)

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool startsWith(const std::string& s, const std::string& prefix) { return s.rfind(prefix, 0) == 0; }

const char* reasonOf(int status) {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    default: return "Bad Request";
    }
}

Response error(int status, const char* message) {
    Response r;
    r.status = status;
    r.body = std::string("{\"error\":") + jsonString(message) + "}";
    return r;
}

// "topics=a,b&x=y" → a, b
std::vector<std::string> topicsOf(const std::string& query) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t end = query.find('&', pos);
        if (end == std::string::npos) end = query.size();
        const std::string part = query.substr(pos, end - pos);
        pos = end + 1;
        if (!startsWith(part, "topics=")) continue;
        for (size_t a = 7; a <= part.size();) {
            size_t b = part.find(',', a);
            if (b == std::string::npos) b = part.size();
            if (b > a) out.push_back(part.substr(a, b - a));
            a = b + 1;
        }
    }
    return out;
}

} // namespace

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<u8>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else out += c;
        }
    }
    return out + "\"";
}

std::string jsonList(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) out += (i ? "," : "") + jsonString(items[i]);
    return out + "]";
}

Server::~Server() { stop(); }

bool Server::start(const ServerConfig& config) {
    stop();
    m_config = config;
    if (!net::startup()) return false;
    m_token = config.token;
    if (m_token.empty()) {
        std::random_device rd; // different value per run (unrelated to the simulation)
        char buf[33];
        std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
        m_token = buf;
    }
    if (config.port)
        for (u16 p = config.port; p < config.port + 10; ++p)
            if (m_listener.open(p, true)) break;
    if (!m_listener.isOpen() && !m_listener.open(0, true)) {
        MEALKIT_WARN("Live server: cannot open a local port");
        net::cleanup();
        return false;
    }
    writeInfo();
    MEALKIT_INFO("Live server (%s): %s", config.kind.c_str(), baseUrl().c_str());
    return true;
}

void Server::stop() {
    if (!m_listener.isOpen()) return;
    m_connections.clear();
    m_listener.close();
    net::cleanup();
    if (!m_infoPath.empty()) removeFile(m_infoPath);
    m_infoPath.clear();
}

std::string Server::origin() const { return running() ? "http://127.0.0.1:" + std::to_string(port()) : std::string(); }

std::string Server::baseUrl() const { return running() ? origin() + "/" + m_token + "/" : std::string(); }

void Server::writeInfo() {
    if (m_config.projectDir.empty()) return;
    const std::string dir = m_config.projectDir + "/" + kInfoFolder;
    if (!createDirectories(dir)) return;
    // Clean up files left by abnormal exits: delete if the pid in the name no longer exists
    for (const std::string& f : listFiles(dir, ".json")) {
        const size_t dash = f.rfind('-');
        const u32 pid = dash == std::string::npos ? 0 : static_cast<u32>(std::strtoul(f.c_str() + dash + 1, nullptr, 10));
        if (pid && pid != processId() && !processRunning(pid)) removeFile(dir + "/" + f);
    }
    const u32 pid = processId();
    m_infoPath = dir + "/" + m_config.kind + "-" + std::to_string(pid) + ".json";
    const std::string json = "{\"protocol\":" + std::to_string(kProtocol) + ",\"kind\":" + jsonString(m_config.kind) +
                             ",\"pid\":" + std::to_string(pid) + ",\"port\":" + std::to_string(port()) + ",\"token\":" + jsonString(m_token) +
                             ",\"project\":" + jsonString(m_config.projectName) + ",\"engine\":" + jsonString(kEngineVersion) +
                             ",\"url\":" + jsonString(baseUrl()) + "}\n";
    if (!writeTextFile(m_infoPath, json)) {
        MEALKIT_WARN("Live server: cannot write %s", m_infoPath.c_str());
        m_infoPath.clear();
    }
}

void Server::addService(Service service) {
    for (Service& s : m_services)
        if (s.name == service.name) {
            s = std::move(service);
            return;
        }
    m_services.push_back(std::move(service));
}

bool Server::listening(const char* topic) const {
    for (const Connection& c : m_connections)
        if (c.streaming && std::find(c.topics.begin(), c.topics.end(), topic) != c.topics.end()) return true;
    return false;
}

void Server::addTopic(const char* topic) {
    if (std::find(m_topics.begin(), m_topics.end(), topic) == m_topics.end()) m_topics.push_back(topic);
}

void Server::publish(const char* topic, const std::string& json) {
    addTopic(topic);
    for (Connection& c : m_connections) {
        if (!c.streaming || std::find(c.topics.begin(), c.topics.end(), topic) == c.topics.end()) continue;
        if (c.out.size() - c.sent > kMaxStreamBacklog) {
            c.stream.close(); // a tool that isn't reading
            continue;
        }
        c.out += std::string("event: ") + topic + "\ndata: " + json + "\n\n";
    }
}

bool Server::originAllowed(const std::string& origin, const Service* service) const {
    if (origin.empty()) return true; // tools (non-browser programs) and the address bar have no Origin (the token guards)
    if (lower(origin) == this->origin()) return true;
    return service && service->allowOrigin && service->allowOrigin(lower(origin));
}

Response Server::handle(const std::string& method, const std::string& fullPath, const std::string& origin, const std::string& body,
                        std::string* allowOrigin) {
    const size_t q = fullPath.find('?');
    std::string path = fullPath.substr(0, q);
    const std::string prefix = "/" + m_token + "/";
    if (m_token.empty() || !startsWith(path, prefix)) return error(404, "not found");
    path = path.substr(prefix.size());
    // Services: /<token>/<name>/...
    const Service* service = nullptr;
    for (const Service& s : m_services)
        if (path == s.name || startsWith(path, s.name + "/")) service = &s;
    if (!originAllowed(origin, service)) return error(403, "origin not allowed");
    if (allowOrigin && !origin.empty()) *allowOrigin = origin;
    if (method == "OPTIONS") { // CORS preflight
        Response r;
        r.status = 204;
        return r;
    }
    if (service) {
        Request req;
        req.method = method;
        req.path = path.size() > service->name.size() ? path.substr(service->name.size() + 1) : std::string();
        req.query = q == std::string::npos ? std::string() : fullPath.substr(q + 1);
        req.origin = origin;
        req.body = body;
        return service->handle(req);
    }
    if (method == "GET" && path == "api/hello") {
        std::vector<std::string> services;
        for (const Service& s : m_services) services.push_back(s.name);
        Response r;
        r.body = "{\"protocol\":" + std::to_string(kProtocol) + ",\"kind\":" + jsonString(m_config.kind) +
                 ",\"project\":" + jsonString(m_config.projectName) + ",\"engine\":" + jsonString(kEngineVersion) +
                 ",\"pid\":" + std::to_string(processId()) + ",\"services\":" + jsonList(services) + ",\"topics\":" + jsonList(m_topics) + "}";
        return r;
    }
    return error(404, "not found");
}

void Server::update() {
    if (!running()) return;
    for (int i = 0; i < 8; ++i) {
        Connection c;
        if (!m_listener.accept(c.stream)) break;
        m_connections.push_back(std::move(c));
    }
    char buffer[65536];
    for (Connection& c : m_connections) {
        ++c.idleFrames;
        if (!c.responded) {
            for (;;) {
                const i32 got = c.stream.receive(buffer, sizeof(buffer));
                if (got < 0) {
                    c.stream.close();
                    break;
                }
                if (got == 0) break;
                c.in.append(buffer, static_cast<size_t>(got));
                c.idleFrames = 0;
                if (c.in.size() > kMaxRequest) {
                    c.stream.close();
                    break;
                }
            }
            const size_t headerEnd = c.in.find("\r\n\r\n");
            if (c.stream.isOpen() && headerEnd != std::string::npos) {
                std::string method, path, origin;
                bool privateNetwork = false;
                size_t contentLength = 0;
                size_t lineStart = 0;
                bool firstLine = true;
                while (lineStart < headerEnd) {
                    size_t lineEnd = c.in.find("\r\n", lineStart);
                    if (lineEnd == std::string::npos || lineEnd > headerEnd) lineEnd = headerEnd;
                    const std::string line = c.in.substr(lineStart, lineEnd - lineStart);
                    lineStart = lineEnd + 2;
                    if (firstLine) {
                        firstLine = false;
                        const size_t a = line.find(' '), b = line.find(' ', a + 1);
                        if (a != std::string::npos && b != std::string::npos) {
                            method = line.substr(0, a);
                            path = line.substr(a + 1, b - a - 1);
                        }
                        continue;
                    }
                    const size_t colon = line.find(':');
                    if (colon == std::string::npos) continue;
                    const std::string name = lower(line.substr(0, colon));
                    std::string value = line.substr(colon + 1);
                    while (!value.empty() && value.front() == ' ') value.erase(0, 1);
                    if (name == "content-length") contentLength = static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
                    else if (name == "origin") origin = value;
                    else if (name == "access-control-request-private-network") privateNetwork = lower(value) == "true";
                }
                if (contentLength > kMaxRequest) {
                    c.stream.close();
                } else if (c.in.size() >= headerEnd + 4 + contentLength) {
                    const std::string body = c.in.substr(headerEnd + 4, contentLength);
                    c.in.clear();
                    c.responded = true;
                    // Status stream: send only the headers and keep the connection open
                    const std::string clean = path.substr(0, path.find('?'));
                    if (method == "GET" && clean == "/" + m_token + "/api/events" && originAllowed(origin, nullptr)) {
                        c.streaming = true;
                        c.topics = topicsOf(path.find('?') == std::string::npos ? std::string() : path.substr(path.find('?') + 1));
                        c.out = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\n";
                        if (!origin.empty()) c.out += "Access-Control-Allow-Origin: " + origin + "\r\nVary: Origin\r\n";
                        c.out += "\r\n: connected\n\n";
                    } else {
                        std::string allow;
                        const Response r = handle(method, path, origin, body, &allow);
                        std::string head = "HTTP/1.1 " + std::to_string(r.status) + " " + reasonOf(r.status) + "\r\n";
                        head += "Content-Type: " + r.contentType + "\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\n";
                        head += "Cache-Control: no-store\r\nConnection: close\r\n";
                        if (!allow.empty()) {
                            head += "Access-Control-Allow-Origin: " + allow + "\r\nVary: Origin\r\n";
                            head += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\n";
                            if (privateNetwork) head += "Access-Control-Allow-Private-Network: true\r\n"; // Chrome private network access
                        }
                        c.out = head + "\r\n" + r.body;
                    }
                }
            }
        }
        if (c.responded && c.stream.isOpen()) {
            while (c.sent < c.out.size()) {
                const u32 chunk = static_cast<u32>(std::min<size_t>(c.out.size() - c.sent, 1u << 20));
                const i32 n = c.stream.send(c.out.data() + c.sent, chunk);
                if (n < 0) {
                    c.stream.close();
                    break;
                }
                if (n == 0) break;
                c.sent += static_cast<size_t>(n);
                c.idleFrames = 0;
            }
            if (c.sent >= c.out.size()) {
                if (!c.streaming) c.stream.close();
                c.out.clear();
                c.sent = 0;
            }
            if (c.streaming && c.stream.isOpen()) { // did the tool disconnect (there's nothing to read)
                const i32 got = c.stream.receive(buffer, sizeof(buffer));
                if (got < 0) c.stream.close();
            }
        }
        if (!c.streaming && c.idleFrames > kIdleFrames) c.stream.close();
    }
    m_connections.erase(std::remove_if(m_connections.begin(), m_connections.end(), [](const Connection& c) { return !c.stream.isOpen(); }),
                        m_connections.end());
}

} // namespace mealkit::live
