#include "render_thread.h"
#include "fake_cursor.h"
#include "gui.h"
#include "mirror_thread.h"
#include "notes_overlay.h"
#include "obs_thread.h"
#include "profiler.h"
#include "render.h"
#include "shared_contexts.h"
#include "stb_image.h"
#include "utils.h"
#include "virtual_camera.h"
#include "window_overlay.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>

// ImGui includes for render thread
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "imgui.h"

// For GetCachedScreenHeight
#include "logic_thread.h"

static std::thread g_renderThread;
std::atomic<bool> g_renderThreadRunning{ false };
static std::atomic<bool> g_renderThreadShouldStop{ false };
std::atomic<uint64_t> g_renderFrameNumber{ 0 };

// OpenGL context for render thread
static HGLRC g_renderThreadContext = NULL;
static HDC g_renderThreadDC = NULL;
static bool g_renderContextIsShared = false; // True if using pre-shared context

struct RenderFBO {
    GLuint fbo = 0;
    GLuint texture = 0;
    GLuint stencilRbo = 0; // Stencil renderbuffer for stencil-based background masking
    int width = 0;
    int height = 0;
    std::atomic<bool> ready{ false }; // True when this FBO has valid rendered content
    uint64_t frameNumber = 0;         // Which frame this contains
    GLsync gpuFence = nullptr;        // Fence to ensure rendering is complete before reading
};

// Main overlay FBOs
static RenderFBO g_renderFBOs[RENDER_THREAD_FBO_COUNT];
static std::atomic<int> g_writeFBOIndex{ 0 }; // FBO currently being written by render thread
static std::atomic<int> g_readFBOIndex{ -1 }; // FBO ready for reading by main thread (-1 = none ready)

// OBS animated frame FBOs (WITH animation, separate from user's view)
static RenderFBO g_obsRenderFBOs[RENDER_THREAD_FBO_COUNT];
static std::atomic<int> g_obsWriteFBOIndex{ 0 };
static std::atomic<int> g_obsReadFBOIndex{ -1 };

// Last known good texture - updated only after GPU fence confirms rendering complete
// This ensures GetCompletedRenderTexture always returns a fully-rendered texture
static std::atomic<GLuint> g_lastGoodTexture{ 0 };
static std::atomic<GLuint> g_lastGoodObsTexture{ 0 };

// Fence for the last good texture - main thread can wait on this for synchronization
// This is more efficient than glFinish() as it only waits for the render thread's commands
static std::atomic<GLsync> g_lastGoodFence{ nullptr };
static std::atomic<GLsync> g_lastGoodObsFence{ nullptr };

// Ring buffer for deferred fence deletion - keeps fences alive for a few frames
// This prevents TOCTOU race where a thread reads fence, then render thread deletes it
// before the reading thread can use it. We delay deletion by 2 cycles.
static constexpr size_t FENCE_DELETION_DELAY = 2;
static GLsync g_pendingDeleteFences[FENCE_DELETION_DELAY] = { nullptr };
static GLsync g_pendingDeleteObsFences[FENCE_DELETION_DELAY] = { nullptr };
static size_t g_pendingDeleteIndex = 0;
static size_t g_pendingDeleteObsIndex = 0;

// Virtual Camera PBO for async readback (CPU fallback path)
static GLuint g_virtualCamPBO = 0;
static int g_virtualCamPBOWidth = 0;
static int g_virtualCamPBOHeight = 0;
static bool g_virtualCamPBOPending = false; // True if async read is in flight
static GLuint g_virtualCamCopyFBO = 0;      // FBO for reading from OBS texture

// Virtual Camera GPU compute shader path (double-buffered image textures + PBO readback)
static GLuint g_vcComputeProgram = 0; // Compute shader program for RGBA->NV12
static GLuint g_vcScaleFBO = 0;       // FBO for resolution downscaling
static GLuint g_vcScaleTexture = 0;   // Texture for downscaled frame
static int g_vcScaleWidth = 0;        // Current downscale texture width
static int g_vcScaleHeight = 0;       // Current downscale texture height
static bool g_vcUseCompute = false;   // True if compute shaders are supported

// Double-buffered NV12 output: dispatch writes to [g_vcWriteIdx], readback reads from [1 - g_vcWriteIdx]
static GLuint g_vcYImage[2] = { 0, 0 };      // r8ui images for Y plane (width x height)
static GLuint g_vcUVImage[2] = { 0, 0 };     // r8ui images for UV plane (width x height/2)
static GLuint g_vcReadbackPBO[2] = { 0, 0 }; // PBOs for async NV12 readback from images
static GLuint g_vcReadbackFBO = 0;           // FBO used for glReadPixels from image textures
static GLsync g_vcFence = nullptr;           // GPU fence after compute dispatch
static int g_vcWriteIdx = 0;                 // Current write buffer index
static int g_vcOutWidth = 0;                 // Current output dimensions
static int g_vcOutHeight = 0;
static bool g_vcComputePending = false;  // True if compute dispatch is in flight
static bool g_vcReadbackPending = false; // True if PBO readback is in flight

// Virtual Camera cursor staging: separate FBO/texture so cursor only appears on virtual camera, not game capture
static GLuint g_vcCursorFBO = 0;
static GLuint g_vcCursorTexture = 0;
static int g_vcCursorWidth = 0;
static int g_vcCursorHeight = 0;

// Cached uniform locations for compute shader (avoid glGetUniformLocation per frame)
static GLint g_vcLocRgbaTexture = -1;
static GLint g_vcLocWidth = -1;
static GLint g_vcLocHeight = -1;

// Double-buffered request queue: main thread writes to one slot, render thread reads from other
// This allows lock-free submission - main thread never blocks waiting for render thread
static FrameRenderRequest g_requestSlots[2];
static std::atomic<int> g_requestWriteSlot{ 0 };    // Slot main thread writes to next
static std::atomic<bool> g_requestPending{ false }; // True when a request is ready to be consumed
static std::mutex g_requestSignalMutex;             // Only for CV signaling, not data protection
static std::condition_variable g_requestCV;

// Double-buffered OBS submission (same pattern)
static ObsFrameSubmission g_obsSubmissionSlots[2];
static std::atomic<int> g_obsWriteSlot{ 0 };
static std::atomic<bool> g_obsSubmissionPending{ false };

static std::mutex g_completionMutex;
static std::condition_variable g_completionCV;
static std::atomic<bool> g_frameComplete{ false };

static std::mutex g_obsCompletionMutex;
static std::condition_variable g_obsCompletionCV;
static std::atomic<bool> g_obsFrameComplete{ false };

// Captured when stable in EyeZoom mode, used during transition-out animation
static GLuint rt_eyeZoomSnapshotTexture = 0;
static GLuint rt_eyeZoomSnapshotFBO = 0;
static int rt_eyeZoomSnapshotWidth = 0;
static int rt_eyeZoomSnapshotHeight = 0;
static bool rt_eyeZoomSnapshotValid = false;

static std::atomic<uint64_t> g_framesRendered{ 0 };
static std::atomic<uint64_t> g_framesDropped{ 0 };
static std::atomic<double> g_avgRenderTimeMs{ 0.0 };
static std::atomic<double> g_lastRenderTimeMs{ 0.0 };

extern std::atomic<HWND> g_minecraftHwnd;
extern std::atomic<bool> g_hwndChanged;

static ImGuiContext* g_renderThreadImGuiContext = nullptr;
static bool g_renderThreadImGuiInitialized = false;

struct RT_MonitorLookupContext {
    HMONITOR target = nullptr;
    int currentIndex = 0;
    int foundIndex = -1;
};

static int RT_ExtractDisplayNumber(const WCHAR* deviceName) {
    if (!deviceName) return -1;
    const WCHAR* p = deviceName;
    while (*p && (*p < L'0' || *p > L'9')) {
        ++p;
    }
    if (!*p) return -1;

    int value = 0;
    while (*p >= L'0' && *p <= L'9') {
        value = (value * 10) + static_cast<int>(*p - L'0');
        ++p;
    }
    if (value < 1 || value > 63) return -1;
    return value;
}

static BOOL CALLBACK RT_FindMonitorIndexEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM userData) {
    auto* ctx = reinterpret_cast<RT_MonitorLookupContext*>(userData);
    if (!ctx) return TRUE;
    if (ctx->foundIndex < 0 && monitor == ctx->target) { ctx->foundIndex = ctx->currentIndex; }
    ctx->currentIndex += 1;
    return TRUE;
}

static int RT_GetCurrentGameMonitorMaskBit() {
    HWND hwnd = g_minecraftHwnd.load();
    HMONITOR monitor = MonitorFromWindow(hwnd ? hwnd : GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (!monitor) return 0;

    // Prefer Windows DISPLAYn identity so GUI monitor selection matches runtime routing.
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&mi))) {
        const int displayNumber = RT_ExtractDisplayNumber(mi.szDevice);
        if (displayNumber >= 1 && displayNumber <= 63) { return displayNumber - 1; }
    }

    // Fallback: enum ordinal if device name parsing fails.
    RT_MonitorLookupContext ctx;
    ctx.target = monitor;
    EnumDisplayMonitors(nullptr, nullptr, RT_FindMonitorIndexEnumProc, reinterpret_cast<LPARAM>(&ctx));
    return (ctx.foundIndex >= 0) ? ctx.foundIndex : 0;
}

static bool RT_ShouldRenderStrongholdOverlayOnCurrentMonitor(const StrongholdOverlayRenderSnapshot& snap) {
    if (snap.renderMonitorMode != 1) return true; // All monitors
    if (snap.renderMonitorMask == 0ull) return false;

    const int monitorMaskBit = RT_GetCurrentGameMonitorMaskBit();
    if (monitorMaskBit < 0 || monitorMaskBit >= 63) return true;
    const unsigned long long bit = (1ull << monitorMaskBit);
    return (snap.renderMonitorMask & bit) != 0ull;
}

// EyeZoom dedicated font
std::atomic<bool> g_eyeZoomFontNeedsReload{ false };
static ImFont* g_eyeZoomTextFont = nullptr;
static std::string g_eyeZoomFontPathCached = "";
static float g_eyeZoomScaleFactor = 1.0f;

static bool RT_TryInitializeImGui(HWND hwnd, const Config& cfg) {
    if (g_renderThreadImGuiInitialized) { return true; }
    if (!hwnd) { return false; }

    IMGUI_CHECKVERSION();

    if (!g_renderThreadImGuiContext) {
        g_renderThreadImGuiContext = ImGui::CreateContext();
        if (!g_renderThreadImGuiContext) {
            Log("Render Thread: Failed to create ImGui context");
            return false;
        }
    }

    ImGui::SetCurrentContext(g_renderThreadImGuiContext);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Scale based on screen height
    const int screenHeight = GetCachedScreenHeight();
    float scaleFactor = 1.0f;
    if (screenHeight > 1080) { scaleFactor = static_cast<float>(screenHeight) / 1080.0f; }
    scaleFactor = roundf(scaleFactor * 4.0f) / 4.0f;
    if (scaleFactor < 1.0f) { scaleFactor = 1.0f; }
    g_eyeZoomScaleFactor = scaleFactor;

    // Load base font (fall back to default if missing)
    if (!cfg.fontPath.empty()) {
        ImFont* baseFont = io.Fonts->AddFontFromFileTTF(cfg.fontPath.c_str(), 16.0f * scaleFactor);
        if (!baseFont) {
            Log("Render Thread: Failed to load base font from " + cfg.fontPath + ", using default");
            io.Fonts->AddFontDefault();
        }
    } else {
        io.Fonts->AddFontDefault();
    }

    // Load EyeZoom text font (uses custom path if set, otherwise global font)
    std::string eyeZoomFontPath = cfg.eyezoom.textFontPath.empty() ? cfg.fontPath : cfg.eyezoom.textFontPath;
    if (!eyeZoomFontPath.empty()) {
        g_eyeZoomTextFont = io.Fonts->AddFontFromFileTTF(eyeZoomFontPath.c_str(), 80.0f * scaleFactor);
        g_eyeZoomFontPathCached = eyeZoomFontPath;
    }
    if (!g_eyeZoomTextFont) {
        Log("Render Thread: Failed to load EyeZoom font, using default");
        g_eyeZoomTextFont = io.Fonts->AddFontDefault();
    }

    ImGui::StyleColorsDark();
    LoadTheme();
    ApplyAppearanceConfig();
    ImGui::GetStyle().ScaleAllSizes(scaleFactor);

    // Initialize backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Initialize larger font for overlay text labels
    InitializeOverlayTextFont(cfg.fontPath, 16.0f, scaleFactor);

    g_renderThreadImGuiInitialized = true;
    LogCategory("init", "Render Thread: ImGui initialized successfully");
    return true;
}

static void DrawContinuousCompassArrow(ImDrawList* drawList, const ImVec2& center, float radius, float relativeYawDeg, ImU32 arrowColor,
                                       ImU32 ringColor) {
    if (!drawList || radius <= 1.0f) return;

    constexpr float kPi = 3.14159265358979323846f;
    const float angleRad = relativeYawDeg * (kPi / 180.0f);
    const ImVec2 dir(std::sin(angleRad), -std::cos(angleRad));
    const ImVec2 perp(-dir.y, dir.x);

    const float tipDist = radius * 0.90f;
    const float tailDist = radius * 0.45f;
    const float headLen = radius * 0.38f;
    const float headHalfWidth = radius * 0.24f;
    const float shaftThickness = std::max(1.5f, radius * 0.13f);

    const ImVec2 tip(center.x + dir.x * tipDist, center.y + dir.y * tipDist);
    const ImVec2 tail(center.x - dir.x * tailDist, center.y - dir.y * tailDist);
    const ImVec2 headBase(tip.x - dir.x * headLen, tip.y - dir.y * headLen);
    const ImVec2 headLeft(headBase.x + perp.x * headHalfWidth, headBase.y + perp.y * headHalfWidth);
    const ImVec2 headRight(headBase.x - perp.x * headHalfWidth, headBase.y - perp.y * headHalfWidth);

    drawList->AddCircle(center, radius, ringColor, 48, std::max(1.0f, radius * 0.06f));
    drawList->AddLine(tail, headBase, arrowColor, shaftThickness);
    drawList->AddTriangleFilled(tip, headLeft, headRight, arrowColor);
    drawList->AddCircleFilled(center, std::max(1.5f, radius * 0.10f), arrowColor);
}

static ImU32 RT_ScaleColor(ImU32 color, float factor) {
    const int r = static_cast<int>((color >> IM_COL32_R_SHIFT) & 0xFF);
    const int g = static_cast<int>((color >> IM_COL32_G_SHIFT) & 0xFF);
    const int b = static_cast<int>((color >> IM_COL32_B_SHIFT) & 0xFF);
    const int a = static_cast<int>((color >> IM_COL32_A_SHIFT) & 0xFF);
    const int sr = std::clamp(static_cast<int>(std::lround(static_cast<float>(r) * factor)), 0, 255);
    const int sg = std::clamp(static_cast<int>(std::lround(static_cast<float>(g) * factor)), 0, 255);
    const int sb = std::clamp(static_cast<int>(std::lround(static_cast<float>(b) * factor)), 0, 255);
    return IM_COL32(sr, sg, sb, a);
}

static ImU32 RT_LerpColor(ImU32 from, ImU32 to, float t) {
    const float clampedT = std::clamp(t, 0.0f, 1.0f);
    const int fr = static_cast<int>((from >> IM_COL32_R_SHIFT) & 0xFF);
    const int fg = static_cast<int>((from >> IM_COL32_G_SHIFT) & 0xFF);
    const int fb = static_cast<int>((from >> IM_COL32_B_SHIFT) & 0xFF);
    const int fa = static_cast<int>((from >> IM_COL32_A_SHIFT) & 0xFF);
    const int tr = static_cast<int>((to >> IM_COL32_R_SHIFT) & 0xFF);
    const int tg = static_cast<int>((to >> IM_COL32_G_SHIFT) & 0xFF);
    const int tb = static_cast<int>((to >> IM_COL32_B_SHIFT) & 0xFF);
    const int ta = static_cast<int>((to >> IM_COL32_A_SHIFT) & 0xFF);
    const int rr = static_cast<int>(std::lround(fr + (tr - fr) * clampedT));
    const int rg = static_cast<int>(std::lround(fg + (tg - fg) * clampedT));
    const int rb = static_cast<int>(std::lround(fb + (tb - fb) * clampedT));
    const int ra = static_cast<int>(std::lround(fa + (ta - fa) * clampedT));
    return IM_COL32(rr, rg, rb, ra);
}

static ImU32 RT_CertaintyHeatColor(float certaintyPercent, int alpha) {
    const float t = std::clamp(certaintyPercent / 100.0f, 0.0f, 1.0f);
    float r = 255.0f;
    float g = 96.0f;
    float b = 96.0f;
    if (t < 0.5f) {
        const float u = t / 0.5f;
        g = 96.0f + (159.0f * u);
        b = 96.0f;
    } else {
        const float u = (t - 0.5f) / 0.5f;
        r = 255.0f - (159.0f * u);
        g = 255.0f;
        b = 96.0f;
    }
    return IM_COL32(static_cast<int>(std::lround(r)), static_cast<int>(std::lround(g)), static_cast<int>(std::lround(b)),
                    std::clamp(alpha, 0, 255));
}

struct McsrTextureCacheEntry {
    GLuint textureId = 0;
    std::string sourcePathUtf8;
    std::filesystem::file_time_type lastWriteTime{};
    bool hasLastWriteTime = false;
    int width = 0;
    int height = 0;
    ImVec2 uvMin = ImVec2(0.0f, 0.0f);
    ImVec2 uvMax = ImVec2(1.0f, 1.0f);
};

static McsrTextureCacheEntry g_mcsrAvatarTextureCache;
static McsrTextureCacheEntry g_mcsrFlagTextureCache;

static void RT_ClearMcsrTextureCacheEntry(McsrTextureCacheEntry& entry) {
    if (entry.textureId != 0) {
        glDeleteTextures(1, &entry.textureId);
        entry.textureId = 0;
    }
    entry.sourcePathUtf8.clear();
    entry.hasLastWriteTime = false;
    entry.width = 0;
    entry.height = 0;
    entry.uvMin = ImVec2(0.0f, 0.0f);
    entry.uvMax = ImVec2(1.0f, 1.0f);
}

static bool RT_EnsureMcsrTextureFromFile(const std::string& pathUtf8, McsrTextureCacheEntry& entry) {
    if (pathUtf8.empty()) {
        RT_ClearMcsrTextureCacheEntry(entry);
        return false;
    }

    std::error_code ec;
    const std::filesystem::path filePath = std::filesystem::path(Utf8ToWide(pathUtf8));
    if (!std::filesystem::exists(filePath, ec) || ec || !std::filesystem::is_regular_file(filePath, ec) || ec) {
        RT_ClearMcsrTextureCacheEntry(entry);
        return false;
    }
    const auto writeTime = std::filesystem::last_write_time(filePath, ec);
    const bool haveWriteTime = !ec;
    const bool needsReload = (entry.textureId == 0) || (entry.sourcePathUtf8 != pathUtf8) ||
                             (haveWriteTime != entry.hasLastWriteTime) ||
                             (haveWriteTime && entry.hasLastWriteTime && writeTime != entry.lastWriteTime);
    if (!needsReload) return true;

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        RT_ClearMcsrTextureCacheEntry(entry);
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size <= 0) {
        RT_ClearMcsrTextureCacheEntry(entry);
        return false;
    }
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file.good() && !file.eof()) {
        RT_ClearMcsrTextureCacheEntry(entry);
        return false;
    }

    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char* pixels =
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        RT_ClearMcsrTextureCacheEntry(entry);
        return false;
    }

    if (entry.textureId == 0) glGenTextures(1, &entry.textureId);
    glBindTexture(GL_TEXTURE_2D, entry.textureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    int minX = w;
    int minY = h;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = pixels + (static_cast<size_t>(y) * static_cast<size_t>(w) * 4ull);
        for (int x = 0; x < w; ++x) {
            const unsigned char a = row[static_cast<size_t>(x) * 4ull + 3ull];
            if (a < 6) continue;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }
    stbi_image_free(pixels);

    entry.sourcePathUtf8 = pathUtf8;
    entry.lastWriteTime = writeTime;
    entry.hasLastWriteTime = haveWriteTime;
    entry.width = w;
    entry.height = h;
    if (maxX >= minX && maxY >= minY) {
        const float fx0 = static_cast<float>(std::max(0, minX)) / static_cast<float>(std::max(1, w));
        const float fy0 = static_cast<float>(std::max(0, minY)) / static_cast<float>(std::max(1, h));
        const float fx1 = static_cast<float>(std::min(w, maxX + 1)) / static_cast<float>(std::max(1, w));
        const float fy1 = static_cast<float>(std::min(h, maxY + 1)) / static_cast<float>(std::max(1, h));
        if ((fx1 - fx0) > 0.1f && (fy1 - fy0) > 0.1f) {
            entry.uvMin = ImVec2(fx0, fy0);
            entry.uvMax = ImVec2(fx1, fy1);
        } else {
            entry.uvMin = ImVec2(0.0f, 0.0f);
            entry.uvMax = ImVec2(1.0f, 1.0f);
        }
    } else {
        entry.uvMin = ImVec2(0.0f, 0.0f);
        entry.uvMax = ImVec2(1.0f, 1.0f);
    }
    return true;
}

static void DrawBoatIconImGui(ImDrawList* drawList, const ImVec2& center, float size, ImU32 boatColor, ImU32 strokeColor) {
    if (!drawList || size <= 2.0f) return;

    constexpr int kBoatSpriteW = 28;
    constexpr int kBoatSpriteH = 18;
    static const char* kBoatSprite[kBoatSpriteH] = {
        "................ooooo.......", "..........ooo.oo32234oo.....", ".........o423o321122334oo...",
        "........o3222211112223334ooo", "...o..oo3221111112222222334o", "..o1oo432111111234433323432o",
        "oo1133211111123443334443211o", "o11342111112444434344321111o", ".o1234422344433344432111111o",
        "..o2233444433344433211111oo.", "...o2223344444431133111oo...", "...o22222333231111231oo.....",
        "....oo22222222111123o.......", "......oo222222111oo3o.......", "........oo22111oo..oo.......",
        "..........ooooo....o3o......", "....................oo......", "....................oo......",
    };

    const ImU32 shade1 = RT_ScaleColor(boatColor, 0.62f);
    const ImU32 shade2 = RT_ScaleColor(boatColor, 0.80f);
    const ImU32 shade3 = RT_ScaleColor(boatColor, 0.98f);
    const ImU32 shade4 = RT_ScaleColor(boatColor, 1.14f);
    const ImU32 outlineColor = RT_LerpColor(RT_ScaleColor(boatColor, 0.40f), strokeColor, 0.08f);
    const float px = std::max(1.0f, size / static_cast<float>(kBoatSpriteH));
    const float spriteW = px * static_cast<float>(kBoatSpriteW);
    const float spriteH = px * static_cast<float>(kBoatSpriteH);
    const ImVec2 topLeft(center.x - spriteW * 0.5f, center.y - spriteH * 0.5f);

    for (int y = 0; y < kBoatSpriteH; ++y) {
        for (int x = 0; x < kBoatSpriteW; ++x) {
            const char p = kBoatSprite[y][x];
            if (p == '.') continue;
            ImU32 fill = shade2;
            if (p == 'o') fill = outlineColor;
            else if (p == '1') fill = shade1;
            else if (p == '2') fill = shade2;
            else if (p == '3') fill = shade3;
            else if (p == '4') fill = shade4;
            const ImVec2 minPt(topLeft.x + x * px, topLeft.y + y * px);
            const ImVec2 maxPt(minPt.x + px, minPt.y + px);
            drawList->AddRectFilled(minPt, maxPt, fill);
        }
    }
}

static void DrawEnderEyeIconImGui(ImDrawList* drawList, const ImVec2& center, float size, float certaintyPercent, ImU32 strokeColor) {
    if (!drawList || size <= 2.0f) return;

    constexpr int kEyeSpriteW = 16;
    constexpr int kEyeSpriteH = 16;
    static const char* kEyeSprite[kEyeSpriteH] = {
        "......oooo......", "....oo2222oo....", "..oo23333332oo..", "..o2233333321o..", ".o223444443322o.",
        ".o334441124333o.", "o23344111124332o", "o24444111124332o", "o24444111124332o", "o23342111144442o",
        ".o223441144233o.", ".o222342242422o.", "..o1222222321o..", "..oo22222232oo..", "....oo2222oo....",
        "......oooo......",
    };

    const int alpha = static_cast<int>((strokeColor >> IM_COL32_A_SHIFT) & 0xFF);
    const ImU32 certaintyColor = RT_CertaintyHeatColor(certaintyPercent, alpha);
    const ImU32 outlineColor = RT_LerpColor(IM_COL32(26, 34, 42, alpha), certaintyColor, 0.20f);
    const ImU32 c1 = RT_LerpColor(IM_COL32(10, 14, 20, alpha), certaintyColor, 0.20f);
    const ImU32 c2 = RT_LerpColor(IM_COL32(36, 46, 58, alpha), certaintyColor, 0.46f);
    const ImU32 c3 = RT_LerpColor(certaintyColor, IM_COL32(255, 255, 255, alpha), 0.12f);
    const ImU32 c4 = RT_LerpColor(certaintyColor, IM_COL32(255, 255, 255, alpha), 0.34f);
    const float px = std::max(1.0f, size / static_cast<float>(kEyeSpriteH));
    const float spriteW = px * static_cast<float>(kEyeSpriteW);
    const float spriteH = px * static_cast<float>(kEyeSpriteH);
    const ImVec2 topLeft(center.x - spriteW * 0.5f, center.y - spriteH * 0.5f);

    for (int y = 0; y < kEyeSpriteH; ++y) {
        for (int x = 0; x < kEyeSpriteW; ++x) {
            const char p = kEyeSprite[y][x];
            if (p == '.') continue;
            ImU32 fill = c3;
            if (p == 'o') fill = outlineColor;
            else if (p == '1') fill = c1;
            else if (p == '2') fill = c2;
            else if (p == '3') fill = c3;
            else if (p == '4') fill = c4;
            const ImVec2 minPt(topLeft.x + x * px, topLeft.y + y * px);
            const ImVec2 maxPt(minPt.x + px, minPt.y + px);
            drawList->AddRectFilled(minPt, maxPt, fill);
        }
    }
}

static void DrawDoubleEnderEyeIconImGui(ImDrawList* drawList, const ImVec2& center, float size, float certaintyPercent, ImU32 strokeColor) {
    if (!drawList || size <= 2.0f) return;
    const float certainty = std::clamp(certaintyPercent, 0.0f, 100.0f);
    const float offset = std::max(1.0f, size * 0.18f);
    const ImU32 backStroke = RT_LerpColor(strokeColor, IM_COL32(200, 214, 235, static_cast<int>((strokeColor >> IM_COL32_A_SHIFT) & 0xFF)),
                                          0.22f);
    DrawEnderEyeIconImGui(drawList, ImVec2(center.x - offset * 0.55f, center.y + offset * 0.16f), size * 0.88f, certainty * 0.94f,
                          backStroke);
    DrawEnderEyeIconImGui(drawList, ImVec2(center.x + offset * 0.48f, center.y - offset * 0.14f), size, certainty, strokeColor);
}

static void DrawStrongholdStatusIconImGui(ImDrawList* drawList, const ImVec2& center, float size, bool boatModeEnabled, int boatState,
                                          bool hasCertainty, float certaintyPercent, ImU32 boatBlueColor, ImU32 boatGreenColor,
                                          ImU32 boatRedColor, ImU32 strokeColor) {
    if (!drawList || size <= 2.0f) return;
    if (boatModeEnabled) {
        ImU32 boatColor = boatBlueColor;
        if (boatState == 1) {
            boatColor = boatGreenColor;
        } else if (boatState == 2) {
            boatColor = boatRedColor;
        }
        DrawBoatIconImGui(drawList, center, size, boatColor, strokeColor);
        return;
    }

    const float certainty = hasCertainty ? std::clamp(certaintyPercent, 0.0f, 100.0f) : 0.0f;
    DrawDoubleEnderEyeIconImGui(drawList, center, size, certainty, strokeColor);
}

static void DrawLockBadgeImGui(ImDrawList* drawList, const ImVec2& topLeft, float size, bool locked, ImU32 fillColor, ImU32 strokeColor) {
    if (!drawList || size <= 2.0f) return;

    const float bodyW = size * 0.74f;
    const float bodyH = size * 0.52f;
    const float bodyX = topLeft.x + (size - bodyW) * 0.5f;
    const float bodyY = topLeft.y + size * 0.42f;
    const float bodyRound = std::max(1.0f, size * 0.10f);
    const float shackleR = std::max(2.0f, size * 0.25f);
    const float shackleY = bodyY + size * 0.02f;
    const float strokeW = std::max(1.0f, size * 0.08f);
    const float leftX = bodyX + bodyW * 0.20f;
    const float rightX = bodyX + bodyW * 0.80f;

    const ImVec2 bodyMin(bodyX, bodyY);
    const ImVec2 bodyMax(bodyX + bodyW, bodyY + bodyH);
    drawList->AddRectFilled(bodyMin, bodyMax, fillColor, bodyRound);
    drawList->AddRect(bodyMin, bodyMax, strokeColor, bodyRound, 0, strokeW);

    constexpr float kPi = 3.14159265358979323846f;
    drawList->PathArcTo(ImVec2((leftX + rightX) * 0.5f, shackleY), shackleR, kPi, 2.0f * kPi, 18);
    drawList->PathStroke(strokeColor, false, strokeW);

    if (locked) {
        drawList->AddLine(ImVec2(leftX, shackleY), ImVec2(leftX, bodyY + strokeW), strokeColor, strokeW);
        drawList->AddLine(ImVec2(rightX, shackleY), ImVec2(rightX, bodyY + strokeW), strokeColor, strokeW);
    } else {
        drawList->AddLine(ImVec2(leftX, shackleY), ImVec2(leftX, bodyY + strokeW), strokeColor, strokeW);
        drawList->AddLine(ImVec2(rightX + size * 0.07f, shackleY + size * 0.10f), ImVec2(rightX + size * 0.10f, bodyY - size * 0.03f),
                          strokeColor, strokeW);
    }
}

static float DrawWorldBadgeImGui(ImDrawList* drawList, ImFont* font, const ImVec2& topLeft, float fontSize, char worldId, ImU32 fillColor,
                                 ImU32 textColor, ImU32 borderColor) {
    if (!drawList || !font || fontSize <= 1.0f) return 0.0f;

    const float h = std::max(10.0f, fontSize * 1.02f);
    const float w = h * 1.08f;
    const float round = std::max(1.0f, h * 0.24f);
    const ImVec2 badgeMin(topLeft.x, topLeft.y);
    const ImVec2 badgeMax(topLeft.x + w, topLeft.y + h);
    drawList->AddRectFilled(badgeMin, badgeMax, fillColor, round);
    drawList->AddRect(badgeMin, badgeMax, borderColor, round, 0, std::max(1.0f, fontSize * 0.08f));

    char label[2] = { worldId, '\0' };
    const float badgeFontSize = fontSize * 0.86f;
    const ImVec2 ts = font->CalcTextSizeA(badgeFontSize, FLT_MAX, 0.0f, label);
    const ImVec2 textPos(topLeft.x + (w - ts.x) * 0.5f, topLeft.y + (h - ts.y) * 0.5f);
    drawList->AddText(font, badgeFontSize, textPos, textColor, label);
    return w;
}

static ImU32 NegativeAwareTextColor(const std::string& text, ImU32 normalColor, ImU32 negativeColor) {
    return (!text.empty() && text[0] == '-') ? negativeColor : normalColor;
}

static std::string RT_TruncateSingleLine(std::string text, size_t maxLen) {
    if (text.size() <= maxLen) return text;
    if (maxLen <= 3) return text.substr(0, maxLen);
    text.resize(maxLen - 3);
    text += "...";
    return text;
}

