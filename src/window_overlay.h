#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "gui.h"
#include "utils.h"
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

// Forward declarations
struct WindowOverlayConfig;
struct Color;

// Render data structure - contains only what the render thread needs (immutable after creation)
struct WindowOverlayRenderData {
    unsigned char* pixelData = nullptr;
    int width = 0;
    int height = 0;

    WindowOverlayRenderData() = default;
    ~WindowOverlayRenderData() {
        if (pixelData) {
            delete[] pixelData;
            pixelData = nullptr;
        }
    }

    // Delete copy but allow move
    WindowOverlayRenderData(const WindowOverlayRenderData&) = delete;
    WindowOverlayRenderData& operator=(const WindowOverlayRenderData&) = delete;
    WindowOverlayRenderData(WindowOverlayRenderData&& other) noexcept
        : pixelData(other.pixelData),
          width(other.width),
          height(other.height) {
        other.pixelData = nullptr;
        other.width = 0;
        other.height = 0;
    }
    WindowOverlayRenderData& operator=(WindowOverlayRenderData&& other) noexcept {
        if (this != &other) {
            if (pixelData) delete[] pixelData;
            pixelData = other.pixelData;
            width = other.width;
            height = other.height;
            other.pixelData = nullptr;
            other.width = 0;
            other.height = 0;
        }
        return *this;
    }
};

// Window overlay cache entry for captured window content
struct WindowOverlayCacheEntry {
    std::string windowTitle;
    std::string windowClass;
    std::string executableName;
    std::string windowMatchPriority = "title";
    HWND targetWindow = NULL;

    // Cached bitmap data (capture thread only)
    HBITMAP hBitmap = NULL;
    HDC hdcMem = NULL;
    unsigned char* pixelData = nullptr;
    int width = 0;
    int height = 0;

    // Triple-buffered render data for lock-free rendering
    // Capture thread writes to writeBuffer, then swaps with readyBuffer
    // Render thread swaps readyBuffer with backBuffer, then reads from backBuffer
    std::unique_ptr<WindowOverlayRenderData> writeBuffer;
    std::unique_ptr<WindowOverlayRenderData> readyBuffer;          // Last completed capture, waiting to swap to render
    std::unique_ptr<WindowOverlayRenderData> backBuffer;           // Currently being read by render thread (safe from capture)
    std::atomic<bool> hasNewFrame{ false };                        // True when readyBuffer has new data for render thread
    std::mutex swapMutex; // Used for swapping buffers between threads

    // OpenGL texture caching (render thread only - no locking needed)
    unsigned int glTextureId = 0;
    int glTextureWidth = 0;
    int glTextureHeight = 0;
    WindowOverlayRenderData* lastUploadedRenderData = nullptr; // Track which buffer was last uploaded

    // Cached rendering data (invalidated when config changes)
    struct CachedRenderState {
        // Config hash to detect changes
        int crop_left = -1;
        int crop_right = -1;
        int crop_top = -1;
        int crop_bottom = -1;
        float scale = -1.0f;
        int x = 0;
        int y = 0;
        std::string relativeTo;
        int screenWidth = 0;
        int screenHeight = 0;

        // Cached computed values
        int displayW = 0;
        int displayH = 0;
        int finalScreenX_win = 0;
        int finalScreenY_win = 0;
        float nx1 = 0, ny1 = 0, nx2 = 0, ny2 = 0;
        float tx1 = 0, ty1 = 0, tx2 = 0, ty2 = 0;

        bool isValid = false;
    } cachedRenderState;

    // Timing for FPS control
    std::chrono::steady_clock::time_point lastCaptureTime;
    std::chrono::steady_clock::time_point lastRenderTime;
    std::atomic<int> fps{ 30 };

    // Window search timing
    std::chrono::steady_clock::time_point lastSearchTime;
    std::atomic<int> searchInterval{ 1000 }; // Search interval in milliseconds

    // Performance profiling
    std::chrono::microseconds lastCaptureTimeUs{ 0 };
    std::chrono::microseconds lastUploadTimeUs{ 0 };

    // Thread safety
    std::mutex captureMutex;
    std::atomic<bool> needsUpdate{ true };

    WindowOverlayCacheEntry() {
        writeBuffer = std::make_unique<WindowOverlayRenderData>();
        readyBuffer = std::make_unique<WindowOverlayRenderData>();
        backBuffer = std::make_unique<WindowOverlayRenderData>();
    }
    ~WindowOverlayCacheEntry();

    // Delete copy and move constructors since std::mutex is not movable/copyable
    WindowOverlayCacheEntry(const WindowOverlayCacheEntry&) = delete;
    WindowOverlayCacheEntry& operator=(const WindowOverlayCacheEntry&) = delete;
    WindowOverlayCacheEntry(WindowOverlayCacheEntry&&) = delete;
    WindowOverlayCacheEntry& operator=(WindowOverlayCacheEntry&&) = delete;
};

