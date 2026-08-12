#include "HttpServer.h"
#include "TlsContext.h"
#include "TlsConnection.h"
#include "../config/ServerConfig.h"
#include "../../common/Logger.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <string_view>

HttpServer::HttpServer() {
#ifdef _WIN32
    WSADATA wsa;
    m_wsaStarted = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
}

HttpServer::~HttpServer() {
    stop();
#ifdef _WIN32
    if (m_wsaStarted) WSACleanup();
#endif
}

bool HttpServer::start(uint16_t port, bool useTls) {
    m_useTls = useTls;

    if (m_useTls) {
        m_tlsCtx = std::make_unique<TlsContext>();
        if (!m_tlsCtx->init()) {
            Logger::warn("Server", "TLS init failed, falling back to HTTP");
            m_useTls = false;
            m_tlsCtx.reset();
        }
    }

    m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSock == INVALID_SOCK) return false;

    int opt = 1;
#ifdef _WIN32
    setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // bind 127.0.0.1 only

    if (bind(m_listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCK_ERR) {
        sock_close(m_listenSock);
        m_listenSock = INVALID_SOCK;
        return false;
    }
    if (listen(m_listenSock, SOMAXCONN) == SOCK_ERR) {
        sock_close(m_listenSock);
        m_listenSock = INVALID_SOCK;
        return false;
    }
    m_running = true;
    m_acceptThread = std::thread(&HttpServer::acceptLoop, this);

    Logger::info("Server", std::string("[BallsHackProService] listening on port ") + std::to_string(port)
              + (m_useTls ? " (TLS 1.2+)" : " (HTTP)"));
    return true;
}

void HttpServer::stop() {
    m_running = false;
    if (m_listenSock != INVALID_SOCK) {
        sock_close(m_listenSock);
        m_listenSock = INVALID_SOCK;
    }
    if (m_acceptThread.joinable())
        m_acceptThread.join();
}

void HttpServer::route(const std::string &method, const std::string &path, Handler h) {
    m_routes.push_back({method, path, std::move(h), false, nullptr});
}

void HttpServer::routeStream(const std::string &method, const std::string &path, StreamHandler h) {
    m_routes.push_back({method, path, nullptr, true, std::move(h)});
}

// ── Accept loop: 无上限并发 ─────────────────────────────
void HttpServer::acceptLoop() {
    while (m_running) {
        sockaddr_in clientAddr{};
#ifdef _WIN32
        int addrLen = sizeof(clientAddr);
#else
        socklen_t addrLen = sizeof(clientAddr);
#endif
        socket_t client = accept(m_listenSock, (sockaddr*)&clientAddr, &addrLen);
        if (client == INVALID_SOCK) continue;

        // 无上限: 每个连接直接起独立线程
        std::thread([this, client]() {
            handleClient(client);
        }).detach();
    }
}

// ── TLS handshake ──────────────────────────────────────
TlsConnection* HttpServer::tlsHandshake(socket_t sock) {
    if (!m_useTls || !m_tlsCtx) return nullptr;

#ifdef _WIN32
    DWORD tv = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    auto* tls = new TlsConnection(*m_tlsCtx, (int)sock);
    if (!tls->valid()) {
        delete tls;
        return nullptr;
    }

    if (!tls->accept()) {
        delete tls;
        return nullptr;
    }

    return tls;
}

// ── Read HTTP request ──────────────────────────────────
std::string HttpServer::readRequest(socket_t sock, TlsConnection *tls) {
    char buf[ServerConfig::BUF_SIZE];
    std::string raw;

    while (true) {
        int n;
        if (tls) {
            n = tls->read(buf, sizeof(buf) - 1);
        } else {
#ifdef _WIN32
            n = recv(sock, buf, sizeof(buf) - 1, 0);
#else
            n = recv(sock, buf, sizeof(buf) - 1, 0);
#endif
        }

        if (n <= 0) break;
        raw.append(buf, n);

        size_t hdrEnd = raw.find("\r\n\r\n");
        if (hdrEnd != std::string::npos) {
            size_t clPos = raw.find("Content-Length:");
            if (clPos != std::string::npos) {
                size_t clEnd = raw.find("\r\n", clPos);
                size_t cl = 0;
                try {
                    cl = std::stoull(raw.substr(clPos + 15, clEnd - clPos - 15));
                } catch (...) { cl = 0; }
                static constexpr size_t MAX_BODY = 10 * 1024 * 1024;
                if (cl > MAX_BODY) break;
                if (raw.size() - (hdrEnd + 4) < cl) continue;
            }
            break;
        }
    }
    return raw;
}

// ── Send response ──────────────────────────────────────
void HttpServer::sendResponse(socket_t sock, TlsConnection *tls,
                               int status, const std::string &body,
                               const std::string &redirectUrl) {
    bool isHtml = (body.find("<!DOCTYPE") == 0 || body.find("<!doctype") == 0 || body.find("<html") == 0);
    bool isBin  = (body.compare(0, 4, "BIN:") == 0);
    std::ostringstream ss;

    if (status == 302) {
        ss << "HTTP/1.1 302 Found\r\n"
           << "Location: " << redirectUrl << "\r\n"
           << "Content-Length: 0\r\n"
           << "Connection: close\r\n\r\n";
    } else {
        ss << "HTTP/1.1 " << status << " "
           << (status == 200 ? "OK" : status == 204 ? "No Content"
              : status == 422 ? "Unprocessable Entity" : "Not Found")
           << "\r\n"
           << "Content-Type: " << (isBin ? "application/octet-stream" : isHtml ? "text/html" : "application/json") << "\r\n"
           << "Content-Length: " << (isBin ? body.size() - 4 : body.size()) << "\r\n"
           << "X-Content-Type-Options: nosniff\r\n"
           << "X-Frame-Options: DENY\r\n"
           << "Strict-Transport-Security: max-age=31536000\r\n"
           << "Access-Control-Allow-Origin: *\r\n"
           << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
           << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
           << "Connection: close\r\n"
           << "\r\n"
           << (isBin ? body.substr(4) : body);
    }

    std::string response = ss.str();
    if (tls) {
        tls->write(response.data(), (int)response.size());
    } else {
#ifdef _WIN32
        send(sock, response.data(), (int)response.size(), 0);
#else
        send(sock, response.data(), response.size(), 0);
#endif
    }
}