static void RT_RenderStrongholdOverlayImGuiCompact(const StrongholdOverlayRenderSnapshot& snap, bool drawBehindGui) {
    if (!snap.enabled || !snap.visible) return;
    if (!ImGui::GetCurrentContext()) return;

    ImDrawList* drawList = drawBehindGui ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    if (!drawList || !font) return;

    const bool showEstimateValues = snap.showEstimateValues;
    const float uiScale = std::clamp(snap.scale, 0.4f, 3.0f);
    const float baseFontSize = ImGui::GetFontSize() * uiScale * 1.30f;
    const float headerFontSize = baseFontSize * 1.24f;
    const float rowFontSize = baseFontSize * 1.12f;
    const float metaFontSize = baseFontSize * 1.02f;
    const float lineAdvance = rowFontSize * 1.28f;
    const float padX = 15.0f * uiScale;
    const float padY = 10.0f * uiScale;
    const float sectionGap = 7.0f * uiScale;

    const int textAlpha = static_cast<int>(std::clamp(snap.overlayOpacity, 0.0f, 1.0f) * 255.0f);
    const int bgAlpha = static_cast<int>(std::clamp(snap.overlayOpacity * snap.backgroundOpacity, 0.0f, 1.0f) * 255.0f);
    const ImU32 bgColor = IM_COL32(7, 15, 24, bgAlpha);
    const ImU32 borderColor = IM_COL32(155, 225, 190, textAlpha);
    const ImU32 statusColor = snap.targetLocked ? IM_COL32(255, 235, 140, textAlpha) : IM_COL32(180, 255, 200, textAlpha);
    const ImU32 lineColor = IM_COL32(242, 248, 255, textAlpha);
    const ImU32 mutedColor = IM_COL32(196, 220, 236, textAlpha);
    const ImU32 highlightColor = IM_COL32(255, 238, 145, textAlpha);
    const ImU32 warningColor = IM_COL32(255, 150, 130, textAlpha);
    const ImU32 boatBlueColor = IM_COL32(130, 185, 255, textAlpha);
    const ImU32 boatGreenColor = IM_COL32(130, 255, 160, textAlpha);
    const ImU32 boatRedColor = IM_COL32(255, 130, 130, textAlpha);
    const ImU32 topAdjColor = IM_COL32(235, 246, 255, textAlpha);
    const ImU32 topAdjPlusColor = IM_COL32(130, 255, 160, textAlpha);
    const ImU32 topAdjMinusColor = IM_COL32(255, 130, 130, textAlpha);
    const ImU32 axisDividerColor = IM_COL32(150, 168, 180, textAlpha);

    auto formatSignedAdjustment = [](float value) {
        std::ostringstream out;
        out << std::showpos << std::fixed;
        if (std::abs(value) < 0.1f) {
            out << std::setprecision(3) << value;
        } else {
            out << std::setprecision(2) << value;
        }
        return out.str();
    };
    auto formatSignedInt = [](int value) {
        std::ostringstream out;
        out << std::showpos << value;
        return out.str();
    };
    auto axisColorFromCloseness = [&](float closeness) {
        const float t = std::clamp(closeness, 0.0f, 1.0f);
        const int r = static_cast<int>(std::lround(255.0f - 178.0f * t));
        const int g = static_cast<int>(std::lround(96.0f + 159.0f * t));
        const int b = static_cast<int>(std::lround(118.0f + 28.0f * t));
        return IM_COL32(r, g, b, textAlpha);
    };
    auto axisCloseness = [](int estimated, int target, int player) {
        const int referenceAbs = std::abs(player - target);
        const float denom = std::max(6.0f, static_cast<float>(referenceAbs));
        return std::clamp(1.0f - (static_cast<float>(std::abs(estimated - target)) / denom), 0.0f, 1.0f);
    };
    auto axisPercent = [](float closeness) { return static_cast<int>(std::lround(std::clamp(closeness, 0.0f, 1.0f) * 100.0f)); };
    auto distance2D = [](int ax, int az, int bx, int bz) {
        const double dx = static_cast<double>(ax - bx);
        const double dz = static_cast<double>(az - bz);
        return static_cast<float>(std::sqrt((dx * dx) + (dz * dz)));
    };
    auto distanceCloseness = [](float distance, float maxDistance) {
        return std::clamp(1.0f - (distance / std::max(1.0f, maxDistance)), 0.0f, 1.0f);
    };

    const float nXCloseness = axisCloseness(snap.estimatedNetherX, snap.targetNetherX, snap.playerNetherX);
    const float nZCloseness = axisCloseness(snap.estimatedNetherZ, snap.targetNetherZ, snap.playerNetherZ);
    const float oXCloseness = axisCloseness(snap.estimatedOverworldX, snap.targetOverworldX, snap.playerOverworldX);
    const float oZCloseness = axisCloseness(snap.estimatedOverworldZ, snap.targetOverworldZ, snap.playerOverworldZ);
    const int nXPct = axisPercent(nXCloseness);
    const int nZPct = axisPercent(nZCloseness);
    const int oXPct = axisPercent(oXCloseness);
    const int oZPct = axisPercent(oZCloseness);

    const int nDx = snap.estimatedNetherX - snap.targetNetherX;
    const int nDz = snap.estimatedNetherZ - snap.targetNetherZ;
    const int oDx = snap.estimatedOverworldX - snap.targetOverworldX;
    const int oDz = snap.estimatedOverworldZ - snap.targetOverworldZ;
    const float nDistToTarget = distance2D(snap.playerNetherX, snap.playerNetherZ, snap.targetNetherX, snap.targetNetherZ);
    const float nErrDistance = distance2D(snap.estimatedNetherX, snap.estimatedNetherZ, snap.targetNetherX, snap.targetNetherZ);
    const float oDistToTarget = distance2D(snap.playerOverworldX, snap.playerOverworldZ, snap.targetOverworldX, snap.targetOverworldZ);
    const float oErrDistance = distance2D(snap.estimatedOverworldX, snap.estimatedOverworldZ, snap.targetOverworldX, snap.targetOverworldZ);
    const float nDistCloseness = distanceCloseness(nDistToTarget, 260.0f);
    const float nErrCloseness = distanceCloseness(nErrDistance, std::max(28.0f, nDistToTarget));
    const float oDistCloseness = distanceCloseness(oDistToTarget, 2200.0f);
    const float oErrCloseness = distanceCloseness(oErrDistance, std::max(220.0f, oDistToTarget));

    const std::string adjustmentText = formatSignedAdjustment(snap.angleAdjustmentDeg);
    const double adjustmentStepDeg = std::max(1e-6, std::abs(static_cast<double>(snap.angleAdjustmentStepDeg)));
    const int adjustmentStepCount = static_cast<int>(std::lround(std::abs(static_cast<double>(snap.angleAdjustmentDeg)) / adjustmentStepDeg));
    const std::string adjustmentStepText =
        (adjustmentStepCount > 0)
            ? std::string((snap.angleAdjustmentDeg >= 0.0f) ? "+" : "-") + std::to_string(adjustmentStepCount)
            : std::string("0");
    const ImU32 adjustmentStepColor =
        (adjustmentStepCount == 0) ? mutedColor : ((snap.angleAdjustmentDeg >= 0.0f) ? topAdjPlusColor : topAdjMinusColor);

    const float alignmentRatio = snap.showComputedDetails ? std::clamp(1.0f - std::abs(snap.relativeYaw) / 90.0f, 0.0f, 1.0f) : 0.0f;
    const int aimPercent = static_cast<int>(std::lround(alignmentRatio * 100.0f));
    const bool hasStatusCertainty = snap.hasTopCertainty || snap.hasCombinedCertainty;
    const float statusCertaintyPercent = snap.hasTopCertainty ? snap.topCertaintyPercent
                                                              : (snap.hasCombinedCertainty ? snap.combinedCertaintyPercent : 50.0f);
    const bool showDistanceMetrics = !snap.mcsrSafeMode;
    const bool showBottomInfo = snap.showComputedDetails;

    std::string summaryLine = snap.showAlignmentText ? ("A" + std::to_string(aimPercent) + "%") : "";

    std::string guidanceLine;
    ImU32 guidanceColor = mutedColor;
    const bool shouldShowMoveGuidance = snap.hasNextThrowDirection && (!snap.hasTopCertainty || snap.topCertaintyPercent < 95.0f);
    if (shouldShowMoveGuidance) {
        std::ostringstream ss;
        ss << "L" << snap.moveLeftBlocks << " / R" << snap.moveRightBlocks << " -> 95%";
        guidanceLine = ss.str();
        guidanceColor = warningColor;
    } else if (!snap.warningLabel.empty()) {
        guidanceLine = RT_TruncateSingleLine(snap.warningLabel, 96);
        guidanceColor = warningColor;
    } else if (!snap.infoLabel.empty()) {
        guidanceLine = RT_TruncateSingleLine(snap.infoLabel, 96);
        if (showBottomInfo) {
            const size_t adjPos = guidanceLine.find(" | Adj ");
            if (adjPos != std::string::npos) {
                const size_t nextSep = guidanceLine.find(" | ", adjPos + 1);
                if (nextSep != std::string::npos) {
                    guidanceLine.erase(adjPos, nextSep - adjPos);
                } else {
                    guidanceLine.erase(adjPos);
                }
            }
        }
    }

    const bool showAltCandidate = (!snap.hasTopCertainty || snap.topCertaintyPercent < 95.0f) && !snap.topCandidate2Label.empty();
    const std::string candidate1 = RT_TruncateSingleLine(snap.topCandidate1Label, 66);
    const std::string candidate2 = RT_TruncateSingleLine(snap.topCandidate2Label, 66);
    struct CandidatePercentSpan {
        bool valid = false;
        size_t start = 0;
        size_t end = 0;
        float pct = 0.0f;
    };
    auto parsePercentSpan = [](const std::string& text) {
        CandidatePercentSpan span;
        const size_t percentPos = text.find('%');
        if (percentPos == std::string::npos || percentPos == 0) return span;
        size_t start = percentPos;
        while (start > 0) {
            const char c = text[start - 1];
            const bool isDigit = (c >= '0' && c <= '9');
            if (!isDigit && c != '.') break;
            --start;
        }
        if (start >= percentPos) return span;
        try {
            span.pct = std::stof(text.substr(start, percentPos - start));
            span.start = start;
            span.end = percentPos + 1;
            span.valid = true;
        } catch (...) {}
        return span;
    };
    auto certaintyColorFromPercent = [&](float pct) {
        const float t = std::clamp(pct / 100.0f, 0.0f, 1.0f);
        float r = 255.0f;
        float g = 96.0f;
        float b = 96.0f;
        if (t < 0.5f) {
            const float u = t / 0.5f;
            g = 96.0f + (159.0f * u);
            b = 96.0f;
        } else {
            const float u = (t - 0.5f) / 0.5f;
            r = 255.0f - (159.0f * u);
            g = 255.0f;
            b = 96.0f;
        }
        return IM_COL32(static_cast<int>(std::lround(r)), static_cast<int>(std::lround(g)), static_cast<int>(std::lround(b)), textAlpha);
    };
    const CandidatePercentSpan candidate1Pct = parsePercentSpan(candidate1);
    const CandidatePercentSpan candidate2Pct = parsePercentSpan(candidate2);
    const ImU32 candidateTopBaseColor = IM_COL32(218, 228, 236, textAlpha);
    const ImU32 candidateAltBaseColor = mutedColor;
    const float candidateTopFont = metaFontSize * 1.06f;
    const float candidateAltFont = metaFontSize;
    const ImU32 candidateTopChipFill = IM_COL32(74, 96, 126, std::max(26, textAlpha / 4));
    const ImU32 candidateTopChipBorder = IM_COL32(132, 164, 196, std::max(34, textAlpha / 3));

    const float displayW = ImGui::GetIO().DisplaySize.x;
    const bool useSideLane = !snap.showDirectionArrow;
    const float fixedPanelWidthPx = showEstimateValues ? 1180.0f : 980.0f;
    const float panelWidth = std::clamp(fixedPanelWidthPx, 360.0f, std::max(360.0f, displayW - 16.0f));

    int leftLineCount = 1;
    int sideLineCount = 0;
    if (snap.showComputedDetails) {
        leftLineCount += 3;
        leftLineCount += 1; // Summary row
        if (useSideLane) {
            if (!candidate1.empty()) sideLineCount += 1;
            if (showAltCandidate) sideLineCount += 1;
        } else {
            if (!candidate1.empty()) leftLineCount += 1;
            if (showAltCandidate) leftLineCount += 1;
        }
    } else {
        leftLineCount += 1;
        if (useSideLane) {
            if (!guidanceLine.empty()) sideLineCount += 1;
        } else {
            if (!guidanceLine.empty()) leftLineCount += 1;
        }
    }
    if (showBottomInfo) leftLineCount += 1;
    const float leftContentHeight = lineAdvance * std::max(1, leftLineCount - 1);
    const float sideContentHeight = lineAdvance * std::max(0, sideLineCount);
    const float panelHeight = (padY * 2.0f) + (headerFontSize + sectionGap) + std::max(leftContentHeight, sideContentHeight);

    const float centeredX = std::max(0.0f, (displayW - panelWidth) * 0.5f);
    const ImVec2 panelMin(centeredX, static_cast<float>(snap.y));
    const ImVec2 panelMax(panelMin.x + panelWidth, panelMin.y + panelHeight);
    const float textClipMinX = panelMin.x + padX - (2.0f * uiScale);
    const auto textWidth = [&](const std::string& s, float fontSize) {
        if (s.empty()) return 0.0f;
        return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, s.c_str()).x;
    };
    const float sideCandidate1W = textWidth(candidate1, candidateTopFont);
    const float sideCandidate2W = showAltCandidate ? textWidth(candidate2, candidateAltFont) : 0.0f;
    const float sideGuidanceW = (useSideLane && !showBottomInfo) ? textWidth(guidanceLine, metaFontSize) : 0.0f;
    const float sideNeededTextW = std::max(120.0f, std::max(sideCandidate1W, std::max(sideCandidate2W, sideGuidanceW)));
    const float dynamicSideLaneW = std::clamp(sideNeededTextW + (20.0f * uiScale), 190.0f, std::max(190.0f, panelWidth * 0.52f));
    float sideClipMaxX = panelMax.x - padX - (4.0f * uiScale);
    float sideClipMinX = panelMin.x + padX;
    float textClipMaxX = panelMax.x - padX - std::min(240.0f, panelWidth * 0.40f);
    if (useSideLane) {
        sideClipMinX = std::max(panelMin.x + padX, sideClipMaxX - dynamicSideLaneW);
        textClipMaxX = std::max(textClipMinX + 140.0f, sideClipMinX - (10.0f * uiScale));
    } else {
        const float rightCompassLaneW = std::min(240.0f, panelWidth * 0.40f);
        textClipMaxX = std::max(textClipMinX + 140.0f, panelMax.x - padX - rightCompassLaneW);
    }

    drawList->AddRectFilled(panelMin, panelMax, bgColor, 10.0f * uiScale);
    drawList->AddRect(panelMin, panelMax, borderColor, 10.0f * uiScale, 0, std::max(1.0f, 1.4f * uiScale));

    auto drawSeg = [&](float& x, float y, const std::string& text, ImU32 color, float fontSize) {
        if (text.empty()) return;
        drawList->PushClipRect(ImVec2(textClipMinX, panelMin.y), ImVec2(textClipMaxX, panelMax.y), true);
        drawList->AddText(font, fontSize, ImVec2(x, y), color, text.c_str());
        drawList->PopClipRect();
        x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str()).x;
    };
    auto drawSideLine = [&](float& sideY, const std::string& text, ImU32 color, float fontSize) {
        if (text.empty()) return;
        drawList->PushClipRect(ImVec2(sideClipMinX, panelMin.y), ImVec2(sideClipMaxX, panelMax.y), true);
        drawList->AddText(font, fontSize, ImVec2(sideClipMinX, sideY), color, text.c_str());
        drawList->PopClipRect();
        sideY += lineAdvance;
    };
    auto drawSideSeg = [&](float& xPos, float yPos, const std::string& text, ImU32 color, float fontSize) {
        if (text.empty()) return;
        drawList->PushClipRect(ImVec2(sideClipMinX, panelMin.y), ImVec2(sideClipMaxX, panelMax.y), true);
        drawList->AddText(font, fontSize, ImVec2(xPos, yPos), color, text.c_str());
        drawList->PopClipRect();
        xPos += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str()).x;
    };
    auto drawCandidateLine = [&](float startX, float yPos, const std::string& text, const CandidatePercentSpan& span, ImU32 baseColor,
                                 float fontSize, bool sideLane) {
        if (text.empty()) return;
        auto drawFn = [&](float& xp, const std::string& s, ImU32 c) {
            if (sideLane) {
                drawSideSeg(xp, yPos, s, c, fontSize);
            } else {
                drawSeg(xp, yPos, s, c, fontSize);
            }
        };
        float xp = startX;
        if (span.valid && span.end <= text.size()) {
            drawFn(xp, text.substr(0, span.start), baseColor);
            drawFn(xp, text.substr(span.start, span.end - span.start), certaintyColorFromPercent(span.pct));
            drawFn(xp, text.substr(span.end), baseColor);
        } else {
            drawFn(xp, text, baseColor);
        }
    };
    auto drawCandidateChip = [&](float startX, float yPos, const std::string& text, float fontSize, bool sideLane) {
        if (text.empty()) return;
        const float textW = textWidth(text, fontSize);
        const float chipPadX = 6.0f * uiScale;
        const float chipH = std::max(12.0f, fontSize + (5.0f * uiScale));
        const ImVec2 minPt(startX - chipPadX, yPos - (2.0f * uiScale));
        const ImVec2 maxPt(startX + textW + chipPadX, minPt.y + chipH);
        if (sideLane) {
            drawList->PushClipRect(ImVec2(sideClipMinX, panelMin.y), ImVec2(sideClipMaxX, panelMax.y), true);
        } else {
            drawList->PushClipRect(ImVec2(textClipMinX, panelMin.y), ImVec2(textClipMaxX, panelMax.y), true);
        }
        drawList->AddRectFilled(minPt, maxPt, candidateTopChipFill, 5.0f * uiScale);
        drawList->AddRect(minPt, maxPt, candidateTopChipBorder, 5.0f * uiScale, 0, std::max(1.0f, 1.0f * uiScale));
        drawList->PopClipRect();
    };

    float y = panelMin.y + padY;
    float x = panelMin.x + padX;
    const float lockIconSize = std::max(10.0f, headerFontSize * 0.92f);
    DrawLockBadgeImGui(drawList, ImVec2(x, y + (headerFontSize - lockIconSize) * 0.5f), lockIconSize, snap.targetLocked, statusColor, lineColor);
    const float topBoatIconSize = std::max(10.0f, headerFontSize * 0.90f);
    const ImVec2 topBoatCenter(panelMax.x - padX - (topBoatIconSize * 0.56f), panelMin.y + padY + (topBoatIconSize * 0.56f));
    DrawStrongholdStatusIconImGui(drawList, topBoatCenter, topBoatIconSize, snap.boatModeEnabled, snap.boatState, hasStatusCertainty,
                                  statusCertaintyPercent, boatBlueColor, boatGreenColor, boatRedColor, mutedColor);

    if (snap.showDirectionArrow) {
        const float alignment = snap.showComputedDetails ? alignmentRatio : 0.5f;
        const int arrowR = static_cast<int>(std::lround(255.0f - 125.0f * alignment));
        const int arrowG = static_cast<int>(std::lround(120.0f + 135.0f * alignment));
        const int arrowB = static_cast<int>(std::lround(110.0f + 60.0f * alignment));
        const ImU32 arrowColor = IM_COL32(arrowR, arrowG, arrowB, textAlpha);
        const ImU32 ringColor = IM_COL32(225, 240, 255, std::max(40, textAlpha / 2));
        const float desiredArrowRadiusPx = 70.0f;
        const float compassLaneW = std::min(240.0f, panelWidth * 0.40f);
        const float arrowRadius =
            std::clamp(desiredArrowRadiusPx, 24.0f, std::max(24.0f, std::min((compassLaneW * 0.50f) - 8.0f, (panelHeight * 0.48f) - padY)));
        float arrowCenterX = panelMax.x - padX - arrowRadius - (2.0f * uiScale);
        arrowCenterX = std::max(arrowCenterX, panelMin.x + (panelWidth * 0.62f));
        float arrowCenterY = panelMin.y + (panelHeight * 0.50f);
        arrowCenterY = std::clamp(arrowCenterY, panelMin.y + padY + arrowRadius, panelMax.y - padY - arrowRadius);
        const ImVec2 arrowCenter(arrowCenterX, arrowCenterY);
        DrawContinuousCompassArrow(drawList, arrowCenter, arrowRadius, snap.relativeYaw, arrowColor, ringColor);
    }

    y += headerFontSize + sectionGap;
    float sideY = y;

    auto drawCompactWorldRow = [&](char worldId, int targetX, int targetZ, int estX, int estZ, int dx, int dz, int xPct, int zPct,
                                   float xCloseness, float zCloseness, float distToTarget, float errDistance, float distCloseness,
                                   float errCloseness) {
        const ImU32 xAxisColor = axisColorFromCloseness(xCloseness);
        const ImU32 zAxisColor = axisColorFromCloseness(zCloseness);
        const ImU32 distColor = axisColorFromCloseness(distCloseness);
        const ImU32 errColor = axisColorFromCloseness(errCloseness);
        const bool emphasizeWorld = (worldId == 'N');
        const float targetFontSize = rowFontSize * (emphasizeWorld ? 1.20f : 1.06f);
        const float aimFontSize = rowFontSize * (emphasizeWorld ? 1.14f : 1.03f);
        float cx = panelMin.x + padX;
        const ImU32 worldBadgeFill = emphasizeWorld ? IM_COL32(56, 98, 136, textAlpha) : IM_COL32(52, 76, 100, textAlpha);
        const ImU32 worldBadgeText = IM_COL32(232, 244, 255, textAlpha);
        const float badgeFontSize = rowFontSize * (emphasizeWorld ? 1.02f : 0.98f);
        const float badgeY = y + std::max(0.0f, (targetFontSize - badgeFontSize) * 0.10f);
        const float badgeW =
            DrawWorldBadgeImGui(drawList, font, ImVec2(cx, badgeY), badgeFontSize, worldId, worldBadgeFill, worldBadgeText, axisDividerColor);
        cx += badgeW + (6.0f * uiScale);

        drawSeg(cx, y, "T(", highlightColor, targetFontSize);
        drawSeg(cx, y, std::to_string(targetX), highlightColor, targetFontSize);
        drawSeg(cx, y, ",", axisDividerColor, targetFontSize);
        drawSeg(cx, y, std::to_string(targetZ), highlightColor, targetFontSize);
        drawSeg(cx, y, ") ", highlightColor, targetFontSize);
        if (showDistanceMetrics) {
            drawSeg(cx, y, "@", mutedColor, rowFontSize);
            drawSeg(cx, y, std::to_string(static_cast<int>(std::lround(distToTarget))), distColor, rowFontSize);
        }
        if (showEstimateValues) {
            drawSeg(cx, y, "  E(", mutedColor, aimFontSize);
            drawSeg(cx, y, std::to_string(estX), xAxisColor, aimFontSize);
            drawSeg(cx, y, ",", axisDividerColor, aimFontSize);
            drawSeg(cx, y, std::to_string(estZ), zAxisColor, aimFontSize);
            drawSeg(cx, y, ") ", mutedColor, aimFontSize);
            drawSeg(cx, y, "D(", mutedColor, rowFontSize);
            drawSeg(cx, y, formatSignedInt(dx), xAxisColor, rowFontSize);
            drawSeg(cx, y, ",", axisDividerColor, rowFontSize);
            drawSeg(cx, y, formatSignedInt(dz), zAxisColor, rowFontSize);
            drawSeg(cx, y, ") ", mutedColor, rowFontSize);
            drawSeg(cx, y, "[", mutedColor, rowFontSize);
            drawSeg(cx, y, std::to_string(xPct), xAxisColor, rowFontSize);
            drawSeg(cx, y, "|", axisDividerColor, rowFontSize);
            drawSeg(cx, y, std::to_string(zPct), zAxisColor, rowFontSize);
            drawSeg(cx, y, "] ", mutedColor, rowFontSize);
            drawSeg(cx, y, "~", mutedColor, rowFontSize);
            drawSeg(cx, y, std::to_string(static_cast<int>(std::lround(errDistance))), errColor, rowFontSize);
        }

        const float rowScale = emphasizeWorld ? 1.12f : 1.0f;
        y += lineAdvance * rowScale;
    };

    if (snap.showComputedDetails) {
        drawCompactWorldRow('N', snap.targetNetherX, snap.targetNetherZ, snap.estimatedNetherX, snap.estimatedNetherZ, nDx, nDz, nXPct, nZPct,
                            nXCloseness, nZCloseness, nDistToTarget, nErrDistance, nDistCloseness, nErrCloseness);
        drawCompactWorldRow('O', snap.targetOverworldX, snap.targetOverworldZ, snap.estimatedOverworldX, snap.estimatedOverworldZ, oDx, oDz,
                            oXPct, oZPct, oXCloseness, oZCloseness, oDistToTarget, oErrDistance, oDistCloseness, oErrCloseness);

        float sx = panelMin.x + padX;
        drawSeg(sx, y, summaryLine, lineColor, metaFontSize);
        y += lineAdvance;

        if (useSideLane) {
            if (!candidate1.empty()) drawCandidateChip(sideClipMinX, sideY, candidate1, candidateTopFont, true);
            drawCandidateLine(sideClipMinX, sideY, candidate1, candidate1Pct, candidateTopBaseColor, candidateTopFont, true);
            sideY += lineAdvance * 1.04f;
            if (showAltCandidate) {
                drawCandidateLine(sideClipMinX, sideY, candidate2, candidate2Pct, candidateAltBaseColor, candidateAltFont, true);
                sideY += lineAdvance;
            }
        } else {
            if (!candidate1.empty()) {
                drawCandidateChip(panelMin.x + padX, y, candidate1, candidateTopFont, false);
                drawCandidateLine(panelMin.x + padX, y, candidate1, candidate1Pct, candidateTopBaseColor, candidateTopFont, false);
                y += lineAdvance * 1.04f;
            }
            if (showAltCandidate) {
                drawCandidateLine(panelMin.x + padX, y, candidate2, candidate2Pct, candidateAltBaseColor, candidateAltFont, false);
                y += lineAdvance;
            }
        }

        const std::string adjPrefix = adjustmentText + " ";
        const std::string adjStep = "[" + adjustmentStepText + "]";
        const std::string bottomSep = guidanceLine.empty() ? "" : "  |  ";
        const float adjPrefixW = textWidth(adjPrefix, metaFontSize);
        const float adjStepW = textWidth(adjStep, metaFontSize);
        const float sepW = textWidth(bottomSep, metaFontSize);
        const float guideW = textWidth(guidanceLine, metaFontSize);
        const float totalW = adjPrefixW + adjStepW + sepW + guideW;
        const float bottomY = panelMax.y - padY - lineAdvance;
        float bx = panelMin.x + std::max(padX, (panelWidth - totalW) * 0.5f);
        drawList->PushClipRect(ImVec2(panelMin.x + padX, panelMin.y), ImVec2(panelMax.x - padX, panelMax.y), true);
        drawList->AddText(font, metaFontSize, ImVec2(bx, bottomY), topAdjColor, adjPrefix.c_str());
        bx += adjPrefixW;
        drawList->AddText(font, metaFontSize, ImVec2(bx, bottomY), adjustmentStepColor, adjStep.c_str());
        bx += adjStepW;
        if (!bottomSep.empty()) {
            drawList->AddText(font, metaFontSize, ImVec2(bx, bottomY), mutedColor, bottomSep.c_str());
            bx += sepW;
            drawList->AddText(font, metaFontSize, ImVec2(bx, bottomY), guidanceColor, guidanceLine.c_str());
        }
        drawList->PopClipRect();
    } else {
        float cx = panelMin.x + padX;
        drawSeg(cx, y, "[S+H] [H]", mutedColor, metaFontSize);
        y += lineAdvance;
        if (useSideLane) {
            drawSideLine(sideY, guidanceLine, guidanceColor, metaFontSize);
        } else if (!guidanceLine.empty()) {
            cx = panelMin.x + padX;
            drawSeg(cx, y, guidanceLine, guidanceColor, metaFontSize);
            y += lineAdvance;
        }
    }
}

static void RT_RenderStrongholdOverlayImGui(const StrongholdOverlayRenderSnapshot& snap, bool drawBehindGui) {
    if (!snap.enabled || !snap.visible) return;
    if (!ImGui::GetCurrentContext()) return;

    if (snap.hudLayoutMode != 0) {
        RT_RenderStrongholdOverlayImGuiCompact(snap, drawBehindGui);
        return;
    }

    ImDrawList* drawList = drawBehindGui ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    if (!drawList || !font) return;

    float uiScale = std::clamp(snap.scale, 0.4f, 3.0f);
    float baseFontSize = ImGui::GetFontSize() * uiScale * 1.30f;
    float statusFontSize = baseFontSize * 1.24f;
    float arrowFontSize = baseFontSize * 3.15f;
    float lineFontSize = baseFontSize * 1.08f;
    float lineAdvance = lineFontSize * 1.32f;
    const bool showEstimateValues = snap.showEstimateValues;
    const bool showDistanceMetrics = !snap.mcsrSafeMode;
    float padX = 18.0f * uiScale;
    float padY = 14.0f * uiScale;
    float sectionGap = 9.0f * uiScale;

    std::vector<std::string> lines;
    std::string targetNetherXText;
    std::string targetNetherZText;
    std::string estimatedNetherXText;
    std::string estimatedNetherZText;
    std::string targetOverworldXText;
    std::string targetOverworldZText;
    std::string estimatedOverworldXText;
    std::string estimatedOverworldZText;
    std::string playerNetherXText;
    std::string playerNetherZText;
    std::string playerOverworldXText;
    std::string playerOverworldZText;
    bool hasCoordRows = false;
    int boatLineIndex = -1;
    int warningLineIndex = -1;
    std::string topAdjustmentText;
    std::string topAdjustmentStepText;
    bool showTopAdjustment = false;
    bool topAdjustmentStepActive = false;
    auto formatSignedAdjustment = [](float value) {
        std::ostringstream out;
        out << std::showpos << std::fixed;
        if (std::abs(value) < 0.1f) {
            out << std::setprecision(3) << value;
        } else {
            out << std::setprecision(2) << value;
        }
        return out.str();
    };
    if (snap.showComputedDetails) {
        std::ostringstream distStream;
        distStream << std::fixed << std::setprecision(0) << snap.distanceDisplay;
        const std::string angleAdjText = formatSignedAdjustment(snap.angleAdjustmentDeg);
        topAdjustmentText = "Adj " + angleAdjText + " deg";
        const double stepDeg = std::max(1e-6, std::abs(static_cast<double>(snap.angleAdjustmentStepDeg)));
        const int adjustmentStepCount = static_cast<int>(std::lround(std::abs(static_cast<double>(snap.angleAdjustmentDeg)) / stepDeg));
        if (adjustmentStepCount > 0) {
            const bool isPositiveAdjustment = snap.angleAdjustmentDeg > 0.0f;
            topAdjustmentStepText = std::string(isPositiveAdjustment ? "+" : "-") + std::to_string(adjustmentStepCount);
            topAdjustmentStepActive = true;
        } else {
            topAdjustmentStepText = "0";
            topAdjustmentStepActive = false;
        }
        showTopAdjustment = true;
        lines.push_back("Mode: " + snap.modeLabel + "  Feed: " + (snap.usingLiveTarget ? "LIVE" : "LOCK"));
        lines.push_back("Throws: " + std::to_string(snap.activeEyeThrowCount));
        targetNetherXText = std::to_string(snap.targetNetherX);
        targetNetherZText = std::to_string(snap.targetNetherZ);
        estimatedNetherXText = std::to_string(snap.estimatedNetherX);
        estimatedNetherZText = std::to_string(snap.estimatedNetherZ);
        targetOverworldXText = std::to_string(snap.targetOverworldX);
        targetOverworldZText = std::to_string(snap.targetOverworldZ);
        estimatedOverworldXText = std::to_string(snap.estimatedOverworldX);
        estimatedOverworldZText = std::to_string(snap.estimatedOverworldZ);
        playerNetherXText = std::to_string(snap.playerNetherX);
        playerNetherZText = std::to_string(snap.playerNetherZ);
        playerOverworldXText = std::to_string(snap.playerOverworldX);
        playerOverworldZText = std::to_string(snap.playerOverworldZ);
        hasCoordRows = true;
        if (showDistanceMetrics) {
            lines.push_back("Dist OW: " + distStream.str());
        }
        if (snap.showAlignmentText) {
            const float alignmentRatio = std::clamp(1.0f - std::abs(snap.relativeYaw) / 90.0f, 0.0f, 1.0f);
            const int alignmentPercent = static_cast<int>(std::lround(alignmentRatio * 100.0f));
            lines.push_back("Aim: " + std::to_string(alignmentPercent) + "%");
        }
        lines.push_back("Adj: " + angleAdjText + " deg");
        boatLineIndex = static_cast<int>(lines.size());
        lines.push_back(snap.boatLabel);
        if (snap.hasTopCertainty) {
            std::ostringstream certaintyStream;
            certaintyStream << std::fixed << std::setprecision(1) << snap.topCertaintyPercent;
            lines.push_back("OW %: " + certaintyStream.str());
        }
        if (snap.hasCombinedCertainty) {
            std::ostringstream combinedStream;
            combinedStream << std::fixed << std::setprecision(1) << snap.combinedCertaintyPercent;
            lines.push_back("Hit %: " + combinedStream.str());
        }
        if (snap.hasNextThrowDirection) {
            lines.push_back("Go left " + std::to_string(snap.moveLeftBlocks) + " blocks, or right " +
                            std::to_string(snap.moveRightBlocks) + " blocks, for ~95% certainty after next measurement.");
        }
        if (!snap.topCandidate1Label.empty()) lines.push_back(snap.topCandidate1Label);
        if (!snap.topCandidate2Label.empty()) lines.push_back(snap.topCandidate2Label);
        if (!snap.warningLabel.empty()) {
            warningLineIndex = static_cast<int>(lines.size());
            lines.push_back(snap.warningLabel);
        }
        if (!snap.infoLabel.empty()) lines.push_back(snap.infoLabel);
    } else {
        boatLineIndex = static_cast<int>(lines.size());
        lines.push_back(snap.boatLabel);
        if (!snap.warningLabel.empty()) {
            warningLineIndex = static_cast<int>(lines.size());
            lines.push_back(snap.warningLabel);
        }
        if (!snap.infoLabel.empty()) lines.push_back(snap.infoLabel);
    }

    ImVec2 statusSize = font->CalcTextSizeA(statusFontSize, FLT_MAX, 0.0f, snap.statusLabel.c_str());
    const bool showArrowGlyph = snap.showDirectionArrow;
    const char* idleArrowLabel = "^";
    const float arrowVisualSize = showArrowGlyph ? (arrowFontSize * 0.95f) : 0.0f;
    ImVec2 arrowSize = showArrowGlyph ? ImVec2(arrowVisualSize, arrowVisualSize) : ImVec2(0, 0);
    const float topAdjFontSize = lineFontSize * 1.12f;
    const float topAdjStepFontSize = lineFontSize * 1.72f;
    ImVec2 topAdjTextSize = showTopAdjustment ? font->CalcTextSizeA(topAdjFontSize, FLT_MAX, 0.0f, topAdjustmentText.c_str()) : ImVec2(0, 0);
    ImVec2 topAdjStepSize = (!topAdjustmentStepText.empty())
                                ? font->CalcTextSizeA(topAdjStepFontSize, FLT_MAX, 0.0f, topAdjustmentStepText.c_str())
                                : ImVec2(0, 0);
    ImVec2 topAdjStepReserveSize = showTopAdjustment ? font->CalcTextSizeA(topAdjStepFontSize, FLT_MAX, 0.0f, "+999") : ImVec2(0, 0);
    const float topAdjStepSlotWidth = std::max(topAdjStepSize.x, topAdjStepReserveSize.x);
    const float topAdjGap = 9.0f * uiScale;
    const float topAdjWidth = showTopAdjustment ? (topAdjTextSize.x + topAdjGap + topAdjStepSlotWidth) : 0.0f;
    const float topAdjHeight = showTopAdjustment ? std::max(topAdjTextSize.y, topAdjStepSize.y) : 0.0f;

    float maxLineWidth = 0.0f;
    for (const auto& line : lines) {
        ImVec2 lineSize = font->CalcTextSizeA(lineFontSize, FLT_MAX, 0.0f, line.c_str());
        if (lineSize.x > maxLineWidth) maxLineWidth = lineSize.x;
    }

    const std::string targetNetherPrefix = "Target N XZ: ";
    const std::string estimatedNetherPrefix = "Est N XZ: ";
    const std::string targetOverworldPrefix = "Target O XZ: ";
    const std::string estimatedOverworldPrefix = "Est O XZ: ";
    const std::string playerNetherPrefix = "You N XZ: ";
    const std::string playerOverworldPrefix = "You O XZ: ";
    const std::string coordSep = ", ";
    const std::string deltaXPrefix = "  dX ";
    const std::string deltaZPrefix = " dZ ";
    auto formatSignedInt = [](int value) {
        std::ostringstream out;
        out << std::showpos << value;
        return out.str();
    };
    const std::string dxNetherText = formatSignedInt(snap.estimatedNetherX - snap.targetNetherX);
    const std::string dzNetherText = formatSignedInt(snap.estimatedNetherZ - snap.targetNetherZ);
    const std::string dxOverworldText = formatSignedInt(snap.estimatedOverworldX - snap.targetOverworldX);
    const std::string dzOverworldText = formatSignedInt(snap.estimatedOverworldZ - snap.targetOverworldZ);
    if (hasCoordRows) {
        const float emphasizedCoordFontSize = lineFontSize * 1.86f;
        auto rowWidth = [&](const std::string& prefix, const std::string& xText, const std::string& zText, float fontSize) {
            return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, prefix.c_str()).x +
                   font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, xText.c_str()).x +
                   font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, coordSep.c_str()).x +
                   font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, zText.c_str()).x;
        };
        auto estimatedRowWidth = [&](const std::string& prefix, const std::string& xText, const std::string& zText, const std::string& dxText,
                                     const std::string& dzText, float fontSize) {
            return rowWidth(prefix, xText, zText, fontSize) +
                   font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, deltaXPrefix.c_str()).x +
                   font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, dxText.c_str()).x +
                   font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, deltaZPrefix.c_str()).x +
                   font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, dzText.c_str()).x;
        };
        float widthTargetN = rowWidth(targetNetherPrefix, targetNetherXText, targetNetherZText, emphasizedCoordFontSize);
        float widthEstN =
            showEstimateValues ? estimatedRowWidth(estimatedNetherPrefix, estimatedNetherXText, estimatedNetherZText, dxNetherText, dzNetherText,
                                                   lineFontSize)
                               : 0.0f;
        float widthTargetO = rowWidth(targetOverworldPrefix, targetOverworldXText, targetOverworldZText, lineFontSize);
        float widthEstO = showEstimateValues
                              ? estimatedRowWidth(estimatedOverworldPrefix, estimatedOverworldXText, estimatedOverworldZText, dxOverworldText,
                                                  dzOverworldText, lineFontSize)
                              : 0.0f;
        float widthPlayerN = rowWidth(playerNetherPrefix, playerNetherXText, playerNetherZText, lineFontSize);
        float widthPlayerO = rowWidth(playerOverworldPrefix, playerOverworldXText, playerOverworldZText, lineFontSize);
        float coordMaxWidth = std::max(std::max(widthTargetN, widthTargetO), std::max(widthPlayerN, widthPlayerO));
        if (showEstimateValues) { coordMaxWidth = std::max(coordMaxWidth, std::max(widthEstN, widthEstO)); }
        maxLineWidth = std::max(maxLineWidth, coordMaxWidth);
    }

    float contentWidth = std::max(statusSize.x, std::max(arrowSize.x, std::max(maxLineWidth, topAdjWidth)));
    float panelWidth = std::max(280.0f * uiScale, contentWidth + padX * 2.0f);
    float linesHeight = lines.empty() ? 0.0f : (lineAdvance * static_cast<float>(lines.size()));
    if (hasCoordRows) linesHeight += lineAdvance * (showEstimateValues ? 12.2f : 4.4f);
    float panelHeight = padY + statusSize.y + sectionGap;
    if (showTopAdjustment) panelHeight += topAdjHeight + sectionGap;
    if (showArrowGlyph) panelHeight += arrowSize.y + sectionGap;
    panelHeight += linesHeight + padY;

    const float displayW = ImGui::GetIO().DisplaySize.x;
    const float centeredX = std::max(0.0f, (displayW - panelWidth) * 0.5f);
    ImVec2 panelMin(centeredX, static_cast<float>(snap.y));
    ImVec2 panelMax(panelMin.x + panelWidth, panelMin.y + panelHeight);

    int textAlpha = static_cast<int>(std::clamp(snap.overlayOpacity, 0.0f, 1.0f) * 255.0f);
    int bgAlpha = static_cast<int>(std::clamp(snap.overlayOpacity * snap.backgroundOpacity, 0.0f, 1.0f) * 255.0f);
    ImU32 bgColor = IM_COL32(7, 15, 24, bgAlpha);
    ImU32 borderColor = IM_COL32(155, 225, 190, textAlpha);
    ImU32 statusColor =
        snap.targetLocked ? IM_COL32(255, 235, 140, textAlpha) : IM_COL32(180, 255, 200, textAlpha);
    float alignmentRatio = snap.showComputedDetails ? std::clamp(1.0f - std::abs(snap.relativeYaw) / 90.0f, 0.0f, 1.0f) : 0.5f;
    int arrowR = static_cast<int>(std::lround(255.0f - 125.0f * alignmentRatio));
    int arrowG = static_cast<int>(std::lround(120.0f + 135.0f * alignmentRatio));
    int arrowB = static_cast<int>(std::lround(110.0f + 60.0f * alignmentRatio));
    ImU32 arrowColor = IM_COL32(arrowR, arrowG, arrowB, textAlpha);
    ImU32 lineColor = IM_COL32(242, 248, 255, textAlpha);
    ImU32 mutedColor = IM_COL32(196, 220, 236, textAlpha);
    ImU32 negativeColor = IM_COL32(255, 165, 165, textAlpha);
    ImU32 boatBlueColor = IM_COL32(130, 185, 255, textAlpha);
    ImU32 boatGreenColor = IM_COL32(130, 255, 160, textAlpha);
    ImU32 boatRedColor = IM_COL32(255, 130, 130, textAlpha);
    ImU32 topAdjColor = IM_COL32(235, 246, 255, textAlpha);
    ImU32 topAdjPlusColor = IM_COL32(130, 255, 160, textAlpha);
    ImU32 topAdjMinusColor = IM_COL32(255, 130, 130, textAlpha);
    ImU32 warningColor = IM_COL32(255, 150, 130, textAlpha);

    drawList->AddRectFilled(panelMin, panelMax, bgColor, 11.0f * uiScale);
    drawList->AddRect(panelMin, panelMax, borderColor, 11.0f * uiScale, 0, std::max(1.0f, 1.5f * uiScale));

    float currentY = panelMin.y + padY;
    ImVec2 statusPos(panelMin.x + (panelWidth - statusSize.x) * 0.5f, currentY);
    drawList->AddText(font, statusFontSize, statusPos, statusColor, snap.statusLabel.c_str());

    currentY += statusSize.y + sectionGap;
    if (showTopAdjustment) {
        float blockWidth = topAdjTextSize.x + topAdjGap + topAdjStepSlotWidth;
        float blockX = panelMin.x + (panelWidth - blockWidth) * 0.5f;
        drawList->AddText(font, topAdjFontSize, ImVec2(blockX, currentY + (topAdjHeight - topAdjTextSize.y) * 0.5f), topAdjColor,
                          topAdjustmentText.c_str());
        ImU32 stepColor = mutedColor;
        if (topAdjustmentStepActive) { stepColor = (snap.angleAdjustmentDeg > 0.0f) ? topAdjPlusColor : topAdjMinusColor; }
        const float stepX = blockX + topAdjTextSize.x + topAdjGap + (topAdjStepSlotWidth - topAdjStepSize.x) * 0.5f;
        drawList->AddText(font, topAdjStepFontSize, ImVec2(stepX, currentY), stepColor, topAdjustmentStepText.c_str());
        currentY += topAdjHeight + sectionGap;
    }

    if (showArrowGlyph) {
        if (snap.showComputedDetails) {
            const ImVec2 arrowCenter(panelMin.x + panelWidth * 0.5f, currentY + arrowSize.y * 0.5f);
            const float arrowRadius = std::max(8.0f * uiScale, arrowSize.y * 0.46f);
            const int ringAlpha = std::max(40, textAlpha / 2);
            const ImU32 ringColor = IM_COL32(225, 240, 255, ringAlpha);
            DrawContinuousCompassArrow(drawList, arrowCenter, arrowRadius, snap.relativeYaw, arrowColor, ringColor);
        } else {
            ImVec2 idleSize = font->CalcTextSizeA(arrowFontSize, FLT_MAX, 0.0f, idleArrowLabel);
            ImVec2 arrowPos(panelMin.x + (panelWidth - idleSize.x) * 0.5f, currentY + (arrowSize.y - idleSize.y) * 0.5f);
            drawList->AddText(font, arrowFontSize, arrowPos, arrowColor, idleArrowLabel);
        }
        currentY += arrowSize.y + sectionGap;
    }
    if (hasCoordRows) {
        const float emphasizedCoordFontSize = lineFontSize * 1.86f;
        const float estimatedCoordFontSize = lineFontSize * 1.02f;
        const float axisLegendFontSize = lineFontSize * 0.84f;
        const float axisBarHeight = std::max(6.0f * uiScale, lineFontSize * 0.27f);
        const float axisBarSpacingX = std::max(10.0f * uiScale, lineFontSize * 0.52f);
        const float axisBarLegendGap = std::max(2.0f * uiScale, lineFontSize * 0.12f);
        const float axisBarAfterGap = std::max(5.0f * uiScale, lineFontSize * 0.26f);
        const float axisBarWidth = std::max(86.0f * uiScale, (panelWidth - (padX * 2.0f) - axisBarSpacingX) * 0.5f);
        const ImU32 emphasizedCoordColor = IM_COL32(255, 238, 145, textAlpha);
        const ImU32 estimatedCoordColor = IM_COL32(145, 220, 255, textAlpha);
        const ImU32 estimatedMetaColor = IM_COL32(196, 220, 236, textAlpha);
        const ImU32 axisTrackColor = IM_COL32(38, 54, 68, std::max(60, static_cast<int>(textAlpha * 0.85f)));
        const ImU32 axisTrackBorderColor = IM_COL32(98, 128, 146, std::max(70, static_cast<int>(textAlpha * 0.88f)));
        auto axisColorFromCloseness = [&](float closeness) {
            const float t = std::clamp(closeness, 0.0f, 1.0f);
            const int r = static_cast<int>(std::lround(255.0f - 178.0f * t));
            const int g = static_cast<int>(std::lround(96.0f + 159.0f * t));
            const int b = static_cast<int>(std::lround(118.0f + 28.0f * t));
            return IM_COL32(r, g, b, textAlpha);
        };
        auto closenessFromDelta = [&](int deltaAbs, int referenceAbs) {
            const float denom = std::max(6.0f, static_cast<float>(referenceAbs));
            return std::clamp(1.0f - (static_cast<float>(deltaAbs) / denom), 0.0f, 1.0f);
        };
        auto drawCoordRow = [&](const std::string& prefix, const std::string& xText, const std::string& zText, float fontSize,
                                ImU32 prefixColor) {
            float x = panelMin.x + padX;
            drawList->AddText(font, fontSize, ImVec2(x, currentY), prefixColor, prefix.c_str());
            x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, prefix.c_str()).x;
            drawList->AddText(font, fontSize, ImVec2(x, currentY), NegativeAwareTextColor(xText, lineColor, negativeColor),
                              xText.c_str());
            x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, xText.c_str()).x;
            drawList->AddText(font, fontSize, ImVec2(x, currentY), lineColor, coordSep.c_str());
            x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, coordSep.c_str()).x;
            drawList->AddText(font, fontSize, ImVec2(x, currentY), NegativeAwareTextColor(zText, lineColor, negativeColor),
                              zText.c_str());
            currentY += lineAdvance * (fontSize / lineFontSize);
        };
        auto drawAxisBar = [&](float x, float yTop, const char* axisLabel, float closeness, ImU32 axisColor, const std::string& deltaText) {
            const int percent = static_cast<int>(std::lround(std::clamp(closeness, 0.0f, 1.0f) * 100.0f));
            const std::string legend = std::string(axisLabel) + " " + std::to_string(percent) + "% " + deltaText;
            drawList->AddText(font, axisLegendFontSize, ImVec2(x, yTop), axisColor, legend.c_str());

            const float barTop = yTop + axisLegendFontSize + axisBarLegendGap;
            const float barBottom = barTop + axisBarHeight;
            const float barRight = x + axisBarWidth;
            drawList->AddRectFilled(ImVec2(x, barTop), ImVec2(barRight, barBottom), axisTrackColor, axisBarHeight * 0.48f);
            drawList->AddRect(ImVec2(x, barTop), ImVec2(barRight, barBottom), axisTrackBorderColor, axisBarHeight * 0.48f);
            const float fillWidth = axisBarWidth * std::clamp(closeness, 0.0f, 1.0f);
            if (fillWidth > 0.5f) {
                drawList->AddRectFilled(ImVec2(x, barTop), ImVec2(x + fillWidth, barBottom), axisColor, axisBarHeight * 0.48f);
            }
        };
        auto drawEstimatedCoordRow = [&](const std::string& prefix, int estimatedX, int estimatedZ, int targetX, int targetZ, int playerX,
                                         int playerZ, float fontSize) {
            const std::string xText = std::to_string(estimatedX);
            const std::string zText = std::to_string(estimatedZ);
            const std::string dxText = formatSignedInt(estimatedX - targetX);
            const std::string dzText = formatSignedInt(estimatedZ - targetZ);
            const int deltaXAbs = std::abs(estimatedX - targetX);
            const int deltaZAbs = std::abs(estimatedZ - targetZ);
            const int referenceXAbs = std::abs(playerX - targetX);
            const int referenceZAbs = std::abs(playerZ - targetZ);
            const float xCloseness = closenessFromDelta(deltaXAbs, referenceXAbs);
            const float zCloseness = closenessFromDelta(deltaZAbs, referenceZAbs);
            const ImU32 xAxisColor = axisColorFromCloseness(xCloseness);
            const ImU32 zAxisColor = axisColorFromCloseness(zCloseness);

            float x = panelMin.x + padX;
            drawList->AddText(font, fontSize, ImVec2(x, currentY), estimatedCoordColor, prefix.c_str());
            x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, prefix.c_str()).x;

            drawList->AddText(font, fontSize, ImVec2(x, currentY), xAxisColor, xText.c_str());
            x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, xText.c_str()).x;
            drawList->AddText(font, fontSize, ImVec2(x, currentY), lineColor, coordSep.c_str());
            x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, coordSep.c_str()).x;
            drawList->AddText(font, fontSize, ImVec2(x, currentY), zAxisColor, zText.c_str());
            x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, zText.c_str()).x;

            drawList->AddText(font, fontSize, ImVec2(x, currentY), estimatedMetaColor, deltaXPrefix.c_str());
            x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, deltaXPrefix.c_str()).x;
            drawList->AddText(font, fontSize, ImVec2(x, currentY), xAxisColor, dxText.c_str());
            x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, dxText.c_str()).x;
            drawList->AddText(font, fontSize, ImVec2(x, currentY), estimatedMetaColor, deltaZPrefix.c_str());
            x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, deltaZPrefix.c_str()).x;
            drawList->AddText(font, fontSize, ImVec2(x, currentY), zAxisColor, dzText.c_str());

            currentY += lineAdvance * (fontSize / lineFontSize);
            const float barY = currentY;
            drawAxisBar(panelMin.x + padX, barY, "X", xCloseness, xAxisColor, dxText);
            drawAxisBar(panelMin.x + padX + axisBarWidth + axisBarSpacingX, barY, "Z", zCloseness, zAxisColor, dzText);
            currentY += axisLegendFontSize + axisBarLegendGap + axisBarHeight + axisBarAfterGap;
        };

        if (snap.usingNetherCoords) {
            drawCoordRow(targetNetherPrefix, targetNetherXText, targetNetherZText, emphasizedCoordFontSize, emphasizedCoordColor);
            if (showEstimateValues) {
                drawEstimatedCoordRow(estimatedNetherPrefix, snap.estimatedNetherX, snap.estimatedNetherZ, snap.targetNetherX,
                                      snap.targetNetherZ, snap.playerNetherX, snap.playerNetherZ, estimatedCoordFontSize);
            }
            drawCoordRow(playerNetherPrefix, playerNetherXText, playerNetherZText, lineFontSize, lineColor);
            drawCoordRow(targetOverworldPrefix, targetOverworldXText, targetOverworldZText, lineFontSize, lineColor);
            if (showEstimateValues) {
                drawEstimatedCoordRow(estimatedOverworldPrefix, snap.estimatedOverworldX, snap.estimatedOverworldZ, snap.targetOverworldX,
                                      snap.targetOverworldZ, snap.playerOverworldX, snap.playerOverworldZ, estimatedCoordFontSize);
            }
            drawCoordRow(playerOverworldPrefix, playerOverworldXText, playerOverworldZText, lineFontSize, lineColor);
        } else {
            drawCoordRow(targetOverworldPrefix, targetOverworldXText, targetOverworldZText, emphasizedCoordFontSize, emphasizedCoordColor);
            if (showEstimateValues) {
                drawEstimatedCoordRow(estimatedOverworldPrefix, snap.estimatedOverworldX, snap.estimatedOverworldZ, snap.targetOverworldX,
                                      snap.targetOverworldZ, snap.playerOverworldX, snap.playerOverworldZ, estimatedCoordFontSize);
            }
            drawCoordRow(playerOverworldPrefix, playerOverworldXText, playerOverworldZText, lineFontSize, lineColor);
            drawCoordRow(targetNetherPrefix, targetNetherXText, targetNetherZText, lineFontSize, lineColor);
            if (showEstimateValues) {
                drawEstimatedCoordRow(estimatedNetherPrefix, snap.estimatedNetherX, snap.estimatedNetherZ, snap.targetNetherX,
                                      snap.targetNetherZ, snap.playerNetherX, snap.playerNetherZ, estimatedCoordFontSize);
            }
            drawCoordRow(playerNetherPrefix, playerNetherXText, playerNetherZText, lineFontSize, lineColor);
        }
    }

    const bool hasStatusCertainty = snap.hasTopCertainty || snap.hasCombinedCertainty;
    const float statusCertaintyPercent = snap.hasTopCertainty ? snap.topCertaintyPercent
                                                              : (snap.hasCombinedCertainty ? snap.combinedCertaintyPercent : 50.0f);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        ImU32 currentColor = lineColor;
        bool isBoatLine = false;
        if (boatLineIndex >= 0 && static_cast<int>(i) == boatLineIndex) {
            currentColor = mutedColor;
            isBoatLine = true;
        } else if (warningLineIndex >= 0 && static_cast<int>(i) == warningLineIndex) {
            currentColor = warningColor;
        }
        std::string displayLine = line;
        float lineX = panelMin.x + padX;
        if (isBoatLine) {
            const float iconSize = std::max(10.0f, lineFontSize * 0.96f);
            DrawStrongholdStatusIconImGui(drawList, ImVec2(lineX + iconSize * 0.56f, currentY + lineFontSize * 0.56f), iconSize,
                                          snap.boatModeEnabled, snap.boatState, hasStatusCertainty, statusCertaintyPercent, boatBlueColor,
                                          boatGreenColor, boatRedColor, mutedColor);
            lineX += iconSize + (4.0f * uiScale);
            (void)lineX;
            displayLine.clear();
        }
        if (!displayLine.empty()) { drawList->AddText(font, lineFontSize, ImVec2(lineX, currentY), currentColor, displayLine.c_str()); }
        currentY += lineAdvance;
    }
}

