#include "NetworkingUtils.h"
#include "StringUtils.h"
#include <vector>
#include <cstdio>
#include <steam/isteamnetworkingutils.h>

#ifdef _WIN32
    #include <WinSock2.h>
    #include <ws2tcpip.h>
    // Link with Ws2_32.lib if not already handled by CMake
    #pragma comment(lib, "Ws2_32.lib")
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>

    // Define helpers for POSIX to match Windows logic
    typedef int SOCKET;
#define INVALID_SOCKET -1
#endif

namespace Safira::Utils {
    bool IsValidIPAddress(std::string_view ipAddress) {
        std::string ipAddressStr(ipAddress.data(), ipAddress.size());

        SteamNetworkingIPAddr address;
        return address.ParseString(ipAddressStr.c_str());
    }

    std::string ResolveDomainName(std::string_view name) {
        // 1. Handle Windows-specific Initialization
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        {
            printf("WSAStartup failed\n");
            return {};
        }
#endif

        // 2. Parse Port
        bool hasPort = name.find(":") != std::string::npos;
        std::string domain, port;
        if (hasPort) {
            std::vector<std::string> domainAndPort = SplitString(name, ':');
            if (domainAndPort.size() != 2) {
#ifdef _WIN32
                WSACleanup();
#endif
                return {};
            }
            domain = domainAndPort[0];
            port = domainAndPort[1];
        }
        else {
            domain = std::string(name);
        }

        // 3. Resolve DNS
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;    // Support both IPv4 and IPv6
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        struct addrinfo* addressResult = nullptr;
        // Note: .data() on string_view is fine here as we aren't modifying it,
        // but getaddrinfo expects a null-terminated string.
        // Since 'domain' is a std::string, use .c_str() for safety.
        int status = getaddrinfo(domain.c_str(), nullptr, &hints, &addressResult);

        if (status != 0) {
            printf("getaddrinfo failed: %s\n", gai_strerror(status));
#ifdef _WIN32
            WSACleanup();
#endif
            return {};
        }

        // 4. Extract IP String (Cross-platform way)
        std::string ipAddressStr;
        char ipString[INET6_ADDRSTRLEN]; // Large enough for IPv4 and IPv6

        for (struct addrinfo* ptr = addressResult; ptr != nullptr; ptr = ptr->ai_next) {
            void* addr;
            if (ptr->ai_family == AF_INET) { // IPv4
                struct sockaddr_in* ipv4 = (struct sockaddr_in*)ptr->ai_addr;
                addr = &(ipv4->sin_addr);
            } else { // IPv6
                struct sockaddr_in6* ipv6 = (struct sockaddr_in6*)ptr->ai_addr;
                addr = &(ipv6->sin6_addr);
            }

            // inet_ntop is the modern, cross-platform way to convert binary to string
            if (inet_ntop(ptr->ai_family, addr, ipString, sizeof(ipString))) {
                ipAddressStr = ipString;
                // Prefer IPv4 for compatibility, or just break at the first result
                if (ptr->ai_family == AF_INET) break;
            }
        }

        // 5. Cleanup
        if (addressResult) freeaddrinfo(addressResult);
#ifdef _WIN32
        WSACleanup();
#endif

        if (ipAddressStr.empty()) return {};

        return hasPort ? (ipAddressStr + ":" + port) : ipAddressStr;
    }
}