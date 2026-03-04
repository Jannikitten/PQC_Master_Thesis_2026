#ifdef __APPLE__

#import <Cocoa/Cocoa.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// ─────────────────────────────────────────────────────────────────────────────
// Vertically centre the three traffic-light buttons inside our custom titlebar.
// ─────────────────────────────────────────────────────────────────────────────
static void CenterTrafficLights(NSWindow* nsWin, float titlebarH) {
    NSButton* close = [nsWin standardWindowButton:NSWindowCloseButton];
    NSButton* mini  = [nsWin standardWindowButton:NSWindowMiniaturizeButton];
    NSButton* zoom  = [nsWin standardWindowButton:NSWindowZoomButton];
    if (!close || !mini || !zoom) return;

    NSView* container = close.superview;
    if (!container) return;

    CGFloat btnH   = close.frame.size.height;       // ~14 pts
    CGFloat offset = (titlebarH - btnH) * 0.5;      // vertical centring

    for (NSButton* btn in @[close, mini, zoom]) {
        NSRect f = btn.frame;
        f.origin.y = container.frame.size.height - offset - btnH;
        btn.frame = f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void MacOS_StyleWindow(GLFWwindow* window, float r, float g, float b,
                       float titlebarHeight) {
    NSWindow* nsWin = (NSWindow*)glfwGetCocoaWindow(window);
    if (!nsWin) return;

    // Hide the title text
    nsWin.titleVisibility = NSWindowTitleHidden;

    // Make the native titlebar transparent so our ImGui bar paints over it
    nsWin.titlebarAppearsTransparent = YES;

    // Content extends behind the titlebar — traffic lights and ImGui
    // content share the same coordinate space.
    nsWin.styleMask |= NSWindowStyleMaskFullSizeContentView;

    // Match the window background to the app background
    nsWin.backgroundColor = [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];

    // Centre traffic lights in our custom titlebar
    CenterTrafficLights(nsWin, titlebarHeight);
}

// ─────────────────────────────────────────────────────────────────────────────
void MacOS_SetWindowColor(GLFWwindow* window, float r, float g, float b) {
    NSWindow* nsWin = (NSWindow*)glfwGetCocoaWindow(window);
    if (!nsWin) return;

    nsWin.backgroundColor = [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];
}

// ─────────────────────────────────────────────────────────────────────────────
void MacOS_RepositionTrafficLights(GLFWwindow* window, float titlebarHeight) {
    NSWindow* nsWin = (NSWindow*)glfwGetCocoaWindow(window);
    if (!nsWin) return;

    CenterTrafficLights(nsWin, titlebarHeight);
}

// ─────────────────────────────────────────────────────────────────────────────
void MacOS_BeginNativeDrag(GLFWwindow* window) {
    NSWindow* nsWin = (NSWindow*)glfwGetCocoaWindow(window);
    if (!nsWin) return;

    // Hand the drag off to the macOS window manager.
    // performWindowDragWithEvent: uses the current event to start a smooth,
    // system-level window move — no per-frame glfwSetWindowPos needed.
    NSEvent* event = nsWin.currentEvent;
    if (event && (event.type == NSEventTypeLeftMouseDown ||
                  event.type == NSEventTypeLeftMouseDragged)) {
        [nsWin performWindowDragWithEvent:event];
    }
}

// ─────────────────────────────────────────────────────────────────────────────
float MacOS_GetTrafficLightWidth(GLFWwindow* window) {
    NSWindow* nsWin = (NSWindow*)glfwGetCocoaWindow(window);
    if (!nsWin) return 0.0f;

    NSButton* zoomBtn = [nsWin standardWindowButton:NSWindowZoomButton];
    if (zoomBtn) {
        NSRect frame = zoomBtn.frame;
        float rightEdge = (float)(frame.origin.x + frame.size.width);
        return rightEdge + 12.0f;
    }

    return 78.0f;
}

#endif