static void RT_RenderMcsrApiTrackerOverlayImGui(const McsrApiTrackerRenderSnapshot& snap, bool drawBehindGui) {
    if (!snap.enabled || !snap.visible) return;
    if (!ImGui::GetCurrentContext()) return;

    const float uiScale = std::clamp(snap.scale, 0.6f, 2.2f);
    const float overlayOpacity = std::clamp(snap.overlayOpacity, 0.4f, 1.0f);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const std::string gameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
    const bool isInWorld = gameState.find("inworld") != std::string::npos;
    static double s_apiDownSinceSec = -1.0;
    const double nowSec = ImGui::GetTime();
    if (snap.apiOnline) {
        s_apiDownSinceSec = -1.0;
    } else if (s_apiDownSinceSec < 0.0) {
        s_apiDownSinceSec = nowSec;
    }
    const bool showApiDownWarning = (!snap.apiOnline && s_apiDownSinceSec >= 0.0 && (nowSec - s_apiDownSinceSec) >= 45.0);
    const std::string statusLabelForDisplay = showApiDownWarning ? (snap.statusLabel.empty() ? "MCSR API unavailable." : snap.statusLabel) : "";

    // In-game path: compact non-interactive HUD.
    // Out-of-game path: full tracker panel.
    if (isInWorld) {
        ImDrawList* dl = drawBehindGui ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
        if (!dl) return;

        const float panelW = std::clamp(display.x * 0.44f, 620.0f * uiScale, 980.0f * uiScale);
        const float panelH = 112.0f * uiScale;
        const float x = std::clamp(display.x - panelW - (24.0f * uiScale) + static_cast<float>(snap.x), 0.0f,
                                   std::max(0.0f, display.x - panelW));
        const float y = std::clamp((24.0f * uiScale) + static_cast<float>(snap.y), 0.0f, std::max(0.0f, display.y - panelH));
        const ImVec2 p0(x, y);
        const ImVec2 p1(x + panelW, y + panelH);
        const ImU32 bg = IM_COL32(12, 18, 30, static_cast<int>(235.0f * overlayOpacity));
        const ImU32 border = IM_COL32(70, 92, 132, static_cast<int>(235.0f * overlayOpacity));
        const ImU32 title = IM_COL32(225, 236, 255, static_cast<int>(255.0f * overlayOpacity));
        const ImU32 body = IM_COL32(198, 210, 236, static_cast<int>(255.0f * overlayOpacity));
        const ImU32 muted = IM_COL32(140, 156, 186, static_cast<int>(255.0f * overlayOpacity));
        const ImU32 warn = IM_COL32(255, 170, 170, static_cast<int>(255.0f * overlayOpacity));

        auto formatDurationMs = [](int durationMs) -> std::string {
            if (durationMs <= 0) return "--:--.--";
            const int totalSeconds = durationMs / 1000;
            const int minutes = totalSeconds / 60;
            const int seconds = totalSeconds % 60;
            const int centiseconds = (durationMs % 1000) / 10;
            std::ostringstream out;
            out << std::setfill('0') << std::setw(2) << minutes << ":" << std::setw(2) << seconds << "." << std::setw(2)
                << centiseconds;
            return out.str();
        };

        const std::string player = !snap.displayPlayer.empty() ? snap.displayPlayer :
                                   (!snap.requestedPlayer.empty() ? snap.requestedPlayer :
                                    (!snap.headerLabel.empty() ? snap.headerLabel : "MCSR"));

        dl->AddRectFilled(p0, p1, bg, 7.0f * uiScale);
        dl->AddRect(p0, p1, border, 7.0f * uiScale, 0, std::max(1.0f, 1.2f * uiScale));
        dl->AddText(ImVec2(p0.x + 10.0f * uiScale, p0.y + 8.0f * uiScale), title,
                    (std::string("#") + std::to_string(std::max(0, snap.eloRank)) + " " + player).c_str());
        dl->AddText(ImVec2(p0.x + 10.0f * uiScale, p0.y + 30.0f * uiScale), body,
                    (std::to_string(std::max(0, snap.eloRate)) + " elo  peak " + std::to_string(std::max(0, snap.peakElo))).c_str());
        dl->AddText(ImVec2(p0.x + 10.0f * uiScale, p0.y + 50.0f * uiScale), body,
                    (std::to_string(std::max(0, snap.seasonWins)) + "W " + std::to_string(std::max(0, snap.seasonLosses)) + "L  pb " +
                     formatDurationMs(snap.bestTimeMs))
                        .c_str());
        if (snap.apiOnline) {
            dl->AddText(ImVec2(p0.x + 10.0f * uiScale, p0.y + 70.0f * uiScale), muted, "Press Ctrl+I to move/resize/search.");
        } else if (showApiDownWarning) {
            dl->AddText(ImVec2(p0.x + 10.0f * uiScale, p0.y + 70.0f * uiScale), warn, "MCSR API has been unavailable for a while.");
        }

        if (!statusLabelForDisplay.empty()) {
            dl->AddText(ImVec2(p0.x + 10.0f * uiScale, p0.y + 88.0f * uiScale), muted, statusLabelForDisplay.c_str());
        }
        return;
    }

    static bool s_expanded = true;
    static bool s_searchDirty = false;
    static bool s_searchDrawerOpen = false;
    static int s_matchFilter = 0; // 0=ranked,1=all,2=private,3=casual,4=event
    static std::string s_lastSyncedRequested;
    static char s_searchBuf[64] = { 0 };
    static std::vector<std::string> s_cachedSearchPlayers;
    static std::vector<std::string> s_recentLoadedPlayers;
    static bool s_recentLoadedPlayersLoaded = false;

    auto trimAscii = [](std::string& value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) value.erase(value.begin());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) value.pop_back();
    };
    auto toLowerAscii = [](const std::string& in) {
        std::string out = in;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    };
    auto containsIgnoreCase = [&](const std::string& haystack, const std::string& needle) {
        if (needle.empty()) return true;
        return toLowerAscii(haystack).find(toLowerAscii(needle)) != std::string::npos;
    };
    auto equalsIgnoreCase = [&](const std::string& a, const std::string& b) {
        return toLowerAscii(a) == toLowerAscii(b);
    };
    auto recentPlayersFilePath = [&]() -> std::filesystem::path {
        if (!g_toolscreenPath.empty()) {
            return std::filesystem::path(g_toolscreenPath) / L"mcsr_recent_players.txt";
        }
        return std::filesystem::path(L"mcsr_recent_players.txt");
    };
    auto persistRecentLoadedPlayers = [&]() {
        std::error_code ec;
        const std::filesystem::path path = recentPlayersFilePath();
        if (!path.parent_path().empty()) { std::filesystem::create_directories(path.parent_path(), ec); }
        std::ofstream out(path, std::ios::trunc);
        if (!out.is_open()) return;
        for (const std::string& name : s_recentLoadedPlayers) {
            if (name.empty()) continue;
            out << name << "\n";
        }
    };
    auto loadRecentLoadedPlayersIfNeeded = [&]() {
        if (s_recentLoadedPlayersLoaded) return;
        s_recentLoadedPlayersLoaded = true;
        s_recentLoadedPlayers.clear();

        std::ifstream in(recentPlayersFilePath());
        if (!in.is_open()) return;

        std::string line;
        while (std::getline(in, line)) {
            trimAscii(line);
            if (line.empty()) continue;
            bool exists = false;
            for (const std::string& existing : s_recentLoadedPlayers) {
                if (equalsIgnoreCase(existing, line)) {
                    exists = true;
                    break;
                }
            }
            if (exists) continue;
            s_recentLoadedPlayers.push_back(line);
            if (s_recentLoadedPlayers.size() >= 5) break;
        }
    };
    auto pushUniqueCachedPlayer = [&](const std::string& candidate) {
        std::string value = candidate;
        trimAscii(value);
        if (value.empty()) return;
        for (const std::string& existing : s_cachedSearchPlayers) {
            if (equalsIgnoreCase(existing, value)) return;
        }
        s_cachedSearchPlayers.push_back(value);
        if (s_cachedSearchPlayers.size() > 4096) { s_cachedSearchPlayers.erase(s_cachedSearchPlayers.begin()); }
    };
    auto pushRecentLoadedPlayer = [&](const std::string& candidate) {
        loadRecentLoadedPlayersIfNeeded();
        std::string value = candidate;
        trimAscii(value);
        if (value.empty()) return;
        auto it = std::remove_if(s_recentLoadedPlayers.begin(), s_recentLoadedPlayers.end(),
                                 [&](const std::string& existing) { return equalsIgnoreCase(existing, value); });
        if (it != s_recentLoadedPlayers.end()) s_recentLoadedPlayers.erase(it, s_recentLoadedPlayers.end());
        s_recentLoadedPlayers.insert(s_recentLoadedPlayers.begin(), value);
        if (s_recentLoadedPlayers.size() > 5) s_recentLoadedPlayers.resize(5);
        persistRecentLoadedPlayers();
    };

    auto formatDurationMs = [](int durationMs) -> std::string {
        if (durationMs <= 0) return "--:--.--";
        const int totalSeconds = durationMs / 1000;
        const int minutes = totalSeconds / 60;
        const int seconds = totalSeconds % 60;
        const int centiseconds = (durationMs % 1000) / 10;
        std::ostringstream out;
        out << std::setfill('0') << std::setw(2) << minutes << ":" << std::setw(2) << seconds << "." << std::setw(2) << centiseconds;
        return out.str();
    };

    auto formatPercentShort = [](float value) -> std::string {
        const float clamped = std::clamp(value, 0.0f, 100.0f);
        std::ostringstream out;
        if (std::fabs(clamped - std::round(clamped)) < 0.05f) {
            out << static_cast<int>(std::round(clamped)) << "%";
        } else {
            out << std::fixed << std::setprecision(1) << clamped << "%";
        }
        return out.str();
    };

    auto tierLabelForElo = [](int elo) -> const char* {
        if (elo >= 1800) return "Netherite";
        if (elo >= 1500) return "Diamond";
        if (elo >= 1200) return "Gold";
        if (elo >= 900) return "Silver";
        if (elo >= 600) return "Iron";
        return "Coal";
    };

    const ImVec2 expandedSize(std::clamp(display.x * 0.76f, 1080.0f * uiScale, 1640.0f * uiScale),
                              std::clamp(display.y * 0.72f, 620.0f * uiScale, 920.0f * uiScale));
    const ImVec2 compactSize(std::clamp(display.x * 0.56f, 780.0f * uiScale, 1160.0f * uiScale),
                             std::clamp(display.y * 0.52f, 430.0f * uiScale, 660.0f * uiScale));

    const ImVec2 chosenSize = s_expanded ? expandedSize : compactSize;
    const ImVec2 defaultPos(std::clamp(display.x - chosenSize.x - (30.0f * uiScale) + static_cast<float>(snap.x), 0.0f,
                                       std::max(0.0f, display.x - 280.0f * uiScale)),
                            std::clamp((34.0f * uiScale) + static_cast<float>(snap.y), 0.0f, std::max(0.0f, display.y - 220.0f * uiScale)));

    ImGui::SetNextWindowPos(defaultPos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(chosenSize, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(1.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * uiScale, 12.0f * uiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * uiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * uiScale);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(10, 16, 28, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(68, 86, 120, static_cast<int>(220.0f * overlayOpacity)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(16, 22, 35, 255));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(30, 48, 78, 180));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(40, 62, 98, 220));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(48, 72, 110, 240));

    const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("MCSR Ranked Tracker###MCSR_TRACKER_OVERLAY", nullptr, windowFlags)) {
        const float pad = 8.0f * uiScale;
        const ImU32 titleColor = IM_COL32(224, 236, 255, static_cast<int>(255.0f * overlayOpacity));
        const ImU32 bodyColor = IM_COL32(196, 208, 232, static_cast<int>(255.0f * overlayOpacity));
        const ImU32 mutedColor = IM_COL32(130, 146, 176, static_cast<int>(255.0f * overlayOpacity));
        const ImU32 winColor = IM_COL32(82, 235, 140, static_cast<int>(255.0f * overlayOpacity));
        const ImU32 lossColor = IM_COL32(255, 104, 116, static_cast<int>(255.0f * overlayOpacity));
        const ImU32 drawColor = IM_COL32(98, 170, 255, static_cast<int>(255.0f * overlayOpacity));
        const ImU32 warnColor = IM_COL32(255, 170, 170, static_cast<int>(255.0f * overlayOpacity));

        const std::string playerLabel = !snap.headerLabel.empty() ? snap.headerLabel :
                                        (!snap.displayPlayer.empty() ? snap.displayPlayer :
                                         (!snap.requestedPlayer.empty() ? snap.requestedPlayer : "MCSR Player"));
        pushUniqueCachedPlayer("Feinberg");
        pushUniqueCachedPlayer(snap.autoDetectedPlayer);
        pushUniqueCachedPlayer(snap.requestedPlayer);
        pushUniqueCachedPlayer(snap.displayPlayer);
        for (const std::string& suggested : snap.suggestedPlayers) {
            pushUniqueCachedPlayer(suggested);
        }
        loadRecentLoadedPlayersIfNeeded();
        auto tierColorForElo = [&](int elo) {
            if (elo >= 1800) return IM_COL32(194, 242, 255, static_cast<int>(255.0f * overlayOpacity));
            if (elo >= 1500) return IM_COL32(120, 206, 255, static_cast<int>(255.0f * overlayOpacity));
            if (elo >= 1200) return IM_COL32(255, 221, 130, static_cast<int>(255.0f * overlayOpacity));
            if (elo >= 900) return IM_COL32(193, 208, 234, static_cast<int>(255.0f * overlayOpacity));
            if (elo >= 600) return IM_COL32(185, 197, 216, static_cast<int>(255.0f * overlayOpacity));
            return IM_COL32(152, 164, 184, static_cast<int>(255.0f * overlayOpacity));
        };
        const std::string homePlayer = snap.autoDetectedPlayer;
        const std::string viewingPlayer = !snap.displayPlayer.empty() ? snap.displayPlayer : playerLabel;
        const bool hasHome = !homePlayer.empty();
        const bool viewingOther = hasHome && !equalsIgnoreCase(viewingPlayer, homePlayer);

        auto makeInitials = [&](const std::string& name) {
            std::string initials;
            for (char ch : name) {
                if (std::isalnum(static_cast<unsigned char>(ch)) == 0) continue;
                initials.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
                if (initials.size() >= 2) break;
            }
            if (initials.empty()) initials = "P";
            return initials;
        };

        auto applyPlayerSelection = [&](const std::string& valueRaw) {
            std::string value = valueRaw;
            trimAscii(value);
            if (value.empty()) return;
            SetMcsrApiTrackerSearchPlayer(value);
            pushRecentLoadedPlayer(value);
            s_searchBuf[0] = '\0';
            s_lastSyncedRequested = value;
            s_searchDirty = false;
        };

        if (s_searchDrawerOpen) {
            const float drawerWidth = std::clamp(290.0f * uiScale, 220.0f * uiScale, 360.0f * uiScale);
            if (ImGui::BeginChild("##McsrSearchDrawer", ImVec2(drawerWidth, 0.0f), true,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                ImGui::TextColored(ImColor(titleColor), "Player Search");
                ImGui::SameLine();
                if (ImGui::SmallButton("X##McsrCloseDrawer")) { s_searchDrawerOpen = false; }
                if (hasHome) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("My Profile##McsrDrawerHome")) { applyPlayerSelection(homePlayer); }
                }
                ImGui::Separator();

                const float slashButtonW = 22.0f * uiScale;
                ImGui::SetNextItemWidth(std::max(120.0f * uiScale, ImGui::GetContentRegionAvail().x - slashButtonW - (6.0f * uiScale)));
                const bool searchEdited =
                    ImGui::InputTextWithHint("##McsrOverlaySearch", "Search for players", s_searchBuf, static_cast<int>(sizeof(s_searchBuf)),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
                if (ImGui::IsItemActivated()) {
                    s_searchBuf[0] = '\0';
                    s_searchDirty = true;
                }
                if (searchEdited) { applyPlayerSelection(s_searchBuf); }
                else if (ImGui::IsItemEdited()) {
                    s_searchDirty = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("/##McsrDrawerSearch")) { applyPlayerSelection(s_searchBuf); }

                std::vector<std::string> filteredSuggestions;
                std::vector<std::string> filteredRecent;
                {
                    std::string query = s_searchBuf;
                    trimAscii(query);
                    for (const std::string& candidate : s_cachedSearchPlayers) {
                        if (candidate.empty()) continue;
                        if (!containsIgnoreCase(candidate, query)) continue;
                        filteredSuggestions.push_back(candidate);
                        if (filteredSuggestions.size() >= 24) break;
                    }
                    for (const std::string& candidate : s_recentLoadedPlayers) {
                        if (candidate.empty()) continue;
                        if (!containsIgnoreCase(candidate, query)) continue;
                        bool alreadyInRanked = false;
                        for (const std::string& rankedCandidate : filteredSuggestions) {
                            if (equalsIgnoreCase(rankedCandidate, candidate)) {
                                alreadyInRanked = true;
                                break;
                            }
                        }
                        if (alreadyInRanked) continue;
                        filteredRecent.push_back(candidate);
                        if (filteredRecent.size() >= 5) break;
                    }
                }

                ImGui::Spacing();
                if (ImGui::BeginChild("##McsrDrawerSuggestions", ImVec2(0.0f, 0.0f), false,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                    ImGui::TextDisabled("Ranked players");
                    ImGui::Separator();
                    for (size_t i = 0; i < filteredSuggestions.size(); ++i) {
                        const std::string& suggestion = filteredSuggestions[i];
                        const bool selected = equalsIgnoreCase(suggestion, viewingPlayer);
                        if (selected) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(238, 204, 130, static_cast<int>(255.0f * overlayOpacity)));
                        std::string label = std::to_string(static_cast<int>(i + 1)) + ". " + suggestion + "##McsrRankedSugg" +
                                            std::to_string(i);
                        if (ImGui::Selectable(label.c_str(), selected)) { applyPlayerSelection(suggestion); }
                        if (selected) ImGui::PopStyleColor();
                    }

                    ImGui::Spacing();
                    ImGui::TextDisabled("Recent quicksearch");
                    ImGui::Separator();
                    if (filteredRecent.empty()) {
                        ImGui::TextDisabled("No recent profiles yet");
                    } else {
                        for (size_t i = 0; i < filteredRecent.size(); ++i) {
                            const std::string& suggestion = filteredRecent[i];
                            const bool selected = equalsIgnoreCase(suggestion, viewingPlayer);
                            if (selected) {
                                ImGui::PushStyleColor(ImGuiCol_Text,
                                                      IM_COL32(238, 204, 130, static_cast<int>(255.0f * overlayOpacity)));
                            }
                            std::string label =
                                "R" + std::to_string(static_cast<int>(i + 1)) + ". " + suggestion + "##McsrRecentSugg" +
                                std::to_string(i);
                            if (ImGui::Selectable(label.c_str(), selected)) { applyPlayerSelection(suggestion); }
                            if (selected) ImGui::PopStyleColor();
                        }
                    }
                }
                ImGui::EndChild();
            }
            ImGui::EndChild();
            ImGui::SameLine(0.0f, pad);
        }

        if (ImGui::BeginChild("##McsrMainContent", ImVec2(0.0f, 0.0f), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::Spacing();
            const float topPanelHeight = s_expanded ? (236.0f * uiScale) : (176.0f * uiScale);
            if (ImGui::BeginChild("##McsrTopPanel", ImVec2(0.0f, topPanelHeight), true,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                const int seasonGames = std::max(0, snap.seasonWins + snap.seasonLosses);
                const float seasonWinrate =
                    (seasonGames > 0) ? (100.0f * static_cast<float>(snap.seasonWins) / static_cast<float>(seasonGames)) : 0.0f;
                const bool hasAvatarTexture = RT_EnsureMcsrTextureFromFile(snap.avatarImagePath, g_mcsrAvatarTextureCache);
                const bool hasFlagTexture = RT_EnsureMcsrTextureFromFile(snap.flagImagePath, g_mcsrFlagTextureCache);

                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImU32 dividerColor = IM_COL32(80, 102, 140, static_cast<int>(220.0f * overlayOpacity));
                const ImU32 valueColor = IM_COL32(236, 245, 255, static_cast<int>(255.0f * overlayOpacity));
                const float menuBtnSize = 24.0f * uiScale;
                const ImVec2 menuPos = ImGui::GetCursorScreenPos();
                const ImVec2 menuEnd(menuPos.x + menuBtnSize, menuPos.y + menuBtnSize);
                ImGui::InvisibleButton("##McsrDrawerToggle", ImVec2(menuBtnSize, menuBtnSize));
                const bool menuClicked = ImGui::IsItemClicked();
                const bool menuHovered = ImGui::IsItemHovered();
                const ImU32 menuBg = s_searchDrawerOpen ? IM_COL32(54, 78, 112, 255)
                                                        : (menuHovered ? IM_COL32(44, 62, 92, 255) : IM_COL32(30, 44, 66, 255));
                const ImU32 menuBorder = IM_COL32(104, 136, 184, 255);
                const ImU32 menuLine = IM_COL32(220, 232, 252, 255);
                dl->AddRectFilled(menuPos, menuEnd, menuBg, 5.0f * uiScale);
                dl->AddRect(menuPos, menuEnd, menuBorder, 5.0f * uiScale, 0, std::max(1.0f, 1.1f * uiScale));
                const float lx0 = menuPos.x + (6.0f * uiScale);
                const float lx1 = menuEnd.x - (6.0f * uiScale);
                const float ly0 = menuPos.y + (7.0f * uiScale);
                const float ldy = 5.0f * uiScale;
                dl->AddLine(ImVec2(lx0, ly0), ImVec2(lx1, ly0), menuLine, std::max(1.0f, 1.6f * uiScale));
                dl->AddLine(ImVec2(lx0, ly0 + ldy), ImVec2(lx1, ly0 + ldy), menuLine, std::max(1.0f, 1.6f * uiScale));
                dl->AddLine(ImVec2(lx0, ly0 + (2.0f * ldy)), ImVec2(lx1, ly0 + (2.0f * ldy)), menuLine,
                            std::max(1.0f, 1.6f * uiScale));
                if (menuClicked) { s_searchDrawerOpen = !s_searchDrawerOpen; }

                const int safeElo = std::max(0, snap.eloRate);
                const int safePeak = std::max(0, snap.peakElo);
                const int safePoints = std::max(0, snap.seasonPoints);

                ImGui::SameLine();
                if (ImGui::Button("Refresh##McsrTopRefresh")) { RequestMcsrApiTrackerRefresh(); }
                ImGui::SameLine();
                if (ImGui::Button(s_expanded ? "Compact##McsrTopCompact" : "Expand##McsrTopCompact")) {
                    s_expanded = !s_expanded;
                    ImGui::SetWindowSize(s_expanded ? expandedSize : compactSize, ImGuiCond_Always);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", snap.refreshOnlyMode ? "Refresh-only mode" : "Auto polling mode");

                const float profileForfeitRatePercent = std::clamp(
                    (snap.profileForfeitRatePercent > 0.0f || snap.recentForfeitRatePercent <= 0.0f) ? snap.profileForfeitRatePercent
                                                                                                       : snap.recentForfeitRatePercent,
                    0.0f, 100.0f);
                const int displayAverageMs = (snap.profileAverageTimeMs > 0) ? snap.profileAverageTimeMs : snap.averageResultTimeMs;

                auto drawSegmentLine = [&](float x, float y, float fontSize,
                                           const std::vector<std::pair<std::string, ImU32>>& segments) {
                    const ImU32 shadow = IM_COL32(8, 12, 18, static_cast<int>(220.0f * overlayOpacity));
                    float cursorX = x;
                    for (const auto& seg : segments) {
                        if (seg.first.empty()) continue;
                        dl->AddText(ImGui::GetFont(), fontSize, ImVec2(cursorX + (1.0f * uiScale), y + (1.0f * uiScale)), shadow,
                                    seg.first.c_str());
                        dl->AddText(ImGui::GetFont(), fontSize, ImVec2(cursorX, y), seg.second, seg.first.c_str());
                        cursorX += ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, seg.first.c_str()).x;
                    }
                };

                auto drawGradientText = [&](const ImVec2& pos, float fontSize, const std::string& text, ImU32 leftCol, ImU32 rightCol) {
                    if (text.empty()) return;
                    float totalWidth = 0.0f;
                    std::vector<float> widths;
                    widths.reserve(text.size());
                    for (char c : text) {
                        char buf[2] = { c, '\0' };
                        const float w = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, buf).x;
                        widths.push_back(w);
                        totalWidth += w;
                    }
                    float cursorX = pos.x;
                    for (size_t i = 0; i < text.size(); ++i) {
                        char buf[2] = { text[i], '\0' };
                        const float centerX = cursorX + (widths[i] * 0.5f);
                        const float t = (totalWidth > 0.0f) ? std::clamp((centerX - pos.x) / totalWidth, 0.0f, 1.0f) : 0.0f;
                        dl->AddText(ImGui::GetFont(), fontSize, ImVec2(cursorX, pos.y), RT_LerpColor(leftCol, rightCol, t), buf);
                        cursorX += widths[i];
                    }
                };

                ImGui::Dummy(ImVec2(0.0f, 5.0f * uiScale));
                const ImVec2 contentPos = ImGui::GetCursorScreenPos();
                const float contentW = std::max(80.0f * uiScale, ImGui::GetContentRegionAvail().x);
                const float avatarSize = s_expanded ? (128.0f * uiScale) : (100.0f * uiScale);
                float nameFontSize = s_expanded ? (34.0f * uiScale) : (27.0f * uiScale);
                const float leftPad = 2.0f * uiScale;
                float nameWidth = ImGui::GetFont()->CalcTextSizeA(nameFontSize, FLT_MAX, 0.0f, viewingPlayer.c_str()).x;
                const float leftBlockMaxW = contentW * (s_expanded ? 0.34f : 0.36f);
                float leftBlockW =
                    std::clamp(std::max(avatarSize + (8.0f * uiScale), nameWidth + (12.0f * uiScale)), 126.0f * uiScale, leftBlockMaxW);
                const float maxNameW = std::max(30.0f * uiScale, leftBlockW - (8.0f * uiScale));
                while (nameFontSize > (16.0f * uiScale) && nameWidth > maxNameW) {
                    nameFontSize -= (1.0f * uiScale);
                    nameWidth = ImGui::GetFont()->CalcTextSizeA(nameFontSize, FLT_MAX, 0.0f, viewingPlayer.c_str()).x;
                }
                const float avatarX = contentPos.x + leftPad + ((leftBlockW - avatarSize) * 0.5f);
                const float avatarY = contentPos.y + (2.0f * uiScale);
                const ImVec2 avatarMin(avatarX, avatarY);
                const ImVec2 avatarMax(avatarX + avatarSize, avatarY + avatarSize);
                const std::string initials = makeInitials(viewingPlayer);
                const ImU32 avatarFallbackBg = viewingOther ? IM_COL32(78, 62, 36, 230) : IM_COL32(40, 58, 86, 230);
                const ImU32 avatarFallbackText = IM_COL32(228, 238, 255, 255);

                if (hasAvatarTexture && g_mcsrAvatarTextureCache.textureId != 0) {
                    ImVec2 uv0 = g_mcsrAvatarTextureCache.uvMin;
                    ImVec2 uv1 = g_mcsrAvatarTextureCache.uvMax;
                    std::swap(uv0.y, uv1.y); // Crafatar head assets are vertically flipped in this draw path.
                    dl->AddImage((ImTextureID)(intptr_t)g_mcsrAvatarTextureCache.textureId, avatarMin, avatarMax, uv0, uv1);
                } else {
                    dl->AddRectFilled(avatarMin, avatarMax, avatarFallbackBg, 4.0f * uiScale);
                    const float fallbackFont = std::max(16.0f, 34.0f * uiScale);
                    const ImVec2 initSize = ImGui::GetFont()->CalcTextSizeA(fallbackFont, FLT_MAX, 0.0f, initials.c_str());
                    dl->AddText(ImGui::GetFont(), fallbackFont,
                                ImVec2(avatarX + ((avatarSize - initSize.x) * 0.5f), avatarY + ((avatarSize - initSize.y) * 0.5f)),
                                avatarFallbackText, initials.c_str());
                }

                const float nameY = avatarY + avatarSize + (8.0f * uiScale);
                const float nameX = contentPos.x + leftPad + ((leftBlockW - nameWidth) * 0.5f);
                const ImU32 gradA = viewingOther ? IM_COL32(255, 188, 118, static_cast<int>(255.0f * overlayOpacity))
                                                 : IM_COL32(117, 234, 255, static_cast<int>(255.0f * overlayOpacity));
                const ImU32 gradB = viewingOther ? IM_COL32(255, 120, 164, static_cast<int>(255.0f * overlayOpacity))
                                                 : IM_COL32(151, 255, 155, static_cast<int>(255.0f * overlayOpacity));
                drawGradientText(ImVec2(nameX, nameY), nameFontSize, viewingPlayer, gradA, gradB);

                const float statsX = contentPos.x + leftPad + leftBlockW + (10.0f * uiScale);
                const float statsW = std::max(150.0f * uiScale, contentW - (leftBlockW + (14.0f * uiScale)));
                float statFontSize = s_expanded ? (24.0f * uiScale) : (19.5f * uiScale);
                if (statsW < (640.0f * uiScale)) {
                    statFontSize *= std::clamp(statsW / (640.0f * uiScale), 0.80f, 1.0f);
                }
                const float statLineStep = statFontSize * 1.33f;
                const float statsY = contentPos.y + (8.0f * uiScale);
                const float midX = statsX + (statsW * 0.50f);
                const float statsBottom = statsY + (statLineStep * 3.02f);
                dl->AddRectFilled(ImVec2(statsX + (2.0f * uiScale), statsY - (5.0f * uiScale)),
                                  ImVec2(midX - (8.0f * uiScale), statsBottom + (7.0f * uiScale)),
                                  IM_COL32(16, 26, 42, static_cast<int>(128.0f * overlayOpacity)), 5.0f * uiScale);
                dl->AddRectFilled(ImVec2(midX + (6.0f * uiScale), statsY - (5.0f * uiScale)),
                                  ImVec2(statsX + statsW - (2.0f * uiScale), statsBottom + (7.0f * uiScale)),
                                  IM_COL32(16, 26, 42, static_cast<int>(128.0f * overlayOpacity)), 5.0f * uiScale);
                dl->AddLine(ImVec2(midX, statsY - (2.0f * uiScale)), ImVec2(midX, statsBottom + (4.0f * uiScale)), dividerColor,
                            std::max(1.0f, 1.5f * uiScale));

                const float col1X = statsX + (6.0f * uiScale);
                const float col2X = midX + (12.0f * uiScale);
                const float y1 = statsY;
                const float y2 = statsY + statLineStep;
                const float y3 = statsY + (2.0f * statLineStep);
                const ImU32 wrColor = RT_LerpColor(lossColor, winColor, std::clamp(seasonWinrate / 100.0f, 0.0f, 1.0f));
                const ImU32 ffColor = RT_LerpColor(winColor, lossColor, std::clamp(profileForfeitRatePercent / 100.0f, 0.0f, 1.0f));
                const ImU32 timeColor = IM_COL32(255, 216, 150, static_cast<int>(255.0f * overlayOpacity));
                const ImU32 accentColor = IM_COL32(146, 212, 255, static_cast<int>(255.0f * overlayOpacity));

                float line1StartX = col1X;
                if (hasFlagTexture && g_mcsrFlagTextureCache.textureId != 0) {
                    const float flagW = 24.0f * uiScale;
                    const float flagH = 16.0f * uiScale;
                    const ImVec2 flagPos(col1X, y1 + (4.0f * uiScale));
                    const ImVec2 flagEnd(flagPos.x + flagW, flagPos.y + flagH);
                    dl->AddImage((ImTextureID)(intptr_t)g_mcsrFlagTextureCache.textureId, flagPos, flagEnd, g_mcsrFlagTextureCache.uvMin,
                                 g_mcsrFlagTextureCache.uvMax);
                    dl->AddRect(flagPos, flagEnd, IM_COL32(98, 122, 168, static_cast<int>(240.0f * overlayOpacity)), 2.0f * uiScale, 0,
                                std::max(1.0f, 1.0f * uiScale));
                    line1StartX += flagW + (9.0f * uiScale);
                }

                drawSegmentLine(line1StartX, y1, statFontSize,
                                { { "#", mutedColor }, { std::to_string(std::max(0, snap.eloRank)), valueColor }, { " | ", mutedColor },
                                  { tierLabelForElo(safeElo), tierColorForElo(safeElo) } });
                drawSegmentLine(col1X, y2, statFontSize,
                                { { "ELO ", mutedColor }, { std::to_string(safeElo), accentColor }, { " | PEAK ", mutedColor },
                                  { std::to_string(safePeak), accentColor } });
                drawSegmentLine(col1X, y3, statFontSize,
                                { { "W ", mutedColor }, { std::to_string(std::max(0, snap.seasonWins)), winColor }, { " | L ", mutedColor },
                                  { std::to_string(std::max(0, snap.seasonLosses)), lossColor }, { " | C ", mutedColor },
                                  { std::to_string(std::max(0, snap.seasonCompletions)), drawColor } });

                drawSegmentLine(col2X, y1, statFontSize,
                                { { "WR ", mutedColor }, { formatPercentShort(seasonWinrate), wrColor }, { " | PB ", mutedColor },
                                  { formatDurationMs(snap.bestTimeMs), timeColor } });
                drawSegmentLine(col2X, y2, statFontSize,
                                { { "AVG ", mutedColor }, { formatDurationMs(displayAverageMs), timeColor }, { " | FF ", mutedColor },
                                  { formatPercentShort(profileForfeitRatePercent), ffColor } });
                drawSegmentLine(col2X, y3, statFontSize,
                                { { "WS ", mutedColor }, { std::to_string(std::max(0, snap.seasonBestWinStreak)), valueColor },
                                  { " | PTS ", mutedColor }, { std::to_string(safePoints), drawColor } });

                if (!statusLabelForDisplay.empty()) {
                    dl->AddText(ImGui::GetFont(), std::max(14.0f, 15.0f * uiScale),
                                ImVec2(statsX, statsBottom + (8.0f * uiScale)), warnColor, statusLabelForDisplay.c_str());
                }
                ImGui::Dummy(ImVec2(0.0f, topPanelHeight * 0.72f));
            }
            ImGui::EndChild();

            if (!snap.apiOnline) {
                if (!snap.autoDetectedPlayer.empty()) { ImGui::TextDisabled("Auto: %s", snap.autoDetectedPlayer.c_str()); }
                ImGui::EndChild();
                ImGui::End();
                ImGui::PopStyleColor(6);
                ImGui::PopStyleVar(3);
                return;
            }

        std::vector<float> eloSeries;
        eloSeries.reserve(std::max<size_t>(1, snap.eloHistory.size()));
        int minElo = std::max(1, snap.eloRate);
        int maxElo = std::max(minElo + 1, snap.eloRate + 1);
        for (int v : snap.eloHistory) {
            eloSeries.push_back(static_cast<float>(v));
            minElo = std::min(minElo, v);
            maxElo = std::max(maxElo, v);
        }
        if (eloSeries.empty()) eloSeries.push_back(static_cast<float>(std::max(0, snap.eloRate)));
        int eloRange = std::max(1, maxElo - minElo);
        const int minVisualRange = 80;
        if (eloRange < minVisualRange) {
            const int mid = (minElo + maxElo) / 2;
            minElo = mid - (minVisualRange / 2);
            maxElo = mid + (minVisualRange / 2);
        }
        eloRange = std::max(1, maxElo - minElo);
        const int graphMargin = std::max(12, eloRange / 12);
        minElo -= graphMargin;
        maxElo += graphMargin;
        minElo = std::max(0, minElo);

        if (s_expanded) {
            const float leftW = std::max(360.0f * uiScale, ImGui::GetContentRegionAvail().x * 0.34f);
            if (ImGui::BeginChild("##McsrMatches", ImVec2(leftW, 0.0f), true)) {
                static const char* kMatchFilterLabels[] = { "Ranked", "All", "Private", "Casual", "Event" };
                auto rowMatchesFilter = [&](const McsrApiTrackerRenderSnapshot::MatchRow& row) {
                    switch (s_matchFilter) {
                    case 0: // ranked
                        return row.categoryType == 0;
                    case 1: // all
                        return true;
                    case 2: // private
                        return row.categoryType == 1;
                    case 3: // casual
                        return row.categoryType == 2;
                    case 4: // event
                        return row.categoryType == 3;
                    default:
                        return row.categoryType == 0;
                    }
                };
                size_t filteredCount = 0;
                for (const auto& row : snap.recentMatches) {
                    if (rowMatchesFilter(row)) ++filteredCount;
                }

                ImGui::TextColored(ImColor(titleColor), "MATCHES");
                ImGui::SameLine();
                ImGui::TextColored(ImColor(mutedColor), "%zu shown", filteredCount);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(112.0f * uiScale);
                if (ImGui::BeginCombo("##McsrMatchFilter", kMatchFilterLabels[std::clamp(s_matchFilter, 0, 4)])) {
                    for (int idx = 0; idx < 5; ++idx) {
                        const bool selected = (s_matchFilter == idx);
                        if (ImGui::Selectable(kMatchFilterLabels[idx], selected)) { s_matchFilter = idx; }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::Separator();

                if (ImGui::BeginTable("##McsrMatchesTable", 4,
                                      ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV |
                                          ImGuiTableFlags_ScrollY,
                                      ImVec2(0.0f, 0.0f))) {
                    ImGui::TableSetupColumn("Opponent", ImGuiTableColumnFlags_WidthStretch, 0.50f);
                    ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch, 0.18f);
                    ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 0.20f);
                    ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthStretch, 0.12f);
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < snap.recentMatches.size(); ++i) {
                        const auto& row = snap.recentMatches[i];
                        if (!rowMatchesFilter(row)) continue;
                        const ImU32 resultClr = (row.resultType > 0) ? winColor : ((row.resultType < 0) ? lossColor : drawColor);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        const bool canLoadOpponent = !row.opponent.empty() && !equalsIgnoreCase(row.opponent, "Unknown");
                        if (canLoadOpponent) {
                            ImGui::PushStyleColor(ImGuiCol_Text, drawColor);
                            ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(28, 45, 72, 140));
                            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(36, 58, 92, 190));
                            ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(46, 72, 110, 210));
                            const std::string oppLabel = row.opponent + "##McsrMatchOpp" + std::to_string(i);
                            if (ImGui::Selectable(oppLabel.c_str(), false)) {
                                applyPlayerSelection(row.opponent);
                            }
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Load profile"); }
                            ImGui::PopStyleColor(4);
                        } else {
                            ImGui::TextColored(ImColor(bodyColor), "%s", row.opponent.c_str());
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextColored(ImColor(resultClr), "%s", row.resultLabel.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextColored(ImColor(mutedColor), "%s", row.forfeited ? "FORFEIT" : row.detailLabel.c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextColored(ImColor(mutedColor), "%s", row.ageLabel.c_str());
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();

            ImGui::SameLine(0.0f, pad);
            if (ImGui::BeginChild("##McsrGraph", ImVec2(0.0f, 0.0f), true,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                ImGui::TextColored(ImColor(titleColor), "ELO TREND");
                ImGui::SameLine();
                ImGui::TextColored(ImColor(mutedColor), "%d points", static_cast<int>(eloSeries.size()));
                ImGui::Separator();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 graphOrigin = ImGui::GetCursorScreenPos();
                const ImVec2 graphAvail = ImGui::GetContentRegionAvail();
                const float graphBottomReserve = 46.0f * uiScale;
                const std::string axisMaxLabel = std::to_string(maxElo);
                const std::string axisMinLabel = std::to_string(minElo);
                const float axisLabelW = std::max(ImGui::CalcTextSize(axisMaxLabel.c_str()).x, ImGui::CalcTextSize(axisMinLabel.c_str()).x);
                const float leftAxisPad = axisLabelW + (18.0f * uiScale);
                const ImVec2 plotMin(graphOrigin.x + leftAxisPad, graphOrigin.y + 8.0f * uiScale);
                const ImVec2 plotMax(graphOrigin.x + graphAvail.x - 14.0f * uiScale, graphOrigin.y + graphAvail.y - graphBottomReserve);
                const float plotW = std::max(1.0f, plotMax.x - plotMin.x);
                const float plotH = std::max(1.0f, plotMax.y - plotMin.y);
                dl->AddRectFilled(plotMin, plotMax, IM_COL32(13, 20, 32, 255), 4.0f * uiScale);
                dl->AddRect(plotMin, plotMax, IM_COL32(62, 84, 123, static_cast<int>(220.0f * overlayOpacity)), 4.0f * uiScale);

                const int yTicks = 5;
                for (int i = 0; i < yTicks; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(yTicks - 1);
                    const float y = plotMin.y + t * plotH;
                    const int labelValue = static_cast<int>(std::round(static_cast<float>(maxElo) - t * static_cast<float>(maxElo - minElo)));
                    dl->AddLine(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y),
                                IM_COL32(60, 80, 110, static_cast<int>(110.0f * overlayOpacity)), 1.0f);
                    const std::string lbl = std::to_string(labelValue);
                    const ImVec2 lblSize = ImGui::CalcTextSize(lbl.c_str());
                    float lblY = y - (lblSize.y * 0.5f);
                    lblY = std::clamp(lblY, plotMin.y, plotMax.y - lblSize.y);
                    dl->AddText(ImVec2(graphOrigin.x + 4.0f * uiScale, lblY),
                                IM_COL32(156, 172, 204, static_cast<int>(255.0f * overlayOpacity)), lbl.c_str());
                }

                const int count = static_cast<int>(eloSeries.size());
                const int xTicks = std::max(2, std::min(7, count));
                for (int i = 0; i < xTicks; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(xTicks - 1);
                    const float x = plotMin.x + t * plotW;
                    dl->AddLine(ImVec2(x, plotMin.y), ImVec2(x, plotMax.y),
                                IM_COL32(48, 66, 94, static_cast<int>(70.0f * overlayOpacity)), 1.0f);
                }

                if (snap.peakElo > 0) {
                    const float peakNorm =
                        (static_cast<float>(snap.peakElo) - static_cast<float>(minElo)) / static_cast<float>(std::max(1, maxElo - minElo));
                    if (peakNorm >= -0.001f && peakNorm <= 1.001f) {
                        const float peakY = plotMax.y - std::clamp(peakNorm, 0.0f, 1.0f) * plotH;
                        const float dashLen = std::max(4.0f, 7.0f * uiScale);
                        const float gapLen = std::max(3.0f, 5.0f * uiScale);
                        const ImU32 peakLineColor = IM_COL32(236, 184, 96, static_cast<int>(220.0f * overlayOpacity));
                        for (float sx = plotMin.x; sx < plotMax.x; sx += (dashLen + gapLen)) {
                            const float ex = std::min(plotMax.x, sx + dashLen);
                            dl->AddLine(ImVec2(sx, peakY), ImVec2(ex, peakY), peakLineColor, std::max(1.0f, 1.2f * uiScale));
                        }
                        const std::string peakLabel = "Peak " + std::to_string(std::max(0, snap.peakElo));
                        const ImVec2 peakLabelSize = ImGui::CalcTextSize(peakLabel.c_str());
                        const float peakLabelX = std::max(plotMin.x + 6.0f * uiScale, plotMax.x - peakLabelSize.x - (6.0f * uiScale));
                        const float peakLabelY = std::clamp(peakY - peakLabelSize.y - (2.0f * uiScale), plotMin.y + 2.0f * uiScale,
                                                            plotMax.y - peakLabelSize.y - (2.0f * uiScale));
                        dl->AddText(ImVec2(peakLabelX, peakLabelY), peakLineColor, peakLabel.c_str());
                    }
                }

                if (count >= 1) {
                    std::vector<ImVec2> points;
                    points.reserve(static_cast<size_t>(count));
                    const float denom = static_cast<float>(std::max(1, count - 1));
                    for (int i = 0; i < count; ++i) {
                        const float tx = static_cast<float>(i) / denom;
                        const float ty =
                            (eloSeries[static_cast<size_t>(i)] - static_cast<float>(minElo)) / static_cast<float>(std::max(1, maxElo - minElo));
                        points.emplace_back(plotMin.x + tx * plotW, plotMax.y - ty * plotH);
                    }
                    if (count >= 2) {
                        dl->AddPolyline(points.data(), static_cast<int>(points.size()),
                                        IM_COL32(201, 220, 255, static_cast<int>(250.0f * overlayOpacity)), 0,
                                        std::max(1.4f, 2.0f * uiScale));
                    }

                    const ImVec2 mousePos = ImGui::GetIO().MousePos;
                    const bool mouseInPlot =
                        mousePos.x >= plotMin.x && mousePos.x <= plotMax.x && mousePos.y >= plotMin.y && mousePos.y <= plotMax.y;
                    int hoveredPoint = -1;
                    float hoveredDistSq = FLT_MAX;

                    for (size_t i = 0; i < points.size(); ++i) {
                        const float baseR = (i + 1 == points.size()) ? (3.4f * uiScale) : (2.2f * uiScale);
                        const float hitR = std::max(baseR + (4.0f * uiScale), 8.0f * uiScale);
                        if (mouseInPlot) {
                            const float dx = mousePos.x - points[i].x;
                            const float dy = mousePos.y - points[i].y;
                            const float distSq = dx * dx + dy * dy;
                            if (distSq <= hitR * hitR && distSq < hoveredDistSq) {
                                hoveredDistSq = distSq;
                                hoveredPoint = static_cast<int>(i);
                            }
                        }
                    }

                    for (size_t i = 0; i < points.size(); ++i) {
                        const bool isHovered = (hoveredPoint == static_cast<int>(i));
                        const float r =
                            isHovered ? (4.4f * uiScale) : ((i + 1 == points.size()) ? (3.4f * uiScale) : (2.2f * uiScale));
                        const ImU32 clr =
                            isHovered ? IM_COL32(255, 230, 146, static_cast<int>(255.0f * overlayOpacity))
                                      : ((i + 1 == points.size()) ? IM_COL32(114, 214, 255, static_cast<int>(255.0f * overlayOpacity))
                                                                  : IM_COL32(166, 196, 255, static_cast<int>(220.0f * overlayOpacity)));
                        dl->AddCircleFilled(points[i], r, clr);
                    }

                    if (hoveredPoint >= 0 && hoveredPoint < count) {
                        ImGui::BeginTooltip();
                        const int pointElo = static_cast<int>(std::lround(eloSeries[static_cast<size_t>(hoveredPoint)]));
                        ImGui::Text("Match #%d (old -> new)", hoveredPoint + 1);
                        ImGui::Text("ELO: %d", std::max(0, pointElo));
                        if (static_cast<size_t>(hoveredPoint) < snap.eloTrendPoints.size()) {
                            const auto& trend = snap.eloTrendPoints[static_cast<size_t>(hoveredPoint)];
                            if (!trend.opponent.empty()) ImGui::Text("Opp: %s", trend.opponent.c_str());
                            if (!trend.resultLabel.empty() || !trend.detailLabel.empty()) {
                                ImGui::Text("%s  %s", trend.resultLabel.empty() ? "-" : trend.resultLabel.c_str(),
                                            trend.detailLabel.empty() ? "-" : trend.detailLabel.c_str());
                            }
                            if (!trend.ageLabel.empty()) ImGui::Text("Age: %s", trend.ageLabel.c_str());
                        }
                        ImGui::EndTooltip();
                    }
                }

                ImGui::Dummy(ImVec2(0.0f, std::max(10.0f, plotH + (6.0f * uiScale))));
                const int oldestMatchesAgo = std::max(1, std::min(30, count));
                const std::string leftMatchLabel = std::to_string(oldestMatchesAgo) + " matches ago";
                const std::string rightMatchLabel = "last match";
                const ImVec2 labelBase = ImGui::GetCursorScreenPos();
                const ImVec2 rightSize = ImGui::CalcTextSize(rightMatchLabel.c_str());
                dl->AddText(labelBase, IM_COL32(156, 172, 204, static_cast<int>(255.0f * overlayOpacity)), leftMatchLabel.c_str());
                dl->AddText(ImVec2(plotMax.x - rightSize.x, labelBase.y), IM_COL32(156, 172, 204, static_cast<int>(255.0f * overlayOpacity)),
                            rightMatchLabel.c_str());
                ImGui::Dummy(ImVec2(0.0f, std::max(12.0f, rightSize.y + (2.0f * uiScale))));
                ImGui::TextColored(ImColor(bodyColor), "Recent: %dW %dL %dD", std::max(0, snap.recentWins), std::max(0, snap.recentLosses),
                                   std::max(0, snap.recentDraws));
            }
            ImGui::EndChild();
        } else {
            if (ImGui::BeginChild("##McsrCompactBody", ImVec2(0.0f, 0.0f), true)) {
                ImGui::TextColored(ImColor(titleColor), "RECENT: %dW %dL %dD", std::max(0, snap.recentWins),
                                   std::max(0, snap.recentLosses), std::max(0, snap.recentDraws));
                if (!snap.recentMatches.empty()) {
                    const size_t maxRows = std::min<size_t>(6, snap.recentMatches.size());
                    for (size_t i = 0; i < maxRows; ++i) {
                        const auto& row = snap.recentMatches[i];
                        const ImU32 resultClr = (row.resultType > 0) ? winColor : ((row.resultType < 0) ? lossColor : drawColor);
                        const bool canLoadOpponent = !row.opponent.empty() && !equalsIgnoreCase(row.opponent, "Unknown");
                        if (canLoadOpponent) {
                            ImGui::PushStyleColor(ImGuiCol_Text, drawColor);
                            ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(28, 45, 72, 120));
                            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(36, 58, 92, 180));
                            ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(46, 72, 110, 210));
                            const std::string oppLabel = row.opponent + "##McsrCompactOpp" + std::to_string(i);
                            if (ImGui::Selectable(oppLabel.c_str(), false)) {
                                applyPlayerSelection(row.opponent);
                            }
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Load profile"); }
                            ImGui::PopStyleColor(4);
                        } else {
                            ImGui::TextColored(ImColor(bodyColor), "%s", row.opponent.c_str());
                        }
                        ImGui::SameLine();
                        ImGui::TextColored(ImColor(resultClr), "%s", row.resultLabel.c_str());
                        ImGui::SameLine();
                        ImGui::TextColored(ImColor(mutedColor), "%s", row.forfeited ? "FORFEIT" : row.detailLabel.c_str());
                    }
                    ImGui::Separator();
                }

                ImGui::PushStyleColor(ImGuiCol_PlotLines, IM_COL32(198, 214, 248, static_cast<int>(255.0f * overlayOpacity)));
                ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(16, 20, 32, 255));
                ImGui::PlotLines("##McsrCompactPlot", eloSeries.data(), static_cast<int>(eloSeries.size()), 0, nullptr,
                                 static_cast<float>(minElo), static_cast<float>(maxElo), ImVec2(-1.0f, 150.0f * uiScale));
                ImGui::PopStyleColor(2);
            }
            ImGui::EndChild();
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(3);
}

