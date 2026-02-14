#include "window_overlay.h"
#include "gui.h"
#include "profiler.h"
#include "render.h"
#include "utils.h"
#include <GL/wglew.h>
#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <tuple>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

// Global variables for window overlay cache and thread management
std::map<std::string, std::unique_ptr<WindowOverlayCacheEntry>> g_windowOverlayCache;
std::mutex g_windowOverlayCacheMutex;

std::atomic<bool> g_stopWindowCaptureThread{ false };
std::thread g_windowCaptureThread;

// Global window list cache for GUI (prevents UI blocking from expensive window enumeration)
std::atomic<std::vector<WindowInfo>*> g_windowListCache{ nullptr };
std::mutex g_windowListCacheMutex;
std::chrono::steady_clock::time_point g_lastWindowListUpdate;

// Deferred overlay reload queue (for GUI thread to request reloads without blocking)
struct DeferredOverlayReload {
    std::string overlayId;
    WindowOverlayConfig config;
};
std::vector<DeferredOverlayReload> g_deferredOverlayReloads;
std::mutex g_deferredOverlayReloadsMutex;

// Implementation of WindowOverlayCacheEntry destructor
WindowOverlayCacheEntry::~WindowOverlayCacheEntry() {
    if (hBitmap) {
        DeleteObject(hBitmap);
        hBitmap = NULL;
    }
    if (hdcMem) {
        DeleteDC(hdcMem);
        hdcMem = NULL;
    }
    if (pixelData) {
        delete[] pixelData;
        pixelData = nullptr;
    }
    // Triple-buffer cleanup is handled automatically by unique_ptr
    // Note: OpenGL texture cleanup should be done on the OpenGL thread
    // The texture will be cleaned up in CleanupWindowOverlayCacheEntry
}

// Forward declaration
std::string GetExecutableNameFromWindow(HWND hwnd);

// Find window with priority-based matching (OBS-style)
HWND FindWindowByTitleAndClass(const std::string& title, const std::string& className, const std::string& executableName = "",
                               const std::string& matchPriority = "title") {
    struct EnumData {
        std::string targetTitle;
        std::string targetClass;
        std::string targetExecutable;
        std::string matchPriority;
        HWND exactMatch;      // Exact title match
        HWND classMatch;      // Same class, different title
        HWND executableMatch; // Same executable, different title
    };

    EnumData data;
    data.targetTitle = title;
    data.targetClass = className;
    data.targetExecutable = executableName;
    data.matchPriority = matchPriority;
    data.exactMatch = NULL;
    data.classMatch = NULL;
    data.executableMatch = NULL;

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            EnumData* data = reinterpret_cast<EnumData*>(lParam);

            // Skip invisible windows
            if (!IsWindowVisible(hwnd)) { return TRUE; }

            // Get window title
            char windowTitle[256];
            GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle));
            std::string title = windowTitle;

            // Get window class
            char windowClass[256];
            GetClassNameA(hwnd, windowClass, sizeof(windowClass));
            std::string className = windowClass;

            // Get executable name
            std::string exeName = GetExecutableNameFromWindow(hwnd);

            // Check for exact title match
            bool titleMatch = !data->targetTitle.empty() && title == data->targetTitle;

            // Check for class match
            bool classMatch = !data->targetClass.empty() && className == data->targetClass;

            // Check for executable match
            bool executableMatch = !data->targetExecutable.empty() && exeName == data->targetExecutable;

            // Priority 1: Exact title match
            if (titleMatch) {
                data->exactMatch = hwnd;
                return FALSE; // Found exact match, stop enumeration
            }

            // Priority 2: Same class (for "title_class" mode)
            if (classMatch && data->classMatch == NULL) { data->classMatch = hwnd; }

            // Priority 3: Same executable (for "title_executable" mode)
            if (executableMatch && data->executableMatch == NULL) { data->executableMatch = hwnd; }

            return TRUE; // Continue enumeration
        },
        reinterpret_cast<LPARAM>(&data));

    // Return based on priority and what was found
    if (data.exactMatch) { return data.exactMatch; }

    // Apply match priority
    if (matchPriority == "title_class" && data.classMatch) { return data.classMatch; }

    if (matchPriority == "title_executable" && data.executableMatch) { return data.executableMatch; }

    // For "title" mode, only return exact matches
    return NULL;
}

// Global initialization flag
std::atomic<bool> g_windowOverlaysInitialized{ false };

// Lazy initialization of window overlays
// NOTE: This is called from the window capture background thread
// to avoid blocking the render thread during expensive window searching
void InitializeWindowOverlays() {
    // Use config snapshot for thread-safe access (called from background thread)
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) {
        Log("No config snapshot available for window overlay initialization, skipping");
        return;
    }

    // Don't initialize if there are no window overlays configured
    if (cfgSnap->windowOverlays.empty()) {
        Log("No window overlays configured, skipping initialization");
        return;
    }

    for (const auto& config : cfgSnap->windowOverlays) {
        try {
            LoadWindowOverlay(config.name, config);
        } catch (const std::exception& e) { Log("Error loading window overlay '" + config.name + "': " + e.what()); } catch (...) {
            Log("Unknown error loading window overlay '" + config.name + "'");
        }
    }

    Log("Initialized " + std::to_string(cfgSnap->windowOverlays.size()) + " window overlays");
}

// Internal helper - updates entry without acquiring lock (caller must hold lock)
static void LoadWindowOverlay_Internal(const std::string& overlayId, const WindowOverlayConfig& config) {
    // Note: Caller must hold g_windowOverlayCacheMutex

    // Check if we're updating an existing entry
    auto it = g_windowOverlayCache.find(overlayId);
    if (it != g_windowOverlayCache.end()) {
        // Update existing entry instead of recreating (preserves OpenGL texture)
        auto& entry = it->second;

        // Check if window target is actually changing
        bool windowChanged = (entry->windowTitle != config.windowTitle || entry->windowClass != config.windowClass ||
                              entry->executableName != config.executableName || entry->windowMatchPriority != config.windowMatchPriority);

        entry->windowTitle = config.windowTitle;
        entry->windowClass = config.windowClass;
        entry->executableName = config.executableName;
        entry->windowMatchPriority = config.windowMatchPriority;
        entry->fps.store(config.fps, std::memory_order_relaxed);
        entry->searchInterval.store(config.searchInterval, std::memory_order_relaxed);
        entry->needsUpdate.store(true, std::memory_order_relaxed);

        // Only reset lastUploadedRenderData if window target changed
        // This prevents flickering when only other properties change
        if (windowChanged) {
            // Keep the current render buffer visible while we capture the new window
            // Don't reset lastUploadedRenderData - let the new capture naturally update it
            entry->targetWindow =
                FindWindowByTitleAndClass(config.windowTitle, config.windowClass, config.executableName, config.windowMatchPriority);

            if (entry->targetWindow) {
                Log("Updated target window for overlay '" + overlayId + "': " + config.windowTitle);
            } else {
                Log("Warning: Could not find target window for overlay '" + overlayId + "': " + config.windowTitle);
            }
        }

        return;
    }

    // Create new cache entry using unique_ptr
    auto entry = std::make_unique<WindowOverlayCacheEntry>();
    entry->windowTitle = config.windowTitle;
    entry->windowClass = config.windowClass;
    entry->executableName = config.executableName;
    entry->windowMatchPriority = config.windowMatchPriority;
    entry->fps.store(config.fps, std::memory_order_relaxed);
    entry->searchInterval.store(config.searchInterval, std::memory_order_relaxed);
    entry->needsUpdate.store(true, std::memory_order_relaxed);

    // Find the target window
    entry->targetWindow =
        FindWindowByTitleAndClass(config.windowTitle, config.windowClass, config.executableName, config.windowMatchPriority);

    if (entry->targetWindow) {
        Log("Found target window for overlay '" + overlayId + "': " + config.windowTitle);
    } else {
        Log("Warning: Could not find target window for overlay '" + overlayId + "': " + config.windowTitle);
    }

    // Insert into cache
    g_windowOverlayCache[overlayId] = std::move(entry);
}

