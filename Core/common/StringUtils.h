#ifndef PQC_MASTER_THESIS_2026_STRINGUTILS_H
#define PQC_MASTER_THESIS_2026_STRINGUTILS_H

#include <string>
#include <string_view>
#include <vector>

namespace Safira::Utils {

    std::vector<std::string> SplitString(const std::string_view string, const std::string_view& delimiters);
    std::vector<std::string> SplitString(const std::string_view string, const char delimiter);

}

#endif //PQC_MASTER_THESIS_2026_STRINGUTILS_H