// RENDER THREAD SHADER PROGRAMS
// These shaders are created on the render thread context (not shared with main thread)

static const char* rt_solid_vert_shader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
void main() {
    gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
})";

static const char* rt_passthrough_vert_shader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
})";

static const char* rt_background_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D backgroundTexture;
uniform float u_opacity;
void main() {
    vec4 texColor = texture(backgroundTexture, TexCoord);
    FragColor = vec4(texColor.rgb, texColor.a * u_opacity);
})";

static const char* rt_solid_color_frag_shader = R"(#version 330 core
out vec4 FragColor;
uniform vec4 u_color;
void main() {
    FragColor = u_color;
})";

static const char* rt_image_render_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;

uniform sampler2D imageTexture;
uniform bool u_enableColorKey;
uniform vec3 u_colorKey;
uniform float u_sensitivity;
uniform float u_opacity;

void main() {
    vec4 texColor = texture(imageTexture, TexCoord);

    if (u_enableColorKey) {
        vec3 linearTexColor = pow(texColor.rgb, vec3(2.2));
        vec3 linearKeyColor = pow(u_colorKey, vec3(2.2));
        float dist = distance(linearTexColor, linearKeyColor);
        if (dist < u_sensitivity) {
            discard;
        }
    }
    
    FragColor = vec4(texColor.rgb, texColor.a * u_opacity);
})";

// Static border shader - draws a border shape (rectangle or ellipse)
// Uses SDF (Signed Distance Field) for smooth shape rendering
// The quad is expanded by thickness on each side to accommodate borders
// that extend outside the shape. The shader calculates the shape edge position
// relative to the expanded quad.
static const char* rt_static_border_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform int u_shape;         // 0=Rectangle (with optional rounded corners), 1=Circle/Ellipse
uniform vec4 u_borderColor;
uniform float u_thickness;   // Border thickness in pixels
uniform float u_radius;      // Corner radius for Rectangle in pixels (0 = sharp corners)
uniform vec2 u_size;         // BASE shape size (width/height) - NOT the expanded quad size
uniform vec2 u_quadSize;     // Actual expanded quad size rendered by GPU

// SDF for a rounded rectangle (works for sharp corners when r=0)
float sdRoundedBox(vec2 p, vec2 b, float r) {
    // Clamp radius to not exceed half of the smaller box dimension
    float maxR = min(b.x, b.y);
    r = clamp(r, 0.0, maxR);
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// SDF for an ellipse - proper signed distance approximation
// Uses gradient-based correction for more accurate distance
float sdEllipse(vec2 p, vec2 ab) {
    // Normalize to unit circle space
    vec2 pn = p / ab;
    float len = length(pn);
    if (len < 0.0001) return -min(ab.x, ab.y); // At center
    
    // Distance in normalized space
    float d = len - 1.0;
    
    // Correct for the stretching using the gradient magnitude
    // The gradient of the implicit function f(p) = |p/ab| - 1 is p/(ab^2 * |p/ab|)
    // Its magnitude gives the local scaling factor
    vec2 grad = pn / (ab * len);
    float gradLen = length(grad);
    
    // Scale distance back to pixel space
    return d / gradLen;
}

void main() {
    // Map TexCoord (0-1) to pixel coordinates within the actual GPU quad
    vec2 pixelPos = TexCoord * u_quadSize;
    
    // Offset so (0,0) is at the center of the quad
    vec2 centeredPixelPos = pixelPos - u_quadSize * 0.5;
    
    // Calculate distance in pixels from the shape edge
    // The shape has size u_size, centered at origin
    // Ensure halfSize has a minimum value to avoid degenerate shapes
    vec2 halfSize = max(u_size * 0.5, vec2(1.0, 1.0));
    
    float dist;
    
    if (u_shape == 0) {
        // Rectangle (with optional rounded corners via u_radius)
        dist = sdRoundedBox(centeredPixelPos, halfSize, u_radius);
    } else {
        // Circle/Ellipse
        dist = sdEllipse(centeredPixelPos, halfSize);
    }
    
    // Border is drawn at the shape edge (dist=0) outward to thickness
    float innerEdge = 0.0;
    float outerEdge = u_thickness;
    
    // Add small epsilon for floating-point precision at quad boundaries
    // The SDF approximations can have slight errors, especially for ellipses
    float epsilon = 0.5;
    
    if (dist >= innerEdge - epsilon && dist <= outerEdge + epsilon) {
        FragColor = u_borderColor;
    } else {
        discard;
    }
})";

// Gradient shader for multi-stop linear gradients with angle and animation support
static const char* rt_gradient_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;

#define MAX_STOPS 8
#define ANIM_NONE 0
#define ANIM_ROTATE 1
#define ANIM_SLIDE 2
#define ANIM_WAVE 3
#define ANIM_SPIRAL 4
#define ANIM_FADE 5

uniform int u_numStops;
uniform vec4 u_stopColors[MAX_STOPS];
uniform float u_stopPositions[MAX_STOPS];
uniform float u_angle; // radians (base angle)
uniform float u_time;  // animation time in seconds
uniform int u_animationType;
uniform float u_animationSpeed;
uniform bool u_colorFade;

// Get color at position t (0-1) with seamless wrapping for slide animation
vec4 getGradientColorSeamless(float t) {
    // Wrap t to 0-1 range
    t = fract(t);
    
    // For seamless tiling, we treat the gradient as a loop:
    // The gradient goes from first stop to last stop, then blends back to first
    // We remap t so that the full 0-1 range covers stops AND the wrap-around blend
    
    // Find position in extended gradient (including wrap segment)
    float lastPos = u_stopPositions[u_numStops - 1];
    float firstPos = u_stopPositions[0];
    float wrapSize = (1.0 - lastPos) + firstPos; // Size of wrap-around segment
    
    if (t <= firstPos && wrapSize > 0.001) {
        // In the wrap-around blend zone (before first stop)
        float wrapT = (firstPos - t) / wrapSize;
        return mix(u_stopColors[0], u_stopColors[u_numStops - 1], wrapT);
    }
    else if (t >= lastPos && wrapSize > 0.001) {
        // In the wrap-around blend zone (after last stop)
        float wrapT = (t - lastPos) / wrapSize;
        return mix(u_stopColors[u_numStops - 1], u_stopColors[0], wrapT);
    }
    
    // Normal gradient interpolation between stops
    vec4 color = u_stopColors[0];
    for (int i = 0; i < u_numStops - 1; i++) {
        if (t >= u_stopPositions[i] && t <= u_stopPositions[i + 1]) {
            float segmentT = (t - u_stopPositions[i]) / max(u_stopPositions[i + 1] - u_stopPositions[i], 0.0001);
            color = mix(u_stopColors[i], u_stopColors[i + 1], segmentT);
            break;
        }
    }
    return color;
}

// Get color at position t with optional time-based color cycling
vec4 getGradientColor(float t, float timeOffset) {
    // Apply color fade - shifts all stop positions over time
    float adjustedT = t;
    if (u_colorFade) {
        adjustedT = fract(t + timeOffset * 0.1);
    }
    adjustedT = clamp(adjustedT, 0.0, 1.0);
    
    // Find which segment we're in and interpolate
    vec4 color = u_stopColors[0];
    for (int i = 0; i < u_numStops - 1; i++) {
        if (adjustedT >= u_stopPositions[i] && adjustedT <= u_stopPositions[i + 1]) {
            float segmentT = (adjustedT - u_stopPositions[i]) / max(u_stopPositions[i + 1] - u_stopPositions[i], 0.0001);
            color = mix(u_stopColors[i], u_stopColors[i + 1], segmentT);
            break;
        }
    }
    // Handle edge cases (beyond last stop)
    if (adjustedT >= u_stopPositions[u_numStops - 1]) {
        color = u_stopColors[u_numStops - 1];
    }
    return color;
}

// Get solid color that cycles through gradient stops over time
vec4 getFadeColor(float timeOffset) {
    // Cycle through stops: time maps to position in color sequence
    float cyclePos = fract(timeOffset * 0.1); // Speed of cycling
    
    // Find which segment we're in and interpolate smoothly
    vec4 color = u_stopColors[0];
    for (int i = 0; i < u_numStops - 1; i++) {
        if (cyclePos >= u_stopPositions[i] && cyclePos <= u_stopPositions[i + 1]) {
            float segmentT = (cyclePos - u_stopPositions[i]) / max(u_stopPositions[i + 1] - u_stopPositions[i], 0.0001);
            color = mix(u_stopColors[i], u_stopColors[i + 1], segmentT);
            break;
        }
    }
    // Wrap around: blend from last color back to first
    if (cyclePos > u_stopPositions[u_numStops - 1]) {
        float wrapRange = 1.0 - u_stopPositions[u_numStops - 1] + u_stopPositions[0];
        float wrapT = (cyclePos - u_stopPositions[u_numStops - 1]) / max(wrapRange, 0.0001);
        color = mix(u_stopColors[u_numStops - 1], u_stopColors[0], wrapT);
    }
    else if (cyclePos < u_stopPositions[0]) {
        float wrapRange = 1.0 - u_stopPositions[u_numStops - 1] + u_stopPositions[0];
        float wrapT = (u_stopPositions[0] - cyclePos) / max(wrapRange, 0.0001);
        color = mix(u_stopColors[0], u_stopColors[u_numStops - 1], wrapT);
    }
    return color;
}

void main() {
    vec2 center = vec2(0.5, 0.5);
    vec2 uv = TexCoord - center;
    float effectiveAngle = u_angle;
    float t = 0.0;
    float timeOffset = u_time * u_animationSpeed;
    
    if (u_animationType == ANIM_NONE) {
        // Static gradient - original behavior
        vec2 dir = vec2(cos(u_angle), sin(u_angle));
        t = dot(uv, dir) + 0.5;
        t = clamp(t, 0.0, 1.0);
        FragColor = getGradientColor(t, timeOffset);
    }
    else if (u_animationType == ANIM_ROTATE) {
        // Rotating gradient - angle changes over time
        effectiveAngle = u_angle + timeOffset;
        vec2 dir = vec2(cos(effectiveAngle), sin(effectiveAngle));
        t = dot(uv, dir) + 0.5;
        t = clamp(t, 0.0, 1.0);
        FragColor = getGradientColor(t, timeOffset);
    }
    else if (u_animationType == ANIM_SLIDE) {
        // Sliding gradient - seamless scrolling along the gradient direction
        vec2 dir = vec2(cos(u_angle), sin(u_angle));
        t = dot(uv, dir) + 0.5;
        t = t + timeOffset * 0.2; // Shift position over time
        FragColor = getGradientColorSeamless(t);
    }
    else if (u_animationType == ANIM_WAVE) {
        // Wave distortion - sine wave applied to gradient
        vec2 dir = vec2(cos(u_angle), sin(u_angle));
        vec2 perpDir = vec2(-sin(u_angle), cos(u_angle));
        float perpPos = dot(uv, perpDir);
        float wave = sin(perpPos * 8.0 + timeOffset * 2.0) * 0.08;
        t = dot(uv, dir) + 0.5 + wave;
        t = clamp(t, 0.0, 1.0);
        FragColor = getGradientColor(t, timeOffset);
    }
    else if (u_animationType == ANIM_SPIRAL) {
        // Spiral effect - colors spiral outward from center
        float dist = length(uv) * 2.0;
        float angle = atan(uv.y, uv.x);
        t = dist + angle / 6.28318 - timeOffset * 0.3;
        FragColor = getGradientColorSeamless(t);
    }
    else if (u_animationType == ANIM_FADE) {
        // Fade - solid color that smoothly cycles through all gradient stops
        FragColor = getFadeColor(timeOffset);
    }
    else {
        t = clamp(t, 0.0, 1.0);
        FragColor = getGradientColor(t, timeOffset);
    }
})";

// NOTE: Border rendering shaders (brute force and JFA) have been removed from render_thread.
// All border rendering is now done by mirror_thread.cpp which has its own local shader programs.
// Render thread just blits the pre-rendered finalTexture using the passthrough/background shader.

// RGBA->NV12 compute shader using Rec. 709 coefficients
// Reads from a sampler2D, writes NV12 (Y plane + interleaved UV plane) to an SSBO
// Optimized NV12 compute shader: writes Y plane as r8ui image (no atomics)
// UV plane is written to a separate r8ui image by even-coordinate threads only
static const char* rt_nv12_compute_shader = R"(
#version 430
layout(local_size_x = 16, local_size_y = 16) in;

uniform sampler2D u_rgbaTexture;
uniform uint u_width;
uniform uint u_height;

// Y plane: width x height, each pixel is one luma byte
layout(r8ui, binding = 0) uniform writeonly uimage2D u_yPlane;
// UV plane: width x (height/2), interleaved U,V pairs stored as bytes
layout(r8ui, binding = 1) uniform writeonly uimage2D u_uvPlane;

void main() {
    uvec2 pos = gl_GlobalInvocationID.xy;
    if (pos.x >= u_width || pos.y >= u_height) return;

    // Flip Y: OpenGL bottom-up -> NV12 top-down
    uint srcY = u_height - 1u - pos.y;
    vec4 rgba = texelFetch(u_rgbaTexture, ivec2(pos.x, srcY), 0);

    // Rec. 709 RGB->Y (limited range 16-235)
    float Y = 0.1826 * rgba.r + 0.6142 * rgba.g + 0.0620 * rgba.b + 0.0625;
    imageStore(u_yPlane, ivec2(pos.x, pos.y), uvec4(uint(clamp(Y * 255.0, 0.0, 255.0)), 0u, 0u, 0u));

    // UV plane: only even-coordinate threads (2x2 subsampling)
    if ((pos.x & 1u) == 0u && (pos.y & 1u) == 0u) {
        // Average 2x2 block for chroma
        vec4 p10 = texelFetch(u_rgbaTexture, ivec2(pos.x + 1u, srcY), 0);
        vec4 p01 = texelFetch(u_rgbaTexture, ivec2(pos.x, srcY - 1u), 0);
        vec4 p11 = texelFetch(u_rgbaTexture, ivec2(pos.x + 1u, srcY - 1u), 0);
        vec4 avg = (rgba + p10 + p01 + p11) * 0.25;

        // Rec. 709 RGB->Cb,Cr (limited range 16-240)
        float U = -0.1006 * avg.r - 0.3386 * avg.g + 0.4392 * avg.b + 0.5;
        float V =  0.4392 * avg.r - 0.3989 * avg.g - 0.0403 * avg.b + 0.5;

        // UV plane: row = pos.y/2, columns = pos.x (U) and pos.x+1 (V)
        uint uvRow = pos.y >> 1u;
        imageStore(u_uvPlane, ivec2(pos.x, uvRow), uvec4(uint(clamp(U * 255.0, 0.0, 255.0)), 0u, 0u, 0u));
        imageStore(u_uvPlane, ivec2(pos.x + 1u, uvRow), uvec4(uint(clamp(V * 255.0, 0.0, 255.0)), 0u, 0u, 0u));
    }
}
)";

static GLuint rt_backgroundProgram = 0;
static GLuint rt_solidColorProgram = 0;
static GLuint rt_imageRenderProgram = 0;
static GLuint rt_staticBorderProgram = 0;
static GLuint rt_gradientProgram = 0;

struct RT_BackgroundShaderLocs {
    GLint backgroundTexture = -1;
    GLint opacity = -1;
};

struct RT_SolidColorShaderLocs {
    GLint color = -1;
};

struct RT_ImageRenderShaderLocs {
    GLint imageTexture = -1;
    GLint enableColorKey = -1;
    GLint colorKey = -1;
    GLint sensitivity = -1;
    GLint opacity = -1;
};

struct RT_StaticBorderShaderLocs {
    GLint shape = -1, borderColor = -1, thickness = -1, radius = -1, size = -1, quadSize = -1;
};

struct RT_GradientShaderLocs {
    GLint numStops = -1;
    GLint stopColors = -1;
    GLint stopPositions = -1;
    GLint angle = -1;
    GLint time = -1;
    GLint animationType = -1;
    GLint animationSpeed = -1;
    GLint colorFade = -1;
};

static RT_BackgroundShaderLocs rt_backgroundShaderLocs;
static RT_SolidColorShaderLocs rt_solidColorShaderLocs;
static RT_ImageRenderShaderLocs rt_imageRenderShaderLocs;
static RT_StaticBorderShaderLocs rt_staticBorderShaderLocs;
static RT_GradientShaderLocs rt_gradientShaderLocs;

static GLuint RT_CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, 512, NULL, log);
        Log("RenderThread: Shader compile failed: " + std::string(log));
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint RT_CreateShaderProgram(const char* vert, const char* frag) {
    GLuint v = RT_CompileShader(GL_VERTEX_SHADER, vert);
    GLuint f = RT_CompileShader(GL_FRAGMENT_SHADER, frag);
    if (v == 0 || f == 0) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, 512, NULL, log);
        Log("RenderThread: Shader link failed: " + std::string(log));
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

// Create a compute shader program from a single compute shader source
static GLuint RT_CreateComputeProgram(const char* src) {
    GLuint cs = RT_CompileShader(GL_COMPUTE_SHADER, src);
    if (cs == 0) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, cs);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, 512, NULL, log);
        Log("RenderThread: Compute shader link failed: " + std::string(log));
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(cs);
    return p;
}

