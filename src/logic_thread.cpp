#include "logic_thread.h"
#include "expression_parser.h"
#include "gui.h"
#include "mirror_thread.h"
#include "profiler.h"
#include "render.h"
#include "stronghold_companion_overlay.h"
#include "utils.h"
#include "version.h"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <winhttp.h>

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
extern std::atomic<bool> g_showGui;
extern std::atomic<bool> g_gameWindowActive;

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

// Screen-metrics refresh coordination
// - Dirty flag is set by window-move/resize messages to force immediate refresh.
// - Periodic refresh is a safety net in case move messages are missed.
// - If another thread detects a size change and updates the cache, it requests
//   an expression-dimension recalculation which MUST occur on the logic thread.
static std::atomic<bool> s_screenMetricsDirty{ true };
static std::atomic<bool> s_screenMetricsRecalcRequested{ false };
static std::atomic<ULONGLONG> s_lastScreenMetricsRefreshMs{ 0 };

namespace {
constexpr wchar_t kStrongholdApiHost[] = L"127.0.0.1";
constexpr INTERNET_PORT kStrongholdApiPort = 52533;
constexpr wchar_t kStrongholdApiPath[] = L"/api/v1/stronghold";
constexpr wchar_t kInformationMessagesApiPath[] = L"/api/v1/information-messages";
constexpr DWORD kStrongholdApiTimeoutMs = 250;
constexpr double kPi = 3.14159265358979323846;
constexpr double kDefaultSigmaNormal = 0.1;
constexpr double kDefaultSigmaAlt = 0.1;
constexpr double kDefaultSigmaManual = 0.03;
constexpr double kDefaultSigmaBoat = 0.001;
constexpr int kStrongholdSnappingRadius = 7;
constexpr int kStrongholdRingCount = 8;
constexpr int kStrongholdCount = 128;
constexpr int kStrongholdDistParam = 32;
constexpr int kStrongholdMaxChunk = static_cast<int>(
    kStrongholdDistParam * ((4.0 + (kStrongholdRingCount - 1) * 6.0) + 0.5 * 2.5) + 2 * kStrongholdSnappingRadius + 1);
constexpr double kBoatInitErrorLimitDeg = 0.03;
constexpr double kBoatInitPositiveIncrementDeg = 1.40625;
constexpr double kBoatInitNegativeIncrementDeg = 0.140625;
constexpr double kNbbDefaultSensitivityAutomatic = 0.012727597;
constexpr double kNbbDefaultCrosshairCorrectionDeg = 0.0;
constexpr wchar_t kNbbPrefsRegistrySubkey[] = L"Software\\JavaSoft\\Prefs\\ninjabrainbot";
constexpr wchar_t kNbbSensitivityRegistryValue[] = L"sensitivity";
constexpr wchar_t kNbbCrosshairCorrectionRegistryValue[] = L"crosshair_correction";
constexpr wchar_t kNbbSigmaRegistryValue[] = L"sigma";
constexpr wchar_t kNbbSigmaAltRegistryValue[] = L"sigma_alt";
constexpr wchar_t kNbbSigmaManualRegistryValue[] = L"sigma_manual";
constexpr wchar_t kNbbSigmaBoatRegistryValue[] = L"sigma_boat";
constexpr wchar_t kNbbAngleAdjustmentTypeRegistryValue[] = L"angle_adjustment_type";
constexpr wchar_t kNbbResolutionHeightRegistryValue[] = L"resolution_height";
constexpr wchar_t kNbbCustomAdjustmentRegistryValue[] = L"custom_adjustment";
constexpr ULONGLONG kNbbPrefsRefreshIntervalMs = 5000;
constexpr int kBoatStateUninitialized = 0;
constexpr int kBoatStateGood = 1;
constexpr int kBoatStateFailed = 2;
// Match NBB ChunkPrediction#success threshold (> 0.0005).
constexpr double kNbbMinimumSuccessfulPosteriorWeight = 0.0005;
constexpr double kMinecraftWalkSpeedBlocksPerSecond = 4.317;
constexpr double kMinecraftSprintSpeedBlocksPerSecond = 5.612;
constexpr double kMinecraftSneakSpeedBlocksPerSecond = 1.295;

constexpr uint32_t kMoveKeyForward = 1u << 0;
constexpr uint32_t kMoveKeyBack = 1u << 1;
constexpr uint32_t kMoveKeyLeft = 1u << 2;
constexpr uint32_t kMoveKeyRight = 1u << 3;
constexpr uint32_t kMoveKeySprint = 1u << 4;
constexpr uint32_t kMoveKeySneak = 1u << 5;

enum class EyeThrowType {
    Normal,
    NormalWithAltStd,
    Manual,
    Boat,
    Unknown
};

struct ParsedEyeThrow {
    double xInOverworld = 0.0;
    double zInOverworld = 0.0;
    double angleDeg = 0.0;
    double verticalAngleDeg = -31.6;
    EyeThrowType type = EyeThrowType::Unknown;
};

struct ParsedPrediction {
    int chunkX = 0;
    int chunkZ = 0;
    double certainty = 0.0;
};

struct ParsedStrongholdApiData {
    bool ok = false;
    double playerX = 0.0;
    double playerZ = 0.0;
    double playerYaw = 0.0;
    bool isInOverworld = true;
    bool isInNether = false;
    int eyeThrowCount = 0;
    bool hasBoatThrow = false;
    std::vector<ParsedEyeThrow> eyeThrows;
    std::vector<ParsedPrediction> predictions;
    bool hasPrediction = false;
    int chunkX = 0;
    int chunkZ = 0;
    bool hasTopCertainty = false;
    double topCertaintyPercent = 0.0;
    bool hasNativeTriangulation = false;
    int nativeChunkX = 0;
    int nativeChunkZ = 0;
};

struct ParsedInformationMessagesData {
    bool ok = false;
    bool hasCombinedCertainty = false;
    double combinedCertaintyPercent = 0.0;
    bool hasNextThrowDirection = false;
    int moveLeftBlocks = 0;
    int moveRightBlocks = 0;
    bool hasMismeasureWarning = false;
    std::string mismeasureWarningText;
};

enum class ClipboardDimension {
    Overworld,
    Nether,
    End,
    Unknown
};

struct ParsedF3CClipboardData {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double horizontalAngle = 0.0;
    double verticalAngle = 0.0;
    ClipboardDimension dimension = ClipboardDimension::Unknown;
};

struct StrongholdRingInfo {
    int strongholdsInRing = 0;
    int ringIndex = 0;
    double innerRadius = 0.0;
    double outerRadius = 0.0;
    double innerRadiusPostSnapping = 0.0;
    double outerRadiusPostSnapping = 0.0;
};

struct StandaloneStrongholdState {
    std::string lastClipboardText;
    DWORD lastClipboardSequenceNumber = 0;
    uint64_t parsedSnapshotCounter = 0;
    bool hasPlayerSnapshot = false;
    double playerXInOverworld = 0.0;
    double playerZInOverworld = 0.0;
    double playerYaw = 0.0;
    bool isInOverworld = true;
    bool isInNether = false;
    int boatState = kBoatStateUninitialized;
    bool hasBoatAngle = false;
    double boatAngleDeg = 0.0;
    std::vector<ParsedEyeThrow> eyeThrows;
};

struct StrongholdLivePlayerPose {
    bool valid = false;
    double xInOverworld = 0.0;
    double zInOverworld = 0.0;
    double yawDeg = 0.0;
    bool isInNether = false;
    std::chrono::steady_clock::time_point lastUpdate = std::chrono::steady_clock::now();
};

struct StrongholdOverlayRuntimeState {
    bool initializedVisibility = false;
    bool visible = false;
    int failCount = 0;

    bool targetLocked = false;
    int lockedChunkX = 0;
    int lockedChunkZ = 0;
    bool lockSourceAuto = false;

    bool hasLiveTarget = false;
    int lastLiveChunkX = 0;
    int lastLiveChunkZ = 0;
    bool liveTargetFromNativeTriangulation = false;
    bool hasAutoLockedOnNether = false;
    bool blockAutoLockUntilThrowClear = false;
    bool wasInNetherLastTick = false;
    int lastEyeThrowCount = 0;
    int activeEyeThrowCount = 0;
    int ignoredThrowsPrefixCount = 0;
    double lastThrowAngleAdjustmentDeg = 0.0;
    int lastAdjustmentStepDirection = 0;
    std::vector<double> perThrowAngleAdjustmentsDeg;
    std::vector<double> adjustmentUndoStackDeg;
    std::vector<double> adjustmentRedoStackDeg;
    int adjustmentHistoryThrowCount = 0;

    bool apiOnline = false;
    bool usingStandalonePipeline = false;
    bool hasPlayerSnapshot = false;
    bool hasPrediction = false;
    bool usingNetherCoords = true;
    bool usingLiveTarget = true;
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
    int boatState = kBoatStateUninitialized;
    std::string boatLabel = "Boat: UNINIT";
    std::string modeLabel = "nether";
    std::string statusLabel = "LIVE/UNLOCKED";
    std::string infoLabel = "No throws yet. Shift+H lock";
    std::string debugBasePredictionsLabel;
    std::string debugAdjustedPredictionsLabel;
    std::string debugSelectionLabel;
    bool showComputedDetails = false;
    double lastActiveThrowVerticalAngleDeg = -31.6;
};

struct ManagedNinjabrainBotProcessState {
    HANDLE processHandle = nullptr;
    DWORD processId = 0;
    bool launchedByToolscreen = false;
    int launchFailures = 0;
    std::wstring lastResolvedJarPath;
    std::chrono::steady_clock::time_point nextLaunchAttempt = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point nextHideAttempt = std::chrono::steady_clock::time_point::min();
};

struct NbbBoatAngleSettings {
    double sensitivityAutomatic = kNbbDefaultSensitivityAutomatic;
    double crosshairCorrectionDeg = kNbbDefaultCrosshairCorrectionDeg;
};

struct NbbStandardDeviationSettings {
    double sigmaNormal = kDefaultSigmaNormal;
    double sigmaAlt = kDefaultSigmaAlt;
    double sigmaManual = kDefaultSigmaManual;
    double sigmaBoat = kDefaultSigmaBoat;
};

struct NbbAngleAdjustmentSettings {
    int adjustmentType = 0; // 0=subpixel, 1=tall, 2=custom
    double resolutionHeight = 16384.0;
    double customAdjustment = 0.01;
};

enum class EnsureManagedBackendResult {
    Disabled,
    ApiOnline,
    AutoStartDisabled,
    WaitingForRunningProcess,
    Launching,
    MissingJarPath,
    LaunchFailed
};

enum class TryStartManagedBackendResult {
    Started,
    AlreadyRunning,
    MissingJarPath,
    LaunchFailed
};

struct WinHttpApi {
    HMODULE module = nullptr;
    decltype(&WinHttpOpen) open = nullptr;
    decltype(&WinHttpConnect) connect = nullptr;
    decltype(&WinHttpOpenRequest) openRequest = nullptr;
    decltype(&WinHttpSetTimeouts) setTimeouts = nullptr;
    decltype(&WinHttpSendRequest) sendRequest = nullptr;
    decltype(&WinHttpReceiveResponse) receiveResponse = nullptr;
    decltype(&WinHttpQueryHeaders) queryHeaders = nullptr;
    decltype(&WinHttpQueryDataAvailable) queryDataAvailable = nullptr;
    decltype(&WinHttpReadData) readData = nullptr;
    decltype(&WinHttpCloseHandle) closeHandle = nullptr;

    bool EnsureLoaded() {
        if (module) return true;

        module = LoadLibraryW(L"winhttp.dll");
        if (!module) return false;

        open = reinterpret_cast<decltype(open)>(GetProcAddress(module, "WinHttpOpen"));
        connect = reinterpret_cast<decltype(connect)>(GetProcAddress(module, "WinHttpConnect"));
        openRequest = reinterpret_cast<decltype(openRequest)>(GetProcAddress(module, "WinHttpOpenRequest"));
        setTimeouts = reinterpret_cast<decltype(setTimeouts)>(GetProcAddress(module, "WinHttpSetTimeouts"));
        sendRequest = reinterpret_cast<decltype(sendRequest)>(GetProcAddress(module, "WinHttpSendRequest"));
        receiveResponse = reinterpret_cast<decltype(receiveResponse)>(GetProcAddress(module, "WinHttpReceiveResponse"));
        queryHeaders = reinterpret_cast<decltype(queryHeaders)>(GetProcAddress(module, "WinHttpQueryHeaders"));
        queryDataAvailable = reinterpret_cast<decltype(queryDataAvailable)>(GetProcAddress(module, "WinHttpQueryDataAvailable"));
        readData = reinterpret_cast<decltype(readData)>(GetProcAddress(module, "WinHttpReadData"));
        closeHandle = reinterpret_cast<decltype(closeHandle)>(GetProcAddress(module, "WinHttpCloseHandle"));

        if (open && connect && openRequest && setTimeouts && sendRequest && receiveResponse && queryHeaders && queryDataAvailable &&
            readData && closeHandle) {
            return true;
        }

        FreeLibrary(module);
        module = nullptr;
        return false;
    }
};

static WinHttpApi s_winHttpApi;
static std::mutex s_strongholdOverlayMutex;
static StrongholdOverlayRuntimeState s_strongholdOverlayState;
static std::chrono::steady_clock::time_point s_nextStrongholdPollTime;
static ManagedNinjabrainBotProcessState s_managedNinjabrainBotProcess;
static StandaloneStrongholdState s_standaloneStrongholdState;
static std::atomic<bool> s_pendingStandaloneReset{ false };
static NbbBoatAngleSettings s_cachedNbbBoatAngleSettings;
static ULONGLONG s_cachedNbbBoatAngleSettingsRefreshMs = 0;
static bool s_cachedNbbBoatAngleSettingsInitialized = false;
static NbbStandardDeviationSettings s_cachedNbbStandardDeviationSettings;
static ULONGLONG s_cachedNbbStandardDeviationSettingsRefreshMs = 0;
static bool s_cachedNbbStandardDeviationSettingsInitialized = false;
static NbbAngleAdjustmentSettings s_cachedNbbAngleAdjustmentSettings;
static ULONGLONG s_cachedNbbAngleAdjustmentSettingsRefreshMs = 0;
static bool s_cachedNbbAngleAdjustmentSettingsInitialized = false;
static std::atomic<int> s_pendingStrongholdMouseDeltaX{ 0 };
static std::atomic<int> s_pendingStrongholdMouseDeltaY{ 0 };
static std::atomic<uint32_t> s_strongholdMovementKeyMask{ 0 };
static StrongholdLivePlayerPose s_strongholdLivePlayerPose;
static uint64_t s_lastAnchoredStandaloneSnapshotCounter = 0;
static std::atomic<bool> s_mcsrRankedInstanceDetected{ false };
static std::atomic<ULONGLONG> s_mcsrRankedDetectionNextRefreshMs{ 0 };
static std::string s_mcsrRankedDetectionSource;
static std::mutex s_mcsrRankedDetectionMutex;

#if defined(TOOLSCREEN_FORCE_MCSR_SAFE)
constexpr bool kForceMcsrSafeBuild = true;
#else
constexpr bool kForceMcsrSafeBuild = false;
#endif

struct EyeSpyAutoHideState {
    std::filesystem::path latestLogPath;
    std::uintmax_t lastReadOffset = 0;
    bool initializedReadOffset = false;
    ULONGLONG nextPathRefreshMs = 0;
};

static EyeSpyAutoHideState s_eyeSpyAutoHideState;

static std::wstring ToLowerAscii(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) {
        if (ch >= L'A' && ch <= L'Z') return static_cast<wchar_t>(ch - L'A' + L'a');
        return ch;
    });
    return s;
}

static bool IsNinjabrainBotJarName(const std::wstring& filename) {
    std::wstring lower = ToLowerAscii(filename);
    if (lower.size() < 4 || lower.substr(lower.size() - 4) != L".jar") return false;
    return lower.find(L"ninjabrain-bot") != std::wstring::npos;
}

static std::wstring NormalizePathForCompare(const std::filesystem::path& p) {
    try {
        return ToLowerAscii(std::filesystem::weakly_canonical(p).wstring());
    } catch (...) {
        return ToLowerAscii(p.lexically_normal().wstring());
    }
}

static void AddUniqueSearchDirectory(std::vector<std::filesystem::path>& outDirs, std::vector<std::wstring>& seenDirs,
                                     const std::filesystem::path& candidate) {
    if (candidate.empty()) return;
    try {
        if (!std::filesystem::exists(candidate) || !std::filesystem::is_directory(candidate)) return;
        std::wstring norm = NormalizePathForCompare(candidate);
        if (std::find(seenDirs.begin(), seenDirs.end(), norm) != seenDirs.end()) return;
        seenDirs.push_back(norm);
        outDirs.push_back(candidate);
    } catch (...) {
    }
}

static std::wstring FindNinjabrainBotJarInDirectory(const std::filesystem::path& dir) {
    try {
        std::filesystem::path bestPath;
        std::filesystem::file_time_type bestWriteTime{};
        bool haveBest = false;

        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const std::filesystem::path p = entry.path();
            if (!IsNinjabrainBotJarName(p.filename().wstring())) continue;

            std::filesystem::file_time_type writeTime = entry.last_write_time();
            if (!haveBest || writeTime > bestWriteTime) {
                haveBest = true;
                bestWriteTime = writeTime;
                bestPath = p;
            }
        }

        if (haveBest) return bestPath.wstring();
    } catch (...) {
    }
    return L"";
}

static std::wstring ResolveNinjabrainBotJarPath(const StrongholdOverlayConfig& overlayCfg) {
    // Explicit user path wins (absolute or relative to toolscreen directory).
    if (!overlayCfg.ninjabrainBotJarPath.empty()) {
        try {
            std::filesystem::path configured = std::filesystem::path(Utf8ToWide(overlayCfg.ninjabrainBotJarPath));
            if (configured.is_relative()) {
                if (!g_toolscreenPath.empty()) {
                    configured = std::filesystem::path(g_toolscreenPath) / configured;
                } else {
                    configured = std::filesystem::current_path() / configured;
                }
            }
            std::wstring ext = ToLowerAscii(configured.extension().wstring());
            if (std::filesystem::exists(configured) && std::filesystem::is_regular_file(configured) && ext == L".jar") {
                return configured.wstring();
            }
        } catch (...) {
        }
        return L"";
    }

    std::vector<std::filesystem::path> searchDirs;
    std::vector<std::wstring> seenDirs;

    if (!g_toolscreenPath.empty()) {
        std::filesystem::path toolscreenDir(g_toolscreenPath);
        AddUniqueSearchDirectory(searchDirs, seenDirs, toolscreenDir);
        AddUniqueSearchDirectory(searchDirs, seenDirs, toolscreenDir.parent_path());
        AddUniqueSearchDirectory(searchDirs, seenDirs, toolscreenDir.parent_path().parent_path());
    }

    try {
        std::filesystem::path cwd = std::filesystem::current_path();
        AddUniqueSearchDirectory(searchDirs, seenDirs, cwd);
        AddUniqueSearchDirectory(searchDirs, seenDirs, cwd.parent_path());
        AddUniqueSearchDirectory(searchDirs, seenDirs, cwd.parent_path().parent_path());
    } catch (...) {
    }

    for (const auto& dir : searchDirs) {
        std::wstring found = FindNinjabrainBotJarInDirectory(dir);
        if (!found.empty()) return found;
    }

    return L"";
}

static bool IsManagedNinjabrainBotProcessRunning() {
    auto& proc = s_managedNinjabrainBotProcess;
    if (!proc.processHandle) return false;

    DWORD waitResult = WaitForSingleObject(proc.processHandle, 0);
    if (waitResult == WAIT_TIMEOUT) return true;

    DWORD exitCode = 0;
    if (GetExitCodeProcess(proc.processHandle, &exitCode)) {
        Log("[StrongholdOverlay] Managed NinjaBrainBot process exited with code " + std::to_string(static_cast<unsigned int>(exitCode)) + ".");
    } else {
        Log("[StrongholdOverlay] Managed NinjaBrainBot process exited.");
    }

    CloseHandle(proc.processHandle);
    proc.processHandle = nullptr;
    proc.processId = 0;
    proc.launchedByToolscreen = false;
    return false;
}

struct HideWindowsContext {
    DWORD processId = 0;
    int hiddenCount = 0;
};

static BOOL CALLBACK HideWindowsForProcessEnumProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<HideWindowsContext*>(lParam);
    if (!ctx || ctx->processId == 0) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->processId) return TRUE;

    ShowWindow(hwnd, SW_HIDE);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ctx->hiddenCount += 1;
    return TRUE;
}

static void HideManagedNinjabrainBotWindowsIfNeeded(const StrongholdOverlayConfig& overlayCfg) {
    if (!overlayCfg.hideNinjabrainBotWindow) return;
    if (!IsManagedNinjabrainBotProcessRunning()) return;

    auto& proc = s_managedNinjabrainBotProcess;
    auto now = std::chrono::steady_clock::now();
    if (now < proc.nextHideAttempt) return;
    proc.nextHideAttempt = now + std::chrono::milliseconds(500);

    HideWindowsContext ctx;
    ctx.processId = proc.processId;
    EnumWindows(HideWindowsForProcessEnumProc, reinterpret_cast<LPARAM>(&ctx));
}

static TryStartManagedBackendResult TryStartManagedNinjabrainBot(const StrongholdOverlayConfig& overlayCfg) {
    if (IsManagedNinjabrainBotProcessRunning()) return TryStartManagedBackendResult::AlreadyRunning;

    const std::wstring jarPath = ResolveNinjabrainBotJarPath(overlayCfg);
    if (jarPath.empty()) return TryStartManagedBackendResult::MissingJarPath;

    wchar_t exePath[MAX_PATH] = { 0 };
    DWORD exeLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (exeLen == 0 || exeLen >= MAX_PATH) return TryStartManagedBackendResult::LaunchFailed;
    std::wstring javaExe(exePath, exeLen);

    std::wstring commandLine = L"\"" + javaExe + L"\" -jar \"" + jarPath + L"\"";
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (overlayCfg.hideNinjabrainBotWindow) {
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }

    PROCESS_INFORMATION pi{};
    std::wstring workingDir;
    try {
        workingDir = std::filesystem::path(jarPath).parent_path().wstring();
    } catch (...) {
        workingDir.clear();
    }

    BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                  workingDir.empty() ? nullptr : workingDir.c_str(), &si, &pi);
    if (!created) {
        Log("[StrongholdOverlay] Failed to launch managed NinjaBrainBot backend. Win32=" + std::to_string(GetLastError()));
        return TryStartManagedBackendResult::LaunchFailed;
    }

    CloseHandle(pi.hThread);

    auto& proc = s_managedNinjabrainBotProcess;
    if (proc.processHandle) CloseHandle(proc.processHandle);
    proc.processHandle = pi.hProcess;
    proc.processId = pi.dwProcessId;
    proc.launchedByToolscreen = true;
    proc.lastResolvedJarPath = jarPath;
    proc.launchFailures = 0;
    proc.nextHideAttempt = std::chrono::steady_clock::time_point::min();

    Log("[StrongholdOverlay] Started managed NinjaBrainBot backend: " + WideToUtf8(jarPath) + " (pid " +
        std::to_string(static_cast<unsigned int>(pi.dwProcessId)) + ")");
    HideManagedNinjabrainBotWindowsIfNeeded(overlayCfg);
    return TryStartManagedBackendResult::Started;
}