// Window overlay system functions
// NOTE: InitializeWindowOverlays() is called from the background window capture thread
// to avoid blocking the render thread during expensive window searching
void InitializeWindowOverlays();
void LoadWindowOverlay(const std::string& overlayId, const WindowOverlayConfig& config);
void QueueOverlayReload(const std::string& overlayId, const WindowOverlayConfig& config); // Non-blocking deferred reload
void CleanupWindowOverlayCache();
void CleanupWindowOverlayCacheEntry(const std::string& overlayId);
void RemoveWindowOverlayFromCache(const std::string& overlayId); // Remove from cache without accessing config
void UpdateWindowOverlay(const std::string& overlayId);
void UpdateWindowOverlayFPS(const std::string& overlayId, int newFPS);
void UpdateWindowOverlaySearchInterval(const std::string& overlayId, int newSearchInterval);
void UpdateAllWindowOverlays();
bool CaptureWindowContent(WindowOverlayCacheEntry& entry, const WindowOverlayConfig& config);
void RenderWindowOverlaysGL(const std::vector<std::string>& windowOverlayIds, int screenWidth, int screenHeight,
                            float opacityMultiplier = 1.0f, bool excludeOnlyOnMyScreen = false);
HWND FindWindowByTitleAndClass(const std::string& title, const std::string& className);
const WindowOverlayConfig* FindWindowOverlayConfig(const std::string& overlayId);
const WindowOverlayConfig* FindWindowOverlayConfigIn(const std::string& overlayId, const Config& config);

// Window enumeration callback for finding target windows
BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam);

// Structure to hold window information for the dropdown
struct WindowInfo {
    std::string title;
    std::string className;
    std::string executableName;
    HWND hwnd;

    std::string GetDisplayName() const {
        std::string display;
        if (!executableName.empty()) { display = "[" + executableName + "] "; }
        if (title.empty()) {
            display += "[No Title]";
        } else {
            display += title;
        }
        return display;
    }
};

// Function to get list of currently open windows for GUI dropdown
std::vector<WindowInfo> GetCurrentlyOpenWindows();

// Helper function to check if a window is still valid/visible
bool IsWindowInfoValid(const WindowInfo& windowInfo);

// Get cached window list (returns empty vector if not ready, avoids blocking GUI)
std::vector<WindowInfo> GetCachedWindowList();

// Get profiling information for performance analysis
std::string GetWindowOverlayProfilingInfo();

// State tracking for focused/interactive window overlay
extern std::atomic<bool> g_windowOverlayInteractionActive;
extern std::string g_focusedWindowOverlayName;
extern std::mutex g_focusedWindowOverlayMutex;

// Check if a point is within a window overlay's bounds
// Returns the overlay name if found, empty string otherwise
std::string GetWindowOverlayAtPoint(int x, int y, int screenWidth, int screenHeight);

// Get the HWND for a window overlay by name
HWND GetWindowOverlayHWND(const std::string& overlayName);

// Translate screen coordinates to window overlay coordinates
// Returns true if successful, sets outX/outY to coordinates within the target window
bool TranslateToWindowOverlayCoords(const std::string& overlayName, int screenX, int screenY, int screenWidth, int screenHeight, int& outX,
                                    int& outY);

// Focus a window overlay for interaction (call when clicking on it)
void FocusWindowOverlay(const std::string& overlayName);

// Unfocus the current window overlay (call when clicking outside or pressing Escape)
void UnfocusWindowOverlay();

// Check if any window overlay is currently focused for interaction
bool IsWindowOverlayFocused();

// Get the name of the currently focused window overlay
std::string GetFocusedWindowOverlayName();

// Forward a mouse message to the focused window overlay
// Returns true if the message was handled
bool ForwardMouseToWindowOverlay(UINT uMsg, int screenX, int screenY, WPARAM wParam, int screenWidth, int screenHeight);

// Forward a keyboard message to the focused window overlay
// Returns true if the message was handled
bool ForwardKeyboardToWindowOverlay(UINT uMsg, WPARAM wParam, LPARAM lParam);

// Global window overlay cache
extern std::map<std::string, std::unique_ptr<WindowOverlayCacheEntry>> g_windowOverlayCache;
extern std::mutex g_windowOverlayCacheMutex;

// Background window list cache for GUI
extern std::atomic<std::vector<WindowInfo>*> g_windowListCache;
extern std::mutex g_windowListCacheMutex;
extern std::chrono::steady_clock::time_point g_lastWindowListUpdate;

// Background capture thread management
extern std::atomic<bool> g_stopWindowCaptureThread;
extern std::thread g_windowCaptureThread;
void WindowCaptureThreadFunc();
void StartWindowCaptureThread();
void StopWindowCaptureThread();