static bool RT_InitializeShaders() {
    LogCategory("init", "RenderThread: Initializing shaders...");

    // NOTE: Border rendering shaders have been removed - all border rendering is done by mirror_thread
    // Render thread only needs: background (for mirror blitting), solid color (for game borders), image render, static border, and gradient
    rt_backgroundProgram = RT_CreateShaderProgram(rt_passthrough_vert_shader, rt_background_frag_shader);
    rt_solidColorProgram = RT_CreateShaderProgram(rt_solid_vert_shader, rt_solid_color_frag_shader);
    rt_imageRenderProgram = RT_CreateShaderProgram(rt_passthrough_vert_shader, rt_image_render_frag_shader);
    rt_staticBorderProgram = RT_CreateShaderProgram(rt_passthrough_vert_shader, rt_static_border_frag_shader);
    rt_gradientProgram = RT_CreateShaderProgram(rt_passthrough_vert_shader, rt_gradient_frag_shader);

    if (!rt_backgroundProgram || !rt_solidColorProgram || !rt_imageRenderProgram || !rt_staticBorderProgram || !rt_gradientProgram) {
        Log("RenderThread: FATAL - Failed to create shader programs");
        return false;
    }

    // Try to compile NV12 compute shader (requires GL 4.3 / ARB_compute_shader + image load/store)
    if (GLEW_ARB_compute_shader && GLEW_ARB_shader_image_load_store) {
        g_vcComputeProgram = RT_CreateComputeProgram(rt_nv12_compute_shader);
        if (g_vcComputeProgram) {
            g_vcUseCompute = true;
            // Cache uniform locations once
            g_vcLocRgbaTexture = glGetUniformLocation(g_vcComputeProgram, "u_rgbaTexture");
            g_vcLocWidth = glGetUniformLocation(g_vcComputeProgram, "u_width");
            g_vcLocHeight = glGetUniformLocation(g_vcComputeProgram, "u_height");
            LogCategory("init", "RenderThread: NV12 compute shader compiled successfully (Rec. 709, image2D path)");
        } else {
            Log("RenderThread: NV12 compute shader failed, falling back to CPU conversion");
            g_vcUseCompute = false;
        }
    } else {
        Log("RenderThread: Compute shaders not supported, using CPU NV12 conversion");
        g_vcUseCompute = false;
    }

    // Get uniform locations
    rt_backgroundShaderLocs.backgroundTexture = glGetUniformLocation(rt_backgroundProgram, "backgroundTexture");
    rt_backgroundShaderLocs.opacity = glGetUniformLocation(rt_backgroundProgram, "u_opacity");

    rt_solidColorShaderLocs.color = glGetUniformLocation(rt_solidColorProgram, "u_color");

    // Static border uniforms
    rt_staticBorderShaderLocs.shape = glGetUniformLocation(rt_staticBorderProgram, "u_shape");
    rt_staticBorderShaderLocs.borderColor = glGetUniformLocation(rt_staticBorderProgram, "u_borderColor");
    rt_staticBorderShaderLocs.thickness = glGetUniformLocation(rt_staticBorderProgram, "u_thickness");
    rt_staticBorderShaderLocs.radius = glGetUniformLocation(rt_staticBorderProgram, "u_radius");
    rt_staticBorderShaderLocs.size = glGetUniformLocation(rt_staticBorderProgram, "u_size");
    rt_staticBorderShaderLocs.quadSize = glGetUniformLocation(rt_staticBorderProgram, "u_quadSize");

    rt_imageRenderShaderLocs.imageTexture = glGetUniformLocation(rt_imageRenderProgram, "imageTexture");
    rt_imageRenderShaderLocs.enableColorKey = glGetUniformLocation(rt_imageRenderProgram, "u_enableColorKey");
    rt_imageRenderShaderLocs.colorKey = glGetUniformLocation(rt_imageRenderProgram, "u_colorKey");
    rt_imageRenderShaderLocs.sensitivity = glGetUniformLocation(rt_imageRenderProgram, "u_sensitivity");
    rt_imageRenderShaderLocs.opacity = glGetUniformLocation(rt_imageRenderProgram, "u_opacity");

    // Gradient shader uniforms
    rt_gradientShaderLocs.numStops = glGetUniformLocation(rt_gradientProgram, "u_numStops");
    rt_gradientShaderLocs.stopColors = glGetUniformLocation(rt_gradientProgram, "u_stopColors");
    rt_gradientShaderLocs.stopPositions = glGetUniformLocation(rt_gradientProgram, "u_stopPositions");
    rt_gradientShaderLocs.angle = glGetUniformLocation(rt_gradientProgram, "u_angle");
    rt_gradientShaderLocs.time = glGetUniformLocation(rt_gradientProgram, "u_time");
    rt_gradientShaderLocs.animationType = glGetUniformLocation(rt_gradientProgram, "u_animationType");
    rt_gradientShaderLocs.animationSpeed = glGetUniformLocation(rt_gradientProgram, "u_animationSpeed");
    rt_gradientShaderLocs.colorFade = glGetUniformLocation(rt_gradientProgram, "u_colorFade");

    // Set texture sampler uniforms once
    glUseProgram(rt_backgroundProgram);
    glUniform1i(rt_backgroundShaderLocs.backgroundTexture, 0);
    glUniform1f(rt_backgroundShaderLocs.opacity, 1.0f);

    glUseProgram(rt_imageRenderProgram);
    glUniform1i(rt_imageRenderShaderLocs.imageTexture, 0);

    glUseProgram(0);

    LogCategory("init", "RenderThread: Shaders initialized successfully");
    return true;
}

static void RT_CleanupShaders() {
    if (rt_backgroundProgram) {
        glDeleteProgram(rt_backgroundProgram);
        rt_backgroundProgram = 0;
    }
    if (rt_solidColorProgram) {
        glDeleteProgram(rt_solidColorProgram);
        rt_solidColorProgram = 0;
    }
    if (rt_imageRenderProgram) {
        glDeleteProgram(rt_imageRenderProgram);
        rt_imageRenderProgram = 0;
    }
    if (rt_gradientProgram) {
        glDeleteProgram(rt_gradientProgram);
        rt_gradientProgram = 0;
    }
}

// Render cursor for OBS/Virtual Camera output
// This renders the current system cursor at the correct position relative to the game viewport
// Supports windowed mode where game content is centered with black borders
// Only renders when the cursor is visible
// Parameters:
//   fullW, fullH - Full output dimensions (virtual camera frame size)
//   viewportX, viewportY - Position of game content in output (top-left corner)
//   viewportW, viewportH - Size of game content in output
//   windowW, windowH - Actual game window dimensions for coordinate mapping
//   vao, vbo - OpenGL vertex array/buffer objects for rendering
static void RT_RenderCursorForObs(int fullW, int fullH, int viewportX, int viewportY, int viewportW, int viewportH, int windowW,
                                  int windowH, GLuint vao, GLuint vbo) {
    // Check if cursor is visible (game state)
    if (!IsCursorVisible()) { return; }

    // Get current cursor info
    CURSORINFO cursorInfo = { 0 };
    cursorInfo.cbSize = sizeof(CURSORINFO);
    if (!GetCursorInfo(&cursorInfo)) { return; }
    if (!cursorInfo.hCursor) { return; }
    if (!(cursorInfo.flags & CURSOR_SHOWING)) { return; }

    // Get cursor data - try to find existing or create from handle
    const CursorTextures::CursorData* cursorData = CursorTextures::LoadOrFindCursorFromHandle(cursorInfo.hCursor);
    if (!cursorData || cursorData->texture == 0) { return; }

    // Get cursor position (screen coordinates)
    POINT cursorPos = cursorInfo.ptScreenPos;

    // Convert screen position to window client coordinates
    HWND hwnd = g_minecraftHwnd.load();
    if (hwnd) { ScreenToClient(hwnd, &cursorPos); }

    // In windowed mode, skip rendering if cursor is outside the window bounds (over black bars)
    if (windowW > 0 && windowH > 0) {
        if (cursorPos.x < 0 || cursorPos.x >= windowW || cursorPos.y < 0 || cursorPos.y >= windowH) { return; }
    }

    // Calculate scaling from window space to viewport space
    // This handles the case where game content is scaled to fit the viewport
    float scaleX = (viewportW > 0 && windowW > 0) ? static_cast<float>(viewportW) / windowW : 1.0f;
    float scaleY = (viewportH > 0 && windowH > 0) ? static_cast<float>(viewportH) / windowH : 1.0f;

    // Transform cursor position from window client coordinates to virtual camera output coordinates
    // Step 1: Scale cursor position by viewport/window ratio
    // Step 2: Add viewport offset to position cursor relative to centered game content
    int renderX = viewportX + static_cast<int>((cursorPos.x - cursorData->hotspotX) * scaleX);
    int renderY = viewportY + static_cast<int>((cursorPos.y - cursorData->hotspotY) * scaleY);

    // Scale cursor size to match the game content scaling
    int renderW = static_cast<int>(cursorData->bitmapWidth * scaleX);
    int renderH = static_cast<int>(cursorData->bitmapHeight * scaleY);

    // Ensure minimum cursor size of 1 pixel
    if (renderW < 1) renderW = 1;
    if (renderH < 1) renderH = 1;

    // Skip if cursor is completely outside bounds
    if (renderX + renderW < 0 || renderX >= fullW || renderY + renderH < 0 || renderY >= fullH) { return; }

    // Use the image render shader to draw cursor texture
    glUseProgram(rt_imageRenderProgram);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    // Enable alpha blending for cursor transparency
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Bind cursor texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, cursorData->texture);
    glUniform1i(rt_imageRenderShaderLocs.imageTexture, 0);
    glUniform1i(rt_imageRenderShaderLocs.enableColorKey, false);
    glUniform1f(rt_imageRenderShaderLocs.opacity, 1.0f);

    // Convert pixel coordinates to NDC (Y needs to be flipped for OpenGL)
    // Window coords: Y=0 at top, Y increases downward
    // OpenGL NDC: Y=-1 at bottom, Y=1 at top
    float left = (static_cast<float>(renderX) / fullW) * 2.0f - 1.0f;
    float right = (static_cast<float>(renderX + renderW) / fullW) * 2.0f - 1.0f;
    float top = 1.0f - (static_cast<float>(renderY) / fullH) * 2.0f;
    float bottom = 1.0f - (static_cast<float>(renderY + renderH) / fullH) * 2.0f;

    // Create quad with texture coordinates
    // Format: x, y, u, v (matching vertex layout)
    float cursorQuad[] = {
        left,  bottom, 0.0f, 1.0f, // Bottom-left
        right, bottom, 1.0f, 1.0f, // Bottom-right
        right, top,    1.0f, 0.0f, // Top-right
        left,  bottom, 0.0f, 1.0f, // Bottom-left
        right, top,    1.0f, 0.0f, // Top-right
        left,  top,    0.0f, 0.0f  // Top-left
    };

    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(cursorQuad), cursorQuad);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Render inverted pixels if cursor has them (for monochrome cursors)
    if (cursorData->hasInvertedPixels && cursorData->invertMaskTexture != 0) {
        glBindTexture(GL_TEXTURE_2D, cursorData->invertMaskTexture);
        // Use XOR blend function to invert background colors
        glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        // Restore normal blending
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
}

// Render a border around an element using the render thread's shaders
// This mirrors RenderGameBorder() from render.cpp but uses render thread resources
static void RT_RenderGameBorder(int x, int y, int w, int h, int borderWidth, int radius, const Color& color, int fullW, int fullH,
                                GLuint vao, GLuint vbo) {
    if (borderWidth <= 0) return;

    glUseProgram(rt_solidColorProgram);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUniform4f(rt_solidColorShaderLocs.color, color.r, color.g, color.b, 1.0f);

    // Convert window coordinates to GL coordinates (Y-flip)
    int y_gl = fullH - y - h;

    // The border extends OUTSIDE the element
    int outerLeft = x - borderWidth;
    int outerRight = x + w + borderWidth;
    int outerBottom = y_gl - borderWidth;
    int outerTop = y_gl + h + borderWidth;

    // Clamp radius to valid range
    int effectiveRadius = radius;
    int maxRadius = (w < h ? w : h) / 2 + borderWidth;
    if (effectiveRadius > maxRadius) effectiveRadius = maxRadius;

    // Helper to convert pixel coords to NDC
    auto toNdcX = [fullW](int px) { return (static_cast<float>(px) / fullW) * 2.0f - 1.0f; };
    auto toNdcY = [fullH](int py) { return (static_cast<float>(py) / fullH) * 2.0f - 1.0f; };

    // Sharp corners: render 4 border rectangles
    // (Rounded corners would require more complex rendering, keeping it simple for now)
    // Top border
    float topBorder[] = { toNdcX(outerLeft),  toNdcY(y_gl + h), 0, 0, toNdcX(outerRight), toNdcY(y_gl + h), 0, 0,
                          toNdcX(outerRight), toNdcY(outerTop), 0, 0, toNdcX(outerLeft),  toNdcY(y_gl + h), 0, 0,
                          toNdcX(outerRight), toNdcY(outerTop), 0, 0, toNdcX(outerLeft),  toNdcY(outerTop), 0, 0 };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(topBorder), topBorder);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Bottom border
    float bottomBorder[] = { toNdcX(outerLeft),  toNdcY(outerBottom), 0, 0, toNdcX(outerRight), toNdcY(outerBottom), 0, 0,
                             toNdcX(outerRight), toNdcY(y_gl),        0, 0, toNdcX(outerLeft),  toNdcY(outerBottom), 0, 0,
                             toNdcX(outerRight), toNdcY(y_gl),        0, 0, toNdcX(outerLeft),  toNdcY(y_gl),        0, 0 };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(bottomBorder), bottomBorder);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Left border
    float leftBorder[] = { toNdcX(outerLeft), toNdcY(y_gl),     0, 0, toNdcX(x),         toNdcY(y_gl),     0, 0,
                           toNdcX(x),         toNdcY(y_gl + h), 0, 0, toNdcX(outerLeft), toNdcY(y_gl),     0, 0,
                           toNdcX(x),         toNdcY(y_gl + h), 0, 0, toNdcX(outerLeft), toNdcY(y_gl + h), 0, 0 };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(leftBorder), leftBorder);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Right border
    float rightBorder[] = { toNdcX(x + w),      toNdcY(y_gl),     0, 0, toNdcX(outerRight), toNdcY(y_gl),     0, 0,
                            toNdcX(outerRight), toNdcY(y_gl + h), 0, 0, toNdcX(x + w),      toNdcY(y_gl),     0, 0,
                            toNdcX(outerRight), toNdcY(y_gl + h), 0, 0, toNdcX(x + w),      toNdcY(y_gl + h), 0, 0 };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(rightBorder), rightBorder);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

// Render background using stencil buffer - draws only in letterbox area (outside game viewport)
// Uses stencil to mask out the viewport area, then draws background to the remaining area
static void RT_RenderBackground(bool isImage, GLuint bgTexture, float bgR, float bgG, float bgB, float opacity, int viewportX,
                                int viewportY, int viewportW, int viewportH, int letterboxExtendX, int letterboxExtendY, int fullW,
                                int fullH, GLuint vao, GLuint vbo) {
    // Skip if mode is fullscreen (no letterbox area to render)
    if (viewportX == 0 && viewportY == 0 && viewportW == fullW && viewportH == fullH) return;

    // Calculate viewport in GL coordinates (Y-flip)
    int viewportY_gl = fullH - viewportY - viewportH;

    // Save GL state
    GLboolean scissorEnabled;
    glGetBooleanv(GL_SCISSOR_TEST, &scissorEnabled);

    // === Step 1: Write viewport area to stencil buffer ===
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glClear(GL_STENCIL_BUFFER_BIT);

    // Configure stencil to write 1 wherever we draw (the viewport area)
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    // Draw a quad covering the viewport area (writes 1 to stencil)
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE); // Don't write to color buffer
    glUseProgram(rt_solidColorProgram);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    // Calculate NDC for viewport quad (shrink inward so background extends closer to game)
    float vpNx1 = (static_cast<float>(viewportX + letterboxExtendX) / fullW) * 2.0f - 1.0f;
    float vpNx2 = (static_cast<float>(viewportX + viewportW - letterboxExtendX) / fullW) * 2.0f - 1.0f;
    float vpNy1 = (static_cast<float>(viewportY_gl + letterboxExtendY) / fullH) * 2.0f - 1.0f;
    float vpNy2 = (static_cast<float>(viewportY_gl + viewportH - letterboxExtendY) / fullH) * 2.0f - 1.0f;

    float stencilQuad[] = { vpNx1, vpNy1, 0, 0, vpNx2, vpNy1, 0, 0, vpNx2, vpNy2, 0, 0,
                            vpNx1, vpNy1, 0, 0, vpNx2, vpNy2, 0, 0, vpNx1, vpNy2, 0, 0 };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(stencilQuad), stencilQuad);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // === Step 2: Draw fullscreen background where stencil == 0 (outside viewport) ===
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); // Restore color writing
    glStencilMask(0x00);                             // Don't modify stencil anymore
    glStencilFunc(GL_EQUAL, 0, 0xFF);                // Only draw where stencil == 0

    if (opacity < 1.0f) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }

    if (isImage && bgTexture != 0) {
        // Render background image
        glUseProgram(rt_backgroundProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, bgTexture);
        glUniform1i(rt_backgroundShaderLocs.backgroundTexture, 0);
        glUniform1f(rt_backgroundShaderLocs.opacity, opacity);
    } else {
        // Render solid color background
        glUseProgram(rt_solidColorProgram);
        glUniform4f(rt_solidColorShaderLocs.color, bgR, bgG, bgB, opacity);
    }

    // Draw fullscreen quad
    float fullscreenQuad[] = { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,  1.0f, 1.0f, 1.0f,
                               -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(fullscreenQuad), fullscreenQuad);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // === Cleanup ===
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);

    // Restore scissor state
    if (scissorEnabled) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

static void InitRenderFBOs(int width, int height) {
    // Initialize main overlay FBOs
    for (int i = 0; i < RENDER_THREAD_FBO_COUNT; i++) {
        RenderFBO& fbo = g_renderFBOs[i];

        // Create FBO if not exists
        if (fbo.fbo == 0) { glGenFramebuffers(1, &fbo.fbo); }

        // Create or resize texture
        if (fbo.texture == 0) { glGenTextures(1, &fbo.texture); }

        // Create stencil renderbuffer if not exists
        if (fbo.stencilRbo == 0) { glGenRenderbuffers(1, &fbo.stencilRbo); }

        if (fbo.width != width || fbo.height != height) {
            glBindTexture(GL_TEXTURE_2D, fbo.texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            // Create stencil renderbuffer
            glBindRenderbuffer(GL_RENDERBUFFER, fbo.stencilRbo);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);

            // Attach texture and stencil to FBO
            glBindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo.texture, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo.stencilRbo);

            // Check FBO completeness
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                Log("RenderThread: FBO " + std::to_string(i) + " incomplete: " + std::to_string(status));
            }

            fbo.width = width;
            fbo.height = height;
            LogCategory("init", "RenderThread: Initialized FBO " + std::to_string(i) + " at " + std::to_string(width) + "x" +
                                    std::to_string(height));
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // Initialize OBS animated frame FBOs
    for (int i = 0; i < RENDER_THREAD_FBO_COUNT; i++) {
        RenderFBO& fbo = g_obsRenderFBOs[i];

        if (fbo.fbo == 0) { glGenFramebuffers(1, &fbo.fbo); }

        if (fbo.texture == 0) { glGenTextures(1, &fbo.texture); }

        // Create stencil renderbuffer if not exists
        if (fbo.stencilRbo == 0) { glGenRenderbuffers(1, &fbo.stencilRbo); }

        if (fbo.width != width || fbo.height != height) {
            glBindTexture(GL_TEXTURE_2D, fbo.texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            // Create stencil renderbuffer
            glBindRenderbuffer(GL_RENDERBUFFER, fbo.stencilRbo);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);

            glBindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo.texture, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo.stencilRbo);

            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                Log("RenderThread: OBS FBO " + std::to_string(i) + " incomplete: " + std::to_string(status));
            }

            fbo.width = width;
            fbo.height = height;
            LogCategory("init", "RenderThread: Initialized OBS FBO " + std::to_string(i) + " at " + std::to_string(width) + "x" +
                                    std::to_string(height));
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

static void CleanupRenderFBOs() {
    // Cleanup main FBOs
    for (int i = 0; i < RENDER_THREAD_FBO_COUNT; i++) {
        RenderFBO& fbo = g_renderFBOs[i];
        if (fbo.fbo != 0) {
            glDeleteFramebuffers(1, &fbo.fbo);
            fbo.fbo = 0;
        }
        if (fbo.texture != 0) {
            glDeleteTextures(1, &fbo.texture);
            fbo.texture = 0;
        }
        if (fbo.stencilRbo != 0) {
            glDeleteRenderbuffers(1, &fbo.stencilRbo);
            fbo.stencilRbo = 0;
        }
        if (fbo.gpuFence != nullptr) {
            glDeleteSync(fbo.gpuFence);
            fbo.gpuFence = nullptr;
        }
        fbo.width = 0;
        fbo.height = 0;
        fbo.ready.store(false);
    }

    // Cleanup OBS FBOs
    for (int i = 0; i < RENDER_THREAD_FBO_COUNT; i++) {
        RenderFBO& fbo = g_obsRenderFBOs[i];
        if (fbo.fbo != 0) {
            glDeleteFramebuffers(1, &fbo.fbo);
            fbo.fbo = 0;
        }
        if (fbo.texture != 0) {
            glDeleteTextures(1, &fbo.texture);
            fbo.texture = 0;
        }
        if (fbo.stencilRbo != 0) {
            glDeleteRenderbuffers(1, &fbo.stencilRbo);
            fbo.stencilRbo = 0;
        }
        if (fbo.gpuFence != nullptr) {
            glDeleteSync(fbo.gpuFence);
            fbo.gpuFence = nullptr;
        }
        fbo.width = 0;
        fbo.height = 0;
        fbo.ready.store(false);
    }

    // Cleanup Virtual Camera resources
    if (g_virtualCamPBO != 0) {
        glDeleteBuffers(1, &g_virtualCamPBO);
        g_virtualCamPBO = 0;
    }
    if (g_virtualCamCopyFBO != 0) {
        glDeleteFramebuffers(1, &g_virtualCamCopyFBO);
        g_virtualCamCopyFBO = 0;
    }
    g_virtualCamPBOWidth = 0;
    g_virtualCamPBOHeight = 0;
    g_virtualCamPBOPending = false;

    // Cleanup GPU compute path resources
    for (int i = 0; i < 2; i++) {
        if (g_vcYImage[i] != 0) {
            glDeleteTextures(1, &g_vcYImage[i]);
            g_vcYImage[i] = 0;
        }
        if (g_vcUVImage[i] != 0) {
            glDeleteTextures(1, &g_vcUVImage[i]);
            g_vcUVImage[i] = 0;
        }
        if (g_vcReadbackPBO[i] != 0) {
            glDeleteBuffers(1, &g_vcReadbackPBO[i]);
            g_vcReadbackPBO[i] = 0;
        }
    }
    if (g_vcReadbackFBO != 0) {
        glDeleteFramebuffers(1, &g_vcReadbackFBO);
        g_vcReadbackFBO = 0;
    }
    if (g_vcFence) {
        glDeleteSync(g_vcFence);
        g_vcFence = nullptr;
    }
    if (g_vcScaleFBO != 0) {
        glDeleteFramebuffers(1, &g_vcScaleFBO);
        g_vcScaleFBO = 0;
    }
    if (g_vcScaleTexture != 0) {
        glDeleteTextures(1, &g_vcScaleTexture);
        g_vcScaleTexture = 0;
    }
    g_vcOutWidth = 0;
    g_vcOutHeight = 0;
    g_vcComputePending = false;
    g_vcReadbackPending = false;

    // Cleanup virtual camera cursor staging resources
    if (g_vcCursorFBO != 0) {
        glDeleteFramebuffers(1, &g_vcCursorFBO);
        g_vcCursorFBO = 0;
    }
    if (g_vcCursorTexture != 0) {
        glDeleteTextures(1, &g_vcCursorTexture);
        g_vcCursorTexture = 0;
    }
    g_vcCursorWidth = 0;
    g_vcCursorHeight = 0;

    RT_ClearMcsrTextureCacheEntry(g_mcsrAvatarTextureCache);
    RT_ClearMcsrTextureCacheEntry(g_mcsrFlagTextureCache);
}

// Advance to next write FBO (called after completing a frame)
static void AdvanceWriteFBO() {
    int current = g_writeFBOIndex.load();
    int next = (current + 1) % RENDER_THREAD_FBO_COUNT;

    // Mark current as ready for reading
    g_renderFBOs[current].ready.store(true, std::memory_order_release);
    g_readFBOIndex.store(current, std::memory_order_release);

    // Move to next write slot
    g_writeFBOIndex.store(next);

    // Mark new write slot as not ready (we're about to overwrite it)
    g_renderFBOs[next].ready.store(false, std::memory_order_release);
}

// Advance to next OBS animated frame write FBO
static void AdvanceObsFBO() {
    int current = g_obsWriteFBOIndex.load();
    int next = (current + 1) % RENDER_THREAD_FBO_COUNT;

    g_obsRenderFBOs[current].ready.store(true, std::memory_order_release);
    g_obsReadFBOIndex.store(current, std::memory_order_release);
    g_obsWriteFBOIndex.store(next);
    g_obsRenderFBOs[next].ready.store(false, std::memory_order_release);
}

// Apply resolution scale to get the virtual camera output dimensions
static void GetVirtualCamScaledSize(int srcW, int srcH, float scale, int& outW, int& outH) {
    outW = static_cast<int>(srcW * scale);
    outH = static_cast<int>(srcH * scale);
    // Ensure even dimensions (required for NV12)
    outW = (outW + 1) & ~1;
    outH = (outH + 1) & ~1;
    // Minimum 64x64
    if (outW < 64) outW = 64;
    if (outH < 64) outH = 64;
}

// Ensure the downscale FBO/texture exist at the right size
static void EnsureVCScaleResources(int w, int h) {
    if (g_vcScaleWidth == w && g_vcScaleHeight == h && g_vcScaleFBO != 0) return;

    if (g_vcScaleFBO == 0) glGenFramebuffers(1, &g_vcScaleFBO);
    if (g_vcScaleTexture != 0) glDeleteTextures(1, &g_vcScaleTexture);
    glGenTextures(1, &g_vcScaleTexture);
    glBindTexture(GL_TEXTURE_2D, g_vcScaleTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, g_vcScaleFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vcScaleTexture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    g_vcScaleWidth = w;
    g_vcScaleHeight = h;
}

// Ensure double-buffered Y/UV image textures and readback PBOs exist at the right size
static void EnsureVCImageResources(int w, int h) {
    if (g_vcOutWidth == w && g_vcOutHeight == h && g_vcYImage[0] != 0) return;

    uint32_t nv12Size = w * h * 3 / 2;

    for (int i = 0; i < 2; i++) {
        // Y plane image: w x h, R8UI
        if (g_vcYImage[i] != 0) glDeleteTextures(1, &g_vcYImage[i]);
        glGenTextures(1, &g_vcYImage[i]);
        glBindTexture(GL_TEXTURE_2D, g_vcYImage[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);

        // UV plane image: w x h/2, R8UI (interleaved U,V as consecutive pixels)
        if (g_vcUVImage[i] != 0) glDeleteTextures(1, &g_vcUVImage[i]);
        glGenTextures(1, &g_vcUVImage[i]);
        glBindTexture(GL_TEXTURE_2D, g_vcUVImage[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI, w, h / 2);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);

        // PBO for async readback of NV12 data (Y + UV contiguous)
        if (g_vcReadbackPBO[i] != 0) glDeleteBuffers(1, &g_vcReadbackPBO[i]);
        glGenBuffers(1, &g_vcReadbackPBO[i]);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_vcReadbackPBO[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, nv12Size, nullptr, GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    if (g_vcReadbackFBO == 0) glGenFramebuffers(1, &g_vcReadbackFBO);

    g_vcOutWidth = w;
    g_vcOutHeight = h;
    g_vcWriteIdx = 0;
    g_vcComputePending = false;
    g_vcReadbackPending = false;
    if (g_vcFence) {
        glDeleteSync(g_vcFence);
        g_vcFence = nullptr;
    }
}

// Complete previous frame's readback: map PBO and write NV12 to virtual camera
static void FlushVirtualCameraReadback() {
    if (!g_vcReadbackPending) return;

    int readIdx = 1 - g_vcWriteIdx; // Read from the buffer we wrote to last frame
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_vcReadbackPBO[readIdx]);
    void* data = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (data) {
        LARGE_INTEGER counter, freq;
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&freq);
        uint64_t timestamp = (counter.QuadPart * 10000000ULL) / freq.QuadPart;
        WriteVirtualCameraFrameNV12(static_cast<const uint8_t*>(data), g_vcOutWidth, g_vcOutHeight, timestamp);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    g_vcReadbackPending = false;
}

// GPU path: dispatch compute shader to convert RGBA texture -> NV12 image textures,
// then start async PBO readback. Uses double-buffering so dispatch and readback overlap.
static void StartVirtualCameraComputeReadback(GLuint srcTexture, int texW, int texH, int outW, int outH) {
    // Step 1: If previous compute finished, start PBO readback of the result
    if (g_vcComputePending && g_vcFence) {
        // Non-blocking check: if GPU isn't done yet, skip this frame's virtual camera update
        GLenum result = glClientWaitSync(g_vcFence, 0, 0);
        if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
            glDeleteSync(g_vcFence);
            g_vcFence = nullptr;
            g_vcComputePending = false;

            // Readback Y plane then UV plane into the PBO (contiguous NV12 layout)
            int readIdx = g_vcWriteIdx; // We just finished writing to this buffer
            uint32_t ySize = outW * outH;

            glBindBuffer(GL_PIXEL_PACK_BUFFER, g_vcReadbackPBO[readIdx]);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g_vcReadbackFBO);

            // Read Y plane
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vcYImage[readIdx], 0);
            glReadPixels(0, 0, outW, outH, GL_RED_INTEGER, GL_UNSIGNED_BYTE, (void*)0);

            // Read UV plane (appended after Y)
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vcUVImage[readIdx], 0);
            glReadPixels(0, 0, outW, outH / 2, GL_RED_INTEGER, GL_UNSIGNED_BYTE, (void*)(uintptr_t)ySize);

            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

            // Mark readback pending — will be mapped + written to virtual camera next call
            g_vcReadbackPending = true;
        }
        // If not signaled yet, we just skip — no stall
    }

    // Step 2: Flush any pending PBO readback from the previous cycle
    FlushVirtualCameraReadback();

    // Step 3: Ensure image resources exist at the right size
    EnsureVCImageResources(outW, outH);

    // Step 4: Swap write buffer index for this frame's dispatch
    g_vcWriteIdx = 1 - g_vcWriteIdx;
    int writeIdx = g_vcWriteIdx;

    // Step 5: Determine source texture (downscale if needed)
    GLuint sampleTexture = srcTexture;
    if (outW != texW || outH != texH) {
        EnsureVCScaleResources(outW, outH);
        if (g_virtualCamCopyFBO == 0) glGenFramebuffers(1, &g_virtualCamCopyFBO);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_virtualCamCopyFBO);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTexture, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_vcScaleFBO);
        glBlitFramebuffer(0, 0, texW, texH, 0, 0, outW, outH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        sampleTexture = g_vcScaleTexture;
    }

    // Step 6: Dispatch compute shader with image2D bindings (no atomics, no SSBO clear)
    glUseProgram(g_vcComputeProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sampleTexture);
    glUniform1i(g_vcLocRgbaTexture, 0);
    glUniform1ui(g_vcLocWidth, outW);
    glUniform1ui(g_vcLocHeight, outH);

    // Bind Y and UV images for writing
    glBindImageTexture(0, g_vcYImage[writeIdx], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);
    glBindImageTexture(1, g_vcUVImage[writeIdx], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);

    GLuint groupsX = (outW + 15) / 16;
    GLuint groupsY = (outH + 15) / 16;
    glDispatchCompute(groupsX, groupsY, 1);

    // Fence after dispatch — we'll check it next frame (non-blocking)
    g_vcFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush(); // Ensure commands are submitted

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    g_vcComputePending = true;
}

// CPU fallback path: PBO readback + CPU NV12 conversion
static void StartVirtualCameraPBOReadback(GLuint obsTexture, int width, int height) {
    // If a previous read is still pending, complete it first and write to virtual camera
    if (g_virtualCamPBOPending && g_virtualCamPBO != 0) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_virtualCamPBO);
        void* data = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
        if (data) {
            LARGE_INTEGER counter, freq;
            QueryPerformanceCounter(&counter);
            QueryPerformanceFrequency(&freq);
            uint64_t timestamp = (counter.QuadPart * 10000000ULL) / freq.QuadPart;

            WriteVirtualCameraFrame(static_cast<const uint8_t*>(data), g_virtualCamPBOWidth, g_virtualCamPBOHeight, timestamp);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        g_virtualCamPBOPending = false;
    }

    // Resize PBO if needed
    if (g_virtualCamPBOWidth != width || g_virtualCamPBOHeight != height || g_virtualCamPBO == 0) {
        if (g_virtualCamPBO != 0) { glDeleteBuffers(1, &g_virtualCamPBO); }
        glGenBuffers(1, &g_virtualCamPBO);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_virtualCamPBO);
        glBufferData(GL_PIXEL_PACK_BUFFER, width * height * 4, nullptr, GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        g_virtualCamPBOWidth = width;
        g_virtualCamPBOHeight = height;

        if (g_virtualCamCopyFBO == 0) { glGenFramebuffers(1, &g_virtualCamCopyFBO); }
    }

    // Bind FBO to read from OBS texture
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_virtualCamCopyFBO);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, obsTexture, 0);

    // Start async read into PBO
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_virtualCamPBO);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    g_virtualCamPBOPending = true;
}

// Start async readback of OBS texture to Virtual Camera
// Routes to GPU compute path or CPU fallback based on hardware support
static void StartVirtualCameraAsyncReadback(GLuint obsTexture, int width, int height) {
    if (obsTexture == 0 || width <= 0 || height <= 0) return;
    if (!IsVirtualCameraActive()) return;

    int outW, outH;
    GetVirtualCamScaledSize(width, height, 1.0f, outW, outH);

    if (g_vcUseCompute && g_vcComputeProgram != 0) {
        StartVirtualCameraComputeReadback(obsTexture, width, height, outW, outH);
    } else {
        // CPU fallback uses the original dimensions (no resolution scaling in CPU path)
        StartVirtualCameraPBOReadback(obsTexture, width, height);
    }
}

// Render the game texture at the specified position
// This is used for OBS pass to render the game at animated position
// srcGameW/H = actual game content dimensions (may be different from texture allocation size)
// texW/H = allocated texture dimensions (for UV calculation)
static void RT_RenderGameTexture(GLuint gameTexture, int x, int y, int w, int h, int fullW, int fullH, int srcGameW, int srcGameH, int texW,
                                 int texH, GLuint vao, GLuint vbo) {
    if (gameTexture == UINT_MAX) return;

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gameTexture);

    glUseProgram(rt_backgroundProgram);
    glUniform1f(rt_backgroundShaderLocs.opacity, 1.0f);
    glDisable(GL_BLEND);

    // Convert window coordinates to NDC
    int y_gl = fullH - y - h; // Flip Y for OpenGL
    float nx1 = (static_cast<float>(x) / fullW) * 2.0f - 1.0f;
    float ny1 = (static_cast<float>(y_gl) / fullH) * 2.0f - 1.0f;
    float nx2 = (static_cast<float>(x + w) / fullW) * 2.0f - 1.0f;
    float ny2 = (static_cast<float>(y_gl + h) / fullH) * 2.0f - 1.0f;

    // Calculate UV coordinates - only sample the game content portion of the texture
    // The texture may be larger than the actual game content (allocated at max size)
    float u_max = (texW > 0) ? static_cast<float>(srcGameW) / texW : 1.0f;
    float v_max = (texH > 0) ? static_cast<float>(srcGameH) / texH : 1.0f;

    // UV: (0,0) = bottom-left, (u_max, v_max) = top-right of game content
    float verts[] = {
        nx1, ny1, 0.0f,  0.0f,  // bottom-left
        nx2, ny1, u_max, 0.0f,  // bottom-right
        nx2, ny2, u_max, v_max, // top-right
        nx1, ny1, 0.0f,  0.0f,  // bottom-left
        nx2, ny2, u_max, v_max, // top-right
        nx1, ny2, 0.0f,  v_max  // top-left
    };

    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Fix alpha values: The game texture may have junk alpha values
    // Set all alpha to 1.0 so OBS captures correctly
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE); // Write alpha only
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);               // Alpha = 1.0
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); // Restore
}