void HttpServer::sendStreamHeader(socket_t sock, TlsConnection *tls,
                                   const std::string &contentType, size_t fileSize) {
    std::ostringstream ss;
    ss << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: " << contentType << "\r\n"
       << "Content-Length: " << fileSize << "\r\n"
       << "Connection: close\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Strict-Transport-Security: max-age=31536000\r\n"
       << "\r\n";
    std::string header = ss.str();
    if (tls) {
        tls->write(header.data(), (int)header.size());
    } else {
#ifdef _WIN32
        send(sock, header.data(), (int)header.size(), 0);
#else
        send(sock, header.data(), header.size(), 0);
#endif
    }
}

// ── Per-client handler ─────────────────────────────────
void HttpServer::handleClient(socket_t sock) {
    auto t0 = std::chrono::steady_clock::now();

    TlsConnection *tls = tlsHandshake(sock);
    if (m_useTls && m_tlsCtx && !tls) { sock_close(sock); return; }

    std::string raw = readRequest(sock, tls);
    Parsed req = parseRequest(raw);

    // 请求日志
    auto logReq = [&](int status) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::cout << "[" << req.method << " " << req.path << "] " << status
                  << " " << ms << "ms" << std::endl;
    };

    if (req.method == "OPTIONS") {
        sendResponse(sock, tls, 204, "", "");
        logReq(204);
        delete tls; sock_close(sock); return;
    }
    if (req.method.empty()) {
        sendResponse(sock, tls, 400, "{\"success\":false,\"message\":\"Bad Request\"}", "");
        logReq(400);
        delete tls; sock_close(sock); return;
    }

    // Route matching — check stream handlers first
    for (auto &r : m_routes) {
        if (r.method != req.method || r.path != req.path) continue;

        if (r.isStream) {
            try {
                r.streamHandler(sock, tls, req.query);
                logReq(200);
            } catch (...) {
                sendResponse(sock, tls, 500,
                    "{\"success\":false,\"message\":\"Internal Server Error\"}", "");
                logReq(500);
            }
            delete tls;
            sock_close(sock);
            return;
        }

        std::string respBody;
        int status = 200;
        std::string redirectUrl;
        try {
            respBody = r.handler(req.body, req.query, req.headers);
        } catch (const std::exception &e) {
            respBody = "{\"success\":false,\"message\":\"Internal Server Error\"}";
            status = 500;
            std::cerr << "[ERROR] " << r.method << " " << r.path << ": " << e.what() << std::endl;
        } catch (...) {
            respBody = "{\"success\":false,\"message\":\"Internal Server Error\"}";
            status = 500;
            std::cerr << "[ERROR] " << r.method << " " << r.path << ": unknown exception" << std::endl;
        }
        if (respBody.rfind("REDIRECT:", 0) == 0) {
            redirectUrl = respBody.substr(9);
            status = 302;
        } else if (respBody.find("\"success\":false") != std::string::npos)
            status = 422;
        sendResponse(sock, tls, status, respBody, redirectUrl);
        logReq(status);
        delete tls;

        // Graceful shutdown
#ifdef _WIN32
        shutdown(sock, SD_SEND);
        DWORD tv = 2000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        char discard[256];
        while (recv(sock, discard, sizeof(discard), 0) > 0) {}
#else
        shutdown(sock, SHUT_WR);
        struct timeval tv;
        tv.tv_sec = 2; tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char discard[256];
        while (recv(sock, discard, sizeof(discard), 0) > 0) {}
#endif
        sock_close(sock);
        return;
    }

    // 404
    sendResponse(sock, tls, 404, "{\"success\":false,\"message\":\"not found\"}", "");
    logReq(404);
    delete tls;
    sock_close(sock);
}

HttpServer::Parsed HttpServer::parseRequest(std::string_view raw) const {
    Parsed p;
    if (raw.empty()) return p;

    auto nl = raw.find('\n');
    if (nl == std::string_view::npos) return p;
    std::string_view reqLine = raw.substr(0, nl);
    auto sp1 = reqLine.find(' ');
    auto sp2 = reqLine.rfind(' ');
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) return p;
    p.method = reqLine.substr(0, sp1);
    std::string fullPath(reqLine.substr(sp1 + 1, sp2 - sp1 - 1));

    auto qm = fullPath.find('?');
    if (qm != std::string::npos) {
        p.path  = fullPath.substr(0, qm);
        p.query = fullPath.substr(qm + 1);
    } else {
        p.path = fullPath;
    }

    auto bodySep = raw.find("\r\n\r\n");
    if (bodySep != std::string_view::npos) {
        auto hdrStart = raw.find("\r\n");
        if (hdrStart != std::string_view::npos)
            p.headers = raw.substr(hdrStart + 2, bodySep - hdrStart - 2);
        p.body = raw.substr(bodySep + 4);
    }

    return p;
}