// Load window overlay configuration and initialize cache entry
void LoadWindowOverlay(const std::string& overlayId, const WindowOverlayConfig& config) {
    std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);
    LoadWindowOverlay_Internal(overlayId, config);
}

// Queue a deferred overlay reload (non-blocking, safe to call from GUI thread)
void QueueOverlayReload(const std::string& overlayId, const WindowOverlayConfig& config) {
    std::lock_guard<std::mutex> lock(g_deferredOverlayReloadsMutex);
    // Check if already queued to avoid duplicates
    for (auto& pending : g_deferredOverlayReloads) {
        if (pending.overlayId == overlayId) {
            // Update existing entry
            pending.config = config;
            return;
        }
    }
    // Add new entry
    g_deferredOverlayReloads.push_back({ overlayId, config });
}

// Update all window overlays (refresh window handles)
void UpdateAllWindowOverlays() {
    std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);
    auto now = std::chrono::steady_clock::now();

    for (auto& [overlayId, entry] : g_windowOverlayCache) {
        // Check if the window is still valid
        if (entry->targetWindow && !IsWindow(entry->targetWindow)) {
            entry->targetWindow = NULL;
            entry->lastSearchTime = now; // Reset search timer when window is lost
        }

        // Try to find window again if we lost it, respecting the search interval
        if (!entry->targetWindow) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry->lastSearchTime);
            int interval = entry->searchInterval.load(std::memory_order_relaxed);

            if (elapsed.count() >= interval) {
                entry->targetWindow =
                    FindWindowByTitleAndClass(entry->windowTitle, entry->windowClass, entry->executableName, entry->windowMatchPriority);
                entry->lastSearchTime = now;

                if (entry->targetWindow) {
                    Log("Reacquired target window for overlay '" + overlayId + "'");
                    entry->needsUpdate.store(true, std::memory_order_relaxed);
                }
            }
        }
    }
}

// Update window overlay FPS setting
void UpdateWindowOverlayFPS(const std::string& overlayId, int newFPS) {
    // LOCK-FREE: fps is atomic, so we just need to get the entry pointer safely
    // We use a very brief lock just to get the pointer, then release immediately
    WindowOverlayCacheEntry* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);
        auto it = g_windowOverlayCache.find(overlayId);
        if (it != g_windowOverlayCache.end()) { entry = it->second.get(); }
    }
    // Lock released - now we can safely access atomic members

    if (entry) {
        // These are atomic operations, no lock needed
        entry->fps.store(newFPS, std::memory_order_relaxed);
        entry->needsUpdate.store(true, std::memory_order_relaxed);
        Log("Updated FPS for overlay '" + overlayId + "' to " + std::to_string(newFPS));
    } else {
        // Cache entry doesn't exist yet, which is normal for new overlays
        // The FPS will be set when LoadWindowOverlay is called
        Log("FPS update requested for overlay '" + overlayId + "' but cache entry not found (overlay may not be loaded yet)");
    }
}

// Update window overlay search interval setting
void UpdateWindowOverlaySearchInterval(const std::string& overlayId, int newSearchInterval) {
    // LOCK-FREE: searchInterval is atomic, so we just need to get the entry pointer safely
    // We use a very brief lock just to get the pointer, then release immediately
    WindowOverlayCacheEntry* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);
        auto it = g_windowOverlayCache.find(overlayId);
        if (it != g_windowOverlayCache.end()) { entry = it->second.get(); }
    }
    // Lock released - now we can safely access atomic members

    if (entry) {
        // This is an atomic operation, no lock needed
        entry->searchInterval.store(newSearchInterval, std::memory_order_relaxed);
        Log("Updated search interval for overlay '" + overlayId + "' to " + std::to_string(newSearchInterval) + "ms");
    } else {
        // Cache entry doesn't exist yet, which is normal for new overlays
        // The search interval will be set when LoadWindowOverlay is called
        Log("Search interval update requested for overlay '" + overlayId + "' but cache entry not found (overlay may not be loaded yet)");
    }
}

// Update window overlay - refresh window handle if needed
// NOTE: This is called from the render thread via GUI. To avoid freezing,
// we only mark the entry for update and let the background thread do the expensive work.
void UpdateWindowOverlay(const std::string& overlayId) {
    std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);

    auto it = g_windowOverlayCache.find(overlayId);
    if (it == g_windowOverlayCache.end()) { return; }

    WindowOverlayCacheEntry& entry = *it->second;

    // Check if the window is still valid (fast check, no enumeration)
    if (entry.targetWindow && !IsWindow(entry.targetWindow)) { entry.targetWindow = NULL; }

    // Mark for update - the background thread will find the window
    // This avoids expensive window enumeration on the render thread
    entry.needsUpdate.store(true, std::memory_order_relaxed);
    entry.lastSearchTime = std::chrono::steady_clock::now() - std::chrono::seconds(100); // Force immediate search
}

