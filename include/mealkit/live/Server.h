#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/net/Socket.h"

#include <functional>
#include <string>
#include <vector>

// 라이브 연결 (도구 사이 약속, docs/CODING_MODEL.md "도구가 실행 중인 에디터·게임과 이야기하기").
// 개발 빌드 게임 exe와 에디터가 127.0.0.1에 작은 HTTP 서버를 열고, MealKit 도구(별도 저장소)가 붙어 값을 바꾸고 상태를 읽는다.
//   접속 정보   <프로젝트>/build/live/<종류>-<pid>.json  {"protocol","kind","pid","port","token","project","engine","url"}
//   GET  /<토큰>/api/hello                  이 서버: 약속 버전, 종류, 프로젝트, 엔진 버전, 서비스·주제 목록
//   GET  /<토큰>/api/events?topics=a,b      상태 스트림 (Server-Sent Events: "event: <주제>" + "data: <JSON>")
//   *    /<토큰>/<서비스>/...                addService로 더한 기능 (에디터의 Penpot 가져오기는 penpot/)
// 보안: 127.0.0.1에만 열고 실행마다 무작위 토큰을 경로에 넣는다(접속 정보 파일에만 있다). 브라우저 Origin은
// 이 서버 자신과, 서비스가 허용한 출처만 받는다(웹 페이지가 로컬 서버를 부르지 못하게).
// 요청은 메인 스레드의 update()에서 처리한다: 서비스가 엔진 상태를 잠금 없이 고친다. 출시 빌드(MEALKIT_LIVE=0)는 열지 않는다.
namespace mealkit::live {

inline constexpr u32 kProtocol = 1;       // 약속 버전 (도구 매니페스트의 protocol). 경로·응답 모양을 바꾸면 올린다
inline constexpr u16 kEditorPort = 27220; // 에디터: 이 포트부터 10개 (Penpot 플러그인 주소가 실행마다 같은 포트), 게임은 아무 포트
inline constexpr const char* kInfoFolder = "build/live";

struct Request {
    std::string method;
    std::string path;  // 서비스 이름 뒤 (/<토큰>/penpot/api/project → "api/project"), ? 앞까지
    std::string query; // ? 뒤
    std::string origin;
    std::string body;
};

struct Response {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
};

struct Service {
    std::string name; // 경로 /<토큰>/<이름>/...
    // 브라우저 Origin을 받을지 (이 서버 자신의 출처는 항상 받는다). 비어 있으면 Origin이 있는 요청은 거절
    std::function<bool(const std::string& origin)> allowOrigin;
    std::function<Response(const Request&)> handle;
};

struct ServerConfig {
    std::string kind;       // "editor", "game"
    std::string projectDir; // 접속 정보 파일을 쓸 프로젝트 폴더 (비면 쓰지 않는다)
    std::string projectName;
    u16 port = 0;      // 0: 아무 포트. 아니면 port부터 10개를 해 보고 없으면 아무 포트
    std::string token; // 비면 무작위 (자동 시험은 정한다)
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
    std::string baseUrl() const; // http://127.0.0.1:<port>/<토큰>/
    const std::string& infoPath() const { return m_infoPath; }

    void addService(Service service);
    void addTopic(const char* topic); // 이 서버가 보내는 주제 (hello의 topics, 듣는 연결이 없어도 보인다)
    // 상태 스트림: 그 주제를 듣는 연결에 보낸다. JSON 만들기가 비싸면 먼저 listening()을 본다
    void publish(const char* topic, const std::string& json);
    bool listening(const char* topic) const;

    void update(); // 매 프레임: 연결 받기, 요청 읽기, 응답·스트림 보내기 (논블로킹)

    // 요청 하나 (소켓 없이 시험할 수 있게 공개). fullPath는 /<토큰>/... 전체
    Response handle(const std::string& method, const std::string& fullPath, const std::string& origin, const std::string& body,
                    std::string* allowOrigin = nullptr);

private:
    struct Connection {
        net::TcpStream stream;
        std::string in, out;
        size_t sent = 0;
        bool responded = false;
        bool streaming = false; // api/events: 응답 뒤에도 열어 두고 publish를 보낸다
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
    std::vector<std::string> m_topics; // addTopic·publish한 주제 (hello)
};

// 작은 JSON 도우미 (응답 만들기)
std::string jsonString(const std::string& s);
std::string jsonList(const std::vector<std::string>& items);

} // namespace mealkit::live