// Render EyeZoom overlay on render thread for OBS capture
// This renders the magnified game texture, colored boxes, center line, and caches text labels
static void RT_RenderEyeZoom(GLuint gameTexture, int requestViewportX, int fullW, int fullH, int gameTexW, int gameTexH, GLuint vao,
                             GLuint vbo, bool isTransitioningFromEyeZoom = false, GLuint snapshotTexture = 0, int snapshotWidth = 0,
                             int snapshotHeight = 0) {
    if (gameTexture == UINT_MAX) return;

    // Get EyeZoom config
    auto zoomCfgSnap = GetConfigSnapshot();
    if (!zoomCfgSnap) return; // Config not yet published
    EyeZoomConfig zoomConfig = zoomCfgSnap->eyezoom;

    // Calculate target position (final destination for EyeZoom mode)
    int modeWidth = zoomConfig.windowWidth;
    int targetViewportX = (fullW - modeWidth) / 2;

    // Use the passed-in requestViewportX parameter - this already accounts for hideAnimationsInGame
    // (caller passes -1 when skipAnimation is true, meaning use target position)
    int viewportX = (requestViewportX >= 0) ? requestViewportX : targetViewportX;

    if (viewportX <= 0) return; // No space for EyeZoom on the left

    // Calculate zoom output dimensions
    int zoomOutputWidth, zoomX;
    bool isTransitioningToEyeZoom = (viewportX < targetViewportX && !isTransitioningFromEyeZoom);

    if (zoomConfig.slideZoomIn) {
        // SLIDE MODE: Zoom is always at full target size, but slides in/out from the left
        // Calculate full size based on target position
        zoomOutputWidth = targetViewportX - (2 * zoomConfig.horizontalMargin);

        // Calculate slide position based on transition progress
        // When stable: zoomX = horizontalMargin (final position)
        // When transitioning TO: slide in from left (starts off-screen)
        // When transitioning FROM: slide out to left (ends off-screen)
        int finalZoomX = zoomConfig.horizontalMargin;
        int offScreenX = -zoomOutputWidth;

        if (isTransitioningToEyeZoom && targetViewportX > 0) {
            // Sliding IN: Calculate progress (0 = start, 1 = destination reached)
            float progress = (float)viewportX / (float)targetViewportX;
            // Slide from off-screen left to final position
            zoomX = offScreenX + (int)((finalZoomX - offScreenX) * progress);
        } else if (isTransitioningFromEyeZoom && targetViewportX > 0) {
            // Sliding OUT: Progress goes from 1 (fully visible) to 0 (off-screen)
            float progress = (float)viewportX / (float)targetViewportX;
            // Slide from final position to off-screen left
            zoomX = offScreenX + (int)((finalZoomX - offScreenX) * progress);
        } else {
            zoomX = finalZoomX;
        }
    } else {
        // GROW MODE (default): Zoom grows with the viewport
        // Horizontal margin applies to both sides:
        // - Left side: gap between screen edge and zoom section = horizontalMargin
        // - Right side: gap between zoom section and game viewport = horizontalMargin
        // During transitions (both in and out), the zoom section shrinks/grows to maintain these margins
        zoomOutputWidth = viewportX - (2 * zoomConfig.horizontalMargin);
        zoomX = zoomConfig.horizontalMargin;
    }

    // Don't render if there's not enough space (would overlap game or be too small)
    if (zoomOutputWidth <= 1) {
        return; // Need at least some minimum width to be useful
    }

    int zoomOutputHeight = fullH - (2 * zoomConfig.verticalMargin);
    int minHeight = (int)(0.2f * fullH);
    if (zoomOutputHeight < minHeight) zoomOutputHeight = minHeight;

    int zoomY = zoomConfig.verticalMargin;
    int zoomY_gl = fullH - zoomY - zoomOutputHeight;

    // Determine which texture to use as source and its dimensions
    GLuint sourceTexture = gameTexture;
    int srcWidth, srcHeight;

    // Get current draw framebuffer (the FBO we're rendering to)
    GLint currentDrawFBO;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &currentDrawFBO);

    // If transitioning FROM EyeZoom and we have a valid snapshot, use the snapshot
    // The snapshot contains the pre-rendered EyeZoom magnified content
    if (isTransitioningFromEyeZoom && rt_eyeZoomSnapshotValid && rt_eyeZoomSnapshotTexture != 0) {
        // Use local snapshot instead of passed parameters
        srcWidth = rt_eyeZoomSnapshotWidth;
        srcHeight = rt_eyeZoomSnapshotHeight;

        // STEP 1: Blit entire snapshot to destination
        GLuint tempFBO = 0;
        glGenFramebuffers(1, &tempFBO);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, tempFBO);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt_eyeZoomSnapshotTexture, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, currentDrawFBO);

        // Blit entire snapshot to destination (snapshot already contains the zoomed content)
        glBlitFramebuffer(0, 0, srcWidth, srcHeight, zoomX, zoomY_gl, zoomX + zoomOutputWidth, zoomY_gl + zoomOutputHeight,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);

        glDeleteFramebuffers(1, &tempFBO);
    } else {
        // Normal path: sample from game texture center
        // Use ACTUAL game texture dimensions, not EyeZoom config dimensions
        // During transitions, the game texture may be at different sizes
        int texWidth = gameTexW;
        int texHeight = gameTexH;

        // Calculate source region from the CENTER of the game texture
        int srcCenterX = texWidth / 2;
        int srcLeft = srcCenterX - zoomConfig.cloneWidth / 2;
        int srcRight = srcCenterX + zoomConfig.cloneWidth / 2;

        int srcCenterY = texHeight / 2;
        int srcBottom = srcCenterY - zoomConfig.cloneHeight / 2;
        int srcTop = srcCenterY + zoomConfig.cloneHeight / 2;

        // Destination region on screen
        int dstLeft = zoomX;
        int dstRight = zoomX + zoomOutputWidth;
        int dstBottom = zoomY_gl;
        int dstTop = zoomY_gl + zoomOutputHeight;

        // STEP 1: Blit from game texture to FBO
        GLuint tempFBO = 0;
        glGenFramebuffers(1, &tempFBO);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, tempFBO);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gameTexture, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, currentDrawFBO);

        glBlitFramebuffer(srcLeft, srcBottom, srcRight, srcTop, dstLeft, dstBottom, dstRight, dstTop, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        glDeleteFramebuffers(1, &tempFBO);

        // CAPTURE SNAPSHOT: Store the EyeZoom output for transition-out animation
        // Only capture when we're NOT transitioning from EyeZoom (stable or transitioning TO)
        // Also check the global atomic flag to catch the case where transition started after request was built
        bool shouldFreezeSnapshot = isTransitioningFromEyeZoom || g_isTransitioningFromEyeZoom.load(std::memory_order_acquire);
        if (!shouldFreezeSnapshot) {
            // Resize snapshot texture if needed
            if (rt_eyeZoomSnapshotTexture == 0 || rt_eyeZoomSnapshotWidth != zoomOutputWidth ||
                rt_eyeZoomSnapshotHeight != zoomOutputHeight) {
                if (rt_eyeZoomSnapshotTexture != 0) { glDeleteTextures(1, &rt_eyeZoomSnapshotTexture); }
                if (rt_eyeZoomSnapshotFBO != 0) { glDeleteFramebuffers(1, &rt_eyeZoomSnapshotFBO); }

                glGenTextures(1, &rt_eyeZoomSnapshotTexture);
                glBindTexture(GL_TEXTURE_2D, rt_eyeZoomSnapshotTexture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, zoomOutputWidth, zoomOutputHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

                glGenFramebuffers(1, &rt_eyeZoomSnapshotFBO);
                glBindFramebuffer(GL_FRAMEBUFFER, rt_eyeZoomSnapshotFBO);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt_eyeZoomSnapshotTexture, 0);

                rt_eyeZoomSnapshotWidth = zoomOutputWidth;
                rt_eyeZoomSnapshotHeight = zoomOutputHeight;

                glBindFramebuffer(GL_FRAMEBUFFER, currentDrawFBO);
            }

            // Copy the rendered EyeZoom content to snapshot
            glBindFramebuffer(GL_READ_FRAMEBUFFER, currentDrawFBO);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rt_eyeZoomSnapshotFBO);
            glBlitFramebuffer(dstLeft, dstBottom, dstRight, dstTop, 0, 0, zoomOutputWidth, zoomOutputHeight, GL_COLOR_BUFFER_BIT,
                              GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, currentDrawFBO);

            rt_eyeZoomSnapshotValid = true;
        }
    }

    // STEP 2: Render colored overlay boxes with numbers
    glBindFramebuffer(GL_FRAMEBUFFER, currentDrawFBO);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(rt_solidColorProgram);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    float pixelWidthOnScreen = zoomOutputWidth / (float)zoomConfig.cloneWidth;
    int labelsPerSide = zoomConfig.cloneWidth / 2;
    // Use zoomY_gl (OpenGL coordinates) for centerY since NDC conversion expects Y=0 at bottom
    float centerY = zoomY_gl + zoomOutputHeight / 2.0f;

    // Use configured font size for box height
    float boxHeight = zoomConfig.linkRectToFont ? (zoomConfig.textFontSize * 1.2f) : (float)zoomConfig.rectHeight;

    int boxIndex = 0;
    for (int xOffset = -labelsPerSide; xOffset <= labelsPerSide; xOffset++) {
        if (xOffset == 0) continue;

        float boxLeft = zoomX + (boxIndex * pixelWidthOnScreen);
        float boxRight = boxLeft + pixelWidthOnScreen;
        float boxBottom = centerY - boxHeight / 2.0f;
        float boxTop = centerY + boxHeight / 2.0f;

        Color boxColor = (boxIndex % 2 == 0) ? zoomConfig.gridColor1 : zoomConfig.gridColor2;
        float boxOpacity = (boxIndex % 2 == 0) ? zoomConfig.gridColor1Opacity : zoomConfig.gridColor2Opacity;
        glUniform4f(rt_solidColorShaderLocs.color, boxColor.r, boxColor.g, boxColor.b, boxOpacity);

        boxIndex++;

        float boxNdcLeft = (boxLeft / (float)fullW) * 2.0f - 1.0f;
        float boxNdcRight = (boxRight / (float)fullW) * 2.0f - 1.0f;
        float boxNdcBottom = (boxBottom / (float)fullH) * 2.0f - 1.0f;
        float boxNdcTop = (boxTop / (float)fullH) * 2.0f - 1.0f;

        float boxVerts[] = {
            boxNdcLeft, boxNdcBottom, 0, 0, boxNdcRight, boxNdcBottom, 0, 0, boxNdcRight, boxNdcTop, 0, 0,
            boxNdcLeft, boxNdcBottom, 0, 0, boxNdcRight, boxNdcTop,    0, 0, boxNdcLeft,  boxNdcTop, 0, 0,
        };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(boxVerts), boxVerts);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Cache text labels for ImGui rendering
        int displayNumber = abs(xOffset);
        float numberCenterX = boxLeft + pixelWidthOnScreen / 2.0f;
        float numberCenterY = centerY;
        // Note: CacheEyeZoomTextLabel is in render.cpp - we need to call it from here
        // For now, we'll skip text labels for OBS (they require cross-thread coordination)
    }

    // STEP 3: Render vertical center line
    float centerX = zoomX + zoomOutputWidth / 2.0f;
    float centerLineWidth = 2.0f;
    float lineLeft = centerX - centerLineWidth / 2.0f;
    float lineRight = centerX + centerLineWidth / 2.0f;
    // Use zoomY_gl and zoomOutputHeight since dstBottom/dstTop are only in else branch
    float lineBottom = (float)zoomY_gl;
    float lineTop = (float)(zoomY_gl + zoomOutputHeight);

    float lineNdcLeft = (lineLeft / (float)fullW) * 2.0f - 1.0f;
    float lineNdcRight = (lineRight / (float)fullW) * 2.0f - 1.0f;
    float lineNdcBottom = (lineBottom / (float)fullH) * 2.0f - 1.0f;
    float lineNdcTop = (lineTop / (float)fullH) * 2.0f - 1.0f;

    glUniform4f(rt_solidColorShaderLocs.color, zoomConfig.centerLineColor.r, zoomConfig.centerLineColor.g, zoomConfig.centerLineColor.b,
                zoomConfig.centerLineColorOpacity);

    float centerLineVerts[] = {
        lineNdcLeft, lineNdcBottom, 0, 0, lineNdcRight, lineNdcBottom, 0, 0, lineNdcRight, lineNdcTop, 0, 0,
        lineNdcLeft, lineNdcBottom, 0, 0, lineNdcRight, lineNdcTop,    0, 0, lineNdcLeft,  lineNdcTop, 0, 0,
    };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(centerLineVerts), centerLineVerts);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisable(GL_BLEND);
}