// Capture window content using various methods based on config
bool CaptureWindowContent(WindowOverlayCacheEntry& entry, const WindowOverlayConfig& config) {
    std::lock_guard<std::mutex> lock(entry.captureMutex);

    if (!entry.targetWindow || !IsWindow(entry.targetWindow) || !IsWindowVisible(entry.targetWindow)) { return false; }

    // Check FPS throttling
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.lastCaptureTime);
    int targetInterval = 1000 / std::max(1, entry.fps.load(std::memory_order_relaxed));

    if (elapsed.count() < targetInterval && !entry.needsUpdate.load(std::memory_order_relaxed)) {
        return true; // Skip capture, too soon
    }

    entry.lastCaptureTime = now;
    entry.needsUpdate.store(false, std::memory_order_relaxed);

    if (!entry.targetWindow || !IsWindow(entry.targetWindow)) { return false; }

    // Get window dimensions using client area for more accurate capture
    RECT clientRect;
    if (!GetClientRect(entry.targetWindow, &clientRect)) { return false; }

    int windowWidth = clientRect.right - clientRect.left;
    int windowHeight = clientRect.bottom - clientRect.top;

    if (windowWidth <= 0 || windowHeight <= 0) { return false; }

    // Apply cropping
    int cropLeft = config.crop_left;
    int cropRight = config.crop_right;
    int cropTop = config.crop_top;
    int cropBottom = config.crop_bottom;

    int captureWidth = windowWidth - cropLeft - cropRight;
    int captureHeight = windowHeight - cropTop - cropBottom;

    if (captureWidth <= 0 || captureHeight <= 0) { return false; }

    // Create device contexts
    // NOTE: For PrintWindow, we DON'T need GetDC(targetWindow) - that causes flickering!
    // PrintWindow renders directly to the memory DC without needing the window's DC.
    // We only need hdcWindow for BitBlt fallback.
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    if (!hdcScreen || !hdcMem) {
        if (hdcScreen) ReleaseDC(NULL, hdcScreen);
        if (hdcMem) DeleteDC(hdcMem);
        return false;
    }

    // Create bitmap
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, captureWidth, captureHeight);
    if (!hBitmap) {
        ReleaseDC(NULL, hdcScreen);
        DeleteDC(hdcMem);
        return false;
    }

    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    // SECURITY: Clear the bitmap to black to prevent leaking screen content
    // If BitBlt/PrintWindow fails, we don't want uninitialized memory showing other windows
    RECT clearRect = { 0, 0, captureWidth, captureHeight };
    HBRUSH hBlackBrush = (HBRUSH)GetStockObject(DKGRAY_BRUSH);
    FillRect(hdcMem, &clearRect, hBlackBrush);

    // Set proper raster operation to avoid visual artifacts
    int oldROP = SetROP2(hdcMem, R2_COPYPEN);

    // Check if window is cloaked (hidden/minimized) or iconic (minimized) using DWM
    BOOL isCloaked = FALSE;
    HRESULT hr = DwmGetWindowAttribute(entry.targetWindow, DWMWA_CLOAKED, &isCloaked, sizeof(isCloaked));
    bool windowIsCloaked = SUCCEEDED(hr) && isCloaked;
    bool windowIsIconic = IsIconic(entry.targetWindow);

    // Determine if we should avoid PrintWindow (though PrintWindow itself is fine)
    // We avoid it for cloaked/iconic windows since they may not render properly
    bool shouldAvoidPrintWindow = windowIsCloaked || windowIsIconic;

    // Try capture methods based on config setting
    BOOL result = FALSE;
    HDC hdcWindow = NULL; // Only create when needed (for BitBlt fallback)
    bool usedPrintWindow = false;

    if (config.captureMethod == "BitBlt") {
        // BitBlt method: Captures from window DC with source offset
        // Note: BitBlt requires GetDC which CAN cause flicker on some windows
        hdcWindow = GetDC(entry.targetWindow);
        if (hdcWindow) { result = BitBlt(hdcMem, 0, 0, captureWidth, captureHeight, hdcWindow, cropLeft, cropTop, SRCCOPY); }
    } else {
        // Windows 10+ method (default): Uses PrintWindow with PW_RENDERFULLCONTENT
        // PrintWindow doesn't cause flicker - it renders directly without needing GetDC on target
        // IMPORTANT: PrintWindow captures from (0,0) so we need to capture the full window
        // and then extract the cropped region afterwards
        if (!shouldAvoidPrintWindow) {
            // If we have cropping, we need to capture the full window first
            bool needsCropping = (cropLeft > 0 || cropTop > 0 || cropRight > 0 || cropBottom > 0);

            if (needsCropping) {
                // Create a full-size bitmap to capture the entire window
                HBITMAP hFullBitmap = CreateCompatibleBitmap(hdcScreen, windowWidth, windowHeight);
                if (hFullBitmap) {
                    HDC hdcFullMem = CreateCompatibleDC(hdcScreen);
                    if (hdcFullMem) {
                        HBITMAP hOldFullBitmap = (HBITMAP)SelectObject(hdcFullMem, hFullBitmap);

                        // Capture the full window
                        result = PrintWindow(entry.targetWindow, hdcFullMem, PW_RENDERFULLCONTENT);

                        if (result) {
                            // Copy only the cropped region to our output bitmap
                            result = BitBlt(hdcMem, 0, 0, captureWidth, captureHeight, hdcFullMem, cropLeft, cropTop, SRCCOPY);
                            usedPrintWindow = true;
                        }

                        SelectObject(hdcFullMem, hOldFullBitmap);
                        DeleteDC(hdcFullMem);
                    }
                    DeleteObject(hFullBitmap);
                }
            } else {
                // No cropping needed, capture directly to our output bitmap
                result = PrintWindow(entry.targetWindow, hdcMem, PW_RENDERFULLCONTENT);
                usedPrintWindow = true;
            }
        }
        // Fall back to BitBlt only if PrintWindow fails or was avoided
        if (!result) {
            hdcWindow = GetDC(entry.targetWindow);
            if (hdcWindow) { result = BitBlt(hdcMem, 0, 0, captureWidth, captureHeight, hdcWindow, cropLeft, cropTop, SRCCOPY); }
        }
    }

    // Restore original ROP
    SetROP2(hdcMem, oldROP);

    // Extract pixel data - always extract even if capture failed (to get the black cleared bitmap)
    // This ensures we never show uninitialized memory or leaked screen content
    bool success = false;

    // Ensure we have pixel data buffer with safe size validation
    if (entry.width != captureWidth || entry.height != captureHeight) {
        if (entry.pixelData) {
            delete[] entry.pixelData;
            entry.pixelData = nullptr;
        }
        entry.width = captureWidth;
        entry.height = captureHeight;
        // Validate size before allocation to prevent overflow
        size_t bufferSize = static_cast<size_t>(captureWidth) * static_cast<size_t>(captureHeight) * 4;
        if (bufferSize > 0 && bufferSize < 100 * 1024 * 1024) { // Sanity check: < 100MB
            entry.pixelData = new unsigned char[bufferSize];    // RGBA
        } else {
            Log("[WindowOverlay] Invalid buffer size: " + std::to_string(bufferSize));
            // Clean up GDI resources before returning
            SelectObject(hdcMem, hOldBitmap);
            DeleteObject(hBitmap);
            DeleteDC(hdcMem);
            if (hdcWindow) { ReleaseDC(entry.targetWindow, hdcWindow); }
            ReleaseDC(NULL, hdcScreen);
            return false;
        }
    }

    // Convert bitmap to RGBA pixel data (will be black if capture failed)
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = captureWidth;
    bmi.bmiHeader.biHeight = -captureHeight; // Negative for top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    int scanlines = GetDIBits(hdcScreen, hBitmap, 0, captureHeight, entry.pixelData, &bmi, DIB_RGB_COLORS);

    if (scanlines == captureHeight) {
        // Convert BGRA to RGBA (even if capture failed, we still have the black cleared bitmap)
        const int totalPixels = captureWidth * captureHeight;

        // Pre-calculate color key values if enabled (optimization)
        if (result && config.enableColorKey && !config.colorKeys.empty()) {
            // Process each pixel and check against all color keys
            for (int i = 0; i < totalPixels; i++) {
                unsigned char* pixel = &entry.pixelData[i * 4];
                std::swap(pixel[0], pixel[2]); // Swap B and R

                // Get pixel color as float
                float r = pixel[0] / 255.0f;
                float g = pixel[1] / 255.0f;
                float b = pixel[2] / 255.0f;

                // Check against all color keys - pixel is transparent if it matches ANY key
                bool matchesAnyKey = false;
                for (const auto& key : config.colorKeys) {
                    float dr = r - key.color.r;
                    float dg = g - key.color.g;
                    float db = b - key.color.b;
                    float distanceSq = dr * dr + dg * dg + db * db;
                    float sensitivitySq = key.sensitivity * key.sensitivity;

                    if (distanceSq <= sensitivitySq) {
                        matchesAnyKey = true;
                        break; // No need to check other keys
                    }
                }

                pixel[3] = matchesAnyKey ? 0 : 255;
            }
        } else {
            // Just convert BGRA to RGBA without color key
            for (int i = 0; i < totalPixels; i++) {
                unsigned char* pixel = &entry.pixelData[i * 4];
                std::swap(pixel[0], pixel[2]); // Swap B and R
                pixel[3] = 255;                // Set alpha to fully opaque
            }
        }

        // Mark as successful only if the actual capture succeeded
        // If capture failed, we still update with black pixels (safe fallback)
        success = true;
    }

    // Mark texture for update if capture was successful
    if (success) {
        // Copy to write buffer for thread-safe transfer to render thread (OpenGL path)
        if (entry.writeBuffer->width != entry.width || entry.writeBuffer->height != entry.height) {
            if (entry.writeBuffer->pixelData) {
                delete[] entry.writeBuffer->pixelData;
                entry.writeBuffer->pixelData = nullptr;
            }
            entry.writeBuffer->width = entry.width;
            entry.writeBuffer->height = entry.height;
            // Validate size before allocation
            size_t bufferSize = static_cast<size_t>(entry.width) * static_cast<size_t>(entry.height) * 4;
            if (bufferSize > 0 && bufferSize < 100 * 1024 * 1024) { entry.writeBuffer->pixelData = new unsigned char[bufferSize]; }
        }

        if (entry.writeBuffer->pixelData && entry.pixelData) {
            size_t copySize = static_cast<size_t>(entry.width) * static_cast<size_t>(entry.height) * 4;
            memcpy(entry.writeBuffer->pixelData, entry.pixelData, copySize);

            // Swap write and ready buffers under lock, then signal new frame available
            {
                std::lock_guard<std::mutex> lock(entry.swapMutex);
                entry.writeBuffer.swap(entry.readyBuffer);
            }

            // Signal that a new frame is available for the render thread
            entry.hasNewFrame.store(true, std::memory_order_release);
        }
    }

    // Cleanup
    SelectObject(hdcMem, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    if (hdcWindow) { ReleaseDC(entry.targetWindow, hdcWindow); }
    ReleaseDC(NULL, hdcScreen);

    // If Windows 10+ method failed and capture failed, show error texture
    // BitBlt failures are not shown as error since BitBlt is the fallback method
    if (!success && config.captureMethod != "BitBlt") {
        // Capture failed with Windows 10+ method - create dark blue error texture
        int errorWidth = 64;
        int errorHeight = 64;

        // Ensure we have pixel data buffer for error texture
        if (entry.width != errorWidth || entry.height != errorHeight) {
            if (entry.pixelData) {
                delete[] entry.pixelData;
                entry.pixelData = nullptr;
            }
            entry.width = errorWidth;
            entry.height = errorHeight;
            // Validate size (error texture should be small anyway)
            size_t bufferSize = static_cast<size_t>(errorWidth) * static_cast<size_t>(errorHeight) * 4;
            if (bufferSize > 0 && bufferSize < 1 * 1024 * 1024) { // 1MB sanity check for error texture
                entry.pixelData = new unsigned char[bufferSize];
            }
        }

        // Fill with dark blue color (RGBA: 0, 32, 96, 255)
        for (int i = 0; i < errorWidth * errorHeight; i++) {
            entry.pixelData[i * 4 + 0] = 0;   // R
            entry.pixelData[i * 4 + 1] = 32;  // G
            entry.pixelData[i * 4 + 2] = 96;  // B
            entry.pixelData[i * 4 + 3] = 255; // A
        }

        // Copy error texture to write buffer
        if (entry.writeBuffer->width != entry.width || entry.writeBuffer->height != entry.height) {
            if (entry.writeBuffer->pixelData) {
                delete[] entry.writeBuffer->pixelData;
                entry.writeBuffer->pixelData = nullptr;
            }
            entry.writeBuffer->width = entry.width;
            entry.writeBuffer->height = entry.height;
            // Validate size before allocation
            size_t bufferSize = static_cast<size_t>(entry.width) * static_cast<size_t>(entry.height) * 4;
            if (bufferSize > 0 && bufferSize < 100 * 1024 * 1024) { entry.writeBuffer->pixelData = new unsigned char[bufferSize]; }
        }

        if (entry.writeBuffer->pixelData && entry.pixelData) {
            size_t copySize = static_cast<size_t>(entry.width) * static_cast<size_t>(entry.height) * 4;
            memcpy(entry.writeBuffer->pixelData, entry.pixelData, copySize);

            // Swap write and ready buffers under lock
            {
                std::lock_guard<std::mutex> lock(entry.swapMutex);
                entry.writeBuffer.swap(entry.readyBuffer);
            }

            // Signal that a new frame is available for the render thread
            entry.hasNewFrame.store(true, std::memory_order_release);
        }
    }

    return success;
}

