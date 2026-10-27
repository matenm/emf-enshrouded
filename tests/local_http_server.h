#pragma once
// A tiny HTTP server on 127.0.0.1 for the WinHTTP tests (no TLS: the requests use allow_loopback_http, which only tests
// set). Each connection's request line is recorded; `respond` gives the raw reply for the n-th connection; with
// `stall` the connection stays open after the reply until the server stops (a transfer that hangs).
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace http_test {

class LocalHttpServer {
public:
    std::function<std::string(int index, const std::string& request)> respond;
    bool stall = false;

    LocalHttpServer() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        bind(listener_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        listen(listener_, 8);
        int len = sizeof a;
        getsockname(listener_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
    }
    ~LocalHttpServer() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        closesocket(listener_);
        WSACleanup();
    }
    void start() { thread_ = std::thread([this] { run(); }); }
    int port() const { return port_; }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    std::vector<std::string> requests() {
        std::lock_guard lock(m_);
        return requests_;
    }

private:
    void run() {
        int index = 0;
        std::vector<SOCKET> open;
        while (!stop_) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(listener_, &fds);
            timeval tv{0, 50000};
            if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET c = accept(listener_, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            std::string req;
            char buf[4096];
            while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536) {
                fd_set r;
                FD_ZERO(&r);
                FD_SET(c, &r);
                timeval t2{2, 0};
                if (select(0, &r, nullptr, nullptr, &t2) <= 0) break;
                const int n = recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                req.append(buf, n);
            }
            {
                std::lock_guard lock(m_);
                requests_.push_back(req.substr(0, req.find("\r\n")));
            }
            const std::string reply = respond ? respond(index++, req) : std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
            send(c, reply.data(), static_cast<int>(reply.size()), 0);
            if (stall) open.push_back(c);  // never finishes: closed when the server stops
            else {
                shutdown(c, SD_SEND);
                closesocket(c);
            }
        }
        for (SOCKET c : open) closesocket(c);
    }

    SOCKET listener_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex m_;
    std::vector<std::string> requests_;
};

}  // namespace http_test
