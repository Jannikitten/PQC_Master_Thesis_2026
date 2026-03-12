#pragma once

// Forward-declare the helper so C++ code can call it.
// Implementation lives in MacOSWindow.mm (Objective-C++).

struct GLFWwindow;

#ifdef __APPLE__
/// Hides the native window title text and makes the titlebar
/// background transparent so ImGui can paint over it seamlessly.
/// Centres the traffic-light buttons within the given titlebar height.
/// Call once after glfwCreateWindow + glfwShowWindow.
void MacOS_StyleWindow(GLFWwindow* window, float r, float g, float b,
                       float titlebarHeight);

/// Update the native titlebar background colour (call when theme changes).
void MacOS_SetWindowColor(GLFWwindow* window, float r, float g, float b);

/// Re-centre traffic-light buttons (call after resize / layout changes).
void MacOS_RepositionTrafficLights(GLFWwindow* window, float titlebarHeight);

/// Start a native macOS window drag using the current event.
/// Call this once when a mouse-down is detected on the titlebar background.
void MacOS_BeginNativeDrag(GLFWwindow* window);

/// Returns the horizontal space occupied by the traffic-light buttons
float MacOS_GetTrafficLightWidth(GLFWwindow* window);
#else
inline void MacOS_StyleWindow(GLFWwindow*, float, float, float, float) {}
inline void MacOS_SetWindowColor(GLFWwindow*, float, float, float) {}
inline void MacOS_RepositionTrafficLights(GLFWwindow*, float) {}
inline void MacOS_BeginNativeDrag(GLFWwindow*) {}
inline float MacOS_GetTrafficLightWidth(GLFWwindow*) { return 0.0f; }
#endif