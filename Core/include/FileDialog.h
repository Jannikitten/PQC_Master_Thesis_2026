#ifndef PQC_MASTER_THESIS_2026_FILE_DIALOG_H
#define PQC_MASTER_THESIS_2026_FILE_DIALOG_H

// ═════════════════════════════════════════════════════════════════════════════
// FileDialog.h — cross-platform native file picker
//
// Opens the OS-native file dialog (NSOpenPanel on macOS, IFileDialog on
// Windows, zenity/kdialog on Linux) filtered to image types.
// Returns the selected file path as std::optional<std::string>.
//
// Usage:
//     auto path = Safira::FileDialog::OpenImage();
//     if (path) { /* load *path */ }
//
// Implementation is in FileDialog.mm (macOS) / FileDialog.cpp (Win/Linux).
// ═════════════════════════════════════════════════════════════════════════════

#include <optional>
#include <string>
#include <vector>

namespace Safira {

    struct FileDialogFilter {
        std::string Name;                   // e.g. "Image Files"
        std::vector<std::string> Extensions; // e.g. {"png", "jpg", "jpeg", "bmp"}
    };

    class FileDialog {
    public:
        /// Open a file dialog filtered to common image types.
        /// Returns std::nullopt if the user cancels.
        [[nodiscard]] static std::optional<std::string> OpenImage();

        /// Open a file dialog with custom filters.
        [[nodiscard]] static std::optional<std::string>
        Open(const std::string& title, const std::vector<FileDialogFilter>& filters);
    };

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_FILE_DIALOG_H