// Helper function to find window overlay config by name
const WindowOverlayConfig* FindWindowOverlayConfig(const std::string& overlayId) {
    // Note: Caller should hold g_configMutex
    const auto& overlays = g_config.windowOverlays;
    const size_t size = overlays.size();
    for (size_t i = 0; i < size; ++i) {
        if (overlays[i].name == overlayId) { return &overlays[i]; }
    }
    return nullptr;
}

// Overload that takes a specific Config reference (for use with config snapshots)
const WindowOverlayConfig* FindWindowOverlayConfigIn(const std::string& overlayId, const Config& config) {
    const auto& overlays = config.windowOverlays;
    const size_t size = overlays.size();
    for (size_t i = 0; i < size; ++i) {
        if (overlays[i].name == overlayId) { return &overlays[i]; }
    }
    return nullptr;
}

// Remove window overlay from cache without accessing config (call when you already have config access)
void RemoveWindowOverlayFromCache(const std::string& overlayId) {
    std::lock_guard<std::mutex> cacheLock(g_windowOverlayCacheMutex);
    auto it = g_windowOverlayCache.find(overlayId);
    if (it != g_windowOverlayCache.end()) {
        // Clean up OpenGL texture if it exists
        if (it->second->glTextureId != 0) {
            glDeleteTextures(1, &it->second->glTextureId);
            it->second->glTextureId = 0;
        }
        g_windowOverlayCache.erase(it);
    }
}

