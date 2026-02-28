#include "logic_thread.h"
#include "expression_parser.h"
#include "gui.h"
#include "mirror_thread.h"
#include "profiler.h"
#include "render.h"
#include "stronghold_companion_overlay.h"
#include "utils.h"
#include "version.h"
#include "json.hpp"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
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
constexpr wchar_t kMcsrApiHost[] = L"api.mcsrranked.com";
constexpr wchar_t kMcsrApiFallbackHost[] = L"mcsrranked.com";
constexpr INTERNET_PORT kMcsrApiPort = INTERNET_DEFAULT_HTTPS_PORT;
constexpr DWORD kMcsrApiTimeoutMs = 1400;
constexpr DWORD kMcsrApiCacheTimeoutMs = 500;
constexpr int kMcsrMatchTypeCasual = 1;
constexpr int kMcsrMatchTypeRanked = 2;
constexpr int kMcsrMatchTypePrivate = 3;
constexpr int kMcsrMatchTypeEvent = 4;
constexpr size_t kMcsrUsernameIndexMaxNames = 8192;
constexpr int kMcsrUsernameIndexWeeklyRefreshSeconds = 7 * 24 * 60 * 60;
constexpr int kMcsrUsernameIndexRefreshRetrySeconds = 20 * 60;
constexpr int kMcsrUsernameIndexMatchPagesPerRefresh = 80;
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
constexpr double kBoatInitIncrementDeg = 1.40625;
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
constexpr ULONGLONG kNbbBoatAngleSettingsRefreshIntervalMs = 750;
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

struct ParsedMcsrUserData {
    bool ok = false;
    std::string uuid;
    std::string nickname;
    std::string country;
    int eloRank = 0;
    int eloRate = 0;
    int peakElo = 0;
    int seasonWinsRanked = 0;
    int seasonLossesRanked = 0;
    int seasonCompletionsRanked = 0;
    int seasonPointsRanked = 0;
    int seasonFfsRanked = 0;
    int seasonDodgesRanked = 0;
    int seasonCurrentWinStreakRanked = 0;
    int allWinsRanked = 0;
    int allLossesRanked = 0;
    int allFfsRanked = 0;
    int bestWinStreak = 0;
    int bestTimeMs = 0;
    int averageTimeMs = 0;
    bool hasForfeitRatePercent = false;
    float forfeitRatePercent = 0.0f;
};

struct ParsedMcsrMatchSummary {
    std::string id;
    int type = 0;
    std::string category;
    std::string gameMode;
    int dateEpochSeconds = 0;
    std::string resultUuid;
    std::string resultName;
    int resultTimeMs = 0;
    bool forfeited = false;
    std::string opponentName;
    bool hasEloAfter = false;
    int eloAfter = 0;
    int eloDelta = 0;
};

struct ParsedMcsrMatchesData {
    bool ok = false;
    std::vector<ParsedMcsrMatchSummary> matches;
};

struct ParsedMcsrTimelineSplit {
    int type = 0;
    int timeMs = 0;
};

struct ParsedMcsrMatchDetailData {
    bool ok = false;
    int completionTimeMs = 0;
    std::vector<ParsedMcsrTimelineSplit> splits;
};

struct ParsedMcsrLeaderboardData {
    bool ok = false;
    std::vector<std::string> nicknames;
};

struct ParsedMcsrMatchFeedUsernamesData {
    bool ok = false;
    bool hasRows = false;
    std::vector<std::string> nicknames;
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
    bool hasLastOverworldRawYaw = false;
    double lastOverworldRawYaw = 0.0;
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

struct McsrApiTrackerRuntimeState {
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

    bool enabled = false;
    bool visible = false;
    bool initializedVisibility = false;
    bool apiOnline = false;
    std::string autoDetectedPlayer;
    std::string autoDetectedUuid;
    std::string requestedPlayer;
    std::string displayPlayer;
    std::string avatarImagePath;
    std::string flagImagePath;
    std::string country;
    std::string userUuid;
    int eloRank = 0;
    int eloRate = 0;
    int peakElo = 0;
    int seasonWins = 0;
    int seasonLosses = 0;
    int seasonCompletions = 0;
    int seasonPoints = 0;
    int bestWinStreak = 0;
    int bestTimeMs = 0;
    int profileAverageTimeMs = 0;
    int seasonFfs = 0;
    int seasonDodges = 0;
    int seasonCurrentWinStreak = 0;
    int recentWins = 0;
    int recentLosses = 0;
    int recentDraws = 0;
    int averageResultTimeMs = 0;
    float recentForfeitRatePercent = 0.0f;
    float profileForfeitRatePercent = 0.0f;
    std::string lastMatchId;
    std::string lastResultLabel;
    int lastResultTimeMs = 0;
    std::vector<int> eloHistory;
    std::vector<TrendPoint> eloTrendPoints;
    std::vector<MatchRow> recentMatches;
    std::vector<std::string> suggestedPlayers;
    std::vector<std::string> splitLines;
    std::string statusLabel;
};

struct McsrAutoPlayerCacheState {
    std::filesystem::path latestLogPath;
    std::filesystem::file_time_type latestWriteTime{};
    bool hasLatestWriteTime = false;
    std::string username;
    ULONGLONG nextRefreshMs = 0;
};

struct McsrAssetCacheState {
    std::string avatarKey;
    std::string avatarPath;
    std::chrono::steady_clock::time_point nextAvatarFetch = std::chrono::steady_clock::time_point::min();
    std::string flagKey;
    std::string flagPath;
    std::chrono::steady_clock::time_point nextFlagFetch = std::chrono::steady_clock::time_point::min();
};

struct McsrCacheServerEndpoint {
    bool enabled = false;
    bool useTls = false;
    INTERNET_PORT port = 0;
    std::wstring host;
    std::wstring basePath;
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
static std::mutex s_mcsrApiTrackerMutex;
static McsrApiTrackerRuntimeState s_mcsrApiTrackerState;
static std::chrono::steady_clock::time_point s_nextMcsrApiTrackerPollTime;
static std::chrono::steady_clock::time_point s_mcsrApiRateLimitUntil;
static int s_mcsrApiRateLimitExponent = 0;
static std::atomic<bool> s_mcsrApiTrackerForceRefresh{ false };
static std::atomic<bool> s_mcsrPreferFallbackHost{ true };
static std::chrono::steady_clock::time_point s_mcsrCacheServerRetryAt;
static McsrAutoPlayerCacheState s_mcsrAutoPlayerCacheState;
static std::mutex s_mcsrSearchOverrideMutex;
static std::string s_mcsrSearchOverridePlayer;
static std::vector<std::string> s_mcsrLeaderboardSuggestions;
static bool s_mcsrUsernameIndexLoaded = false;
static std::chrono::steady_clock::time_point s_mcsrUsernameIndexNextRefresh;
static std::mutex s_mcsrAssetCacheMutex;
static McsrAssetCacheState s_mcsrAssetCacheState;
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

static bool TryReadEnvironmentVariable(const wchar_t* name, std::wstring& outValue);
static std::string SanitizeHttpHeaderToken(std::string token);

static std::wstring ToLowerAscii(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) {
        if (ch >= L'A' && ch <= L'Z') return static_cast<wchar_t>(ch - L'A' + L'a');
        return ch;
    });
    return s;
}

static void TrimAsciiWhitespaceInPlace(std::wstring& value) {
    auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!value.empty() && isSpace(value.front())) value.erase(value.begin());
    while (!value.empty() && isSpace(value.back())) value.pop_back();
}

static bool TryParseMcsrCacheServerUrl(const std::wstring& rawUrl, McsrCacheServerEndpoint& outEndpoint) {
    outEndpoint = McsrCacheServerEndpoint{};
    std::wstring url = rawUrl;
    TrimAsciiWhitespaceInPlace(url);
    if (url.empty()) return false;

    const std::wstring lowerUrl = ToLowerAscii(url);
    if (lowerUrl == L"off" || lowerUrl == L"none" || lowerUrl == L"disabled" || lowerUrl == L"disable") {
        outEndpoint.enabled = false;
        return true;
    }

    size_t schemeEnd = lowerUrl.find(L"://");
    if (schemeEnd == std::wstring::npos) return false;
    const std::wstring scheme = lowerUrl.substr(0, schemeEnd);
    if (scheme == L"http") {
        outEndpoint.useTls = false;
        outEndpoint.port = INTERNET_DEFAULT_HTTP_PORT;
    } else if (scheme == L"https") {
        outEndpoint.useTls = true;
        outEndpoint.port = INTERNET_DEFAULT_HTTPS_PORT;
    } else {
        return false;
    }

    const size_t authorityStart = schemeEnd + 3;
    if (authorityStart >= url.size()) return false;

    size_t pathStart = url.find(L'/', authorityStart);
    size_t queryStart = url.find(L'?', authorityStart);
    size_t fragStart = url.find(L'#', authorityStart);

    size_t authorityEnd = std::wstring::npos;
    if (pathStart != std::wstring::npos) authorityEnd = pathStart;
    if (queryStart != std::wstring::npos) authorityEnd = std::min(authorityEnd, queryStart);
    if (fragStart != std::wstring::npos) authorityEnd = std::min(authorityEnd, fragStart);
    if (authorityEnd == std::wstring::npos) authorityEnd = url.size();

    std::wstring authority = url.substr(authorityStart, authorityEnd - authorityStart);
    TrimAsciiWhitespaceInPlace(authority);
    if (authority.empty()) return false;

    if (authority.front() == L'[') {
        const size_t bracketEnd = authority.find(L']');
        if (bracketEnd == std::wstring::npos) return false;
        outEndpoint.host = authority.substr(0, bracketEnd + 1);
        if (bracketEnd + 1 < authority.size() && authority[bracketEnd + 1] == L':') {
            const std::wstring portText = authority.substr(bracketEnd + 2);
            if (!portText.empty()) {
                try {
                    const int parsedPort = std::stoi(portText);
                    if (parsedPort <= 0 || parsedPort > 65535) return false;
                    outEndpoint.port = static_cast<INTERNET_PORT>(parsedPort);
                } catch (...) {
                    return false;
                }
            }
        } else if (bracketEnd + 1 < authority.size()) {
            return false;
        }
    } else {
        const size_t firstColon = authority.find(L':');
        const size_t lastColon = authority.rfind(L':');
        if (firstColon != std::wstring::npos && firstColon == lastColon) {
            outEndpoint.host = authority.substr(0, firstColon);
            const std::wstring portText = authority.substr(firstColon + 1);
            if (!portText.empty()) {
                try {
                    const int parsedPort = std::stoi(portText);
                    if (parsedPort <= 0 || parsedPort > 65535) return false;
                    outEndpoint.port = static_cast<INTERNET_PORT>(parsedPort);
                } catch (...) {
                    return false;
                }
            }
        } else {
            outEndpoint.host = authority;
        }
    }

    TrimAsciiWhitespaceInPlace(outEndpoint.host);
    if (outEndpoint.host.empty()) return false;

    if (pathStart != std::wstring::npos) {
        std::wstring basePath = url.substr(pathStart);
        const size_t cutPos = basePath.find_first_of(L"?#");
        if (cutPos != std::wstring::npos) basePath.erase(cutPos);
        while (basePath.size() > 1 && basePath.back() == L'/') basePath.pop_back();
        if (basePath == L"/") basePath.clear();
        outEndpoint.basePath = std::move(basePath);
    }

    outEndpoint.enabled = true;
    return true;
}

static McsrCacheServerEndpoint ResolveMcsrCacheServerEndpoint() {
    std::wstring rawUrl;
    if (TryReadEnvironmentVariable(L"MCSR_CACHE_SERVER_URL", rawUrl)) {
        McsrCacheServerEndpoint parsed;
        if (TryParseMcsrCacheServerUrl(rawUrl, parsed)) return parsed;
        Log("[MCSR] Invalid MCSR_CACHE_SERVER_URL: " + WideToUtf8(rawUrl) + ". Falling back to default local cache server.");
    }

    McsrCacheServerEndpoint localDefault;
    localDefault.enabled = true;
    localDefault.useTls = false;
    localDefault.host = L"127.0.0.1";
    localDefault.port = 8787;
    localDefault.basePath.clear();
    return localDefault;
}

static std::wstring BuildMcsrCacheServerRequestPath(const std::wstring& basePath, const std::wstring& requestPath) {
    if (basePath.empty()) return requestPath;
    if (requestPath.empty()) return basePath;
    const bool baseEndsWithSlash = !basePath.empty() && basePath.back() == L'/';
    const bool requestStartsWithSlash = !requestPath.empty() && requestPath.front() == L'/';
    if (baseEndsWithSlash && requestStartsWithSlash) {
        return basePath + requestPath.substr(1);
    }
    if (!baseEndsWithSlash && !requestStartsWithSlash) {
        return basePath + L"/" + requestPath;
    }
    return basePath + requestPath;
}

