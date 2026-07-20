#include "network_utils.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace opennow {

std::string NetworkUtils::GetLocalIPAddress() {
    std::string local_ip = "127.0.0.1";

#ifdef _WIN32
    // Windows implementation (fallback if compiled for PC)
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock != -1) {
        struct sockaddr_in serv;
        serv.sin_family = AF_INET;
        serv.sin_addr.s_addr = inet_addr("8.8.8.8");
        serv.sin_port = htons(53);

        if (connect(sock, (const struct sockaddr*)&serv, sizeof(serv)) == 0) {
            struct sockaddr_in name;
            socklen_t namelen = sizeof(name);
            if (getsockname(sock, (struct sockaddr*)&name, &namelen) == 0) {
                local_ip = inet_ntoa(name.sin_addr);
            }
        }
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
    }

#ifdef _WIN32
    WSACleanup();
#endif

    return local_ip;
}

bool NetworkUtils::HasInternetConnection() {
#ifdef __SWITCH__
    NifmInternetConnectionType type {};
    NifmInternetConnectionStatus status {};
    u32 strength = 0;
    const Result rc = nifmGetInternetConnectionStatus(&type, &strength, &status);
    if (R_FAILED(rc))
        return true; // Unknown service state must not terminate a healthy stream.
    return status == NifmInternetConnectionStatus_Connected;
#else
    return true;
#endif
}

} // namespace opennow