static EnsureManagedBackendResult EnsureManagedNinjabrainBotBackend(const StrongholdOverlayConfig& overlayCfg, bool apiOnline) {
    if (!overlayCfg.manageNinjabrainBotProcess) return EnsureManagedBackendResult::Disabled;

    bool running = IsManagedNinjabrainBotProcessRunning();
    if (running) HideManagedNinjabrainBotWindowsIfNeeded(overlayCfg);
    if (apiOnline) return EnsureManagedBackendResult::ApiOnline;

    if (!overlayCfg.autoStartNinjabrainBot) return EnsureManagedBackendResult::AutoStartDisabled;

    auto& proc = s_managedNinjabrainBotProcess;
    auto now = std::chrono::steady_clock::now();
    if (running || now < proc.nextLaunchAttempt) return EnsureManagedBackendResult::WaitingForRunningProcess;

    TryStartManagedBackendResult startResult = TryStartManagedNinjabrainBot(overlayCfg);
    switch (startResult) {
    case TryStartManagedBackendResult::Started:
        proc.nextLaunchAttempt = now + std::chrono::seconds(2);
        return EnsureManagedBackendResult::Launching;
    case TryStartManagedBackendResult::AlreadyRunning:
        return EnsureManagedBackendResult::WaitingForRunningProcess;
    case TryStartManagedBackendResult::MissingJarPath:
        proc.nextLaunchAttempt = now + std::chrono::seconds(5);
        return EnsureManagedBackendResult::MissingJarPath;
    case TryStartManagedBackendResult::LaunchFailed:
    default:
        proc.launchFailures += 1;
        proc.nextLaunchAttempt = now + std::chrono::seconds(std::clamp(proc.launchFailures * 2, 4, 20));
        return EnsureManagedBackendResult::LaunchFailed;
    }
}

static std::string ManagedBackendOfflineMessage(EnsureManagedBackendResult result) {
    switch (result) {
    case EnsureManagedBackendResult::AutoStartDisabled:
        return "Backend API unavailable. Enable Auto-Start Backend.";
    case EnsureManagedBackendResult::Launching:
        return "Starting backend...";
    case EnsureManagedBackendResult::WaitingForRunningProcess:
        return "Waiting for backend API...";
    case EnsureManagedBackendResult::MissingJarPath:
        return "Backend jar not found. Set strongholdOverlay.ninjabrainBotJarPath.";
    case EnsureManagedBackendResult::LaunchFailed:
        return "Failed to start backend. Check ninjabrainBotJarPath.";
    case EnsureManagedBackendResult::Disabled:
    case EnsureManagedBackendResult::ApiOnline:
    default:
        return "Backend API unavailable.";
    }
}

static void ShutdownManagedNinjabrainBotProcess() {
    auto& proc = s_managedNinjabrainBotProcess;
    if (proc.processHandle) {
        if (proc.launchedByToolscreen && IsManagedNinjabrainBotProcessRunning()) {
            TerminateProcess(proc.processHandle, 0);
            WaitForSingleObject(proc.processHandle, 1000);
            Log("[StrongholdOverlay] Stopped managed NinjaBrainBot backend.");
        }
        if (proc.processHandle) CloseHandle(proc.processHandle);
    }
    proc = ManagedNinjabrainBotProcessState{};
}

static bool ExtractRegexDouble(const std::string& input, const std::regex& pattern, double& out) {
    std::smatch match;
    if (!std::regex_search(input, match, pattern) || match.size() < 2) return false;
    try {
        out = std::stod(match[1].str());
        return true;
    } catch (...) {
        return false;
    }
}

static bool ExtractRegexInt(const std::string& input, const std::regex& pattern, int& out) {
    std::smatch match;
    if (!std::regex_search(input, match, pattern) || match.size() < 2) return false;
    try {
        out = std::stoi(match[1].str());
        return true;
    } catch (...) {
        return false;
    }
}

static bool ExtractRegexBool(const std::string& input, const std::regex& pattern, bool& out) {
    std::smatch match;
    if (!std::regex_search(input, match, pattern) || match.size() < 2) return false;
    std::string value = match[1].str();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "true") {
        out = true;
        return true;
    }
    if (value == "false") {
        out = false;
        return true;
    }
    return false;
}

static double NormalizeDegrees(double degrees) {
    while (degrees > 180.0) degrees -= 360.0;
    while (degrees <= -180.0) degrees += 360.0;
    return degrees;
}

static double DegreesToRadians(double degrees) {
    return degrees * kPi / 180.0;
}

static double MinecraftYawDegreesPerMouseCount(double sensitivity) {
    double preMultiplier = sensitivity * 0.6 + 0.2;
    preMultiplier = preMultiplier * preMultiplier * preMultiplier * 8.0;
    return preMultiplier * 0.15;
}

static bool IsInWorldGameStateForStrongholdTracking() {
    const std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
    return localGameState.find("inworld") != std::string::npos;
}

static bool IsStrongholdLiveTrackingInputAllowed() {
    if (g_isShuttingDown.load(std::memory_order_relaxed)) return false;
    if (g_showGui.load(std::memory_order_relaxed)) return false;
    return IsInWorldGameStateForStrongholdTracking();
}

static NbbStandardDeviationSettings GetResolvedNbbStandardDeviationSettings();

static std::string FormatSignedHundredths(double value) {
    std::ostringstream out;
    out << std::showpos << std::fixed;
    if (std::abs(value) < 0.1) {
        out << std::setprecision(3) << value;
    } else {
        out << std::setprecision(2) << value;
    }
    return out.str();
}

static EyeThrowType EyeThrowTypeFromString(std::string type) {
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (type == "NORMAL") return EyeThrowType::Normal;
    if (type == "NORMAL_WITH_ALT_STD") return EyeThrowType::NormalWithAltStd;
    if (type == "MANUAL") return EyeThrowType::Manual;
    if (type == "BOAT") return EyeThrowType::Boat;
    return EyeThrowType::Unknown;
}

static double SigmaDegreesForThrowType(EyeThrowType type) {
    const NbbStandardDeviationSettings settings = GetResolvedNbbStandardDeviationSettings();
    switch (type) {
    case EyeThrowType::NormalWithAltStd:
        return settings.sigmaAlt;
    case EyeThrowType::Manual:
        return settings.sigmaManual;
    case EyeThrowType::Boat:
        return settings.sigmaBoat;
    case EyeThrowType::Normal:
    case EyeThrowType::Unknown:
    default:
        return settings.sigmaNormal;
    }
}

static void TrimAsciiWhitespaceInPlace(std::string& value) {
    auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) value.pop_back();
}

static bool TryParseFlexibleDouble(std::string rawValue, double& outValue) {
    outValue = 0.0;
    TrimAsciiWhitespaceInPlace(rawValue);
    if (rawValue.empty()) return false;

    std::string normalized;
    normalized.reserve(rawValue.size());
    for (size_t i = 0; i < rawValue.size(); ++i) {
        const char c = rawValue[i];
        if (c == ',' && rawValue.find('.') == std::string::npos) {
            normalized.push_back('.');
            continue;
        }

        // Java preference dumps sometimes include a slash before exponent (e.g. 7.0/E-4).
        if (c == '/' && i + 1 < rawValue.size() && (rawValue[i + 1] == 'e' || rawValue[i + 1] == 'E')) { continue; }
        normalized.push_back(c);
    }
    TrimAsciiWhitespaceInPlace(normalized);
    if (normalized.empty()) return false;

    try {
        size_t parsedChars = 0;
        const double parsed = std::stod(normalized, &parsedChars);
        while (parsedChars < normalized.size() && std::isspace(static_cast<unsigned char>(normalized[parsedChars]))) {
            ++parsedChars;
        }
        if (parsedChars != normalized.size()) return false;
        if (!std::isfinite(parsed)) return false;
        outValue = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

static bool TryReadRegistryStringValue(HKEY rootKey, const wchar_t* subKey, const wchar_t* valueName, std::string& outValue) {
    outValue.clear();

    DWORD valueType = 0;
    DWORD bufferBytes = 0;
    const LONG sizeResult = RegGetValueW(rootKey, subKey, valueName, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &valueType, nullptr, &bufferBytes);
    if (sizeResult != ERROR_SUCCESS || bufferBytes < sizeof(wchar_t)) return false;

    std::vector<wchar_t> buffer((bufferBytes / sizeof(wchar_t)) + 1, L'\0');
    const LONG readResult =
        RegGetValueW(rootKey, subKey, valueName, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &valueType, buffer.data(), &bufferBytes);
    if (readResult != ERROR_SUCCESS) return false;

    buffer.back() = L'\0';
    outValue = WideToUtf8(std::wstring(buffer.data()));
    TrimAsciiWhitespaceInPlace(outValue);
    return !outValue.empty();
}

static bool TryReadRegistryDouble(HKEY rootKey, const wchar_t* subKey, const wchar_t* valueName, double& outValue) {
    outValue = 0.0;
    std::string value;
    if (!TryReadRegistryStringValue(rootKey, subKey, valueName, value)) return false;
    return TryParseFlexibleDouble(value, outValue);
}

static bool TryReadEnvironmentVariable(const wchar_t* name, std::wstring& outValue) {
    outValue.clear();
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return false;

    std::vector<wchar_t> buffer(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, buffer.data(), required);
    if (written == 0 || written >= required) return false;

    outValue.assign(buffer.data(), written);
    return !outValue.empty();
}

static bool ContainsMcsrRankedToken(const std::wstring& text) {
    if (text.empty()) return false;
    const std::wstring lower = ToLowerAscii(text);
    return lower.find(L"mcsrranked") != std::wstring::npos || lower.find(L"mcsr-ranked") != std::wstring::npos ||
           lower.find(L"mcsr ranked") != std::wstring::npos;
}

static bool DetectMcsrRankedInstancePath(std::string& outSource) {
    outSource.clear();

    auto checkPathValue = [&](const std::wstring& value, const char* sourceTag) -> bool {
        if (value.empty()) return false;
        if (!ContainsMcsrRankedToken(value)) return false;
        outSource = std::string(sourceTag) + ": " + WideToUtf8(value);
        return true;
    };

    std::wstring envValue;
    if (TryReadEnvironmentVariable(L"INST_MC_DIR", envValue) && checkPathValue(envValue, "INST_MC_DIR")) return true;
    if (TryReadEnvironmentVariable(L"INST_DIR", envValue) && checkPathValue(envValue, "INST_DIR")) return true;

    if (!g_toolscreenPath.empty() && checkPathValue(g_toolscreenPath, "toolscreenPath")) return true;

    try {
        const std::filesystem::path cwd = std::filesystem::current_path();
        if (checkPathValue(cwd.wstring(), "cwd")) return true;
    } catch (...) {
    }

    return false;
}

static void RefreshMcsrRankedDetectionIfNeeded(bool force = false) {
    const ULONGLONG nowMs = GetTickCount64();
    if (!force && nowMs < s_mcsrRankedDetectionNextRefreshMs.load(std::memory_order_relaxed)) return;
    s_mcsrRankedDetectionNextRefreshMs.store(nowMs + 5000, std::memory_order_relaxed);

    std::string detectedSource;
    const bool detected = DetectMcsrRankedInstancePath(detectedSource);
    const bool previous = s_mcsrRankedInstanceDetected.exchange(detected, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(s_mcsrRankedDetectionMutex);
        s_mcsrRankedDetectionSource = detected ? detectedSource : "";
    }

    if (detected != previous) {
        if (detected) {
            Log("[MCSR] Ranked-instance mode enabled (" + detectedSource + "). Non-approved visuals are forced OFF.");
        } else {
            Log("[MCSR] Ranked-instance mode disabled (no MCSRRanked path hints detected).");
        }
    }
}

static bool TryReadMouseSensitivityFromOptionsFile(const std::filesystem::path& optionsPath, double& outSensitivity) {
    outSensitivity = 0.0;
    std::ifstream file(optionsPath);
    if (!file.is_open()) return false;

    static const std::string kMouseSensitivityPrefix = "mouseSensitivity:";
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind(kMouseSensitivityPrefix, 0) != 0) continue;
        std::string value = line.substr(kMouseSensitivityPrefix.size());
        double parsed = 0.0;
        if (!TryParseFlexibleDouble(value, parsed)) return false;
        outSensitivity = std::clamp(parsed, 0.0, 1.0);
        return true;
    }
    return false;
}

static void AddUniquePathCandidate(std::vector<std::filesystem::path>& outPaths, std::vector<std::wstring>& seenPaths,
                                   const std::filesystem::path& candidate) {
    if (candidate.empty()) return;
    const std::wstring normalized = ToLowerAscii(candidate.lexically_normal().wstring());
    if (std::find(seenPaths.begin(), seenPaths.end(), normalized) != seenPaths.end()) return;
    seenPaths.push_back(normalized);
    outPaths.push_back(candidate);
}

static void AddCommonInstanceOptionsCandidates(std::vector<std::filesystem::path>& outPaths, std::vector<std::wstring>& seenPaths,
                                               const std::filesystem::path& instanceDir) {
    AddUniquePathCandidate(outPaths, seenPaths, instanceDir / L".minecraft" / L"options.txt");
    AddUniquePathCandidate(outPaths, seenPaths, instanceDir / L"minecraft" / L"options.txt");
    AddUniquePathCandidate(outPaths, seenPaths, instanceDir / L"options.txt");
    AddUniquePathCandidate(outPaths, seenPaths, instanceDir / L"game" / L"options.txt");
}

static void AddLauncherInstanceOptionsCandidates(std::vector<std::filesystem::path>& outPaths, std::vector<std::wstring>& seenPaths,
                                                 const std::filesystem::path& launcherRoot,
                                                 const std::filesystem::path& instancesRelativePath) {
    if (launcherRoot.empty()) return;

    const std::filesystem::path instancesRoot = launcherRoot / instancesRelativePath;
    std::error_code ec;
    if (!std::filesystem::exists(instancesRoot, ec) || ec) return;
    if (!std::filesystem::is_directory(instancesRoot, ec) || ec) return;

    AddUniquePathCandidate(outPaths, seenPaths, instancesRoot / L"options.txt");
    for (std::filesystem::directory_iterator it(instancesRoot, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code typeEc;
        if (!it->is_directory(typeEc) || typeEc) continue;
        AddCommonInstanceOptionsCandidates(outPaths, seenPaths, it->path());
    }
}

static void AddCommonMinecraftLogCandidates(std::vector<std::filesystem::path>& outPaths, std::vector<std::wstring>& seenPaths,
                                            const std::filesystem::path& baseDir) {
    if (baseDir.empty()) return;
    AddUniquePathCandidate(outPaths, seenPaths, baseDir / L"logs" / L"latest.log");
    AddUniquePathCandidate(outPaths, seenPaths, baseDir / L".minecraft" / L"logs" / L"latest.log");
    AddUniquePathCandidate(outPaths, seenPaths, baseDir / L"minecraft" / L"logs" / L"latest.log");
}

static void AddLauncherInstanceLogCandidates(std::vector<std::filesystem::path>& outPaths, std::vector<std::wstring>& seenPaths,
                                             const std::filesystem::path& launcherRoot,
                                             const std::filesystem::path& instancesRelativePath) {
    if (launcherRoot.empty()) return;

    const std::filesystem::path instancesRoot = launcherRoot / instancesRelativePath;
    std::error_code ec;
    if (!std::filesystem::exists(instancesRoot, ec) || ec) return;
    if (!std::filesystem::is_directory(instancesRoot, ec) || ec) return;

    AddCommonMinecraftLogCandidates(outPaths, seenPaths, instancesRoot);
    for (std::filesystem::directory_iterator it(instancesRoot, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code typeEc;
        if (!it->is_directory(typeEc) || typeEc) continue;
        AddCommonMinecraftLogCandidates(outPaths, seenPaths, it->path());
    }
}

static bool TryResolveMinecraftLatestLogPath(std::filesystem::path& outPath) {
    outPath.clear();

    const ULONGLONG nowMs = GetTickCount64();
    if (!s_eyeSpyAutoHideState.latestLogPath.empty()) {
        std::error_code cacheEc;
        if (std::filesystem::exists(s_eyeSpyAutoHideState.latestLogPath, cacheEc) &&
            std::filesystem::is_regular_file(s_eyeSpyAutoHideState.latestLogPath, cacheEc) && !cacheEc) {
            outPath = s_eyeSpyAutoHideState.latestLogPath;
            return true;
        }
    }

    if (nowMs < s_eyeSpyAutoHideState.nextPathRefreshMs && !s_eyeSpyAutoHideState.latestLogPath.empty()) { return false; }
    s_eyeSpyAutoHideState.nextPathRefreshMs = nowMs + 5000;

    std::vector<std::filesystem::path> candidates;
    std::vector<std::wstring> seenCandidates;

    std::wstring instMcDirW;
    if (TryReadEnvironmentVariable(L"INST_MC_DIR", instMcDirW)) {
        const std::filesystem::path instPath(instMcDirW);
        AddCommonMinecraftLogCandidates(candidates, seenCandidates, instPath);
        AddCommonMinecraftLogCandidates(candidates, seenCandidates, instPath.parent_path());
    }

    if (!g_toolscreenPath.empty()) {
        try {
            const std::filesystem::path toolscreenDir(g_toolscreenPath);
            AddCommonMinecraftLogCandidates(candidates, seenCandidates, toolscreenDir);
            AddCommonMinecraftLogCandidates(candidates, seenCandidates, toolscreenDir.parent_path());
        } catch (...) {
        }
    }

    try {
        const std::filesystem::path cwd = std::filesystem::current_path();
        AddCommonMinecraftLogCandidates(candidates, seenCandidates, cwd);
        AddCommonMinecraftLogCandidates(candidates, seenCandidates, cwd.parent_path());
    } catch (...) {
    }

    std::wstring userProfile;
    if (TryReadEnvironmentVariable(L"USERPROFILE", userProfile)) {
        const std::filesystem::path userRoot(userProfile);
        AddCommonMinecraftLogCandidates(candidates, seenCandidates, userRoot / L".minecraft");
        AddCommonMinecraftLogCandidates(candidates, seenCandidates, userRoot / L"AppData" / L"Roaming" / L".minecraft");
        AddCommonMinecraftLogCandidates(
            candidates, seenCandidates,
            userRoot / L"Desktop" / L"msr" / L"MultiMC" / L"instances" / L"MCSRRanked-Windows-1.16.1-All" / L".minecraft");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, userRoot / L"Desktop" / L"msr" / L"MultiMC", L"instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, userRoot / L"curseforge" / L"minecraft", L"Instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, userRoot / L"FTB", L"Instances");
    }

    std::wstring appData;
    if (TryReadEnvironmentVariable(L"APPDATA", appData)) {
        const std::filesystem::path appDataRoot(appData);
        AddCommonMinecraftLogCandidates(candidates, seenCandidates, appDataRoot / L".minecraft");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, appDataRoot / L"PrismLauncher", L"instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, appDataRoot / L"MultiMC", L"instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, appDataRoot / L"PolyMC", L"instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, appDataRoot / L"ATLauncher", L"instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, appDataRoot / L"gdlauncher_next", L"instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, appDataRoot / L"GDLauncher_Carbon", L"instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, appDataRoot / L"curseforge" / L"minecraft", L"Instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, appDataRoot / L".technic", L"modpacks");
    }

    std::wstring localAppData;
    if (TryReadEnvironmentVariable(L"LOCALAPPDATA", localAppData)) {
        const std::filesystem::path localAppDataRoot(localAppData);
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, localAppDataRoot / L"PrismLauncher", L"instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, localAppDataRoot / L"MultiMC", L"instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, localAppDataRoot / L"PolyMC", L"instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, localAppDataRoot / L"ATLauncher", L"instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, localAppDataRoot / L"gdlauncher_next", L"instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, localAppDataRoot / L"GDLauncher_Carbon", L"instances");
        AddLauncherInstanceLogCandidates(candidates, seenCandidates, localAppDataRoot / L"curseforge" / L"minecraft", L"Instances");
    }

    std::filesystem::path newestPath;
    std::filesystem::file_time_type newestWrite{};
    bool found = false;
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || ec) continue;
        if (!std::filesystem::is_regular_file(candidate, ec) || ec) continue;
        const auto writeTime = std::filesystem::last_write_time(candidate, ec);
        if (!found || (!ec && writeTime > newestWrite)) {
            newestPath = candidate;
            newestWrite = writeTime;
            found = true;
        }
    }
    if (!found) return false;

    if (s_eyeSpyAutoHideState.latestLogPath != newestPath) {
        s_eyeSpyAutoHideState.latestLogPath = newestPath;
        s_eyeSpyAutoHideState.initializedReadOffset = false;
        s_eyeSpyAutoHideState.lastReadOffset = 0;
        Log("Stronghold overlay: tracking Minecraft log " + WideToUtf8(newestPath.wstring()));
    }
    outPath = newestPath;
    return true;
}

static bool ContainsEyeSpyMarker(const std::string& text) {
    if (text.empty()) return false;
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("eye spy") != std::string::npos || lower.find("eye_spy") != std::string::npos ||
           lower.find("minecraft:end/eye_spy") != std::string::npos;
}

static bool PollEyeSpyAdvancementDetected() {
    std::filesystem::path latestLogPath;
    if (!TryResolveMinecraftLatestLogPath(latestLogPath)) return false;

    std::error_code ec;
    const std::uintmax_t fileSize = std::filesystem::file_size(latestLogPath, ec);
    if (ec) return false;

    if (!s_eyeSpyAutoHideState.initializedReadOffset) {
        s_eyeSpyAutoHideState.lastReadOffset = fileSize;
        s_eyeSpyAutoHideState.initializedReadOffset = true;
        return false;
    }

    if (fileSize < s_eyeSpyAutoHideState.lastReadOffset) {
        s_eyeSpyAutoHideState.lastReadOffset = fileSize;
        return false;
    }
    if (fileSize == s_eyeSpyAutoHideState.lastReadOffset) return false;

    constexpr std::uintmax_t kMaxChunkReadBytes = 256ull * 1024ull;
    std::uintmax_t readOffset = s_eyeSpyAutoHideState.lastReadOffset;
    if (fileSize - readOffset > kMaxChunkReadBytes) { readOffset = fileSize - kMaxChunkReadBytes; }
    const std::uintmax_t bytesToRead = fileSize - readOffset;

    std::ifstream file(latestLogPath, std::ios::binary);
    if (!file.is_open()) return false;
    file.seekg(static_cast<std::streamoff>(readOffset), std::ios::beg);
    if (!file.good()) return false;

    std::string chunk;
    chunk.resize(static_cast<size_t>(bytesToRead));
    file.read(chunk.data(), static_cast<std::streamsize>(bytesToRead));
    const std::streamsize bytesRead = file.gcount();
    if (bytesRead <= 0) {
        s_eyeSpyAutoHideState.lastReadOffset = fileSize;
        return false;
    }
    chunk.resize(static_cast<size_t>(bytesRead));
    s_eyeSpyAutoHideState.lastReadOffset = readOffset + static_cast<std::uintmax_t>(bytesRead);

    if (ContainsEyeSpyMarker(chunk)) {
        Log("Stronghold overlay: detected Eye Spy advancement marker in Minecraft log");
        return true;
    }
    return false;
}

