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
    if (!close || !close.superview) return;

    NSView* container = close.superview;
    NSView* parent    = container.superview;
    if (!parent) return;

    // ── Padding ──────────────────────────────────────────────────
    constexpr CGFloat padLeft = 8.0;
    constexpr CGFloat padTop  = 0.0;
    // ─────────────────────────────────────────────────────────────

    static NSLayoutConstraint* sLeading = nil;
    static NSLayoutConstraint* sTop     = nil;
    static bool sSetup = false;

    if (!sSetup) {
        // Save original size before touching constraints
        CGFloat origW = container.frame.size.width;
        CGFloat origH = container.frame.size.height;

        // Remove ALL constraints involving the container from its parent
        NSMutableArray* toRemove = [NSMutableArray array];
        for (NSLayoutConstraint* c in parent.constraints) {
            if (c.firstItem == container || c.secondItem == container)
                [toRemove addObject:c];
        }
        [NSLayoutConstraint deactivateConstraints:toRemove];

        // Switch from autoresizing to explicit constraints
        container.translatesAutoresizingMaskIntoConstraints = NO;

        // Fully specify position AND size — no ambiguity
        sLeading = [container.leadingAnchor
                    constraintEqualToAnchor:parent.leadingAnchor
                    constant:padLeft];

        CGFloat topVal = (titlebarH - origH) / 2.0 + padTop;
        sTop = [container.topAnchor
                constraintEqualToAnchor:parent.topAnchor
                constant:topVal];

        NSLayoutConstraint* w = [container.widthAnchor
                                 constraintEqualToConstant:origW];
        NSLayoutConstraint* h = [container.heightAnchor
                                 constraintEqualToConstant:origH];

        [NSLayoutConstraint activateConstraints:@[sLeading, sTop, w, h]];
        sSetup = true;
    } else {
        // Update vertical position each frame (handles window resize)
        CGFloat origH = container.frame.size.height;
        sTop.constant = (titlebarH - origH) / 2.0 + padTop;
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
        return rightEdge + 20.0f;  // was 12.0f — increase this for more gap
    }
    return 78.0f;
}

#endif