// Clean up specific cache entry
void CleanupWindowOverlayCacheEntry(const std::string& overlayId) {
    // Don't actually erase the entry - this would cause crashes when capture thread is using it
    // Instead, just find the config and reload it which will update the entry in-place

    // First, get the config (without holding cache lock) - use snapshot for thread safety
    const WindowOverlayConfig* config = nullptr;
    WindowOverlayConfig configCopy; // Make a copy
    {
        auto cleanupSnap = GetConfigSnapshot();
        const WindowOverlayConfig* foundConfig = cleanupSnap ? FindWindowOverlayConfigIn(overlayId, *cleanupSnap) : nullptr;
        if (foundConfig) {
            configCopy = *foundConfig;
            config = &configCopy;
        }
    }

    // Now acquire cache lock and update
    std::lock_guard<std::mutex> cacheLock(g_windowOverlayCacheMutex);

    if (config) {
        // This will update the existing entry in-place safely
        LoadWindowOverlay_Internal(overlayId, *config);
    } else {
        // Config doesn't exist, so actually erase the entry (safe case - overlay removed from config)
        auto it = g_windowOverlayCache.find(overlayId);
        if (it != g_windowOverlayCache.end()) {
            // Clean up OpenGL texture if it exists
            if (it->second->glTextureId != 0) {
                glDeleteTextures(1, &it->second->glTextureId);
                it->second->glTextureId = 0;
            }
            g_windowOverlayCache.erase(it);
        }
    }
}

// Clean up entire window overlay cache
void CleanupWindowOverlayCache() {
    std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);

    // CRITICAL: Clean up OpenGL textures before clearing the cache
    // Only do this if we have a valid GL context to avoid crashes
    HGLRC currentContext = wglGetCurrentContext();
    if (currentContext) {
        for (auto& [id, entry] : g_windowOverlayCache) {
            if (entry && entry->glTextureId != 0) {
                try {
                    glDeleteTextures(1, &entry->glTextureId);
                    entry->glTextureId = 0;
                } catch (...) { Log("Exception cleaning up window overlay texture: " + id); }
            }
        }
    } else {
        Log("CleanupWindowOverlayCache: No valid GL context, skipping texture cleanup to avoid crashes");
    }

    g_windowOverlayCache.clear();
}

// NOTE: RenderWindowOverlaysGL() has been removed.
// All overlay rendering is now done asynchronously via the render thread.
// See RT_RenderWindowOverlays() in render_thread.cpp

std::atomic<bool> g_windowOverlayInteractionActive{ false };
std::string g_focusedWindowOverlayName;
std::mutex g_focusedWindowOverlayMutex;

// Helper function to calculate window overlay dimensions (internal use)
static void CalculateWindowOverlayDimensions(const WindowOverlayConfig& config, int& displayW, int& displayH) {
    // Try to get actual texture dimensions from cache
    std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);
    auto it = g_windowOverlayCache.find(config.name);
    if (it != g_windowOverlayCache.end() && it->second) {
        int texWidth = it->second->glTextureWidth;
        int texHeight = it->second->glTextureHeight;
        if (texWidth > 0 && texHeight > 0) {
            int croppedWidth = texWidth - config.crop_left - config.crop_right;
            int croppedHeight = texHeight - config.crop_top - config.crop_bottom;
            displayW = static_cast<int>(croppedWidth * config.scale);
            displayH = static_cast<int>(croppedHeight * config.scale);
            return;
        }
    }
    // Fallback to default dimensions
    displayW = static_cast<int>(100 * config.scale);
    displayH = static_cast<int>(100 * config.scale);
}

// Get the overlay under a specific screen point
std::string GetWindowOverlayAtPoint(int x, int y, int screenWidth, int screenHeight) {
    std::string currentModeId;
    {
        extern std::mutex g_modeIdMutex;
        extern std::string g_currentModeId;
        std::lock_guard<std::mutex> lock(g_modeIdMutex);
        currentModeId = g_currentModeId;
    }

    // Get the list of active window overlays for the current mode (use snapshot for thread safety)
    std::vector<std::pair<std::string, WindowOverlayConfig>> activeOverlays;
    {
        auto overlaySnap = GetConfigSnapshot();
        const ModeConfig* mode = overlaySnap ? GetModeFromSnapshot(*overlaySnap, currentModeId) : nullptr;
        if (!mode) return "";

        // Iterate in reverse order so topmost (last rendered) is checked first
        for (auto it = mode->windowOverlayIds.rbegin(); it != mode->windowOverlayIds.rend(); ++it) {
            const std::string& overlayId = *it;
            const WindowOverlayConfig* config = FindWindowOverlayConfigIn(overlayId, *overlaySnap);
            if (config && config->enableInteraction) { activeOverlays.emplace_back(overlayId, *config); }
        }
    }

    // Get current viewport for viewport-relative positioning
    ModeViewportInfo viewport = GetCurrentModeViewport();

    for (const auto& [overlayId, config] : activeOverlays) {
        int displayW, displayH;
        CalculateWindowOverlayDimensions(config, displayW, displayH);

        int finalScreenX, finalScreenY;

        // Check if this is a viewport-relative overlay
        bool isViewportRelative = IsViewportRelativeAnchor(config.relativeTo);

        if (isViewportRelative && viewport.valid) {
            // Use viewport-relative positioning
            GetRelativeCoordsForImageWithViewport(config.relativeTo, config.x, config.y, displayW, displayH, viewport.stretchX,
                                                  viewport.stretchY, viewport.stretchWidth, viewport.stretchHeight, screenWidth,
                                                  screenHeight, finalScreenX, finalScreenY);
        } else {
            // Use screen-relative positioning
            GetRelativeCoordsForImage(config.relativeTo, config.x, config.y, displayW, displayH, screenWidth, screenHeight, finalScreenX,
                                      finalScreenY);
        }

        // Check if point is within bounds
        if (x >= finalScreenX && x < finalScreenX + displayW && y >= finalScreenY && y < finalScreenY + displayH) { return overlayId; }
    }

    return "";
}