static bool TryResolveMouseSensitivityFromOptionsTxt(double& outSensitivity) {
    outSensitivity = 0.0;

    // Instance-local resolution first. This keeps sensitivity lookup deterministic
    // for per-instance installs where options.txt lives in <instance>/.minecraft.
    std::vector<std::filesystem::path> instanceCandidates;
    std::vector<std::wstring> seenInstanceCandidates;
    if (!g_toolscreenPath.empty()) {
        try {
            const std::filesystem::path toolscreenDir(g_toolscreenPath);
            AddCommonInstanceOptionsCandidates(instanceCandidates, seenInstanceCandidates, toolscreenDir);
            AddCommonInstanceOptionsCandidates(instanceCandidates, seenInstanceCandidates, toolscreenDir.parent_path());
        } catch (...) {
        }
    }
    try {
        const std::filesystem::path cwd = std::filesystem::current_path();
        AddCommonInstanceOptionsCandidates(instanceCandidates, seenInstanceCandidates, cwd);
        AddCommonInstanceOptionsCandidates(instanceCandidates, seenInstanceCandidates, cwd.parent_path());
    } catch (...) {
    }
    for (const std::filesystem::path& candidate : instanceCandidates) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || ec) continue;
        if (!std::filesystem::is_regular_file(candidate, ec) || ec) continue;

        double parsed = 0.0;
        if (!TryReadMouseSensitivityFromOptionsFile(candidate, parsed)) continue;
        outSensitivity = parsed;
        return true;
    }

    std::vector<std::filesystem::path> candidates;
    std::vector<std::wstring> seenCandidates;

    // CWD-local candidates (launcher-agnostic fallback).
    try {
        const std::filesystem::path cwd = std::filesystem::current_path();
        AddUniquePathCandidate(candidates, seenCandidates, cwd / L"options.txt");
        AddUniquePathCandidate(candidates, seenCandidates, cwd / L".minecraft" / L"options.txt");
        AddUniquePathCandidate(candidates, seenCandidates, cwd.parent_path() / L"options.txt");
        AddUniquePathCandidate(candidates, seenCandidates, cwd.parent_path() / L".minecraft" / L"options.txt");
    } catch (...) {
    }

    std::wstring userProfile;
    if (TryReadEnvironmentVariable(L"USERPROFILE", userProfile)) {
        const std::filesystem::path userRoot(userProfile);
        AddUniquePathCandidate(candidates, seenCandidates, userRoot / L".minecraft" / L"options.txt");
        AddUniquePathCandidate(candidates, seenCandidates, userRoot / L"AppData" / L"Roaming" / L".minecraft" / L"options.txt");
        AddUniquePathCandidate(candidates, seenCandidates,
                               userRoot / L"Desktop" / L"msr" / L"MultiMC" / L"instances" / L"MCSRRanked-Windows-1.16.1-All" /
                                   L".minecraft" / L"options.txt");

        // Common Windows launcher instance roots under USERPROFILE.
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, userRoot / L"Desktop" / L"msr" / L"MultiMC", L"instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, userRoot / L"curseforge" / L"minecraft", L"Instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, userRoot / L"FTB", L"Instances");
    }

    std::wstring appData;
    if (TryReadEnvironmentVariable(L"APPDATA", appData)) {
        const std::filesystem::path appDataRoot(appData);
        AddUniquePathCandidate(candidates, seenCandidates, appDataRoot / L".minecraft" / L"options.txt");

        // Launcher-agnostic instance discovery (Roaming/AppData launchers).
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, appDataRoot / L"PrismLauncher", L"instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, appDataRoot / L"MultiMC", L"instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, appDataRoot / L"PolyMC", L"instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, appDataRoot / L"ATLauncher", L"instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, appDataRoot / L"gdlauncher_next", L"instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, appDataRoot / L"GDLauncher_Carbon", L"instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, appDataRoot / L"curseforge" / L"minecraft", L"Instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, appDataRoot / L".technic", L"modpacks");
    }

    std::wstring localAppData;
    if (TryReadEnvironmentVariable(L"LOCALAPPDATA", localAppData)) {
        const std::filesystem::path localAppDataRoot(localAppData);
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, localAppDataRoot / L"PrismLauncher", L"instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, localAppDataRoot / L"MultiMC", L"instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, localAppDataRoot / L"PolyMC", L"instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, localAppDataRoot / L"ATLauncher", L"instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, localAppDataRoot / L"gdlauncher_next", L"instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, localAppDataRoot / L"GDLauncher_Carbon", L"instances");
        AddLauncherInstanceOptionsCandidates(candidates, seenCandidates, localAppDataRoot / L"curseforge" / L"minecraft", L"Instances");
    }

    bool found = false;
    double latestSensitivity = 0.0;
    std::filesystem::file_time_type latestWriteTime{};
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || ec) continue;
        if (!std::filesystem::is_regular_file(candidate, ec) || ec) continue;

        double parsed = 0.0;
        if (!TryReadMouseSensitivityFromOptionsFile(candidate, parsed)) continue;

        const auto writeTime = std::filesystem::last_write_time(candidate, ec);
        if (!found || (!ec && writeTime > latestWriteTime)) {
            found = true;
            latestWriteTime = writeTime;
            latestSensitivity = parsed;
        }
    }

    if (!found) return false;
    outSensitivity = latestSensitivity;
    return true;
}

static NbbBoatAngleSettings GetResolvedNbbBoatAngleSettings() {
    const ULONGLONG now = GetTickCount64();
    if (s_cachedNbbBoatAngleSettingsInitialized && now - s_cachedNbbBoatAngleSettingsRefreshMs <= kNbbPrefsRefreshIntervalMs) {
        return s_cachedNbbBoatAngleSettings;
    }

    NbbBoatAngleSettings resolved;

    double sensitivity = 0.0;
    if (TryReadRegistryDouble(HKEY_CURRENT_USER, kNbbPrefsRegistrySubkey, kNbbSensitivityRegistryValue, sensitivity) ||
        TryResolveMouseSensitivityFromOptionsTxt(sensitivity)) {
        resolved.sensitivityAutomatic = std::clamp(sensitivity, 0.0, 1.0);
    }

    double crosshairCorrection = 0.0;
    if (TryReadRegistryDouble(HKEY_CURRENT_USER, kNbbPrefsRegistrySubkey, kNbbCrosshairCorrectionRegistryValue, crosshairCorrection)) {
        resolved.crosshairCorrectionDeg = std::clamp(crosshairCorrection, -1.0, 1.0);
    }

    s_cachedNbbBoatAngleSettings = resolved;
    s_cachedNbbBoatAngleSettingsRefreshMs = now;
    s_cachedNbbBoatAngleSettingsInitialized = true;
    return resolved;
}

static uint32_t StrongholdMovementMaskForVirtualKey(int virtualKey) {
    switch (virtualKey) {
    case 'W':
        return kMoveKeyForward;
    case 'S':
        return kMoveKeyBack;
    case 'A':
        return kMoveKeyLeft;
    case 'D':
        return kMoveKeyRight;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return kMoveKeySprint;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return kMoveKeySneak;
    default:
        return 0;
    }
}

static void AdvanceStrongholdLivePlayerPose() {
    const int mouseDeltaX = s_pendingStrongholdMouseDeltaX.exchange(0, std::memory_order_relaxed);
    const int mouseDeltaY = s_pendingStrongholdMouseDeltaY.exchange(0, std::memory_order_relaxed);
    (void)mouseDeltaY; // reserved for future pitch-aware overlays

    const auto now = std::chrono::steady_clock::now();
    if (!s_strongholdLivePlayerPose.valid) {
        s_strongholdLivePlayerPose.lastUpdate = now;
        return;
    }

    double dtSeconds = std::chrono::duration<double>(now - s_strongholdLivePlayerPose.lastUpdate).count();
    if (!std::isfinite(dtSeconds) || dtSeconds < 0.0) dtSeconds = 0.0;
    dtSeconds = std::clamp(dtSeconds, 0.0, 0.25);
    s_strongholdLivePlayerPose.lastUpdate = now;

    if (!IsStrongholdLiveTrackingInputAllowed()) return;

    if (mouseDeltaX != 0) {
        const NbbBoatAngleSettings settings = GetResolvedNbbBoatAngleSettings();
        const double yawPerCountDeg = MinecraftYawDegreesPerMouseCount(std::clamp(settings.sensitivityAutomatic, 0.0, 1.0));
        s_strongholdLivePlayerPose.yawDeg = NormalizeDegrees(s_strongholdLivePlayerPose.yawDeg + mouseDeltaX * yawPerCountDeg);
    }

    const uint32_t movementMask = s_strongholdMovementKeyMask.load(std::memory_order_relaxed);
    int forwardInput = 0;
    int strafeInput = 0;
    if ((movementMask & kMoveKeyForward) != 0u) forwardInput += 1;
    if ((movementMask & kMoveKeyBack) != 0u) forwardInput -= 1;
    if ((movementMask & kMoveKeyRight) != 0u) strafeInput += 1;
    if ((movementMask & kMoveKeyLeft) != 0u) strafeInput -= 1;
    if (forwardInput == 0 && strafeInput == 0) return;

    const double yawRad = DegreesToRadians(s_strongholdLivePlayerPose.yawDeg);
    const double forwardX = -std::sin(yawRad);
    const double forwardZ = std::cos(yawRad);
    const double rightX = -std::cos(yawRad);
    const double rightZ = -std::sin(yawRad);

    double moveX = forwardX * static_cast<double>(forwardInput) + rightX * static_cast<double>(strafeInput);
    double moveZ = forwardZ * static_cast<double>(forwardInput) + rightZ * static_cast<double>(strafeInput);
    const double length = std::sqrt(moveX * moveX + moveZ * moveZ);
    if (length <= 1e-9) return;
    moveX /= length;
    moveZ /= length;

    double speedBlocksPerSecond = kMinecraftWalkSpeedBlocksPerSecond;
    const bool sprintHeld = (movementMask & kMoveKeySprint) != 0u;
    const bool sneakHeld = (movementMask & kMoveKeySneak) != 0u;
    if (sneakHeld) {
        speedBlocksPerSecond = kMinecraftSneakSpeedBlocksPerSecond;
    } else if (sprintHeld && forwardInput > 0) {
        speedBlocksPerSecond = kMinecraftSprintSpeedBlocksPerSecond;
    }

    // Pose is stored in overworld units. While player is in nether, convert
    // nether movement blocks to overworld scale for consistent targeting math.
    const double dimensionScale = s_strongholdLivePlayerPose.isInNether ? 8.0 : 1.0;
    const double stepDistance = speedBlocksPerSecond * dtSeconds * dimensionScale;
    s_strongholdLivePlayerPose.xInOverworld += moveX * stepDistance;
    s_strongholdLivePlayerPose.zInOverworld += moveZ * stepDistance;
}

static void AnchorStrongholdLivePlayerPose(double xInOverworld, double zInOverworld, double yawDeg, bool isInNether) {
    s_strongholdLivePlayerPose.valid = true;
    s_strongholdLivePlayerPose.xInOverworld = xInOverworld;
    s_strongholdLivePlayerPose.zInOverworld = zInOverworld;
    s_strongholdLivePlayerPose.yawDeg = NormalizeDegrees(yawDeg);
    s_strongholdLivePlayerPose.isInNether = isInNether;
    s_strongholdLivePlayerPose.lastUpdate = std::chrono::steady_clock::now();
}

static NbbStandardDeviationSettings GetResolvedNbbStandardDeviationSettings() {
    const ULONGLONG now = GetTickCount64();
    if (s_cachedNbbStandardDeviationSettingsInitialized &&
        now - s_cachedNbbStandardDeviationSettingsRefreshMs <= kNbbPrefsRefreshIntervalMs) {
        return s_cachedNbbStandardDeviationSettings;
    }

    NbbStandardDeviationSettings resolved;
    double parsed = 0.0;
    if (TryReadRegistryDouble(HKEY_CURRENT_USER, kNbbPrefsRegistrySubkey, kNbbSigmaRegistryValue, parsed)) {
        resolved.sigmaNormal = std::clamp(parsed, 0.001, 1.0);
    }
    if (TryReadRegistryDouble(HKEY_CURRENT_USER, kNbbPrefsRegistrySubkey, kNbbSigmaAltRegistryValue, parsed)) {
        resolved.sigmaAlt = std::clamp(parsed, 0.001, 1.0);
    }
    if (TryReadRegistryDouble(HKEY_CURRENT_USER, kNbbPrefsRegistrySubkey, kNbbSigmaManualRegistryValue, parsed)) {
        resolved.sigmaManual = std::clamp(parsed, 0.001, 1.0);
    }
    if (TryReadRegistryDouble(HKEY_CURRENT_USER, kNbbPrefsRegistrySubkey, kNbbSigmaBoatRegistryValue, parsed)) {
        resolved.sigmaBoat = std::clamp(parsed, 0.0001, 1.0);
    }

    s_cachedNbbStandardDeviationSettings = resolved;
    s_cachedNbbStandardDeviationSettingsRefreshMs = now;
    s_cachedNbbStandardDeviationSettingsInitialized = true;
    return resolved;
}

static NbbAngleAdjustmentSettings GetResolvedNbbAngleAdjustmentSettings() {
    const ULONGLONG now = GetTickCount64();
    if (s_cachedNbbAngleAdjustmentSettingsInitialized &&
        now - s_cachedNbbAngleAdjustmentSettingsRefreshMs <= kNbbPrefsRefreshIntervalMs) {
        return s_cachedNbbAngleAdjustmentSettings;
    }

    NbbAngleAdjustmentSettings resolved;
    double parsed = 0.0;
    if (TryReadRegistryDouble(HKEY_CURRENT_USER, kNbbPrefsRegistrySubkey, kNbbAngleAdjustmentTypeRegistryValue, parsed)) {
        const int type = static_cast<int>(std::llround(parsed));
        if (type >= 0 && type <= 2) resolved.adjustmentType = type;
    }
    if (TryReadRegistryDouble(HKEY_CURRENT_USER, kNbbPrefsRegistrySubkey, kNbbResolutionHeightRegistryValue, parsed)) {
        resolved.resolutionHeight = std::clamp(parsed, 1.0, 16384.0);
    }
    if (TryReadRegistryDouble(HKEY_CURRENT_USER, kNbbPrefsRegistrySubkey, kNbbCustomAdjustmentRegistryValue, parsed)) {
        resolved.customAdjustment = std::clamp(parsed, 0.0001, 1.0);
    }

    s_cachedNbbAngleAdjustmentSettings = resolved;
    s_cachedNbbAngleAdjustmentSettingsRefreshMs = now;
    s_cachedNbbAngleAdjustmentSettingsInitialized = true;
    return resolved;
}

static double ComputeNbbAngleCorrectionStepDegrees(double throwVerticalAngleDeg) {
    const NbbAngleAdjustmentSettings settings = GetResolvedNbbAngleAdjustmentSettings();
    switch (settings.adjustmentType) {
    case 1: {
        const double toRad = kPi / 180.0;
        const double denominator = std::cos(throwVerticalAngleDeg * toRad);
        if (std::abs(denominator) <= 1e-9) return 0.01;
        const double radians = std::atan(2.0 * std::tan(15.0 * toRad) / settings.resolutionHeight) / denominator;
        const double degrees = radians / toRad;
        if (!std::isfinite(degrees) || degrees <= 0.0) return 0.01;
        return degrees;
    }
    case 2:
        return settings.customAdjustment;
    case 0:
    default:
        return 0.01;
    }
}

static bool EndsWithIgnoreCaseAscii(const std::string& value, const std::string& suffix) {
    if (value.size() < suffix.size()) return false;
    size_t offset = value.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(value[offset + i]);
        const unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

static bool TryParseClipboardDimensionToken(const std::string& worldToken, ClipboardDimension& outDimension) {
    if (EndsWithIgnoreCaseAscii(worldToken, "overworld")) {
        outDimension = ClipboardDimension::Overworld;
        return true;
    }
    if (EndsWithIgnoreCaseAscii(worldToken, "the_nether") || EndsWithIgnoreCaseAscii(worldToken, "nether")) {
        outDimension = ClipboardDimension::Nether;
        return true;
    }
    if (EndsWithIgnoreCaseAscii(worldToken, "the_end") || EndsWithIgnoreCaseAscii(worldToken, "end")) {
        outDimension = ClipboardDimension::End;
        return true;
    }
    outDimension = ClipboardDimension::Unknown;
    return false;
}

static bool TryParseF3CClipboardData(const std::string& clipboardText, ParsedF3CClipboardData& outData) {
    std::string text = clipboardText;
    TrimAsciiWhitespaceInPlace(text);
    if (text.rfind("/execute in ", 0) != 0) return false;

    std::istringstream stream(text);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) tokens.push_back(token);
    if (tokens.size() != 11) return false;
    if (tokens[0] != "/execute" || tokens[1] != "in" || tokens[3] != "run" || tokens[4] != "tp") return false;

    ClipboardDimension dimension = ClipboardDimension::Unknown;
    if (!TryParseClipboardDimensionToken(tokens[2], dimension)) return false;

    try {
        outData.x = std::stod(tokens[6]);
        outData.y = std::stod(tokens[7]);
        outData.z = std::stod(tokens[8]);
        outData.horizontalAngle = std::stod(tokens[9]);
        outData.verticalAngle = std::stod(tokens[10]);
    } catch (...) {
        return false;
    }
    outData.dimension = dimension;
    return true;
}

static bool ReadClipboardTextUtf8(std::string& outText) {
    outText.clear();
    HWND ownerHwnd = g_minecraftHwnd.load();
    if (!OpenClipboard(ownerHwnd)) return false;

    struct ClipboardGuard {
        ~ClipboardGuard() { CloseClipboard(); }
    } guard;

    HANDLE dataHandle = GetClipboardData(CF_UNICODETEXT);
    if (!dataHandle) return false;

    const wchar_t* wideData = static_cast<const wchar_t*>(GlobalLock(dataHandle));
    if (!wideData) return false;

    std::wstring wideText(wideData);
    GlobalUnlock(dataHandle);
    outText = WideToUtf8(wideText);
    TrimAsciiWhitespaceInPlace(outText);
    return !outText.empty();
}

static bool TryResolveBoatInitAngle(double rawAngleDeg, float& outBoatAngleDeg) {
    if (!std::isfinite(rawAngleDeg)) return false;
    if (std::abs(rawAngleDeg) > 360.0) return false;

    // Keep NBB's existing measurement behavior for first boat setup validation.
    const double increment = (rawAngleDeg >= 0.0) ? kBoatInitPositiveIncrementDeg : kBoatInitNegativeIncrementDeg;
    const float candidate = static_cast<float>(std::round(rawAngleDeg / increment) * increment);
    const double roundedCandidate = std::round(static_cast<double>(candidate) * 100.0) / 100.0;
    if (std::abs(roundedCandidate - rawAngleDeg) > kBoatInitErrorLimitDeg) return false;

    outBoatAngleDeg = candidate;
    return true;
}

static double ApplyNbbCorrectedHorizontalAngle(double angleDeg, double crosshairCorrectionDeg) {
    double alpha = angleDeg + crosshairCorrectionDeg;
    // Match NBB EnderEyeThrow#getCorrectedHorizontalAngle.
    alpha -= 0.000824 * std::sin((alpha + 45.0) * kPi / 180.0);
    return alpha;
}

static double ComputeNbbPreciseBoatHorizontalAngle(double angleDeg, double sensitivity, double crosshairCorrectionDeg, double boatAngleDeg) {
    double preMultiplier = sensitivity * 0.6 + 0.2;
    preMultiplier = preMultiplier * preMultiplier * preMultiplier * 8.0;
    const double minInc = preMultiplier * 0.15;
    const double snapped = boatAngleDeg + std::round((angleDeg - boatAngleDeg) / minInc) * minInc;
    return ApplyNbbCorrectedHorizontalAngle(snapped, crosshairCorrectionDeg);
}

static bool IsSameThrowForDedup(const ParsedEyeThrow& a, const ParsedEyeThrow& b) {
    return std::abs(a.xInOverworld - b.xInOverworld) <= 1e-9 && std::abs(a.zInOverworld - b.zInOverworld) <= 1e-9 &&
           std::abs(a.angleDeg - b.angleDeg) <= 1e-9 && a.type == b.type;
}

static bool ExtractJsonEnclosedAfterKey(const std::string& json, const std::string& key, char openCh, char closeCh, std::string& outBlock) {
    outBlock.clear();
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) return false;

    const size_t colonPos = json.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos) return false;

    const size_t startPos = json.find(openCh, colonPos + 1);
    if (startPos == std::string::npos) return false;

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = startPos; i < json.size(); ++i) {
        const char c = json[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
            continue;
        }
        if (c == openCh) {
            ++depth;
        } else if (c == closeCh) {
            --depth;
            if (depth == 0) {
                outBlock = json.substr(startPos, i - startPos + 1);
                return true;
            }
        }
    }

    return false;
}