static std::wstring BuildMcsrCacheServerAuthHeaders() {
    std::wstring token;
    if (!TryReadEnvironmentVariable(L"MCSR_CACHE_AUTH_TOKEN", token)) return L"";
    TrimAsciiWhitespaceInPlace(token);
    if (token.empty()) return L"";

    std::wstring headerNameW;
    std::string headerName = "x-toolscreen-token";
    if (TryReadEnvironmentVariable(L"MCSR_CACHE_AUTH_HEADER", headerNameW)) {
        TrimAsciiWhitespaceInPlace(headerNameW);
        if (!headerNameW.empty()) {
            headerName = SanitizeHttpHeaderToken(WideToUtf8(headerNameW));
            if (headerName.empty()) headerName = "x-toolscreen-token";
        }
    }

    return Utf8ToWide(headerName) + L": " + token + L"\r\n";
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

static bool TryReadMouseSensitivityFromStandardSettingsFile(const std::filesystem::path& standardSettingsPath, double& outSensitivity) {
    outSensitivity = 0.0;
    std::ifstream file(standardSettingsPath);
    if (!file.is_open()) return false;

    nlohmann::json root;
    try {
        file >> root;
    } catch (...) {
        return false;
    }
    if (!root.is_object()) return false;

    std::function<bool(const nlohmann::json&, double&)> parseJsonValue;
    parseJsonValue = [&](const nlohmann::json& value, double& out) -> bool {
        if (value.is_number_float() || value.is_number_integer() || value.is_number_unsigned()) {
            out = std::clamp(value.get<double>(), 0.0, 1.0);
            return true;
        }
        if (value.is_string()) {
            double parsed = 0.0;
            if (!TryParseFlexibleDouble(value.get<std::string>(), parsed)) return false;
            out = std::clamp(parsed, 0.0, 1.0);
            return true;
        }
        if (value.is_object()) {
            auto it = value.find("value");
            if (it != value.end()) return parseJsonValue(*it, out);
        }
        return false;
    };

    auto it = root.find("mouseSensitivity");
    if (it != root.end()) {
        if (parseJsonValue(*it, outSensitivity)) return true;
    }

    it = root.find("sensitivity");
    if (it != root.end()) {
        if (parseJsonValue(*it, outSensitivity)) return true;
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

static bool TryResolveActiveMinecraftConfigPathsForStronghold(std::filesystem::path& outOptionsPath,
                                                              std::filesystem::path& outStandardSettingsPath) {
    outOptionsPath.clear();
    outStandardSettingsPath.clear();

    std::vector<std::filesystem::path> optionCandidates;
    std::vector<std::wstring> seenCandidates;
    auto addInstanceOptions = [&](const std::filesystem::path& base) {
        AddUniquePathCandidate(optionCandidates, seenCandidates, base / L"options.txt");
        AddUniquePathCandidate(optionCandidates, seenCandidates, base / L".minecraft" / L"options.txt");
        AddUniquePathCandidate(optionCandidates, seenCandidates, base / L"minecraft" / L"options.txt");
        AddUniquePathCandidate(optionCandidates, seenCandidates, base / L"game" / L"options.txt");
    };

    std::wstring instMcDir;
    if (TryReadEnvironmentVariable(L"INST_MC_DIR", instMcDir)) {
        AddUniquePathCandidate(optionCandidates, seenCandidates, std::filesystem::path(instMcDir) / L"options.txt");
    }

    std::wstring instDir;
    if (TryReadEnvironmentVariable(L"INST_DIR", instDir)) { addInstanceOptions(std::filesystem::path(instDir)); }

    try {
        const std::filesystem::path cwd = std::filesystem::current_path();
        addInstanceOptions(cwd);
        addInstanceOptions(cwd.parent_path());
    } catch (...) {
    }

    if (!g_toolscreenPath.empty()) {
        try {
            const std::filesystem::path toolscreenDir(g_toolscreenPath);
            addInstanceOptions(toolscreenDir);
            addInstanceOptions(toolscreenDir.parent_path());
        } catch (...) {
        }
    }

    std::wstring userProfile;
    if (TryReadEnvironmentVariable(L"USERPROFILE", userProfile)) {
        const std::filesystem::path userRoot(userProfile);
        AddUniquePathCandidate(optionCandidates, seenCandidates, userRoot / L".minecraft" / L"options.txt");
        AddUniquePathCandidate(optionCandidates, seenCandidates, userRoot / L"AppData" / L"Roaming" / L".minecraft" / L"options.txt");
        AddUniquePathCandidate(optionCandidates, seenCandidates,
                               userRoot / L"Desktop" / L"msr" / L"MultiMC" / L"instances" / L"MCSRRanked-Windows-1.16.1-All" /
                                   L".minecraft" / L"options.txt");
    }

    std::error_code ec;
    std::filesystem::path resolvedOptions;
    for (const auto& candidate : optionCandidates) {
        ec.clear();
        if (!std::filesystem::exists(candidate, ec) || ec) continue;
        if (!std::filesystem::is_regular_file(candidate, ec) || ec) continue;
        resolvedOptions = candidate;
        break;
    }
    if (resolvedOptions.empty()) return false;

    outOptionsPath = resolvedOptions.lexically_normal();

    const std::filesystem::path optionsDir = outOptionsPath.parent_path();
    std::vector<std::filesystem::path> stdCandidates;
    std::vector<std::wstring> seenStdCandidates;
    AddUniquePathCandidate(stdCandidates, seenStdCandidates, optionsDir / L"config" / L"mcsr" / L"standardsettings.json");
    AddUniquePathCandidate(stdCandidates, seenStdCandidates, optionsDir / L"config" / L"standardsettings.json");
    AddUniquePathCandidate(stdCandidates, seenStdCandidates, optionsDir / L".minecraft" / L"config" / L"mcsr" / L"standardsettings.json");
    AddUniquePathCandidate(stdCandidates, seenStdCandidates, optionsDir / L".minecraft" / L"config" / L"standardsettings.json");

    for (const auto& stdPath : stdCandidates) {
        ec.clear();
        if (!std::filesystem::exists(stdPath, ec) || ec) continue;
        if (!std::filesystem::is_regular_file(stdPath, ec) || ec) continue;
        outStandardSettingsPath = stdPath.lexically_normal();
        break;
    }

    return true;
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

static bool IsValidMinecraftUsername(const std::string& value) {
    if (value.size() < 2 || value.size() > 16) return false;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') continue;
        return false;
    }
    return true;
}

static bool IsLikelyMinecraftUuid(const std::string& value) {
    if (value.size() == 32) {
        return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
    }
    if (value.size() == 36) {
        for (size_t i = 0; i < value.size(); ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (value[i] != '-') return false;
            } else if (std::isxdigit(static_cast<unsigned char>(value[i])) == 0) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static std::string SanitizeHttpHeaderToken(std::string token) {
    TrimAsciiWhitespaceInPlace(token);
    std::string out;
    out.reserve(token.size());
    for (unsigned char c : token) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

static bool TryResolveMinecraftIdentityFromCommandLine(std::string& outUsername, std::string& outUuid) {
    outUsername.clear();
    outUuid.clear();

    const wchar_t* rawCmdW = GetCommandLineW();
    if (!rawCmdW || rawCmdW[0] == L'\0') return false;

    const std::string cmd = WideToUtf8(std::wstring(rawCmdW));
    if (cmd.empty()) return false;

    static const std::regex reUsername(R"REGEX((?:^|\s)--username\s+"?([A-Za-z0-9_]{2,16})"?)REGEX");
    static const std::regex reUuid(R"REGEX((?:^|\s)--uuid\s+"?([0-9A-Fa-f-]{32,36})"?)REGEX");

    std::smatch m;
    if (std::regex_search(cmd, m, reUsername) && m.size() >= 2) {
        std::string candidate = m[1].str();
        TrimAsciiWhitespaceInPlace(candidate);
        if (IsValidMinecraftUsername(candidate)) outUsername = candidate;
    }
    if (std::regex_search(cmd, m, reUuid) && m.size() >= 2) {
        std::string candidate = m[1].str();
        TrimAsciiWhitespaceInPlace(candidate);
        if (IsLikelyMinecraftUuid(candidate)) outUuid = candidate;
    }

    return !outUsername.empty() || !outUuid.empty();
}

static bool TryExtractMinecraftUsernameFromLog(const std::filesystem::path& latestLogPath, std::string& outUsername) {
    outUsername.clear();

    std::ifstream file(latestLogPath, std::ios::binary);
    if (!file.is_open()) return false;

    static const std::regex reSettingUser(R"(Setting user:\s*([A-Za-z0-9_]{2,16}))");
    std::string line;
    std::string lastMatched;
    while (std::getline(file, line)) {
        std::smatch m;
        if (std::regex_search(line, m, reSettingUser) && m.size() >= 2) {
            std::string candidate = m[1].str();
            TrimAsciiWhitespaceInPlace(candidate);
            if (IsValidMinecraftUsername(candidate)) { lastMatched = candidate; }
        }
    }

    if (lastMatched.empty()) return false;
    outUsername = lastMatched;
    return true;
}

static bool TryReadSmallTextFile(const std::filesystem::path& path, std::string& outText, size_t maxBytes = 2 * 1024 * 1024) {
    outText.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return false;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
    const std::uintmax_t fileSize = std::filesystem::file_size(path, ec);
    if (ec || fileSize == 0 || fileSize > maxBytes) return false;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    std::string text;
    text.resize(static_cast<size_t>(fileSize));
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    const std::streamsize got = file.gcount();
    if (got <= 0) return false;
    text.resize(static_cast<size_t>(got));
    outText = std::move(text);
    return true;
}

static bool TryExtractMinecraftIdentityFromAccountJson(const std::string& json, std::string& outUsername, std::string& outUuid) {
    outUsername.clear();
    outUuid.clear();

    // MultiMC / Prism style: active account profile identity.
    static const std::regex reActiveProfileName(
        R"REGEX("active"\s*:\s*true[\s\S]*?"profile"\s*:\s*\{[\s\S]*?"name"\s*:\s*"([A-Za-z0-9_]{2,16})")REGEX");
    static const std::regex reActiveProfileId(
        R"REGEX("active"\s*:\s*true[\s\S]*?"profile"\s*:\s*\{[\s\S]*?"id"\s*:\s*"([0-9A-Fa-f-]{32,36})")REGEX");
    // Vanilla/launcher_accounts style.
    static const std::regex reMinecraftProfileName(
        R"REGEX("minecraftProfile"\s*:\s*\{[\s\S]*?"name"\s*:\s*"([A-Za-z0-9_]{2,16})")REGEX");
    static const std::regex reMinecraftProfileId(
        R"REGEX("minecraftProfile"\s*:\s*\{[\s\S]*?"id"\s*:\s*"([0-9A-Fa-f-]{32,36})")REGEX");
    // Fallbacks.
    static const std::regex reDisplayName(R"REGEX("displayName"\s*:\s*"([A-Za-z0-9_]{2,16})")REGEX");
    static const std::regex reProfileName(
        R"REGEX("profile"\s*:\s*\{[\s\S]*?"name"\s*:\s*"([A-Za-z0-9_]{2,16})")REGEX");
    static const std::regex reProfileId(
        R"REGEX("profile"\s*:\s*\{[\s\S]*?"id"\s*:\s*"([0-9A-Fa-f-]{32,36})")REGEX");

    auto tryExtract = [&](const std::regex& pattern, std::string& outValue) -> bool {
        std::smatch m;
        if (!std::regex_search(json, m, pattern) || m.size() < 2) return false;
        outValue = m[1].str();
        TrimAsciiWhitespaceInPlace(outValue);
        return !outValue.empty();
    };

    std::string usernameCandidate;
    if (!tryExtract(reActiveProfileName, usernameCandidate)) {
        if (!tryExtract(reMinecraftProfileName, usernameCandidate)) {
            if (!tryExtract(reDisplayName, usernameCandidate)) { (void)tryExtract(reProfileName, usernameCandidate); }
        }
    }
    if (IsValidMinecraftUsername(usernameCandidate)) outUsername = usernameCandidate;

    std::string uuidCandidate;
    if (!tryExtract(reActiveProfileId, uuidCandidate)) {
        if (!tryExtract(reMinecraftProfileId, uuidCandidate)) { (void)tryExtract(reProfileId, uuidCandidate); }
    }
    if (IsLikelyMinecraftUuid(uuidCandidate)) outUuid = uuidCandidate;

    return !outUsername.empty() || !outUuid.empty();
}

static void AddCommonMinecraftAccountCandidates(std::vector<std::filesystem::path>& outPaths, std::vector<std::wstring>& seenPaths,
                                                const std::filesystem::path& baseDir) {
    if (baseDir.empty()) return;
    AddUniquePathCandidate(outPaths, seenPaths, baseDir / L"accounts.json");
    AddUniquePathCandidate(outPaths, seenPaths, baseDir / L"launcher_accounts.json");
    AddUniquePathCandidate(outPaths, seenPaths, baseDir / L"launcher_profiles.json");
    AddUniquePathCandidate(outPaths, seenPaths, baseDir / L".minecraft" / L"launcher_accounts.json");
    AddUniquePathCandidate(outPaths, seenPaths, baseDir / L".minecraft" / L"launcher_profiles.json");
}

static bool TryResolveMinecraftIdentityFromAccountFiles(std::string& outUsername, std::string& outUuid) {
    outUsername.clear();
    outUuid.clear();

    std::vector<std::filesystem::path> candidates;
    std::vector<std::wstring> seenCandidates;

    std::wstring instMcDirW;
    if (TryReadEnvironmentVariable(L"INST_MC_DIR", instMcDirW)) {
        const std::filesystem::path instMcDir(instMcDirW);
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, instMcDir);
        const std::filesystem::path instanceRoot = instMcDir.parent_path();
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, instanceRoot);
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, instanceRoot.parent_path());
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, instanceRoot.parent_path().parent_path());
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, instanceRoot.parent_path().parent_path().parent_path());
    }

    if (!g_toolscreenPath.empty()) {
        try {
            const std::filesystem::path toolscreenDir(g_toolscreenPath);
            AddCommonMinecraftAccountCandidates(candidates, seenCandidates, toolscreenDir);
            AddCommonMinecraftAccountCandidates(candidates, seenCandidates, toolscreenDir.parent_path());
            AddCommonMinecraftAccountCandidates(candidates, seenCandidates, toolscreenDir.parent_path().parent_path());
            AddCommonMinecraftAccountCandidates(candidates, seenCandidates, toolscreenDir.parent_path().parent_path().parent_path());
        } catch (...) {
        }
    }

    try {
        const std::filesystem::path cwd = std::filesystem::current_path();
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, cwd);
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, cwd.parent_path());
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, cwd.parent_path().parent_path());
    } catch (...) {
    }

    std::wstring userProfile;
    if (TryReadEnvironmentVariable(L"USERPROFILE", userProfile)) {
        const std::filesystem::path userRoot(userProfile);
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, userRoot / L".minecraft");
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, userRoot / L"AppData" / L"Roaming" / L".minecraft");
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, userRoot / L"Desktop" / L"msr" / L"MultiMC");
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, userRoot / L"Desktop" / L"msr");
    }

    std::wstring appData;
    if (TryReadEnvironmentVariable(L"APPDATA", appData)) {
        const std::filesystem::path appDataRoot(appData);
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, appDataRoot / L".minecraft");
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, appDataRoot / L"PrismLauncher");
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, appDataRoot / L"MultiMC");
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, appDataRoot / L"PolyMC");
    }

    std::wstring localAppData;
    if (TryReadEnvironmentVariable(L"LOCALAPPDATA", localAppData)) {
        const std::filesystem::path localAppDataRoot(localAppData);
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, localAppDataRoot / L"PrismLauncher");
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, localAppDataRoot / L"MultiMC");
        AddCommonMinecraftAccountCandidates(candidates, seenCandidates, localAppDataRoot / L"PolyMC");
    }

    std::string newestUsername;
    std::string newestUuid;
    std::filesystem::file_time_type newestWrite{};
    bool found = false;
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || ec) continue;
        if (!std::filesystem::is_regular_file(candidate, ec) || ec) continue;

        std::string json;
        if (!TryReadSmallTextFile(candidate, json)) continue;

        std::string parsedName;
        std::string parsedUuid;
        if (!TryExtractMinecraftIdentityFromAccountJson(json, parsedName, parsedUuid)) continue;

        const auto writeTime = std::filesystem::last_write_time(candidate, ec);
        if (!found || (!ec && writeTime > newestWrite)) {
            found = true;
            newestWrite = writeTime;
            newestUsername = parsedName;
            newestUuid = parsedUuid;
        }
    }

    if (!found) return false;
    if (!newestUsername.empty()) outUsername = newestUsername;
    if (!newestUuid.empty()) outUuid = newestUuid;
    return !outUsername.empty() || !outUuid.empty();
}

static bool TryResolveMcsrAutoDetectedIdentity(std::string& outUsername, std::string& outUuid) {
    outUsername.clear();
    outUuid.clear();

    if (TryResolveMinecraftIdentityFromCommandLine(outUsername, outUuid)) return true;

    // Account files are preferred because they identify the currently signed-in profile
    // even before/without log lines.
    if (TryResolveMinecraftIdentityFromAccountFiles(outUsername, outUuid)) return true;

    std::filesystem::path latestLogPath;
    if (!TryResolveMinecraftLatestLogPath(latestLogPath)) return false;

    const ULONGLONG nowMs = GetTickCount64();
    if (!s_mcsrAutoPlayerCacheState.latestLogPath.empty() && s_mcsrAutoPlayerCacheState.latestLogPath == latestLogPath &&
        nowMs < s_mcsrAutoPlayerCacheState.nextRefreshMs && !s_mcsrAutoPlayerCacheState.username.empty()) {
        outUsername = s_mcsrAutoPlayerCacheState.username;
        return true;
    }

    std::error_code ec;
    const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(latestLogPath, ec);
    if (!ec && !s_mcsrAutoPlayerCacheState.latestLogPath.empty() && s_mcsrAutoPlayerCacheState.latestLogPath == latestLogPath &&
        s_mcsrAutoPlayerCacheState.hasLatestWriteTime && s_mcsrAutoPlayerCacheState.latestWriteTime == writeTime &&
        !s_mcsrAutoPlayerCacheState.username.empty()) {
        s_mcsrAutoPlayerCacheState.nextRefreshMs = nowMs + 2000;
        outUsername = s_mcsrAutoPlayerCacheState.username;
        return true;
    }

    std::string parsedUsername;
    if (!TryExtractMinecraftUsernameFromLog(latestLogPath, parsedUsername)) {
        s_mcsrAutoPlayerCacheState.latestLogPath = latestLogPath;
        if (!ec) {
            s_mcsrAutoPlayerCacheState.latestWriteTime = writeTime;
            s_mcsrAutoPlayerCacheState.hasLatestWriteTime = true;
        } else {
            s_mcsrAutoPlayerCacheState.hasLatestWriteTime = false;
        }
        s_mcsrAutoPlayerCacheState.nextRefreshMs = nowMs + 3000;
        return false;
    }

    s_mcsrAutoPlayerCacheState.latestLogPath = latestLogPath;
    if (!ec) {
        s_mcsrAutoPlayerCacheState.latestWriteTime = writeTime;
        s_mcsrAutoPlayerCacheState.hasLatestWriteTime = true;
    } else {
        s_mcsrAutoPlayerCacheState.hasLatestWriteTime = false;
    }
    s_mcsrAutoPlayerCacheState.username = parsedUsername;
    s_mcsrAutoPlayerCacheState.nextRefreshMs = nowMs + 2000;
    outUsername = parsedUsername;
    return true;
}

