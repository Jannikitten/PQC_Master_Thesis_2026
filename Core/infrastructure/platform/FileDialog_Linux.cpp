// Linux implementation of Safira::FileDialog — shells out to zenity (GTK) or
// kdialog (KDE). If neither is installed, returns std::nullopt so the caller
// can fall back to manual path entry.

#include "FileDialog.h"

#if !defined(__APPLE__) && !defined(_WIN32)

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>

namespace {

bool CommandExists(const char* cmd) {
    std::string probe = "command -v ";
    probe += cmd;
    probe += " >/dev/null 2>&1";
    return std::system(probe.c_str()) == 0;
}

std::optional<std::string> RunAndCapture(const std::string& cmd) {
    std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return std::nullopt;

    std::string out;
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), buf.size(), pipe.get()) != nullptr) {
        out += buf.data();
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    if (out.empty()) return std::nullopt;
    return out;
}

std::string ShellEscape(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

} // namespace

namespace Safira {

std::optional<std::string> FileDialog::OpenImage() {
    return Open("Select Avatar Image", {
        { "Image Files", { "png", "jpg", "jpeg", "bmp", "gif", "webp" } }
    });
}

std::optional<std::string>
FileDialog::Open(const std::string& title,
                 const std::vector<FileDialogFilter>& filters) {
    if (CommandExists("zenity")) {
        std::ostringstream cmd;
        cmd << "zenity --file-selection --title=" << ShellEscape(title);
        for (const auto& f : filters) {
            std::ostringstream pattern;
            pattern << f.Name << " |";
            for (const auto& ext : f.Extensions) pattern << " *." << ext;
            cmd << " --file-filter=" << ShellEscape(pattern.str());
        }
        cmd << " 2>/dev/null";
        return RunAndCapture(cmd.str());
    }

    if (CommandExists("kdialog")) {
        std::ostringstream cmd;
        cmd << "kdialog --title " << ShellEscape(title) << " --getopenfilename . ";
        std::ostringstream pattern;
        for (const auto& f : filters) {
            for (const auto& ext : f.Extensions) pattern << "*." << ext << ' ';
        }
        std::string p = pattern.str();
        if (!p.empty() && p.back() == ' ') p.pop_back();
        cmd << ShellEscape(p + "|Images");
        cmd << " 2>/dev/null";
        return RunAndCapture(cmd.str());
    }

    return std::nullopt;
}

} // namespace Safira

#endif // !__APPLE__ && !_WIN32
