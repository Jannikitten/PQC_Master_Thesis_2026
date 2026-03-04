// ═════════════════════════════════════════════════════════════════════════════
// FileDialog.mm — macOS native file dialog via NSOpenPanel
//
// Must be compiled as Objective-C++ (.mm extension).
// Link against Cocoa.framework (already linked for the ImGui/GLFW app).
// ═════════════════════════════════════════════════════════════════════════════

#include "FileDialog.h"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>

namespace Safira {

std::optional<std::string> FileDialog::OpenImage() {
    return Open("Select Avatar Image", {
        { "Image Files", { "png", "jpg", "jpeg", "bmp", "gif", "webp" } }
    });
}

std::optional<std::string>
FileDialog::Open(const std::string& title,
                 const std::vector<FileDialogFilter>& filters) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setTitle:[NSString stringWithUTF8String:title.c_str()]];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];

        // Build UTType array from filter extensions
        NSMutableArray<UTType*>* contentTypes = [NSMutableArray array];
        for (const auto& filter : filters) {
            for (const auto& ext : filter.Extensions) {
                NSString* nsExt = [NSString stringWithUTF8String:ext.c_str()];
                UTType* type = [UTType typeWithFilenameExtension:nsExt];
                if (type)
                    [contentTypes addObject:type];
            }
        }
        if (contentTypes.count > 0)
            [panel setAllowedContentTypes:contentTypes];

        NSModalResponse result = [panel runModal];
        if (result != NSModalResponseOK)
            return std::nullopt;

        NSURL* url = [[panel URLs] firstObject];
        if (!url)
            return std::nullopt;

        return std::string([[url path] UTF8String]);
    }
}

} // namespace Safira

#endif // __APPLE__