static std::vector<std::string> ExtractTopLevelObjectsFromArray(const std::string& arrayBlock) {
    std::vector<std::string> objects;
    if (arrayBlock.empty()) return objects;

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    size_t objectStart = std::string::npos;

    for (size_t i = 0; i < arrayBlock.size(); ++i) {
        const char c = arrayBlock[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
            continue;
        }

        if (c == '{') {
            if (depth == 0) objectStart = i;
            ++depth;
        } else if (c == '}') {
            if (depth <= 0) continue;
            --depth;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(arrayBlock.substr(objectStart, i - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
    }

    return objects;
}

static bool ExtractRegexString(const std::string& input, const std::regex& pattern, std::string& out) {
    std::smatch match;
    if (!std::regex_search(input, match, pattern) || match.size() < 2) return false;
    out = match[1].str();
    return true;
}

static double GetVarianceFromPositionImprecision(double distance2, double throwX, double throwZ) {
    if (distance2 <= 1e-9) return 0.0;

    // From NBB Posterior#getVarianceFromPositionImprecision.
    const double fx = throwX - std::floor(throwX);
    const double fz = throwZ - std::floor(throwZ);
    const bool xCorner = std::abs(fx - 0.3) < 1e-6 || std::abs(fx - 0.7) < 1e-6;
    const bool zCorner = std::abs(fz - 0.3) < 1e-6 || std::abs(fz - 0.7) < 1e-6;
    if (xCorner && zCorner) return 0.0;

    const double maxLateralError = 0.005 * std::sqrt(2.0) * 180.0 / kPi;
    return (maxLateralError * maxLateralError) / distance2 / 6.0;
}

static double ComputeChunkAngleObjective(int chunkX, int chunkZ, const std::vector<ParsedEyeThrow>& throws) {
    if (throws.empty()) return std::numeric_limits<double>::infinity();

    constexpr double kChunkCoord = 8.0; // NBB pre-1.19 chunk aim coordinate.
    const double targetX = chunkX * 16.0 + kChunkCoord;
    const double targetZ = chunkZ * 16.0 + kChunkCoord;

    double objective = 0.0;
    for (const ParsedEyeThrow& t : throws) {
        const double dx = targetX - t.xInOverworld;
        const double dz = targetZ - t.zInOverworld;
        const double gamma = -std::atan2(dx, dz) * 180.0 / kPi;
        const double delta = NormalizeDegrees(gamma - t.angleDeg);

        const double sigma = SigmaDegreesForThrowType(t.type);
        const double variance = std::max(1e-8, sigma * sigma + GetVarianceFromPositionImprecision(dx * dx + dz * dz, t.xInOverworld, t.zInOverworld));
        objective += (delta * delta) / variance;
    }

    return objective;
}

static bool ComputeChunkThrowObjectiveTerm(int chunkX, int chunkZ, const ParsedEyeThrow& throwData, double& outObjectiveTerm) {
    outObjectiveTerm = 0.0;
    constexpr double kChunkCoord = 8.0; // NBB pre-1.19 chunk aim coordinate.
    const double targetX = chunkX * 16.0 + kChunkCoord;
    const double targetZ = chunkZ * 16.0 + kChunkCoord;

    const double dx = targetX - throwData.xInOverworld;
    const double dz = targetZ - throwData.zInOverworld;
    const double gamma = -std::atan2(dx, dz) * 180.0 / kPi;
    const double delta = NormalizeDegrees(gamma - throwData.angleDeg);
    const double sigma = SigmaDegreesForThrowType(throwData.type);
    const double variance =
        std::max(1e-8, sigma * sigma + GetVarianceFromPositionImprecision(dx * dx + dz * dz, throwData.xInOverworld, throwData.zInOverworld));
    outObjectiveTerm = (delta * delta) / variance;
    return std::isfinite(outObjectiveTerm);
}

static std::vector<StrongholdRingInfo> BuildStrongholdRings() {
    std::vector<StrongholdRingInfo> rings;
    rings.reserve(kStrongholdRingCount);

    int strongholdsInRing = 1;
    int currentStrongholds = 0;
    for (int ring = 0; ring < kStrongholdRingCount; ++ring) {
        strongholdsInRing += (2 * strongholdsInRing) / (ring + 1);
        strongholdsInRing = std::min(strongholdsInRing, kStrongholdCount - currentStrongholds);
        currentStrongholds += strongholdsInRing;

        StrongholdRingInfo info;
        info.strongholdsInRing = strongholdsInRing;
        info.ringIndex = ring;
        info.innerRadius = static_cast<double>(kStrongholdDistParam) * ((4.0 + ring * 6.0) - 1.25);
        info.outerRadius = static_cast<double>(kStrongholdDistParam) * ((4.0 + ring * 6.0) + 1.25);
        info.innerRadiusPostSnapping = info.innerRadius - (kStrongholdSnappingRadius + 1.0) * std::sqrt(2.0);
        info.outerRadiusPostSnapping = info.outerRadius + (kStrongholdSnappingRadius + 1.0) * std::sqrt(2.0);
        rings.push_back(info);
    }

    return rings;
}

static const std::vector<StrongholdRingInfo>& GetStrongholdRings() {
    static const std::vector<StrongholdRingInfo> rings = BuildStrongholdRings();
    return rings;
}

static double ComputeMaxStrongholdDistanceBlocks(double throwXInOverworld, double throwZInOverworld) {
    const auto& rings = GetStrongholdRings();
    if (rings.empty()) return 5000.0;

    const double playerRadiusInChunks = std::sqrt(throwXInOverworld * throwXInOverworld + throwZInOverworld * throwZInOverworld) / 16.0;
    double maxDistanceInChunks = std::numeric_limits<double>::infinity();
    for (const StrongholdRingInfo& ring : rings) {
        const double inner = ring.innerRadius * ring.innerRadius + playerRadiusInChunks * playerRadiusInChunks -
                             2.0 * playerRadiusInChunks * ring.innerRadius * std::cos(kPi / ring.strongholdsInRing);
        const double outer = ring.outerRadius * ring.outerRadius + playerRadiusInChunks * playerRadiusInChunks -
                             2.0 * playerRadiusInChunks * ring.outerRadius * std::cos(kPi / ring.strongholdsInRing);
        const double maxCandidate = std::sqrt(std::max(inner, outer));
        if (maxCandidate < maxDistanceInChunks) maxDistanceInChunks = maxCandidate;
    }

    if (!std::isfinite(maxDistanceInChunks)) return 5000.0;
    return (maxDistanceInChunks + std::sqrt(2.0) * (kStrongholdSnappingRadius + 0.5)) * 16.0;
}

static const StrongholdRingInfo* GetStrongholdRingForChunkRadius(double chunkR) {
    const auto& rings = GetStrongholdRings();
    for (const StrongholdRingInfo& ring : rings) {
        if (chunkR >= ring.innerRadiusPostSnapping && chunkR <= ring.outerRadiusPostSnapping) return &ring;
    }
    return nullptr;
}

struct NbbApproximatedDensityCache {
    bool initialized = false;
    std::vector<double> density;
    std::vector<double> cumulativePolar;
};

static int FloorDivBy4(int value) {
    if (value >= 0) return value / 4;
    return -(((-value) + 3) / 4);
}

static void EnsureNbbApproximatedDensityInitialized(NbbApproximatedDensityCache& cache) {
    if (cache.initialized) return;

    const int length = kStrongholdMaxChunk + 5;
    std::vector<double> densityPreSnapping(static_cast<size_t>(length), 0.0);
    for (const StrongholdRingInfo& ring : GetStrongholdRings()) {
        const int c0 = static_cast<int>(ring.innerRadius);
        const int c1 = static_cast<int>(ring.outerRadius);
        for (int i = c0; i <= c1 && i < length; ++i) {
            if (i <= 0) continue;
            double rho = ring.strongholdsInRing / (2.0 * kPi * (ring.outerRadius - ring.innerRadius) * static_cast<double>(i));
            if (i == c0 || i == c1) rho *= 0.5;
            densityPreSnapping[static_cast<size_t>(i)] = rho;
        }
    }

    std::unordered_map<int, int> offsetWeights;
    for (int i = -26; i <= 30; ++i) {
        const int chunkOffset = FloorDivBy4(i);
        offsetWeights[-chunkOffset] += 1;
    }

    const int filterRadius = static_cast<int>(std::ceil(kStrongholdSnappingRadius * std::sqrt(2.0)));
    std::vector<double> filter(static_cast<size_t>(filterRadius + 1), 0.0);
    double sum = 0.0;
    constexpr int sampleCount = 200;
    for (int k = -kStrongholdSnappingRadius; k <= kStrongholdSnappingRadius; ++k) {
        const int xOffsetWeight = offsetWeights[k];
        for (int l = -kStrongholdSnappingRadius; l <= kStrongholdSnappingRadius; ++l) {
            const int zOffsetWeight = offsetWeights[l];
            const int w = xOffsetWeight * zOffsetWeight;
            const double radial = std::sqrt(static_cast<double>(k * k + l * l));
            for (int i = 0; i < sampleCount; ++i) {
                const double phi = 2.0 * kPi * static_cast<double>(i) / sampleCount;
                int dr = static_cast<int>(std::llround(radial * std::sin(phi)));
                if (dr < 0) dr = -dr;
                if (dr > filterRadius) dr = filterRadius;
                filter[static_cast<size_t>(dr)] += static_cast<double>(w);
                sum += (dr == 0 ? static_cast<double>(w) : 2.0 * static_cast<double>(w));
            }
        }
    }
    if (sum > 0.0) {
        for (double& value : filter) value /= sum;
    }

    cache.density.assign(static_cast<size_t>(length), 0.0);
    for (int i = 0; i < length; ++i) {
        double convolved = 0.0;
        for (int j = -filterRadius; j <= filterRadius; ++j) {
            const int source = i + j;
            if (source < 0 || source >= length) continue;
            convolved += densityPreSnapping[static_cast<size_t>(source)] * filter[static_cast<size_t>(std::abs(j))];
        }
        cache.density[static_cast<size_t>(i)] = convolved;
    }

    cache.cumulativePolar.assign(static_cast<size_t>(length), 0.0);
    double cumsum = 0.0;
    for (int i = 0; i < length; ++i) {
        cumsum += cache.density[static_cast<size_t>(i)] * static_cast<double>(i) * 2.0 * kPi;
        cache.cumulativePolar[static_cast<size_t>(i)] = cumsum;
    }

    cache.initialized = true;
}

static const NbbApproximatedDensityCache& GetNbbApproximatedDensityCache() {
    static NbbApproximatedDensityCache cache;
    EnsureNbbApproximatedDensityInitialized(cache);
    return cache;
}

static double NbbApproximatedDensityAtChunk(double chunkX, double chunkZ) {
    const auto& cache = GetNbbApproximatedDensityCache();
    const double k = std::sqrt(chunkX * chunkX + chunkZ * chunkZ);
    const int i0 = static_cast<int>(k);
    const int i1 = i0 + 1;
    if (i0 < 0 || i1 < 0 || i1 >= static_cast<int>(cache.density.size())) return 0.0;
    const double t = k - static_cast<double>(i0);
    return (1.0 - t) * cache.density[static_cast<size_t>(i0)] + t * cache.density[static_cast<size_t>(i1)];
}

static double NbbApproximatedDensityCumulativePolar(double radiusInChunks) {
    if (radiusInChunks < 0.0) return 0.0;
    const auto& cache = GetNbbApproximatedDensityCache();
    const double k = radiusInChunks;
    const int i0 = static_cast<int>(k);
    const int i1 = i0 + 1;
    if (i0 < 0) return 0.0;
    if (i1 >= static_cast<int>(cache.cumulativePolar.size())) { return cache.cumulativePolar.back(); }
    const double t = k - static_cast<double>(i0);
    return (1.0 - t) * cache.cumulativePolar[static_cast<size_t>(i0)] + t * cache.cumulativePolar[static_cast<size_t>(i1)];
}

static double NbbOrthogonalComponent(double ax, double az, double ux, double uz) {
    const double uParallelMag = ux * ax + uz * az;
    const double uParallelX = ux * uParallelMag;
    const double uParallelZ = uz * uParallelMag;
    const double uOrthX = uParallelX - ax;
    const double uOrthZ = uParallelZ - az;
    return uz * uOrthX - ux * uOrthZ;
}

static double NbbProjectAndGetMajorComponent(double ax, double az, double ux, double uz, bool majorX) {
    const double projMag = ax * ux + az * uz;
    return majorX ? (ux * projMag) : (uz * projMag);
}

static double NbbFindCircleIntersection(double ox, double oz, double ux, double uz, double radius, bool majorX) {
    const double oDotU = ox * ux + oz * uz;
    const double a = oDotU * oDotU + radius * radius - ox * ox - oz * oz;
    if (a < 0.0) return 0.0;
    const double b = -oDotU - std::sqrt(a);
    return majorX ? (ox + b * ux) : (oz + b * uz);
}

static double NbbGetIterStartMajor(double oMajor, double oMinor, double ux, double uz, double vx, double vz, bool majorX,
                                   bool majorPositive) {
    if (oMajor * oMajor + oMinor * oMinor <= static_cast<double>(kStrongholdMaxChunk * kStrongholdMaxChunk)) return oMajor;

    const double ox = majorX ? oMajor : oMinor;
    const double oz = majorX ? oMinor : oMajor;
    const double uOrthMag = NbbOrthogonalComponent(-ox, -oz, ux, uz);
    const double vOrthMag = NbbOrthogonalComponent(-ox, -oz, vx, vz);

    if (uOrthMag > 0.0 && vOrthMag < 0.0) {
        const double oMag = std::sqrt(ox * ox + oz * oz);
        if (oMag <= 1e-12) return oMajor;
        const double ix = ox / oMag * kStrongholdMaxChunk;
        const double iz = oz / oMag * kStrongholdMaxChunk;
        const double m1 = oMajor + NbbProjectAndGetMajorComponent(ix - ox, iz - oz, ux, uz, majorX);
        const double m2 = oMajor + NbbProjectAndGetMajorComponent(ix - ox, iz - oz, vx, vz, majorX);
        return (majorPositive ^ (m1 > m2)) ? m1 : m2;
    }

    const double iUMajor = NbbFindCircleIntersection(ox, oz, ux, uz, static_cast<double>(kStrongholdMaxChunk), majorX);
    const double iVMajor = NbbFindCircleIntersection(ox, oz, vx, vz, static_cast<double>(kStrongholdMaxChunk), majorX);
    if (iUMajor != 0.0 || iVMajor != 0.0) {
        if (iUMajor != 0.0 && iVMajor != 0.0) { return (majorPositive ^ (iUMajor > iVMajor)) ? iUMajor : iVMajor; }
        return iUMajor != 0.0 ? iUMajor : iVMajor;
    }
    return oMajor;
}

static std::vector<std::pair<int, int>> BuildRayCandidateChunks(const ParsedEyeThrow& firstThrow, double toleranceRadians) {
    std::vector<std::pair<int, int>> candidates;
    const double range = 5000.0 / 16.0;
    const double phi = DegreesToRadians(firstThrow.angleDeg);

    const double dx = -std::sin(phi);
    const double dz = std::cos(phi);
    const double ux = -std::sin(phi - toleranceRadians);
    const double uz = std::cos(phi - toleranceRadians);
    const double vx = -std::sin(phi + toleranceRadians);
    const double vz = std::cos(phi + toleranceRadians);

    const bool majorX = std::cos(phi) * std::cos(phi) < 0.5;
    const bool majorPositive = majorX ? (-std::sin(phi) > 0.0) : (std::cos(phi) > 0.0);

    constexpr double kChunkCoord = 8.0;
    const double originMajor =
        ((majorX ? firstThrow.xInOverworld : firstThrow.zInOverworld) - kChunkCoord) / 16.0;
    const double originMinor =
        ((majorX ? firstThrow.zInOverworld : firstThrow.xInOverworld) - kChunkCoord) / 16.0;

    const double iterStartMajor = NbbGetIterStartMajor(originMajor, originMinor, ux, uz, vx, vz, majorX, majorPositive);
    const double uk = majorX ? (uz / ux) : (ux / uz);
    const double vk = majorX ? (vz / vx) : (vx / vz);
    const bool rightPositive = majorPositive ? (vk - uk > 0.0) : (uk - vk > 0.0);

    int i = static_cast<int>(majorPositive ? std::ceil(iterStartMajor) : std::floor(iterStartMajor));
    std::unordered_set<unsigned long long> seen;
    while ((majorX ? (i - iterStartMajor) / dx : (i - iterStartMajor) / dz) < range) {
        const double minorU = originMinor + uk * (i - originMajor);
        const double minorV = originMinor + vk * (i - originMajor);

        int j = static_cast<int>(rightPositive ? std::ceil(minorU) : std::floor(minorU));
        j = std::clamp(j, -kStrongholdMaxChunk, kStrongholdMaxChunk);

        while (true) {
            if (rightPositive) {
                if (!(j < minorV) || j > kStrongholdMaxChunk) break;
            } else {
                if (!(j > minorV) || j < -kStrongholdMaxChunk) break;
            }

            const int chunkX = majorX ? i : j;
            const int chunkZ = majorX ? j : i;
            if (chunkX >= -kStrongholdMaxChunk && chunkX <= kStrongholdMaxChunk && chunkZ >= -kStrongholdMaxChunk &&
                chunkZ <= kStrongholdMaxChunk) {
                const unsigned long long key =
                    (static_cast<unsigned long long>(static_cast<unsigned int>(chunkX)) << 32) | static_cast<unsigned int>(chunkZ);
                if (seen.insert(key).second) candidates.emplace_back(chunkX, chunkZ);
            }

            j += rightPositive ? 1 : -1;
        }

        i += majorPositive ? 1 : -1;
    }

    return candidates;
}

static double ComputeRayPriorWeightForChunk(int chunkX, int chunkZ) {
    constexpr int kSamplesPerAxis = 2;
    double weight = 0.0;
    for (int k = 0; k < kSamplesPerAxis; ++k) {
        const double x = static_cast<double>(chunkX) - 0.5 + static_cast<double>(k) / (kSamplesPerAxis - 1.0);
        for (int l = 0; l < kSamplesPerAxis; ++l) {
            const double z = static_cast<double>(chunkZ) - 0.5 + static_cast<double>(l) / (kSamplesPerAxis - 1.0);
            weight += NbbApproximatedDensityAtChunk(x, z);
        }
    }
    return weight / static_cast<double>(kSamplesPerAxis * kSamplesPerAxis);
}

static bool NormalizePredictionWeights(std::vector<ParsedPrediction>& predictions) {
    double totalWeight = 0.0;
    for (const ParsedPrediction& prediction : predictions) {
        if (std::isfinite(prediction.certainty) && prediction.certainty > 0.0) totalWeight += prediction.certainty;
    }
    if (!(totalWeight > 0.0) || !std::isfinite(totalWeight)) return false;
    for (ParsedPrediction& prediction : predictions) {
        prediction.certainty = std::max(0.0, prediction.certainty) / totalWeight;
    }
    return true;
}

static void ApplyThrowConditionToPredictions(std::vector<ParsedPrediction>& predictions, const ParsedEyeThrow& throwData) {
    constexpr double kChunkCoord = 8.0;
    for (ParsedPrediction& prediction : predictions) {
        const double deltaX = prediction.chunkX * 16.0 + kChunkCoord - throwData.xInOverworld;
        const double deltaZ = prediction.chunkZ * 16.0 + kChunkCoord - throwData.zInOverworld;
        const double gamma = -180.0 / kPi * std::atan2(deltaX, deltaZ);
        double delta = std::fabs(std::fmod(gamma - throwData.angleDeg, 360.0));
        delta = std::min(delta, 360.0 - delta);

        const double sigma = SigmaDegreesForThrowType(throwData.type);
        const double variance =
            sigma * sigma + GetVarianceFromPositionImprecision(deltaX * deltaX + deltaZ * deltaZ, throwData.xInOverworld, throwData.zInOverworld);
        if (!(variance > 0.0) || !std::isfinite(variance)) continue;
        prediction.certainty *= std::exp(-(delta * delta) / (2.0 * variance));
    }
}

static double ClosestStrongholdIntegralForRing(const StrongholdRingInfo& ring, int l, double phiPrime, double dphi, double phiP, double rP,
                                               double dI, bool sameRingAsChunk) {
    constexpr int kIntegrationHalfSpan = 7;
    const double phiPrimeLMu = phiPrime + (l * 2.0 * kPi / ring.strongholdsInRing);
    double pdfint = 0.0;
    double integral = 0.0;

    for (int k = -kIntegrationHalfSpan; k <= kIntegrationHalfSpan; ++k) {
        const double deltaPhi = k * dphi;
        double pdf = 1.0;
        if (sameRingAsChunk) {
            const double term = deltaPhi * ring.innerRadius / (15.0 * std::sqrt(2.0));
            pdf = std::pow(std::max(0.0, 1.0 + term), 4.5) * std::pow(std::max(0.0, 1.0 - term), 4.5);
        }
        pdfint += pdf * dphi;

        const double phiPrimeL = phiPrimeLMu + k * dphi;
        const double gamma = phiP - phiPrimeL;
        const double sinGamma = std::sin(gamma);
        if (std::abs(sinGamma) <= 1e-12) continue;

        const double sinBeta = (rP / dI) * sinGamma;
        if (!(sinBeta < 1.0 && sinBeta > -1.0)) continue;

        const double beta = std::asin(sinBeta);
        const double alpha0 = beta - gamma;
        const double alpha1 = kPi - gamma - beta;
        double r0 = dI * std::sin(alpha0) / sinGamma;
        double r1 = dI * std::sin(alpha1) / sinGamma;

        if (r1 > ring.outerRadiusPostSnapping) r1 = ring.outerRadiusPostSnapping;
        if (r0 < ring.innerRadiusPostSnapping) r0 = ring.innerRadiusPostSnapping;
        if (r0 > ring.outerRadiusPostSnapping) r0 = ring.outerRadiusPostSnapping;
        if (r1 < ring.innerRadiusPostSnapping) r1 = ring.innerRadiusPostSnapping;

        integral += pdf * (NbbApproximatedDensityCumulativePolar(r1) - NbbApproximatedDensityCumulativePolar(r0)) * dphi /
                    ring.strongholdsInRing;
    }

    if (pdfint > 0.0) integral /= pdfint;
    if (!std::isfinite(integral)) return 0.0;
    return std::clamp(integral, 0.0, 1.0);
}

static double ApplyClosestStrongholdConditionForChunk(ParsedPrediction& prediction, const ParsedEyeThrow& referenceThrow) {
    double closestStrongholdProbability = 1.0;
    constexpr double kChunkCoord = 8.0;
    const double deltaX = prediction.chunkX + (kChunkCoord - referenceThrow.xInOverworld) / 16.0;
    const double deltaZ = prediction.chunkZ + (kChunkCoord - referenceThrow.zInOverworld) / 16.0;
    const double rP = std::sqrt(referenceThrow.xInOverworld * referenceThrow.xInOverworld +
                                referenceThrow.zInOverworld * referenceThrow.zInOverworld) /
                      16.0;
    const double dI = std::sqrt(deltaX * deltaX + deltaZ * deltaZ);
    if (dI <= 1e-12) return 0.0;

    const double phiPrime = -std::atan2(static_cast<double>(prediction.chunkX), static_cast<double>(prediction.chunkZ));
    const double phiP = -std::atan2(referenceThrow.xInOverworld, referenceThrow.zInOverworld);
    const double maxDist = ComputeMaxStrongholdDistanceBlocks(referenceThrow.xInOverworld, referenceThrow.zInOverworld) / 16.0;
    const double strongholdRMin = rP - maxDist;
    const double strongholdRMax = rP + maxDist;

    const StrongholdRingInfo* ringChunk = GetStrongholdRingForChunkRadius(
        std::sqrt(static_cast<double>(prediction.chunkX * prediction.chunkX + prediction.chunkZ * prediction.chunkZ)));
    if (!ringChunk) return 0.0;

    for (const StrongholdRingInfo& ring : GetStrongholdRings()) {
        if (strongholdRMax < ring.innerRadius || strongholdRMin > ring.outerRadius) continue;
        const bool sameRing = (ringChunk->ringIndex == ring.ringIndex);
        if (sameRing && std::abs(ringChunk->innerRadius) <= 1e-12) continue;
        const double dphi = sameRing ? (2.0 / 15.0 * 15.0 * std::sqrt(2.0) / ringChunk->innerRadius)
                                     : (2.0 / 15.0 * kPi / ring.strongholdsInRing);

        for (int l = 0; l < ring.strongholdsInRing; ++l) {
            if (sameRing && l == 0) continue;
            const double integral = ClosestStrongholdIntegralForRing(ring, l, phiPrime, dphi, phiP, rP, dI, sameRing);
            closestStrongholdProbability *= (1.0 - integral);
        }
    }

    prediction.certainty *= closestStrongholdProbability;
    return closestStrongholdProbability;
}

static bool ApplyClosestStrongholdCondition(std::vector<ParsedPrediction>& predictions, const ParsedEyeThrow& referenceThrow) {
    if (predictions.empty()) return false;
    std::sort(predictions.begin(), predictions.end(),
              [](const ParsedPrediction& a, const ParsedPrediction& b) { return a.certainty > b.certainty; });

    double totalClosestStrongholdProbability = 0.0;
    int samples = 0;
    constexpr double kProbabilityThreshold = 0.001;
    for (size_t i = 0; i < predictions.size(); ++i) {
        ParsedPrediction& prediction = predictions[i];
        if (i < 100 || prediction.certainty > kProbabilityThreshold) {
            const double probability = ApplyClosestStrongholdConditionForChunk(prediction, referenceThrow);
            totalClosestStrongholdProbability += probability;
            samples += 1;
        } else if (samples > 0) {
            prediction.certainty *= totalClosestStrongholdProbability / samples;
        }
    }

    return NormalizePredictionWeights(predictions);
}

static bool BuildApproxPosteriorPredictionsFromThrows(const std::vector<ParsedEyeThrow>& throws, std::vector<ParsedPrediction>& outPredictions) {
    outPredictions.clear();
    if (throws.empty()) return false;

    const ParsedEyeThrow& firstThrow = throws.front();
    const double sigma0 = SigmaDegreesForThrowType(firstThrow.type);
    const double toleranceRadians = DegreesToRadians(std::min(1.0, 30.0 * sigma0));
    const double maxDistanceBlocks = ComputeMaxStrongholdDistanceBlocks(firstThrow.xInOverworld, firstThrow.zInOverworld);
    const std::vector<std::pair<int, int>> candidateChunks = BuildRayCandidateChunks(firstThrow, toleranceRadians);
    if (candidateChunks.empty()) return false;

    constexpr double kChunkCoord = 8.0;
    outPredictions.reserve(candidateChunks.size());
    for (const auto& chunk : candidateChunks) {
        const int chunkX = chunk.first;
        const int chunkZ = chunk.second;
        const double targetX = chunkX * 16.0 + kChunkCoord;
        const double targetZ = chunkZ * 16.0 + kChunkCoord;
        const double dx = targetX - firstThrow.xInOverworld;
        const double dz = targetZ - firstThrow.zInOverworld;
        const double distanceBlocks = std::sqrt(dx * dx + dz * dz);
        if (distanceBlocks > maxDistanceBlocks) continue;

        const double priorWeight = ComputeRayPriorWeightForChunk(chunkX, chunkZ);
        if (!(priorWeight > 0.0) || !std::isfinite(priorWeight)) continue;

        ParsedPrediction prediction;
        prediction.chunkX = chunkX;
        prediction.chunkZ = chunkZ;
        prediction.certainty = priorWeight;
        outPredictions.push_back(prediction);
    }

    if (outPredictions.empty()) return false;
    if (!NormalizePredictionWeights(outPredictions)) return false;

    for (const ParsedEyeThrow& throwData : throws) {
        ApplyThrowConditionToPredictions(outPredictions, throwData);
        if (!NormalizePredictionWeights(outPredictions)) return false;
    }

    if (!ApplyClosestStrongholdCondition(outPredictions, firstThrow)) return false;

    std::sort(outPredictions.begin(), outPredictions.end(),
              [](const ParsedPrediction& a, const ParsedPrediction& b) { return a.certainty > b.certainty; });
    constexpr size_t kMaxPredictions = 4096;
    if (outPredictions.size() > kMaxPredictions) outPredictions.resize(kMaxPredictions);
    return true;
}

static bool ReweightPredictionsByAdjustedThrows(const std::vector<ParsedPrediction>& predictions,
                                                const std::vector<ParsedEyeThrow>& baseThrows,
                                                const std::vector<ParsedEyeThrow>& adjustedThrows,
                                                std::vector<ParsedPrediction>& outPredictions) {
    outPredictions.clear();
    if (predictions.empty() || baseThrows.empty() || adjustedThrows.empty()) return false;
    const size_t throwCount = std::min(baseThrows.size(), adjustedThrows.size());
    if (throwCount == 0) return false;

    struct WeightedPrediction {
        ParsedPrediction prediction;
        double logWeight = -std::numeric_limits<double>::infinity();
    };

    std::vector<WeightedPrediction> weighted;
    weighted.reserve(predictions.size());
    double maxLogWeight = -std::numeric_limits<double>::infinity();

    for (const ParsedPrediction& prediction : predictions) {
        // Start from NBB posterior certainty, then apply only the relative change from local angle offsets.
        double logWeight = std::log(std::max(1e-12, prediction.certainty));
        bool hadFiniteUpdateTerm = false;

        for (size_t i = 0; i < throwCount; ++i) {
            if (std::abs(adjustedThrows[i].angleDeg - baseThrows[i].angleDeg) <= 1e-9) continue;

            double baseTerm = 0.0;
            double adjustedTerm = 0.0;
            if (!ComputeChunkThrowObjectiveTerm(prediction.chunkX, prediction.chunkZ, baseThrows[i], baseTerm)) continue;
            if (!ComputeChunkThrowObjectiveTerm(prediction.chunkX, prediction.chunkZ, adjustedThrows[i], adjustedTerm)) continue;

            logWeight += -0.5 * (adjustedTerm - baseTerm);
            hadFiniteUpdateTerm = true;
        }

        if (!std::isfinite(logWeight)) continue;
        if (!hadFiniteUpdateTerm) {
            // No valid delta term found (should be rare); keep original posterior for this chunk.
            logWeight = std::log(std::max(1e-12, prediction.certainty));
        }

        WeightedPrediction w;
        w.prediction = prediction;
        w.logWeight = logWeight;
        weighted.push_back(w);
        if (logWeight > maxLogWeight) maxLogWeight = logWeight;
    }

    if (weighted.empty() || !std::isfinite(maxLogWeight)) return false;

    double weightSum = 0.0;
    for (const WeightedPrediction& w : weighted) {
        weightSum += std::exp(w.logWeight - maxLogWeight);
    }
    if (!(weightSum > 0.0) || !std::isfinite(weightSum)) return false;

    outPredictions.reserve(weighted.size());
    for (const WeightedPrediction& w : weighted) {
        ParsedPrediction normalized = w.prediction;
        normalized.certainty = std::exp(w.logWeight - maxLogWeight) / weightSum;
        outPredictions.push_back(normalized);
    }

    std::sort(outPredictions.begin(), outPredictions.end(),
              [](const ParsedPrediction& a, const ParsedPrediction& b) { return a.certainty > b.certainty; });
    return true;
}

static bool TryGetTopPrediction(const std::vector<ParsedPrediction>& predictions, int& outChunkX, int& outChunkZ, double& outCertainty) {
    outChunkX = 0;
    outChunkZ = 0;
    outCertainty = 0.0;
    if (predictions.empty()) return false;

    const ParsedPrediction* best = &predictions.front();
    for (const ParsedPrediction& prediction : predictions) {
        if (prediction.certainty > best->certainty) best = &prediction;
    }

    outChunkX = best->chunkX;
    outChunkZ = best->chunkZ;
    outCertainty = best->certainty;
    return true;
}

static bool TryGetPredictionCertaintyForChunk(const std::vector<ParsedPrediction>& predictions, int chunkX, int chunkZ, double& outCertainty) {
    outCertainty = 0.0;
    for (const ParsedPrediction& prediction : predictions) {
        if (prediction.chunkX == chunkX && prediction.chunkZ == chunkZ) {
            outCertainty = prediction.certainty;
            return true;
        }
    }
    return false;
}

static int FindPredictionRank(const std::vector<ParsedPrediction>& predictions, int chunkX, int chunkZ) {
    for (size_t i = 0; i < predictions.size(); ++i) {
        if (predictions[i].chunkX == chunkX && predictions[i].chunkZ == chunkZ) return static_cast<int>(i + 1);
    }
    return 0;
}

static std::string FormatPredictionDebugLabel(const std::vector<ParsedPrediction>& sortedPredictions, int maxCount, bool netherCoords) {
    if (sortedPredictions.empty() || maxCount <= 0) return "-";

    std::ostringstream out;
    out << std::fixed << std::setprecision(1);

    const int count = std::min(maxCount, static_cast<int>(sortedPredictions.size()));
    for (int i = 0; i < count; ++i) {
        if (i > 0) out << " | ";
        const ParsedPrediction& p = sortedPredictions[static_cast<size_t>(i)];
        int displayX = netherCoords ? p.chunkX * 2 : p.chunkX * 16;
        int displayZ = netherCoords ? p.chunkZ * 2 : p.chunkZ * 16;
        out << "#" << (i + 1) << " " << displayX << "," << displayZ << " " << (p.certainty * 100.0) << "%";
    }
    return out.str();
}

static std::string FormatPredictionCandidateRow(int rank, const ParsedPrediction& prediction, double playerX, double playerZ, double playerYaw,
                                                bool useChunkCenterTarget, bool includeDistanceAndYaw) {
    (void)useChunkCenterTarget;
    // Match NBB display convention: OW uses chunk center, nether uses 2x chunk.
    const double overworldX = prediction.chunkX * 16.0 + 8.0;
    const double overworldZ = prediction.chunkZ * 16.0 + 8.0;
    const double netherX = prediction.chunkX * 2.0;
    const double netherZ = prediction.chunkZ * 2.0;

    const double dx = overworldX - playerX;
    const double dz = overworldZ - playerZ;
    const double dist = std::sqrt(dx * dx + dz * dz);

    double yawDelta = 0.0;
    if (!(dx == 0.0 && dz == 0.0)) {
        const double travelYaw = -std::atan2(dx, dz) * 180.0 / kPi;
        yawDelta = NormalizeDegrees(travelYaw - playerYaw);
    }

    std::ostringstream row;
    row << "#" << rank << " ";
    row << "(" << static_cast<int>(std::llround(overworldX)) << ", " << static_cast<int>(std::llround(overworldZ)) << ") ";
    row << std::fixed << std::setprecision(1) << std::clamp(prediction.certainty * 100.0, 0.0, 100.0) << "%";
    if (includeDistanceAndYaw) {
        row << " ";
        row << std::setprecision(0) << dist << " ";
        row << "(" << static_cast<int>(std::llround(netherX)) << ", " << static_cast<int>(std::llround(netherZ)) << ") ";
        row << std::showpos << std::setprecision(2) << yawDelta;
    }
    return row.str();
}

static bool ComputeNativeTriangulatedChunkFromThrows(const std::vector<ParsedEyeThrow>& throws, int& outChunkX, int& outChunkZ) {
    outChunkX = 0;
    outChunkZ = 0;
    if (throws.size() < 2) return false;

    // Weighted least-squares intersection of throw rays in overworld space.
    double a11 = 0.0;
    double a12 = 0.0;
    double a22 = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;

    for (const ParsedEyeThrow& t : throws) {
        const double phi = DegreesToRadians(t.angleDeg);
        const double dx = -std::sin(phi);
        const double dz = std::cos(phi);
        const double nx = -dz;
        const double nz = dx;

        const double sigma = SigmaDegreesForThrowType(t.type);
        const double weight = std::clamp(1.0 / std::max(1e-8, sigma * sigma), 1.0, 1e6);

        const double ndotp = nx * t.xInOverworld + nz * t.zInOverworld;
        a11 += weight * nx * nx;
        a12 += weight * nx * nz;
        a22 += weight * nz * nz;
        b1 += weight * nx * ndotp;
        b2 += weight * nz * ndotp;
    }

    const double det = a11 * a22 - a12 * a12;
    if (!std::isfinite(det) || std::abs(det) < 1e-9) return false;

    const double intersectionX = (b1 * a22 - b2 * a12) / det;
    const double intersectionZ = (a11 * b2 - a12 * b1) / det;
    if (!std::isfinite(intersectionX) || !std::isfinite(intersectionZ)) return false;

    constexpr double kChunkCoord = 8.0;
    int centerChunkX = static_cast<int>(std::floor((intersectionX - kChunkCoord) / 16.0));
    int centerChunkZ = static_cast<int>(std::floor((intersectionZ - kChunkCoord) / 16.0));

    // Refine by minimizing NBB-like angular objective around the continuous solution.
    constexpr int kSearchRadiusChunks = 12;
    double bestObjective = std::numeric_limits<double>::infinity();
    int bestChunkX = centerChunkX;
    int bestChunkZ = centerChunkZ;

    for (int dz = -kSearchRadiusChunks; dz <= kSearchRadiusChunks; ++dz) {
        for (int dx = -kSearchRadiusChunks; dx <= kSearchRadiusChunks; ++dx) {
            const int candidateChunkX = centerChunkX + dx;
            const int candidateChunkZ = centerChunkZ + dz;
            const double objective = ComputeChunkAngleObjective(candidateChunkX, candidateChunkZ, throws);
            if (objective < bestObjective) {
                bestObjective = objective;
                bestChunkX = candidateChunkX;
                bestChunkZ = candidateChunkZ;
            }
        }
    }

    if (!std::isfinite(bestObjective)) return false;
    outChunkX = bestChunkX;
    outChunkZ = bestChunkZ;
    return true;
}

static bool AreNeighboringChunks(int chunkX1, int chunkZ1, int chunkX2, int chunkZ2) {
    return std::abs(chunkX1 - chunkX2) <= 1 && std::abs(chunkZ1 - chunkZ2) <= 1;
}

static bool TryComputeCombinedCertaintyFallback(const std::vector<ParsedPrediction>& predictions, double& outPercent) {
    outPercent = 0.0;
    if (predictions.size() < 2) return false;

    std::vector<ParsedPrediction> sortedPredictions = predictions;
    std::sort(sortedPredictions.begin(), sortedPredictions.end(),
              [](const ParsedPrediction& a, const ParsedPrediction& b) { return a.certainty > b.certainty; });

    const ParsedPrediction& best = sortedPredictions[0];
    const ParsedPrediction& second = sortedPredictions[1];
    if (best.certainty > 0.95) return false;
    if (!AreNeighboringChunks(best.chunkX, best.chunkZ, second.chunkX, second.chunkZ)) return false;

    const double combined = best.certainty + second.certainty;
    if (combined <= 0.80) return false;

    outPercent = std::clamp(combined * 100.0, 0.0, 100.0);
    return true;
}

static bool TryComputeMismeasureWarningFallback(const std::vector<ParsedEyeThrow>& activeThrows, int bestChunkX, int bestChunkZ,
                                                std::string& outWarningText) {
    outWarningText.clear();
    if (activeThrows.empty()) return false;

    const double targetX = bestChunkX * 16.0 + 8.0;
    const double targetZ = bestChunkZ * 16.0 + 8.0;

    double likelihood = 1.0;
    double expectedLikelihood = 1.0;
    for (const ParsedEyeThrow& t : activeThrows) {
        const double dx = targetX - t.xInOverworld;
        const double dz = targetZ - t.zInOverworld;
        if (dx == 0.0 && dz == 0.0) continue;
        const double gamma = -std::atan2(dx, dz) * 180.0 / kPi;
        const double error = NormalizeDegrees(gamma - t.angleDeg);
        const double sigma = std::max(1e-6, SigmaDegreesForThrowType(t.type));
        likelihood *= std::exp(-0.5 * (error / sigma) * (error / sigma));
        expectedLikelihood *= (1.0 / std::sqrt(2.0));
    }

    if (expectedLikelihood <= 0.0) return false;
    const double likelihoodRatio = likelihood / expectedLikelihood;
    if (likelihoodRatio >= 0.01) return false;

    outWarningText = "Detected unusually large errors, you probably mismeasured or your standard deviation is too low.";
    return true;
}

static double MeasurementErrorPdf(double errorInRadians, double sigmaDegrees) {
    if (sigmaDegrees <= 1e-9) return 0.0;
    double errorDegrees = errorInRadians * 180.0 / kPi;
    return std::exp(-errorDegrees * errorDegrees / (2.0 * sigmaDegrees * sigmaDegrees));
}

static double AngleToChunkFromOverworldPos(int chunkX, int chunkZ, double originX, double originZ) {
    constexpr double kChunkCoord = 8.0;
    const double dx = chunkX * 16.0 + kChunkCoord - originX;
    const double dz = chunkZ * 16.0 + kChunkCoord - originZ;
    return -std::atan2(dx, dz);
}

static double ComputeExpectedTopCertaintyAfterSidewaysMove(const std::vector<ParsedPrediction>& predictions, double throwX, double throwZ,
                                                           double sigmaDegrees) {
    if (predictions.empty()) return 0.0;

    double expectedCertaintyAfterThrow = 0.0;
    double totalOriginalCertainty = 0.0;

    for (size_t i = 0; i < predictions.size(); ++i) {
        const ParsedPrediction& assumed = predictions[i];
        double phiToStronghold = AngleToChunkFromOverworldPos(assumed.chunkX, assumed.chunkZ, throwX, throwZ);
        double certaintyThatPredictionHitsStronghold = 0.0;
        double totalCertaintyAfterSecondThrow = 0.0;

        for (size_t j = 0; j < predictions.size(); ++j) {
            const ParsedPrediction& other = predictions[j];
            if (i == j) {
                // NBB approximation for expected true-chunk likelihood.
                totalCertaintyAfterSecondThrow += assumed.certainty * 0.9;
                certaintyThatPredictionHitsStronghold += assumed.certainty * 0.9;
                continue;
            }

            double phiToPrediction = AngleToChunkFromOverworldPos(other.chunkX, other.chunkZ, throwX, throwZ);
            double errorLikelihood = MeasurementErrorPdf(phiToPrediction - phiToStronghold, sigmaDegrees);
            totalCertaintyAfterSecondThrow += other.certainty * errorLikelihood;
            if (AreNeighboringChunks(assumed.chunkX, assumed.chunkZ, other.chunkX, other.chunkZ)) {
                certaintyThatPredictionHitsStronghold += other.certainty * errorLikelihood;
            }
        }

        if (totalCertaintyAfterSecondThrow <= 1e-9) continue;
        double newCertainty = certaintyThatPredictionHitsStronghold / totalCertaintyAfterSecondThrow;
        expectedCertaintyAfterThrow += newCertainty * assumed.certainty;
        totalOriginalCertainty += assumed.certainty;
    }

    if (totalOriginalCertainty <= 1e-9) return 0.0;
    return expectedCertaintyAfterThrow / totalOriginalCertainty;
}

static double ComputeSidewaysDistanceFor95PercentCertainty(const std::vector<ParsedPrediction>& predictions, const ParsedEyeThrow& lastThrow,
                                                           double phiSideways) {
    double expectedTopCertainty = 0.0;
    double sidewaysDistance = 0.0;
    double sidewaysDistanceIncrement = 5.0;
    bool binarySearching = false;
    const double sigmaDegrees = SigmaDegreesForThrowType(lastThrow.type);

    for (int iteration = 0; iteration < 1000; ++iteration) {
        sidewaysDistance += sidewaysDistanceIncrement * (expectedTopCertainty > 0.95 ? -1.0 : 1.0);
        const double newX = lastThrow.xInOverworld + (-sidewaysDistance * std::sin(phiSideways));
        const double newZ = lastThrow.zInOverworld + (sidewaysDistance * std::cos(phiSideways));
        expectedTopCertainty = ComputeExpectedTopCertaintyAfterSidewaysMove(predictions, newX, newZ, sigmaDegrees);

        if (expectedTopCertainty > 0.95) binarySearching = true;
        if (binarySearching) sidewaysDistanceIncrement *= 0.5;
        if (sidewaysDistanceIncrement <= 0.1) break;
        if (sidewaysDistance > 5000.0) break;
    }

    return sidewaysDistance;
}

static bool TryComputeNextThrowDirectionFallback(const std::vector<ParsedPrediction>& predictions, const std::vector<ParsedEyeThrow>& activeThrows,
                                                 int& outMoveLeftBlocks, int& outMoveRightBlocks, bool forceEvenWhenConfidentBest = false) {
    outMoveLeftBlocks = 0;
    outMoveRightBlocks = 0;
    if (predictions.empty() || activeThrows.empty()) return false;

    std::vector<ParsedPrediction> sortedPredictions = predictions;
    std::sort(sortedPredictions.begin(), sortedPredictions.end(),
              [](const ParsedPrediction& a, const ParsedPrediction& b) { return a.certainty > b.certainty; });

    const double bestCertainty = sortedPredictions.front().certainty;
    if (!forceEvenWhenConfidentBest && !(bestCertainty > 0.05 && bestCertainty < 0.95)) return false;

    std::vector<ParsedPrediction> considered;
    considered.reserve(sortedPredictions.size());
    double cumulativeProbability = 0.0;
    const size_t minimumPredictions = forceEvenWhenConfidentBest ? std::min<size_t>(2, sortedPredictions.size()) : 1;
    for (const ParsedPrediction& prediction : sortedPredictions) {
        if (cumulativeProbability > 0.99 && considered.size() >= minimumPredictions) break;
        cumulativeProbability += std::max(0.0, prediction.certainty);
        considered.push_back(prediction);
    }
    if (considered.empty()) return false;

    const ParsedEyeThrow& lastThrow = activeThrows.back();
    const double phiRight = DegreesToRadians(lastThrow.angleDeg + 90.0);
    const double phiLeft = DegreesToRadians(lastThrow.angleDeg - 90.0);

    const double rightDistance = ComputeSidewaysDistanceFor95PercentCertainty(considered, lastThrow, phiRight);
    const double leftDistance = ComputeSidewaysDistanceFor95PercentCertainty(considered, lastThrow, phiLeft);

    outMoveRightBlocks = std::max(0, static_cast<int>(std::ceil(rightDistance)));
    outMoveLeftBlocks = std::max(0, static_cast<int>(std::ceil(leftDistance)));
    return true;
}

static std::string GetUnlockedStatusLabel(bool autoLockPaused) {
    return autoLockPaused ? "LIVE/UNLOCKED (auto paused)" : "LIVE/UNLOCKED";
}

static void LockStrongholdTargetLocked(StrongholdOverlayRuntimeState& state, int chunkX, int chunkZ, bool isAutoLock) {
    state.targetLocked = true;
    state.lockedChunkX = chunkX;
    state.lockedChunkZ = chunkZ;
    state.lockSourceAuto = isAutoLock;
}

static void ResetStrongholdOverlayLocked(StrongholdOverlayRuntimeState& state, const std::string& message, bool pauseAutoLockUntilThrowClear) {
    state.failCount = 0;
    state.targetLocked = false;
    state.hasLiveTarget = false;
    state.liveTargetFromNativeTriangulation = false;
    state.hasAutoLockedOnNether = false;
    state.wasInNetherLastTick = false;
    state.lockSourceAuto = false;
    state.lastEyeThrowCount = 0;
    state.activeEyeThrowCount = 0;
    state.ignoredThrowsPrefixCount = 0;
    state.lastThrowAngleAdjustmentDeg = 0.0;
    state.lastAdjustmentStepDirection = 0;
    state.perThrowAngleAdjustmentsDeg.clear();
    state.adjustmentUndoStackDeg.clear();
    state.adjustmentRedoStackDeg.clear();
    state.adjustmentHistoryThrowCount = 0;
    state.hasPrediction = false;
    state.usingLiveTarget = true;
    state.relativeYaw = 0.0f;
    state.distanceDisplay = 0.0f;
    state.targetDisplayX = 0;
    state.targetDisplayZ = 0;
    state.playerDisplayX = 0;
    state.playerDisplayZ = 0;
    state.targetNetherX = 0;
    state.targetNetherZ = 0;
    state.estimatedNetherX = 0;
    state.estimatedNetherZ = 0;
    state.playerNetherX = 0;
    state.playerNetherZ = 0;
    state.targetOverworldX = 0;
    state.targetOverworldZ = 0;
    state.estimatedOverworldX = 0;
    state.estimatedOverworldZ = 0;
    state.playerOverworldX = 0;
    state.playerOverworldZ = 0;
    state.hasTopCertainty = false;
    state.topCertaintyPercent = 0.0f;
    state.hasCombinedCertainty = false;
    state.combinedCertaintyPercent = 0.0f;
    state.hasNextThrowDirection = false;
    state.moveLeftBlocks = 0;
    state.moveRightBlocks = 0;
    state.topCandidate1Label.clear();
    state.topCandidate2Label.clear();
    state.warningLabel.clear();
    state.boatState = kBoatStateUninitialized;
    state.boatLabel = "Boat: UNINIT";
    state.modeLabel = "nether";
    state.statusLabel = GetUnlockedStatusLabel(pauseAutoLockUntilThrowClear);
    state.infoLabel = message;
    state.debugBasePredictionsLabel.clear();
    state.debugAdjustedPredictionsLabel.clear();
    state.debugSelectionLabel.clear();
    state.showComputedDetails = false;
    state.lastActiveThrowVerticalAngleDeg = -31.6;
    state.blockAutoLockUntilThrowClear = pauseAutoLockUntilThrowClear;
}

static void ApplyPlayerPoseAndTargetToOverlayState(StrongholdOverlayRuntimeState& state, const StrongholdOverlayConfig& overlayCfg,
                                                   double playerXInOverworld, double playerZInOverworld, double playerYawDeg,
                                                   int targetChunkX, int targetChunkZ, bool playerInNether) {
    // Match NBB convention: target in OW center, nether as 2x chunk coord.
    const double targetX = targetChunkX * 16.0 + 8.0;
    const double targetZ = targetChunkZ * 16.0 + 8.0;
    const double targetNetherX = targetChunkX * 2.0;
    const double targetNetherZ = targetChunkZ * 2.0;

    const double dx = targetX - playerXInOverworld;
    const double dz = targetZ - playerZInOverworld;
    double relativeYaw = 0.0;
    double distance = 0.0;
    if (!(dx == 0.0 && dz == 0.0)) {
        const double targetYaw = -std::atan2(dx, dz) * 180.0 / kPi;
        relativeYaw = NormalizeDegrees(targetYaw - playerYawDeg);
        distance = std::sqrt(dx * dx + dz * dz);
    }

    const int playerNetherX = static_cast<int>(std::round(playerXInOverworld / 8.0));
    const int playerNetherZ = static_cast<int>(std::round(playerZInOverworld / 8.0));
    const int targetNetherXi = static_cast<int>(std::round(targetNetherX));
    const int targetNetherZi = static_cast<int>(std::round(targetNetherZ));
    const int targetOverworldX = static_cast<int>(std::round(targetX));
    const int targetOverworldZ = static_cast<int>(std::round(targetZ));
    const int playerOverworldX = static_cast<int>(std::round(playerXInOverworld));
    const int playerOverworldZ = static_cast<int>(std::round(playerZInOverworld));

    const double yawRad = playerYawDeg * kPi / 180.0;
    const double forwardX = -std::sin(yawRad);
    const double forwardZ = std::cos(yawRad);
    const double estimatedOverworldX = playerXInOverworld + forwardX * distance;
    const double estimatedOverworldZ = playerZInOverworld + forwardZ * distance;
    const int estimatedOverworldXi = static_cast<int>(std::round(estimatedOverworldX));
    const int estimatedOverworldZi = static_cast<int>(std::round(estimatedOverworldZ));
    const int estimatedNetherXi = static_cast<int>(std::round(estimatedOverworldX / 8.0));
    const int estimatedNetherZi = static_cast<int>(std::round(estimatedOverworldZ / 8.0));

    state.targetNetherX = targetNetherXi;
    state.targetNetherZ = targetNetherZi;
    state.estimatedNetherX = estimatedNetherXi;
    state.estimatedNetherZ = estimatedNetherZi;
    state.playerNetherX = playerNetherX;
    state.playerNetherZ = playerNetherZ;
    state.targetOverworldX = targetOverworldX;
    state.targetOverworldZ = targetOverworldZ;
    state.estimatedOverworldX = estimatedOverworldXi;
    state.estimatedOverworldZ = estimatedOverworldZi;
    state.playerOverworldX = playerOverworldX;
    state.playerOverworldZ = playerOverworldZ;

    const bool useNetherCoords = overlayCfg.preferNetherCoords || playerInNether;
    state.usingNetherCoords = useNetherCoords;
    if (useNetherCoords) {
        state.modeLabel = "nether";
        state.targetDisplayX = targetNetherXi;
        state.targetDisplayZ = targetNetherZi;
        state.playerDisplayX = playerNetherX;
        state.playerDisplayZ = playerNetherZ;
    } else {
        state.modeLabel = "overworld";
        state.targetDisplayX = targetOverworldX;
        state.targetDisplayZ = targetOverworldZ;
        state.playerDisplayX = playerOverworldX;
        state.playerDisplayZ = playerOverworldZ;
    }

    state.relativeYaw = static_cast<float>(relativeYaw);
    state.distanceDisplay = static_cast<float>(distance);
}

static bool HttpGetJson(const wchar_t* requestPath, std::string& outJson) {
    outJson.clear();
    if (!s_winHttpApi.EnsureLoaded()) return false;

    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hRequest = nullptr;
    bool success = false;

    do {
        hSession =
            s_winHttpApi.open(L"Toolscreen/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) break;

        s_winHttpApi.setTimeouts(hSession, kStrongholdApiTimeoutMs, kStrongholdApiTimeoutMs, kStrongholdApiTimeoutMs, kStrongholdApiTimeoutMs);

        hConnect = s_winHttpApi.connect(hSession, kStrongholdApiHost, kStrongholdApiPort, 0);
        if (!hConnect) break;

        hRequest =
            s_winHttpApi.openRequest(hConnect, L"GET", requestPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!hRequest) break;

        if (!s_winHttpApi.sendRequest(hRequest, L"Accept: application/json\r\n", static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            break;
        }
        if (!s_winHttpApi.receiveResponse(hRequest, nullptr)) break;

        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        if (!s_winHttpApi.queryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                       &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX)) {
            break;
        }
        if (statusCode != 200) break;

        std::string response;
        while (true) {
            DWORD bytesAvailable = 0;
            if (!s_winHttpApi.queryDataAvailable(hRequest, &bytesAvailable)) break;
            if (bytesAvailable == 0) {
                outJson = std::move(response);
                success = !outJson.empty();
                break;
            }

            std::vector<char> buffer(bytesAvailable);
            DWORD bytesRead = 0;
            if (!s_winHttpApi.readData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) break;
            if (bytesRead == 0) break;
            response.append(buffer.data(), bytesRead);
        }
    } while (false);

    if (hRequest) s_winHttpApi.closeHandle(hRequest);
    if (hConnect) s_winHttpApi.closeHandle(hConnect);
    if (hSession) s_winHttpApi.closeHandle(hSession);
    return success;
}

static bool HttpGetStrongholdJson(std::string& outJson) {
    return HttpGetJson(kStrongholdApiPath, outJson);
}

static bool HttpGetInformationMessagesJson(std::string& outJson) {
    return HttpGetJson(kInformationMessagesApiPath, outJson);
}

static std::string JsonUnescapeBasic(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c != '\\' || i + 1 >= in.size()) {
            out.push_back(c);
            continue;
        }
        char n = in[++i];
        switch (n) {
        case '"':
            out.push_back('"');
            break;
        case '\\':
            out.push_back('\\');
            break;
        case '/':
            out.push_back('/');
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        default:
            // Keep unsupported escape sequences in a readable form.
            out.push_back(n);
            break;
        }
    }
    return out;
}

static void FinalizeParsedStrongholdData(ParsedStrongholdApiData& data) {
    data.eyeThrowCount = static_cast<int>(data.eyeThrows.size());
    data.hasBoatThrow =
        std::any_of(data.eyeThrows.begin(), data.eyeThrows.end(), [](const ParsedEyeThrow& t) { return t.type == EyeThrowType::Boat; });

    if (ComputeNativeTriangulatedChunkFromThrows(data.eyeThrows, data.nativeChunkX, data.nativeChunkZ)) { data.hasNativeTriangulation = true; }

    if (data.predictions.empty()) { BuildApproxPosteriorPredictionsFromThrows(data.eyeThrows, data.predictions); }

    if (!data.predictions.empty()) {
        const ParsedPrediction* bestPrediction = &data.predictions.front();
        for (const ParsedPrediction& prediction : data.predictions) {
            if (prediction.certainty > bestPrediction->certainty) bestPrediction = &prediction;
        }
        data.chunkX = bestPrediction->chunkX;
        data.chunkZ = bestPrediction->chunkZ;
        data.hasPrediction = true;
        if (std::isfinite(bestPrediction->certainty)) {
            data.hasTopCertainty = true;
            data.topCertaintyPercent = std::clamp(bestPrediction->certainty * 100.0, 0.0, 100.0);
        }
    } else if (data.hasNativeTriangulation) {
        data.chunkX = data.nativeChunkX;
        data.chunkZ = data.nativeChunkZ;
        data.hasPrediction = true;
    }
}

static void PollStandaloneClipboardState(bool allowNonBoatThrows) {
    const DWORD clipboardSequence = GetClipboardSequenceNumber();
    if (clipboardSequence != 0 && clipboardSequence == s_standaloneStrongholdState.lastClipboardSequenceNumber) return;

    std::string clipboardText;
    if (!ReadClipboardTextUtf8(clipboardText)) return;
    if (clipboardSequence == 0 && clipboardText == s_standaloneStrongholdState.lastClipboardText) return;

    if (clipboardSequence != 0) { s_standaloneStrongholdState.lastClipboardSequenceNumber = clipboardSequence; }
    s_standaloneStrongholdState.lastClipboardText = clipboardText;

    ParsedF3CClipboardData parsed;
    if (!TryParseF3CClipboardData(clipboardText, parsed)) return;
    if (parsed.dimension != ClipboardDimension::Overworld && parsed.dimension != ClipboardDimension::Nether) return;

    const bool isOverworldSnapshot = parsed.dimension == ClipboardDimension::Overworld;
    const bool isNetherSnapshot = parsed.dimension == ClipboardDimension::Nether;
    const double dimensionScale = isNetherSnapshot ? 8.0 : 1.0;
    s_standaloneStrongholdState.hasPlayerSnapshot = true;
    s_standaloneStrongholdState.playerXInOverworld = parsed.x * dimensionScale;
    s_standaloneStrongholdState.playerZInOverworld = parsed.z * dimensionScale;
    s_standaloneStrongholdState.playerYaw = NormalizeDegrees(parsed.horizontalAngle);
    s_standaloneStrongholdState.isInOverworld = isOverworldSnapshot;
    s_standaloneStrongholdState.isInNether = isNetherSnapshot;
    s_standaloneStrongholdState.parsedSnapshotCounter += 1;

    if (!allowNonBoatThrows) {
        // Boat init is an overworld setup check: first valid capture initializes
        // boat state, then the following capture(s) are used for throw logging.
        if (s_standaloneStrongholdState.boatState != kBoatStateGood) {
            if (!isOverworldSnapshot) return;

            float resolvedBoatAngleDeg = 0.0f;
            if (TryResolveBoatInitAngle(parsed.horizontalAngle, resolvedBoatAngleDeg)) {
                s_standaloneStrongholdState.boatState = kBoatStateGood;
                s_standaloneStrongholdState.hasBoatAngle = true;
                s_standaloneStrongholdState.boatAngleDeg = resolvedBoatAngleDeg;
            } else {
                s_standaloneStrongholdState.boatState = kBoatStateFailed;
                s_standaloneStrongholdState.hasBoatAngle = false;
                s_standaloneStrongholdState.boatAngleDeg = 0.0;
            }
            return;
        }
    } else {
        // Non-boat mode uses standard eye throws and bypasses boat initialization.
        s_standaloneStrongholdState.boatState = kBoatStateUninitialized;
        s_standaloneStrongholdState.hasBoatAngle = false;
        s_standaloneStrongholdState.boatAngleDeg = 0.0;
    }

    // Boat-eye throw logging is overworld-only. Nether snapshots may still
    // update player/dimension display state but must never create throws.
    if (!isOverworldSnapshot) return;

    // Mirror NBB behavior: throw entries only count while looking above horizon.
    if (parsed.verticalAngle > 0.0) { return; }

    ParsedEyeThrow newThrow;
    newThrow.xInOverworld = s_standaloneStrongholdState.playerXInOverworld;
    newThrow.zInOverworld = s_standaloneStrongholdState.playerZInOverworld;
    newThrow.verticalAngleDeg = parsed.verticalAngle;

    double throwAngleDeg = parsed.horizontalAngle;
    if (allowNonBoatThrows) {
        const NbbBoatAngleSettings settings = GetResolvedNbbBoatAngleSettings();
        throwAngleDeg = ApplyNbbCorrectedHorizontalAngle(parsed.horizontalAngle, settings.crosshairCorrectionDeg);
        newThrow.type = EyeThrowType::Normal;
    } else if (s_standaloneStrongholdState.hasBoatAngle) {
        const NbbBoatAngleSettings settings = GetResolvedNbbBoatAngleSettings();
        throwAngleDeg = ComputeNbbPreciseBoatHorizontalAngle(parsed.horizontalAngle, settings.sensitivityAutomatic,
                                                             settings.crosshairCorrectionDeg, s_standaloneStrongholdState.boatAngleDeg);
        newThrow.type = EyeThrowType::Boat;
    } else {
        newThrow.type = EyeThrowType::Boat;
    }
    newThrow.angleDeg = NormalizeDegrees(throwAngleDeg);

    if (!s_standaloneStrongholdState.eyeThrows.empty() &&
        IsSameThrowForDedup(s_standaloneStrongholdState.eyeThrows.back(), newThrow)) {
        return;
    }

    s_standaloneStrongholdState.eyeThrows.push_back(newThrow);
}

static ParsedStrongholdApiData BuildStandaloneStrongholdApiData(bool allowNonBoatThrows) {
    PollStandaloneClipboardState(allowNonBoatThrows);

    ParsedStrongholdApiData data;
    if (!s_standaloneStrongholdState.hasPlayerSnapshot) return data;

    data.playerX = s_standaloneStrongholdState.playerXInOverworld;
    data.playerZ = s_standaloneStrongholdState.playerZInOverworld;
    data.playerYaw = s_standaloneStrongholdState.playerYaw;
    data.isInOverworld = s_standaloneStrongholdState.isInOverworld;
    data.isInNether = s_standaloneStrongholdState.isInNether;
    data.eyeThrows = s_standaloneStrongholdState.eyeThrows;
    FinalizeParsedStrongholdData(data);
    data.ok = true;
    return data;
}

static ParsedStrongholdApiData ParseStrongholdApiPayload(const std::string& json) {
    // Supports standard and scientific notation (e.g. 3.378E-4).
    static const std::string numberPattern = "(-?\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?)";
    static const std::regex rePlayerX("\"xInOverworld\"\\s*:\\s*" + numberPattern);
    static const std::regex rePlayerZ("\"zInOverworld\"\\s*:\\s*" + numberPattern);
    static const std::regex rePlayerYaw("\"horizontalAngle\"\\s*:\\s*" + numberPattern);
    static const std::regex reInNether("\"isInNether\"\\s*:\\s*(true|false)");
    static const std::regex reInOverworld("\"isInOverworld\"\\s*:\\s*(true|false)");

    static const std::regex reThrowX("\"xInOverworld\"\\s*:\\s*" + numberPattern);
    static const std::regex reThrowZ("\"zInOverworld\"\\s*:\\s*" + numberPattern);
    static const std::regex reThrowAngle("\"angle\"\\s*:\\s*" + numberPattern);
    static const std::regex reThrowVerticalAngle("\"verticalAngle\"\\s*:\\s*" + numberPattern);
    static const std::regex reThrowAngleWithoutCorrection("\"angleWithoutCorrection\"\\s*:\\s*" + numberPattern);
    static const std::regex reThrowCorrection("\"correction\"\\s*:\\s*" + numberPattern);
    static const std::regex reThrowType("\"type\"\\s*:\\s*\"([A-Z_]+)\"");

    static const std::regex rePredictionChunkX("\"chunkX\"\\s*:\\s*(-?\\d+)");
    static const std::regex rePredictionChunkZ("\"chunkZ\"\\s*:\\s*(-?\\d+)");
    static const std::regex rePredictionCertainty("\"certainty\"\\s*:\\s*" + numberPattern);

    ParsedStrongholdApiData data;

    std::string playerPositionObject;
    if (!ExtractJsonEnclosedAfterKey(json, "playerPosition", '{', '}', playerPositionObject)) return data;
    if (!ExtractRegexDouble(playerPositionObject, rePlayerX, data.playerX)) return data;
    if (!ExtractRegexDouble(playerPositionObject, rePlayerZ, data.playerZ)) return data;
    if (!ExtractRegexDouble(playerPositionObject, rePlayerYaw, data.playerYaw)) return data;

    bool hasNetherFlag = ExtractRegexBool(playerPositionObject, reInNether, data.isInNether);
    bool hasOverworldFlag = ExtractRegexBool(playerPositionObject, reInOverworld, data.isInOverworld);
    if (!hasNetherFlag && !hasOverworldFlag) return data;
    if (!hasNetherFlag) data.isInNether = !data.isInOverworld;
    if (!hasOverworldFlag) data.isInOverworld = !data.isInNether;

    std::string throwsArray;
    if (ExtractJsonEnclosedAfterKey(json, "eyeThrows", '[', ']', throwsArray)) {
        for (const std::string& throwObject : ExtractTopLevelObjectsFromArray(throwsArray)) {
            ParsedEyeThrow parsedThrow;
            if (!ExtractRegexDouble(throwObject, reThrowX, parsedThrow.xInOverworld)) continue;
            if (!ExtractRegexDouble(throwObject, reThrowZ, parsedThrow.zInOverworld)) continue;
            ExtractRegexDouble(throwObject, reThrowVerticalAngle, parsedThrow.verticalAngleDeg);

            if (!ExtractRegexDouble(throwObject, reThrowAngle, parsedThrow.angleDeg)) {
                double angleWithoutCorrection = 0.0;
                double correction = 0.0;
                if (!ExtractRegexDouble(throwObject, reThrowAngleWithoutCorrection, angleWithoutCorrection)) continue;
                ExtractRegexDouble(throwObject, reThrowCorrection, correction);
                parsedThrow.angleDeg = angleWithoutCorrection + correction;
            }

            std::string typeString = "UNKNOWN";
            ExtractRegexString(throwObject, reThrowType, typeString);
            parsedThrow.type = EyeThrowTypeFromString(typeString);
            data.eyeThrows.push_back(parsedThrow);
        }
    }

    std::string predictionsArray;
    if (ExtractJsonEnclosedAfterKey(json, "predictions", '[', ']', predictionsArray)) {
        for (const std::string& predictionObject : ExtractTopLevelObjectsFromArray(predictionsArray)) {
            ParsedPrediction prediction;
            if (!ExtractRegexInt(predictionObject, rePredictionChunkX, prediction.chunkX)) continue;
            if (!ExtractRegexInt(predictionObject, rePredictionChunkZ, prediction.chunkZ)) continue;
            ExtractRegexDouble(predictionObject, rePredictionCertainty, prediction.certainty);
            data.predictions.push_back(prediction);
        }
    }

    FinalizeParsedStrongholdData(data);
    data.ok = true;
    return data;
}

static ParsedInformationMessagesData ParseInformationMessagesPayload(const std::string& json) {
    static const std::regex reType("\"type\"\\s*:\\s*\"([A-Z_]+)\"");
    static const std::regex reMessage("\"message\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
    static const std::regex rePercent("(-?\\d+(?:\\.\\d+)?)\\s*%");
    static const std::regex reLeftRight("left\\s+(\\d+)\\s+blocks?.*right\\s+(\\d+)\\s+blocks?");

    ParsedInformationMessagesData data;
    std::string messagesArray;
    if (!ExtractJsonEnclosedAfterKey(json, "informationMessages", '[', ']', messagesArray)) { return data; }

    for (const std::string& messageObject : ExtractTopLevelObjectsFromArray(messagesArray)) {
        std::string type;
        std::string messageEscaped;
        if (!ExtractRegexString(messageObject, reType, type)) continue;
        if (!ExtractRegexString(messageObject, reMessage, messageEscaped)) continue;

        std::string message = JsonUnescapeBasic(messageEscaped);

        if (type == "COMBINED_CERTAINTY") {
            std::smatch match;
            if (std::regex_search(message, match, rePercent) && match.size() >= 2) {
                try {
                    data.combinedCertaintyPercent = std::clamp(std::stod(match[1].str()), 0.0, 100.0);
                    data.hasCombinedCertainty = true;
                } catch (...) {
                }
            }
            continue;
        }

        if (type == "NEXT_THROW_DIRECTION") {
            std::string lowerMessage = message;
            std::transform(lowerMessage.begin(), lowerMessage.end(), lowerMessage.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::smatch match;
            if (std::regex_search(lowerMessage, match, reLeftRight) && match.size() >= 3) {
                try {
                    data.moveLeftBlocks = std::stoi(match[1].str());
                    data.moveRightBlocks = std::stoi(match[2].str());
                    data.hasNextThrowDirection = true;
                } catch (...) {
                }
            }
            continue;
        }

        if (type == "MISMEASURE") {
            data.hasMismeasureWarning = true;
            data.mismeasureWarningText = message;
            continue;
        }
    }

    data.ok = true;
    return data;
}
} // namespace

static void ComputeScreenMetricsForGameWindow(int& outW, int& outH) {
    outW = 0;
    outH = 0;

    HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
    if (!GetMonitorSizeForWindow(hwnd, outW, outH)) {
        // Fallback to primary monitor.
        outW = GetSystemMetrics(SM_CXSCREEN);
        outH = GetSystemMetrics(SM_CYSCREEN);
    }
}

// Returns true if the cached width/height changed.
static bool RefreshCachedScreenMetricsIfNeeded(bool requestRecalcOnChange) {
    constexpr ULONGLONG kPeriodicRefreshMs = 250; // fast enough to catch monitor moves, cheap enough for render thread callers
    ULONGLONG now = GetTickCount64();

    bool forced = s_screenMetricsDirty.exchange(false, std::memory_order_relaxed);
    ULONGLONG last = s_lastScreenMetricsRefreshMs.load(std::memory_order_relaxed);
    bool periodic = (now - last) >= kPeriodicRefreshMs;

    if (!forced && !periodic) { return false; }
    s_lastScreenMetricsRefreshMs.store(now, std::memory_order_relaxed);

    int newW = 0, newH = 0;
    ComputeScreenMetricsForGameWindow(newW, newH);
    if (newW <= 0 || newH <= 0) { return false; }

    int prevW = s_cachedScreenWidth.load(std::memory_order_relaxed);
    int prevH = s_cachedScreenHeight.load(std::memory_order_relaxed);

    if (prevW != newW || prevH != newH) {
        s_cachedScreenWidth.store(newW, std::memory_order_relaxed);
        s_cachedScreenHeight.store(newH, std::memory_order_relaxed);

        if (requestRecalcOnChange) { s_screenMetricsRecalcRequested.store(true, std::memory_order_relaxed); }
        return true;
    }

    return false;
}

void InvalidateCachedScreenMetrics() {
    s_screenMetricsDirty.store(true, std::memory_order_relaxed);
}

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

    // Store previous values to detect changes.
    // Note: other threads may refresh the cache (to avoid returning stale values),
    // so we also honor an explicit "recalc requested" flag.
    int prevWidth = s_cachedScreenWidth.load(std::memory_order_relaxed);
    int prevHeight = s_cachedScreenHeight.load(std::memory_order_relaxed);

    bool changed = RefreshCachedScreenMetricsIfNeeded(/*requestRecalcOnChange=*/false);
    bool recalcRequested = s_screenMetricsRecalcRequested.exchange(false, std::memory_order_relaxed);

    int newWidth = s_cachedScreenWidth.load(std::memory_order_relaxed);
    int newHeight = s_cachedScreenHeight.load(std::memory_order_relaxed);

    // Recalculate expression-based dimensions if screen size changed or if another thread requested it.
    // Only do this when we already had non-zero values once (prevents doing work during early startup).
    if (prevWidth != 0 && prevHeight != 0 && (changed || recalcRequested || prevWidth != newWidth || prevHeight != newHeight)) {
        RecalculateExpressionDimensions();
        // RecalculateExpressionDimensions mutates g_config.modes in-place (width/height/stretch fields).
        // Publish updated snapshot so reader threads see the recalculated dimensions.
        PublishConfigSnapshot();
    }
}

int GetCachedScreenWidth() {
    // Refresh opportunistically so we don't return stale monitor dimensions after a window move.
    // This is throttled (see RefreshCachedScreenMetricsIfNeeded).
    RefreshCachedScreenMetricsIfNeeded(/*requestRecalcOnChange=*/true);

    int w = s_cachedScreenWidth.load(std::memory_order_relaxed);
    if (w == 0) {
        // Startup fallback if logic thread hasn't populated the cache yet.
        int tmpW = 0, tmpH = 0;
        ComputeScreenMetricsForGameWindow(tmpW, tmpH);
        if (tmpW > 0) {
            s_cachedScreenWidth.store(tmpW, std::memory_order_relaxed);
            s_cachedScreenHeight.store(tmpH, std::memory_order_relaxed);
            w = tmpW;
        }
    }
    return w;
}

int GetCachedScreenHeight() {
    RefreshCachedScreenMetricsIfNeeded(/*requestRecalcOnChange=*/true);

    int h = s_cachedScreenHeight.load(std::memory_order_relaxed);
    if (h == 0) {
        // Startup fallback if logic thread hasn't populated the cache yet.
        int tmpW = 0, tmpH = 0;
        ComputeScreenMetricsForGameWindow(tmpW, tmpH);
        if (tmpH > 0) {
            s_cachedScreenWidth.store(tmpW, std::memory_order_relaxed);
            s_cachedScreenHeight.store(tmpH, std::memory_order_relaxed);
            h = tmpH;
        }
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

        {
            std::lock_guard<std::mutex> overlayLock(s_strongholdOverlayMutex);
            const std::string resetMessage = "World exited. Shift+H lock.";
            ResetStrongholdOverlayLocked(s_strongholdOverlayState, resetMessage, false);
            if (cfgSnap) { s_strongholdOverlayState.visible = cfgSnap->strongholdOverlay.visible; }
            s_strongholdOverlayState.initializedVisibility = true;
        }
        s_standaloneStrongholdState = StandaloneStrongholdState{};
        s_lastAnchoredStandaloneSnapshotCounter = 0;
        s_strongholdLivePlayerPose = StrongholdLivePlayerPose{};
        ResetStrongholdLiveInputState();
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
        // NOTE: The GUI spinners represent an explicit switch to absolute pixel sizing.
        // If a mode was previously driven by an expression (e.g. Thin/Wide defaults) or
        // by percentage sizing, changing the spinner should disable that and persist the
        // new numeric value.
        if (g_pendingDimensionChange.newWidth > 0) {
            mode->width = g_pendingDimensionChange.newWidth;
            mode->widthExpr.clear();
            mode->relativeWidth = -1.0f;
        }
        if (g_pendingDimensionChange.newHeight > 0) {
            mode->height = g_pendingDimensionChange.newHeight;
            mode->heightExpr.clear();
            mode->relativeHeight = -1.0f;
        }

        // If no relative sizing remains, clear the flag (keeps UI/serialization consistent).
        const bool hasRelativeWidth = (mode->relativeWidth >= 0.0f && mode->relativeWidth <= 1.0f);
        const bool hasRelativeHeight = (mode->relativeHeight >= 0.0f && mode->relativeHeight <= 1.0f);
        if (!hasRelativeWidth && !hasRelativeHeight) { mode->useRelativeSize = false; }

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

void UpdateStrongholdOverlayState() {
    PROFILE_SCOPE_CAT("LT Stronghold Overlay", "Logic Thread");

    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return;
    StrongholdOverlayConfig overlayCfg = cfgSnap->strongholdOverlay;
    // Standalone-only release: force local clipboard pipeline and disable backend management.
    overlayCfg.standaloneClipboardMode = true;
    overlayCfg.manageNinjabrainBotProcess = false;
    overlayCfg.autoStartNinjabrainBot = false;
    overlayCfg.hideNinjabrainBotWindow = false;

    {
        std::lock_guard<std::mutex> lock(s_strongholdOverlayMutex);
        if (!s_strongholdOverlayState.initializedVisibility) {
            s_strongholdOverlayState.visible = overlayCfg.visible;
            s_strongholdOverlayState.initializedVisibility = true;
        }
    }

    if (!overlayCfg.enabled) {
        s_pendingStrongholdMouseDeltaX.exchange(0, std::memory_order_relaxed);
        s_pendingStrongholdMouseDeltaY.exchange(0, std::memory_order_relaxed);
        return;
    }

    if (overlayCfg.autoHideOnEyeSpy && PollEyeSpyAdvancementDetected()) {
        std::lock_guard<std::mutex> lock(s_strongholdOverlayMutex);
        auto& st = s_strongholdOverlayState;
        if (st.visible) {
            st.visible = false;
            st.infoLabel = "Eye Spy detected. Overlay auto-hidden.";
        }
    }

    if (s_pendingStandaloneReset.exchange(false)) {
        StandaloneStrongholdState resetState{};
        resetState.lastClipboardSequenceNumber = GetClipboardSequenceNumber();
        resetState.lastClipboardText = s_standaloneStrongholdState.lastClipboardText;
        s_standaloneStrongholdState = std::move(resetState);
        s_lastAnchoredStandaloneSnapshotCounter = 0;
        s_strongholdLivePlayerPose.valid = false;
        s_strongholdLivePlayerPose.isInNether = false;
        s_strongholdLivePlayerPose.lastUpdate = std::chrono::steady_clock::now();
    }

    AdvanceStrongholdLivePlayerPose();

    int pollIntervalMs = std::clamp(overlayCfg.pollIntervalMs, 50, 2000);
    auto now = std::chrono::steady_clock::now();
    if (now < s_nextStrongholdPollTime) {
        std::lock_guard<std::mutex> lock(s_strongholdOverlayMutex);
        auto& st = s_strongholdOverlayState;
        if (st.hasPrediction && s_strongholdLivePlayerPose.valid) {
            int targetChunkX = 0;
            int targetChunkZ = 0;
            bool hasTarget = false;
            if (st.targetLocked) {
                targetChunkX = st.lockedChunkX;
                targetChunkZ = st.lockedChunkZ;
                st.usingLiveTarget = false;
                hasTarget = true;
            } else if (st.hasLiveTarget) {
                targetChunkX = st.lastLiveChunkX;
                targetChunkZ = st.lastLiveChunkZ;
                st.usingLiveTarget = true;
                hasTarget = true;
            }
            if (hasTarget) {
                ApplyPlayerPoseAndTargetToOverlayState(st, overlayCfg, s_strongholdLivePlayerPose.xInOverworld,
                                                       s_strongholdLivePlayerPose.zInOverworld, s_strongholdLivePlayerPose.yawDeg,
                                                       targetChunkX, targetChunkZ, st.wasInNetherLastTick);
            }
        }
        return;
    }
    s_nextStrongholdPollTime = now + std::chrono::milliseconds(pollIntervalMs);

    const bool useStandaloneSource = true;
    ParsedStrongholdApiData data;
    ParsedInformationMessagesData infoData;

    if (useStandaloneSource) {
        data = BuildStandaloneStrongholdApiData(overlayCfg.standaloneAllowNonBoatThrows);
    } else {
        std::string json;
        if (!HttpGetStrongholdJson(json)) {
            EnsureManagedBackendResult backendResult = EnsureManagedNinjabrainBotBackend(overlayCfg, false);
            std::lock_guard<std::mutex> lock(s_strongholdOverlayMutex);
            auto& st = s_strongholdOverlayState;
            st.failCount += 1;
            if (st.failCount >= 3) {
                st.apiOnline = false;
                st.usingStandalonePipeline = false;
                st.hasPlayerSnapshot = false;
                st.hasPrediction = false;
                st.hasLiveTarget = false;
                st.liveTargetFromNativeTriangulation = false;
                st.activeEyeThrowCount = 0;
                st.hasTopCertainty = false;
                st.hasCombinedCertainty = false;
                st.hasNextThrowDirection = false;
                st.topCandidate1Label.clear();
                st.topCandidate2Label.clear();
                st.warningLabel.clear();
                st.showComputedDetails = false;
                st.boatState = kBoatStateUninitialized;
                st.boatLabel = "Boat: UNINIT";
                st.statusLabel = GetUnlockedStatusLabel(st.blockAutoLockUntilThrowClear);
                st.infoLabel = ManagedBackendOfflineMessage(backendResult);
                st.debugBasePredictionsLabel.clear();
                st.debugAdjustedPredictionsLabel.clear();
                st.debugSelectionLabel.clear();
            }
            return;
        }

        EnsureManagedNinjabrainBotBackend(overlayCfg, true);
        data = ParseStrongholdApiPayload(json);

        std::string infoJson;
        if (HttpGetInformationMessagesJson(infoJson)) { infoData = ParseInformationMessagesPayload(infoJson); }
    }

    if (data.ok) {
        if (useStandaloneSource) {
            const uint64_t snapshotCounter = s_standaloneStrongholdState.parsedSnapshotCounter;
            if (!s_strongholdLivePlayerPose.valid || snapshotCounter != s_lastAnchoredStandaloneSnapshotCounter) {
                AnchorStrongholdLivePlayerPose(data.playerX, data.playerZ, data.playerYaw, data.isInNether || !data.isInOverworld);
                s_lastAnchoredStandaloneSnapshotCounter = snapshotCounter;
            }
        } else {
            AnchorStrongholdLivePlayerPose(data.playerX, data.playerZ, data.playerYaw, data.isInNether || !data.isInOverworld);
        }
    }

    std::lock_guard<std::mutex> lock(s_strongholdOverlayMutex);
    auto& st = s_strongholdOverlayState;

    st.failCount = 0;
    st.apiOnline = true;
    st.usingStandalonePipeline = useStandaloneSource;

    if (!data.ok) {
        st.hasPlayerSnapshot = false;
        st.hasPrediction = false;
        st.hasLiveTarget = false;
        st.liveTargetFromNativeTriangulation = false;
        st.activeEyeThrowCount = 0;
        st.hasTopCertainty = false;
        st.hasCombinedCertainty = false;
        st.hasNextThrowDirection = false;
        st.topCandidate1Label.clear();
        st.topCandidate2Label.clear();
        st.warningLabel.clear();
        st.showComputedDetails = false;
        st.boatState = kBoatStateUninitialized;
        st.boatLabel = (useStandaloneSource && overlayCfg.standaloneAllowNonBoatThrows) ? "Boat: OFF" : "Boat: UNINIT";
        st.statusLabel = GetUnlockedStatusLabel(st.blockAutoLockUntilThrowClear);
        st.infoLabel = useStandaloneSource ? "No F3+C snapshot yet. Copy F3+C in-game." : "No player snapshot yet.";
        st.debugBasePredictionsLabel.clear();
        st.debugAdjustedPredictionsLabel.clear();
        st.debugSelectionLabel.clear();
        st.topCandidate1Label.clear();
        st.topCandidate2Label.clear();
        st.warningLabel.clear();
        return;
    }

    st.hasPlayerSnapshot = true;

    // Local reset support: ignore throws up to prefix count. This allows NumPad5
    // (and Ctrl+Shift+H) to reset calculation without forcing source-side clears.
    if (st.ignoredThrowsPrefixCount < 0) st.ignoredThrowsPrefixCount = 0;
    if (st.ignoredThrowsPrefixCount > data.eyeThrowCount) { st.ignoredThrowsPrefixCount = data.eyeThrowCount; }

    int activeThrowStart = st.ignoredThrowsPrefixCount;
    std::vector<ParsedEyeThrow> activeThrows;
    if (activeThrowStart < data.eyeThrowCount) { activeThrows.assign(data.eyeThrows.begin() + activeThrowStart, data.eyeThrows.end()); }
    const std::vector<ParsedEyeThrow> activeThrowsBase = activeThrows;
    int activeEyeThrowCount = static_cast<int>(activeThrows.size());
    st.activeEyeThrowCount = activeEyeThrowCount;

    if (st.perThrowAngleAdjustmentsDeg.size() < static_cast<size_t>(activeEyeThrowCount)) {
        st.perThrowAngleAdjustmentsDeg.resize(static_cast<size_t>(activeEyeThrowCount), 0.0);
    } else if (st.perThrowAngleAdjustmentsDeg.size() > static_cast<size_t>(activeEyeThrowCount)) {
        st.perThrowAngleAdjustmentsDeg.resize(static_cast<size_t>(activeEyeThrowCount));
    }
    if (st.adjustmentHistoryThrowCount != activeEyeThrowCount) {
        st.adjustmentUndoStackDeg.clear();
        st.adjustmentRedoStackDeg.clear();
        st.adjustmentHistoryThrowCount = activeEyeThrowCount;
    }

    bool hasLocalAngleOverride = false;
    for (size_t i = 0; i < activeThrows.size(); ++i) {
        const double adjustmentDeg = st.perThrowAngleAdjustmentsDeg[i];
        if (std::abs(adjustmentDeg) <= 1e-9) continue;
        activeThrows[i].angleDeg = NormalizeDegrees(activeThrows[i].angleDeg + adjustmentDeg);
        hasLocalAngleOverride = true;
    }
    st.lastThrowAngleAdjustmentDeg =
        activeEyeThrowCount > 0 ? st.perThrowAngleAdjustmentsDeg[static_cast<size_t>(activeEyeThrowCount) - 1] : 0.0;
    if (activeEyeThrowCount <= 0) {
        st.lastAdjustmentStepDirection = 0;
        st.lastActiveThrowVerticalAngleDeg = -31.6;
    } else {
        st.lastActiveThrowVerticalAngleDeg = activeThrows.back().verticalAngleDeg;
    }

    bool activeHasBoatThrow =
        std::any_of(activeThrows.begin(), activeThrows.end(), [](const ParsedEyeThrow& t) { return t.type == EyeThrowType::Boat; });

    const bool localResetOverrideActive = (activeThrowStart > 0 && activeEyeThrowCount > 0);
    const bool localOverrideActive = localResetOverrideActive || hasLocalAngleOverride;

    int nativeChunkX = 0;
    int nativeChunkZ = 0;
    bool hasNativeTriangulation = ComputeNativeTriangulatedChunkFromThrows(activeThrows, nativeChunkX, nativeChunkZ);

    std::vector<ParsedPrediction> effectivePredictions;
    if (activeThrowStart == 0) {
        if (hasLocalAngleOverride) {
            if (useStandaloneSource) {
                // Local standalone mode should rebuild from adjusted throws so candidates
                // outside truncated base predictions can still surface.
                BuildApproxPosteriorPredictionsFromThrows(activeThrows, effectivePredictions);
            } else {
                effectivePredictions = data.predictions;
                if (!effectivePredictions.empty()) {
                    std::vector<ParsedPrediction> reweightedPredictions;
                    if (ReweightPredictionsByAdjustedThrows(data.predictions, activeThrowsBase, activeThrows, reweightedPredictions)) {
                        effectivePredictions = std::move(reweightedPredictions);
                    }
                }
            }
        } else {
            effectivePredictions = data.predictions;
        }
    } else {
        // After local reset (ignoring N initial throws), rebuild posterior from the
        // remaining throw set so targeting stays stable even when backend state still
        // includes older throws.
        BuildApproxPosteriorPredictionsFromThrows(activeThrows, effectivePredictions);
    }

    int topPredictionChunkX = 0;
    int topPredictionChunkZ = 0;
    double topPredictionCertainty = 0.0;
    const bool hasTopPredictionRaw =
        TryGetTopPrediction(effectivePredictions, topPredictionChunkX, topPredictionChunkZ, topPredictionCertainty);
    const bool topPredictionLowConfidence =
        hasTopPredictionRaw && (!std::isfinite(topPredictionCertainty) || topPredictionCertainty <= kNbbMinimumSuccessfulPosteriorWeight);
    const bool hasTopPrediction = hasTopPredictionRaw && !topPredictionLowConfidence;

    std::vector<ParsedPrediction> baseSortedPredictions = data.predictions;
    std::sort(baseSortedPredictions.begin(), baseSortedPredictions.end(),
              [](const ParsedPrediction& a, const ParsedPrediction& b) { return a.certainty > b.certainty; });
    std::vector<ParsedPrediction> effectiveSortedPredictions = effectivePredictions;
    std::sort(effectiveSortedPredictions.begin(), effectiveSortedPredictions.end(),
              [](const ParsedPrediction& a, const ParsedPrediction& b) { return a.certainty > b.certainty; });
    std::string selectionReason = hasLocalAngleOverride ? "local-delta" : (useStandaloneSource ? "local-top" : "nbb-top");

    st.hasTopCertainty = hasTopPrediction && std::isfinite(topPredictionCertainty) && activeEyeThrowCount >= 2;
    st.topCertaintyPercent = st.hasTopCertainty ? static_cast<float>(std::clamp(topPredictionCertainty * 100.0, 0.0, 100.0)) : 0.0f;
    const bool debugUseNetherCoords = overlayCfg.preferNetherCoords || data.isInNether;
    st.debugBasePredictionsLabel = "Base: " + FormatPredictionDebugLabel(baseSortedPredictions, 4, debugUseNetherCoords);
    st.debugAdjustedPredictionsLabel =
        hasLocalAngleOverride ? ("Adj: " + FormatPredictionDebugLabel(effectiveSortedPredictions, 4, debugUseNetherCoords)) : "Adj: (off)";
    if (hasTopPredictionRaw) {
        const int chosenBaseRank = FindPredictionRank(baseSortedPredictions, topPredictionChunkX, topPredictionChunkZ);
        const int chosenAdjRank = FindPredictionRank(effectiveSortedPredictions, topPredictionChunkX, topPredictionChunkZ);
        const int chosenX = debugUseNetherCoords ? topPredictionChunkX * 2 : topPredictionChunkX * 16;
        const int chosenZ = debugUseNetherCoords ? topPredictionChunkZ * 2 : topPredictionChunkZ * 16;
        st.debugSelectionLabel = "Pick: " + std::to_string(chosenX) + "," + std::to_string(chosenZ) + " base#" +
                                 std::to_string(chosenBaseRank) + " adj#" + std::to_string(chosenAdjRank) + " (" + selectionReason +
                                 (topPredictionLowConfidence ? ",low-conf" : "") + ")";
    } else {
        st.debugSelectionLabel = "Pick: (none)";
    }

    st.topCandidate1Label.clear();
    st.topCandidate2Label.clear();
    const bool includeDetailedCandidateMetrics = (!IsMcsrRankedInstanceDetected()) && overlayCfg.nonMcsrFeaturesEnabled;
    if (!effectiveSortedPredictions.empty()) {
        st.topCandidate1Label = FormatPredictionCandidateRow(1, effectiveSortedPredictions[0], data.playerX, data.playerZ, data.playerYaw,
                                                             overlayCfg.useChunkCenterTarget, includeDetailedCandidateMetrics);
    }
    if (effectiveSortedPredictions.size() >= 2) {
        st.topCandidate2Label = FormatPredictionCandidateRow(2, effectiveSortedPredictions[1], data.playerX, data.playerZ, data.playerYaw,
                                                             overlayCfg.useChunkCenterTarget, includeDetailedCandidateMetrics);
    }

    const bool hasNbbInfoMessages = infoData.ok;
    bool hasCombinedCertainty = (!localOverrideActive && hasNbbInfoMessages && infoData.hasCombinedCertainty);
    double combinedCertaintyPercent = hasCombinedCertainty ? infoData.combinedCertaintyPercent : 0.0;
    if (!hasCombinedCertainty && !hasNbbInfoMessages &&
        TryComputeCombinedCertaintyFallback(effectivePredictions, combinedCertaintyPercent)) {
        hasCombinedCertainty = true;
    }

    bool hasNextThrowDirection = (!localOverrideActive && hasNbbInfoMessages && infoData.hasNextThrowDirection);
    int moveLeftBlocks = hasNextThrowDirection ? infoData.moveLeftBlocks : 0;
    int moveRightBlocks = hasNextThrowDirection ? infoData.moveRightBlocks : 0;
    const bool forceNextThrowGuidance = (activeEyeThrowCount <= 1);
    if (!hasNextThrowDirection && !hasNbbInfoMessages &&
        TryComputeNextThrowDirectionFallback(effectivePredictions, activeThrows, moveLeftBlocks, moveRightBlocks, forceNextThrowGuidance)) {
        hasNextThrowDirection = true;
    }
    // Show movement guidance only when top certainty is below 95%.
    const bool topCertaintyHighEnoughToSuppressGuidance =
        hasTopPredictionRaw && std::isfinite(topPredictionCertainty) && topPredictionCertainty >= 0.95;
    if (topCertaintyHighEnoughToSuppressGuidance) {
        hasNextThrowDirection = false;
        moveLeftBlocks = 0;
        moveRightBlocks = 0;
    }
    st.hasCombinedCertainty = hasCombinedCertainty;
    st.combinedCertaintyPercent = static_cast<float>(combinedCertaintyPercent);
    st.hasNextThrowDirection = hasNextThrowDirection;
    st.moveLeftBlocks = moveLeftBlocks;
    st.moveRightBlocks = moveRightBlocks;
    std::string warningText;
    bool hasWarning = (!localOverrideActive && hasNbbInfoMessages && infoData.hasMismeasureWarning);
    if (hasWarning) {
        warningText = infoData.mismeasureWarningText;
    } else if (!hasNbbInfoMessages && hasTopPrediction &&
               TryComputeMismeasureWarningFallback(activeThrows, topPredictionChunkX, topPredictionChunkZ, warningText)) {
        hasWarning = true;
    }
    st.warningLabel = hasWarning ? warningText : "";

    bool sawHardReset = (data.eyeThrowCount == 0 && st.lastEyeThrowCount > 0) ||
                        (activeThrowStart == 0 && activeEyeThrowCount == 0 && !hasNativeTriangulation && !hasTopPrediction &&
                         (st.hasLiveTarget || st.targetLocked));
    if (sawHardReset) {
        ResetStrongholdOverlayLocked(st, "Detected throw source reset.", false);
        st.apiOnline = true;
        st.hasPlayerSnapshot = true;
        st.wasInNetherLastTick = data.isInNether || !data.isInOverworld;
        return;
    }

    st.lastEyeThrowCount = data.eyeThrowCount;
    if (st.blockAutoLockUntilThrowClear && data.eyeThrowCount <= st.ignoredThrowsPrefixCount) { st.blockAutoLockUntilThrowClear = false; }

    // NBB treats very low posterior top-weight as failed triangulation.
    // Preserve the previous live target (if present) so noisy throws don't hard-jump.
    const bool keepPreviousLiveTargetForLowConfidence = topPredictionLowConfidence && !st.targetLocked && st.hasLiveTarget;

    if (!keepPreviousLiveTargetForLowConfidence) {
        if (hasTopPrediction) {
            st.hasLiveTarget = true;
            st.lastLiveChunkX = topPredictionChunkX;
            st.lastLiveChunkZ = topPredictionChunkZ;
            st.liveTargetFromNativeTriangulation = false;
        } else if (hasNativeTriangulation && !topPredictionLowConfidence) {
            st.hasLiveTarget = true;
            st.lastLiveChunkX = nativeChunkX;
            st.lastLiveChunkZ = nativeChunkZ;
            st.liveTargetFromNativeTriangulation = true;
        } else {
            st.hasLiveTarget = false;
            st.liveTargetFromNativeTriangulation = false;
        }
    }

    bool nowInNether = data.isInNether || !data.isInOverworld;
    bool enteredNether = nowInNether && !st.wasInNetherLastTick;
    st.wasInNetherLastTick = nowInNether;
    const bool standaloneNonBoatAutoLockReady =
        useStandaloneSource && overlayCfg.standaloneAllowNonBoatThrows && activeEyeThrowCount > 0 && hasTopPrediction &&
        !topPredictionLowConfidence;
    bool autoLockTrigger =
        enteredNether || (nowInNether && activeEyeThrowCount > 0) || activeHasBoatThrow || standaloneNonBoatAutoLockReady;

    int boatState = useStandaloneSource ? s_standaloneStrongholdState.boatState : kBoatStateUninitialized;
    if (useStandaloneSource && overlayCfg.standaloneAllowNonBoatThrows) {
        st.boatState = kBoatStateUninitialized;
        st.boatLabel = "Boat: OFF";
    } else {
        if (activeHasBoatThrow) {
            boatState = kBoatStateGood;
        } else if (!useStandaloneSource && nowInNether && activeEyeThrowCount > 0) {
            boatState = kBoatStateFailed;
        }
        st.boatState = boatState;
        switch (boatState) {
        case kBoatStateGood:
            st.boatLabel = "Boat: GOOD";
            break;
        case kBoatStateFailed:
            st.boatLabel = "Boat: FAILED";
            break;
        case kBoatStateUninitialized:
        default:
            st.boatLabel = "Boat: UNINIT";
            break;
        }
    }

    if (overlayCfg.autoLockOnFirstNether && autoLockTrigger && !st.hasAutoLockedOnNether && !st.targetLocked && st.hasLiveTarget &&
        !st.blockAutoLockUntilThrowClear) {
        LockStrongholdTargetLocked(st, st.lastLiveChunkX, st.lastLiveChunkZ, true);
        st.hasAutoLockedOnNether = true;
    }

    int targetChunkX = 0;
    int targetChunkZ = 0;
    if (st.targetLocked) {
        targetChunkX = st.lockedChunkX;
        targetChunkZ = st.lockedChunkZ;
        st.usingLiveTarget = false;
    } else if (st.hasLiveTarget) {
        targetChunkX = st.lastLiveChunkX;
        targetChunkZ = st.lastLiveChunkZ;
        st.usingLiveTarget = true;
    } else {
        st.hasPrediction = false;
        st.showComputedDetails = false;
        st.statusLabel = GetUnlockedStatusLabel(st.blockAutoLockUntilThrowClear);
        if (activeEyeThrowCount == 0) {
            st.infoLabel = "No throws yet. Shift+H lock";
        } else if (activeEyeThrowCount == 1) {
            st.infoLabel = "Need 2 throws. Shift+H lock";
        } else if (topPredictionLowConfidence) {
            st.infoLabel = "Low confidence after latest throw. Re-throw. Shift+H lock";
        } else {
            st.infoLabel = "No target yet. Shift+H lock";
        }
        st.debugBasePredictionsLabel.clear();
        st.debugAdjustedPredictionsLabel.clear();
        st.debugSelectionLabel.clear();
        return;
    }

    st.hasPrediction = true;
    double playerXForDisplay = data.playerX;
    double playerZForDisplay = data.playerZ;
    double playerYawForDisplay = data.playerYaw;
    if (s_strongholdLivePlayerPose.valid) {
        playerXForDisplay = s_strongholdLivePlayerPose.xInOverworld;
        playerZForDisplay = s_strongholdLivePlayerPose.zInOverworld;
        playerYawForDisplay = s_strongholdLivePlayerPose.yawDeg;
    }
    ApplyPlayerPoseAndTargetToOverlayState(st, overlayCfg, playerXForDisplay, playerZForDisplay, playerYawForDisplay, targetChunkX,
                                           targetChunkZ, nowInNether);
    st.statusLabel = st.targetLocked ? (st.lockSourceAuto ? "AUTO-LOCKED" : "LOCKED (manual)")
                                     : GetUnlockedStatusLabel(st.blockAutoLockUntilThrowClear);
    const char* sourceLabel = useStandaloneSource ? "Local" : "NBB";
    if (st.targetLocked) {
        st.infoLabel = st.lockSourceAuto ? "Auto lock. Shift+H unlock" : "Manual lock. Shift+H unlock";
    } else if (hasLocalAngleOverride && !st.liveTargetFromNativeTriangulation) {
        st.infoLabel = std::string(sourceLabel) + " adj " + std::to_string(activeEyeThrowCount) + " throws. Shift+H lock";
        st.infoLabel += " | Adj " + FormatSignedHundredths(st.lastThrowAngleAdjustmentDeg);
    } else if (st.liveTargetFromNativeTriangulation) {
        st.infoLabel = "Native " + std::to_string(activeEyeThrowCount) + " throws. Shift+H lock";
        if (std::abs(st.lastThrowAngleAdjustmentDeg) > 1e-9) {
            st.infoLabel += " | Adj " + FormatSignedHundredths(st.lastThrowAngleAdjustmentDeg);
        }
    } else {
        st.infoLabel = std::string(sourceLabel) + " top. Shift+H lock";
    }
    if (!st.targetLocked && topPredictionLowConfidence) {
        if (keepPreviousLiveTargetForLowConfidence) {
            st.infoLabel = "Low confidence after latest throw. Keeping previous target. Re-throw.";
        } else {
            st.infoLabel = "Low confidence after latest throw. Re-throw.";
        }
    }
    if (!st.targetLocked && activeEyeThrowCount <= 1) { st.infoLabel += " | Re-throw to confirm"; }
    st.showComputedDetails = true;
}

bool IsMcsrRankedInstanceDetected() {
    if (kForceMcsrSafeBuild) return true;
    RefreshMcsrRankedDetectionIfNeeded();
    return s_mcsrRankedInstanceDetected.load(std::memory_order_relaxed);
}

std::string GetMcsrRankedDetectionSource() {
    if (kForceMcsrSafeBuild) return "build-flag: TOOLSCREEN_FORCE_MCSR_SAFE";
    RefreshMcsrRankedDetectionIfNeeded();
    std::lock_guard<std::mutex> lock(s_mcsrRankedDetectionMutex);
    return s_mcsrRankedDetectionSource;
}

StrongholdOverlayRenderSnapshot GetStrongholdOverlayRenderSnapshot() {
    StrongholdOverlayRenderSnapshot snapshot;

    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return snapshot;
    const StrongholdOverlayConfig& overlayCfg = cfgSnap->strongholdOverlay;
    snapshot.mcsrSafeMode = IsMcsrRankedInstanceDetected();

    snapshot.enabled = overlayCfg.enabled;
    snapshot.overlayOpacity = std::clamp(overlayCfg.opacity, 0.0f, 1.0f);
    snapshot.backgroundOpacity = std::clamp(overlayCfg.backgroundOpacity, 0.0f, 1.0f);
    snapshot.scale = std::clamp(overlayCfg.scale, 0.4f, 3.0f);
    snapshot.renderMonitorMode = std::clamp(overlayCfg.renderMonitorMode, 0, 1);
    snapshot.renderMonitorMask = overlayCfg.renderMonitorMask;
    snapshot.x = overlayCfg.x;
    snapshot.y = overlayCfg.y;
    const bool nonMcsrEnabled = (!snapshot.mcsrSafeMode) && overlayCfg.nonMcsrFeaturesEnabled;
    snapshot.showDirectionArrow = nonMcsrEnabled && overlayCfg.showDirectionArrow;
    snapshot.showEstimateValues = nonMcsrEnabled && overlayCfg.showEstimateValues;
    snapshot.showAlignmentText = nonMcsrEnabled && overlayCfg.showAlignmentText;
    snapshot.boatModeEnabled = !overlayCfg.standaloneAllowNonBoatThrows;
    snapshot.hudLayoutMode = std::clamp(overlayCfg.hudLayoutMode, 0, 2);
    if (snapshot.hudLayoutMode == 1) snapshot.hudLayoutMode = 2; // Compact merged into Speedrun
    snapshot.preferNetherCoords = overlayCfg.preferNetherCoords;

    std::lock_guard<std::mutex> lock(s_strongholdOverlayMutex);
    if (!s_strongholdOverlayState.initializedVisibility) {
        s_strongholdOverlayState.visible = overlayCfg.visible;
        s_strongholdOverlayState.initializedVisibility = true;
    }

    snapshot.visible = s_strongholdOverlayState.visible;
    snapshot.apiOnline = s_strongholdOverlayState.apiOnline;
    snapshot.hasPlayerSnapshot = s_strongholdOverlayState.hasPlayerSnapshot;
    snapshot.hasPrediction = s_strongholdOverlayState.hasPrediction;
    snapshot.targetLocked = s_strongholdOverlayState.targetLocked;
    snapshot.lockWasAuto = s_strongholdOverlayState.lockSourceAuto;
    snapshot.blockAutoLockUntilThrowClear = s_strongholdOverlayState.blockAutoLockUntilThrowClear;
    snapshot.usingNetherCoords = s_strongholdOverlayState.usingNetherCoords;
    snapshot.usingLiveTarget = s_strongholdOverlayState.usingLiveTarget;
    snapshot.targetDisplayX = s_strongholdOverlayState.targetDisplayX;
    snapshot.targetDisplayZ = s_strongholdOverlayState.targetDisplayZ;
    snapshot.playerDisplayX = s_strongholdOverlayState.playerDisplayX;
    snapshot.playerDisplayZ = s_strongholdOverlayState.playerDisplayZ;
    snapshot.targetNetherX = s_strongholdOverlayState.targetNetherX;
    snapshot.targetNetherZ = s_strongholdOverlayState.targetNetherZ;
    snapshot.estimatedNetherX = s_strongholdOverlayState.estimatedNetherX;
    snapshot.estimatedNetherZ = s_strongholdOverlayState.estimatedNetherZ;
    snapshot.playerNetherX = s_strongholdOverlayState.playerNetherX;
    snapshot.playerNetherZ = s_strongholdOverlayState.playerNetherZ;
    snapshot.targetOverworldX = s_strongholdOverlayState.targetOverworldX;
    snapshot.targetOverworldZ = s_strongholdOverlayState.targetOverworldZ;
    snapshot.estimatedOverworldX = s_strongholdOverlayState.estimatedOverworldX;
    snapshot.estimatedOverworldZ = s_strongholdOverlayState.estimatedOverworldZ;
    snapshot.playerOverworldX = s_strongholdOverlayState.playerOverworldX;
    snapshot.playerOverworldZ = s_strongholdOverlayState.playerOverworldZ;
    snapshot.distanceDisplay = s_strongholdOverlayState.distanceDisplay;
    snapshot.relativeYaw = s_strongholdOverlayState.relativeYaw;
    snapshot.activeEyeThrowCount = s_strongholdOverlayState.activeEyeThrowCount;
    snapshot.angleAdjustmentDeg = static_cast<float>(s_strongholdOverlayState.lastThrowAngleAdjustmentDeg);
    snapshot.angleAdjustmentStepDeg =
        static_cast<float>(ComputeNbbAngleCorrectionStepDegrees(s_strongholdOverlayState.lastActiveThrowVerticalAngleDeg));
    snapshot.lastAdjustmentStepDirection = s_strongholdOverlayState.lastAdjustmentStepDirection;
    snapshot.hasTopCertainty = s_strongholdOverlayState.hasTopCertainty;
    snapshot.topCertaintyPercent = s_strongholdOverlayState.topCertaintyPercent;
    snapshot.hasCombinedCertainty = s_strongholdOverlayState.hasCombinedCertainty;
    snapshot.combinedCertaintyPercent = s_strongholdOverlayState.combinedCertaintyPercent;
    snapshot.hasNextThrowDirection = s_strongholdOverlayState.hasNextThrowDirection;
    snapshot.moveLeftBlocks = s_strongholdOverlayState.moveLeftBlocks;
    snapshot.moveRightBlocks = s_strongholdOverlayState.moveRightBlocks;
    snapshot.topCandidate1Label = s_strongholdOverlayState.topCandidate1Label;
    snapshot.topCandidate2Label = s_strongholdOverlayState.topCandidate2Label;
    snapshot.warningLabel = s_strongholdOverlayState.warningLabel;
    snapshot.boatState = s_strongholdOverlayState.boatState;
    snapshot.boatLabel = s_strongholdOverlayState.boatLabel;
    snapshot.modeLabel = s_strongholdOverlayState.modeLabel;
    snapshot.statusLabel = s_strongholdOverlayState.statusLabel;
    snapshot.infoLabel = s_strongholdOverlayState.infoLabel;
    snapshot.showComputedDetails = s_strongholdOverlayState.showComputedDetails;

    return snapshot;
}

bool HandleStrongholdOverlayHotkeyH(bool shiftDown, bool ctrlDown) {
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return false;
    if (!cfgSnap->strongholdOverlay.enabled) return false;

    std::lock_guard<std::mutex> lock(s_strongholdOverlayMutex);
    auto& st = s_strongholdOverlayState;

    if (!st.initializedVisibility) {
        st.visible = cfgSnap->strongholdOverlay.visible;
        st.initializedVisibility = true;
    }

    // Ctrl+Shift+H => full reset + pause auto-lock until throws clear.
    if (shiftDown && ctrlDown) {
        const int frozenThrowCount = std::max(0, st.lastEyeThrowCount);
        ResetStrongholdOverlayLocked(st, "Reset. Auto-lock paused until throws clear. Shift+H lock.", true);
        st.ignoredThrowsPrefixCount = frozenThrowCount;
        s_pendingStandaloneReset.store(true);
        return true;
    }

    // Shift+H => lock/unlock target.
    if (shiftDown) {
        if (st.targetLocked) {
            st.targetLocked = false;
            st.lockSourceAuto = false;
            st.statusLabel = GetUnlockedStatusLabel(st.blockAutoLockUntilThrowClear);
            st.infoLabel = "Target unlocked. Following live target. Shift+H lock.";
            st.showComputedDetails = false;
            return true;
        }

        if (!st.hasLiveTarget) {
            st.statusLabel = GetUnlockedStatusLabel(st.blockAutoLockUntilThrowClear);
            st.infoLabel = "No live target available yet. Shift+H lock.";
            st.showComputedDetails = false;
            return true;
        }

        LockStrongholdTargetLocked(st, st.lastLiveChunkX, st.lastLiveChunkZ, false);
        st.statusLabel = "LOCKED (manual)";
        st.infoLabel = "Target locked at chunk " + std::to_string(st.lastLiveChunkX) + ", " + std::to_string(st.lastLiveChunkZ) + ".";
        st.showComputedDetails = false;
        return true;
    }

    // H => show/hide overlay panel.
    st.visible = !st.visible;
    return true;
}

bool HandleStrongholdOverlayNumpadHotkey(int virtualKey) {
    if (virtualKey != VK_NUMPAD8 && virtualKey != VK_NUMPAD2 && virtualKey != VK_NUMPAD5 && virtualKey != VK_NUMPAD4 &&
        virtualKey != VK_NUMPAD6) {
        return false;
    }

    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return false;
    if (!cfgSnap->strongholdOverlay.enabled) return false;

    std::lock_guard<std::mutex> lock(s_strongholdOverlayMutex);
    auto& st = s_strongholdOverlayState;

    if (!st.initializedVisibility) {
        st.visible = cfgSnap->strongholdOverlay.visible;
        st.initializedVisibility = true;
    }

    if (virtualKey == VK_NUMPAD5) {
        const int frozenThrowCount = std::max(0, st.lastEyeThrowCount);

        st.targetLocked = false;
        st.lockSourceAuto = false;
        st.hasLiveTarget = false;
        st.liveTargetFromNativeTriangulation = false;
        st.hasPrediction = false;
        st.usingLiveTarget = true;
        st.hasAutoLockedOnNether = false;
        st.blockAutoLockUntilThrowClear = false;
        st.relativeYaw = 0.0f;
        st.distanceDisplay = 0.0f;
        st.targetDisplayX = 0;
        st.targetDisplayZ = 0;
        st.playerDisplayX = 0;
        st.playerDisplayZ = 0;
        st.targetNetherX = 0;
        st.targetNetherZ = 0;
        st.estimatedNetherX = 0;
        st.estimatedNetherZ = 0;
        st.playerNetherX = 0;
        st.playerNetherZ = 0;
        st.targetOverworldX = 0;
        st.targetOverworldZ = 0;
        st.estimatedOverworldX = 0;
        st.estimatedOverworldZ = 0;
        st.playerOverworldX = 0;
        st.playerOverworldZ = 0;
        st.activeEyeThrowCount = 0;
        st.ignoredThrowsPrefixCount = frozenThrowCount;
        st.lastThrowAngleAdjustmentDeg = 0.0;
        st.lastAdjustmentStepDirection = 0;
        st.perThrowAngleAdjustmentsDeg.clear();
        st.adjustmentUndoStackDeg.clear();
        st.adjustmentRedoStackDeg.clear();
        st.adjustmentHistoryThrowCount = 0;
        st.lastActiveThrowVerticalAngleDeg = -31.6;
        st.statusLabel = GetUnlockedStatusLabel(false);
        st.infoLabel = "Calc reset. Log new throws.";
        st.debugBasePredictionsLabel.clear();
        st.debugAdjustedPredictionsLabel.clear();
        st.debugSelectionLabel.clear();
        st.showComputedDetails = false;
        s_pendingStandaloneReset.store(true);
        return true;
    }

    if (st.activeEyeThrowCount <= 0) {
        st.infoLabel = "No throws to adjust.";
        return true;
    }

    if (st.targetLocked) {
        st.targetLocked = false;
        st.lockSourceAuto = false;
    }
    // Prevent auto-lock from immediately re-engaging while tuning angle offset.
    st.hasAutoLockedOnNether = true;

    if (st.perThrowAngleAdjustmentsDeg.size() < static_cast<size_t>(st.activeEyeThrowCount)) {
        st.perThrowAngleAdjustmentsDeg.resize(static_cast<size_t>(st.activeEyeThrowCount), 0.0);
    }
    if (st.adjustmentHistoryThrowCount != st.activeEyeThrowCount) {
        st.adjustmentUndoStackDeg.clear();
        st.adjustmentRedoStackDeg.clear();
        st.adjustmentHistoryThrowCount = st.activeEyeThrowCount;
    }

    const size_t lastThrowIndex = static_cast<size_t>(st.activeEyeThrowCount) - 1;
    double currentAdjustment = st.perThrowAngleAdjustmentsDeg[lastThrowIndex];

    if (virtualKey == VK_NUMPAD4) {
        if (st.adjustmentUndoStackDeg.empty()) {
            st.infoLabel = "Undo empty.";
            return true;
        }
        const double previousAdjustment = st.adjustmentUndoStackDeg.back();
        st.adjustmentUndoStackDeg.pop_back();
        st.adjustmentRedoStackDeg.push_back(currentAdjustment);
        st.perThrowAngleAdjustmentsDeg[lastThrowIndex] = previousAdjustment;
        st.lastThrowAngleAdjustmentDeg = previousAdjustment;
        const double deltaApplied = previousAdjustment - currentAdjustment;
        st.lastAdjustmentStepDirection = (deltaApplied > 1e-9) ? 1 : ((deltaApplied < -1e-9) ? -1 : 0);
        st.infoLabel = "Undo adj " + FormatSignedHundredths(st.lastThrowAngleAdjustmentDeg) + ".";
        return true;
    }

    if (virtualKey == VK_NUMPAD6) {
        if (st.adjustmentRedoStackDeg.empty()) {
            st.infoLabel = "Redo empty.";
            return true;
        }
        const double redoAdjustment = st.adjustmentRedoStackDeg.back();
        st.adjustmentRedoStackDeg.pop_back();
        st.adjustmentUndoStackDeg.push_back(currentAdjustment);
        st.perThrowAngleAdjustmentsDeg[lastThrowIndex] = redoAdjustment;
        st.lastThrowAngleAdjustmentDeg = redoAdjustment;
        const double deltaApplied = redoAdjustment - currentAdjustment;
        st.lastAdjustmentStepDirection = (deltaApplied > 1e-9) ? 1 : ((deltaApplied < -1e-9) ? -1 : 0);
        st.infoLabel = "Redo adj " + FormatSignedHundredths(st.lastThrowAngleAdjustmentDeg) + ".";
        return true;
    }

    const double stepDeg = ComputeNbbAngleCorrectionStepDegrees(st.lastActiveThrowVerticalAngleDeg);
    const double delta = (virtualKey == VK_NUMPAD8) ? stepDeg : -stepDeg;
    const double nextAdjustment = std::clamp(currentAdjustment + delta, -5.0, 5.0);
    if (std::abs(nextAdjustment - currentAdjustment) <= 1e-9) {
        st.infoLabel = "Adj limit reached.";
        return true;
    }

    st.adjustmentUndoStackDeg.push_back(currentAdjustment);
    if (st.adjustmentUndoStackDeg.size() > 256) { st.adjustmentUndoStackDeg.erase(st.adjustmentUndoStackDeg.begin()); }
    st.adjustmentRedoStackDeg.clear();
    st.perThrowAngleAdjustmentsDeg[lastThrowIndex] = nextAdjustment;
    st.lastThrowAngleAdjustmentDeg = nextAdjustment;
    st.lastAdjustmentStepDirection = (delta > 0.0) ? 1 : -1;
    st.infoLabel = "Last angle adj " + FormatSignedHundredths(st.lastThrowAngleAdjustmentDeg) + ".";
    return true;
}

void ReportStrongholdLiveMouseDelta(int deltaX, int deltaY) {
    if (deltaX != 0) { s_pendingStrongholdMouseDeltaX.fetch_add(deltaX, std::memory_order_relaxed); }
    if (deltaY != 0) { s_pendingStrongholdMouseDeltaY.fetch_add(deltaY, std::memory_order_relaxed); }
}

void ReportStrongholdLiveKeyState(int virtualKey, bool isDown) {
    const uint32_t bit = StrongholdMovementMaskForVirtualKey(virtualKey);
    if (bit == 0u) return;

    if (isDown) {
        s_strongholdMovementKeyMask.fetch_or(bit, std::memory_order_relaxed);
    } else {
        s_strongholdMovementKeyMask.fetch_and(~bit, std::memory_order_relaxed);
    }
}

void ResetStrongholdLiveInputState() {
    s_pendingStrongholdMouseDeltaX.store(0, std::memory_order_relaxed);
    s_pendingStrongholdMouseDeltaY.store(0, std::memory_order_relaxed);
    s_strongholdMovementKeyMask.store(0, std::memory_order_relaxed);
    s_strongholdLivePlayerPose.lastUpdate = std::chrono::steady_clock::now();
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
        UpdateStrongholdOverlayState();
        UpdateStrongholdCompanionOverlays();
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

    ShutdownStrongholdCompanionOverlays();
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

    ShutdownStrongholdCompanionOverlays();
    ShutdownManagedNinjabrainBotProcess();

    g_logicThreadRunning.store(false);
    Log("[LogicThread] Logic thread stopped");
}
