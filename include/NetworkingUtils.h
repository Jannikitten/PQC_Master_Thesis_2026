#ifndef PQC_MASTER_THESIS_2026_NETWORKINGUTILS_H
#define PQC_MASTER_THESIS_2026_NETWORKINGUTILS_H

#include <string>

namespace Safira::Utils {

    bool IsValidIPAddress(std::string_view ipAddress);

    // Platform-specific implementations
    std::string ResolveDomainName(std::string_view name);

}

#endif //PQC_MASTER_THESIS_2026_NETWORKINGUTILS_H