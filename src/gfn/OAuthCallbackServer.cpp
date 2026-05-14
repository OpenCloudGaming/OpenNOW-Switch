#include "opennow/gfn/OAuthCallbackServer.hpp"

#include "opennow/gfn/Auth.hpp"

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace opennow::gfn {

namespace {

constexpr std::uint32_t kLoopbackIpv4 = 0x7f000001U;

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void closeSocket(SocketHandle socket) { closesocket(socket); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void closeSocket(SocketHandle socket) { close(socket); }
#endif

std::string urlDecode(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < value.size()) {
            const auto hex = std::string(value.substr(i + 1, 2));
            char* end = nullptr;
            const auto decoded = std::strtol(hex.c_str(), &end, 16);
            if (end && *end == '\0') {
                out.push_back(static_cast<char>(decoded));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string queryParam(std::string_view target, std::string_view key) {
    const auto question = target.find('?');
    if (question == std::string_view::npos) {
        return "";
    }
    auto query = target.substr(question + 1);
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto pair = query.substr(0, amp);
        const auto eq = pair.find('=');
        const auto rawKey = eq == std::string_view::npos ? pair : pair.substr(0, eq);
        const auto rawValue = eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1);
        if (urlDecode(rawKey) == key) {
            return urlDecode(rawValue);
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query = query.substr(amp + 1);
    }
    return "";
}

}

OAuthCallbackResult OAuthCallbackServer::waitForCode(std::uint16_t port, std::uint32_t timeoutMs) const {
#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return {false, {}, "WSAStartup failed"};
    }
#endif

    const auto server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server == kInvalidSocket) {
#if defined(_WIN32)
        WSACleanup();
#endif
        return {false, {}, "socket creation failed"};
    }

    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(kLoopbackIpv4);

    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(server, 1) != 0) {
        closeSocket(server);
#if defined(_WIN32)
        WSACleanup();
#endif
        return {false, {}, "failed to bind OAuth callback port"};
    }

    fd_set set;
    FD_ZERO(&set);
    FD_SET(server, &set);
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(timeoutMs / 1000);
    timeout.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);

    const auto ready = select(static_cast<int>(server + 1), &set, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        closeSocket(server);
#if defined(_WIN32)
        WSACleanup();
#endif
        return {false, {}, "timed out waiting for OAuth callback"};
    }

    const auto client = accept(server, nullptr, nullptr);
    closeSocket(server);
    if (client == kInvalidSocket) {
#if defined(_WIN32)
        WSACleanup();
#endif
        return {false, {}, "failed to accept OAuth callback"};
    }

    char buffer[2048] = {};
    const auto received = recv(client, buffer, sizeof(buffer) - 1, 0);
    std::string request = received > 0 ? std::string(buffer, static_cast<std::size_t>(received)) : "";
    const std::string response =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
        "<!doctype html><title>OpenNOW Login</title><body>Login complete. Return to OpenNOW Switch.</body>";
    send(client, response.c_str(), static_cast<int>(response.size()), 0);
    closeSocket(client);
#if defined(_WIN32)
    WSACleanup();
#endif

    const auto firstSpace = request.find(' ');
    const auto secondSpace = firstSpace == std::string::npos ? std::string::npos : request.find(' ', firstSpace + 1);
    if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
        return {false, {}, "invalid OAuth callback request"};
    }

    return parseRequestTarget(request.substr(firstSpace + 1, secondSpace - firstSpace - 1));
}

OAuthCallbackResult OAuthCallbackServer::parseRequestTarget(const std::string& requestTarget) {
    const auto code = queryParam(requestTarget, "code");
    if (!code.empty()) {
        return {true, code, {}};
    }

    auto error = queryParam(requestTarget, "error");
    if (error.empty()) {
        error = "authorization callback did not include code";
    }
    return {false, {}, error};
}

} // namespace opennow::gfn