// Get the HWND for a window overlay by name
HWND GetWindowOverlayHWND(const std::string& overlayName) {
    std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);
    auto it = g_windowOverlayCache.find(overlayName);
    if (it != g_windowOverlayCache.end() && it->second) { return it->second->targetWindow; }
    return NULL;
}

// Translate screen coordinates to window overlay coordinates
bool TranslateToWindowOverlayCoords(const std::string& overlayName, int screenX, int screenY, int screenWidth, int screenHeight, int& outX,
                                    int& outY) {
    WindowOverlayConfig config;
    int texWidth = 0, texHeight = 0;

    // Get config and texture dimensions (use snapshot for thread safety)
    {
        auto translateSnap = GetConfigSnapshot();
        const WindowOverlayConfig* configPtr = translateSnap ? FindWindowOverlayConfigIn(overlayName, *translateSnap) : nullptr;
        if (!configPtr) return false;
        config = *configPtr;
    }

    {
        std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);
        auto it = g_windowOverlayCache.find(overlayName);
        if (it == g_windowOverlayCache.end() || !it->second) return false;
        texWidth = it->second->glTextureWidth;
        texHeight = it->second->glTextureHeight;
    }

    if (texWidth <= 0 || texHeight <= 0) return false;

    // Calculate display dimensions
    int croppedWidth = texWidth - config.crop_left - config.crop_right;
    int croppedHeight = texHeight - config.crop_top - config.crop_bottom;
    int displayW = static_cast<int>(croppedWidth * config.scale);
    int displayH = static_cast<int>(croppedHeight * config.scale);

    if (displayW <= 0 || displayH <= 0) return false;

    // Get overlay position on screen
    int overlayScreenX, overlayScreenY;

    // Check if this is a viewport-relative overlay
    bool isViewportRelative = IsViewportRelativeAnchor(config.relativeTo);

    if (isViewportRelative) {
        // Get current viewport for viewport-relative positioning
        ModeViewportInfo viewport = GetCurrentModeViewport();
        if (viewport.valid) {
            GetRelativeCoordsForImageWithViewport(config.relativeTo, config.x, config.y, displayW, displayH, viewport.stretchX,
                                                  viewport.stretchY, viewport.stretchWidth, viewport.stretchHeight, screenWidth,
                                                  screenHeight, overlayScreenX, overlayScreenY);
        } else {
            // Fallback to screen-relative if no valid viewport
            GetRelativeCoordsForImage(config.relativeTo, config.x, config.y, displayW, displayH, screenWidth, screenHeight, overlayScreenX,
                                      overlayScreenY);
        }
    } else {
        // Use screen-relative positioning
        GetRelativeCoordsForImage(config.relativeTo, config.x, config.y, displayW, displayH, screenWidth, screenHeight, overlayScreenX,
                                  overlayScreenY);
    }

    // Calculate relative position within the overlay (0.0 to 1.0)
    float relX = static_cast<float>(screenX - overlayScreenX) / displayW;
    float relY = static_cast<float>(screenY - overlayScreenY) / displayH;

    // Clamp to valid range
    relX = std::max(0.0f, std::min(1.0f, relX));
    relY = std::max(0.0f, std::min(1.0f, relY));

    // Map to the cropped region of the original window
    // The cropped region starts at (crop_left, crop_top) in window coords
    outX = config.crop_left + static_cast<int>(relX * croppedWidth);
    outY = config.crop_top + static_cast<int>(relY * croppedHeight);

    return true;
}

// Focus a window overlay for interaction
void FocusWindowOverlay(const std::string& overlayName) {
    HWND targetHwnd = GetWindowOverlayHWND(overlayName);

    std::lock_guard<std::mutex> lock(g_focusedWindowOverlayMutex);
    g_focusedWindowOverlayName = overlayName;
    g_windowOverlayInteractionActive.store(true);
    Log("[WindowOverlay] Focused overlay for interaction: " + overlayName);

    // Notify target window that it's gaining focus for input
    if (targetHwnd && IsWindow(targetHwnd)) {
        // Send WM_SETFOCUS to the target window to activate text boxes, etc.
        PostMessage(targetHwnd, WM_SETFOCUS, 0, 0);
        // Also send WM_ACTIVATE to make it think it's the active window
        PostMessage(targetHwnd, WM_ACTIVATE, WA_ACTIVE, 0);
    }
}

// Unfocus the current window overlay
void UnfocusWindowOverlay() {
    std::string overlayToUnfocus;
    {
        std::lock_guard<std::mutex> lock(g_focusedWindowOverlayMutex);
        if (!g_focusedWindowOverlayName.empty()) {
            Log("[WindowOverlay] Unfocused overlay: " + g_focusedWindowOverlayName);
            overlayToUnfocus = g_focusedWindowOverlayName;
        }
        g_focusedWindowOverlayName = "";
        g_windowOverlayInteractionActive.store(false);
    }

    // Get HWND outside the lock to avoid deadlock (GetWindowOverlayHWND acquires g_windowOverlayCacheMutex)
    if (!overlayToUnfocus.empty()) {
        HWND targetHwnd = GetWindowOverlayHWND(overlayToUnfocus);
        // Notify target window that it's losing focus - this makes text boxes lose selection, etc.
        if (targetHwnd && IsWindow(targetHwnd)) {
            // Send WM_KILLFOCUS to deactivate focused controls in target window
            PostMessage(targetHwnd, WM_KILLFOCUS, 0, 0);
            // Send WM_ACTIVATE with WA_INACTIVE to deactivate the window
            PostMessage(targetHwnd, WM_ACTIVATE, WA_INACTIVE, 0);
        }
    }
}

// Check if any window overlay is currently focused
bool IsWindowOverlayFocused() { return g_windowOverlayInteractionActive.load(); }

// Get the name of the currently focused window overlay
std::string GetFocusedWindowOverlayName() {
    std::lock_guard<std::mutex> lock(g_focusedWindowOverlayMutex);
    return g_focusedWindowOverlayName;
}