// Render mirrors using render thread's local shader programs
static void RT_RenderMirrors(const std::vector<MirrorConfig>& activeMirrors, const GameViewportGeometry& geo, int fullW, int fullH,
                             float modeOpacity, bool excludeOnlyOnMyScreen, bool relativeStretching, float transitionProgress,
                             float mirrorSlideProgress, int fromX, int fromY, int fromW, int fromH, int toX, int toY, int toW, int toH,
                             bool isEyeZoomMode, bool isTransitioningFromEyeZoom, int eyeZoomAnimatedViewportX, bool skipAnimation,
                             const std::string& fromModeId, bool fromSlideMirrorsIn, bool toSlideMirrorsIn, bool isSlideOutPass, GLuint vao,
                             GLuint vbo) {
    if (activeMirrors.empty()) return;

    // Grab config snapshot for thread-safe access
    auto slideCfgSnap = GetConfigSnapshot();
    if (!slideCfgSnap) return; // Config not yet published
    const Config& slideCfg = *slideCfgSnap;

    // Collect source mode mirror names (for determining which mirrors exist in both modes)
    // Mirrors that exist in both the source mode and target mode should use normal bounce animation,
    // not the slide animation (which is for mode-specific mirrors only)
    std::set<std::string> sourceMirrorNames;
    if (!fromModeId.empty() && (fromSlideMirrorsIn || toSlideMirrorsIn || slideCfg.eyezoom.slideMirrorsIn)) {
        // Look up the FROM mode to get its mirror list
        for (const auto& mode : slideCfg.modes) {
            if (EqualsIgnoreCase(mode.id, fromModeId)) {
                for (const auto& mirrorName : mode.mirrorIds) { sourceMirrorNames.insert(mirrorName); }
                // Also include mirrors from mirror groups
                for (const auto& groupName : mode.mirrorGroupIds) {
                    for (const auto& group : slideCfg.mirrorGroups) {
                        if (group.name == groupName) {
                            for (const auto& item : group.mirrors) { sourceMirrorNames.insert(item.mirrorId); }
                            break;
                        }
                    }
                }
                break;
            }
        }
    }

    // Pre-cache mirror render data
    // Use unique_lock because we need to wait on the fence while holding the lock
    std::vector<MirrorRenderData> mirrorsToRender;
    mirrorsToRender.reserve(activeMirrors.size());

    {
        std::unique_lock<std::shared_mutex> mirrorLock(g_mirrorInstancesMutex);
        for (const auto& conf : activeMirrors) {
            if (excludeOnlyOnMyScreen && conf.onlyOnMyScreen) continue;

            auto it = g_mirrorInstances.find(conf.name);
            if (it == g_mirrorInstances.end()) continue;

            MirrorInstance& inst = it->second;
            if (!inst.hasValidContent) continue;

            MirrorRenderData data;
            data.config = &conf;

            // Calculate effective scale values
            float scaleX = conf.output.separateScale ? conf.output.scaleX : conf.output.scale;
            float scaleY = conf.output.separateScale ? conf.output.scaleY : conf.output.scale;

            // ALWAYS prefer finalTexture when available - it has borders already applied by mirror_thread
            // This avoids redundant border rendering
            // NOTE: We calculate outW/outH from FBO base dimensions and config scale, NOT from
            // inst.final_w/h. This allows the same mirror texture to be rendered at different scales:
            // - Mirror's own scale when used directly
            // - Group's scale when used in a group (conf.output comes from group via RT_CollectActiveElements)
            if (inst.finalTexture != 0 && inst.final_w > 0 && inst.final_h > 0) {
                data.texture = inst.finalTexture;
                data.tex_w = inst.final_w;
                data.tex_h = inst.final_h;
                // Calculate output size from FBO base dimensions and config scale
                data.outW = static_cast<int>(inst.fbo_w * scaleX);
                data.outH = static_cast<int>(inst.fbo_h * scaleY);
            } else {
                // Fallback to fboTexture only when finalTexture doesn't exist
                // This shouldn't happen in normal operation - mirror_thread always produces finalTexture
                data.texture = inst.fboTexture;
                data.tex_w = inst.fbo_w;
                data.tex_h = inst.fbo_h;
                data.outW = static_cast<int>(inst.fbo_w * scaleX);
                data.outH = static_cast<int>(inst.fbo_h * scaleY);
            }

            if (data.texture == 0) continue;

            // CRITICAL: Wait for capture thread's GPU work to complete before reading texture
            // We wait on the fence but do NOT delete it - multiple render paths may need to
            // wait on the same fence. The fence will be deleted when SwapMirrorBuffers swaps
            // in a new fence from the capture thread.
            if (inst.gpuFence) {
                // Wait with timeout loop to handle GPU load - keep waiting until complete
                GLenum waitResult;
                do {
                    waitResult = glClientWaitSync(inst.gpuFence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
                } while (waitResult == GL_TIMEOUT_EXPIRED);
            }
            data.gpuFence = nullptr; // Not used, kept for struct compatibility

            // Check if cache is still valid for current viewport geometry AND output position
            // During animations, geo.finalX/Y/W/H change every frame, so cached positions become stale
            // Also force recalculation during animation (transitionProgress < 1.0) to ensure proper A->B interpolation
            const auto& cache = inst.cachedRenderState;
            bool isAnimating = transitionProgress < 1.0f;
            bool cacheMatchesCurrentGeo =
                cache.isValid && !isAnimating && cache.finalX == geo.finalX && cache.finalY == geo.finalY && cache.finalW == geo.finalW &&
                cache.finalH == geo.finalH && cache.screenW == fullW && cache.screenH == fullH &&
                // Also check output position settings
                cache.outputX == conf.output.x && cache.outputY == conf.output.y && cache.outputScale == conf.output.scale &&
                cache.outputSeparateScale == conf.output.separateScale && cache.outputScaleX == conf.output.scaleX &&
                cache.outputScaleY == conf.output.scaleY && cache.outputRelativeTo == conf.output.relativeTo;

            if (cacheMatchesCurrentGeo) {
                memcpy(data.vertices, inst.cachedRenderState.vertices, sizeof(data.vertices));
                // Use actual mirror screen position from cache (for static borders)
                data.screenX = cache.mirrorScreenX;
                data.screenY = cache.mirrorScreenY;
                data.screenW = cache.mirrorScreenW;
                data.screenH = cache.mirrorScreenH;
                data.cacheValid = true;
            } else {
                data.cacheValid = false;
            }

            // Copy content presence flag for static border rendering
            data.hasFrameContent = inst.hasFrameContent;

            mirrorsToRender.push_back(data);
        }
    }

    if (mirrorsToRender.empty()) return;

    // Memory barrier to ensure all mirror texture writes are visible
    // This is critical for cross-context texture sharing under GPU load
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_BLEND);
    // Use separate blend functions for proper premultiplied alpha output
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // All border rendering is now done by mirror_thread
    // Render thread just blits the pre-rendered finalTexture using passthrough shader
    glUseProgram(rt_backgroundProgram);

    for (auto& renderData : mirrorsToRender) {
        const MirrorConfig& conf = *renderData.config;
        glUniform1f(rt_backgroundShaderLocs.opacity, modeOpacity * conf.opacity);

        glBindTexture(GL_TEXTURE_2D, renderData.texture);

        if (renderData.cacheValid) {
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(renderData.vertices), renderData.vertices);
        } else {
            // Calculate vertices on the fly (fallback)
            std::string anchor = conf.output.relativeTo;

            // Check if this is a Screen anchor (absolute screen positioning)
            bool isScreenRelative = false;
            if (anchor.length() > 6 && anchor.substr(anchor.length() - 6) == "Screen") {
                anchor = anchor.substr(0, anchor.length() - 6);
                isScreenRelative = true;
            } else if (anchor.length() > 8 && anchor.substr(anchor.length() - 8) == "Viewport") {
                anchor = anchor.substr(0, anchor.length() - 8);
            }

            int finalX_screen, finalY_screen, finalW_screen, finalH_screen;

            if (isScreenRelative) {
                // Screen-relative: position directly on screen
                int outX, outY;
                GetRelativeCoords(anchor, conf.output.x, conf.output.y, renderData.outW, renderData.outH, fullW, fullH, outX, outY);
                finalX_screen = outX;
                finalY_screen = outY;
                finalW_screen = renderData.outW;
                finalH_screen = renderData.outH;

                // Store calculated position for border pass
                renderData.screenX = finalX_screen;
                renderData.screenY = finalY_screen;
                renderData.screenW = finalW_screen;
                renderData.screenH = finalH_screen;
            } else {
                // Viewport-relative: lerp position from FROM viewport to TO viewport
                // Must calculate position relative to EACH viewport's actual dimensions

                // Calculate mirror size for each viewport
                float toScaleX = (toW > 0 && geo.gameW > 0) ? static_cast<float>(toW) / geo.gameW : 1.0f;
                float toScaleY = (toH > 0 && geo.gameH > 0) ? static_cast<float>(toH) / geo.gameH : 1.0f;
                float fromScaleX = (fromW > 0 && geo.gameW > 0) ? static_cast<float>(fromW) / geo.gameW : toScaleX;
                float fromScaleY = (fromH > 0 && geo.gameH > 0) ? static_cast<float>(fromH) / geo.gameH : toScaleY;

                int toSizeW = relativeStretching ? static_cast<int>(renderData.outW * toScaleX) : renderData.outW;
                int toSizeH = relativeStretching ? static_cast<int>(renderData.outH * toScaleY) : renderData.outH;
                int fromSizeW = relativeStretching ? static_cast<int>(renderData.outW * fromScaleX) : renderData.outW;
                int fromSizeH = relativeStretching ? static_cast<int>(renderData.outH * fromScaleY) : renderData.outH;

                // Calculate position at TO viewport - use TO viewport dimensions as reference
                int toOutX, toOutY;
                GetRelativeCoords(anchor, conf.output.x, conf.output.y, toSizeW, toSizeH, toW, toH, toOutX, toOutY);
                int toPosX = toX + toOutX;
                int toPosY = toY + toOutY;

                // Calculate position at FROM viewport - use FROM viewport dimensions as reference
                // Special case: when transitioning FROM EyeZoom, use target height/Y for Y calculations
                // This prevents vertical sliding due to EyeZoom's tall viewport (e.g., 16384)
                int fromOutX, fromOutY;
                int effectiveFromH = isTransitioningFromEyeZoom ? toH : fromH;
                int effectiveFromY = isTransitioningFromEyeZoom ? toY : fromY;
                int effectiveFromSizeH = isTransitioningFromEyeZoom ? toSizeH : fromSizeH;
                GetRelativeCoords(anchor, conf.output.x, conf.output.y, fromSizeW, effectiveFromSizeH, fromW, effectiveFromH, fromOutX,
                                  fromOutY);
                int fromPosX = fromX + fromOutX;
                int fromPosY = effectiveFromY + fromOutY;

                // Lerp between FROM and TO positions
                float t = transitionProgress;
                finalX_screen = static_cast<int>(fromPosX + (toPosX - fromPosX) * t);
                finalY_screen = static_cast<int>(fromPosY + (toPosY - fromPosY) * t);

                if (relativeStretching) {
                    finalW_screen = static_cast<int>(fromSizeW + (toSizeW - fromSizeW) * t);
                    finalH_screen = static_cast<int>(fromSizeH + (toSizeH - fromSizeH) * t);
                } else {
                    finalW_screen = renderData.outW;
                    finalH_screen = renderData.outH;
                }

                // Store calculated position for border pass
                renderData.screenX = finalX_screen;
                renderData.screenY = finalY_screen;
                renderData.screenW = finalW_screen;
                renderData.screenH = finalH_screen;
            }

            // === Slide Animation Logic ===
            // There are two slide animation systems:
            // 1. EyeZoom-specific: Uses eyeZoomAnimatedViewportX for precise viewport-synchronized slide
            // 2. Generic mode slide: Uses transitionProgress for any mode with slideMirrorsIn enabled

            bool shouldApplySlide = false;
            float slideProgress = 1.0f; // 1.0 = at final position, 0.0 = off-screen

            // --- EyeZoom slide animation (uses viewport X for synchronization) ---
            auto ezCfgSnap = GetConfigSnapshot();
            if (!ezCfgSnap) continue; // Config not yet published
            EyeZoomConfig zoomConfig = ezCfgSnap->eyezoom;
            int modeWidth = zoomConfig.windowWidth;
            int targetViewportX = (fullW - modeWidth) / 2;

            bool hasEyeZoomAnimatedPosition = eyeZoomAnimatedViewportX >= 0 && targetViewportX > 0;
            bool isEyeZoomTransitioning = hasEyeZoomAnimatedPosition && eyeZoomAnimatedViewportX < targetViewportX;

            // EyeZoom slide applies in two cases:
            // 1. Slide-in: entering EyeZoom mode (isEyeZoomMode=true, transitioning, !leaving)
            // 2. Slide-out: EyeZoom mirrors leaving (isEyeZoomMode=true, isTransitioningFromEyeZoom=true)
            bool isTransitioningToEyeZoom = isEyeZoomMode && isEyeZoomTransitioning && !isTransitioningFromEyeZoom;
            bool isEyeZoomSlideOut = isEyeZoomMode && isTransitioningFromEyeZoom && isEyeZoomTransitioning;

            if (zoomConfig.slideMirrorsIn && (isTransitioningToEyeZoom || isEyeZoomSlideOut) && hasEyeZoomAnimatedPosition) {
                shouldApplySlide = true;
                slideProgress = static_cast<float>(eyeZoomAnimatedViewportX) / static_cast<float>(targetViewportX);
            }

            // --- Generic mode slide animation (uses mirrorSlideProgress) ---
            // For non-EyeZoom modes with slideMirrorsIn enabled
            // When skipAnimation is true (hideAnimationsInGame enabled), skip slide animation
            // so mirrors appear at their final positions immediately on user's screen
            // Uses mirrorSlideProgress instead of transitionProgress so slides work even when
            // overlay transition is set to "Cut"
            if (!shouldApplySlide && mirrorSlideProgress < 1.0f && !skipAnimation) {
                // Slide-in: when entering a mode that has slideMirrorsIn enabled
                if (toSlideMirrorsIn && !isSlideOutPass) {
                    shouldApplySlide = true;
                    slideProgress = mirrorSlideProgress;
                }
                // Slide-out: when this is a slide-out pass for mirrors from a mode with slideMirrorsIn
                // Invert progress: at transition start (mirrorSlideProgress=0), mirrors are at position (slideProgress=1)
                // at transition end (mirrorSlideProgress=1), mirrors are off-screen (slideProgress=0)
                else if (fromSlideMirrorsIn && isSlideOutPass) {
                    shouldApplySlide = true;
                    slideProgress = 1.0f - mirrorSlideProgress;
                }
            }

            // Skip slide for mirrors that exist in both source and target modes (they should bounce normally)
            if (shouldApplySlide && sourceMirrorNames.count(conf.name) > 0) { shouldApplySlide = false; }

            if (shouldApplySlide) {
                slideProgress = (slideProgress < 0.0f) ? 0.0f : (slideProgress > 1.0f ? 1.0f : slideProgress);

                // Calculate mirror center X to determine which side of screen it's on
                int mirrorCenterX = finalX_screen + finalW_screen / 2;
                bool isOnLeftSide = mirrorCenterX < (fullW / 2);

                // Off-screen positions for sliding
                int offScreenLeft = -finalW_screen; // Fully off left edge
                int offScreenRight = fullW;         // Fully off right edge

                // At slideProgress=0: mirror is off-screen
                // At slideProgress=1: mirror is at final position
                if (isOnLeftSide) {
                    // Left-side mirror: slide from left edge to final position
                    int slideX = offScreenLeft + static_cast<int>((finalX_screen - offScreenLeft) * slideProgress);
                    finalX_screen = slideX;
                } else {
                    // Right-side mirror: slide from right edge to final position
                    int slideX = offScreenRight - static_cast<int>((offScreenRight - finalX_screen) * slideProgress);
                    finalX_screen = slideX;
                }

                // Update stored position for border pass
                renderData.screenX = finalX_screen;
            }

            int finalY_gl = fullH - finalY_screen - finalH_screen;

            float nx1 = (static_cast<float>(finalX_screen) / fullW) * 2.0f - 1.0f;
            float ny1 = (static_cast<float>(finalY_gl) / fullH) * 2.0f - 1.0f;
            float nx2 = (static_cast<float>(finalX_screen + finalW_screen) / fullW) * 2.0f - 1.0f;
            float ny2 = (static_cast<float>(finalY_gl + finalH_screen) / fullH) * 2.0f - 1.0f;

            float verts[] = { nx1, ny1, 0, 0, nx2, ny1, 1, 0, nx2, ny2, 1, 1, nx1, ny1, 0, 0, nx2, ny2, 1, 1, nx1, ny2, 0, 1 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        }

        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // === PASS 2: Static Border Rendering ===
    // Render borders after all mirrors are drawn so they can overlay on top
    // and extend outside mirror bounds
    glUseProgram(rt_staticBorderProgram);

    for (const auto& renderData : mirrorsToRender) {
        const MirrorConfig& conf = *renderData.config;
        const MirrorBorderConfig& border = conf.border;

        // Skip if border type is Dynamic (only render static borders here)
        if (border.type != MirrorBorderType::Static) continue;

        // Skip if disabled (thickness <= 0 means disabled)
        if (border.staticThickness <= 0) continue;

        // Skip if mirror has no content (no matching pixels found by filter)
        if (!renderData.hasFrameContent) continue;

        // Use the screen position calculated/cached in the mirror pass
        // If dimensions are invalid, skip
        if (renderData.screenW <= 0 || renderData.screenH <= 0) continue;

        // 1. Calculate Border Quad Position
        // Apply custom size override if specified, otherwise use mirror size
        // Enforce minimum dimensions to avoid degenerate shapes
        int baseW = (border.staticWidth > 0) ? border.staticWidth : renderData.screenW;
        int baseH = (border.staticHeight > 0) ? border.staticHeight : renderData.screenH;
        baseW = (baseW < 2) ? 2 : baseW;
        baseH = (baseH < 2) ? 2 : baseH;

        // The shader draws borders OUTSIDE the shape edge, so we need to expand the quad
        // to accommodate the border. The border extends from 0 to thickness
        // outside the edge, so total extension = thickness on each side.
        // Add +1 padding for floating-point precision at boundaries (matches shader epsilon)
        int borderExtension = border.staticThickness + 1;
        int quadW = baseW + borderExtension * 2;
        int quadH = baseH + borderExtension * 2;

        // Center border on mirror if border is larger than mirror
        // This allows borders to extend beyond the mirror FBO bounds
        int centerOffsetX = (baseW - renderData.screenW) / 2;
        int centerOffsetY = (baseH - renderData.screenH) / 2;

        // Apply custom offsets from config (applied after centering)
        // Also offset by borderExtension to account for the expanded quad
        int quadX = renderData.screenX - centerOffsetX + border.staticOffsetX - borderExtension;
        int quadY = renderData.screenY - centerOffsetY + border.staticOffsetY - borderExtension;

        // 2. Setup Uniforms
        glUniform1i(rt_staticBorderShaderLocs.shape, static_cast<int>(border.staticShape));
        glUniform4f(rt_staticBorderShaderLocs.borderColor, border.staticColor.r, border.staticColor.g, border.staticColor.b,
                    border.staticColor.a * conf.opacity * modeOpacity);
        glUniform1f(rt_staticBorderShaderLocs.thickness, static_cast<float>(border.staticThickness));
        glUniform1f(rt_staticBorderShaderLocs.radius, static_cast<float>(border.staticRadius));
        // Pass the BASE size for SDF calculations and the actual QUAD size for pixel coordinate mapping
        glUniform2f(rt_staticBorderShaderLocs.size, static_cast<float>(baseW), static_cast<float>(baseH));
        glUniform2f(rt_staticBorderShaderLocs.quadSize, static_cast<float>(quadW), static_cast<float>(quadH));

        // 3. Render Quad
        int finalY_gl = fullH - (quadY + quadH);

        float nx1 = (static_cast<float>(quadX) / fullW) * 2.0f - 1.0f;
        float ny1 = (static_cast<float>(finalY_gl) / fullH) * 2.0f - 1.0f;
        float nx2 = (static_cast<float>(quadX + quadW) / fullW) * 2.0f - 1.0f;
        float ny2 = (static_cast<float>(finalY_gl + quadH) / fullH) * 2.0f - 1.0f;

        float verts[] = { nx1, ny1, 0, 0, nx2, ny1, 1, 0, nx2, ny2, 1, 1, nx1, ny1, 0, 0, nx2, ny2, 1, 1, nx1, ny2, 0, 1 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glDisable(GL_BLEND);
}

// Render images using render thread's local shader programs
// gameX/Y/W/H = game viewport position on screen (for viewport-relative positioning)
static void RT_RenderImages(const std::vector<ImageConfig>& activeImages, int fullW, int fullH, int gameX, int gameY, int gameW, int gameH,
                            int gameResW, int gameResH, bool relativeStretching, float transitionProgress, int fromX, int fromY, int fromW,
                            int fromH, float modeOpacity, bool excludeOnlyOnMyScreen, GLuint vao, GLuint vbo) {
    if (activeImages.empty()) return;

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    // Use separate blend functions for RGB and Alpha:
    // RGB: standard alpha blend (src.rgb * src.a + dst.rgb * (1 - src.a))
    // Alpha: additive with destination attenuation (src.a + dst.a * (1 - src.a))
    // This ensures the FBO contains properly premultiplied alpha content
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    for (const auto& conf : activeImages) {
        if (excludeOnlyOnMyScreen && conf.onlyOnMyScreen) continue;

        auto it_inst = g_userImages.find(conf.name);
        if (it_inst == g_userImages.end() || it_inst->second.textureId == 0) continue;
        const UserImageInstance& inst = it_inst->second;

        // Use cached render state if valid AND config hasn't changed
        float nx1, ny1, nx2, ny2;
        int displayW, displayH;
        const auto& cache = inst.cachedRenderState;

        // Check if config has changed - must match main thread's validation logic
        bool configChanged = !cache.isValid || cache.crop_left != conf.crop_left || cache.crop_right != conf.crop_right ||
                             cache.crop_top != conf.crop_top || cache.crop_bottom != conf.crop_bottom || cache.scale != conf.scale ||
                             cache.x != conf.x || cache.y != conf.y || cache.relativeTo != conf.relativeTo || cache.screenWidth != fullW ||
                             cache.screenHeight != fullH;

        if (!configChanged) {
            nx1 = cache.nx1;
            ny1 = cache.ny1;
            nx2 = cache.nx2;
            ny2 = cache.ny2;
            displayW = cache.displayW;
            displayH = cache.displayH;
        } else {
            // Cache is stale - recalculate
            CalculateImageDimensions(conf, displayW, displayH);

            // Check if viewport-relative (ends with "Viewport")
            bool isViewportRelative = conf.relativeTo.length() > 8 && conf.relativeTo.substr(conf.relativeTo.length() - 8) == "Viewport";

            int finalScreenX_win, finalScreenY_win;
            int finalDisplayW = displayW;
            int finalDisplayH = displayH;

            if (isViewportRelative) {
                // Calculate sizes for FROM and TO viewports
                float toScaleX = (gameW > 0 && gameResW > 0) ? static_cast<float>(gameW) / gameResW : 1.0f;
                float toScaleY = (gameH > 0 && gameResH > 0) ? static_cast<float>(gameH) / gameResH : 1.0f;
                float fromScaleX = (fromW > 0 && gameResW > 0) ? static_cast<float>(fromW) / gameResW : toScaleX;
                float fromScaleY = (fromH > 0 && gameResH > 0) ? static_cast<float>(fromH) / gameResH : toScaleY;

                // Calculate display sizes at FROM and TO
                int toDisplayW = relativeStretching ? static_cast<int>(displayW * toScaleX) : displayW;
                int toDisplayH = relativeStretching ? static_cast<int>(displayH * toScaleY) : displayH;
                int fromDisplayW = relativeStretching ? static_cast<int>(displayW * fromScaleX) : displayW;
                int fromDisplayH = relativeStretching ? static_cast<int>(displayH * fromScaleY) : displayH;

                // Calculate position at TO viewport
                int toPosX, toPosY;
                GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, toDisplayW, toDisplayH, gameX, gameY, gameW, gameH,
                                                      fullW, fullH, toPosX, toPosY);

                // Calculate position at FROM viewport
                int fromPosX, fromPosY;
                GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, fromDisplayW, fromDisplayH, fromX, fromY, fromW,
                                                      fromH, fullW, fullH, fromPosX, fromPosY);

                // Lerp between FROM and TO positions
                float t = transitionProgress;
                finalScreenX_win = static_cast<int>(fromPosX + (toPosX - fromPosX) * t);
                finalScreenY_win = static_cast<int>(fromPosY + (toPosY - fromPosY) * t);

                if (relativeStretching) {
                    finalDisplayW = static_cast<int>(fromDisplayW + (toDisplayW - fromDisplayW) * t);
                    finalDisplayH = static_cast<int>(fromDisplayH + (toDisplayH - fromDisplayH) * t);
                }
            } else {
                // Screen-relative: no interpolation needed
                GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, finalDisplayW, finalDisplayH, gameX, gameY, gameW,
                                                      gameH, fullW, fullH, finalScreenX_win, finalScreenY_win);
            }

            int finalScreenY_gl = fullH - finalScreenY_win - finalDisplayH;
            nx1 = (static_cast<float>(finalScreenX_win) / fullW) * 2.0f - 1.0f;
            ny1 = (static_cast<float>(finalScreenY_gl) / fullH) * 2.0f - 1.0f;
            nx2 = (static_cast<float>(finalScreenX_win + finalDisplayW) / fullW) * 2.0f - 1.0f;
            ny2 = (static_cast<float>(finalScreenY_gl + finalDisplayH) / fullH) * 2.0f - 1.0f;
            displayW = finalDisplayW;
            displayH = finalDisplayH;
        }

        // Draw background if enabled
        if (conf.background.enabled && conf.background.opacity > 0.0f && !inst.isFullyTransparent) {
            glUseProgram(rt_solidColorProgram);
            glUniform4f(rt_solidColorShaderLocs.color, conf.background.color.r, conf.background.color.g, conf.background.color.b,
                        conf.background.opacity * modeOpacity);
            float bg_verts[] = { nx1, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny2, 0, 0 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(bg_verts), bg_verts);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        // Draw image
        glUseProgram(rt_imageRenderProgram);
        glBindTexture(GL_TEXTURE_2D, inst.textureId);

        // Set texture filtering based on pixelatedScaling config
        if (conf.pixelatedScaling) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        glUniform1i(rt_imageRenderShaderLocs.enableColorKey, conf.enableColorKey && !conf.colorKeys.empty() ? 1 : 0);
        if (conf.enableColorKey && !conf.colorKeys.empty()) {
            glUniform3f(rt_imageRenderShaderLocs.colorKey, conf.colorKeys[0].color.r, conf.colorKeys[0].color.g, conf.colorKeys[0].color.b);
            glUniform1f(rt_imageRenderShaderLocs.sensitivity, conf.colorKeys[0].sensitivity);
        }
        glUniform1f(rt_imageRenderShaderLocs.opacity, conf.opacity * modeOpacity);

        // Calculate texture coordinates with cropping
        // OpenGL texture coordinates: Y=0 at bottom, Y=1 at top
        // Vertices are arranged: bottom uses ty1, top uses ty2
        // So ty1 maps to bottom (after cropping from bottom), ty2 to top (after cropping from top)
        float tu1 = static_cast<float>(conf.crop_left) / inst.width;
        float tu2 = static_cast<float>(inst.width - conf.crop_right) / inst.width;
        float tv1 = static_cast<float>(conf.crop_bottom) / inst.height;
        float tv2 = static_cast<float>(inst.height - conf.crop_top) / inst.height;

        float verts[] = { nx1, ny1, tu1, tv1, nx2, ny1, tu2, tv1, nx2, ny2, tu2, tv2,
                          nx1, ny1, tu1, tv1, nx2, ny2, tu2, tv2, nx1, ny2, tu1, tv2 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Render border if enabled (matching RenderImages behavior in render.cpp)
        if (conf.border.enabled && conf.border.width > 0 && !inst.isFullyTransparent) {
            // Calculate window coordinates from cached NDC values
            int finalScreenX_win = static_cast<int>((nx1 + 1.0f) / 2.0f * fullW);
            int finalScreenY_gl = static_cast<int>((ny1 + 1.0f) / 2.0f * fullH);
            int finalScreenY_win = fullH - finalScreenY_gl - displayH;

            RT_RenderGameBorder(finalScreenX_win, finalScreenY_win, displayW, displayH, conf.border.width, conf.border.radius,
                                conf.border.color, fullW, fullH, vao, vbo);

            // Restore state after border rendering
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
        }
    }

    glDisable(GL_BLEND);
}

// Render window overlays using render thread's local shader programs
// gameX/Y/W/H = game viewport position on screen (for viewport-relative positioning)
static void RT_RenderWindowOverlays(const std::vector<std::string>& overlayIds, int fullW, int fullH, int gameX, int gameY, int gameW,
                                    int gameH, int gameResW, int gameResH, bool relativeStretching, float transitionProgress, int fromX,
                                    int fromY, int fromW, int fromH, float modeOpacity, bool excludeOnlyOnMyScreen, GLuint vao,
                                    GLuint vbo) {
    if (overlayIds.empty()) return;

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    // Use separate blend functions for proper premultiplied alpha output
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(rt_imageRenderProgram);
    glUniform1i(rt_imageRenderShaderLocs.enableColorKey, 0);
    glUniform1f(rt_imageRenderShaderLocs.opacity, modeOpacity);

    std::unique_lock<std::mutex> cacheLock(g_windowOverlayCacheMutex, std::try_to_lock);
    if (!cacheLock.owns_lock()) {
        glDisable(GL_BLEND);
        return; // Skip if can't get lock
    }

    for (const auto& overlayId : overlayIds) {
        // Use snapshot for thread-safe config lookup (render thread reads, GUI thread writes)
        auto rtOverlaySnap = GetConfigSnapshot();
        const WindowOverlayConfig* conf = rtOverlaySnap ? FindWindowOverlayConfigIn(overlayId, *rtOverlaySnap) : nullptr;
        if (!conf) continue;
        if (excludeOnlyOnMyScreen && conf->onlyOnMyScreen) continue;

        auto it = g_windowOverlayCache.find(overlayId);
        if (it == g_windowOverlayCache.end() || !it->second) continue;

        WindowOverlayCacheEntry& entry = *it->second;

        // Check if capture thread has a new frame ready
        if (entry.hasNewFrame.load(std::memory_order_acquire)) {
            // Swap readyBuffer with backBuffer under lock - this gives us exclusive access to backBuffer
            {
                std::lock_guard<std::mutex> lock(entry.swapMutex);
                entry.readyBuffer.swap(entry.backBuffer);
            }
            entry.hasNewFrame.store(false, std::memory_order_release);
        }

        // Now read from backBuffer - it's safe, capture thread won't touch it
        WindowOverlayRenderData* renderData = entry.backBuffer.get();
        if (renderData && renderData->pixelData && renderData->width > 0 && renderData->height > 0) {
            // Check if this is actually new data we haven't uploaded yet
            if (renderData != entry.lastUploadedRenderData) {
                // Create texture if it doesn't exist
                if (entry.glTextureId == 0) { glGenTextures(1, &entry.glTextureId); }

                // Upload the pixel data to the texture
                glBindTexture(GL_TEXTURE_2D, entry.glTextureId);

                // Check if we need to reallocate (size changed)
                if (entry.glTextureWidth != renderData->width || entry.glTextureHeight != renderData->height) {
                    entry.glTextureWidth = renderData->width;
                    entry.glTextureHeight = renderData->height;
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, renderData->width, renderData->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                 renderData->pixelData);
                } else {
                    // Same size, just update the data
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, renderData->width, renderData->height, GL_RGBA, GL_UNSIGNED_BYTE,
                                    renderData->pixelData);
                }

                // Set texture parameters
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                // Track which data we uploaded
                entry.lastUploadedRenderData = renderData;
            }
        }

        // Skip if no valid texture
        if (entry.glTextureId == 0) continue;

        // Calculate dimensions
        int croppedW = entry.glTextureWidth - conf->crop_left - conf->crop_right;
        int croppedH = entry.glTextureHeight - conf->crop_top - conf->crop_bottom;
        int displayW = static_cast<int>(croppedW * conf->scale);
        int displayH = static_cast<int>(croppedH * conf->scale);

        // Check if viewport-relative (ends with "Viewport")
        bool isViewportRelative = conf->relativeTo.length() > 8 && conf->relativeTo.substr(conf->relativeTo.length() - 8) == "Viewport";

        int screenX, screenY;

        if (isViewportRelative) {
            // Calculate sizes for FROM and TO viewports
            float toScaleX = (gameW > 0 && gameResW > 0) ? static_cast<float>(gameW) / gameResW : 1.0f;
            float toScaleY = (gameH > 0 && gameResH > 0) ? static_cast<float>(gameH) / gameResH : 1.0f;
            float fromScaleX = (fromW > 0 && gameResW > 0) ? static_cast<float>(fromW) / gameResW : toScaleX;
            float fromScaleY = (fromH > 0 && gameResH > 0) ? static_cast<float>(fromH) / gameResH : toScaleY;

            // Calculate display sizes at FROM and TO
            int toDisplayW = relativeStretching ? static_cast<int>(displayW * toScaleX) : displayW;
            int toDisplayH = relativeStretching ? static_cast<int>(displayH * toScaleY) : displayH;
            int fromDisplayW = relativeStretching ? static_cast<int>(displayW * fromScaleX) : displayW;
            int fromDisplayH = relativeStretching ? static_cast<int>(displayH * fromScaleY) : displayH;

            // Calculate position at TO viewport
            int toPosX, toPosY;
            GetRelativeCoordsForImageWithViewport(conf->relativeTo, conf->x, conf->y, toDisplayW, toDisplayH, gameX, gameY, gameW, gameH,
                                                  fullW, fullH, toPosX, toPosY);

            // Calculate position at FROM viewport
            int fromPosX, fromPosY;
            GetRelativeCoordsForImageWithViewport(conf->relativeTo, conf->x, conf->y, fromDisplayW, fromDisplayH, fromX, fromY, fromW,
                                                  fromH, fullW, fullH, fromPosX, fromPosY);

            // Lerp between FROM and TO positions
            float t = transitionProgress;
            screenX = static_cast<int>(fromPosX + (toPosX - fromPosX) * t);
            screenY = static_cast<int>(fromPosY + (toPosY - fromPosY) * t);

            if (relativeStretching) {
                displayW = static_cast<int>(fromDisplayW + (toDisplayW - fromDisplayW) * t);
                displayH = static_cast<int>(fromDisplayH + (toDisplayH - fromDisplayH) * t);
            }
        } else {
            // Screen-relative: no interpolation needed
            GetRelativeCoordsForImageWithViewport(conf->relativeTo, conf->x, conf->y, displayW, displayH, gameX, gameY, gameW, gameH, fullW,
                                                  fullH, screenX, screenY);
        }

        int screenY_gl = fullH - screenY - displayH;

        float nx1 = (static_cast<float>(screenX) / fullW) * 2.0f - 1.0f;
        float ny1 = (static_cast<float>(screenY_gl) / fullH) * 2.0f - 1.0f;
        float nx2 = (static_cast<float>(screenX + displayW) / fullW) * 2.0f - 1.0f;
        float ny2 = (static_cast<float>(screenY_gl + displayH) / fullH) * 2.0f - 1.0f;

        // Draw background if enabled (matching image overlay behavior)
        if (conf->background.enabled && conf->background.opacity > 0.0f) {
            glUseProgram(rt_solidColorProgram);
            glUniform4f(rt_solidColorShaderLocs.color, conf->background.color.r, conf->background.color.g, conf->background.color.b,
                        conf->background.opacity * modeOpacity);
            float bg_verts[] = { nx1, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny2, 0, 0 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(bg_verts), bg_verts);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        // Draw window overlay
        glUseProgram(rt_imageRenderProgram);
        glBindTexture(GL_TEXTURE_2D, entry.glTextureId);

        // Set texture filtering based on pixelatedScaling config
        if (conf->pixelatedScaling) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }

        glUniform1i(rt_imageRenderShaderLocs.enableColorKey, 0);
        // Apply per-overlay opacity multiplied by mode opacity
        glUniform1f(rt_imageRenderShaderLocs.opacity, conf->opacity * modeOpacity);

        // Texture coordinates with cropping
        float tu1 = static_cast<float>(conf->crop_left) / entry.glTextureWidth;
        float tv1 = static_cast<float>(conf->crop_top) / entry.glTextureHeight;
        float tu2 = static_cast<float>(entry.glTextureWidth - conf->crop_right) / entry.glTextureWidth;
        float tv2 = static_cast<float>(entry.glTextureHeight - conf->crop_bottom) / entry.glTextureHeight;

        float verts[] = { nx1, ny1, tu1, tv2, nx2, ny1, tu2, tv2, nx2, ny2, tu2, tv1,
                          nx1, ny1, tu1, tv2, nx2, ny2, tu2, tv1, nx1, ny2, tu1, tv1 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Render border if enabled (matching RenderWindowOverlaysGL behavior in window_overlay.cpp)
        if (conf->border.enabled && conf->border.width > 0) {
            RT_RenderGameBorder(screenX, screenY, displayW, displayH, conf->border.width, conf->border.radius, conf->border.color, fullW,
                                fullH, vao, vbo);

            // Restore state after border rendering
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
        }

        // Render special focused border if this overlay is currently taking inputs
        std::string focusedName = GetFocusedWindowOverlayName();
        if (!focusedName.empty() && focusedName == overlayId) {
            // Bright green border to indicate focused state
            Color focusedBorderColor = { 0.0f, 1.0f, 0.0f, 1.0f }; // Bright green
            int focusedBorderWidth = 3;
            int focusedBorderRadius = conf->border.enabled ? conf->border.radius : 0;

            RT_RenderGameBorder(screenX, screenY, displayW, displayH, focusedBorderWidth, focusedBorderRadius, focusedBorderColor, fullW,
                                fullH, vao, vbo);

            // Restore state after border rendering
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
        }
    }

    glDisable(GL_BLEND);
}

// Collect active mirrors/images/overlays for a mode from g_config
// This runs on the render thread, moving the work off the main thread
// When onlyOnMyScreenPass is true, only items with onlyOnMyScreen=true are collected
static void RT_CollectActiveElements(const Config& config, const std::string& modeId, bool onlyOnMyScreenPass,
                                     std::vector<MirrorConfig>& outMirrors, std::vector<ImageConfig>& outImages,
                                     std::vector<std::string>& outWindowOverlayIds) {
    outMirrors.clear();
    outImages.clear();
    outWindowOverlayIds.clear();

    // Look up mode by ID
    const ModeConfig* mode = nullptr;
    for (const auto& m : config.modes) {
        if (EqualsIgnoreCase(m.id, modeId)) {
            mode = &m;
            break;
        }
    }
    if (!mode) return;

    // Reserve space upfront
    outMirrors.reserve(mode->mirrorIds.size() + mode->mirrorGroupIds.size());
    outImages.reserve(mode->imageIds.size());
    outWindowOverlayIds.reserve(mode->windowOverlayIds.size());

    // Collect mirrors - use linear search (render thread has more time budget)
    for (const auto& mirrorName : mode->mirrorIds) {
        for (const auto& mirror : config.mirrors) {
            if (mirror.name == mirrorName) {
                // Filter by onlyOnMyScreen if this is an OOMS pass
                if (!onlyOnMyScreenPass || mirror.onlyOnMyScreen) { outMirrors.push_back(mirror); }
                break;
            }
        }
    }

    // Collect mirror groups (override output position for each mirror in the group)
    // Per-item sizing: each mirror in the group has its own widthPercent/heightPercent
    for (const auto& groupName : mode->mirrorGroupIds) {
        for (const auto& group : config.mirrorGroups) {
            if (group.name != groupName) continue;

            for (const auto& item : group.mirrors) {
                if (!item.enabled) continue; // Skip disabled items
                for (const auto& mirror : config.mirrors) {
                    if (mirror.name == item.mirrorId) {
                        if (!onlyOnMyScreenPass || mirror.onlyOnMyScreen) {
                            MirrorConfig groupedMirror = mirror;
                            // Calculate group position - use relative percentages if enabled
                            int groupX = group.output.x;
                            int groupY = group.output.y;
                            if (group.output.useRelativePosition) {
                                int screenW = GetCachedScreenWidth();
                                int screenH = GetCachedScreenHeight();
                                groupX = static_cast<int>(group.output.relativeX * screenW);
                                groupY = static_cast<int>(group.output.relativeY * screenH);
                            }
                            // Position comes from group output settings + per-item offset
                            groupedMirror.output.x = groupX + item.offsetX;
                            groupedMirror.output.y = groupY + item.offsetY;
                            groupedMirror.output.relativeTo = group.output.relativeTo;
                            groupedMirror.output.useRelativePosition = group.output.useRelativePosition;
                            groupedMirror.output.relativeX = group.output.relativeX;
                            groupedMirror.output.relativeY = group.output.relativeY;
                            // Per-item sizing: multiply mirror's own scale by item's widthPercent/heightPercent
                            // Use separate scale when per-item sizing differs from 100%
                            if (item.widthPercent != 1.0f || item.heightPercent != 1.0f) {
                                groupedMirror.output.separateScale = true;
                                float baseScaleX = mirror.output.separateScale ? mirror.output.scaleX : mirror.output.scale;
                                float baseScaleY = mirror.output.separateScale ? mirror.output.scaleY : mirror.output.scale;
                                groupedMirror.output.scaleX = baseScaleX * item.widthPercent;
                                groupedMirror.output.scaleY = baseScaleY * item.heightPercent;
                            }
                            // Otherwise keep mirror's original scale settings
                            outMirrors.push_back(groupedMirror);
                        }
                        break;
                    }
                }
            }
            break;
        }
    }

    // Collect images
    for (const auto& imageName : mode->imageIds) {
        for (const auto& image : config.images) {
            if (image.name == imageName) {
                if (!onlyOnMyScreenPass || image.onlyOnMyScreen) { outImages.push_back(image); }
                break;
            }
        }
    }

    // Collect window overlays
    for (const auto& overlayId : mode->windowOverlayIds) {
        for (const auto& overlay : config.windowOverlays) {
            if (overlay.name == overlayId) {
                if (!onlyOnMyScreenPass || overlay.onlyOnMyScreen) { outWindowOverlayIds.push_back(overlayId); }
                break;
            }
        }
    }
}

static void RenderThreadFunc(void* gameGLContext) {
    _set_se_translator(SEHTranslator);

    try {
        Log("Render Thread: Starting...");

        // Validate pre-created context
        if (!g_renderThreadDC || !g_renderThreadContext) {
            Log("Render Thread: Missing pre-created context or DC");
            g_renderThreadRunning.store(false);
            return;
        }

        // Make context current on this thread
        if (!wglMakeCurrent(g_renderThreadDC, g_renderThreadContext)) {
            Log("Render Thread: Failed to make context current (error " + std::to_string(GetLastError()) + ")");
            g_renderThreadRunning.store(false);
            return;
        }

        // Initialize GLEW on this context
        if (glewInit() != GLEW_OK) {
            Log("Render Thread: GLEW init failed");
            wglMakeCurrent(NULL, NULL);
            g_renderThreadRunning.store(false);
            return;
        }

        LogCategory("init", "Render Thread: Context initialized successfully");

        // Initialize shaders on this context
        if (!RT_InitializeShaders()) {
            Log("Render Thread: Shader initialization failed");
            wglMakeCurrent(NULL, NULL);
            g_renderThreadRunning.store(false);
            return;
        }

        // Initialize Virtual Camera if enabled in config
        auto initCfg = GetConfigSnapshot();
        if (initCfg && initCfg->debug.virtualCameraEnabled) {
            int screenW = GetCachedScreenWidth();
            int screenH = GetCachedScreenHeight();
            int vcW, vcH;
            GetVirtualCamScaledSize(screenW, screenH, 1.0f, vcW, vcH);
            if (StartVirtualCamera(vcW, vcH, initCfg->debug.virtualCameraFps)) {
                LogCategory("init", "Render Thread: Virtual Camera initialized at " + std::to_string(vcW) + "x" + std::to_string(vcH) +
                                        " @ " + std::to_string(initCfg->debug.virtualCameraFps) + "fps");
            } else {
                Log("Render Thread: Virtual Camera initialization failed");
            }
        }

        // Create local VAO/VBO for rendering
        GLuint renderVAO = 0, renderVBO = 0;
        glGenVertexArrays(1, &renderVAO);
        glGenBuffers(1, &renderVBO);
        glBindVertexArray(renderVAO);
        glBindBuffer(GL_ARRAY_BUFFER, renderVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        int lastWidth = 0, lastHeight = 0;

        // Initialize ImGui on render thread
        {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd) {
                IMGUI_CHECKVERSION();
                g_renderThreadImGuiContext = ImGui::CreateContext();
                ImGui::SetCurrentContext(g_renderThreadImGuiContext);

                ImGuiIO& io = ImGui::GetIO();
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

                // Scale based on screen height
                const int screenHeight = GetCachedScreenHeight();
                float scaleFactor = 1.0f;
                if (screenHeight > 1080) { scaleFactor = static_cast<float>(screenHeight) / 1080.0f; }
                scaleFactor = roundf(scaleFactor * 4.0f) / 4.0f;
                if (scaleFactor < 1.0f) { scaleFactor = 1.0f; }
                g_eyeZoomScaleFactor = scaleFactor;

                // Load base font
                auto fontCfg = GetConfigSnapshot();
                if (!fontCfg) {
                    Log("Render Thread: Config snapshot not available for font loading, using defaults");
                    fontCfg = std::make_shared<const Config>(); // Use default Config
                }
                const Config& fontCfgRef = *fontCfg;
                std::string fontPath = fontCfgRef.fontPath;
                io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f * scaleFactor);

                // Load EyeZoom text font (uses custom path if set, otherwise global font)
                std::string eyeZoomFontPath =
                    fontCfgRef.eyezoom.textFontPath.empty() ? fontCfgRef.fontPath : fontCfgRef.eyezoom.textFontPath;
                g_eyeZoomTextFont = io.Fonts->AddFontFromFileTTF(eyeZoomFontPath.c_str(), 80.0f * scaleFactor);
                g_eyeZoomFontPathCached = eyeZoomFontPath;
                if (!g_eyeZoomTextFont) {
                    Log("Render Thread: Failed to load EyeZoom font from " + eyeZoomFontPath + ", using default");
                    g_eyeZoomTextFont = io.Fonts->AddFontDefault();
                }

                ImGui::StyleColorsDark();
                LoadTheme();             // Load theme from theme.toml
                ApplyAppearanceConfig(); // Apply saved theme and custom colors
                ImGui::GetStyle().ScaleAllSizes(scaleFactor);

                // Initialize backends
                ImGui_ImplWin32_Init(hwnd);
                ImGui_ImplOpenGL3_Init("#version 330");

                // Initialize larger font for overlay text labels
                InitializeOverlayTextFont(fontPath, 16.0f, scaleFactor);

                g_renderThreadImGuiInitialized = true;
                LogCategory("init", "Render Thread: ImGui initialized successfully");
            } else {
                LogCategory("init", "Render Thread: HWND not available, ImGui not initialized");
            }
        }

        LogCategory("init", "Render Thread: Entering main loop");

        while (!g_renderThreadShouldStop.load()) {
            // Wait for frame request (lock only held during wait, not during processing)
            FrameRenderRequest request;
            bool isObsRequest = false;

            {
                std::unique_lock<std::mutex> lock(g_requestSignalMutex);
                g_requestCV.wait_for(lock, std::chrono::milliseconds(16), [] {
                    return g_requestPending.load(std::memory_order_acquire) || g_obsSubmissionPending.load(std::memory_order_acquire) ||
                           g_renderThreadShouldStop.load();
                });
            }
            // Lock released - we don't hold it while processing

            if (g_renderThreadShouldStop.load()) break;

            // Check which request types are pending
            bool hasObsRequest = g_obsSubmissionPending.exchange(false, std::memory_order_acq_rel);
            bool hasMainRequest = g_requestPending.exchange(false, std::memory_order_acq_rel);

            if (!hasObsRequest && !hasMainRequest) {
                continue; // Timeout, no request
            }

            // Process OBS request first if pending (virtual camera needs this)
            if (hasObsRequest) {
                PROFILE_SCOPE_CAT("RT Build OBS Request", "Render Thread");
                // Read from the slot that was last written
                int readSlot = 1 - g_obsWriteSlot.load(std::memory_order_relaxed);
                ObsFrameSubmission submission = g_obsSubmissionSlots[readSlot];
                // Build the full request on the render thread (deferred from main thread)
                request = BuildObsFrameRequest(submission.context, submission.isDualRenderingPath);
                request.gameTextureFence = submission.gameTextureFence;
                isObsRequest = true;
            } else {
                // Only main request pending
                int readSlot = 1 - g_requestWriteSlot.load(std::memory_order_relaxed);
                request = g_requestSlots[readSlot];
                isObsRequest = false;
            }

            // Store main request for later if we're processing OBS first
            FrameRenderRequest pendingMainRequest;
            bool hasPendingMain = hasObsRequest && hasMainRequest;
            if (hasPendingMain) {
                int readSlot = 1 - g_requestWriteSlot.load(std::memory_order_relaxed);
                pendingMainRequest = g_requestSlots[readSlot];
            }

        // Label for processing a request (used to process both OBS and main in same iteration)
        process_request:

            auto startTime = std::chrono::high_resolution_clock::now();

            // Grab immutable config snapshot for this frame - all config reads use this
            auto cfgSnapshot = GetConfigSnapshot();
            if (!cfgSnapshot) continue; // Config not yet published, skip frame
            const Config& cfg = *cfgSnapshot;

            // === Image Processing (moved from main thread) ===
            // Process decoded images and upload to GPU
            {
                PROFILE_SCOPE_CAT("RT Image Processing", "Render Thread");
                std::vector<DecodedImageData> imagesToProcess;
                {
                    std::lock_guard<std::mutex> lock(g_decodedImagesMutex);
                    if (!g_decodedImagesQueue.empty()) { imagesToProcess.swap(g_decodedImagesQueue); }
                }
                if (!imagesToProcess.empty()) {
                    for (const auto& decodedImg : imagesToProcess) {
                        UploadDecodedImageToGPU(decodedImg);
                        if (decodedImg.data) { stbi_image_free(decodedImg.data); }
                    }
                }
            }

            // Ensure FBOs are sized correctly
            if (request.fullW != lastWidth || request.fullH != lastHeight) {
                InitRenderFBOs(request.fullW, request.fullH);
                lastWidth = request.fullW;
                lastHeight = request.fullH;
            }

            // Select appropriate FBO set based on request type
            RenderFBO* fboArray;
            std::atomic<int>* writeFBOIndexPtr;
            if (isObsRequest) {
                fboArray = g_obsRenderFBOs;
                writeFBOIndexPtr = &g_obsWriteFBOIndex;
            } else {
                fboArray = g_renderFBOs;
                writeFBOIndexPtr = &g_writeFBOIndex;
            }

            // Get current write FBO
            int writeIdx = writeFBOIndexPtr->load();
            RenderFBO& writeFBO = fboArray[writeIdx];

            // Bind FBO and set viewport
            glBindFramebuffer(GL_FRAMEBUFFER, writeFBO.fbo);
            if (oglViewport)
                oglViewport(0, 0, request.fullW, request.fullH);
            else
                glViewport(0, 0, request.fullW, request.fullH);

            // Clear FBO - for OBS pass use mode background, otherwise transparent
            if (isObsRequest) {
                glDisable(GL_SCISSOR_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                // OBS pass: fill with mode background color first
                glClearColor(request.bgR, request.bgG, request.bgB, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);

                // In raw windowed mode, skip all custom backgrounds - just use black
                // Check if mode uses image background and render it if so
                // When transitioning FROM EyeZoom, use EyeZoom's background (not the target mode's)
                // When transitioning TO Fullscreen, use the from-mode's background (Fullscreen has no background)
                if (!request.isRawWindowedMode) {
                    std::string bgModeId = request.modeId;
                    // If transitioning FROM EyeZoom, use EyeZoom's background instead of target mode
                    if (request.isTransitioningFromEyeZoom) {
                        bgModeId = "EyeZoom";
                    }
                    // If transitioning TO Fullscreen, use the from-mode's background (Fullscreen has no background of its own)
                    else if (EqualsIgnoreCase(request.modeId, "Fullscreen") && !request.fromModeId.empty()) {
                        bgModeId = request.fromModeId;
                    }

                    const ModeConfig* mode = nullptr;
                    for (const auto& m : cfg.modes) {
                        if (EqualsIgnoreCase(m.id, bgModeId)) {
                            mode = &m;
                            break;
                        }
                    }

                    if (mode && mode->background.selectedMode == "gradient" && mode->background.gradientStops.size() >= 2) {
                        // Render gradient background fullscreen
                        glUseProgram(rt_gradientProgram);
                        glBindVertexArray(renderVAO);
                        glBindBuffer(GL_ARRAY_BUFFER, renderVBO);

                        // Set gradient uniforms
                        int numStops = (std::min)(static_cast<int>(mode->background.gradientStops.size()), 8);
                        glUniform1i(rt_gradientShaderLocs.numStops, numStops);

                        float colors[8 * 4]; // 4 components per color
                        float positions[8];
                        for (int i = 0; i < numStops; i++) {
                            colors[i * 4 + 0] = mode->background.gradientStops[i].color.r;
                            colors[i * 4 + 1] = mode->background.gradientStops[i].color.g;
                            colors[i * 4 + 2] = mode->background.gradientStops[i].color.b;
                            colors[i * 4 + 3] = 1.0f; // Full opacity for OBS
                            positions[i] = mode->background.gradientStops[i].position;
                        }
                        glUniform4fv(rt_gradientShaderLocs.stopColors, numStops, colors);
                        glUniform1fv(rt_gradientShaderLocs.stopPositions, numStops, positions);
                        glUniform1f(rt_gradientShaderLocs.angle, mode->background.gradientAngle * 3.14159265f / 180.0f);

                        // Animation uniforms
                        static auto startTime = std::chrono::steady_clock::now();
                        auto now = std::chrono::steady_clock::now();
                        float timeSeconds = std::chrono::duration<float>(now - startTime).count();
                        glUniform1f(rt_gradientShaderLocs.time, timeSeconds);
                        glUniform1i(rt_gradientShaderLocs.animationType, static_cast<int>(mode->background.gradientAnimation));
                        glUniform1f(rt_gradientShaderLocs.animationSpeed, mode->background.gradientAnimationSpeed);
                        glUniform1i(rt_gradientShaderLocs.colorFade, mode->background.gradientColorFade ? 1 : 0);

                        // Fullscreen quad vertices
                        float bgVerts[] = { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,  1.0f, 1.0f, 1.0f,
                                            -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f };
                        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(bgVerts), bgVerts);
                        glDrawArrays(GL_TRIANGLES, 0, 6);
                    } else if (mode && mode->background.selectedMode == "image") {
                        std::lock_guard<std::mutex> bgLock(g_backgroundTexturesMutex);
                        auto bgTexIt = g_backgroundTextures.find(bgModeId);
                        if (bgTexIt != g_backgroundTextures.end()) {
                            BackgroundTextureInstance& bgInst = bgTexIt->second;

                            // Advance animation frame if animated - using time-based approach for smooth playback
                            if (bgInst.isAnimated && !bgInst.frameTextures.empty()) {
                                auto now = std::chrono::steady_clock::now();
                                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - bgInst.lastFrameTime).count();
                                int delay = bgInst.frameDelays.empty() ? 100 : bgInst.frameDelays[bgInst.currentFrame];
                                if (delay < 10) delay = 100;
                                // Advance multiple frames if needed to keep animation in sync with real time
                                while (elapsed >= delay) {
                                    elapsed -= delay;
                                    bgInst.currentFrame = (bgInst.currentFrame + 1) % bgInst.frameTextures.size();
                                    delay = bgInst.frameDelays.empty() ? 100 : bgInst.frameDelays[bgInst.currentFrame];
                                    if (delay < 10) delay = 100;
                                }
                                bgInst.textureId = bgInst.frameTextures[bgInst.currentFrame];
                                // Adjust lastFrameTime by remaining elapsed to maintain accuracy
                                bgInst.lastFrameTime = now - std::chrono::milliseconds(elapsed);
                            }

                            GLuint bgTex = bgInst.textureId;
                            if (bgTex != 0) {
                                // Render background image fullscreen
                                glUseProgram(rt_backgroundProgram);
                                glBindVertexArray(renderVAO);
                                glBindBuffer(GL_ARRAY_BUFFER, renderVBO);
                                glActiveTexture(GL_TEXTURE0);
                                glBindTexture(GL_TEXTURE_2D, bgTex);
                                glUniform1i(rt_backgroundShaderLocs.backgroundTexture, 0);
                                glUniform1f(rt_backgroundShaderLocs.opacity, 1.0f);

                                // Fullscreen quad vertices
                                float bgVerts[] = { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,  1.0f, 1.0f, 1.0f,
                                                    -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f };
                                glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(bgVerts), bgVerts);
                                glDrawArrays(GL_TRIANGLES, 0, 6);
                            }
                        }
                    }
                } // end if (!request.isRawWindowedMode)

                // Use the READY frame texture - guaranteed complete by mirror thread
                // No fence wait needed - mirror thread already waited on the fence
                // This works even if no mirrors exist, as the ready frame is published
                // immediately after fence signals in Phase 1 of mirror thread loop
                GLuint readyTex = GetReadyGameTexture();
                int srcW = GetReadyGameWidth();
                int srcH = GetReadyGameHeight();

                // Fallback: if ready frame not available, use the safe read texture
                // GetSafeReadTexture returns the texture NOT being written to (always valid, no fence needed)
                // This may be 1 frame behind but won't flicker
                if (readyTex == 0 || srcW <= 0 || srcH <= 0) {
                    GLuint safeTex = GetSafeReadTexture();
                    if (safeTex != 0) {
                        readyTex = safeTex;
                        // Use fallback dimensions (these track the last completed frame)
                        srcW = GetFallbackGameWidth();
                        srcH = GetFallbackGameHeight();
                        // If dimensions not available, use request dimensions as last resort
                        if (srcW <= 0 || srcH <= 0) {
                            srcW = request.fullW;
                            srcH = request.fullH;
                        }
                    }
                }

                if (readyTex != 0 && srcW > 0 && srcH > 0) {
                    // For pre-1.13 windowed mode, the texture contains fullscreen-sized data but
                    // the actual game content is only in the top-left window-sized portion.
                    // Use window dimensions for srcGameW/H to sample only the content portion.
                    int uvSrcW = srcW;
                    int uvSrcH = srcH;
                    if (request.isPre113Windowed && request.windowW > 0 && request.windowH > 0) {
                        uvSrcW = request.windowW;
                        uvSrcH = request.windowH;
                    }

                    // No fence wait needed for ready frame - frame is guaranteed complete
                    RT_RenderGameTexture(readyTex, request.animatedX, request.animatedY, request.animatedW, request.animatedH,
                                         request.fullW, request.fullH, uvSrcW, uvSrcH, srcW,
                                         srcH, // For ready frame, content size may differ from texture size
                                         renderVAO, renderVBO);

                    // Render mode border around the game viewport (after game texture, before overlays)
                    // Uses from-mode border when transitioning TO Fullscreen, otherwise uses current mode's border
                    // Skip borders in raw windowed mode
                    if (!request.isRawWindowedMode && request.transitioningToFullscreen && request.fromBorderEnabled &&
                        request.fromBorderWidth > 0) {
                        Color fromBorderColor = { request.fromBorderR, request.fromBorderG, request.fromBorderB, 1.0f };
                        RT_RenderGameBorder(request.animatedX, request.animatedY, request.animatedW, request.animatedH,
                                            request.fromBorderWidth, request.fromBorderRadius, fromBorderColor, request.fullW,
                                            request.fullH, renderVAO, renderVBO);
                    } else if (!request.isRawWindowedMode && request.borderEnabled && request.borderWidth > 0) {
                        Color borderColor = { request.borderR, request.borderG, request.borderB, 1.0f };
                        RT_RenderGameBorder(request.animatedX, request.animatedY, request.animatedW, request.animatedH, request.borderWidth,
                                            request.borderRadius, borderColor, request.fullW, request.fullH, renderVAO, renderVBO);
                    }

                    // Render EyeZoom overlay for OBS if enabled (skip in raw windowed mode)
                    if (!request.isRawWindowedMode && request.showEyeZoom) {
                        // Pass animated viewport X directly - RT_RenderEyeZoom handles -1 by calculating target position
                        // Also pass snapshot texture for transition-out consistency
                        RT_RenderEyeZoom(readyTex, request.eyeZoomAnimatedViewportX, request.fullW, request.fullH, srcW, srcH, renderVAO,
                                         renderVBO, request.isTransitioningFromEyeZoom, request.eyeZoomSnapshotTexture,
                                         request.eyeZoomSnapshotWidth, request.eyeZoomSnapshotHeight);
                    }
                }
                // If no ready frame and no fallback available, just show background (first few frames at startup)

                // Clean up the game fence (we may have used it above for fallback)
                if (request.gameTextureFence) { glDeleteSync(request.gameTextureFence); }
            } else {
                // Non-OBS pass: transparent background so overlays composite on top of game
                // Background/border rendering is done on main thread (render.cpp), we only render overlays here
                glDisable(GL_SCISSOR_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }

            // Create geometry struct for rendering functions - use animated position for OBS
            GameViewportGeometry geo;
            geo.gameW = request.gameW;
            geo.gameH = request.gameH;
            if (isObsRequest) {
                // OBS: use animated position for overlay placement
                geo.finalX = request.animatedX;
                geo.finalY = request.animatedY;
                geo.finalW = request.animatedW;
                geo.finalH = request.animatedH;
            } else {
                geo.finalX = request.finalX;
                geo.finalY = request.finalY;
                geo.finalW = request.finalW;
                geo.finalH = request.finalH;
            }

            // Collect active elements from g_config (this work moved from main thread)
            std::vector<MirrorConfig> activeMirrors;
            std::vector<ImageConfig> activeImages;
            std::vector<std::string> activeWindowOverlayIds;
            {
                PROFILE_SCOPE_CAT("RT Collect Active Elements", "Render Thread");
                RT_CollectActiveElements(cfg, request.modeId, false, activeMirrors, activeImages, activeWindowOverlayIds);
            }

            StrongholdOverlayRenderSnapshot strongholdOverlaySnap = GetStrongholdOverlayRenderSnapshot();
            bool shouldRenderStrongholdOverlay = strongholdOverlaySnap.enabled && strongholdOverlaySnap.visible &&
                                                strongholdOverlaySnap.renderInGameOverlay &&
                                                RT_ShouldRenderStrongholdOverlayOnCurrentMonitor(strongholdOverlaySnap);
            McsrApiTrackerRenderSnapshot mcsrApiTrackerSnap = GetMcsrApiTrackerRenderSnapshot();
            bool shouldRenderMcsrApiTracker =
                mcsrApiTrackerSnap.enabled && mcsrApiTrackerSnap.visible && mcsrApiTrackerSnap.renderInGameOverlay;
            bool shouldRenderNotesOverlay = HasNotesOverlayPendingWork();

            // Check if we need to render any ImGui content
            bool shouldRenderAnyImGui = request.shouldRenderGui || request.showPerformanceOverlay || request.showProfiler ||
                                        request.showEyeZoom || request.showTextureGrid || shouldRenderStrongholdOverlay ||
                                        shouldRenderMcsrApiTracker ||
                                        shouldRenderNotesOverlay;

            // Lazy-init ImGui the first time we actually need to render it.
            // Some systems can start the render thread before a valid HWND is published,
            // which previously meant the GUI never initialized (Ctrl+I would do nothing, then ESC could crash).
            if (!g_renderThreadImGuiInitialized && shouldRenderAnyImGui) {
                HWND hwnd = g_minecraftHwnd.load();
                if (hwnd) { RT_TryInitializeImGui(hwnd, cfg); }
            }

            // Early exit if nothing to render
            // BUT don't early exit if we need to render ImGui or the welcome toast (raw OpenGL)
            if (activeMirrors.empty() && activeImages.empty() && activeWindowOverlayIds.empty() && !shouldRenderAnyImGui &&
                !request.showWelcomeToast) {
                // Still need to advance FBO and signal completion even if empty
                // Create fence for synchronization
                GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                glFlush();

                writeFBO.frameNumber = request.frameNumber;

                // Update last good texture and fence atomically
                // The main thread will wait on the fence before using the texture
                if (isObsRequest) {
                    // Exchange fences - delete the OLDEST pending fence, not the one just swapped out
                    GLsync oldFence = g_lastGoodObsFence.exchange(fence, std::memory_order_acq_rel);
                    // Deferred deletion: delete the fence from 2 cycles ago, store current old fence
                    if (g_pendingDeleteObsFences[g_pendingDeleteObsIndex]) {
                        glDeleteSync(g_pendingDeleteObsFences[g_pendingDeleteObsIndex]);
                    }
                    g_pendingDeleteObsFences[g_pendingDeleteObsIndex] = oldFence;
                    g_pendingDeleteObsIndex = (g_pendingDeleteObsIndex + 1) % FENCE_DELETION_DELAY;
                    g_lastGoodObsTexture.store(writeFBO.texture, std::memory_order_release);
                } else {
                    // Exchange fences - delete the OLDEST pending fence, not the one just swapped out
                    GLsync oldFence = g_lastGoodFence.exchange(fence, std::memory_order_acq_rel);
                    // Deferred deletion: delete the fence from 2 cycles ago, store current old fence
                    if (g_pendingDeleteFences[g_pendingDeleteIndex]) { glDeleteSync(g_pendingDeleteFences[g_pendingDeleteIndex]); }
                    g_pendingDeleteFences[g_pendingDeleteIndex] = oldFence;
                    g_pendingDeleteIndex = (g_pendingDeleteIndex + 1) % FENCE_DELETION_DELAY;
                    g_lastGoodTexture.store(writeFBO.texture, std::memory_order_release);
                }

                if (isObsRequest) {
                    AdvanceObsFBO();
                    {
                        std::lock_guard<std::mutex> lock(g_obsCompletionMutex);
                        g_obsFrameComplete.store(true);
                    }
                    g_obsCompletionCV.notify_one();
                } else {
                    AdvanceWriteFBO();
                    g_renderFrameNumber.store(request.frameNumber);
                    {
                        std::lock_guard<std::mutex> lock(g_completionMutex);
                        g_frameComplete.store(true);
                    }
                    g_completionCV.notify_one();
                }
                continue;
            }

            // excludeOnlyOnMyScreen controls filtering
            bool excludeOoms = request.excludeOnlyOnMyScreen;

            // Render EyeZoom for non-OBS passes (OBS already renders EyeZoom above)
            // This ensures EyeZoom boxes and text are in the same FBO, synchronized
            // Use ready frame texture from mirror thread for synchronized, flicker-free capture
            if (!isObsRequest && request.showEyeZoom) {
                GLuint readyTex = GetReadyGameTexture();
                int srcW = GetReadyGameWidth();
                int srcH = GetReadyGameHeight();

                if (readyTex != 0 && srcW > 0 && srcH > 0) {
                    PROFILE_SCOPE_CAT("RT EyeZoom Render", "Render Thread");
                    RT_RenderEyeZoom(readyTex, request.eyeZoomAnimatedViewportX, request.fullW, request.fullH, srcW, srcH, renderVAO,
                                     renderVBO, request.isTransitioningFromEyeZoom, request.eyeZoomSnapshotTexture,
                                     request.eyeZoomSnapshotWidth, request.eyeZoomSnapshotHeight);
                }
            }

            // Render mirrors using local shaders (skip in raw windowed mode)
            if (!request.isRawWindowedMode && !activeMirrors.empty()) {
                PROFILE_SCOPE_CAT("RT Mirror Render", "Render Thread");
                // Swap ready buffers from capture thread (done on render thread to avoid main thread locks)
                // This must happen before reading mirror textures
                SwapMirrorBuffers();

                // Determine if we're in EyeZoom mode (for the collected mirrors)
                bool isEyeZoomMode = (request.modeId == "EyeZoom");

                RT_RenderMirrors(activeMirrors, geo, request.fullW, request.fullH, request.overlayOpacity, excludeOoms,
                                 request.relativeStretching, request.transitionProgress, request.mirrorSlideProgress, request.fromX,
                                 request.fromY, request.fromW, request.fromH, request.toX, request.toY, request.toW, request.toH,
                                 isEyeZoomMode, request.isTransitioningFromEyeZoom, request.eyeZoomAnimatedViewportX, request.skipAnimation,
                                 request.fromModeId, request.fromSlideMirrorsIn, request.toSlideMirrorsIn, false /* isSlideOutPass */,
                                 renderVAO, renderVBO);
            }

            // When transitioning FROM EyeZoom, also render EyeZoom-specific mirrors with slide-out animation
            // These mirrors are NOT in the target mode's mirror list, so they need a separate render pass
            // Skip this pass entirely when skipAnimation is true - mirrors should disappear immediately
            // Also skip in raw windowed mode - no overlays
            if (!request.isRawWindowedMode && request.isTransitioningFromEyeZoom && cfg.eyezoom.slideMirrorsIn && !request.skipAnimation) {
                PROFILE_SCOPE_CAT("RT EyeZoom Mirror Slide Out", "Render Thread");

                // Collect EyeZoom mirrors (not the target mode's mirrors)
                std::vector<MirrorConfig> eyeZoomMirrors;
                std::vector<ImageConfig> unusedImages;
                std::vector<std::string> unusedOverlays;
                RT_CollectActiveElements(cfg, "EyeZoom", false, eyeZoomMirrors, unusedImages, unusedOverlays);

                // Filter out mirrors that already exist in the target mode (don't slide those out)
                std::vector<MirrorConfig> mirrorsToSlideOut;
                for (const auto& ezMirror : eyeZoomMirrors) {
                    bool existsInTarget = false;
                    for (const auto& targetMirror : activeMirrors) {
                        if (targetMirror.name == ezMirror.name) {
                            existsInTarget = true;
                            break;
                        }
                    }
                    if (!existsInTarget) { mirrorsToSlideOut.push_back(ezMirror); }
                }

                if (!mirrorsToSlideOut.empty()) {
                    // Render these EyeZoom mirrors with slide-out animation
                    // isEyeZoomMode=true because these ARE EyeZoom mirrors
                    // Pass request.modeId as fromModeId - this is the target mode we're transitioning TO
                    RT_RenderMirrors(mirrorsToSlideOut, geo, request.fullW, request.fullH, request.overlayOpacity, excludeOoms,
                                     request.relativeStretching, request.transitionProgress, request.mirrorSlideProgress, request.fromX,
                                     request.fromY, request.fromW, request.fromH, request.toX, request.toY, request.toW, request.toH, true,
                                     request.isTransitioningFromEyeZoom, request.eyeZoomAnimatedViewportX, request.skipAnimation,
                                     request.modeId, cfg.eyezoom.slideMirrorsIn, request.toSlideMirrorsIn, true /* isSlideOutPass */,
                                     renderVAO, renderVBO);
                }
            }

            // When transitioning FROM a mode with slideMirrorsIn (non-EyeZoom), render slide-out animation
            // for mirrors unique to the FROM mode
            // Skip animation when hideAnimationsInGame is enabled (skipAnimation flag)
            if (!request.isTransitioningFromEyeZoom && request.fromSlideMirrorsIn && !request.fromModeId.empty() &&
                request.mirrorSlideProgress < 1.0f && !request.skipAnimation) {
                PROFILE_SCOPE_CAT("RT Generic Mirror Slide Out", "Render Thread");

                // Collect FROM mode mirrors
                std::vector<MirrorConfig> fromModeMirrors;
                std::vector<ImageConfig> unusedImages;
                std::vector<std::string> unusedOverlays;
                RT_CollectActiveElements(cfg, request.fromModeId, false, fromModeMirrors, unusedImages, unusedOverlays);

                // Filter out mirrors that already exist in the target mode (don't slide those out)
                std::vector<MirrorConfig> mirrorsToSlideOut;
                for (const auto& fromMirror : fromModeMirrors) {
                    bool existsInTarget = false;
                    for (const auto& targetMirror : activeMirrors) {
                        if (targetMirror.name == fromMirror.name) {
                            existsInTarget = true;
                            break;
                        }
                    }
                    if (!existsInTarget) { mirrorsToSlideOut.push_back(fromMirror); }
                }

                if (!mirrorsToSlideOut.empty()) {
                    // Render these mirrors with slide-out animation
                    RT_RenderMirrors(mirrorsToSlideOut, geo, request.fullW, request.fullH, request.overlayOpacity, excludeOoms,
                                     request.relativeStretching, request.transitionProgress, request.mirrorSlideProgress, request.fromX,
                                     request.fromY, request.fromW, request.fromH, request.toX, request.toY, request.toW, request.toH, false,
                                     false, -1, request.skipAnimation, request.modeId, request.fromSlideMirrorsIn, request.toSlideMirrorsIn,
                                     true /* isSlideOutPass */, renderVAO, renderVBO);
                }
            }

            // Render images using local shaders (skip in raw windowed mode)
            if (!request.isRawWindowedMode && !activeImages.empty()) {
                PROFILE_SCOPE_CAT("RT Image Render", "Render Thread");
                RT_RenderImages(activeImages, request.fullW, request.fullH, request.toX, request.toY, request.toW, request.toH,
                                request.gameW, request.gameH, request.relativeStretching, request.transitionProgress, request.fromX,
                                request.fromY, request.fromW, request.fromH, request.overlayOpacity, excludeOoms, renderVAO, renderVBO);
            }

            // Render window overlays using local shaders
            if (!activeWindowOverlayIds.empty()) {
                PROFILE_SCOPE_CAT("RT Window Overlay Render", "Render Thread");
                RT_RenderWindowOverlays(activeWindowOverlayIds, request.fullW, request.fullH, request.toX, request.toY, request.toW,
                                        request.toH, request.gameW, request.gameH, request.relativeStretching, request.transitionProgress,
                                        request.fromX, request.fromY, request.fromW, request.fromH, request.overlayOpacity, excludeOoms,
                                        renderVAO, renderVBO);
            }

            // Render ImGui to overlay FBO (if enabled) - runs every frame when any overlay is active
            // Note: shouldRenderAnyImGui was computed earlier (before the early exit check)
            if (g_renderThreadImGuiInitialized && shouldRenderAnyImGui) {
                PROFILE_SCOPE_CAT("RT ImGui Render", "Render Thread");

                ImGui::SetCurrentContext(g_renderThreadImGuiContext);

                // Check if EyeZoom font needs to be reloaded (hot-reload support)
                if (g_eyeZoomFontNeedsReload.exchange(false)) {
                    std::string newFontPath = cfg.eyezoom.textFontPath.empty() ? cfg.fontPath : cfg.eyezoom.textFontPath;

                    if (newFontPath != g_eyeZoomFontPathCached) {
                        Log("Render Thread: Reloading EyeZoom font from " + newFontPath);
                        ImGuiIO& io = ImGui::GetIO();

                        // Add the new font to the atlas
                        ImFont* newFont = io.Fonts->AddFontFromFileTTF(newFontPath.c_str(), 80.0f * g_eyeZoomScaleFactor);
                        if (newFont) {
                            g_eyeZoomTextFont = newFont;
                            g_eyeZoomFontPathCached = newFontPath;

                            // Rebuild font atlas - new ImGui handles texture upload automatically
                            io.Fonts->Build();
                            Log("Render Thread: EyeZoom font reloaded successfully");
                        } else {
                            Log("Render Thread: Failed to load EyeZoom font from " + newFontPath);
                        }
                    }
                }

                // Check if HWND changed (fullscreen toggle in MC < 1.13.0)
                // If so, reinitialize ImGui Win32 backend with the new HWND
                if (g_hwndChanged.exchange(false)) {
                    HWND newHwnd = g_minecraftHwnd.load();
                    if (newHwnd != NULL) {
                        Log("Render Thread: HWND changed, reinitializing ImGui Win32 backend");
                        ImGui_ImplWin32_Shutdown();
                        ImGui_ImplWin32_Init(newHwnd);
                    }
                }

                // Start ImGui frame
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();

                // Render texture grid if enabled
                if (request.showTextureGrid) {
                    RenderTextureGridOverlay(true, request.textureGridModeWidth, request.textureGridModeHeight);
                }

                // Render eye zoom text labels directly for all request types
                // Boxes and text are now both rendered in the same FBO using the same request values,
                // so they stay synchronized during transitions
                if (request.showEyeZoom && request.eyeZoomFadeOpacity > 0.0f) {
                    // Calculate EyeZoom text positions directly
                    EyeZoomConfig zoomConfig = cfg.eyezoom;

                    // Calculate target position
                    int modeWidth = zoomConfig.windowWidth;
                    int targetViewportX = (request.fullW - modeWidth) / 2;

                    // Use request.eyeZoomAnimatedViewportX - this already accounts for hideAnimationsInGame
                    // (caller sets to -1 when skipAnimation is true, meaning use target position)
                    int viewportX = (request.eyeZoomAnimatedViewportX >= 0) ? request.eyeZoomAnimatedViewportX : targetViewportX;

                    // Calculate dimensions and position - must match RT_RenderEyeZoom logic
                    int zoomOutputWidth, zoomX;
                    bool isTransitioningFromEyeZoom = g_isTransitioningFromEyeZoom.load(std::memory_order_relaxed);
                    bool isTransitioningToEyeZoom = (viewportX < targetViewportX && !isTransitioningFromEyeZoom);

                    if (zoomConfig.slideZoomIn) {
                        // SLIDE MODE: Full size, sliding X position
                        zoomOutputWidth = targetViewportX - (2 * zoomConfig.horizontalMargin);
                        int finalZoomX = zoomConfig.horizontalMargin;
                        int offScreenX = -zoomOutputWidth;

                        if (isTransitioningToEyeZoom && targetViewportX > 0) {
                            // Sliding IN
                            float progress = (float)viewportX / (float)targetViewportX;
                            zoomX = offScreenX + (int)((finalZoomX - offScreenX) * progress);
                        } else if (isTransitioningFromEyeZoom && targetViewportX > 0) {
                            // Sliding OUT
                            float progress = (float)viewportX / (float)targetViewportX;
                            zoomX = offScreenX + (int)((finalZoomX - offScreenX) * progress);
                        } else {
                            zoomX = finalZoomX;
                        }
                    } else {
                        // GROW MODE: Growing size, fixed X position
                        zoomOutputWidth = viewportX - (2 * zoomConfig.horizontalMargin);
                        zoomX = zoomConfig.horizontalMargin;
                    }

                    if (viewportX > 0 && zoomOutputWidth > 20) {

                        int zoomOutputHeight = request.fullH - (2 * zoomConfig.verticalMargin);
                        int minHeight = (int)(0.2f * request.fullH);
                        if (zoomOutputHeight < minHeight) zoomOutputHeight = minHeight;

                        int zoomY = zoomConfig.verticalMargin;

                        // Calculate per-box width based on the actual output width
                        float pixelWidthOnScreen = zoomOutputWidth / (float)zoomConfig.cloneWidth;
                        int labelsPerSide = zoomConfig.cloneWidth / 2;
                        float centerY = zoomY + zoomOutputHeight / 2.0f;

                        ImDrawList* drawList = request.shouldRenderGui ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
                        float fontSize = (float)zoomConfig.textFontSize;
                        // Combine textColorOpacity with the fade opacity
                        float finalTextAlpha = zoomConfig.textColorOpacity * request.eyeZoomFadeOpacity;
                        ImU32 textColor =
                            IM_COL32(static_cast<int>(zoomConfig.textColor.r * 255), static_cast<int>(zoomConfig.textColor.g * 255),
                                     static_cast<int>(zoomConfig.textColor.b * 255), static_cast<int>(finalTextAlpha * 255));

                        // Get the font to use for rendering (use EyeZoom-specific font if available)
                        ImFont* font = g_eyeZoomTextFont ? g_eyeZoomTextFont : ImGui::GetFont();

                        int boxIndex = 0;
                        for (int xOffset = -labelsPerSide; xOffset <= labelsPerSide; xOffset++) {
                            if (xOffset == 0) continue;

                            float boxLeft = zoomX + (boxIndex * pixelWidthOnScreen);
                            boxIndex++;

                            int displayNumber = abs(xOffset);
                            std::string text = std::to_string(displayNumber);

                            // Use font->CalcTextSizeA with the configured fontSize for proper sizing
                            ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
                            float numberCenterX = boxLeft + pixelWidthOnScreen / 2.0f;
                            float numberCenterY = centerY;
                            ImVec2 textPos(numberCenterX - textSize.x / 2.0f, numberCenterY - textSize.y / 2.0f);

                            // Use AddText overload with font and fontSize to render at configured size
                            drawList->AddText(font, fontSize, textPos, textColor, text.c_str());
                        }
                    }
                }

                if (shouldRenderStrongholdOverlay) {
                    RT_RenderStrongholdOverlayImGui(strongholdOverlaySnap, request.shouldRenderGui);
                }
                if (shouldRenderMcsrApiTracker) { RT_RenderMcsrApiTrackerOverlayImGui(mcsrApiTrackerSnap, request.shouldRenderGui); }

                if (shouldRenderNotesOverlay) { RenderNotesOverlayImGui(); }

                // Render texture grid labels
                RenderCachedTextureGridLabels();

                // Render main settings GUI if visible
                if (request.shouldRenderGui) { RenderSettingsGUI(); }

                // Render performance overlay
                RenderPerformanceOverlay(request.showPerformanceOverlay);

                // Render profiler
                RenderProfilerOverlay(request.showProfiler, request.showPerformanceOverlay);

                ImGui::Render();
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            }

            // Render welcome toast AFTER ImGui (raw OpenGL, renders on top of everything)
            if (request.showWelcomeToast) { RenderWelcomeToast(request.welcomeToastIsFullscreen); }

            // Create fence to signal when GPU completes all rendering commands
            // NOTE: Cursor is NOT rendered here - it's rendered separately below for virtual camera only
            GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

            // Flush to ensure commands are submitted to GPU
            glFlush();

            // Store frame number
            writeFBO.frameNumber = request.frameNumber;

            // Update last good texture and fence atomically
            // The main thread will wait on the fence before using the texture
            if (isObsRequest) {
                // Exchange fences - delete the OLDEST pending fence, not the one just swapped out
                GLsync oldFence = g_lastGoodObsFence.exchange(fence, std::memory_order_acq_rel);
                // Deferred deletion: delete the fence from 2 cycles ago, store current old fence
                if (g_pendingDeleteObsFences[g_pendingDeleteObsIndex]) { glDeleteSync(g_pendingDeleteObsFences[g_pendingDeleteObsIndex]); }
                g_pendingDeleteObsFences[g_pendingDeleteObsIndex] = oldFence;
                g_pendingDeleteObsIndex = (g_pendingDeleteObsIndex + 1) % FENCE_DELETION_DELAY;
                g_lastGoodObsTexture.store(writeFBO.texture, std::memory_order_release);

                // Virtual Camera: render cursor onto a SEPARATE staging texture so it doesn't
                // appear on game capture (which reads g_lastGoodObsTexture directly)
                if (IsVirtualCameraActive()) {
                    int vcW = request.fullW;
                    int vcH = request.fullH;

                    // Ensure staging FBO/texture exists and is correct size
                    if (g_vcCursorFBO == 0 || g_vcCursorWidth != vcW || g_vcCursorHeight != vcH) {
                        if (g_vcCursorTexture != 0) { glDeleteTextures(1, &g_vcCursorTexture); }
                        if (g_vcCursorFBO == 0) { glGenFramebuffers(1, &g_vcCursorFBO); }

                        glGenTextures(1, &g_vcCursorTexture);
                        glBindTexture(GL_TEXTURE_2D, g_vcCursorTexture);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vcW, vcH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        glBindTexture(GL_TEXTURE_2D, 0);

                        glBindFramebuffer(GL_FRAMEBUFFER, g_vcCursorFBO);
                        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vcCursorTexture, 0);
                        glBindFramebuffer(GL_FRAMEBUFFER, 0);

                        g_vcCursorWidth = vcW;
                        g_vcCursorHeight = vcH;
                    }

                    // Blit OBS texture to staging texture
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, writeFBO.fbo);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_vcCursorFBO);
                    glBlitFramebuffer(0, 0, vcW, vcH, 0, 0, vcW, vcH, GL_COLOR_BUFFER_BIT, GL_NEAREST);

                    // Render cursor onto the staging texture
                    glBindFramebuffer(GL_FRAMEBUFFER, g_vcCursorFBO);
                    if (oglViewport)
                        oglViewport(0, 0, vcW, vcH);
                    else
                        glViewport(0, 0, vcW, vcH);

                    int viewportX = request.isWindowed ? request.animatedX : 0;
                    int viewportY = request.isWindowed ? request.animatedY : 0;
                    int viewportW = request.isWindowed ? request.animatedW : vcW;
                    int viewportH = request.isWindowed ? request.animatedH : vcH;
                    int windowW = request.isWindowed ? request.windowW : vcW;
                    int windowH = request.isWindowed ? request.windowH : vcH;

                    RT_RenderCursorForObs(vcW, vcH, viewportX, viewportY, viewportW, viewportH, windowW, windowH, renderVAO, renderVBO);

                    // Rebind the write FBO
                    glBindFramebuffer(GL_FRAMEBUFFER, writeFBO.fbo);

                    // Start async readback from the staging texture (with cursor)
                    StartVirtualCameraAsyncReadback(g_vcCursorTexture, vcW, vcH);
                } else {
                    // No virtual camera - no cursor rendering needed
                }
            } else {
                // Exchange fences - delete the OLDEST pending fence, not the one just swapped out
                GLsync oldFence = g_lastGoodFence.exchange(fence, std::memory_order_acq_rel);
                // Deferred deletion: delete the fence from 2 cycles ago, store current old fence
                if (g_pendingDeleteFences[g_pendingDeleteIndex]) { glDeleteSync(g_pendingDeleteFences[g_pendingDeleteIndex]); }
                g_pendingDeleteFences[g_pendingDeleteIndex] = oldFence;
                g_pendingDeleteIndex = (g_pendingDeleteIndex + 1) % FENCE_DELETION_DELAY;
                g_lastGoodTexture.store(writeFBO.texture, std::memory_order_release);

                // NOTE: Virtual Camera readback is NOT called here because the non-OBS path
                // only renders overlays with transparent background (no game texture).
                // Virtual camera is only fed from the OBS path which has the full game + overlays.
            }

            // Advance to next FBO and signal completion
            if (isObsRequest) {
                AdvanceObsFBO();
                {
                    std::lock_guard<std::mutex> lock(g_obsCompletionMutex);
                    g_obsFrameComplete.store(true);
                }
                g_obsCompletionCV.notify_one();
            } else {
                AdvanceWriteFBO();
                g_renderFrameNumber.store(request.frameNumber);
                {
                    std::lock_guard<std::mutex> lock(g_completionMutex);
                    g_frameComplete.store(true);
                }
                g_completionCV.notify_one();
            }

            // If we processed OBS first and there was also a main request pending, process it now
            // This prevents user's screen overlays from being starved when virtual camera is active
            if (hasPendingMain) {
                request = pendingMainRequest;
                isObsRequest = false;
                hasPendingMain = false; // Don't loop forever
                goto process_request;
            }

            // Update statistics
            {
                auto endTime = std::chrono::high_resolution_clock::now();
                double renderTime = std::chrono::duration<double, std::milli>(endTime - startTime).count();
                g_lastRenderTimeMs.store(renderTime);

                // Running average
                double avg = g_avgRenderTimeMs.load();
                g_avgRenderTimeMs.store(avg * 0.95 + renderTime * 0.05);

                g_framesRendered.fetch_add(1);
            }
        }

        Log("Render Thread: Cleaning up...");

        // Cleanup
        RT_CleanupShaders();
        CleanupRenderFBOs();
        if (renderVAO) glDeleteVertexArrays(1, &renderVAO);
        if (renderVBO) glDeleteBuffers(1, &renderVBO);

        // Shutdown ImGui
        if (g_renderThreadImGuiInitialized) {
            ImGui::SetCurrentContext(g_renderThreadImGuiContext);
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(g_renderThreadImGuiContext);
            g_renderThreadImGuiContext = nullptr;
            g_renderThreadImGuiInitialized = false;
            Log("Render Thread: ImGui shutdown complete");
        }

        wglMakeCurrent(NULL, NULL);
        if (g_renderThreadContext) {
            // Only delete context if we created it (not if using pre-shared context)
            if (!g_renderContextIsShared) { wglDeleteContext(g_renderThreadContext); }
            g_renderThreadContext = NULL;
        }

        g_renderThreadRunning.store(false);
        Log("Render Thread: Stopped");

    } catch (const SE_Exception& e) {
        LogException("RenderThreadFunc (SEH)", e.getCode(), e.getInfo());
        g_renderThreadRunning.store(false);
    } catch (const std::exception& e) {
        LogException("RenderThreadFunc", e);
        g_renderThreadRunning.store(false);
    } catch (...) {
        Log("EXCEPTION in RenderThreadFunc: Unknown exception");
        g_renderThreadRunning.store(false);
    }
}

