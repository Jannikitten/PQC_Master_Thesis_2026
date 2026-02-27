#include "NetworkingUtils.h"
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#include <netdb.h>
#include <arpa/inet.h>

namespace Safira::Utils {

    bool IsValidIPAddress(std::string_view ipAddress) {
        SteamNetworkingIPAddr addr;
        // Steam's ParseString is cross-platform and handles "127.0.0.1:8192"
        return addr.ParseString(std::string(ipAddress).c_str());
    }

    std::string ResolveDomainName(std::string_view name) {
        SteamNetworkingIPAddr addr;
        std::string input(name);

        // 1. If it's already an IP (like 127.0.0.1), don't do DNS lookup
        if (addr.ParseString(input.c_str())) {
            return input;
        }

        // 2. DNS Resolution for macOS
        size_t colonPos = name.find(':');
        std::string domain = std::string(name.substr(0, colonPos));
        std::string portStr = (colonPos != std::string::npos) ? std::string(name.substr(colonPos + 1)) : "";

        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET; // Force IPv4 to avoid macOS ::1 vs 127.0.0.1 issues
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(domain.c_str(), nullptr, &hints, &res) != 0) {
            return {};
        }

        // Convert the first result to an IP string
        char ipBuf[INET_ADDRSTRLEN];
        struct sockaddr_in* ipv4 = (struct sockaddr_in*)res->ai_addr;
        inet_ntop(AF_INET, &(ipv4->sin_addr), ipBuf, INET_ADDRSTRLEN);
        freeaddrinfo(res);

        std::string resolvedIP = ipBuf;
        return portStr.empty() ? resolvedIP : (resolvedIP + ":" + portStr);
    }
}