// Forward a mouse message to the focused window overlay
bool ForwardMouseToWindowOverlay(UINT uMsg, int screenX, int screenY, WPARAM wParam, int screenWidth, int screenHeight) {
    if (!g_windowOverlayInteractionActive.load()) return false;

    std::string overlayName;
    {
        std::lock_guard<std::mutex> lock(g_focusedWindowOverlayMutex);
        overlayName = g_focusedWindowOverlayName;
    }

    if (overlayName.empty()) return false;

    // Get the target window handle
    HWND targetHwnd = GetWindowOverlayHWND(overlayName);
    if (!targetHwnd || !IsWindow(targetHwnd)) {
        UnfocusWindowOverlay();
        return false;
    }

    // Handle WM_MOUSEWHEEL and WM_MOUSEHWHEEL specially
    if (uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL) {
        // For scroll messages, translate the coordinates to the target window
        int windowX, windowY;
        if (!TranslateToWindowOverlayCoords(overlayName, screenX, screenY, screenWidth, screenHeight, windowX, windowY)) {
            // If translation fails, just use center of the target window as fallback
            RECT clientRect;
            if (GetClientRect(targetHwnd, &clientRect)) {
                windowX = (clientRect.right - clientRect.left) / 2;
                windowY = (clientRect.bottom - clientRect.top) / 2;
            } else {
                windowX = 0;
                windowY = 0;
            }
        }

        // Convert window client coords to screen coords for the wheel message
        POINT targetScreenPos = { windowX, windowY };
        ClientToScreen(targetHwnd, &targetScreenPos);

        // For wheel messages, lParam contains screen coordinates
        LPARAM wheelLParam = MAKELPARAM(targetScreenPos.x, targetScreenPos.y);

        // Use SendMessage for more reliable wheel delivery (some apps don't handle posted wheel messages)
        SendMessage(targetHwnd, uMsg, wParam, wheelLParam);
        return true;
    }

    // Translate screen coords to window coords for non-wheel messages
    int windowX, windowY;
    if (!TranslateToWindowOverlayCoords(overlayName, screenX, screenY, screenWidth, screenHeight, windowX, windowY)) { return false; }

    // Convert WM_MOUSE* to appropriate message for the target window (client coordinates)
    LPARAM lParam = MAKELPARAM(windowX, windowY);

    // Post the message to the target window
    PostMessage(targetHwnd, uMsg, wParam, lParam);

    return true;
}

// Forward a keyboard message to the focused window overlay
bool ForwardKeyboardToWindowOverlay(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (!g_windowOverlayInteractionActive.load()) return false;

    std::string overlayName;
    {
        std::lock_guard<std::mutex> lock(g_focusedWindowOverlayMutex);
        overlayName = g_focusedWindowOverlayName;
    }

    if (overlayName.empty()) return false;

    // Check for Escape to unfocus
    if (uMsg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        UnfocusWindowOverlay();
        return true; // Consume the Escape key
    }

    // Get the target window handle
    HWND targetHwnd = GetWindowOverlayHWND(overlayName);
    if (!targetHwnd || !IsWindow(targetHwnd)) {
        UnfocusWindowOverlay();
        return false;
    }

    // Post the keyboard message to the target window
    PostMessage(targetHwnd, uMsg, wParam, lParam);

    // For WM_KEYDOWN, synthesize WM_CHAR for special control keys that need it.
    // NOTE: We only do this for Enter, Tab, and Backspace - NOT for regular characters.
    // Regular printable characters (A-Z, 0-9, etc.) will get WM_CHAR generated by the
    // target window's message loop calling TranslateMessage() on our posted WM_KEYDOWN.
    // If we also sent WM_CHAR for those, they'd type twice.
    // But Enter/Tab/Backspace need explicit WM_CHAR for proper behavior (e.g., Enter to send a message).
    if (uMsg == WM_KEYDOWN) {
        WCHAR charCode = 0;

        // Only synthesize WM_CHAR for special control keys
        if (wParam == VK_RETURN) {
            charCode = '\r'; // Carriage return - needed for "send message" behavior
        } else if (wParam == VK_TAB) {
            charCode = '\t'; // Tab - for tab navigation
        } else if (wParam == VK_BACK) {
            charCode = '\b'; // Backspace - for character deletion
        }
        // Note: Regular printable characters are intentionally NOT handled here
        // They get WM_CHAR from the target window's TranslateMessage() call

        if (charCode != 0) {
            // Post WM_CHAR with the character code
            // lParam for WM_CHAR should have the same repeat count and scan code as WM_KEYDOWN
            PostMessage(targetHwnd, WM_CHAR, charCode, lParam);
        }
    }

    return true;
}

// Window enumeration callback
// Helper function to get executable name from HWND
std::string GetExecutableNameFromWindow(HWND hwnd) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);

    if (processId == 0) { return ""; }

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!hProcess) { return ""; }

    char exePath[MAX_PATH];
    DWORD size = MAX_PATH;
    bool success = QueryFullProcessImageNameA(hProcess, 0, exePath, &size);
    CloseHandle(hProcess);

    if (success && size > 0) {
        // Extract just the filename from the full path (optimized with reverse search)
        const char* fileName = exePath;
        for (const char* p = exePath + size - 1; p >= exePath; --p) {
            if (*p == '\\' || *p == '/') {
                fileName = p + 1;
                break;
            }
        }
        return std::string(fileName);
    }

    return "";
}

BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    std::vector<WindowInfo>* windows = reinterpret_cast<std::vector<WindowInfo>*>(lParam);

    // Skip invisible windows
    if (!IsWindowVisible(hwnd)) { return TRUE; }

    // Skip windows without titles (unless they have interesting classes)
    char windowTitle[256] = { 0 };
    GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle) - 1);

    char windowClass[256] = { 0 };
    GetClassNameA(hwnd, windowClass, sizeof(windowClass) - 1);

    std::string title = windowTitle;
    std::string className = windowClass;
    std::string executableName = GetExecutableNameFromWindow(hwnd);

    // Hardcoded list of executables to exclude from window capture dropdown
    static const std::vector<std::string> excludedExecutables = { "TextInputHost.exe", "RazerAppEngine.exe" };

    // Skip excluded executables
    for (const auto& excluded : excludedExecutables) {
        if (executableName == excluded) { return TRUE; }
    }

    // Skip empty titles unless it's a known interesting class
    if (title.empty() && className.find("Chrome") == std::string::npos && className.find("Firefox") == std::string::npos &&
        className.find("Notepad") == std::string::npos) {
        return TRUE;
    }

    // Skip desktop and shell windows
    if (className == "Shell_TrayWnd" || className == "Progman" || className == "WorkerW" || className == "DV2ControlHost") { return TRUE; }

    WindowInfo info;
    info.title = title;
    info.className = className;
    info.executableName = executableName;
    info.hwnd = hwnd;

    windows->push_back(info);

    return TRUE;
}

// Get list of currently open windows
std::vector<WindowInfo> GetCurrentlyOpenWindows() {
    std::vector<WindowInfo> windows;
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&windows));

    // Sort by title for better user experience
    std::sort(windows.begin(), windows.end(),
              [](const WindowInfo& a, const WindowInfo& b) { return a.GetDisplayName() < b.GetDisplayName(); });

    return windows;
}