void StartRenderThread(void* gameGLContext) {
    // If thread is already running, don't start another
    if (g_renderThread.joinable()) {
        if (g_renderThreadRunning.load()) {
            Log("Render Thread: Already running");
            return;
        } else {
            Log("Render Thread: Joining finished thread...");
            g_renderThread.join();
        }
    }

    // Check if pre-shared context is available (from InitializeSharedContexts)
    HGLRC sharedContext = GetSharedRenderContext();
    HDC sharedDC = GetSharedRenderContextDC();

    if (sharedContext && sharedDC) {
        // Use the pre-shared context (GPU sharing enabled for all threads)
        g_renderThreadContext = sharedContext;
        g_renderThreadDC = sharedDC;
        g_renderContextIsShared = true;
        Log("Render Thread: Using pre-shared context (GPU texture sharing enabled)");
    } else {
        // Fallback: Create and share context now
        g_renderContextIsShared = false;

        // Get current DC
        HDC hdc = wglGetCurrentDC();
        if (!hdc) {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd) { hdc = GetDC(hwnd); }
        }

        if (!hdc) {
            Log("Render Thread: No DC available");
            return;
        }

        g_renderThreadDC = hdc;

        // Create the render context on main thread
        g_renderThreadContext = wglCreateContext(hdc);
        if (!g_renderThreadContext) {
            Log("Render Thread: Failed to create GL context (error " + std::to_string(GetLastError()) + ")");
            return;
        }

        // Share OpenGL objects with game context (textures, buffers - NOT shaders)
        // IMPORTANT: wglShareLists requires neither context to be current.
        HDC prevDC = wglGetCurrentDC();
        HGLRC prevRC = wglGetCurrentContext();
        if (prevRC) { wglMakeCurrent(NULL, NULL); }

        if (!wglShareLists((HGLRC)gameGLContext, g_renderThreadContext)) {
            DWORD err1 = GetLastError();
            if (!wglShareLists(g_renderThreadContext, (HGLRC)gameGLContext)) {
                DWORD err2 = GetLastError();
                Log("Render Thread: wglShareLists failed (errors " + std::to_string(err1) + ", " + std::to_string(err2) + ")");
                wglDeleteContext(g_renderThreadContext);
                g_renderThreadContext = NULL;
                if (prevRC && prevDC) { wglMakeCurrent(prevDC, prevRC); }
                return;
            }
        }

        if (prevRC && prevDC) { wglMakeCurrent(prevDC, prevRC); }

        Log("Render Thread: Context created and shared on main thread (fallback mode)");
    }

    // Reset state
    g_renderThreadShouldStop.store(false);
    g_renderThreadRunning.store(true);
    g_requestPending.store(false);
    g_frameComplete.store(false);
    g_writeFBOIndex.store(0);
    g_readFBOIndex.store(-1);
    g_lastGoodTexture.store(0);
    g_lastGoodObsTexture.store(0);
    g_framesRendered.store(0);
    g_framesDropped.store(0);

    // Start thread
    g_renderThread = std::thread(RenderThreadFunc, gameGLContext);
    LogCategory("init", "Render Thread: Started");
}

void StopRenderThread() {
    if (!g_renderThreadRunning.load() && !g_renderThread.joinable()) { return; }

    Log("Render Thread: Stopping...");
    g_renderThreadShouldStop.store(true);

    // Wake up thread if waiting
    g_requestCV.notify_one();

    if (g_renderThread.joinable()) { g_renderThread.join(); }

    Log("Render Thread: Joined");
}

void SubmitFrameForRendering(const FrameRenderRequest& request) {
    // Lock-free submission using double-buffered slots
    // Main thread ALWAYS succeeds - never blocks waiting for render thread

    // If there was a pending request we're overwriting, count it as dropped
    if (g_requestPending.load(std::memory_order_relaxed)) { g_framesDropped.fetch_add(1, std::memory_order_relaxed); }

    // Write to current write slot
    int writeSlot = g_requestWriteSlot.load(std::memory_order_relaxed);
    g_requestSlots[writeSlot] = request;

    // Swap write slot so next submission goes to the other slot
    // This also tells render thread which slot to read from (the one we just wrote)
    g_requestWriteSlot.store(1 - writeSlot, std::memory_order_relaxed);

    // Mark as pending
    g_requestPending.store(true, std::memory_order_release);
    g_frameComplete.store(false, std::memory_order_relaxed);

    // Signal the condition variable (brief lock only for CV, not for data protection)
    { std::lock_guard<std::mutex> lock(g_requestSignalMutex); }
    g_requestCV.notify_one();
}

int WaitForRenderComplete(int timeoutMs) {
    std::unique_lock<std::mutex> lock(g_completionMutex);

    bool completed = g_completionCV.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                             [] { return g_frameComplete.load() || g_renderThreadShouldStop.load(); });

    if (g_renderThreadShouldStop.load()) return -1;
    if (!completed) return -1;

    g_frameComplete.store(false);
    return g_readFBOIndex.load();
}

GLuint GetCompletedRenderTexture() {
    // Return the last known good texture
    // This is guaranteed to be fully rendered because we only update it
    // after the GPU fence wait completes on the render thread
    return g_lastGoodTexture.load(std::memory_order_acquire);
}

GLsync GetCompletedRenderFence() {
    // Return the fence associated with the last good texture
    // The caller should use glWaitSync() to wait for GPU completion before reading the texture
    // This is more efficient than glFinish() as it only waits for the render thread's commands
    return g_lastGoodFence.load(std::memory_order_acquire);
}

void SubmitObsFrameContext(const ObsFrameSubmission& submission) {
    // Lock-free submission using double-buffered slots
    // Main thread ALWAYS succeeds - never blocks waiting for render thread

    // NOTE: We do NOT delete fences here even if overwriting a pending submission.
    // The render thread owns the gameTextureFence and is responsible for deleting it
    // after processing. Deleting here causes a race condition where the render thread
    // may have already copied the fence pointer and will try to delete it again.
    // Occasional fence leaks from dropped frames are acceptable and rare.

    // Write to current write slot
    int writeSlot = g_obsWriteSlot.load(std::memory_order_relaxed);
    g_obsSubmissionSlots[writeSlot] = submission;

    // Swap write slot
    g_obsWriteSlot.store(1 - writeSlot, std::memory_order_relaxed);

    // Mark as pending
    g_obsSubmissionPending.store(true, std::memory_order_release);
    g_obsFrameComplete.store(false, std::memory_order_relaxed);

    // Signal the condition variable
    { std::lock_guard<std::mutex> lock(g_requestSignalMutex); }
    g_requestCV.notify_one();
}

GLuint GetCompletedObsTexture() {
    // Return the last known good OBS texture
    // This is guaranteed to be fully rendered because we only update it
    // after the GPU fence wait completes on the render thread
    return g_lastGoodObsTexture.load(std::memory_order_acquire);
}

GLsync GetCompletedObsFence() {
    // Return the fence associated with the last good OBS texture
    // The caller should use glWaitSync() to wait for GPU completion before reading the texture
    // This is more efficient than glFinish() as it only waits for the render thread's commands
    return g_lastGoodObsFence.load(std::memory_order_acquire);
}

FrameRenderRequest BuildObsFrameRequest(const ObsFrameContext& ctx, bool isDualRenderingPath) {
    static uint64_t s_obsFrameNumber = 0;

    // Use config snapshot for thread-safe access
    auto obsCfgSnap = GetConfigSnapshot();
    if (!obsCfgSnap) return {}; // Config not yet published
    const Config& obsCfg = *obsCfgSnap;

    ModeTransitionState transitionState = GetModeTransitionState();

    FrameRenderRequest req;
    req.frameNumber = ++s_obsFrameNumber;
    req.fullW = ctx.fullW;
    req.fullH = ctx.fullH;
    req.gameW = ctx.gameW;
    req.gameH = ctx.gameH;
    req.gameTextureId = ctx.gameTextureId;
    req.modeId = ctx.modeId;
    req.overlayOpacity = 1.0f;
    req.obsDetected = true;
    req.excludeOnlyOnMyScreen = true;
    req.skipAnimation = false;
    req.isObsPass = true;
    req.relativeStretching = ctx.relativeStretching;
    req.fromModeId = transitionState.fromModeId; // For source mirror check in sliding animation

    // Slide mirrors animation settings
    if (!transitionState.fromModeId.empty()) {
        const ModeConfig* fromMode = GetModeFromSnapshot(obsCfg, transitionState.fromModeId);
        if (fromMode) { req.fromSlideMirrorsIn = fromMode->slideMirrorsIn; }
    }
    const ModeConfig* toMode = GetModeFromSnapshot(obsCfg, ctx.modeId);
    if (toMode) { req.toSlideMirrorsIn = toMode->slideMirrorsIn; }

    // Mirror slide progress - uses actual moveProgress independent of overlay transition type
    if (transitionState.active && transitionState.moveProgress < 1.0f) {
        req.mirrorSlideProgress = transitionState.moveProgress;
    } else {
        req.mirrorSlideProgress = 1.0f;
    }

    // Determine if transition is effectively complete
    bool transitionEffectivelyComplete = !transitionState.active || transitionState.progress >= 1.0f;

    if (isDualRenderingPath) {
        // Dual rendering path - OBS gets animations even when hideAnimationsInGame is enabled
        // Check if there's an active transition to animate
        bool stillAnimating = transitionState.active && transitionState.progress < 1.0f;

        if (stillAnimating) {
            // Active transition - use animated geometry for OBS
            req.isAnimating = true;
            req.finalX = transitionState.targetX;
            req.finalY = transitionState.targetY;
            req.finalW = transitionState.targetWidth;
            req.finalH = transitionState.targetHeight;
            req.animatedX = transitionState.x;
            req.animatedY = transitionState.y;
            req.animatedW = transitionState.width;
            req.animatedH = transitionState.height;

            req.transitionProgress = transitionState.moveProgress;
            req.fromX = transitionState.fromX;
            req.fromY = transitionState.fromY;
            req.fromW = transitionState.fromWidth;
            req.fromH = transitionState.fromHeight;

            // TO geometry - where overlays will end (TARGET position, not animated)
            // Must match screen behavior: always use target, not animated position
            req.toX = transitionState.targetX;
            req.toY = transitionState.targetY;
            req.toW = transitionState.targetWidth;
            req.toH = transitionState.targetHeight;
        } else {
            // Transition just ended or not active - use current mode viewport
            // This fixes the black frame issue when HAIG is enabled and the transition
            // completes: the transitionState values become all zeros, causing a black frame.
            req.isAnimating = false;
            ModeViewportInfo viewport = GetCurrentModeViewport();
            int finalX, finalY, finalW, finalH;
            if (viewport.valid) {
                finalX = viewport.stretchX;
                finalY = viewport.stretchY;
                finalW = viewport.stretchWidth;
                finalH = viewport.stretchHeight;
            } else {
                // Fallback: center the game viewport
                finalX = (ctx.fullW - ctx.gameW) / 2;
                finalY = (ctx.fullH - ctx.gameH) / 2;
                finalW = ctx.gameW;
                finalH = ctx.gameH;
            }
            req.animatedX = finalX;
            req.animatedY = finalY;
            req.animatedW = finalW;
            req.animatedH = finalH;
            req.transitionProgress = 1.0f;
            req.fromX = finalX;
            req.fromY = finalY;
            req.fromW = finalW;
            req.fromH = finalH;
            req.toX = finalX;
            req.toY = finalY;
            req.toW = finalW;
            req.toH = finalH;
            req.finalX = finalX;
            req.finalY = finalY;
            req.finalW = finalW;
            req.finalH = finalH;
        }
    } else {
        // Normal path - check if actually animating
        if (!transitionEffectivelyComplete) {
            req.isAnimating = true;
            req.animatedX = transitionState.x;
            req.animatedY = transitionState.y;
            req.animatedW = transitionState.width;
            req.animatedH = transitionState.height;
            req.transitionProgress = transitionState.moveProgress;
            req.fromX = transitionState.fromX;
            req.fromY = transitionState.fromY;
            req.fromW = transitionState.fromWidth;
            req.fromH = transitionState.fromHeight;

            // During bounce phase, use animated position as TO
            bool inBouncePhase = transitionState.moveProgress >= 1.0f;
            if (inBouncePhase) {
                req.toX = transitionState.x;
                req.toY = transitionState.y;
                req.toW = transitionState.width;
                req.toH = transitionState.height;
            } else {
                req.toX = transitionState.targetX;
                req.toY = transitionState.targetY;
                req.toW = transitionState.targetWidth;
                req.toH = transitionState.targetHeight;
            }

            req.finalX = transitionState.targetX;
            req.finalY = transitionState.targetY;
            req.finalW = transitionState.targetWidth;
            req.finalH = transitionState.targetHeight;
        } else {
            req.isAnimating = false;
            // Calculate proper geometry from ctx values when not transitioning
            // Use the current mode viewport from GetCurrentModeViewport for accurate positioning
            ModeViewportInfo viewport = GetCurrentModeViewport();
            int finalX, finalY, finalW, finalH;
            if (viewport.valid) {
                finalX = viewport.stretchX;
                finalY = viewport.stretchY;
                finalW = viewport.stretchWidth;
                finalH = viewport.stretchHeight;
            } else {
                // Fallback: center the game viewport
                finalX = (ctx.fullW - ctx.gameW) / 2;
                finalY = (ctx.fullH - ctx.gameH) / 2;
                finalW = ctx.gameW;
                finalH = ctx.gameH;
            }
            req.animatedX = finalX;
            req.animatedY = finalY;
            req.animatedW = finalW;
            req.animatedH = finalH;
            req.transitionProgress = 1.0f;
            req.fromX = finalX;
            req.fromY = finalY;
            req.fromW = finalW;
            req.fromH = finalH;
            req.toX = finalX;
            req.toY = finalY;
            req.toW = finalW;
            req.toH = finalH;
            req.finalX = finalX;
            req.finalY = finalY;
            req.finalW = finalW;
            req.finalH = finalH;
        }
    }

    // Windowed mode override: center the window content in the fullscreen output
    // This ensures virtual camera shows centered game content with black borders for BOTH versions.
    // For BOTH versions, we need the same UV handling because the copy texture contains
    // window-sized content copied from the game texture at (0,0)→(windowW,windowH).
    if (ctx.isWindowed && ctx.windowW > 0 && ctx.windowH > 0) {
        // Use window dimensions for centering - this is the actual content size
        int contentW = ctx.windowW;
        int contentH = ctx.windowH;

        int centeredX = (ctx.fullW - contentW) / 2;
        int centeredY = (ctx.fullH - contentH) / 2;

        // Override all viewport positions to center the windowed game
        req.animatedX = centeredX;
        req.animatedY = centeredY;
        req.animatedW = contentW;
        req.animatedH = contentH;
        req.fromX = centeredX;
        req.fromY = centeredY;
        req.fromW = contentW;
        req.fromH = contentH;
        req.toX = centeredX;
        req.toY = centeredY;
        req.toW = contentW;
        req.toH = contentH;
        req.finalX = centeredX;
        req.finalY = centeredY;
        req.finalW = contentW;
        req.finalH = contentH;
        req.gameW = contentW;
        req.gameH = contentH;
        req.isAnimating = false;
        req.transitionProgress = 1.0f;

        // Set windowed mode fields - UV sampling needed for BOTH versions
        // The copy texture is resized to window dimensions, so UV 0→1 samples the full content
        req.isWindowed = true;
        req.windowW = ctx.windowW;
        req.windowH = ctx.windowH;
        // Both versions need windowed mode handling since both have window-sized copy textures
        req.isPre113Windowed = true;                   // Use windowed UV for ALL versions
        req.isRawWindowedMode = ctx.isRawWindowedMode; // Skip overlays if set

        // Force black background for centered windowed output
        req.bgR = 0.0f;
        req.bgG = 0.0f;
        req.bgB = 0.0f;
    }

    // Background color - check for fullscreen transition
    bool transitioningToFullscreen = EqualsIgnoreCase(ctx.modeId, "Fullscreen") && !transitionState.fromModeId.empty();
    if (transitioningToFullscreen && !transitionEffectivelyComplete) {
        const ModeConfig* fromMode = GetModeFromSnapshot(obsCfg, transitionState.fromModeId);
        if (fromMode) {
            req.bgR = fromMode->background.color.r;
            req.bgG = fromMode->background.color.g;
            req.bgB = fromMode->background.color.b;
        } else {
            req.bgR = ctx.bgR;
            req.bgG = ctx.bgG;
            req.bgB = ctx.bgB;
        }
    } else {
        req.bgR = ctx.bgR;
        req.bgG = ctx.bgG;
        req.bgB = ctx.bgB;
    }

    // Mode border config - look up from current mode
    const ModeConfig* currentMode = GetModeFromSnapshot(obsCfg, ctx.modeId);
    if (currentMode) {
        req.borderEnabled = currentMode->border.enabled;
        req.borderR = currentMode->border.color.r;
        req.borderG = currentMode->border.color.g;
        req.borderB = currentMode->border.color.b;
        req.borderWidth = currentMode->border.width;
        req.borderRadius = currentMode->border.radius;
    }

    // Transition-related border (for transitioning TO Fullscreen)
    req.transitioningToFullscreen = transitioningToFullscreen && !transitionEffectivelyComplete;
    if (req.transitioningToFullscreen && !transitionState.fromModeId.empty()) {
        const ModeConfig* fromMode = GetModeFromSnapshot(obsCfg, transitionState.fromModeId);
        if (fromMode) {
            req.fromBorderEnabled = fromMode->border.enabled;
            req.fromBorderR = fromMode->border.color.r;
            req.fromBorderG = fromMode->border.color.g;
            req.fromBorderB = fromMode->border.color.b;
            req.fromBorderWidth = fromMode->border.width;
            req.fromBorderRadius = fromMode->border.radius;
        }
    }

    // ImGui rendering state
    req.shouldRenderGui = ctx.shouldRenderGui;
    req.showPerformanceOverlay = ctx.showPerformanceOverlay;
    req.showProfiler = ctx.showProfiler;
    req.showEyeZoom = ctx.isEyeZoom || ctx.isTransitioningFromEyeZoom;
    req.eyeZoomFadeOpacity = 1.0f;
    // For OBS, use animated position during transition
    req.eyeZoomAnimatedViewportX = isDualRenderingPath ? transitionState.x : ctx.eyeZoomAnimatedViewportX;
    req.isTransitioningFromEyeZoom = ctx.isTransitioningFromEyeZoom;
    req.eyeZoomSnapshotTexture = ctx.eyeZoomSnapshotTexture;
    req.eyeZoomSnapshotWidth = ctx.eyeZoomSnapshotWidth;
    req.eyeZoomSnapshotHeight = ctx.eyeZoomSnapshotHeight;
    req.showTextureGrid = ctx.showTextureGrid;
    req.textureGridModeWidth = ctx.gameW;
    req.textureGridModeHeight = ctx.gameH;

    // Welcome toast (shown briefly after DLL injection - bypasses isRawWindowedMode)
    req.showWelcomeToast = ctx.showWelcomeToast;
    req.welcomeToastIsFullscreen = ctx.welcomeToastIsFullscreen;

    return req;
}
