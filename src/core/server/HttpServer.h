#pragma once
// ── Cross-platform HTTP server ───────────────────────
// Supports Windows (Winsock2) and Linux (POSIX sockets).

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using socket_t = SOCKET;
    constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
    #define SOCK_ERR SOCKET_ERROR
    inline void sock_close(socket_t s) { closesocket(s); }
    inline int  sock_last_err()        { return WSAGetLastError(); }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
    #include <fcntl.h>
    using socket_t = int;
    constexpr socket_t INVALID_SOCK = -1;
    constexpr int     SOCK_ERR      = -1;
    inline void sock_close(socket_t s) { close(s); }
    inline int  sock_last_err()        { return errno; }
#endif

#include <string>
#include <functional>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>

class TlsContext;
class TlsConnection;

class HttpServer {
public:
    // ── 普通 handler: 返回响应体字符串 ──
    using Handler = std::function<std::string(const std::string &body,
                                              const std::string &query,
                                              const std::string &headers)>;

    // ── 流式 handler: 直接操作 socket, 返回 body 大小 (用于大文件)
    using StreamHandler = std::function<void(socket_t sock, TlsConnection *tls,
                                             const std::string &query)>;

    HttpServer();
    ~HttpServer();

    bool start(uint16_t port, bool useTls = true);
    void stop();

    void route(const std::string &method, const std::string &path, Handler h);
    void routeStream(const std::string &method, const std::string &path, StreamHandler h);

private:
    void acceptLoop();
    void handleClient(socket_t clientSock);

    TlsConnection* tlsHandshake(socket_t sock);
    std::string    readRequest(socket_t sock, TlsConnection *tls);
    void           sendResponse(socket_t sock, TlsConnection *tls,
                                int status, const std::string &body,
                                const std::string &redirectUrl = "");
    void           sendStreamHeader(socket_t sock, TlsConnection *tls,
                                    const std::string &contentType, size_t fileSize);

    struct Parsed {
        std::string method;
        std::string path;
        std::string query;
        std::string headers;
        std::string body;
    };
    Parsed parseRequest(std::string_view raw) const;

    socket_t m_listenSock = INVALID_SOCK;
    std::atomic<bool> m_running{false};
    std::thread m_acceptThread;
    bool m_useTls = false;

    std::unique_ptr<TlsContext> m_tlsCtx;

    struct Route { std::string method, path; Handler handler; bool isStream = false; StreamHandler streamHandler; };
    std::vector<Route> m_routes;

#ifdef _WIN32
    bool m_wsaStarted = false;
#endif
};