static bool TryResolveMouseSensitivityFromOptionsTxt(double& outSensitivity) {
    outSensitivity = 0.0;

    std::filesystem::path activeOptionsPath;
    std::filesystem::path activeStandardSettingsPath;
    if (TryResolveActiveMinecraftConfigPathsForStronghold(activeOptionsPath, activeStandardSettingsPath)) {
        double parsed = 0.0;
        if (!activeOptionsPath.empty() && TryReadMouseSensitivityFromOptionsFile(activeOptionsPath, parsed)) {
            outSensitivity = parsed;
            return true;
        }
    }

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

static bool TryResolveMouseSensitivityFromStandardSettingsJson(double& outSensitivity) {
    outSensitivity = 0.0;

    std::filesystem::path activeOptionsPath;
    std::filesystem::path activeStandardSettingsPath;
    if (TryResolveActiveMinecraftConfigPathsForStronghold(activeOptionsPath, activeStandardSettingsPath)) {
        double parsed = 0.0;
        if (!activeStandardSettingsPath.empty() && TryReadMouseSensitivityFromStandardSettingsFile(activeStandardSettingsPath, parsed)) {
            outSensitivity = parsed;
            return true;
        }
    }
    return false;
}

static NbbBoatAngleSettings GetResolvedNbbBoatAngleSettings() {
    const ULONGLONG now = GetTickCount64();
    if (s_cachedNbbBoatAngleSettingsInitialized &&
        now - s_cachedNbbBoatAngleSettingsRefreshMs <= kNbbBoatAngleSettingsRefreshIntervalMs) {
        return s_cachedNbbBoatAngleSettings;
    }

    NbbBoatAngleSettings resolved;

    bool sensitivityResolved = false;
    double sensitivity = 0.0;
    if (TryResolveMouseSensitivityFromStandardSettingsJson(sensitivity) || TryResolveMouseSensitivityFromOptionsTxt(sensitivity)) {
        resolved.sensitivityAutomatic = std::clamp(sensitivity, 0.0, 1.0);
        sensitivityResolved = true;
    }

    if (!sensitivityResolved) {
        if (TryReadRegistryDouble(HKEY_CURRENT_USER, kNbbPrefsRegistrySubkey, kNbbSensitivityRegistryValue, sensitivity)) {
            resolved.sensitivityAutomatic = std::clamp(sensitivity, 0.0, 1.0);
            sensitivityResolved = true;
        }
    }

    if (!sensitivityResolved) {
        if (auto cfgSnap = GetConfigSnapshot(); cfgSnap) {
            const double appliedSensitivity = static_cast<double>(cfgSnap->boatSetup.appliedRecommendedSensitivity);
            if (cfgSnap->boatSetup.enabled && std::isfinite(appliedSensitivity) && appliedSensitivity >= 0.0 &&
                appliedSensitivity <= 1.0) {
                resolved.sensitivityAutomatic = std::clamp(appliedSensitivity, 0.0, 1.0);
                sensitivityResolved = true;
            }
        }
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

    // Boat yaw is valid only on the 360/256 grid (1.40625). Captures taken during
    // the initial settle phase (0.140625 grid) should fail this check.
    const float candidate = static_cast<float>(std::round(rawAngleDeg / kBoatInitIncrementDeg) * kBoatInitIncrementDeg);
    const double roundedCandidate = std::round(static_cast<double>(candidate) * 100.0) / 100.0;
    if (std::abs(roundedCandidate - rawAngleDeg) > kBoatInitErrorLimitDeg) return false;

    outBoatAngleDeg = candidate;
    return true;
}

static bool IsBoatEyeSensitivityEligible(double sensitivity) {
    if (!std::isfinite(sensitivity)) return false;
    const double minIncrement = MinecraftYawDegreesPerMouseCount(std::clamp(sensitivity, 0.0, 1.0));
    if (!std::isfinite(minIncrement)) return false;
    // Boat-eye decimal inference requires minimum increment > 0.01 deg.
    return minIncrement > 0.01;
}

static bool IsLikelyMod360Discontinuity(double previousRawYawDeg, double currentRawYawDeg) {
    if (!std::isfinite(previousRawYawDeg) || !std::isfinite(currentRawYawDeg)) return false;
    // The copied F3+C yaw is "total yaw"; portal/relog/pearl can mod it back
    // into [-360, 360], creating a hard discontinuity for boat-eye inference.
    const bool wasOutsideWrapRange = std::abs(previousRawYawDeg) > 360.0;
    const bool nowInsideWrapRange = std::abs(currentRawYawDeg) <= 360.0;
    if (!wasOutsideWrapRange || !nowInsideWrapRange) return false;
    return std::abs(currentRawYawDeg - previousRawYawDeg) >= 180.0;
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

static bool HttpGetJson(const wchar_t* host, INTERNET_PORT port, const wchar_t* requestPath, DWORD timeoutMs, bool useTls,
                        std::string& outJson, DWORD* outStatusCode = nullptr, DWORD* outLastError = nullptr,
                        const wchar_t* extraHeaders = nullptr) {
    outJson.clear();
    if (outStatusCode) *outStatusCode = 0;
    if (outLastError) *outLastError = 0;
    if (!s_winHttpApi.EnsureLoaded()) return false;

    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hRequest = nullptr;
    bool success = false;

    do {
        hSession =
            s_winHttpApi.open(L"Toolscreen/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            if (outLastError) *outLastError = GetLastError();
            break;
        }

        s_winHttpApi.setTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

        hConnect = s_winHttpApi.connect(hSession, host, port, 0);
        if (!hConnect) {
            if (outLastError) *outLastError = GetLastError();
            break;
        }

        const DWORD requestFlags = useTls ? WINHTTP_FLAG_SECURE : 0;
        hRequest = s_winHttpApi.openRequest(hConnect, L"GET", requestPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            requestFlags);
        if (!hRequest) {
            if (outLastError) *outLastError = GetLastError();
            break;
        }

        std::wstring headerBlob = L"Accept: application/json\r\n";
        if (extraHeaders && extraHeaders[0] != L'\0') {
            headerBlob += extraHeaders;
            if (headerBlob.size() < 2 || headerBlob.substr(headerBlob.size() - 2) != L"\r\n") { headerBlob += L"\r\n"; }
        }

        if (!s_winHttpApi.sendRequest(hRequest, headerBlob.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            if (outLastError) *outLastError = GetLastError();
            break;
        }
        if (!s_winHttpApi.receiveResponse(hRequest, nullptr)) {
            if (outLastError) *outLastError = GetLastError();
            break;
        }

        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        if (!s_winHttpApi.queryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                       &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX)) {
            if (outLastError) *outLastError = GetLastError();
            break;
        }
        if (outStatusCode) *outStatusCode = statusCode;
        if (statusCode != 200) break;

        std::string response;
        while (true) {
            DWORD bytesAvailable = 0;
            if (!s_winHttpApi.queryDataAvailable(hRequest, &bytesAvailable)) {
                if (outLastError) *outLastError = GetLastError();
                break;
            }
            if (bytesAvailable == 0) {
                outJson = std::move(response);
                success = !outJson.empty();
                break;
            }

            std::vector<char> buffer(bytesAvailable);
            DWORD bytesRead = 0;
            if (!s_winHttpApi.readData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
                if (outLastError) *outLastError = GetLastError();
                break;
            }
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
    return HttpGetJson(kStrongholdApiHost, kStrongholdApiPort, kStrongholdApiPath, kStrongholdApiTimeoutMs, false, outJson);
}

static bool HttpGetInformationMessagesJson(std::string& outJson) {
    return HttpGetJson(kStrongholdApiHost, kStrongholdApiPort, kInformationMessagesApiPath, kStrongholdApiTimeoutMs, false, outJson);
}

static bool HttpGetBinary(const wchar_t* host, INTERNET_PORT port, const wchar_t* requestPath, DWORD timeoutMs, bool useTls,
                          std::vector<unsigned char>& outBytes, DWORD* outStatusCode = nullptr, DWORD* outLastError = nullptr,
                          const wchar_t* extraHeaders = nullptr) {
    outBytes.clear();
    if (outStatusCode) *outStatusCode = 0;
    if (outLastError) *outLastError = 0;
    if (!s_winHttpApi.EnsureLoaded()) return false;

    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hRequest = nullptr;
    bool success = false;

    do {
        hSession =
            s_winHttpApi.open(L"Toolscreen/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            if (outLastError) *outLastError = GetLastError();
            break;
        }
        s_winHttpApi.setTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

        hConnect = s_winHttpApi.connect(hSession, host, port, 0);
        if (!hConnect) {
            if (outLastError) *outLastError = GetLastError();
            break;
        }

        const DWORD requestFlags = useTls ? WINHTTP_FLAG_SECURE : 0;
        hRequest = s_winHttpApi.openRequest(hConnect, L"GET", requestPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            requestFlags);
        if (!hRequest) {
            if (outLastError) *outLastError = GetLastError();
            break;
        }

        std::wstring headerBlob = L"Accept: image/png,image/*,*/*\r\n";
        if (extraHeaders && extraHeaders[0] != L'\0') {
            headerBlob += extraHeaders;
            if (headerBlob.size() < 2 || headerBlob.substr(headerBlob.size() - 2) != L"\r\n") { headerBlob += L"\r\n"; }
        }

        if (!s_winHttpApi.sendRequest(hRequest, headerBlob.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            if (outLastError) *outLastError = GetLastError();
            break;
        }
        if (!s_winHttpApi.receiveResponse(hRequest, nullptr)) {
            if (outLastError) *outLastError = GetLastError();
            break;
        }

        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        if (!s_winHttpApi.queryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                       &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX)) {
            if (outLastError) *outLastError = GetLastError();
            break;
        }
        if (outStatusCode) *outStatusCode = statusCode;
        if (statusCode != 200) break;

        std::vector<unsigned char> response;
        while (true) {
            DWORD bytesAvailable = 0;
            if (!s_winHttpApi.queryDataAvailable(hRequest, &bytesAvailable)) {
                if (outLastError) *outLastError = GetLastError();
                break;
            }
            if (bytesAvailable == 0) {
                outBytes = std::move(response);
                success = !outBytes.empty();
                break;
            }
            const size_t offset = response.size();
            response.resize(offset + static_cast<size_t>(bytesAvailable));
            DWORD bytesRead = 0;
            if (!s_winHttpApi.readData(hRequest, response.data() + offset, bytesAvailable, &bytesRead)) {
                if (outLastError) *outLastError = GetLastError();
                break;
            }
            if (bytesRead == 0) break;
            response.resize(offset + static_cast<size_t>(bytesRead));
        }
    } while (false);

    if (hRequest) s_winHttpApi.closeHandle(hRequest);
    if (hConnect) s_winHttpApi.closeHandle(hConnect);
    if (hSession) s_winHttpApi.closeHandle(hSession);
    return success;
}

static bool HttpGetMcsrJson(const std::wstring& requestPath, std::string& outJson, DWORD* outStatusCode = nullptr,
                            DWORD* outLastError = nullptr, const std::wstring& extraHeaders = L"") {
    if (requestPath.empty()) return false;
    const std::wstring cacheAuthHeaders = BuildMcsrCacheServerAuthHeaders();

    const auto now = std::chrono::steady_clock::now();
    if (now >= s_mcsrCacheServerRetryAt) {
        const McsrCacheServerEndpoint cacheEndpoint = ResolveMcsrCacheServerEndpoint();
        if (cacheEndpoint.enabled && !cacheEndpoint.host.empty() && cacheEndpoint.port > 0) {
            DWORD cacheStatus = 0;
            DWORD cacheError = 0;
            const std::wstring cacheRequestPath = BuildMcsrCacheServerRequestPath(cacheEndpoint.basePath, requestPath);
            std::wstring cacheRequestHeaders = extraHeaders;
            if (!cacheAuthHeaders.empty()) cacheRequestHeaders += cacheAuthHeaders;
            const bool cacheOk = HttpGetJson(cacheEndpoint.host.c_str(), cacheEndpoint.port, cacheRequestPath.c_str(),
                                             kMcsrApiCacheTimeoutMs, cacheEndpoint.useTls, outJson, &cacheStatus, &cacheError,
                                             cacheRequestHeaders.empty() ? nullptr : cacheRequestHeaders.c_str());
            if (cacheOk) {
                s_mcsrCacheServerRetryAt = std::chrono::steady_clock::time_point::min();
                if (outStatusCode) *outStatusCode = 200;
                if (outLastError) *outLastError = 0;
                return true;
            }

            const bool cacheNetworkError = (cacheStatus == 0 && cacheError != 0);
            if (cacheNetworkError) {
                s_mcsrCacheServerRetryAt = now + std::chrono::seconds(15);
            } else {
                // Service is reachable but returned an API status (e.g. 404/429).
                // Retry soon so fresh cache is used as soon as it is valid again.
                s_mcsrCacheServerRetryAt = now + std::chrono::seconds(2);
            }
        }
    }

    auto requestOnHost = [&](const wchar_t* host, DWORD* statusCodeOut, DWORD* lastErrorOut) -> bool {
        return HttpGetJson(host, kMcsrApiPort, requestPath.c_str(), kMcsrApiTimeoutMs, true, outJson, statusCodeOut, lastErrorOut,
                           extraHeaders.empty() ? nullptr : extraHeaders.c_str());
    };

    DWORD statusA = 0;
    DWORD errorA = 0;
    DWORD statusB = 0;
    DWORD errorB = 0;
    const bool preferFallback = s_mcsrPreferFallbackHost.load(std::memory_order_relaxed);

    const wchar_t* firstHost = preferFallback ? kMcsrApiFallbackHost : kMcsrApiHost;
    const wchar_t* secondHost = preferFallback ? kMcsrApiHost : kMcsrApiFallbackHost;

    if (requestOnHost(firstHost, &statusA, &errorA)) {
        s_mcsrPreferFallbackHost.store(firstHost == kMcsrApiFallbackHost, std::memory_order_relaxed);
        if (outStatusCode) *outStatusCode = 200;
        if (outLastError) *outLastError = 0;
        return true;
    }

    const bool firstNotFoundLike = (statusA == 400 || statusA == 404);
    const bool firstNetworkLike = (statusA == 0 && errorA != 0);
    if (firstNotFoundLike || firstNetworkLike) {
        if (requestOnHost(secondHost, &statusB, &errorB)) {
            s_mcsrPreferFallbackHost.store(secondHost == kMcsrApiFallbackHost, std::memory_order_relaxed);
            if (outStatusCode) *outStatusCode = 200;
            if (outLastError) *outLastError = 0;
            return true;
        }

        if (outStatusCode) *outStatusCode = (statusB != 0) ? statusB : statusA;
        if (outLastError) *outLastError = (errorB != 0) ? errorB : errorA;
        return false;
    }

    if (outStatusCode) *outStatusCode = statusA;
    if (outLastError) *outLastError = errorA;
    return false;
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
    const bool mod360Discontinuity =
        !allowNonBoatThrows && isOverworldSnapshot && s_standaloneStrongholdState.hasBoatAngle &&
        s_standaloneStrongholdState.hasLastOverworldRawYaw &&
        IsLikelyMod360Discontinuity(s_standaloneStrongholdState.lastOverworldRawYaw, parsed.horizontalAngle);
    if (isOverworldSnapshot) {
        s_standaloneStrongholdState.hasLastOverworldRawYaw = true;
        s_standaloneStrongholdState.lastOverworldRawYaw = parsed.horizontalAngle;
    }

    if (mod360Discontinuity) {
        // Mod-360 events (portal/relog/pearl) break boat-eye continuity.
        // Force re-init and discard stale throws.
        s_standaloneStrongholdState.boatState = kBoatStateFailed;
        s_standaloneStrongholdState.hasBoatAngle = false;
        s_standaloneStrongholdState.boatAngleDeg = 0.0;
        s_standaloneStrongholdState.eyeThrows.clear();
        return;
    }

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
                s_standaloneStrongholdState.eyeThrows.clear();
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
        s_standaloneStrongholdState.hasLastOverworldRawYaw = false;
        s_standaloneStrongholdState.lastOverworldRawYaw = 0.0;
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
        if (!IsBoatEyeSensitivityEligible(settings.sensitivityAutomatic)) {
            s_standaloneStrongholdState.boatState = kBoatStateFailed;
            s_standaloneStrongholdState.hasBoatAngle = false;
            s_standaloneStrongholdState.boatAngleDeg = 0.0;
            return;
        }
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

static std::string UrlEncodePathSegment(const std::string& text) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (unsigned char c : text) {
        const bool isAlpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool isDigit = (c >= '0' && c <= '9');
        const bool isSafe = (c == '-') || (c == '_') || (c == '.') || (c == '~');
        if (isAlpha || isDigit || isSafe) {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

static std::string ToLowerAsciiCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static bool EqualsIgnoreCaseAscii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

static bool ContainsIgnoreCaseAscii(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return ToLowerAsciiCopy(haystack).find(ToLowerAsciiCopy(needle)) != std::string::npos;
}

static void PushUniqueCaseInsensitive(std::vector<std::string>& values, const std::string& value, size_t maxCount) {
    std::string trimmed = value;
    TrimAsciiWhitespaceInPlace(trimmed);
    if (trimmed.empty()) return;
    for (const std::string& existing : values) {
        if (EqualsIgnoreCaseAscii(existing, trimmed)) return;
    }
    values.push_back(std::move(trimmed));
    if (values.size() > maxCount) values.resize(maxCount);
}

static std::string FormatDurationMs(int durationMs) {
    if (durationMs <= 0) return "--:--.--";
    const int totalSeconds = durationMs / 1000;
    const int minutes = totalSeconds / 60;
    const int seconds = totalSeconds % 60;
    const int centiseconds = (durationMs % 1000) / 10;
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << minutes << ":" << std::setw(2) << seconds << "." << std::setw(2)
        << centiseconds;
    return out.str();
}

static std::string FormatAgeShortFromEpoch(int epochSeconds) {
    if (epochSeconds <= 0) return "--";
    const std::time_t now = std::time(nullptr);
    if (now <= 0) return "--";
    int delta = static_cast<int>(now - static_cast<std::time_t>(epochSeconds));
    if (delta < 0) delta = 0;
    if (delta < 60) return std::to_string(delta) + "s";
    if (delta < 3600) return std::to_string(delta / 60) + "m";
    if (delta < 86400) return std::to_string(delta / 3600) + "h";
    return std::to_string(delta / 86400) + "d";
}

enum class McsrMatchCategoryType : int {
    Ranked = 0,
    Private = 1,
    Casual = 2,
    Event = 3,
    Other = 4,
};

static bool TryClassifyMcsrMatchCategoryFromType(int type, McsrMatchCategoryType& outCategory) {
    switch (type) {
    case kMcsrMatchTypeRanked:
        outCategory = McsrMatchCategoryType::Ranked;
        return true;
    case kMcsrMatchTypePrivate:
        outCategory = McsrMatchCategoryType::Private;
        return true;
    case kMcsrMatchTypeCasual:
        outCategory = McsrMatchCategoryType::Casual;
        return true;
    case kMcsrMatchTypeEvent:
        outCategory = McsrMatchCategoryType::Event;
        return true;
    default:
        return false;
    }
}

static McsrMatchCategoryType ClassifyMcsrMatchCategory(const ParsedMcsrMatchSummary& match) {
    const std::string modeLower = ToLowerAsciiCopy(match.gameMode);
    const std::string categoryLower = ToLowerAsciiCopy(match.category);
    if (ContainsIgnoreCaseAscii(modeLower, "private") || ContainsIgnoreCaseAscii(categoryLower, "private")) {
        return McsrMatchCategoryType::Private;
    }
    if (ContainsIgnoreCaseAscii(modeLower, "casual") || ContainsIgnoreCaseAscii(categoryLower, "casual")) {
        return McsrMatchCategoryType::Casual;
    }
    if (ContainsIgnoreCaseAscii(modeLower, "event") || ContainsIgnoreCaseAscii(categoryLower, "event") ||
        ContainsIgnoreCaseAscii(categoryLower, "tournament") || ContainsIgnoreCaseAscii(categoryLower, "weekly")) {
        return McsrMatchCategoryType::Event;
    }
    if (ContainsIgnoreCaseAscii(modeLower, "ranked") || ContainsIgnoreCaseAscii(categoryLower, "ranked")) {
        return McsrMatchCategoryType::Ranked;
    }
    McsrMatchCategoryType fromType = McsrMatchCategoryType::Other;
    if (TryClassifyMcsrMatchCategoryFromType(match.type, fromType)) return fromType;
    return McsrMatchCategoryType::Other;
}

static bool IsMcsrRankedMatch(const ParsedMcsrMatchSummary& match) {
    return ClassifyMcsrMatchCategory(match) == McsrMatchCategoryType::Ranked;
}

static std::string McsrTimelineTypeLabel(int type) {
    switch (type) {
    case 2:
        return "Portal";
    case 7:
        return "Bastion";
    case 11:
        return "Fortress";
    case 12:
        return "Travel";
    case 15:
        return "Finish";
    default:
        return "Split " + std::to_string(type);
    }
}

static std::filesystem::path GetMcsrUsernameIndexPath() {
    if (!g_toolscreenPath.empty()) { return std::filesystem::path(g_toolscreenPath) / L"mcsr_username_index.txt"; }
    return std::filesystem::path(L"mcsr_username_index.txt");
}

static std::filesystem::path GetMcsrUsernameIndexMetaPath() {
    if (!g_toolscreenPath.empty()) { return std::filesystem::path(g_toolscreenPath) / L"mcsr_username_index.meta"; }
    return std::filesystem::path(L"mcsr_username_index.meta");
}

static bool TryReadEpochSecondsFile(const std::filesystem::path& path, std::time_t& outEpochSeconds) {
    outEpochSeconds = 0;
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line;
    if (!std::getline(in, line)) return false;
    TrimAsciiWhitespaceInPlace(line);
    if (line.empty()) return false;
    try {
        const long long parsed = std::stoll(line);
        if (parsed <= 0) return false;
        outEpochSeconds = static_cast<std::time_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

static void WriteEpochSecondsFile(const std::filesystem::path& path, std::time_t epochSeconds) {
    if (epochSeconds <= 0) return;
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return;
    out << static_cast<long long>(epochSeconds) << "\n";
}

static void LoadMcsrUsernameIndexFromDiskIfNeeded() {
    if (s_mcsrUsernameIndexLoaded) return;
    s_mcsrUsernameIndexLoaded = true;

    s_mcsrLeaderboardSuggestions.clear();
    std::ifstream in(GetMcsrUsernameIndexPath());
    if (in.is_open()) {
        std::string line;
        while (std::getline(in, line)) {
            TrimAsciiWhitespaceInPlace(line);
            if (!IsValidMinecraftUsername(line)) continue;
            PushUniqueCaseInsensitive(s_mcsrLeaderboardSuggestions, line, kMcsrUsernameIndexMaxNames);
            if (s_mcsrLeaderboardSuggestions.size() >= kMcsrUsernameIndexMaxNames) break;
        }
    }

    const auto nowSteady = std::chrono::steady_clock::now();
    const std::time_t nowEpoch = std::time(nullptr);
    if (nowEpoch <= 0) {
        s_mcsrUsernameIndexNextRefresh = nowSteady;
        return;
    }

    std::time_t lastSyncEpoch = 0;
    if (!TryReadEpochSecondsFile(GetMcsrUsernameIndexMetaPath(), lastSyncEpoch) || lastSyncEpoch > nowEpoch) {
        s_mcsrUsernameIndexNextRefresh = nowSteady;
        return;
    }

    const long long elapsedSeconds = static_cast<long long>(nowEpoch - lastSyncEpoch);
    if (elapsedSeconds < kMcsrUsernameIndexWeeklyRefreshSeconds) {
        const int remainingSeconds = static_cast<int>(kMcsrUsernameIndexWeeklyRefreshSeconds - elapsedSeconds);
        s_mcsrUsernameIndexNextRefresh = nowSteady + std::chrono::seconds(remainingSeconds);
    } else {
        s_mcsrUsernameIndexNextRefresh = nowSteady;
    }
}

static bool SaveMcsrUsernameIndexToDisk(const std::vector<std::string>& names) {
    const std::filesystem::path indexPath = GetMcsrUsernameIndexPath();
    std::error_code ec;
    if (!indexPath.parent_path().empty()) std::filesystem::create_directories(indexPath.parent_path(), ec);

    std::ofstream out(indexPath, std::ios::trunc);
    if (!out.is_open()) return false;
    for (const std::string& name : names) {
        if (!IsValidMinecraftUsername(name)) continue;
        out << name << "\n";
    }
    if (!out.good()) return false;

    const std::time_t nowEpoch = std::time(nullptr);
    WriteEpochSecondsFile(GetMcsrUsernameIndexMetaPath(), nowEpoch);
    return true;
}

static void MergeMcsrGlobalSuggestions(std::vector<std::string>& out, size_t maxCount) {
    for (const std::string& globalName : s_mcsrLeaderboardSuggestions) {
        PushUniqueCaseInsensitive(out, globalName, maxCount);
        if (out.size() >= maxCount) break;
    }
}

static ParsedMcsrUserData ParseMcsrUserPayload(const std::string& json) {
    static const std::regex reUuid("\"uuid\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reNickname("\"nickname\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reCountry("\"country\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reEloRank("\"eloRank\"\\s*:\\s*(-?\\d+)");
    static const std::regex reEloRate("\"eloRate\"\\s*:\\s*(-?\\d+)");
    static const std::regex rePeakElo("\"(?:peakElo|eloPeak)\"\\s*:\\s*(-?\\d+)");
    static const std::regex reRankedCount("\"ranked\"\\s*:\\s*(-?\\d+)");
    static const std::regex reRankedCountFloat("\"ranked\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
    static const std::regex reAllCountFloat("\"all\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
    static const std::regex reValueCountFloat("\"value\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
    static const std::regex reForfeitRate("\"(?:forfeitRate|forfeitRatePercent|ffRate)\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
    static const std::regex reAverageTime("\"(?:averageTime|avgTime)\"\\s*:\\s*(-?\\d+)");
    static const std::regex reAchievementId("\"id\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reAchievementValue("\"value\"\\s*:\\s*(-?\\d+)");

    ParsedMcsrUserData out;
    std::string dataObject;
    if (!ExtractJsonEnclosedAfterKey(json, "data", '{', '}', dataObject)) return out;

    ExtractRegexString(dataObject, reUuid, out.uuid);
    ExtractRegexString(dataObject, reNickname, out.nickname);
    ExtractRegexString(dataObject, reCountry, out.country);
    ExtractRegexInt(dataObject, reEloRank, out.eloRank);
    ExtractRegexInt(dataObject, reEloRate, out.eloRate);
    ExtractRegexInt(dataObject, rePeakElo, out.peakElo);

    double selectedForfeitRatePercent = -1.0;
    int selectedForfeitRatePriority = -1;
    int selectedAverageTimeMs = -1;
    int selectedAverageTimePriority = -1;
    auto normalizeForfeitRatePercent = [](double raw) {
        if (!std::isfinite(raw)) return -1.0;
        if (raw >= 0.0 && raw <= 1.0) raw *= 100.0;
        return std::clamp(raw, 0.0, 100.0);
    };
    auto considerForfeitRatePercent = [&](double raw, int priority) {
        const double normalized = normalizeForfeitRatePercent(raw);
        if (normalized < 0.0) return;
        if (priority > selectedForfeitRatePriority) {
            selectedForfeitRatePriority = priority;
            selectedForfeitRatePercent = normalized;
        }
    };
    auto considerAverageTimeMs = [&](int avgMs, int priority) {
        if (avgMs <= 0) return;
        if (priority > selectedAverageTimePriority) {
            selectedAverageTimePriority = priority;
            selectedAverageTimeMs = avgMs;
        }
    };
    auto tryExtractForfeitRateFromObject = [&](const std::string& objectText, double& outRate) {
        for (const char* rateKey : { "forfeitRate", "forfeitRatePercent", "ffRate" }) {
            std::string rateObject;
            if (!ExtractJsonEnclosedAfterKey(objectText, rateKey, '{', '}', rateObject)) continue;
            if (ExtractRegexDouble(rateObject, reRankedCountFloat, outRate)) return true;
            if (ExtractRegexDouble(rateObject, reAllCountFloat, outRate)) return true;
            if (ExtractRegexDouble(rateObject, reValueCountFloat, outRate)) return true;
        }
        if (ExtractRegexDouble(objectText, reForfeitRate, outRate)) return true;
        return false;
    };
    auto tryExtractAverageTimeFromObject = [&](const std::string& objectText, int& outAvgMs) {
        for (const char* avgKey : { "averageTime", "avgTime" }) {
            std::string avgObject;
            if (!ExtractJsonEnclosedAfterKey(objectText, avgKey, '{', '}', avgObject)) continue;
            int rankedInt = 0;
            if (ExtractRegexInt(avgObject, reRankedCount, rankedInt)) {
                outAvgMs = std::max(0, rankedInt);
                return true;
            }
            double rankedFloat = 0.0;
            if (ExtractRegexDouble(avgObject, reRankedCountFloat, rankedFloat)) {
                outAvgMs = std::max(0, static_cast<int>(std::lround(rankedFloat)));
                return true;
            }
            double allFloat = 0.0;
            if (ExtractRegexDouble(avgObject, reAllCountFloat, allFloat)) {
                outAvgMs = std::max(0, static_cast<int>(std::lround(allFloat)));
                return true;
            }
            double valueFloat = 0.0;
            if (ExtractRegexDouble(avgObject, reValueCountFloat, valueFloat)) {
                outAvgMs = std::max(0, static_cast<int>(std::lround(valueFloat)));
                return true;
            }
        }
        if (ExtractRegexInt(objectText, reAverageTime, outAvgMs)) return true;
        return false;
    };
    int topLevelAverageTime = 0;
    if (tryExtractAverageTimeFromObject(dataObject, topLevelAverageTime)) {
        considerAverageTimeMs(topLevelAverageTime, 220);
    }

    std::string statsObject;
    if (ExtractJsonEnclosedAfterKey(dataObject, "statistics", '{', '}', statsObject)) {
        auto extractRankedFrom = [&](const std::string& parentObject, const char* key, int& outValue) {
            std::string nestedObject;
            if (!ExtractJsonEnclosedAfterKey(parentObject, key, '{', '}', nestedObject)) return false;
            return ExtractRegexInt(nestedObject, reRankedCount, outValue);
        };

        std::string seasonObject;
        if (ExtractJsonEnclosedAfterKey(statsObject, "season", '{', '}', seasonObject)) {
            extractRankedFrom(seasonObject, "wins", out.seasonWinsRanked);
            if (!extractRankedFrom(seasonObject, "loses", out.seasonLossesRanked)) {
                extractRankedFrom(seasonObject, "losses", out.seasonLossesRanked);
            }
            extractRankedFrom(seasonObject, "completions", out.seasonCompletionsRanked);
            extractRankedFrom(seasonObject, "points", out.seasonPointsRanked);
            extractRankedFrom(seasonObject, "ffs", out.seasonFfsRanked);
            extractRankedFrom(seasonObject, "dodges", out.seasonDodgesRanked);
            extractRankedFrom(seasonObject, "currentWinStreak", out.seasonCurrentWinStreakRanked);
            int seasonAverageTime = 0;
            if (tryExtractAverageTimeFromObject(seasonObject, seasonAverageTime)) {
                considerAverageTimeMs(seasonAverageTime, 120);
            }
            double seasonForfeitRate = 0.0;
            if (tryExtractForfeitRateFromObject(seasonObject, seasonForfeitRate)) {
                considerForfeitRatePercent(seasonForfeitRate, 120);
            }
        }

        std::string overallObject;
        bool hasOverallStats = false;
        for (const char* overallKey : { "all", "allTime", "overall", "global", "lifetime" }) {
            if (ExtractJsonEnclosedAfterKey(statsObject, overallKey, '{', '}', overallObject)) {
                hasOverallStats = true;
                break;
            }
        }
        if (hasOverallStats) {
            extractRankedFrom(overallObject, "wins", out.allWinsRanked);
            if (!extractRankedFrom(overallObject, "loses", out.allLossesRanked)) {
                extractRankedFrom(overallObject, "losses", out.allLossesRanked);
            }
            extractRankedFrom(overallObject, "ffs", out.allFfsRanked);
            int overallAverageTime = 0;
            if (tryExtractAverageTimeFromObject(overallObject, overallAverageTime)) {
                considerAverageTimeMs(overallAverageTime, 320);
            }
            double overallForfeitRate = 0.0;
            if (tryExtractForfeitRateFromObject(overallObject, overallForfeitRate)) {
                considerForfeitRatePercent(overallForfeitRate, 300);
            }
            const int totalAllGames = std::max(0, out.allWinsRanked + out.allLossesRanked);
            if (totalAllGames > 0) {
                const double computedAllRate =
                    (100.0 * static_cast<double>(std::max(0, out.allFfsRanked))) / static_cast<double>(totalAllGames);
                considerForfeitRatePercent(computedAllRate, 260);
            }
        }

        int statsAverageTime = 0;
        if (tryExtractAverageTimeFromObject(statsObject, statsAverageTime)) {
            // Low-priority fallback because this can include nested season/all structures.
            considerAverageTimeMs(statsAverageTime, 80);
        }
        double statsForfeitRate = 0.0;
        if (tryExtractForfeitRateFromObject(statsObject, statsForfeitRate)) {
            // Lowest priority fallback because this can include nested season/all blobs.
            considerForfeitRatePercent(statsForfeitRate, 80);
        }
    }

    if (selectedForfeitRatePriority < 0) {
        const int totalSeasonGames = std::max(0, out.seasonWinsRanked + out.seasonLossesRanked);
        if (totalSeasonGames > 0) {
            const double computedSeasonRate =
                (100.0 * static_cast<double>(std::max(0, out.seasonFfsRanked))) / static_cast<double>(totalSeasonGames);
            considerForfeitRatePercent(computedSeasonRate, 110);
        }
    }
    if (selectedForfeitRatePriority >= 0) {
        out.hasForfeitRatePercent = true;
        out.forfeitRatePercent = static_cast<float>(selectedForfeitRatePercent);
    }

    std::string achievementsObject;
    std::string displayArray;
    if (ExtractJsonEnclosedAfterKey(dataObject, "achievements", '{', '}', achievementsObject) &&
        ExtractJsonEnclosedAfterKey(achievementsObject, "display", '[', ']', displayArray)) {
        for (const std::string& achievementObject : ExtractTopLevelObjectsFromArray(displayArray)) {
            std::string id;
            int value = 0;
            if (!ExtractRegexString(achievementObject, reAchievementId, id)) continue;
            if (!ExtractRegexInt(achievementObject, reAchievementValue, value)) continue;
            const std::string idLower = ToLowerAsciiCopy(id);
            if (idLower == "besttime") {
                out.bestTimeMs = std::max(0, value);
            } else if (idLower == "highestwinstreak") {
                out.bestWinStreak = std::max(0, value);
            } else if (idLower == "averagetime" || idLower == "avgtime") {
                considerAverageTimeMs(std::max(0, value), 260);
            }
        }
    }

    if (selectedAverageTimePriority >= 0) {
        out.averageTimeMs = selectedAverageTimeMs;
    } else {
        out.averageTimeMs = 0;
    }

    if (out.bestWinStreak <= 0) out.bestWinStreak = std::max(0, out.seasonCurrentWinStreakRanked);
    out.ok = !out.uuid.empty() || !out.nickname.empty();
    return out;
}

static ParsedMcsrMatchesData ParseMcsrMatchesPayload(const std::string& json, const std::string& playerUuid,
                                                     const std::string& playerNickname) {
    static const std::regex reIdStr("\"id\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reIdInt("\"id\"\\s*:\\s*(-?\\d+)");
    static const std::regex reType("\"type\"\\s*:\\s*(-?\\d+)");
    static const std::regex reCategory("\"category\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reGameMode("\"gameMode\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reDateEpoch("\"date\"\\s*:\\s*(-?\\d+)");
    static const std::regex reForfeited("\"forfeited\"\\s*:\\s*(true|false)");
    static const std::regex reResultUuid("\"uuid\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reResultNickname("\"nickname\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reResultMcName("\"mc_name\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reResultName("\"name\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reResultTime("\"time\"\\s*:\\s*(-?\\d+)");
    static const std::regex reChange("\"change\"\\s*:\\s*(-?\\d+)");
    static const std::regex reEloRate("\"eloRate\"\\s*:\\s*(-?\\d+)");

    ParsedMcsrMatchesData out;
    std::string dataArray;
    if (!ExtractJsonEnclosedAfterKey(json, "data", '[', ']', dataArray)) return out;

    for (const std::string& matchObject : ExtractTopLevelObjectsFromArray(dataArray)) {
        ParsedMcsrMatchSummary parsed;
        if (!ExtractRegexString(matchObject, reIdStr, parsed.id)) {
            int numericId = 0;
            if (ExtractRegexInt(matchObject, reIdInt, numericId)) parsed.id = std::to_string(numericId);
        }
        if (parsed.id.empty()) continue;

        ExtractRegexInt(matchObject, reType, parsed.type);
        ExtractRegexString(matchObject, reCategory, parsed.category);
        ExtractRegexString(matchObject, reGameMode, parsed.gameMode);
        ExtractRegexInt(matchObject, reDateEpoch, parsed.dateEpochSeconds);
        ExtractRegexBool(matchObject, reForfeited, parsed.forfeited);

        std::string resultObject;
        if (ExtractJsonEnclosedAfterKey(matchObject, "result", '{', '}', resultObject)) {
            ExtractRegexString(resultObject, reResultUuid, parsed.resultUuid);
            if (!ExtractRegexString(resultObject, reResultNickname, parsed.resultName)) {
                if (!ExtractRegexString(resultObject, reResultMcName, parsed.resultName)) {
                    ExtractRegexString(resultObject, reResultName, parsed.resultName);
                }
            }
            ExtractRegexInt(resultObject, reResultTime, parsed.resultTimeMs);
        }

        std::vector<std::pair<std::string, std::string>> players;
        std::string playersArray;
        if (ExtractJsonEnclosedAfterKey(matchObject, "players", '[', ']', playersArray)) {
            for (const std::string& playerObject : ExtractTopLevelObjectsFromArray(playersArray)) {
                std::string playerUuidValue;
                std::string playerNameValue;
                ExtractRegexString(playerObject, reResultUuid, playerUuidValue);
                if (!ExtractRegexString(playerObject, reResultNickname, playerNameValue)) {
                    if (!ExtractRegexString(playerObject, reResultMcName, playerNameValue)) {
                        if (!ExtractRegexString(playerObject, reResultName, playerNameValue)) {
                            std::string playerUserObject;
                            if (ExtractJsonEnclosedAfterKey(playerObject, "user", '{', '}', playerUserObject)) {
                                if (!ExtractRegexString(playerUserObject, reResultNickname, playerNameValue)) {
                                    if (!ExtractRegexString(playerUserObject, reResultMcName, playerNameValue)) {
                                        ExtractRegexString(playerUserObject, reResultName, playerNameValue);
                                    }
                                }
                            }
                        }
                    }
                }
                players.emplace_back(std::move(playerUuidValue), std::move(playerNameValue));
            }
        }

        if (parsed.resultName.empty() && !parsed.resultUuid.empty()) {
            for (const auto& player : players) {
                if (!player.first.empty() && EqualsIgnoreCaseAscii(player.first, parsed.resultUuid) && !player.second.empty()) {
                    parsed.resultName = player.second;
                    break;
                }
            }
        }

        for (const auto& player : players) {
            bool isSelf = false;
            if (!playerUuid.empty() && !player.first.empty() && EqualsIgnoreCaseAscii(player.first, playerUuid)) {
                isSelf = true;
            } else if (!playerNickname.empty() && !player.second.empty() && EqualsIgnoreCaseAscii(player.second, playerNickname)) {
                isSelf = true;
            }
            if (!isSelf && !player.second.empty()) {
                parsed.opponentName = player.second;
                break;
            }
        }

        std::string changesArray;
        if (ExtractJsonEnclosedAfterKey(matchObject, "changes", '[', ']', changesArray)) {
            for (const std::string& changeObject : ExtractTopLevelObjectsFromArray(changesArray)) {
                std::string changeUuid;
                ExtractRegexString(changeObject, reResultUuid, changeUuid);
                const bool isSelf = !playerUuid.empty() && !changeUuid.empty() && EqualsIgnoreCaseAscii(changeUuid, playerUuid);
                if (!isSelf && parsed.hasEloAfter) continue;
                int eloAfter = 0;
                if (ExtractRegexInt(changeObject, reEloRate, eloAfter)) {
                    parsed.hasEloAfter = true;
                    parsed.eloAfter = eloAfter;
                }
                ExtractRegexInt(changeObject, reChange, parsed.eloDelta);
                if (isSelf) break;
            }
        }

        if (parsed.resultName.empty() && !parsed.opponentName.empty() && !playerNickname.empty()) {
            const bool opponentWon = !parsed.resultUuid.empty() && !playerUuid.empty() && !EqualsIgnoreCaseAscii(parsed.resultUuid, playerUuid);
            if (opponentWon) { parsed.resultName = parsed.opponentName; }
        }

        if (parsed.resultName.empty() && !parsed.opponentName.empty()) {
            const bool selfWon = !parsed.resultUuid.empty() && !playerUuid.empty() && EqualsIgnoreCaseAscii(parsed.resultUuid, playerUuid);
            if (selfWon) { parsed.resultName = playerNickname; }
        }

        if (parsed.resultTimeMs <= 0) {
            std::string completionTimeObject;
            if (ExtractJsonEnclosedAfterKey(matchObject, "result", '{', '}', completionTimeObject)) {
                ExtractRegexInt(completionTimeObject, reResultTime, parsed.resultTimeMs);
            }
        }

        if (parsed.resultName.empty() && parsed.resultUuid.empty()) {
            if (!ExtractRegexString(matchObject, reResultMcName, parsed.resultName)) {
                ExtractRegexString(matchObject, reResultName, parsed.resultName);
            }
            if (!ExtractRegexString(resultObject, reResultMcName, parsed.resultName)) {
                ExtractRegexString(resultObject, reResultName, parsed.resultName);
            }
        }

        out.matches.push_back(std::move(parsed));
    }

    out.ok = true;
    return out;
}

static ParsedMcsrMatchDetailData ParseMcsrMatchDetailPayload(const std::string& json, const std::string& playerUuid) {
    static const std::regex reUuid("\"uuid\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reType("\"type\"\\s*:\\s*(-?\\d+)");
    static const std::regex reTime("\"time\"\\s*:\\s*(-?\\d+)");

    ParsedMcsrMatchDetailData out;
    std::string dataObject;
    if (!ExtractJsonEnclosedAfterKey(json, "data", '{', '}', dataObject)) return out;

    std::string completionsArray;
    if (ExtractJsonEnclosedAfterKey(dataObject, "completions", '[', ']', completionsArray)) {
        for (const std::string& completionObject : ExtractTopLevelObjectsFromArray(completionsArray)) {
            std::string uuid;
            if (!ExtractRegexString(completionObject, reUuid, uuid)) continue;
            if (!playerUuid.empty() && !EqualsIgnoreCaseAscii(uuid, playerUuid)) continue;
            int completionMs = 0;
            if (ExtractRegexInt(completionObject, reTime, completionMs)) {
                out.completionTimeMs = completionMs;
                break;
            }
        }
    }

    std::string timelinesArray;
    if (ExtractJsonEnclosedAfterKey(dataObject, "timelines", '[', ']', timelinesArray)) {
        for (const std::string& timelineObject : ExtractTopLevelObjectsFromArray(timelinesArray)) {
            std::string uuid;
            if (!ExtractRegexString(timelineObject, reUuid, uuid)) continue;
            if (!playerUuid.empty() && !EqualsIgnoreCaseAscii(uuid, playerUuid)) continue;

            ParsedMcsrTimelineSplit split;
            if (!ExtractRegexInt(timelineObject, reType, split.type)) continue;
            if (!ExtractRegexInt(timelineObject, reTime, split.timeMs)) continue;
            out.splits.push_back(split);
        }
    }

    std::sort(out.splits.begin(), out.splits.end(),
              [](const ParsedMcsrTimelineSplit& a, const ParsedMcsrTimelineSplit& b) { return a.timeMs < b.timeMs; });
    out.ok = true;
    return out;
}

static ParsedMcsrLeaderboardData ParseMcsrLeaderboardPayload(const std::string& json) {
    static const std::regex reNickname("\"nickname\"\\s*:\\s*\"([^\"]+)\"");

    ParsedMcsrLeaderboardData out;
    std::string dataObject;
    if (!ExtractJsonEnclosedAfterKey(json, "data", '{', '}', dataObject)) return out;

    std::string usersArray;
    if (!ExtractJsonEnclosedAfterKey(dataObject, "users", '[', ']', usersArray)) return out;

    for (const std::string& userObject : ExtractTopLevelObjectsFromArray(usersArray)) {
        std::string nickname;
        if (!ExtractRegexString(userObject, reNickname, nickname)) continue;
        TrimAsciiWhitespaceInPlace(nickname);
        if (!IsValidMinecraftUsername(nickname)) continue;
        PushUniqueCaseInsensitive(out.nicknames, nickname, kMcsrUsernameIndexMaxNames);
    }

    out.ok = true;
    return out;
}

static ParsedMcsrLeaderboardData ParseMcsrRecordLeaderboardPayload(const std::string& json) {
    static const std::regex reNickname("\"nickname\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reMcName("\"mc_name\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reName("\"name\"\\s*:\\s*\"([^\"]+)\"");

    ParsedMcsrLeaderboardData out;
    std::string dataArray;
    if (!ExtractJsonEnclosedAfterKey(json, "data", '[', ']', dataArray)) return out;

    for (const std::string& rowObject : ExtractTopLevelObjectsFromArray(dataArray)) {
        std::string userObject;
        if (!ExtractJsonEnclosedAfterKey(rowObject, "user", '{', '}', userObject)) continue;

        std::string nickname;
        if (!ExtractRegexString(userObject, reNickname, nickname)) {
            if (!ExtractRegexString(userObject, reMcName, nickname)) {
                ExtractRegexString(userObject, reName, nickname);
            }
        }

        TrimAsciiWhitespaceInPlace(nickname);
        if (!IsValidMinecraftUsername(nickname)) continue;
        PushUniqueCaseInsensitive(out.nicknames, nickname, kMcsrUsernameIndexMaxNames);
    }

    out.ok = true;
    return out;
}

static ParsedMcsrMatchFeedUsernamesData ParseMcsrMatchFeedUsernamesPayload(const std::string& json) {
    static const std::regex reNickname("\"nickname\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reMcName("\"mc_name\"\\s*:\\s*\"([^\"]+)\"");
    static const std::regex reName("\"name\"\\s*:\\s*\"([^\"]+)\"");

    ParsedMcsrMatchFeedUsernamesData out;
    std::string dataArray;
    if (!ExtractJsonEnclosedAfterKey(json, "data", '[', ']', dataArray)) return out;

    const std::vector<std::string> matches = ExtractTopLevelObjectsFromArray(dataArray);
    out.hasRows = !matches.empty();

    for (const std::string& matchObject : matches) {
        std::string playersArray;
        if (!ExtractJsonEnclosedAfterKey(matchObject, "players", '[', ']', playersArray)) continue;
        for (const std::string& playerObject : ExtractTopLevelObjectsFromArray(playersArray)) {
            std::string nickname;
            if (!ExtractRegexString(playerObject, reNickname, nickname)) {
                if (!ExtractRegexString(playerObject, reMcName, nickname)) {
                    if (!ExtractRegexString(playerObject, reName, nickname)) {
                        std::string userObject;
                        if (ExtractJsonEnclosedAfterKey(playerObject, "user", '{', '}', userObject)) {
                            if (!ExtractRegexString(userObject, reNickname, nickname)) {
                                if (!ExtractRegexString(userObject, reMcName, nickname)) {
                                    ExtractRegexString(userObject, reName, nickname);
                                }
                            }
                        }
                    }
                }
            }

            TrimAsciiWhitespaceInPlace(nickname);
            if (!IsValidMinecraftUsername(nickname)) continue;
            PushUniqueCaseInsensitive(out.nicknames, nickname, kMcsrUsernameIndexMaxNames);
        }
    }

    out.ok = true;
    return out;
}

static bool DidPlayerWinMatch(const ParsedMcsrMatchSummary& match, const ParsedMcsrUserData& user) {
    if (!user.uuid.empty() && !match.resultUuid.empty()) { return EqualsIgnoreCaseAscii(user.uuid, match.resultUuid); }
    if (!user.nickname.empty() && !match.resultName.empty()) { return EqualsIgnoreCaseAscii(user.nickname, match.resultName); }
    if (!match.resultName.empty() && !user.nickname.empty()) { return ToLowerAsciiCopy(match.resultName) == ToLowerAsciiCopy(user.nickname); }
    return false;
}

static int ClassifyMcsrMatchOutcome(const ParsedMcsrMatchSummary& match, const ParsedMcsrUserData& user) {
    if (match.resultUuid.empty() && match.resultName.empty()) return 0;
    return DidPlayerWinMatch(match, user) ? 1 : -1;
}

static void ResetMcsrApiRateLimitBackoff() {
    s_mcsrApiRateLimitUntil = std::chrono::steady_clock::time_point::min();
    s_mcsrApiRateLimitExponent = 0;
}

static int RegisterMcsrApiRateLimitBackoff(int pollIntervalMs) {
    const int baseSeconds = std::max(30, pollIntervalMs / 1000);
    const int exponent = std::clamp(s_mcsrApiRateLimitExponent, 0, 4);
    int waitSeconds = baseSeconds;
    waitSeconds *= (1 << exponent);
    waitSeconds = std::clamp(waitSeconds, 30, 300);
    s_mcsrApiRateLimitUntil = std::chrono::steady_clock::now() + std::chrono::seconds(waitSeconds);
    s_nextMcsrApiTrackerPollTime = s_mcsrApiRateLimitUntil;
    s_mcsrApiRateLimitExponent = std::clamp(s_mcsrApiRateLimitExponent + 1, 0, 6);
    return waitSeconds;
}

static void MaybeRefreshMcsrUsernameIndex(const std::wstring& extraHeaders, bool forceRefresh) {
    LoadMcsrUsernameIndexFromDiskIfNeeded();

    const auto now = std::chrono::steady_clock::now();
    if (!forceRefresh && now < s_mcsrUsernameIndexNextRefresh) return;

    std::vector<std::string> mergedNames = s_mcsrLeaderboardSuggestions;
    bool gotAnyData = false;
    bool hitRateLimit = false;

    auto mergeNames = [&](const std::vector<std::string>& names) {
        for (const std::string& name : names) {
            PushUniqueCaseInsensitive(mergedNames, name, kMcsrUsernameIndexMaxNames);
            if (mergedNames.size() >= kMcsrUsernameIndexMaxNames) break;
        }
    };

    {
        std::string payload;
        DWORD statusCode = 0;
        DWORD lastError = 0;
        if (HttpGetMcsrJson(L"/api/leaderboard", payload, &statusCode, &lastError, extraHeaders)) {
            ParsedMcsrLeaderboardData parsed = ParseMcsrLeaderboardPayload(payload);
            if (parsed.ok) {
                if (!parsed.nicknames.empty()) gotAnyData = true;
                mergeNames(parsed.nicknames);
            }
        } else if (statusCode == 429) {
            hitRateLimit = true;
        }
    }

    if (!hitRateLimit) {
        std::string payload;
        DWORD statusCode = 0;
        DWORD lastError = 0;
        if (HttpGetMcsrJson(L"/api/record-leaderboard", payload, &statusCode, &lastError, extraHeaders)) {
            ParsedMcsrLeaderboardData parsed = ParseMcsrRecordLeaderboardPayload(payload);
            if (parsed.ok) {
                if (!parsed.nicknames.empty()) gotAnyData = true;
                mergeNames(parsed.nicknames);
            }
        } else if (statusCode == 429) {
            hitRateLimit = true;
        }
    }

    if (!hitRateLimit) {
        for (int page = 0; page < kMcsrUsernameIndexMatchPagesPerRefresh; ++page) {
            std::wstring path = L"/api/matches?page=" + std::to_wstring(page);
            std::string payload;
            DWORD statusCode = 0;
            DWORD lastError = 0;
            if (!HttpGetMcsrJson(path, payload, &statusCode, &lastError, extraHeaders)) {
                if (statusCode == 429) hitRateLimit = true;
                break;
            }

            ParsedMcsrMatchFeedUsernamesData parsed = ParseMcsrMatchFeedUsernamesPayload(payload);
            if (!parsed.ok) break;
            if (!parsed.hasRows) break;
            if (!parsed.nicknames.empty()) {
                gotAnyData = true;
                mergeNames(parsed.nicknames);
            }
            if (mergedNames.size() >= kMcsrUsernameIndexMaxNames) break;
        }
    }

    if (gotAnyData && !mergedNames.empty()) {
        std::sort(mergedNames.begin(), mergedNames.end(), [](const std::string& a, const std::string& b) {
            return ToLowerAsciiCopy(a) < ToLowerAsciiCopy(b);
        });
        s_mcsrLeaderboardSuggestions = mergedNames;
        (void)SaveMcsrUsernameIndexToDisk(s_mcsrLeaderboardSuggestions);
        s_mcsrUsernameIndexNextRefresh = now + std::chrono::seconds(kMcsrUsernameIndexWeeklyRefreshSeconds);
        return;
    }

    if (hitRateLimit) {
        s_mcsrUsernameIndexNextRefresh = now + std::chrono::seconds(kMcsrUsernameIndexRefreshRetrySeconds);
    } else if (s_mcsrLeaderboardSuggestions.empty()) {
        s_mcsrUsernameIndexNextRefresh = now + std::chrono::minutes(15);
    } else {
        s_mcsrUsernameIndexNextRefresh = now + std::chrono::hours(6);
    }
}

static std::string SanitizeMcsrAssetKey(const std::string& source, size_t maxLen = 64) {
    std::string out;
    out.reserve(std::min(source.size(), maxLen));
    for (unsigned char c : source) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out.push_back(static_cast<char>(std::tolower(c)));
            if (out.size() >= maxLen) break;
        }
    }
    return out;
}

static std::filesystem::path GetMcsrAssetCacheRootPath() {
    std::wstring localAppData;
    if (TryReadEnvironmentVariable(L"LOCALAPPDATA", localAppData) && !localAppData.empty()) {
        return std::filesystem::path(localAppData) / L"Toolscreen" / L"cache" / L"mcsr";
    }
    std::wstring tempDir;
    if (TryReadEnvironmentVariable(L"TEMP", tempDir) && !tempDir.empty()) {
        return std::filesystem::path(tempDir) / L"toolscreen_mcsr_cache";
    }
    return std::filesystem::path(L".") / L"toolscreen_mcsr_cache";
}

static bool TryWriteBinaryFile(const std::filesystem::path& filePath, const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(filePath.parent_path(), ec);
    if (ec) return false;

    const std::filesystem::path tempPath = filePath.wstring() + L".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) return false;
    }

    std::filesystem::rename(tempPath, filePath, ec);
    if (!ec) return true;
    ec.clear();
    std::filesystem::remove(filePath, ec);
    ec.clear();
    std::filesystem::rename(tempPath, filePath, ec);
    return !ec;
}

static bool LooksLikeImageBytes(const std::vector<unsigned char>& bytes) {
    if (bytes.size() >= 8) {
        // PNG: 89 50 4E 47 0D 0A 1A 0A
        if (bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47 && bytes[4] == 0x0D &&
            bytes[5] == 0x0A && bytes[6] == 0x1A && bytes[7] == 0x0A) {
            return true;
        }
    }
    if (bytes.size() >= 3) {
        // JPEG: FF D8 FF
        if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) return true;
    }
    if (bytes.size() >= 12) {
        // WEBP: RIFF....WEBP
        if (bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' && bytes[8] == 'W' && bytes[9] == 'E' &&
            bytes[10] == 'B' && bytes[11] == 'P') {
            return true;
        }
    }
    if (bytes.size() >= 6) {
        // GIF87a/GIF89a
        if (bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == '8' &&
            (bytes[4] == '7' || bytes[4] == '9') && bytes[5] == 'a') {
            return true;
        }
    }
    return false;
}

static bool FileLooksLikeImage(const std::filesystem::path& filePath) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in.is_open()) return false;
    std::vector<unsigned char> header(16, 0);
    in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const std::streamsize readCount = in.gcount();
    if (readCount <= 0) return false;
    header.resize(static_cast<size_t>(readCount));
    return LooksLikeImageBytes(header);
}

static std::string RemoveUuidDashes(std::string uuidText) {
    uuidText.erase(std::remove(uuidText.begin(), uuidText.end(), '-'), uuidText.end());
    return uuidText;
}

static std::filesystem::path GetMcsrTrackerCacheDbRootPath() {
    return GetMcsrAssetCacheRootPath() / L"tracker_db";
}

static std::filesystem::path GetMcsrTrackerCachePathForKey(const std::string& cacheKey) {
    const std::string normalized = SanitizeMcsrAssetKey(cacheKey, 96);
    if (normalized.empty()) return {};
    return GetMcsrTrackerCacheDbRootPath() / L"users" / (Utf8ToWide(normalized) + L".json");
}

static void ApplyMcsrTrackerRuntimeEnvelope(McsrApiTrackerRuntimeState& state, bool enabled, bool visible, bool initializedVisibility,
                                            const std::string& autoDetectedPlayer, const std::string& autoDetectedUuid,
                                            const std::string& requestedIdentifier) {
    state.enabled = enabled;
    state.visible = visible;
    state.initializedVisibility = initializedVisibility;
    state.autoDetectedPlayer = autoDetectedPlayer;
    state.autoDetectedUuid = autoDetectedUuid;
    state.requestedPlayer = requestedIdentifier;
    if (state.displayPlayer.empty()) state.displayPlayer = requestedIdentifier;
}

static bool TrySerializeMcsrTrackerCache(const McsrApiTrackerRuntimeState& state, std::string& outJsonText) {
    try {
        nlohmann::json j = nlohmann::json::object();
        j["schema"] = 1;
        j["savedEpochSeconds"] = static_cast<long long>(std::time(nullptr));

        j["displayPlayer"] = state.displayPlayer;
        j["requestedPlayer"] = state.requestedPlayer;
        j["country"] = state.country;
        j["userUuid"] = state.userUuid;
        j["avatarImagePath"] = state.avatarImagePath;
        j["flagImagePath"] = state.flagImagePath;
        j["eloRank"] = state.eloRank;
        j["eloRate"] = state.eloRate;
        j["peakElo"] = state.peakElo;
        j["seasonWins"] = state.seasonWins;
        j["seasonLosses"] = state.seasonLosses;
        j["seasonCompletions"] = state.seasonCompletions;
        j["seasonPoints"] = state.seasonPoints;
        j["bestWinStreak"] = state.bestWinStreak;
        j["bestTimeMs"] = state.bestTimeMs;
        j["profileAverageTimeMs"] = state.profileAverageTimeMs;
        j["averageResultTimeMs"] = state.averageResultTimeMs;
        j["seasonFfs"] = state.seasonFfs;
        j["seasonDodges"] = state.seasonDodges;
        j["seasonCurrentWinStreak"] = state.seasonCurrentWinStreak;
        j["recentWins"] = state.recentWins;
        j["recentLosses"] = state.recentLosses;
        j["recentDraws"] = state.recentDraws;
        j["recentForfeitRatePercent"] = state.recentForfeitRatePercent;
        j["profileForfeitRatePercent"] = state.profileForfeitRatePercent;
        j["lastMatchId"] = state.lastMatchId;
        j["lastResultLabel"] = state.lastResultLabel;
        j["lastResultTimeMs"] = state.lastResultTimeMs;
        j["statusLabel"] = state.statusLabel;
        j["apiOnline"] = state.apiOnline;
        j["eloHistory"] = state.eloHistory;
        j["splitLines"] = state.splitLines;
        j["suggestedPlayers"] = state.suggestedPlayers;

        nlohmann::json recentMatches = nlohmann::json::array();
        for (const auto& row : state.recentMatches) {
            nlohmann::json outRow = nlohmann::json::object();
            outRow["opponent"] = row.opponent;
            outRow["resultLabel"] = row.resultLabel;
            outRow["detailLabel"] = row.detailLabel;
            outRow["ageLabel"] = row.ageLabel;
            outRow["resultType"] = row.resultType;
            outRow["forfeited"] = row.forfeited;
            outRow["categoryType"] = row.categoryType;
            recentMatches.push_back(std::move(outRow));
        }
        j["recentMatches"] = std::move(recentMatches);

        nlohmann::json trendPoints = nlohmann::json::array();
        for (const auto& point : state.eloTrendPoints) {
            nlohmann::json outPoint = nlohmann::json::object();
            outPoint["elo"] = point.elo;
            outPoint["opponent"] = point.opponent;
            outPoint["resultLabel"] = point.resultLabel;
            outPoint["detailLabel"] = point.detailLabel;
            outPoint["ageLabel"] = point.ageLabel;
            trendPoints.push_back(std::move(outPoint));
        }
        j["eloTrendPoints"] = std::move(trendPoints);

        outJsonText = j.dump(2);
        return !outJsonText.empty();
    } catch (...) {
        return false;
    }
}

static bool TryDeserializeMcsrTrackerCache(const std::string& jsonText, McsrApiTrackerRuntimeState& outState,
                                           std::time_t* outSavedEpochSeconds = nullptr) {
    if (outSavedEpochSeconds) *outSavedEpochSeconds = 0;
    try {
        const nlohmann::json j = nlohmann::json::parse(jsonText, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return false;
        if (j.contains("schema") && j["schema"].is_number_integer() && j["schema"].get<int>() != 1) return false;

        McsrApiTrackerRuntimeState state;

        auto readString = [&](const char* key, std::string& out) {
            auto it = j.find(key);
            if (it != j.end() && it->is_string()) out = it->get<std::string>();
        };
        auto readInt = [&](const char* key, int& out) {
            auto it = j.find(key);
            if (it == j.end()) return;
            if (it->is_number_integer()) {
                out = it->get<int>();
            } else if (it->is_number_float()) {
                out = static_cast<int>(std::lround(it->get<double>()));
            }
        };
        auto readFloat = [&](const char* key, float& out) {
            auto it = j.find(key);
            if (it == j.end()) return;
            if (it->is_number()) out = static_cast<float>(it->get<double>());
        };
        auto readBool = [&](const char* key, bool& out) {
            auto it = j.find(key);
            if (it != j.end() && it->is_boolean()) out = it->get<bool>();
        };

        readString("displayPlayer", state.displayPlayer);
        readString("requestedPlayer", state.requestedPlayer);
        readString("country", state.country);
        readString("userUuid", state.userUuid);
        readString("avatarImagePath", state.avatarImagePath);
        readString("flagImagePath", state.flagImagePath);
        readString("lastMatchId", state.lastMatchId);
        readString("lastResultLabel", state.lastResultLabel);
        readString("statusLabel", state.statusLabel);

        readInt("eloRank", state.eloRank);
        readInt("eloRate", state.eloRate);
        readInt("peakElo", state.peakElo);
        readInt("seasonWins", state.seasonWins);
        readInt("seasonLosses", state.seasonLosses);
        readInt("seasonCompletions", state.seasonCompletions);
        readInt("seasonPoints", state.seasonPoints);
        readInt("bestWinStreak", state.bestWinStreak);
        readInt("bestTimeMs", state.bestTimeMs);
        readInt("profileAverageTimeMs", state.profileAverageTimeMs);
        readInt("averageResultTimeMs", state.averageResultTimeMs);
        readInt("seasonFfs", state.seasonFfs);
        readInt("seasonDodges", state.seasonDodges);
        readInt("seasonCurrentWinStreak", state.seasonCurrentWinStreak);
        readInt("recentWins", state.recentWins);
        readInt("recentLosses", state.recentLosses);
        readInt("recentDraws", state.recentDraws);
        readInt("lastResultTimeMs", state.lastResultTimeMs);

        readFloat("recentForfeitRatePercent", state.recentForfeitRatePercent);
        readFloat("profileForfeitRatePercent", state.profileForfeitRatePercent);
        readBool("apiOnline", state.apiOnline);

        constexpr size_t kMaxCachedRows = 256;
        auto histIt = j.find("eloHistory");
        if (histIt != j.end() && histIt->is_array()) {
            for (const auto& value : *histIt) {
                if (!value.is_number()) continue;
                state.eloHistory.push_back(static_cast<int>(std::lround(value.get<double>())));
                if (state.eloHistory.size() >= kMaxCachedRows) break;
            }
        }

        auto splitIt = j.find("splitLines");
        if (splitIt != j.end() && splitIt->is_array()) {
            for (const auto& value : *splitIt) {
                if (!value.is_string()) continue;
                state.splitLines.push_back(value.get<std::string>());
                if (state.splitLines.size() >= kMaxCachedRows) break;
            }
        }

        auto suggestedIt = j.find("suggestedPlayers");
        if (suggestedIt != j.end() && suggestedIt->is_array()) {
            for (const auto& value : *suggestedIt) {
                if (!value.is_string()) continue;
                PushUniqueCaseInsensitive(state.suggestedPlayers, value.get<std::string>(), kMcsrUsernameIndexMaxNames);
            }
        }

        auto matchesIt = j.find("recentMatches");
        if (matchesIt != j.end() && matchesIt->is_array()) {
            for (const auto& value : *matchesIt) {
                if (!value.is_object()) continue;
                McsrApiTrackerRuntimeState::MatchRow row;
                if (value.contains("opponent") && value["opponent"].is_string()) row.opponent = value["opponent"].get<std::string>();
                if (value.contains("resultLabel") && value["resultLabel"].is_string()) {
                    row.resultLabel = value["resultLabel"].get<std::string>();
                }
                if (value.contains("detailLabel") && value["detailLabel"].is_string()) {
                    row.detailLabel = value["detailLabel"].get<std::string>();
                }
                if (value.contains("ageLabel") && value["ageLabel"].is_string()) row.ageLabel = value["ageLabel"].get<std::string>();
                if (value.contains("resultType") && value["resultType"].is_number()) {
                    row.resultType = static_cast<int>(std::lround(value["resultType"].get<double>()));
                }
                if (value.contains("forfeited") && value["forfeited"].is_boolean()) row.forfeited = value["forfeited"].get<bool>();
                if (value.contains("categoryType") && value["categoryType"].is_number()) {
                    row.categoryType = static_cast<int>(std::lround(value["categoryType"].get<double>()));
                }
                state.recentMatches.push_back(std::move(row));
                if (state.recentMatches.size() >= kMaxCachedRows) break;
            }
        }

        auto trendIt = j.find("eloTrendPoints");
        if (trendIt != j.end() && trendIt->is_array()) {
            for (const auto& value : *trendIt) {
                if (!value.is_object()) continue;
                McsrApiTrackerRuntimeState::TrendPoint point;
                if (value.contains("elo") && value["elo"].is_number()) point.elo = static_cast<int>(std::lround(value["elo"].get<double>()));
                if (value.contains("opponent") && value["opponent"].is_string()) point.opponent = value["opponent"].get<std::string>();
                if (value.contains("resultLabel") && value["resultLabel"].is_string()) {
                    point.resultLabel = value["resultLabel"].get<std::string>();
                }
                if (value.contains("detailLabel") && value["detailLabel"].is_string()) {
                    point.detailLabel = value["detailLabel"].get<std::string>();
                }
                if (value.contains("ageLabel") && value["ageLabel"].is_string()) point.ageLabel = value["ageLabel"].get<std::string>();
                state.eloTrendPoints.push_back(std::move(point));
                if (state.eloTrendPoints.size() >= kMaxCachedRows) break;
            }
        }

        if (outSavedEpochSeconds) {
            auto savedIt = j.find("savedEpochSeconds");
            if (savedIt != j.end() && savedIt->is_number_integer()) {
                const long long epoch = savedIt->get<long long>();
                if (epoch > 0) *outSavedEpochSeconds = static_cast<std::time_t>(epoch);
            }
        }

        if (state.displayPlayer.empty() && state.requestedPlayer.empty()) return false;
        outState = std::move(state);
        return true;
    } catch (...) {
        return false;
    }
}

static bool TrySaveMcsrTrackerCacheByKey(const std::string& key, const McsrApiTrackerRuntimeState& state) {
    const std::filesystem::path cachePath = GetMcsrTrackerCachePathForKey(key);
    if (cachePath.empty()) return false;

    std::string jsonText;
    if (!TrySerializeMcsrTrackerCache(state, jsonText) || jsonText.empty()) return false;
    const std::vector<unsigned char> bytes(jsonText.begin(), jsonText.end());
    return TryWriteBinaryFile(cachePath, bytes);
}

static bool TryLoadMcsrTrackerCacheByKey(const std::string& key, McsrApiTrackerRuntimeState& outState,
                                         std::time_t* outSavedEpochSeconds = nullptr) {
    const std::filesystem::path cachePath = GetMcsrTrackerCachePathForKey(key);
    if (cachePath.empty()) return false;
    std::string jsonText;
    if (!TryReadSmallTextFile(cachePath, jsonText, 1024 * 1024)) return false;
    if (!TryDeserializeMcsrTrackerCache(jsonText, outState, outSavedEpochSeconds)) return false;
    return true;
}

static bool TryLoadMcsrTrackerCache(const std::string& requestedIdentifier, const std::string& autoDetectedUuid,
                                    McsrApiTrackerRuntimeState& outState, std::time_t* outSavedEpochSeconds = nullptr) {
    if (outSavedEpochSeconds) *outSavedEpochSeconds = 0;

    std::vector<std::string> keysToTry;
    auto pushKey = [&](std::string value) {
        TrimAsciiWhitespaceInPlace(value);
        if (value.empty()) return;
        for (const std::string& existing : keysToTry) {
            if (EqualsIgnoreCaseAscii(existing, value)) return;
        }
        keysToTry.push_back(std::move(value));
    };

    pushKey(requestedIdentifier);
    pushKey(autoDetectedUuid);
    pushKey(RemoveUuidDashes(autoDetectedUuid));
    if (IsLikelyMinecraftUuid(requestedIdentifier)) pushKey(RemoveUuidDashes(requestedIdentifier));

    for (const std::string& key : keysToTry) {
        McsrApiTrackerRuntimeState candidate;
        std::time_t candidateSavedEpoch = 0;
        if (!TryLoadMcsrTrackerCacheByKey(key, candidate, &candidateSavedEpoch)) continue;
        outState = std::move(candidate);
        if (outSavedEpochSeconds) *outSavedEpochSeconds = candidateSavedEpoch;
        return true;
    }
    return false;
}

static void SaveMcsrTrackerCache(const std::string& requestedIdentifier, const McsrApiTrackerRuntimeState& state) {
    std::vector<std::string> keys;
    auto pushKey = [&](std::string value) {
        TrimAsciiWhitespaceInPlace(value);
        if (value.empty()) return;
        for (const std::string& existing : keys) {
            if (EqualsIgnoreCaseAscii(existing, value)) return;
        }
        keys.push_back(std::move(value));
    };

    pushKey(requestedIdentifier);
    pushKey(state.displayPlayer);
    pushKey(state.requestedPlayer);
    pushKey(state.userUuid);
    pushKey(RemoveUuidDashes(state.userUuid));

    for (const std::string& key : keys) {
        (void)TrySaveMcsrTrackerCacheByKey(key, state);
    }
}

static bool TryCacheMcsrAvatar(const std::string& playerName, const std::string& uuid, std::string& outPathUtf8) {
    outPathUtf8.clear();
    const std::string uuidNoDash = SanitizeMcsrAssetKey(RemoveUuidDashes(uuid), 48);
    const std::string playerKey = SanitizeMcsrAssetKey(playerName, 32);
    std::string key = !uuidNoDash.empty() ? uuidNoDash : playerKey;
    if (key.empty()) return false;

    const std::filesystem::path avatarPath = GetMcsrAssetCacheRootPath() / L"avatars" / (L"head3d_v2_" + Utf8ToWide(key) + L".png");
    {
        std::error_code ec;
        if (std::filesystem::exists(avatarPath, ec) && !ec && std::filesystem::is_regular_file(avatarPath, ec) && !ec &&
            std::filesystem::file_size(avatarPath, ec) > 0 && !ec) {
            if (FileLooksLikeImage(avatarPath)) {
                outPathUtf8 = WideToUtf8(avatarPath.wstring());
                return true;
            }
            std::filesystem::remove(avatarPath, ec);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(s_mcsrAssetCacheMutex);
        if (s_mcsrAssetCacheState.avatarKey == key && now < s_mcsrAssetCacheState.nextAvatarFetch) {
            outPathUtf8 = s_mcsrAssetCacheState.avatarPath;
            return !outPathUtf8.empty();
        }
    }

    std::vector<unsigned char> bytes;
    DWORD statusCode = 0;
    DWORD lastError = 0;
    bool ok = false;
    auto tryFetchAvatar = [&](const wchar_t* host, const std::wstring& path) -> bool {
        bytes.clear();
        statusCode = 0;
        lastError = 0;
        return HttpGetBinary(host, INTERNET_DEFAULT_HTTPS_PORT, path.c_str(), 2200, true, bytes, &statusCode, &lastError);
    };
    if (!uuidNoDash.empty()) {
        ok = tryFetchAvatar(L"crafatar.com", L"/renders/head/" + Utf8ToWide(uuidNoDash) + L"?size=96&overlay");
    }
    if (!ok && !uuidNoDash.empty()) {
        ok = tryFetchAvatar(L"crafatar.com", L"/avatars/" + Utf8ToWide(uuidNoDash) + L"?size=96&overlay");
    }
    if (!ok && !uuidNoDash.empty()) {
        ok = tryFetchAvatar(L"visage.surgeplay.com", L"/head/96/" + Utf8ToWide(uuidNoDash));
    }
    if (!ok && !playerKey.empty()) {
        ok = tryFetchAvatar(L"crafatar.com", L"/renders/head/" + Utf8ToWide(playerKey) + L"?size=96&overlay");
    }
    if (!ok && !playerKey.empty()) {
        ok = tryFetchAvatar(L"crafatar.com", L"/avatars/" + Utf8ToWide(playerKey) + L"?size=96&overlay");
    }
    if (!ok && !playerKey.empty()) {
        ok = tryFetchAvatar(L"mc-heads.net", L"/avatar/" + Utf8ToWide(playerKey) + L"/96");
    }
    if (!ok && !playerKey.empty()) {
        ok = tryFetchAvatar(L"minotar.net", L"/helm/" + Utf8ToWide(playerKey) + L"/96.png");
    }
    if (!ok && !playerKey.empty()) {
        ok = tryFetchAvatar(L"minotar.net", L"/avatar/" + Utf8ToWide(playerKey) + L"/96.png");
    }

    if (ok && LooksLikeImageBytes(bytes) && TryWriteBinaryFile(avatarPath, bytes)) {
        outPathUtf8 = WideToUtf8(avatarPath.wstring());
        std::lock_guard<std::mutex> lock(s_mcsrAssetCacheMutex);
        s_mcsrAssetCacheState.avatarKey = key;
        s_mcsrAssetCacheState.avatarPath = outPathUtf8;
        s_mcsrAssetCacheState.nextAvatarFetch = now + std::chrono::hours(6);
        return true;
    }
    if (ok && !LooksLikeImageBytes(bytes)) {
        Log("[MCSR] Avatar fetch returned non-image content for '" + key + "'.");
    }

    {
        std::lock_guard<std::mutex> lock(s_mcsrAssetCacheMutex);
        s_mcsrAssetCacheState.avatarKey = key;
        s_mcsrAssetCacheState.avatarPath.clear();
        s_mcsrAssetCacheState.nextAvatarFetch = now + std::chrono::seconds(45);
    }
    if (!ok) {
        Log("[MCSR] Avatar fetch failed for '" + key + "' (status=" + std::to_string(statusCode) +
            ", error=" + std::to_string(lastError) + ").");
    }
    return false;
}

static bool TryCacheMcsrFlag(const std::string& countryCode, std::string& outPathUtf8) {
    outPathUtf8.clear();
    std::string key = SanitizeMcsrAssetKey(countryCode, 4);
    if (key.size() < 2) return false;
    if (key.size() > 2) key.resize(2);

    const std::filesystem::path flagPath = GetMcsrAssetCacheRootPath() / L"flags" / (L"v2_" + Utf8ToWide(key) + L".png");
    {
        std::error_code ec;
        if (std::filesystem::exists(flagPath, ec) && !ec && std::filesystem::is_regular_file(flagPath, ec) && !ec &&
            std::filesystem::file_size(flagPath, ec) > 0 && !ec) {
            if (FileLooksLikeImage(flagPath)) {
                outPathUtf8 = WideToUtf8(flagPath.wstring());
                return true;
            }
            std::filesystem::remove(flagPath, ec);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(s_mcsrAssetCacheMutex);
        if (s_mcsrAssetCacheState.flagKey == key && now < s_mcsrAssetCacheState.nextFlagFetch) {
            outPathUtf8 = s_mcsrAssetCacheState.flagPath;
            return !outPathUtf8.empty();
        }
    }

    std::vector<unsigned char> bytes;
    DWORD statusCode = 0;
    DWORD lastError = 0;
    bool ok = false;
    auto tryFetchFlag = [&](const wchar_t* host, const std::wstring& path) -> bool {
        bytes.clear();
        statusCode = 0;
        lastError = 0;
        return HttpGetBinary(host, INTERNET_DEFAULT_HTTPS_PORT, path.c_str(), 2200, true, bytes, &statusCode, &lastError);
    };

    ok = tryFetchFlag(L"flagcdn.com", L"/w40/" + Utf8ToWide(key) + L".png");
    if (!ok) {
        std::string keyUpper = key;
        std::transform(keyUpper.begin(), keyUpper.end(), keyUpper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        ok = tryFetchFlag(L"flagsapi.com", L"/" + Utf8ToWide(keyUpper) + L"/flat/32.png");
    }
    if (!ok) {
        ok = tryFetchFlag(L"flagcdn.com", L"/" + Utf8ToWide(key) + L".png");
    }
    if (!ok && key.size() == 2) {
        const unsigned int cp0 = 0x1F1E6u + static_cast<unsigned int>(std::toupper(static_cast<unsigned char>(key[0])) - 'A');
        const unsigned int cp1 = 0x1F1E6u + static_cast<unsigned int>(std::toupper(static_cast<unsigned char>(key[1])) - 'A');
        std::ostringstream emojiPath;
        emojiPath << "/ajax/libs/twemoji/14.0.2/72x72/" << std::hex << std::nouppercase << cp0 << "-" << cp1 << ".png";
        ok = tryFetchFlag(L"cdnjs.cloudflare.com", Utf8ToWide(emojiPath.str()));
    }

    if (ok && LooksLikeImageBytes(bytes) && TryWriteBinaryFile(flagPath, bytes)) {
        outPathUtf8 = WideToUtf8(flagPath.wstring());
        std::lock_guard<std::mutex> lock(s_mcsrAssetCacheMutex);
        s_mcsrAssetCacheState.flagKey = key;
        s_mcsrAssetCacheState.flagPath = outPathUtf8;
        s_mcsrAssetCacheState.nextFlagFetch = now + std::chrono::hours(24);
        return true;
    }
    if (ok && !LooksLikeImageBytes(bytes)) { Log("[MCSR] Flag fetch returned non-image content for '" + key + "'."); }

    {
        std::lock_guard<std::mutex> lock(s_mcsrAssetCacheMutex);
        s_mcsrAssetCacheState.flagKey = key;
        s_mcsrAssetCacheState.flagPath.clear();
        s_mcsrAssetCacheState.nextFlagFetch = now + std::chrono::minutes(2);
    }
    if (!ok) {
        Log("[MCSR] Flag fetch failed for '" + key + "' (status=" + std::to_string(statusCode) + ", error=" +
            std::to_string(lastError) + ").");
    }
    return false;
}

static void UpdateMcsrApiTrackerState(const McsrTrackerOverlayConfig& trackerCfg) {
    const bool trackerEnabled = trackerCfg.enabled;
    const bool refreshOnlyMode = trackerCfg.refreshOnlyMode;
    const int pollIntervalMs = std::clamp(trackerCfg.pollIntervalMs, 10000, 3600000);
    const auto now = std::chrono::steady_clock::now();
    const bool forceRefresh = s_mcsrApiTrackerForceRefresh.exchange(false, std::memory_order_relaxed);

    bool runtimeVisible = false;
    bool runtimeInitializedVisibility = false;
    McsrApiTrackerRuntimeState previousState;
    {
        std::lock_guard<std::mutex> lock(s_mcsrApiTrackerMutex);
        if (!s_mcsrApiTrackerState.initializedVisibility) {
            s_mcsrApiTrackerState.visible = false;
            s_mcsrApiTrackerState.initializedVisibility = true;
        }
        previousState = s_mcsrApiTrackerState;
        s_mcsrApiTrackerState.enabled = trackerEnabled;
        runtimeVisible = s_mcsrApiTrackerState.visible;
        runtimeInitializedVisibility = s_mcsrApiTrackerState.initializedVisibility;
    }

    if (!trackerEnabled) {
        ResetMcsrApiRateLimitBackoff();
        std::lock_guard<std::mutex> lock(s_mcsrApiTrackerMutex);
        s_mcsrApiTrackerState = McsrApiTrackerRuntimeState{};
        s_mcsrApiTrackerState.enabled = false;
        s_mcsrApiTrackerState.visible = false;
        s_mcsrApiTrackerState.initializedVisibility = true;
        s_mcsrApiTrackerState.statusLabel = "MCSR tracker disabled.";
        return;
    }

    std::string manualPlayer = trackerCfg.player;
    TrimAsciiWhitespaceInPlace(manualPlayer);
    {
        std::lock_guard<std::mutex> lock(s_mcsrSearchOverrideMutex);
        if (!s_mcsrSearchOverridePlayer.empty()) { manualPlayer = s_mcsrSearchOverridePlayer; }
    }
    std::string autoDetectedPlayer;
    std::string autoDetectedUuid;
    if (trackerCfg.autoDetectPlayer) { (void)TryResolveMcsrAutoDetectedIdentity(autoDetectedPlayer, autoDetectedUuid); }
    std::string requestedIdentifier = manualPlayer;
    if (requestedIdentifier.empty()) {
        requestedIdentifier = !autoDetectedPlayer.empty() ? autoDetectedPlayer : autoDetectedUuid;
    }

    std::wstring mcsrExtraHeadersW;
    if (trackerCfg.useApiKey) {
        std::string headerName = SanitizeHttpHeaderToken(trackerCfg.apiKeyHeader);
        std::string headerValue = trackerCfg.apiKey;
        TrimAsciiWhitespaceInPlace(headerValue);
        if (headerName.empty()) headerName = "x-api-key";
        if (!headerValue.empty()) {
            mcsrExtraHeadersW = Utf8ToWide(headerName + ": " + headerValue + "\r\n");
        }
    }

    MaybeRefreshMcsrUsernameIndex(mcsrExtraHeadersW, forceRefresh);

    if (requestedIdentifier.empty()) {
        std::lock_guard<std::mutex> lock(s_mcsrApiTrackerMutex);
        s_mcsrApiTrackerState.enabled = true;
        s_mcsrApiTrackerState.visible = runtimeVisible;
        s_mcsrApiTrackerState.initializedVisibility = runtimeInitializedVisibility;
        s_mcsrApiTrackerState.apiOnline = false;
        s_mcsrApiTrackerState.autoDetectedPlayer = autoDetectedPlayer;
        s_mcsrApiTrackerState.autoDetectedUuid = autoDetectedUuid;
        s_mcsrApiTrackerState.requestedPlayer.clear();
        s_mcsrApiTrackerState.displayPlayer.clear();
        if (s_mcsrApiTrackerState.suggestedPlayers.empty()) {
            MergeMcsrGlobalSuggestions(s_mcsrApiTrackerState.suggestedPlayers, kMcsrUsernameIndexMaxNames);
        }
        s_mcsrApiTrackerState.statusLabel =
            trackerCfg.autoDetectPlayer ? "No Minecraft identity detected. Enter player in Ctrl+I -> MCSR."
                                        : "Set player in Ctrl+I -> MCSR.";
        return;
    }

    if (now < s_mcsrApiRateLimitUntil) {
        const int waitSeconds =
            std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(s_mcsrApiRateLimitUntil - now).count()));
        std::lock_guard<std::mutex> lock(s_mcsrApiTrackerMutex);
        s_mcsrApiTrackerState.enabled = true;
        s_mcsrApiTrackerState.visible = runtimeVisible;
        s_mcsrApiTrackerState.initializedVisibility = runtimeInitializedVisibility;
        s_mcsrApiTrackerState.autoDetectedPlayer = autoDetectedPlayer;
        s_mcsrApiTrackerState.autoDetectedUuid = autoDetectedUuid;
        s_mcsrApiTrackerState.requestedPlayer = requestedIdentifier;
        if (s_mcsrApiTrackerState.suggestedPlayers.empty()) {
            MergeMcsrGlobalSuggestions(s_mcsrApiTrackerState.suggestedPlayers, kMcsrUsernameIndexMaxNames);
        }
        s_mcsrApiTrackerState.statusLabel = "MCSR API rate-limited (429). Retry in " + std::to_string(waitSeconds) + "s.";
        return;
    }

    bool shouldPollNow = false;
    {
        std::lock_guard<std::mutex> lock(s_mcsrApiTrackerMutex);
        shouldPollNow = forceRefresh || (s_mcsrApiTrackerState.requestedPlayer != requestedIdentifier) ||
                        (s_mcsrApiTrackerState.autoDetectedPlayer != autoDetectedPlayer) ||
                        (s_mcsrApiTrackerState.autoDetectedUuid != autoDetectedUuid);
        if (!shouldPollNow && !refreshOnlyMode) { shouldPollNow = (now >= s_nextMcsrApiTrackerPollTime); }
        s_mcsrApiTrackerState.enabled = true;
        s_mcsrApiTrackerState.visible = runtimeVisible;
        s_mcsrApiTrackerState.initializedVisibility = runtimeInitializedVisibility;
        s_mcsrApiTrackerState.autoDetectedPlayer = autoDetectedPlayer;
        s_mcsrApiTrackerState.autoDetectedUuid = autoDetectedUuid;
        s_mcsrApiTrackerState.requestedPlayer = requestedIdentifier;
    }
    if (!shouldPollNow) {
        std::lock_guard<std::mutex> lock(s_mcsrApiTrackerMutex);
        if (s_mcsrApiTrackerState.suggestedPlayers.empty()) {
            MergeMcsrGlobalSuggestions(s_mcsrApiTrackerState.suggestedPlayers, kMcsrUsernameIndexMaxNames);
        }
        return;
    }

    McsrApiTrackerRuntimeState cachedState;
    std::time_t cachedSavedEpochSeconds = 0;
    const bool hasCachedState = TryLoadMcsrTrackerCache(requestedIdentifier, autoDetectedUuid, cachedState, &cachedSavedEpochSeconds);

    s_nextMcsrApiTrackerPollTime = now + std::chrono::milliseconds(pollIntervalMs);
    bool rateLimitedThisCycle = false;

    McsrApiTrackerRuntimeState next = previousState;
    if (hasCachedState) {
        next = cachedState;
        ApplyMcsrTrackerRuntimeEnvelope(next, true, runtimeVisible, runtimeInitializedVisibility, autoDetectedPlayer, autoDetectedUuid,
                                        requestedIdentifier);
    } else {
        ApplyMcsrTrackerRuntimeEnvelope(next, true, runtimeVisible, runtimeInitializedVisibility, autoDetectedPlayer, autoDetectedUuid,
                                        requestedIdentifier);
    }
    MergeMcsrGlobalSuggestions(next.suggestedPlayers, kMcsrUsernameIndexMaxNames);
    if (next.displayPlayer.empty()) next.displayPlayer = requestedIdentifier;

    if (refreshOnlyMode && !forceRefresh && hasCachedState) {
        std::string ageLabel = "cached";
        const std::time_t nowEpoch = std::time(nullptr);
        if (cachedSavedEpochSeconds > 0 && nowEpoch > cachedSavedEpochSeconds) {
            const int ageMinutes = static_cast<int>((nowEpoch - cachedSavedEpochSeconds) / 60);
            if (ageMinutes < 1) {
                ageLabel = "just now";
            } else if (ageMinutes < 60) {
                ageLabel = std::to_string(ageMinutes) + "m ago";
            } else {
                ageLabel = std::to_string(ageMinutes / 60) + "h ago";
            }
        }
        next.apiOnline = true;
        next.statusLabel = "Cached data (" + ageLabel + "). Press Refresh for latest.";
        std::lock_guard<std::mutex> lock(s_mcsrApiTrackerMutex);
        s_mcsrApiTrackerState = std::move(next);
        return;
    }

    auto requestUserByIdentifier = [&](const std::string& identifier, std::string& outJson, DWORD& outStatus, DWORD& outErr) -> bool {
        const std::string encodedIdentifier = UrlEncodePathSegment(identifier);
        const std::wstring userPath = L"/api/users/" + Utf8ToWide(encodedIdentifier);
        return HttpGetMcsrJson(userPath, outJson, &outStatus, &outErr, mcsrExtraHeadersW);
    };

    std::string effectiveIdentifier = requestedIdentifier;
    std::string userJson;
    DWORD userStatusCode = 0;
    DWORD userLastError = 0;
    bool userFetchOk = requestUserByIdentifier(effectiveIdentifier, userJson, userStatusCode, userLastError);
    if (!userFetchOk) {
        // If auto-resolved username misses but we also have UUID, retry UUID once.
        const bool isNotFoundLike = (userStatusCode == 400 || userStatusCode == 404);
        if (manualPlayer.empty() && isNotFoundLike && !autoDetectedUuid.empty() &&
            !EqualsIgnoreCaseAscii(effectiveIdentifier, autoDetectedUuid)) {
            effectiveIdentifier = autoDetectedUuid;
            userJson.clear();
            userStatusCode = 0;
            userLastError = 0;
            userFetchOk = requestUserByIdentifier(effectiveIdentifier, userJson, userStatusCode, userLastError);
        }
    }
    if (!userFetchOk) {
        std::string failureLabel = "MCSR API offline.";
        if (userStatusCode == 400 || userStatusCode == 404) {
            failureLabel = "Player not found: " + effectiveIdentifier;
        } else if (userStatusCode == 429) {
            const int waitSeconds = RegisterMcsrApiRateLimitBackoff(pollIntervalMs);
            failureLabel = "MCSR API rate-limited (429). Retry in " + std::to_string(waitSeconds) + "s.";
            rateLimitedThisCycle = true;
        } else if (userStatusCode >= 500 && userStatusCode <= 599) {
            failureLabel = "MCSR API server error (" + std::to_string(userStatusCode) + ").";
        } else if (userStatusCode >= 400 && userStatusCode <= 499) {
            failureLabel = "MCSR API request rejected (" + std::to_string(userStatusCode) + ").";
        } else if (userLastError != 0) {
            failureLabel = "MCSR API network error (" + std::to_string(userLastError) + ").";
        }

        if (hasCachedState) {
            next.apiOnline = true;
            next.statusLabel = "Cached data active. " + failureLabel;
        } else {
            next.apiOnline = false;
            next.statusLabel = std::move(failureLabel);
        }
        std::lock_guard<std::mutex> lock(s_mcsrApiTrackerMutex);
        s_mcsrApiTrackerState = std::move(next);
        return;
    }

    ParsedMcsrUserData userData = ParseMcsrUserPayload(userJson);
    if (!userData.ok) {
        if (hasCachedState) {
            next.apiOnline = true;
            next.statusLabel = "Cached data active. Player profile parse failed.";
        } else {
            next.apiOnline = false;
            next.statusLabel = "Player not found.";
        }
        std::lock_guard<std::mutex> lock(s_mcsrApiTrackerMutex);
        s_mcsrApiTrackerState = std::move(next);
        return;
    }

    next.apiOnline = true;
    next.recentWins = 0;
    next.recentLosses = 0;
    next.recentDraws = 0;
    next.averageResultTimeMs = 0;
    next.profileAverageTimeMs = 0;
    next.recentForfeitRatePercent = 0.0f;
    next.profileForfeitRatePercent = 0.0f;
    next.lastMatchId.clear();
    next.lastResultLabel.clear();
    next.lastResultTimeMs = 0;
    next.recentMatches.clear();
    next.eloHistory.clear();
    next.eloTrendPoints.clear();
    next.splitLines.clear();
    next.userUuid = userData.uuid;
    if (!userData.nickname.empty()) next.displayPlayer = userData.nickname;
    next.country = userData.country;
    next.eloRank = userData.eloRank;
    next.eloRate = userData.eloRate;
    next.peakElo = (userData.peakElo > 0) ? userData.peakElo : userData.eloRate;
    next.seasonWins = userData.seasonWinsRanked;
    next.seasonLosses = userData.seasonLossesRanked;
    next.seasonCompletions = userData.seasonCompletionsRanked;
    next.seasonPoints = userData.seasonPointsRanked;
    next.bestWinStreak = userData.bestWinStreak;
    next.bestTimeMs = userData.bestTimeMs;
    next.profileAverageTimeMs = std::max(0, userData.averageTimeMs);
    if (userData.hasForfeitRatePercent) {
        next.profileForfeitRatePercent = std::clamp(userData.forfeitRatePercent, 0.0f, 100.0f);
    }
    next.seasonFfs = userData.seasonFfsRanked;
    next.seasonDodges = userData.seasonDodgesRanked;
    next.seasonCurrentWinStreak = userData.seasonCurrentWinStreakRanked;
    next.avatarImagePath.clear();
    next.flagImagePath.clear();
    {
        const std::string avatarName = !next.displayPlayer.empty() ? next.displayPlayer : requestedIdentifier;
        (void)TryCacheMcsrAvatar(avatarName, next.userUuid, next.avatarImagePath);
        (void)TryCacheMcsrFlag(next.country, next.flagImagePath);
    }

    const std::string encodedPlayer = UrlEncodePathSegment(effectiveIdentifier);
    const std::wstring matchesPath = L"/api/users/" + Utf8ToWide(encodedPlayer) + L"/matches?page=0";
    std::string matchesJson;
    ParsedMcsrMatchesData matchesData;
    DWORD matchesStatusCode = 0;
    DWORD matchesLastError = 0;
    if (HttpGetMcsrJson(matchesPath, matchesJson, &matchesStatusCode, &matchesLastError, mcsrExtraHeadersW)) {
        matchesData = ParseMcsrMatchesPayload(matchesJson, userData.uuid, userData.nickname);
    } else if (matchesStatusCode == 429) {
        const int waitSeconds = RegisterMcsrApiRateLimitBackoff(pollIntervalMs);
        next.statusLabel = "MCSR API rate-limited (429). Retry in " + std::to_string(waitSeconds) + "s.";
        rateLimitedThisCycle = true;
    }

    std::vector<ParsedMcsrMatchSummary> rankedMatches;
    if (matchesData.ok) {
        for (const ParsedMcsrMatchSummary& match : matchesData.matches) {
            if (IsMcsrRankedMatch(match)) rankedMatches.push_back(match);
        }
    }

    PushUniqueCaseInsensitive(next.suggestedPlayers, next.displayPlayer, kMcsrUsernameIndexMaxNames);
    PushUniqueCaseInsensitive(next.suggestedPlayers, requestedIdentifier, kMcsrUsernameIndexMaxNames);
    PushUniqueCaseInsensitive(next.suggestedPlayers, autoDetectedPlayer, kMcsrUsernameIndexMaxNames);
    MergeMcsrGlobalSuggestions(next.suggestedPlayers, kMcsrUsernameIndexMaxNames);

    const size_t recentLimit = std::min<size_t>(30, rankedMatches.size());
    int recentForfeitCount = 0;
    long long recentTimeTotalMs = 0;
    int recentTimeCount = 0;
    for (size_t i = 0; i < recentLimit; ++i) {
        const ParsedMcsrMatchSummary& match = rankedMatches[i];
        const int outcome = ClassifyMcsrMatchOutcome(match, userData);
        if (outcome > 0) {
            next.recentWins += 1;
        } else if (outcome < 0) {
            next.recentLosses += 1;
        } else {
            next.recentDraws += 1;
        }
        if (match.forfeited) recentForfeitCount += 1;
        // result.time in this endpoint is the winner time; only include own completed wins for user avg.
        if (match.resultTimeMs > 0 && outcome > 0 && !match.forfeited) {
            recentTimeTotalMs += static_cast<long long>(match.resultTimeMs);
            recentTimeCount += 1;
        }

        PushUniqueCaseInsensitive(next.suggestedPlayers, match.opponentName, kMcsrUsernameIndexMaxNames);
        PushUniqueCaseInsensitive(next.suggestedPlayers, match.resultName, kMcsrUsernameIndexMaxNames);
    }

    if (matchesData.ok) {
        const size_t panelLimit = std::min<size_t>(42, matchesData.matches.size());
        for (size_t i = 0; i < panelLimit; ++i) {
            const ParsedMcsrMatchSummary& match = matchesData.matches[i];
            const int outcome = ClassifyMcsrMatchOutcome(match, userData);
            McsrApiTrackerRuntimeState::MatchRow row;
            row.opponent = match.opponentName.empty() ? "Unknown" : match.opponentName;
            if (outcome > 0) {
                row.resultType = 1;
                row.resultLabel = "WON";
            } else if (outcome < 0) {
                row.resultType = -1;
                row.resultLabel = "LOST";
            } else {
                row.resultType = 0;
                row.resultLabel = "DRAW";
            }
            const bool hasTime = (match.resultTimeMs > 0);
            const bool preferTime = hasTime && (outcome > 0 || !match.forfeited);
            row.forfeited = match.forfeited && !preferTime;
            row.detailLabel = preferTime ? FormatDurationMs(match.resultTimeMs) : (row.forfeited ? "FORFEIT" : FormatDurationMs(match.resultTimeMs));
            row.ageLabel = FormatAgeShortFromEpoch(match.dateEpochSeconds);
            row.categoryType = static_cast<int>(ClassifyMcsrMatchCategory(match));
            next.recentMatches.push_back(std::move(row));

            PushUniqueCaseInsensitive(next.suggestedPlayers, match.opponentName, kMcsrUsernameIndexMaxNames);
            PushUniqueCaseInsensitive(next.suggestedPlayers, match.resultName, kMcsrUsernameIndexMaxNames);
        }
    }
    if (recentLimit > 0) {
        next.recentForfeitRatePercent = (100.0f * static_cast<float>(recentForfeitCount)) / static_cast<float>(recentLimit);
    }
    if (recentTimeCount > 0) {
        next.averageResultTimeMs = static_cast<int>(recentTimeTotalMs / static_cast<long long>(recentTimeCount));
    }
    if (!userData.hasForfeitRatePercent && next.profileForfeitRatePercent <= 0.0f && next.recentForfeitRatePercent > 0.0f) {
        next.profileForfeitRatePercent = next.recentForfeitRatePercent;
    }

    if (!rankedMatches.empty()) {
        const ParsedMcsrMatchSummary& latest = rankedMatches.front();
        const int outcome = ClassifyMcsrMatchOutcome(latest, userData);
        next.lastMatchId = latest.id;
        next.lastResultLabel = (outcome > 0) ? "WON" : ((outcome < 0) ? "LOST" : "DRAW");
        next.lastResultTimeMs = latest.resultTimeMs;
    }

    {
        const size_t trendLimit = std::min<size_t>(30, rankedMatches.size());
        std::vector<int> newestToOldest;
        std::vector<McsrApiTrackerRuntimeState::TrendPoint> newestToOldestTrend;
        newestToOldest.reserve(trendLimit);
        newestToOldestTrend.reserve(trendLimit);

        int rollingElo = std::max(0, next.eloRate);
        for (size_t i = 0; i < trendLimit; ++i) {
            const ParsedMcsrMatchSummary& match = rankedMatches[i];
            int eloAfter = rollingElo;
            if (match.hasEloAfter) {
                eloAfter = match.eloAfter;
            } else if (rollingElo <= 0 && match.eloDelta != 0) {
                eloAfter = std::max(0, rollingElo + match.eloDelta);
            }

            const int eloPoint = std::max(0, eloAfter);
            newestToOldest.push_back(eloPoint);
            rollingElo = std::max(0, eloAfter - match.eloDelta);

            const int outcome = ClassifyMcsrMatchOutcome(match, userData);
            McsrApiTrackerRuntimeState::TrendPoint trendPoint;
            trendPoint.elo = eloPoint;
            trendPoint.opponent = match.opponentName.empty() ? "Unknown" : match.opponentName;
            trendPoint.resultLabel = (outcome > 0) ? "WON" : ((outcome < 0) ? "LOST" : "DRAW");
            const bool hasTime = (match.resultTimeMs > 0);
            const bool preferTime = hasTime && (outcome > 0 || !match.forfeited);
            trendPoint.detailLabel =
                preferTime ? FormatDurationMs(match.resultTimeMs) : (match.forfeited ? "FORFEIT" : FormatDurationMs(match.resultTimeMs));
            trendPoint.ageLabel = FormatAgeShortFromEpoch(match.dateEpochSeconds);
            newestToOldestTrend.push_back(std::move(trendPoint));
        }

        for (size_t i = newestToOldest.size(); i > 0; --i) {
            next.eloHistory.push_back(newestToOldest[i - 1]);
            next.eloTrendPoints.push_back(newestToOldestTrend[i - 1]);
        }
        if (next.eloHistory.empty()) {
            next.eloHistory.push_back(std::max(0, next.eloRate));
            McsrApiTrackerRuntimeState::TrendPoint trendPoint;
            trendPoint.elo = std::max(0, next.eloRate);
            trendPoint.resultLabel = "CURRENT";
            trendPoint.detailLabel = "--";
            trendPoint.ageLabel = "now";
            next.eloTrendPoints.push_back(std::move(trendPoint));
        } else if (next.eloRate > 0 && next.eloHistory.back() != next.eloRate) {
            next.eloHistory.push_back(next.eloRate);
            McsrApiTrackerRuntimeState::TrendPoint trendPoint;
            trendPoint.elo = std::max(0, next.eloRate);
            trendPoint.resultLabel = "CURRENT";
            trendPoint.detailLabel = "--";
            trendPoint.ageLabel = "now";
            next.eloTrendPoints.push_back(std::move(trendPoint));
        }
    }

    if (!next.lastMatchId.empty()) {
        const std::wstring matchPath = L"/api/matches/" + Utf8ToWide(UrlEncodePathSegment(next.lastMatchId));
        std::string matchJson;
        DWORD matchStatusCode = 0;
        DWORD matchLastError = 0;
        if (HttpGetMcsrJson(matchPath, matchJson, &matchStatusCode, &matchLastError, mcsrExtraHeadersW)) {
            ParsedMcsrMatchDetailData matchDetail = ParseMcsrMatchDetailPayload(matchJson, next.userUuid);
            if (matchDetail.ok) {
                if (next.lastResultTimeMs <= 0 && matchDetail.completionTimeMs > 0) { next.lastResultTimeMs = matchDetail.completionTimeMs; }
                std::unordered_set<int> seenTypes;
                for (const ParsedMcsrTimelineSplit& split : matchDetail.splits) {
                    if (!seenTypes.insert(split.type).second) continue;
                    next.splitLines.push_back(McsrTimelineTypeLabel(split.type) + " " + FormatDurationMs(split.timeMs));
                    if (next.splitLines.size() >= 6) break;
                }
            }
        } else if (matchStatusCode == 429) {
            const int waitSeconds = RegisterMcsrApiRateLimitBackoff(pollIntervalMs);
            next.statusLabel = "MCSR API rate-limited (429). Retry in " + std::to_string(waitSeconds) + "s.";
            rateLimitedThisCycle = true;
        }
    }

    if (next.apiOnline && !rateLimitedThisCycle) { next.statusLabel.clear(); }
    if (!rateLimitedThisCycle) { ResetMcsrApiRateLimitBackoff(); }
    if (next.apiOnline) { SaveMcsrTrackerCache(requestedIdentifier, next); }

    std::lock_guard<std::mutex> lock(s_mcsrApiTrackerMutex);
    s_mcsrApiTrackerState = std::move(next);
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
    static bool s_visualEffectsRetryPending = false;
    static ULONGLONG s_visualEffectsRetryAtMs = 0;
    static bool s_visualEffectsNoStateApplied = false;
    static bool s_visualEffectsNoStateRetryPending = false;
    static ULONGLONG s_visualEffectsNoStateRetryAtMs = 0;
    static ULONGLONG s_visualEffectsNoStateFirstSeenMs = 0;
    const ULONGLONG nowMs = GetTickCount64();

    // Fallback: if State Output isn't available, we cannot observe world-enter transitions.
    // Apply once after startup and retry once, so configured values still land without manual Apply.
    if (!g_isStateOutputAvailable.load(std::memory_order_acquire)) {
        if (s_visualEffectsNoStateFirstSeenMs == 0) { s_visualEffectsNoStateFirstSeenMs = nowMs; }
        if (!s_visualEffectsNoStateApplied && nowMs - s_visualEffectsNoStateFirstSeenMs >= 12000) {
            RequestVisualEffectsApplyOnWorldEnter();
            s_visualEffectsNoStateApplied = true;
            s_visualEffectsNoStateRetryPending = true;
            s_visualEffectsNoStateRetryAtMs = nowMs + 5000;
            Log("[LogicThread] State Output unavailable; applied visual-effects fallback.");
        } else if (s_visualEffectsNoStateRetryPending && nowMs >= s_visualEffectsNoStateRetryAtMs) {
            RequestVisualEffectsApplyOnWorldEnter();
            s_visualEffectsNoStateRetryPending = false;
            Log("[LogicThread] State Output unavailable; applied visual-effects fallback retry.");
        }
    } else {
        s_visualEffectsNoStateFirstSeenMs = 0;
    }

    // Transitioning from "not in world" to "in world" - apply configured visual effects.
    if (!s_wasInWorld && isInWorld) {
        if (g_captureCursorOnWorldEnter.exchange(false, std::memory_order_acq_rel)) {
            g_showGui.store(false, std::memory_order_release);
            HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
            if (hwnd != NULL) {
                SetForegroundWindow(hwnd);
                SetActiveWindow(hwnd);
                SetFocus(hwnd);
            }
            RECT fullScreenRect;
            fullScreenRect.left = 0;
            fullScreenRect.top = 0;
            fullScreenRect.right = GetCachedScreenWidth();
            fullScreenRect.bottom = GetCachedScreenHeight();
            ClipCursor(&fullScreenRect);
            SetCursor(NULL);
            Log("[Practice] Applied cursor recapture on world-enter.");
        }

        RequestVisualEffectsApplyOnWorldEnter();
        // Some mods/settings systems can overwrite values shortly after world join.
        // Schedule one delayed re-apply to make startup behavior deterministic.
        s_visualEffectsRetryPending = true;
        s_visualEffectsRetryAtMs = nowMs + 5000;
    }

    if (isInWorld && s_visualEffectsRetryPending && nowMs >= s_visualEffectsRetryAtMs) {
        RequestVisualEffectsApplyOnWorldEnter();
        s_visualEffectsRetryPending = false;
    }

    // Transitioning from "in world" to "not in world" - reset all secondary modes
    if (s_wasInWorld && !isInWorld) {
        s_visualEffectsRetryPending = false;
        s_visualEffectsRetryAtMs = 0;
        s_visualEffectsNoStateApplied = false;
        s_visualEffectsNoStateRetryPending = false;
        s_visualEffectsNoStateRetryAtMs = 0;
        s_visualEffectsNoStateFirstSeenMs = 0;
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
    McsrTrackerOverlayConfig mcsrTrackerCfg = cfgSnap->mcsrTrackerOverlay;
    // Standalone-only release: force local clipboard pipeline and disable backend management.
    overlayCfg.standaloneClipboardMode = true;
    overlayCfg.manageNinjabrainBotProcess = false;
    overlayCfg.autoStartNinjabrainBot = false;
    overlayCfg.hideNinjabrainBotWindow = false;
    UpdateMcsrApiTrackerState(mcsrTrackerCfg);

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
        st.boatLabel = (useStandaloneSource && overlayCfg.standaloneAllowNonBoatThrows) ? "Mode: D-EYE" : "Boat: UNINIT";
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
        st.boatLabel = "Mode: D-EYE";
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
    snapshot.renderInGameOverlay = overlayCfg.renderInGameOverlay;
    snapshot.renderCompanionOverlay = overlayCfg.renderCompanionOverlay;
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

McsrApiTrackerRenderSnapshot GetMcsrApiTrackerRenderSnapshot() {
    McsrApiTrackerRenderSnapshot snapshot;

    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return snapshot;

    const McsrTrackerOverlayConfig& trackerCfg = cfgSnap->mcsrTrackerOverlay;
    snapshot.enabled = trackerCfg.enabled;
    snapshot.renderInGameOverlay = trackerCfg.renderInGameOverlay;
    snapshot.refreshOnlyMode = trackerCfg.refreshOnlyMode;
    snapshot.scale = std::clamp(trackerCfg.scale, 0.4f, 3.0f);
    snapshot.overlayOpacity = std::clamp(trackerCfg.opacity, 0.0f, 1.0f);
    snapshot.backgroundOpacity = std::clamp(trackerCfg.backgroundOpacity, 0.0f, 1.0f);
    snapshot.x = trackerCfg.x;
    snapshot.y = trackerCfg.y;
    if (!snapshot.enabled) return snapshot;

    McsrApiTrackerRuntimeState state;
    {
        std::lock_guard<std::mutex> lock(s_mcsrApiTrackerMutex);
        if (!s_mcsrApiTrackerState.initializedVisibility) {
            s_mcsrApiTrackerState.visible = false;
            s_mcsrApiTrackerState.initializedVisibility = true;
        }
        s_mcsrApiTrackerState.enabled = trackerCfg.enabled;
        state = s_mcsrApiTrackerState;
    }

    snapshot.visible = state.visible;
    if (!snapshot.visible) return snapshot;

    snapshot.apiOnline = state.apiOnline;
    snapshot.headerLabel = state.displayPlayer.empty() ? "MCSR Ranked" : state.displayPlayer;
    snapshot.statusLabel = state.statusLabel;
    snapshot.displayPlayer = state.displayPlayer;
    snapshot.requestedPlayer = state.requestedPlayer;
    snapshot.autoDetectedPlayer = !state.autoDetectedPlayer.empty() ? state.autoDetectedPlayer : state.autoDetectedUuid;
    snapshot.avatarImagePath = state.avatarImagePath;
    snapshot.flagImagePath = state.flagImagePath;
    snapshot.country = state.country;
    snapshot.eloRank = state.eloRank;
    snapshot.eloRate = state.eloRate;
    snapshot.peakElo = state.peakElo;
    snapshot.seasonWins = state.seasonWins;
    snapshot.seasonLosses = state.seasonLosses;
    snapshot.seasonCompletions = state.seasonCompletions;
    snapshot.seasonBestWinStreak = state.bestWinStreak;
    snapshot.seasonPoints = state.seasonPoints;
    snapshot.bestTimeMs = state.bestTimeMs;
    snapshot.averageResultTimeMs = state.averageResultTimeMs;
    snapshot.profileAverageTimeMs = state.profileAverageTimeMs;
    snapshot.recentWins = state.recentWins;
    snapshot.recentLosses = state.recentLosses;
    snapshot.recentDraws = state.recentDraws;
    snapshot.recentForfeitRatePercent = state.recentForfeitRatePercent;
    snapshot.profileForfeitRatePercent = state.profileForfeitRatePercent;
    snapshot.eloHistory = state.eloHistory;
    snapshot.eloTrendPoints.reserve(state.eloTrendPoints.size());
    for (const McsrApiTrackerRuntimeState::TrendPoint& row : state.eloTrendPoints) {
        McsrApiTrackerRenderSnapshot::TrendPoint outRow;
        outRow.elo = row.elo;
        outRow.opponent = row.opponent;
        outRow.resultLabel = row.resultLabel;
        outRow.detailLabel = row.detailLabel;
        outRow.ageLabel = row.ageLabel;
        snapshot.eloTrendPoints.push_back(std::move(outRow));
    }
    snapshot.suggestedPlayers = state.suggestedPlayers;
    snapshot.recentMatches.reserve(state.recentMatches.size());
    for (const McsrApiTrackerRuntimeState::MatchRow& row : state.recentMatches) {
        McsrApiTrackerRenderSnapshot::MatchRow outRow;
        outRow.opponent = row.opponent;
        outRow.resultLabel = row.resultLabel;
        outRow.detailLabel = row.detailLabel;
        outRow.ageLabel = row.ageLabel;
        outRow.resultType = row.resultType;
        outRow.forfeited = row.forfeited;
        outRow.categoryType = row.categoryType;
        snapshot.recentMatches.push_back(std::move(outRow));
    }

    return snapshot;
}

void RequestMcsrApiTrackerRefresh() { s_mcsrApiTrackerForceRefresh.store(true, std::memory_order_relaxed); }

void SetMcsrApiTrackerSearchPlayer(const std::string& playerName) {
    std::string value = playerName;
    TrimAsciiWhitespaceInPlace(value);
    if (value.size() > 64) value.resize(64);
    {
        std::lock_guard<std::mutex> lock(s_mcsrSearchOverrideMutex);
        s_mcsrSearchOverridePlayer = value;
    }
    s_mcsrApiTrackerForceRefresh.store(true, std::memory_order_relaxed);
}

void ClearMcsrApiTrackerSearchPlayer() {
    {
        std::lock_guard<std::mutex> lock(s_mcsrSearchOverrideMutex);
        s_mcsrSearchOverridePlayer.clear();
    }
    s_mcsrApiTrackerForceRefresh.store(true, std::memory_order_relaxed);
}

bool ShouldAllowMcsrTrackerUiInput() {
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return false;

    const McsrTrackerOverlayConfig& trackerCfg = cfgSnap->mcsrTrackerOverlay;
    if (!trackerCfg.enabled || !trackerCfg.renderInGameOverlay) return false;

    const std::string gameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
    if (gameState.find("inworld") != std::string::npos) return false;

    std::lock_guard<std::mutex> lock(s_mcsrApiTrackerMutex);
    if (!s_mcsrApiTrackerState.initializedVisibility) return false;
    return s_mcsrApiTrackerState.visible;
}

bool HandleMcsrTrackerOverlayToggleHotkey(unsigned int keyVk, bool ctrlDown, bool shiftDown, bool altDown) {
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return false;
    const McsrTrackerOverlayConfig& trackerCfg = cfgSnap->mcsrTrackerOverlay;
    if (!trackerCfg.enabled) return false;

    const unsigned int configuredVk = static_cast<unsigned int>(std::clamp(trackerCfg.hotkeyKey, 1, 255));
    if (keyVk != configuredVk) return false;
    if (ctrlDown != trackerCfg.hotkeyCtrl) return false;
    if (shiftDown != trackerCfg.hotkeyShift) return false;
    if (altDown != trackerCfg.hotkeyAlt) return false;

    std::lock_guard<std::mutex> lock(s_mcsrApiTrackerMutex);
    if (!s_mcsrApiTrackerState.initializedVisibility) {
        s_mcsrApiTrackerState.visible = false;
        s_mcsrApiTrackerState.initializedVisibility = true;
    }
    s_mcsrApiTrackerState.visible = !s_mcsrApiTrackerState.visible;
    s_mcsrApiTrackerState.enabled = trackerCfg.enabled;
    return true;
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