// Check if window info is still valid
bool IsWindowInfoValid(const WindowInfo& windowInfo) { return IsWindow(windowInfo.hwnd) && IsWindowVisible(windowInfo.hwnd); }

// Background capture thread function
void WindowCaptureThreadFunc() {
    _set_se_translator(SEHTranslator);

    try {
        Log("Window capture thread started");

        // Initialize window overlays on the background thread (avoids blocking render thread)
        // This is safe here because the window capture thread runs independently
        if (!g_windowOverlaysInitialized.load()) {
            Log("Initializing window overlays from capture thread");
            InitializeWindowOverlays();
            g_windowOverlaysInitialized.store(true);
        }

        auto lastWindowUpdateCheck = std::chrono::steady_clock::now();
        const auto windowUpdateInterval = std::chrono::seconds(5); // Check for window changes every 5 seconds

        auto lastWindowListUpdate = std::chrono::steady_clock::now();
        const auto windowListUpdateInterval = std::chrono::milliseconds(500); // Update window list cache every 500ms

        while (!g_stopWindowCaptureThread) {
            try {
                auto now = std::chrono::steady_clock::now();

                // Periodically update window handles in case windows were closed/reopened
                if (now - lastWindowUpdateCheck >= windowUpdateInterval) {
                    UpdateAllWindowOverlays();
                    lastWindowUpdateCheck = now;
                }

                // Periodically refresh the window list cache for the GUI (non-blocking window enumeration)
                if (now - lastWindowListUpdate >= windowListUpdateInterval) {
                    auto newWindowList = std::make_unique<std::vector<WindowInfo>>();
                    *newWindowList = GetCurrentlyOpenWindows(); // Expensive call on background thread

                    {
                        std::lock_guard<std::mutex> lock(g_windowListCacheMutex);
                        auto oldList = g_windowListCache.exchange(newWindowList.release());
                        if (oldList) {
                            delete oldList; // Clean up old cache
                        }
                        g_lastWindowListUpdate = now;
                    }
                    lastWindowListUpdate = now;
                }

                // Process deferred overlay reloads (from GUI thread)
                {
                    std::vector<DeferredOverlayReload> reloadsToProcess;
                    {
                        std::lock_guard<std::mutex> lock(g_deferredOverlayReloadsMutex);
                        reloadsToProcess = std::move(g_deferredOverlayReloads);
                        g_deferredOverlayReloads.clear();
                    }

                    // Process reloads without holding the deferred queue lock
                    for (const auto& reload : reloadsToProcess) {
                        try {
                            LoadWindowOverlay(reload.overlayId, reload.config);
                            Log("Processed deferred reload for overlay: " + reload.overlayId);
                        } catch (const std::exception& e) {
                            Log("Error processing deferred reload for overlay '" + reload.overlayId + "': " + e.what());
                        }
                    }
                }

                // Build a list of overlay IDs and configs to capture (safer than holding pointers)
                std::vector<std::pair<std::string, WindowOverlayConfig>> overlaysToCapture;
                {
                    // Use snapshot for thread-safe config access + cache lock for cache access
                    auto captureSnap = GetConfigSnapshot();
                    std::lock_guard<std::mutex> cacheLock(g_windowOverlayCacheMutex);

                    const size_t cacheSize = g_windowOverlayCache.size();
                    overlaysToCapture.reserve(cacheSize);

                    for (const auto& [overlayId, entry] : g_windowOverlayCache) {
                        // Find config for this overlay from snapshot
                        const WindowOverlayConfig* config = captureSnap ? FindWindowOverlayConfigIn(overlayId, *captureSnap) : nullptr;
                        if (config) { overlaysToCapture.emplace_back(overlayId, *config); }
                    }
                }
                // Locks released - now we can capture without blocking config/cache changes

                // Process each overlay
                for (auto& [overlayId, config] : overlaysToCapture) {
                    if (g_stopWindowCaptureThread) { break; }

                    try {
                        // Get entry pointer with minimal lock duration
                        WindowOverlayCacheEntry* entry = nullptr;
                        {
                            std::lock_guard<std::mutex> cacheLock(g_windowOverlayCacheMutex);
                            auto it = g_windowOverlayCache.find(overlayId);
                            if (it != g_windowOverlayCache.end()) { entry = it->second.get(); }
                        }
                        // Lock released - now we can capture without blocking GUI thread

                        if (entry) {
                            // Capture without holding the cache mutex
                            // The entry's own captureMutex protects against concurrent modifications
                            // Note: Entry deletion is only done from GUI thread via RemoveWindowOverlayFromCache,
                            // and the capture thread checks for null before each capture
                            CaptureWindowContent(*entry, config);
                        }
                        // If entry was removed, skip this overlay (it was deleted from config)
                    } catch (const std::exception& e) {
                        Log("Error capturing window content for overlay '" + overlayId + "': " + e.what());
                    } catch (...) { Log("Unknown error capturing window content for overlay '" + overlayId + "'"); }
                }

                // Sleep for a short time to prevent excessive CPU usage
                std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS max
            } catch (const std::exception& e) { Log("Error in window capture thread: " + std::string(e.what())); } catch (...) {
                Log("Unknown error in window capture thread");
            }
        }
    } catch (const SE_Exception& e) {
        LogException("WindowCaptureThreadFunc (SEH)", e.getCode(), e.getInfo());
    } catch (const std::exception& e) { LogException("WindowCaptureThreadFunc", e); } catch (...) {
        Log("EXCEPTION in WindowCaptureThreadFunc: Unknown exception");
    }

    Log("Window capture thread stopped");
}

// Start background capture thread
void StartWindowCaptureThread() {
    if (!g_windowCaptureThread.joinable()) {
        g_stopWindowCaptureThread = false;
        g_windowCaptureThread = std::thread(WindowCaptureThreadFunc);
        Log("Started window capture background thread");
    }
}

// Stop background capture thread
void StopWindowCaptureThread() {
    if (g_windowCaptureThread.joinable()) {
        Log("Stopping window capture thread...");
        g_stopWindowCaptureThread = true;

        // Wait for thread to finish with timeout protection
        // Note: std::thread::join() will block until thread exits
        try {
            g_windowCaptureThread.join();
            Log("Window capture thread stopped cleanly");
        } catch (const std::system_error& e) { Log("Exception while joining window capture thread: " + std::string(e.what())); }
    }
}

// Get cached window list for GUI (non-blocking)
// Returns a copy of the cached list or empty vector if not available
std::vector<WindowInfo> GetCachedWindowList() {
    std::lock_guard<std::mutex> lock(g_windowListCacheMutex);
    if (auto* cachedList = g_windowListCache.load()) {
        return *cachedList; // Return a copy
    }
    return std::vector<WindowInfo>(); // Return empty list if cache not ready
}
