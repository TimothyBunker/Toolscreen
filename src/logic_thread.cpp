#include "logic_thread.h"
#include "expression_parser.h"
#include "gui.h"
#include "mirror_thread.h"
#include "profiler.h"
#include "render.h"
#include "utils.h"
#include "version.h"
#include <Windows.h>
#include <thread>

std::atomic<bool> g_logicThreadRunning{ false };
static std::thread g_logicThread;
static std::atomic<bool> g_logicThreadShouldStop{ false };

extern std::atomic<bool> g_graphicsHookDetected;
extern std::atomic<HMODULE> g_graphicsHookModule;
extern std::chrono::steady_clock::time_point g_lastGraphicsHookCheck;
extern const int GRAPHICS_HOOK_CHECK_INTERVAL_MS;

extern std::atomic<HWND> g_minecraftHwnd;
extern std::atomic<bool> g_configLoaded;
extern Config g_config;

extern std::string g_gameStateBuffers[2];
extern std::atomic<int> g_currentGameStateIndex;

extern std::atomic<bool> g_windowsMouseSpeedApplied;
extern int g_originalWindowsMouseSpeed;

extern std::atomic<bool> g_isShuttingDown;

extern PendingModeSwitch g_pendingModeSwitch;
extern std::mutex g_pendingModeSwitchMutex;

extern PendingDimensionChange g_pendingDimensionChange;
extern std::mutex g_pendingDimensionChangeMutex;

extern GameVersion g_gameVersion;

// Forward declarations for functions in dllmain.cpp
void ApplyWindowsMouseSpeed();

// Double-buffered viewport cache for lock-free access by hkglViewport
CachedModeViewport g_viewportModeCache[2];
std::atomic<int> g_viewportModeCacheIndex{ 0 };
static std::string s_lastCachedModeId; // Track which mode ID is cached

static bool s_wasInWorld = false;
static int s_lastAppliedWindowsMouseSpeed = -1;
static std::string s_previousGameStateForReset = "init";

static std::atomic<int> s_cachedScreenWidth{ 0 };
static std::atomic<int> s_cachedScreenHeight{ 0 };

// Tracked for UpdateActiveMirrorConfigs - detect when active mirrors change
static std::vector<std::string> s_lastActiveMirrorIds;

