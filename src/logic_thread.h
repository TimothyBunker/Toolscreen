#pragma once

#include <atomic>
#include <string>

// Thread runs independently at ~60Hz, handling logic checks that don't require the GL context
// This offloads work from the game's render thread (SwapBuffers hook)

// Pre-computed viewport mode data, updated by logic_thread when mode changes
// Used by hkglViewport to avoid GetMode() lookup on every call
struct CachedModeViewport {
    int width = 0;
    int height = 0;
    bool stretchEnabled = false;
    int stretchX = 0;
    int stretchY = 0;
    int stretchWidth = 0;
    int stretchHeight = 0;
    bool valid = false; // True if mode was found and data is valid
};

// Double-buffered viewport cache for lock-free access
// Logic thread writes, game thread (hkglViewport) reads
extern CachedModeViewport g_viewportModeCache[2];
extern std::atomic<int> g_viewportModeCacheIndex;

// Update the cached viewport mode data (called by logic_thread when mode changes)
void UpdateCachedViewportMode();

extern std::atomic<bool> g_logicThreadRunning;

// Start the logic thread (call after config is loaded and HWND is known)
void StartLogicThread();

// Stop the logic thread (call before DLL unload)
void StopLogicThread();

// These are updated by the logic thread and read by the render thread
// Already declared in dllmain.cpp as:
//   extern std::atomic<bool> g_graphicsHookDetected;
//   extern std::atomic<HMODULE> g_graphicsHookModule;

// These can be called manually for testing or to force immediate updates

// Poll for OBS graphics-hook64.dll presence
// Updates g_graphicsHookDetected and g_graphicsHookModule
void PollObsGraphicsHook();

// Check if player exited world and reset hotkey secondary modes
// Parses window title looking for '-' character
void CheckWorldExitReset();

// Apply Windows mouse speed setting if config changed
void CheckWindowsMouseSpeedChange();

// Process any pending mode switch requests
// This handles deferred switches from GUI or hotkeys
void ProcessPendingModeSwitch();

// Check for game state transition (inworld -> wall/title/waiting) and reset to default mode
// This handles the automatic mode reset when leaving a world
void CheckGameStateReset();

// Returns cached primary monitor dimensions, updated at ~60Hz by logic thread
// Safe to call from any thread without locking
int GetCachedScreenWidth();
int GetCachedScreenHeight();
