#pragma once

#include <atomic>
#include <string>
#include <vector>

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

struct StrongholdOverlayRenderSnapshot {
    bool enabled = false;
    bool visible = false;
    bool apiOnline = false;
    bool hasPlayerSnapshot = false;
    bool hasPrediction = false;
    bool targetLocked = false;
    bool lockWasAuto = false;
    bool blockAutoLockUntilThrowClear = false;
    bool showDirectionArrow = true;
    bool showEstimateValues = true;
    bool showAlignmentText = true;
    bool renderInGameOverlay = true;
    bool renderCompanionOverlay = true;
    bool boatModeEnabled = true;
    bool preferNetherCoords = true;
    bool usingNetherCoords = true;
    bool usingLiveTarget = true;
    bool mcsrSafeMode = false;
    int hudLayoutMode = 2; // 0=full, 2=speedrun (1=legacy compact alias -> speedrun)
    int renderMonitorMode = 0; // 0=all monitors, 1=selected monitor(s)
    unsigned long long renderMonitorMask = ~0ull;
    float overlayOpacity = 1.0f;
    float backgroundOpacity = 0.55f;
    float scale = 1.0f;
    int x = 24;
    int y = 24;
    int targetDisplayX = 0;
    int targetDisplayZ = 0;
    int playerDisplayX = 0;
    int playerDisplayZ = 0;
    int targetNetherX = 0;
    int targetNetherZ = 0;
    int estimatedNetherX = 0;
    int estimatedNetherZ = 0;
    int playerNetherX = 0;
    int playerNetherZ = 0;
    int targetOverworldX = 0;
    int targetOverworldZ = 0;
    int estimatedOverworldX = 0;
    int estimatedOverworldZ = 0;
    int playerOverworldX = 0;
    int playerOverworldZ = 0;
    float distanceDisplay = 0.0f;
    float relativeYaw = 0.0f;
    int activeEyeThrowCount = 0;
    float angleAdjustmentDeg = 0.0f;
    float angleAdjustmentStepDeg = 0.01f;
    int lastAdjustmentStepDirection = 0; // -1 red, +1 green, 0 none
    bool hasTopCertainty = false;
    float topCertaintyPercent = 0.0f;
    bool hasCombinedCertainty = false;
    float combinedCertaintyPercent = 0.0f;
    bool hasNextThrowDirection = false;
    int moveLeftBlocks = 0;
    int moveRightBlocks = 0;
    std::string topCandidate1Label;
    std::string topCandidate2Label;
    std::string warningLabel;
    int boatState = 0; // 0=uninitialized(blue), 1=good(green), 2=failed(red)
    std::string boatLabel = "Boat: UNINIT";
    std::string modeLabel = "nether";
    std::string statusLabel = "LIVE/UNLOCKED";
    std::string infoLabel = "No throws yet. Shift+H lock";
    bool showComputedDetails = false;
};

struct McsrApiTrackerRenderSnapshot {
    bool enabled = false;
    bool visible = false;
    bool renderInGameOverlay = true;
    bool apiOnline = false;
    bool refreshOnlyMode = true;
    float scale = 1.0f;
    float overlayOpacity = 1.0f;
    float backgroundOpacity = 0.55f;
    int x = 0;
    int y = 0;
    std::string headerLabel;
    std::string statusLabel;
    std::string displayPlayer;
    std::string requestedPlayer;
    std::string autoDetectedPlayer;
    std::string avatarImagePath;
    std::string flagImagePath;
    std::string country;
    int eloRank = 0;
    int eloRate = 0;
    int peakElo = 0;
    int seasonWins = 0;
    int seasonLosses = 0;
    int seasonCompletions = 0;
    int seasonBestWinStreak = 0;
    int seasonPoints = 0;
    int bestTimeMs = 0;
    int averageResultTimeMs = 0;
    int profileAverageTimeMs = 0;
    int recentWins = 0;
    int recentLosses = 0;
    int recentDraws = 0;
    float recentForfeitRatePercent = 0.0f;
    float profileForfeitRatePercent = 0.0f;
    struct MatchRow {
        std::string opponent;
        std::string resultLabel;
        std::string detailLabel;
        std::string ageLabel;
        int resultType = 0; // 1=win, 0=draw, -1=loss
        bool forfeited = false;
        int categoryType = 0; // 0=ranked, 1=private, 2=casual, 3=event, 4=other
    };
    struct TrendPoint {
        int elo = 0;
        std::string opponent;
        std::string resultLabel;
        std::string detailLabel;
        std::string ageLabel;
    };
    std::vector<int> eloHistory;
    std::vector<TrendPoint> eloTrendPoints;
    std::vector<MatchRow> recentMatches;
    std::vector<std::string> suggestedPlayers;
};

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

// Returns cached monitor dimensions for the monitor the game window is currently on (multi-monitor aware)
// Safe to call from any thread without locking
int GetCachedScreenWidth();
int GetCachedScreenHeight();

// Marks cached screen metrics as dirty so the next refresh re-queries the monitor
// the game window is currently on. Safe to call from any thread.
void InvalidateCachedScreenMetrics();

// ============================================================================
// STRONGHOLD OVERLAY
// ============================================================================

// Called by logic thread to poll NinjaBrainBot API and update overlay state.
void UpdateStrongholdOverlayState();

// Snapshot for render thread drawing.
StrongholdOverlayRenderSnapshot GetStrongholdOverlayRenderSnapshot();
McsrApiTrackerRenderSnapshot GetMcsrApiTrackerRenderSnapshot();
void RequestMcsrApiTrackerRefresh();
void SetMcsrApiTrackerSearchPlayer(const std::string& playerName);
void ClearMcsrApiTrackerSearchPlayer();
bool ShouldAllowMcsrTrackerUiInput();

// Hotkey handlers (called from input hook).
// Returns true when handled and should consume the key event.
bool HandleStrongholdOverlayHotkeyH(bool shiftDown, bool ctrlDown);
bool HandleStrongholdOverlayNumpadHotkey(int virtualKey);
bool HandleMcsrTrackerOverlayToggleHotkey(unsigned int keyVk, bool ctrlDown, bool shiftDown, bool altDown);

// Runtime environment detection helpers.
// MCSR-safe mode is auto-detected from launcher/instance path hints.
bool IsMcsrRankedInstanceDetected();
std::string GetMcsrRankedDetectionSource();

// Live input feed for continuous stronghold guidance between F3+C/API samples.
// Called from raw-input/keyboard hooks.
void ReportStrongholdLiveMouseDelta(int deltaX, int deltaY);
void ReportStrongholdLiveKeyState(int virtualKey, bool isDown);
void ResetStrongholdLiveInputState();