// Update mirror capture configs when active mirrors change (mode switch or config edit)
// This was previously done on every frame in RenderModeInternal - now only when needed
void UpdateActiveMirrorConfigs() {
    PROFILE_SCOPE_CAT("LT Mirror Configs", "Logic Thread");

    // Use config snapshot for thread-safe access to modes/mirrors/mirrorGroups
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return;
    const Config& cfg = *cfgSnap;

    // Get current mode ID from double-buffer (lock-free)
    std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
    const ModeConfig* mode = GetModeFromSnapshot(cfg, currentModeId);
    if (!mode) { return; }

    // Collect all mirror IDs from both direct mirrors and mirror groups
    std::vector<std::string> currentMirrorIds = mode->mirrorIds;
    for (const auto& groupName : mode->mirrorGroupIds) {
        for (const auto& group : cfg.mirrorGroups) {
            if (group.name == groupName) {
                for (const auto& item : group.mirrors) {
                    if (std::find(currentMirrorIds.begin(), currentMirrorIds.end(), item.mirrorId) == currentMirrorIds.end()) {
                        currentMirrorIds.push_back(item.mirrorId);
                    }
                }
                break;
            }
        }
    }

    // Only update if the list of active mirrors changed
    if (currentMirrorIds != s_lastActiveMirrorIds) {
        // Collect MirrorConfig objects for UpdateMirrorCaptureConfigs
        std::vector<MirrorConfig> activeMirrorsForCapture;
        activeMirrorsForCapture.reserve(currentMirrorIds.size());
        for (const auto& mirrorId : currentMirrorIds) {
            for (const auto& mirror : cfg.mirrors) {
                if (mirror.name == mirrorId) {
                    MirrorConfig activeMirror = mirror;

                    // Check if this mirror is part of a group in the current mode
                    // If so, apply the group's output settings (position + per-item sizing)
                    for (const auto& groupName : mode->mirrorGroupIds) {
                        for (const auto& group : cfg.mirrorGroups) {
                            if (group.name == groupName) {
                                // Check if this mirror is in this group
                                for (const auto& item : group.mirrors) {
                                    if (!item.enabled) continue; // Skip disabled items
                                    if (item.mirrorId == mirrorId) {
                                        // Calculate group position - use relative percentages if enabled
                                        int groupX = group.output.x;
                                        int groupY = group.output.y;
                                        if (group.output.useRelativePosition) {
                                            int screenW = GetCachedScreenWidth();
                                            int screenH = GetCachedScreenHeight();
                                            groupX = static_cast<int>(group.output.relativeX * screenW);
                                            groupY = static_cast<int>(group.output.relativeY * screenH);
                                        }
                                        // Position from group + per-item offset
                                        activeMirror.output.x = groupX + item.offsetX;
                                        activeMirror.output.y = groupY + item.offsetY;
                                        activeMirror.output.relativeTo = group.output.relativeTo;
                                        activeMirror.output.useRelativePosition = group.output.useRelativePosition;
                                        activeMirror.output.relativeX = group.output.relativeX;
                                        activeMirror.output.relativeY = group.output.relativeY;
                                        // Per-item sizing (multiply mirror scale by item percentages)
                                        if (item.widthPercent != 1.0f || item.heightPercent != 1.0f) {
                                            activeMirror.output.separateScale = true;
                                            float baseScaleX = mirror.output.separateScale ? mirror.output.scaleX : mirror.output.scale;
                                            float baseScaleY = mirror.output.separateScale ? mirror.output.scaleY : mirror.output.scale;
                                            activeMirror.output.scaleX = baseScaleX * item.widthPercent;
                                            activeMirror.output.scaleY = baseScaleY * item.heightPercent;
                                        }
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                    }

                    activeMirrorsForCapture.push_back(activeMirror);
                    break;
                }
            }
        }
        UpdateMirrorCaptureConfigs(activeMirrorsForCapture);
        s_lastActiveMirrorIds = currentMirrorIds;
    }
}

void UpdateCachedScreenMetrics() {
    PROFILE_SCOPE_CAT("LT Screen Metrics", "Logic Thread");

    // Store previous values to detect changes
    int prevWidth = s_cachedScreenWidth.load(std::memory_order_relaxed);
    int prevHeight = s_cachedScreenHeight.load(std::memory_order_relaxed);

    int newWidth = GetSystemMetrics(SM_CXSCREEN);
    int newHeight = GetSystemMetrics(SM_CYSCREEN);

    s_cachedScreenWidth.store(newWidth, std::memory_order_relaxed);
    s_cachedScreenHeight.store(newHeight, std::memory_order_relaxed);

    // Recalculate expression-based dimensions if screen size changed
    if (prevWidth != 0 && prevHeight != 0 && (prevWidth != newWidth || prevHeight != newHeight)) {
        RecalculateExpressionDimensions();
        // RecalculateExpressionDimensions mutates g_config.modes in-place (width/height/stretch fields).
        // Publish updated snapshot so reader threads see the recalculated dimensions.
        PublishConfigSnapshot();
    }
}

int GetCachedScreenWidth() {
    int w = s_cachedScreenWidth.load(std::memory_order_relaxed);
    if (w == 0) {
        w = GetSystemMetrics(SM_CXSCREEN);
        s_cachedScreenWidth.store(w, std::memory_order_relaxed);
    }
    return w;
}

int GetCachedScreenHeight() {
    int h = s_cachedScreenHeight.load(std::memory_order_relaxed);
    if (h == 0) {
        h = GetSystemMetrics(SM_CYSCREEN);
        s_cachedScreenHeight.store(h, std::memory_order_relaxed);
    }
    return h;
}

void UpdateCachedViewportMode() {
    PROFILE_SCOPE_CAT("LT Viewport Cache", "Logic Thread");

    // Read current mode ID from double-buffer (lock-free)
    std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];

    // Always update cache when GUI is open (user may be editing width/height/x/y)
    // Also force periodic refresh every 60 ticks (~1 second) as a safety net
    static int s_ticksSinceRefresh = 0;
    bool guiOpen = g_showGui.load(std::memory_order_relaxed);
    bool periodicRefresh = (++s_ticksSinceRefresh >= 60);

    if (currentModeId == s_lastCachedModeId && !guiOpen && !periodicRefresh) { return; }

    if (periodicRefresh) { s_ticksSinceRefresh = 0; }

    // Get mode data via config snapshot (thread-safe, lock-free)
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return; // Config not yet published
    const ModeConfig* mode = GetModeFromSnapshot(*cfgSnap, currentModeId);

    // Write to inactive buffer
    int nextIndex = 1 - g_viewportModeCacheIndex.load(std::memory_order_relaxed);
    CachedModeViewport& cache = g_viewportModeCache[nextIndex];

    if (mode) {
        cache.width = mode->width;
        cache.height = mode->height;
        cache.stretchEnabled = mode->stretch.enabled;
        cache.stretchX = mode->stretch.x;
        cache.stretchY = mode->stretch.y;
        cache.stretchWidth = mode->stretch.width;
        cache.stretchHeight = mode->stretch.height;
        cache.valid = true;
    } else {
        cache.valid = false;
    }

    // Atomic swap to make new cache visible
    g_viewportModeCacheIndex.store(nextIndex, std::memory_order_release);
    s_lastCachedModeId = currentModeId;
}

void PollObsGraphicsHook() {
    PROFILE_SCOPE_CAT("LT OBS Hook Poll", "Logic Thread");
    auto now = std::chrono::steady_clock::now();
    auto msSinceLastCheck = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastGraphicsHookCheck).count();

    if (msSinceLastCheck >= GRAPHICS_HOOK_CHECK_INTERVAL_MS) {
        g_lastGraphicsHookCheck = now;
        HMODULE hookModule = GetModuleHandleA("graphics-hook64.dll");
        bool wasDetected = g_graphicsHookDetected.load();
        bool nowDetected = (hookModule != NULL);

        if (nowDetected != wasDetected) {
            g_graphicsHookDetected.store(nowDetected);
            g_graphicsHookModule.store(hookModule);
            if (nowDetected) {
                Log("[OBS] graphics-hook64.dll DETECTED - OBS overlay active");
            } else {
                Log("[OBS] graphics-hook64.dll UNLOADED - OBS overlay inactive");
            }
        }
    }
}

void CheckWorldExitReset() {
    PROFILE_SCOPE_CAT("LT World Exit Check", "Logic Thread");

    // Get current game state from lock-free buffer
    std::string currentGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
    bool isInWorld = (currentGameState.find("inworld") != std::string::npos);

    // Transitioning from "in world" to "not in world" - reset all secondary modes
    if (s_wasInWorld && !isInWorld) {
        auto cfgSnap = GetConfigSnapshot();
        if (cfgSnap) {
            const Config& cfg = *cfgSnap;
            for (size_t i = 0; i < cfg.hotkeys.size(); ++i) {
                const auto& hotkey = cfg.hotkeys[i];
                // Only reset if this hotkey has a secondary mode configured
                if (!hotkey.secondaryMode.empty() && GetHotkeySecondaryMode(i) != hotkey.secondaryMode) {
                    SetHotkeySecondaryMode(i, hotkey.secondaryMode);
                    Log("[Hotkey] Reset secondary mode for hotkey to: " + hotkey.secondaryMode);
                }
            }
        }
    }
    s_wasInWorld = isInWorld;
}

void CheckWindowsMouseSpeedChange() {
    PROFILE_SCOPE_CAT("LT Mouse Speed Check", "Logic Thread");
    auto cfgSnap = GetConfigSnapshot();
    int currentWindowsMouseSpeed = cfgSnap ? cfgSnap->windowsMouseSpeed : 0;
    if (currentWindowsMouseSpeed != s_lastAppliedWindowsMouseSpeed) {
        ApplyWindowsMouseSpeed();
        s_lastAppliedWindowsMouseSpeed = currentWindowsMouseSpeed;
    }
}

void ProcessPendingModeSwitch() {
    PROFILE_SCOPE_CAT("LT Mode Switch", "Logic Thread");
    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
    if (!g_pendingModeSwitch.pending) { return; }

    if (g_pendingModeSwitch.isPreview && !g_pendingModeSwitch.previewFromModeId.empty()) {
        // Preview mode: first switch to the "from" mode instantly (with Cut transition)
        Log("[GUI] Processing preview mode switch: " + g_pendingModeSwitch.previewFromModeId + " -> " + g_pendingModeSwitch.modeId);

        std::string fromModeId = g_pendingModeSwitch.previewFromModeId;
        std::string toModeId = g_pendingModeSwitch.modeId;

        // Switch to "from" mode instantly using forceCut (no g_config mutation needed)
        SwitchToMode(fromModeId, "Preview (instant)", /*forceCut=*/true);

        // Now switch to target mode with its configured transition
        SwitchToMode(toModeId, "Preview (animated)");
    } else {
        // Normal mode switch
        LogCategory("gui", "[GUI] Processing deferred mode switch to: " + g_pendingModeSwitch.modeId +
                               " (source: " + g_pendingModeSwitch.source + ")");

        // Use forceCut parameter instead of temporarily mutating g_config.modes
        // This avoids cross-thread mutation of g_config from the logic thread
        SwitchToMode(g_pendingModeSwitch.modeId, g_pendingModeSwitch.source,
                     /*forceCut=*/g_pendingModeSwitch.forceInstant);
    }

    g_pendingModeSwitch.pending = false;
    g_pendingModeSwitch.isPreview = false;
    g_pendingModeSwitch.forceInstant = false;
    g_pendingModeSwitch.modeId.clear();
    g_pendingModeSwitch.source.clear();
    g_pendingModeSwitch.previewFromModeId.clear();
}

// This processes dimension changes from the GUI (render thread) on the logic thread
// to avoid race conditions between render thread modifying config and game thread reading it
void ProcessPendingDimensionChange() {
    PROFILE_SCOPE_CAT("LT Dimension Change", "Logic Thread");
    std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
    if (!g_pendingDimensionChange.pending) { return; }

    // Find the mode and apply dimension changes
    ModeConfig* mode = GetModeMutable(g_pendingDimensionChange.modeId);
    if (mode) {
        if (g_pendingDimensionChange.newWidth > 0) { mode->width = g_pendingDimensionChange.newWidth; }
        if (g_pendingDimensionChange.newHeight > 0) { mode->height = g_pendingDimensionChange.newHeight; }

        // Post WM_SIZE if requested and this is the current mode
        if (g_pendingDimensionChange.sendWmSize && g_currentModeId == g_pendingDimensionChange.modeId) {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd) { PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(mode->width, mode->height)); }
        }

        g_configIsDirty = true;
    }

    g_pendingDimensionChange.pending = false;
    g_pendingDimensionChange.modeId.clear();
    g_pendingDimensionChange.newWidth = 0;
    g_pendingDimensionChange.newHeight = 0;
    g_pendingDimensionChange.sendWmSize = false;
}

void CheckGameStateReset() {
    PROFILE_SCOPE_CAT("LT Game State Reset", "Logic Thread");

    // Only perform mode switching if resolution changes are supported
    if (!IsResolutionChangeSupported(g_gameVersion)) { return; }

    // Get current game state from lock-free buffer
    std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];

    // Check if transitioning from non-wall/title/waiting to wall/title/waiting
    if (isWallTitleOrWaiting(localGameState) && !isWallTitleOrWaiting(s_previousGameStateForReset)) {
        // Reset all hotkey secondary modes to default
        auto cfgSnap = GetConfigSnapshot();
        if (cfgSnap) {
            const Config& cfg = *cfgSnap;
            for (size_t i = 0; i < cfg.hotkeys.size(); ++i) {
                if (GetHotkeySecondaryMode(i) != cfg.hotkeys[i].secondaryMode) { SetHotkeySecondaryMode(i, cfg.hotkeys[i].secondaryMode); }
            }

            std::string targetMode = cfg.defaultMode;
            Log("[LogicThread] Reset all hotkey secondary modes to default due to wall/title/waiting state.");
            SwitchToMode(targetMode, "game state reset", /*forceCut=*/true);
        }
    }

    s_previousGameStateForReset = localGameState;
}

static void LogicThreadFunc() {
    LogCategory("init", "[LogicThread] Started");

    // Target ~60Hz tick rate (approximately 16.67ms per tick)
    const auto tickInterval = std::chrono::milliseconds(16);

    while (!g_logicThreadShouldStop.load()) {
        PROFILE_SCOPE_CAT("Logic Thread Tick", "Logic Thread");
        auto tickStart = std::chrono::steady_clock::now();

        // Skip all logic if shutting down
        if (g_isShuttingDown.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Skip if config not loaded yet
        if (!g_configLoaded.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Run all logic checks
        UpdateCachedScreenMetrics();
        UpdateCachedViewportMode();
        UpdateActiveMirrorConfigs();
        PollObsGraphicsHook();
        CheckWorldExitReset();
        CheckWindowsMouseSpeedChange();
        ProcessPendingModeSwitch();
        ProcessPendingDimensionChange();
        CheckGameStateReset();

        // Sleep for remaining time in tick
        auto tickEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tickEnd - tickStart);
        if (elapsed < tickInterval) { std::this_thread::sleep_for(tickInterval - elapsed); }
    }

    Log("[LogicThread] Stopped");
}

void StartLogicThread() {
    if (g_logicThreadRunning.load()) {
        Log("[LogicThread] Already running, not starting again");
        return;
    }

    Log("[LogicThread] Starting logic thread...");
    g_logicThreadShouldStop.store(false);

    g_logicThread = std::thread(LogicThreadFunc);
    g_logicThreadRunning.store(true);

    LogCategory("init", "[LogicThread] Logic thread started");
}

void StopLogicThread() {
    if (!g_logicThreadRunning.load()) { return; }

    Log("[LogicThread] Stopping logic thread...");
    g_logicThreadShouldStop.store(true);

    if (g_logicThread.joinable()) { g_logicThread.join(); }

    g_logicThreadRunning.store(false);
    Log("[LogicThread] Logic thread stopped");
}
