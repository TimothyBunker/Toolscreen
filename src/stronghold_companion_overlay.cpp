#include "stronghold_companion_overlay.h"

#include "logic_thread.h"
#include "utils.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

extern std::atomic<HWND> g_minecraftHwnd;

namespace {
constexpr wchar_t kCompanionClassName[] = L"ToolscreenStrongholdCompanionWindow";
constexpr auto kMinCompanionUpdateInterval = std::chrono::milliseconds(33); // ~30 FPS

struct MonitorInfo {
    int index = 0;
    int maskBitIndex = 0;
    int displayNumber = 0;
    HMONITOR handle = nullptr;
    RECT rect{ 0, 0, 0, 0 };
};

struct CompanionWindowEntry {
    HWND hwnd = nullptr;
    bool useLayered = true;
    int layeredFailureCount = 0;
};

std::map<int, CompanionWindowEntry> s_companionWindows;
std::chrono::steady_clock::time_point s_lastCompanionUpdate{};
bool s_hasLastCompanionUpdate = false;
ATOM s_companionWindowClassAtom = 0;
ULONG_PTR s_gdiplusToken = 0;
bool s_gdiplusInitialized = false;

std::wstring ToWide(const std::string& textUtf8) {
    if (textUtf8.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, textUtf8.c_str(), -1, nullptr, 0);
    if (needed <= 0) { return std::wstring(textUtf8.begin(), textUtf8.end()); }

    std::wstring out(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, textUtf8.c_str(), -1, out.data(), needed) <= 0) {
        return std::wstring(textUtf8.begin(), textUtf8.end());
    }
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

std::wstring FormatSignedAdjustment(double valueDeg) {
    std::wostringstream ss;
    ss.setf(std::ios::showpos);
    ss.setf(std::ios::fixed, std::ios::floatfield);
    if (std::abs(valueDeg) < 0.1) {
        ss.precision(3);
    } else {
        ss.precision(2);
    }
    ss << valueDeg;
    return ss.str();
}

int ExtractDisplayNumber(const WCHAR* deviceName) {
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

void BuildRoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float radius) {
    const float r = std::max(0.0f, std::min(radius, std::min(w, h) * 0.5f));
    const float d = r * 2.0f;

    if (r <= 0.0f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        path.CloseFigure();
        return;
    }

    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void DrawCompassArrow(Gdiplus::Graphics& g, float centerX, float centerY, float radius, float relativeYawDeg, const Gdiplus::Color& arrowColor,
                      const Gdiplus::Color& ringColor) {
    constexpr float kPi = 3.14159265358979323846f;
    const float angleRad = relativeYawDeg * (kPi / 180.0f);
    const float dirX = std::sin(angleRad);
    const float dirY = -std::cos(angleRad);
    const float perpX = -dirY;
    const float perpY = dirX;

    const float tipDist = radius * 0.9f;
    const float tailDist = radius * 0.45f;
    const float headLen = radius * 0.38f;
    const float headHalfW = radius * 0.24f;
    const float shaftWidth = std::max(2.0f, radius * 0.13f);

    const Gdiplus::PointF tip(centerX + dirX * tipDist, centerY + dirY * tipDist);
    const Gdiplus::PointF tail(centerX - dirX * tailDist, centerY - dirY * tailDist);
    const Gdiplus::PointF headBase(tip.X - dirX * headLen, tip.Y - dirY * headLen);
    const Gdiplus::PointF headLeft(headBase.X + perpX * headHalfW, headBase.Y + perpY * headHalfW);
    const Gdiplus::PointF headRight(headBase.X - perpX * headHalfW, headBase.Y - perpY * headHalfW);

    Gdiplus::Pen ringPen(ringColor, std::max(1.5f, radius * 0.06f));
    ringPen.SetAlignment(Gdiplus::PenAlignmentCenter);
    g.DrawEllipse(&ringPen, centerX - radius, centerY - radius, radius * 2.0f, radius * 2.0f);

    Gdiplus::Pen shaftPen(arrowColor, shaftWidth);
    shaftPen.SetStartCap(Gdiplus::LineCapRound);
    shaftPen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&shaftPen, tail, headBase);

    Gdiplus::SolidBrush arrowBrush(arrowColor);
    Gdiplus::PointF tri[3] = { tip, headLeft, headRight };
    g.FillPolygon(&arrowBrush, tri, 3);
    g.FillEllipse(&arrowBrush, centerX - radius * 0.1f, centerY - radius * 0.1f, radius * 0.2f, radius * 0.2f);
}

Gdiplus::Color ScaleColor(const Gdiplus::Color& color, float factor) {
    const int r = std::clamp(static_cast<int>(std::lround(static_cast<float>(color.GetR()) * factor)), 0, 255);
    const int g = std::clamp(static_cast<int>(std::lround(static_cast<float>(color.GetG()) * factor)), 0, 255);
    const int b = std::clamp(static_cast<int>(std::lround(static_cast<float>(color.GetB()) * factor)), 0, 255);
    return Gdiplus::Color(color.GetA(), r, g, b);
}

Gdiplus::Color LerpColor(const Gdiplus::Color& from, const Gdiplus::Color& to, float t) {
    const float clampedT = std::clamp(t, 0.0f, 1.0f);
    const int r = static_cast<int>(std::lround(from.GetR() + (to.GetR() - from.GetR()) * clampedT));
    const int g = static_cast<int>(std::lround(from.GetG() + (to.GetG() - from.GetG()) * clampedT));
    const int b = static_cast<int>(std::lround(from.GetB() + (to.GetB() - from.GetB()) * clampedT));
    const int a = static_cast<int>(std::lround(from.GetA() + (to.GetA() - from.GetA()) * clampedT));
    return Gdiplus::Color(static_cast<BYTE>(std::clamp(a, 0, 255)), static_cast<BYTE>(std::clamp(r, 0, 255)),
                          static_cast<BYTE>(std::clamp(g, 0, 255)), static_cast<BYTE>(std::clamp(b, 0, 255)));
}

Gdiplus::Color CertaintyHeatColor(float certaintyPercent, BYTE alpha) {
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
    return Gdiplus::Color(alpha, static_cast<BYTE>(std::lround(r)), static_cast<BYTE>(std::lround(g)), static_cast<BYTE>(std::lround(b)));
}

void DrawBoatIcon(Gdiplus::Graphics& g, float centerX, float centerY, float size, const Gdiplus::Color& boatColor,
                  const Gdiplus::Color& strokeColor) {
    if (size <= 2.0f) return;

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

    const Gdiplus::Color c1 = ScaleColor(boatColor, 0.62f);
    const Gdiplus::Color c2 = ScaleColor(boatColor, 0.80f);
    const Gdiplus::Color c3 = ScaleColor(boatColor, 0.98f);
    const Gdiplus::Color c4 = ScaleColor(boatColor, 1.14f);
    Gdiplus::SolidBrush b1(c1);
    Gdiplus::SolidBrush b2(c2);
    Gdiplus::SolidBrush b3(c3);
    Gdiplus::SolidBrush b4(c4);
    const Gdiplus::Color outlineColor = LerpColor(ScaleColor(boatColor, 0.40f), strokeColor, 0.08f);
    Gdiplus::SolidBrush outline(outlineColor);

    const float px = std::max(1.0f, size / static_cast<float>(kBoatSpriteH));
    const float spriteW = px * static_cast<float>(kBoatSpriteW);
    const float spriteH = px * static_cast<float>(kBoatSpriteH);
    const float left = centerX - spriteW * 0.5f;
    const float top = centerY - spriteH * 0.5f;

    for (int y = 0; y < kBoatSpriteH; ++y) {
        for (int x = 0; x < kBoatSpriteW; ++x) {
            const char p = kBoatSprite[y][x];
            if (p == '.') continue;
            Gdiplus::SolidBrush* brush = &b2;
            if (p == 'o') brush = &outline;
            else if (p == '1') brush = &b1;
            else if (p == '2') brush = &b2;
            else if (p == '3') brush = &b3;
            else if (p == '4') brush = &b4;
            g.FillRectangle(brush, left + x * px, top + y * px, px, px);
        }
    }
}

void DrawEnderEyeIcon(Gdiplus::Graphics& g, float centerX, float centerY, float size, float certaintyPercent, const Gdiplus::Color& strokeColor) {
    if (size <= 2.0f) return;

    constexpr int kEyeSpriteW = 16;
    constexpr int kEyeSpriteH = 16;
    static const char* kEyeSprite[kEyeSpriteH] = {
        "......oooo......", "....oo2222oo....", "..oo23333332oo..", "..o2233333321o..", ".o223444443322o.",
        ".o334441124333o.", "o23344111124332o", "o24444111124332o", "o24444111124332o", "o23342111144442o",
        ".o223441144233o.", ".o222342242422o.", "..o1222222321o..", "..oo22222232oo..", "....oo2222oo....",
        "......oooo......",
    };

    const Gdiplus::Color certaintyColor = CertaintyHeatColor(certaintyPercent, strokeColor.GetA());
    const Gdiplus::Color outlineColor = LerpColor(Gdiplus::Color(strokeColor.GetA(), 26, 34, 42), certaintyColor, 0.20f);
    const Gdiplus::Color c1 = LerpColor(Gdiplus::Color(strokeColor.GetA(), 10, 14, 20), certaintyColor, 0.20f);
    const Gdiplus::Color c2 = LerpColor(Gdiplus::Color(strokeColor.GetA(), 36, 46, 58), certaintyColor, 0.46f);
    const Gdiplus::Color c3 = LerpColor(certaintyColor, Gdiplus::Color(strokeColor.GetA(), 255, 255, 255), 0.12f);
    const Gdiplus::Color c4 = LerpColor(certaintyColor, Gdiplus::Color(strokeColor.GetA(), 255, 255, 255), 0.34f);
    Gdiplus::SolidBrush b1(c1);
    Gdiplus::SolidBrush b2(c2);
    Gdiplus::SolidBrush b3(c3);
    Gdiplus::SolidBrush b4(c4);
    Gdiplus::SolidBrush outline(outlineColor);

    const float px = std::max(1.0f, size / static_cast<float>(kEyeSpriteH));
    const float spriteW = px * static_cast<float>(kEyeSpriteW);
    const float spriteH = px * static_cast<float>(kEyeSpriteH);
    const float left = centerX - spriteW * 0.5f;
    const float top = centerY - spriteH * 0.5f;

    for (int y = 0; y < kEyeSpriteH; ++y) {
        for (int x = 0; x < kEyeSpriteW; ++x) {
            const char p = kEyeSprite[y][x];
            if (p == '.') continue;
            Gdiplus::SolidBrush* brush = &b3;
            if (p == 'o') brush = &outline;
            else if (p == '1') brush = &b1;
            else if (p == '2') brush = &b2;
            else if (p == '3') brush = &b3;
            else if (p == '4') brush = &b4;
            g.FillRectangle(brush, left + x * px, top + y * px, px, px);
        }
    }
}

void DrawDoubleEnderEyeIcon(Gdiplus::Graphics& g, float centerX, float centerY, float size, float certaintyPercent,
                            const Gdiplus::Color& strokeColor) {
    if (size <= 2.0f) return;
    const float certainty = std::clamp(certaintyPercent, 0.0f, 100.0f);
    const float offset = std::max(1.0f, size * 0.18f);
    const Gdiplus::Color backStroke = LerpColor(strokeColor, Gdiplus::Color(strokeColor.GetA(), 200, 214, 235), 0.22f);
    DrawEnderEyeIcon(g, centerX - offset * 0.55f, centerY + offset * 0.16f, size * 0.88f, certainty * 0.94f, backStroke);
    DrawEnderEyeIcon(g, centerX + offset * 0.48f, centerY - offset * 0.14f, size, certainty, strokeColor);
}

void DrawStrongholdStatusIcon(Gdiplus::Graphics& g, float centerX, float centerY, float size, bool boatModeEnabled, int boatState,
                              bool hasCertainty, float certaintyPercent, const Gdiplus::Color& boatBlue, const Gdiplus::Color& boatGreen,
                              const Gdiplus::Color& boatRed, const Gdiplus::Color& strokeColor) {
    if (boatModeEnabled) {
        Gdiplus::Color boatColor = boatBlue;
        if (boatState == 1) {
            boatColor = boatGreen;
        } else if (boatState == 2) {
            boatColor = boatRed;
        }
        DrawBoatIcon(g, centerX, centerY, size, boatColor, strokeColor);
        return;
    }
    const float certainty = hasCertainty ? std::clamp(certaintyPercent, 0.0f, 100.0f) : 0.0f;
    DrawDoubleEnderEyeIcon(g, centerX, centerY, size, certainty, strokeColor);
}

LRESULT CALLBACK CompanionWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool EnsureCompanionClassRegistered() {
    if (s_companionWindowClassAtom != 0) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = CompanionWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kCompanionClassName;

    s_companionWindowClassAtom = RegisterClassExW(&wc);
    if (s_companionWindowClassAtom == 0) {
        DWORD err = GetLastError();
        if (err == ERROR_CLASS_ALREADY_EXISTS) {
            s_companionWindowClassAtom = 1;
            return true;
        }
        Log("Stronghold companion: RegisterClassExW failed (" + std::to_string(err) + ")");
        return false;
    }

    return true;
}

bool EnsureGdiplusInitialized() {
    if (s_gdiplusInitialized) return true;

    Gdiplus::GdiplusStartupInput startupInput;
    const Gdiplus::Status status = Gdiplus::GdiplusStartup(&s_gdiplusToken, &startupInput, nullptr);
    if (status != Gdiplus::Ok) {
        Log("Stronghold companion: GdiplusStartup failed (status " + std::to_string(static_cast<int>(status)) + ")");
        return false;
    }
    s_gdiplusInitialized = true;
    return true;
}

void DestroyCompanionWindow(int monitorIndex) {
    auto it = s_companionWindows.find(monitorIndex);
    if (it == s_companionWindows.end()) return;

    HWND hwnd = it->second.hwnd;
    if (hwnd && IsWindow(hwnd)) { DestroyWindow(hwnd); }
    s_companionWindows.erase(it);
}

void DestroyAllCompanionWindows() {
    for (auto& kv : s_companionWindows) {
        HWND hwnd = kv.second.hwnd;
        if (hwnd && IsWindow(hwnd)) { DestroyWindow(hwnd); }
    }
    s_companionWindows.clear();
}

HWND EnsureCompanionWindowForMonitor(int monitorIndex) {
    auto it = s_companionWindows.find(monitorIndex);
    if (it != s_companionWindows.end() && it->second.hwnd && IsWindow(it->second.hwnd)) { return it->second.hwnd; }

    if (!EnsureCompanionClassRegistered()) return nullptr;

    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    const DWORD style = WS_POPUP;
    HWND hwnd = CreateWindowExW(exStyle, kCompanionClassName, L"Toolscreen Stronghold Companion", style, 0, 0, 1, 1, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        Log("Stronghold companion: CreateWindowExW failed (" + std::to_string(GetLastError()) + ")");
        return nullptr;
    }

    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    CompanionWindowEntry entry;
    entry.hwnd = hwnd;
    entry.useLayered = true;
    entry.layeredFailureCount = 0;
    s_companionWindows[monitorIndex] = entry;
    Log("Stronghold companion: created window for monitor-bit " + std::to_string(monitorIndex));
    return hwnd;
}

BOOL CALLBACK EnumMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM userData) {
    auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(userData);
    if (!out) return FALSE;

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&mi))) return TRUE;

    MonitorInfo info;
    info.index = static_cast<int>(out->size());
    const int displayNumber = ExtractDisplayNumber(mi.szDevice);
    info.displayNumber = (displayNumber > 0) ? displayNumber : (info.index + 1);
    info.maskBitIndex = (displayNumber >= 1 && displayNumber <= 63) ? (displayNumber - 1) : info.index;
    info.handle = monitor;
    info.rect = mi.rcMonitor;
    out->push_back(info);
    return TRUE;
}

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> monitors;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorProc, reinterpret_cast<LPARAM>(&monitors));
    if (monitors.empty()) {
        MonitorInfo fallback;
        fallback.index = 0;
        fallback.maskBitIndex = 0;
        fallback.displayNumber = 1;
        fallback.rect.left = 0;
        fallback.rect.top = 0;
        fallback.rect.right = GetSystemMetrics(SM_CXSCREEN);
        fallback.rect.bottom = GetSystemMetrics(SM_CYSCREEN);
        monitors.push_back(fallback);
    }
    return monitors;
}

int GetGameMonitorMaskBitIndex(const std::vector<MonitorInfo>& monitors) {
    HWND hwnd = g_minecraftHwnd.load();
    HMONITOR gameMonitor = MonitorFromWindow(hwnd ? hwnd : GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (!gameMonitor) return 0;

    for (const MonitorInfo& monitor : monitors) {
        if (monitor.handle == gameMonitor) return monitor.maskBitIndex;
    }

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(gameMonitor, reinterpret_cast<MONITORINFO*>(&mi))) {
        const int displayNumber = ExtractDisplayNumber(mi.szDevice);
        if (displayNumber >= 1 && displayNumber <= 63) return displayNumber - 1;
    }

    return 0;
}

bool IsMonitorEnabledInMask(const StrongholdOverlayRenderSnapshot& snap, int monitorIndex) {
    if (snap.renderMonitorMode != 1) return true; // All monitors selected
    if (monitorIndex < 0 || monitorIndex >= 63) return true;
    const unsigned long long bit = (1ull << monitorIndex);
    return (snap.renderMonitorMask & bit) != 0ull;
}

bool RenderSnapshotToWindow(CompanionWindowEntry& entry, const RECT& monitorRect, const StrongholdOverlayRenderSnapshot& snap) {
    HWND hwnd = entry.hwnd;
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (!EnsureGdiplusInitialized()) return false;

    const bool compactMode = (snap.hudLayoutMode != 0);
    const bool showEstimateValues = snap.showEstimateValues;
    const float uiScale = std::clamp(snap.scale, 0.5f, 1.7f);
    const int monitorW = monitorRect.right - monitorRect.left;
    const int monitorH = monitorRect.bottom - monitorRect.top;
    const int cornerRadiusPx = static_cast<int>(std::lround((compactMode ? 14.0f : 12.0f) * uiScale));

    const float panelBaseW = compactMode ? (showEstimateValues ? 920.0f : 760.0f) : (showEstimateValues ? 560.0f : 500.0f);
    const float panelBaseH = compactMode ? (showEstimateValues ? 232.0f : 208.0f) : (showEstimateValues ? 390.0f : 340.0f);
    int panelW = static_cast<int>(std::lround(panelBaseW * uiScale));
    panelW = std::min(panelW, std::max(240, monitorW - 20));
    int panelH = static_cast<int>(std::lround(panelBaseH * uiScale));
    panelH = std::min(panelH, std::max(160, monitorH - 20));

    int dstX = monitorRect.left + (monitorW - panelW) / 2 + snap.x;
    int dstY = monitorRect.top + snap.y;
    const int minX = static_cast<int>(monitorRect.left);
    const int maxX = static_cast<int>(std::max<LONG>(monitorRect.left, monitorRect.right - panelW));
    const int minY = static_cast<int>(monitorRect.top);
    const int maxY = static_cast<int>(std::max<LONG>(monitorRect.top, monitorRect.bottom - panelH));
    dstX = std::clamp(dstX, minX, maxX);
    dstY = std::clamp(dstY, minY, maxY);

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) return false;

    HDC memDc = CreateCompatibleDC(screenDc);
    if (!memDc) {
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = panelW;
    bmi.bmiHeader.biHeight = -panelH; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!dib) {
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    HGDIOBJ oldBmp = SelectObject(memDc, dib);

    {
        Gdiplus::Graphics g(memDc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        const int textAlpha = static_cast<int>(std::clamp(snap.overlayOpacity, 0.0f, 1.0f) * 255.0f);
        const int bgAlpha = static_cast<int>(std::clamp(snap.overlayOpacity * snap.backgroundOpacity, 0.0f, 1.0f) * 255.0f);

        const Gdiplus::Color borderColor(textAlpha, 155, 225, 190);
        const Gdiplus::Color textColor(textAlpha, 242, 248, 255);
        const Gdiplus::Color mutedTextColor(textAlpha, 204, 220, 236);
        const Gdiplus::Color highlightColor(textAlpha, 255, 238, 145);
        const Gdiplus::Color warningColor(textAlpha, 255, 150, 130);
        const Gdiplus::Color boatBlue(textAlpha, 130, 185, 255);
        const Gdiplus::Color boatGreen(textAlpha, 130, 255, 160);
        const Gdiplus::Color boatRed(textAlpha, 255, 130, 130);
        const Gdiplus::Color topAdjPlus(textAlpha, 130, 255, 160);
        const Gdiplus::Color topAdjMinus(textAlpha, 255, 130, 130);
        const Gdiplus::Color statusColor =
            snap.targetLocked ? Gdiplus::Color(textAlpha, 255, 235, 140) : Gdiplus::Color(textAlpha, 180, 255, 200);

        const float alignmentRatio = snap.showComputedDetails ? std::clamp(1.0f - std::abs(snap.relativeYaw) / 90.0f, 0.0f, 1.0f) : 0.5f;
        const bool showDistanceMetrics = !snap.mcsrSafeMode;
        const int arrowR = static_cast<int>(std::lround(255.0f - 125.0f * alignmentRatio));
        const int arrowG = static_cast<int>(std::lround(120.0f + 135.0f * alignmentRatio));
        const int arrowB = static_cast<int>(std::lround(110.0f + 60.0f * alignmentRatio));
        const Gdiplus::Color arrowColor(textAlpha, arrowR, arrowG, arrowB);
        const Gdiplus::Color arrowRingColor(std::max(40, textAlpha / 2), 225, 240, 255);

        const float pad = compactMode ? (12.0f * uiScale) : (14.0f * uiScale);
        const float radius = compactMode ? (14.0f * uiScale) : (12.0f * uiScale);
        const float sideLaneW = snap.showDirectionArrow ? 236.0f : std::min(300.0f, panelW * 0.35f);
        const float textAreaW = std::max(140.0f, panelW - (pad * 2.0f) - sideLaneW);
        const float sideLaneX =
            std::max(pad, pad + textAreaW + (8.0f * uiScale) - (!snap.showDirectionArrow ? (56.0f * uiScale) : 0.0f));
        float sideDrawX = sideLaneX;
        float y = pad;

        Gdiplus::GraphicsPath panelPath(Gdiplus::FillModeAlternate);
        BuildRoundedRect(panelPath, 0.0f, 0.0f, static_cast<float>(panelW), static_cast<float>(panelH), radius);
        Gdiplus::LinearGradientBrush bgBrush(
            Gdiplus::PointF(0.0f, 0.0f), Gdiplus::PointF(0.0f, static_cast<float>(panelH)),
            Gdiplus::Color(std::clamp(bgAlpha + 24, 0, 255), 36, 58, 78), Gdiplus::Color(std::clamp(bgAlpha + 6, 0, 255), 23, 41, 60));
        Gdiplus::Pen borderPen(borderColor, std::max(1.0f, 1.6f * uiScale));
        g.FillPath(&bgBrush, &panelPath);
        g.DrawPath(&borderPen, &panelPath);

        Gdiplus::FontFamily fontFamily(L"Segoe UI");
        Gdiplus::Font statusFont(&fontFamily, (compactMode ? 21.0f : 22.0f) * uiScale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font rowFont(&fontFamily, (compactMode ? 17.8f : 18.0f) * uiScale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font metaFont(&fontFamily, (compactMode ? 15.4f : 16.0f) * uiScale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font infoFont(&fontFamily, (compactMode ? 14.6f : 14.0f) * uiScale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

        Gdiplus::StringFormat noWrap;
        noWrap.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        noWrap.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

        auto measureTextWidth = [&](const std::wstring& text, const Gdiplus::Font& font) {
            Gdiplus::RectF bounds;
            g.MeasureString(text.c_str(), -1, &font, Gdiplus::PointF(0.0f, 0.0f), &noWrap, &bounds);
            return bounds.Width;
        };
        auto drawSegment = [&](float& xPos, float yPos, const std::wstring& text, const Gdiplus::Color& color, const Gdiplus::Font& font) {
            if (text.empty()) return;
            Gdiplus::SolidBrush brush(color);
            Gdiplus::RectF rect(xPos, yPos, std::max(1.0f, static_cast<float>(panelW) - xPos - pad), font.GetSize() + (6.0f * uiScale));
            g.DrawString(text.c_str(), -1, &font, rect, &noWrap, &brush);
            xPos += measureTextWidth(text, font);
        };
        auto drawLineRect = [&](const std::wstring& text, const Gdiplus::Color& color, const Gdiplus::Font& font, float heightScale = 1.0f) {
            Gdiplus::SolidBrush brush(color);
            const float lineH = (font.GetSize() + 6.0f * uiScale) * heightScale;
            Gdiplus::RectF rect(pad, y, textAreaW, lineH);
            g.DrawString(text.c_str(), -1, &font, rect, &noWrap, &brush);
            y += lineH;
        };
        auto drawLockBadge = [&](float xPos, float yPos, float size, bool locked, const Gdiplus::Color& fill, const Gdiplus::Color& stroke) {
            if (size <= 2.0f) return;
            const float bodyW = size * 0.74f;
            const float bodyH = size * 0.52f;
            const float bodyX = xPos + (size - bodyW) * 0.5f;
            const float bodyY = yPos + size * 0.42f;
            const float round = std::max(1.0f, size * 0.10f);
            const float shackleR = std::max(2.0f, size * 0.25f);
            const float shackleY = bodyY + size * 0.02f;
            const float strokeW = std::max(1.0f, size * 0.08f);
            const float leftX = bodyX + bodyW * 0.20f;
            const float rightX = bodyX + bodyW * 0.80f;

            Gdiplus::SolidBrush fillBrush(fill);
            Gdiplus::Pen strokePen(stroke, strokeW);
            strokePen.SetLineJoin(Gdiplus::LineJoinRound);
            g.FillRectangle(&fillBrush, bodyX, bodyY, bodyW, bodyH);
            g.DrawRectangle(&strokePen, bodyX, bodyY, bodyW, bodyH);

            Gdiplus::GraphicsPath shacklePath;
            shacklePath.AddArc((leftX + rightX) * 0.5f - shackleR, shackleY - shackleR, shackleR * 2.0f, shackleR * 2.0f, 180.0f, 180.0f);
            g.DrawPath(&strokePen, &shacklePath);
            if (locked) {
                g.DrawLine(&strokePen, leftX, shackleY, leftX, bodyY + strokeW);
                g.DrawLine(&strokePen, rightX, shackleY, rightX, bodyY + strokeW);
            } else {
                g.DrawLine(&strokePen, leftX, shackleY, leftX, bodyY + strokeW);
                g.DrawLine(&strokePen, rightX + size * 0.07f, shackleY + size * 0.10f, rightX + size * 0.10f, bodyY - size * 0.03f);
            }
        };
        auto drawWorldBadge = [&](float xPos, float yPos, wchar_t worldId, float fontSize, const Gdiplus::Color& fill, const Gdiplus::Color& text,
                                  const Gdiplus::Color& border) {
            const float badgeH = std::max(10.0f, fontSize * 1.02f);
            const float badgeW = badgeH * 1.08f;
            const float round = std::max(1.0f, badgeH * 0.24f);
            Gdiplus::SolidBrush fillBrush(fill);
            Gdiplus::Pen borderPen(border, std::max(1.0f, fontSize * 0.08f));
            Gdiplus::GraphicsPath badgePath;
            BuildRoundedRect(badgePath, xPos, yPos, badgeW, badgeH, round);
            g.FillPath(&fillBrush, &badgePath);
            g.DrawPath(&borderPen, &badgePath);

            std::wstring label(1, worldId);
            Gdiplus::Font badgeFont(&fontFamily, fontSize * 0.86f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            const float tw = measureTextWidth(label, badgeFont);
            Gdiplus::SolidBrush textBrush(text);
            const float textX = xPos + (badgeW - tw) * 0.5f;
            const float textY = yPos + (badgeH - badgeFont.GetSize()) * 0.46f;
            Gdiplus::RectF rect(textX, textY, badgeW, badgeH);
            g.DrawString(label.c_str(), -1, &badgeFont, rect, &noWrap, &textBrush);
            return badgeW;
        };
        auto signedIntW = [](int value) {
            std::wostringstream ss;
            ss << std::showpos << value;
            return ss.str();
        };
        auto axisCloseness = [](int estimated, int target, int player) {
            const int referenceAbs = std::abs(player - target);
            const float denom = std::max(6.0f, static_cast<float>(referenceAbs));
            return std::clamp(1.0f - (static_cast<float>(std::abs(estimated - target)) / denom), 0.0f, 1.0f);
        };
        auto axisPct = [](float c) { return static_cast<int>(std::lround(std::clamp(c, 0.0f, 1.0f) * 100.0f)); };
        auto axisColor = [&](float c) {
            const float t = std::clamp(c, 0.0f, 1.0f);
            const int r = static_cast<int>(std::lround(255.0f - 178.0f * t));
            const int g = static_cast<int>(std::lround(96.0f + 159.0f * t));
            const int b = static_cast<int>(std::lround(118.0f + 28.0f * t));
            return Gdiplus::Color(textAlpha, r, g, b);
        };
        auto distance2D = [](int ax, int az, int bx, int bz) {
            const double dx = static_cast<double>(ax - bx);
            const double dz = static_cast<double>(az - bz);
            return static_cast<float>(std::sqrt((dx * dx) + (dz * dz)));
        };
        auto distanceCloseness = [](float distance, float maxDistance) {
            return std::clamp(1.0f - (distance / std::max(1.0f, maxDistance)), 0.0f, 1.0f);
        };

        const std::wstring adjText = FormatSignedAdjustment(snap.angleAdjustmentDeg);
        const double adjustmentStepDeg = std::max(1e-6, std::abs(static_cast<double>(snap.angleAdjustmentStepDeg)));
        const int adjustmentStepCount =
            static_cast<int>(std::lround(std::abs(static_cast<double>(snap.angleAdjustmentDeg)) / adjustmentStepDeg));
        const std::wstring adjustmentStepText =
            (adjustmentStepCount > 0)
                ? std::wstring((snap.angleAdjustmentDeg >= 0.0f) ? L"+" : L"-") + std::to_wstring(adjustmentStepCount)
                : std::wstring(L"0");
        const Gdiplus::Color adjustmentStepColor =
            (adjustmentStepCount == 0) ? mutedTextColor : ((snap.angleAdjustmentDeg >= 0.0f) ? topAdjPlus : topAdjMinus);
        const bool hasStatusCertainty = snap.hasTopCertainty || snap.hasCombinedCertainty;
        const float statusCertaintyPercent = snap.hasTopCertainty ? snap.topCertaintyPercent
                                                                  : (snap.hasCombinedCertainty ? snap.combinedCertaintyPercent : 50.0f);
        float headerX = pad;
        const float lockIconSize = std::max(10.0f, statusFont.GetSize() * 0.90f);
        drawLockBadge(headerX, y + (statusFont.GetSize() - lockIconSize) * 0.5f, lockIconSize, snap.targetLocked, statusColor, textColor);
        const float topBoatIconSize = std::max(10.0f, statusFont.GetSize() * 0.90f);
        DrawStrongholdStatusIcon(g, static_cast<float>(panelW) - pad - topBoatIconSize * 0.56f, pad + topBoatIconSize * 0.56f,
                                 topBoatIconSize, snap.boatModeEnabled, snap.boatState, hasStatusCertainty, statusCertaintyPercent, boatBlue,
                                 boatGreen, boatRed, mutedTextColor);

        if (snap.showDirectionArrow) {
            const float desiredRadiusPx = 70.0f;
            const float compassRadius =
                std::clamp(desiredRadiusPx, 24.0f, std::max(24.0f, std::min((panelH * 0.48f) - pad, (sideLaneW * 0.50f) - 8.0f)));
            float compassCenterX = static_cast<float>(panelW) - pad - compassRadius - (2.0f * uiScale);
            compassCenterX = std::max(compassCenterX, (panelW * 0.62f));
            float compassCenterY = static_cast<float>(panelH) * 0.50f;
            compassCenterY = std::clamp(compassCenterY, pad + compassRadius, static_cast<float>(panelH) - pad - compassRadius);
            DrawCompassArrow(g, compassCenterX, compassCenterY, compassRadius, snap.relativeYaw, arrowColor, arrowRingColor);
        }
        y += statusFont.GetSize() + (8.0f * uiScale);
        float sideY = y;

        auto drawWorldRow = [&](wchar_t worldId, int targetX, int targetZ, int estX, int estZ, int playerX, int playerZ) {
            const int dX = estX - targetX;
            const int dZ = estZ - targetZ;
            const float closeX = axisCloseness(estX, targetX, playerX);
            const float closeZ = axisCloseness(estZ, targetZ, playerZ);
            const Gdiplus::Color xColor = axisColor(closeX);
            const Gdiplus::Color zColor = axisColor(closeZ);
            const float distToTarget = distance2D(playerX, playerZ, targetX, targetZ);
            const float errDistance = distance2D(estX, estZ, targetX, targetZ);
            const Gdiplus::Color distColor = axisColor(distanceCloseness(distToTarget, (worldId == L'N') ? 260.0f : 2200.0f));
            const Gdiplus::Color errColor =
                axisColor(distanceCloseness(errDistance, std::max((worldId == L'N') ? 28.0f : 220.0f, distToTarget)));
            const bool emphasizeWorld = (worldId == L'N');
            const float targetFontSize = rowFont.GetSize() * (emphasizeWorld ? 1.18f : 1.04f);
            const float aimFontSize = rowFont.GetSize() * (emphasizeWorld ? 1.12f : 1.02f);
            Gdiplus::Font targetFont(&fontFamily, targetFontSize, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::Font aimFont(&fontFamily, aimFontSize, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

            float xPos = pad;
            const Gdiplus::Color worldBadgeFill =
                emphasizeWorld ? Gdiplus::Color(textAlpha, 56, 98, 136) : Gdiplus::Color(textAlpha, 52, 76, 100);
            const Gdiplus::Color worldBadgeText(textAlpha, 232, 244, 255);
            const float badgeFontSize = rowFont.GetSize() * (emphasizeWorld ? 1.02f : 0.98f);
            const float badgeY = y + std::max(0.0f, (targetFont.GetSize() - badgeFontSize) * 0.10f);
            xPos += drawWorldBadge(xPos, badgeY, worldId, badgeFontSize, worldBadgeFill, worldBadgeText, mutedTextColor) + (6.0f * uiScale);
            drawSegment(xPos, y, L"T(", highlightColor, targetFont);
            drawSegment(xPos, y, std::to_wstring(targetX), highlightColor, targetFont);
            drawSegment(xPos, y, L",", mutedTextColor, targetFont);
            drawSegment(xPos, y, std::to_wstring(targetZ), highlightColor, targetFont);
            drawSegment(xPos, y, L") ", highlightColor, targetFont);
            if (showDistanceMetrics) {
                drawSegment(xPos, y, L"@", mutedTextColor, rowFont);
                drawSegment(xPos, y, std::to_wstring(static_cast<int>(std::lround(distToTarget))), distColor, rowFont);
            }
            if (showEstimateValues) {
                drawSegment(xPos, y, L"  E(", mutedTextColor, aimFont);
                drawSegment(xPos, y, std::to_wstring(estX), xColor, aimFont);
                drawSegment(xPos, y, L",", mutedTextColor, aimFont);
                drawSegment(xPos, y, std::to_wstring(estZ), zColor, aimFont);
                drawSegment(xPos, y, L") ", mutedTextColor, aimFont);
                drawSegment(xPos, y, L"D(", mutedTextColor, rowFont);
                drawSegment(xPos, y, signedIntW(dX), xColor, rowFont);
                drawSegment(xPos, y, L",", mutedTextColor, rowFont);
                drawSegment(xPos, y, signedIntW(dZ), zColor, rowFont);
                drawSegment(xPos, y, L") [", mutedTextColor, rowFont);
                drawSegment(xPos, y, std::to_wstring(axisPct(closeX)), xColor, rowFont);
                drawSegment(xPos, y, L"|", mutedTextColor, rowFont);
                drawSegment(xPos, y, std::to_wstring(axisPct(closeZ)), zColor, rowFont);
                drawSegment(xPos, y, L"] ~", mutedTextColor, rowFont);
                drawSegment(xPos, y, std::to_wstring(static_cast<int>(std::lround(errDistance))), errColor, rowFont);
            }
            y += (rowFont.GetSize() + (6.0f * uiScale)) * (emphasizeWorld ? 1.10f : 1.0f);
        };

        if (snap.showComputedDetails) {
            drawWorldRow(L'N', snap.targetNetherX, snap.targetNetherZ, snap.estimatedNetherX, snap.estimatedNetherZ, snap.playerNetherX,
                         snap.playerNetherZ);
            drawWorldRow(L'O', snap.targetOverworldX, snap.targetOverworldZ, snap.estimatedOverworldX, snap.estimatedOverworldZ,
                         snap.playerOverworldX, snap.playerOverworldZ);

            std::wostringstream summary;
            const int aimPct = static_cast<int>(std::lround(std::clamp(alignmentRatio, 0.0f, 1.0f) * 100.0f));
            summary << std::fixed << std::setprecision(0);
            if (snap.showAlignmentText) summary << L"A" << aimPct << L"%";
            float summaryX = pad;
            drawSegment(summaryX, y, summary.str(), textColor, metaFont);
            y += metaFont.GetSize() + (6.0f * uiScale);

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
                float gVal = 96.0f;
                float bVal = 96.0f;
                if (t < 0.5f) {
                    const float u = t / 0.5f;
                    gVal = 96.0f + (159.0f * u);
                    bVal = 96.0f;
                } else {
                    const float u = (t - 0.5f) / 0.5f;
                    r = 255.0f - (159.0f * u);
                    gVal = 255.0f;
                    bVal = 96.0f;
                }
                return Gdiplus::Color(textAlpha, static_cast<BYTE>(std::lround(r)), static_cast<BYTE>(std::lround(gVal)),
                                      static_cast<BYTE>(std::lround(bVal)));
            };

            auto truncateSingleLine = [](std::string text, size_t maxLen) {
                if (text.size() <= maxLen) return text;
                if (maxLen <= 3) return text.substr(0, maxLen);
                text.resize(maxLen - 3);
                text += "...";
                return text;
            };
            const std::string topCandidate1Raw = truncateSingleLine(snap.topCandidate1Label, 66);
            const std::string topCandidate2Raw = truncateSingleLine(snap.topCandidate2Label, 66);
            const std::wstring topCandidate1 = ToWide(topCandidate1Raw);
            const bool showAltCandidate = (!snap.hasTopCertainty || snap.topCertaintyPercent < 95.0f) && !snap.topCandidate2Label.empty();
            const std::wstring topCandidate2 = ToWide(topCandidate2Raw);
            const CandidatePercentSpan topCandidate1Pct = parsePercentSpan(topCandidate1Raw);
            const CandidatePercentSpan topCandidate2Pct = parsePercentSpan(topCandidate2Raw);
            const Gdiplus::Color topCandidate1Base(textAlpha, 218, 228, 236);
            const Gdiplus::Color topCandidate2Base = mutedTextColor;
            const Gdiplus::Color topCandidateChipFill(std::max(26, textAlpha / 4), 74, 96, 126);
            const Gdiplus::Color topCandidateChipBorder(std::max(34, textAlpha / 3), 132, 164, 196);
            Gdiplus::Font topCandidate1Font(&fontFamily, infoFont.GetSize() * 1.06f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

            auto drawCandidateLineAt = [&](float xStart, float yPos, const std::wstring& text, const CandidatePercentSpan& span,
                                           const Gdiplus::Color& baseColor, const Gdiplus::Font& font) {
                if (text.empty()) return;
                float xPos = xStart;
                if (span.valid && span.end <= text.size()) {
                    drawSegment(xPos, yPos, text.substr(0, span.start), baseColor, font);
                    drawSegment(xPos, yPos, text.substr(span.start, span.end - span.start), certaintyColorFromPercent(span.pct), font);
                    drawSegment(xPos, yPos, text.substr(span.end), baseColor, font);
                } else {
                    drawSegment(xPos, yPos, text, baseColor, font);
                }
            };
            auto lineAdvanceForFont = [&](const Gdiplus::Font& f, float scale = 1.0f) {
                return (f.GetSize() + (6.0f * uiScale)) * scale;
            };
            auto drawCandidateChipAt = [&](float xStart, float yPos, const std::wstring& text, const Gdiplus::Font& font) {
                if (text.empty()) return;
                const float textW = measureTextWidth(text, font);
                const float chipPadX = 6.0f * uiScale;
                const float chipH = std::max(12.0f, font.GetSize() + (5.0f * uiScale));
                const float chipY = yPos - (2.0f * uiScale);
                Gdiplus::GraphicsPath chipPath;
                BuildRoundedRect(chipPath, xStart - chipPadX, chipY, textW + chipPadX * 2.0f, chipH, 5.0f * uiScale);
                Gdiplus::SolidBrush chipFill(topCandidateChipFill);
                Gdiplus::Pen chipBorder(topCandidateChipBorder, std::max(1.0f, 1.0f * uiScale));
                g.FillPath(&chipFill, &chipPath);
                g.DrawPath(&chipBorder, &chipPath);
            };

            std::wstring infoLine;
            Gdiplus::Color infoColor = mutedTextColor;
            const bool shouldShowMoveGuidance =
                snap.hasNextThrowDirection && (!snap.hasTopCertainty || (snap.topCertaintyPercent < 95.0f));
            if (shouldShowMoveGuidance) {
                std::wostringstream ss;
                ss << L"L" << snap.moveLeftBlocks << L" / R" << snap.moveRightBlocks << L" -> 95%";
                infoLine = ss.str();
                infoColor = warningColor;
            } else if (!snap.warningLabel.empty()) {
                infoLine = ToWide(snap.warningLabel);
                infoColor = warningColor;
            } else if (!snap.infoLabel.empty()) {
                std::string infoCompact = snap.infoLabel;
                const size_t adjPos = infoCompact.find(" | Adj ");
                if (adjPos != std::string::npos) {
                    const size_t nextSep = infoCompact.find(" | ", adjPos + 1);
                    if (nextSep != std::string::npos) {
                        infoCompact.erase(adjPos, nextSep - adjPos);
                    } else {
                        infoCompact.erase(adjPos);
                    }
                }
                infoLine = ToWide(infoCompact);
            } else {
                infoLine = L"[S+H] [H]";
            }

            if (!snap.showDirectionArrow) {
                const float sideCandidate1W = measureTextWidth(topCandidate1, topCandidate1Font);
                const float sideCandidate2W = showAltCandidate ? measureTextWidth(topCandidate2, infoFont) : 0.0f;
                const float sideInfoW = 0.0f; // guidance is rendered on bottom row in compact mode
                const float neededTextW = std::max(120.0f, std::max(sideCandidate1W, std::max(sideCandidate2W, sideInfoW)));
                const float dynamicSideLaneW =
                    std::clamp(neededTextW + (20.0f * uiScale), 190.0f, std::max(190.0f, static_cast<float>(panelW) * 0.52f));
                const float sideRight = static_cast<float>(panelW) - pad - (4.0f * uiScale);
                sideDrawX = std::max(pad, sideRight - dynamicSideLaneW);
            }

            if (!snap.showDirectionArrow) {
                if (!topCandidate1.empty()) { drawCandidateChipAt(sideDrawX, sideY, topCandidate1, topCandidate1Font); }
                drawCandidateLineAt(sideDrawX, sideY, topCandidate1, topCandidate1Pct, topCandidate1Base, topCandidate1Font);
                sideY += lineAdvanceForFont(topCandidate1Font, 1.04f);
                if (showAltCandidate) {
                    drawCandidateLineAt(sideDrawX, sideY, topCandidate2, topCandidate2Pct, topCandidate2Base, infoFont);
                    sideY += lineAdvanceForFont(infoFont);
                }
            } else {
                if (!topCandidate1.empty()) {
                    drawCandidateChipAt(pad, y, topCandidate1, topCandidate1Font);
                    drawCandidateLineAt(pad, y, topCandidate1, topCandidate1Pct, topCandidate1Base, topCandidate1Font);
                    y += lineAdvanceForFont(topCandidate1Font, 1.04f);
                }
                if (showAltCandidate) {
                    drawCandidateLineAt(pad, y, topCandidate2, topCandidate2Pct, topCandidate2Base, infoFont);
                    y += lineAdvanceForFont(infoFont);
                }
            }

            const std::wstring adjPrefix = adjText + L" ";
            const std::wstring adjStep = L"[" + adjustmentStepText + L"]";
            const std::wstring bottomSep = infoLine.empty() ? L"" : L"  |  ";
            const float bottomW = measureTextWidth(adjPrefix, metaFont) + measureTextWidth(adjStep, metaFont) +
                                  measureTextWidth(bottomSep, metaFont) + measureTextWidth(infoLine, metaFont);
            const float bottomY = static_cast<float>(panelH) - pad - (metaFont.GetSize() + (6.0f * uiScale));
            float bx = std::max(pad, (static_cast<float>(panelW) - bottomW) * 0.5f);
            drawSegment(bx, bottomY, adjPrefix, mutedTextColor, metaFont);
            drawSegment(bx, bottomY, adjStep, adjustmentStepColor, metaFont);
            if (!bottomSep.empty()) {
                drawSegment(bx, bottomY, bottomSep, mutedTextColor, metaFont);
                drawSegment(bx, bottomY, infoLine, infoColor, metaFont);
            }
        } else {
            float keyX = pad;
            drawSegment(keyX, y, L"[S+H] [H]", mutedTextColor, metaFont);
            y += metaFont.GetSize() + (6.0f * uiScale);
        }
    }

    // GDI+/DIB interop can leave alpha as 0 on some systems.
    // Promote non-black pixels to fully opaque so the layered window is reliably visible.
    if (dibBits) {
        uint32_t* pixels = reinterpret_cast<uint32_t*>(dibBits);
        const size_t pixelCount = static_cast<size_t>(panelW) * static_cast<size_t>(panelH);
        for (size_t i = 0; i < pixelCount; ++i) {
            uint32_t px = pixels[i];
            const uint8_t b = static_cast<uint8_t>((px >> 0) & 0xFFu);
            const uint8_t g = static_cast<uint8_t>((px >> 8) & 0xFFu);
            const uint8_t r = static_cast<uint8_t>((px >> 16) & 0xFFu);
            uint8_t a = static_cast<uint8_t>((px >> 24) & 0xFFu);
            if (a == 0 && (r != 0 || g != 0 || b != 0)) {
                a = 255;
                pixels[i] = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
                            static_cast<uint32_t>(b);
            }
        }
    }

    SIZE size{ panelW, panelH };
    POINT srcPt{ 0, 0 };
    POINT dstPt{ dstX, dstY };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    BOOL updateOk = TRUE;
    if (entry.useLayered) {
        updateOk = UpdateLayeredWindow(hwnd, screenDc, &dstPt, &size, memDc, &srcPt, 0, &blend, ULW_ALPHA);
        if (!updateOk) {
            DWORD err = GetLastError();
            entry.layeredFailureCount += 1;
            if (entry.layeredFailureCount <= 3 || (entry.layeredFailureCount % 30) == 0) {
                Log("Stronghold companion: UpdateLayeredWindow failed (" + std::to_string(err) +
                    "), failure count=" + std::to_string(entry.layeredFailureCount));
            }
            if (entry.layeredFailureCount >= 6) {
                LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                exStyle |= static_cast<LONG_PTR>(WS_EX_LAYERED);
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
                const BYTE fallbackAlpha = static_cast<BYTE>(std::clamp(
                    static_cast<int>(std::lround(255.0f * std::clamp(snap.overlayOpacity * 0.90f, 0.25f, 1.0f))), 64, 245));
                SetLayeredWindowAttributes(hwnd, 0, fallbackAlpha, LWA_ALPHA);
                SetWindowPos(hwnd, HWND_TOPMOST, dstX, dstY, panelW, panelH, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
                entry.useLayered = false;
                Log("Stronghold companion: switching to GDI fallback mode with global alpha after repeated layered failures");
            }
        } else {
            entry.layeredFailureCount = 0;
        }
    }

    if (!entry.useLayered) {
        SetWindowPos(hwnd, HWND_TOPMOST, dstX, dstY, panelW, panelH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        HRGN region = CreateRoundRectRgn(0, 0, panelW + 1, panelH + 1, std::max(1, cornerRadiusPx * 2), std::max(1, cornerRadiusPx * 2));
        if (region) {
            SetWindowRgn(hwnd, region, TRUE); // ownership transferred to window
        }
        HDC wndDc = GetDC(hwnd);
        if (wndDc) {
            BitBlt(wndDc, 0, 0, panelW, panelH, memDc, 0, 0, SRCCOPY);
            ReleaseDC(hwnd, wndDc);
            updateOk = TRUE;
        } else {
            updateOk = FALSE;
        }
    }

    SelectObject(memDc, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);

    return updateOk == TRUE;
}

} // namespace

void UpdateStrongholdCompanionOverlays() {
    const auto now = std::chrono::steady_clock::now();
    if (s_hasLastCompanionUpdate && (now - s_lastCompanionUpdate) < kMinCompanionUpdateInterval) return;
    s_lastCompanionUpdate = now;
    s_hasLastCompanionUpdate = true;

    StrongholdOverlayRenderSnapshot snap = GetStrongholdOverlayRenderSnapshot();
    std::vector<MonitorInfo> monitors = EnumerateMonitors();
    const int gameMonitorMaskBit = GetGameMonitorMaskBitIndex(monitors);
    int gameDisplayNumber = gameMonitorMaskBit + 1;
    for (const MonitorInfo& monitor : monitors) {
        if (monitor.maskBitIndex == gameMonitorMaskBit) {
            gameDisplayNumber = monitor.displayNumber;
            break;
        }
    }

    {
        static std::string s_lastStateLog;
        static auto s_lastStateLogTime = std::chrono::steady_clock::time_point{};
        static bool s_hasLastStateLogTime = false;
        std::ostringstream ss;
        ss << "Stronghold companion state: enabled=" << snap.enabled << " visible=" << snap.visible
           << " companion=" << snap.renderCompanionOverlay
           << " mode=" << snap.renderMonitorMode << " mask=0x" << std::hex << snap.renderMonitorMask << std::dec
           << " monitors=" << monitors.size() << " game=display" << gameDisplayNumber << " bit=" << gameMonitorMaskBit;
        const std::string line = ss.str();
        const auto nowLog = std::chrono::steady_clock::now();
        if (line != s_lastStateLog || !s_hasLastStateLogTime || (nowLog - s_lastStateLogTime) > std::chrono::seconds(3)) {
            Log(line);
            s_lastStateLog = line;
            s_lastStateLogTime = nowLog;
            s_hasLastStateLogTime = true;
        }
    }

    if (!snap.enabled || !snap.visible || !snap.renderCompanionOverlay) {
        DestroyAllCompanionWindows();
        return;
    }

    std::set<int> desiredMonitorIndices;
    for (const MonitorInfo& monitor : monitors) {
        if (!IsMonitorEnabledInMask(snap, monitor.maskBitIndex)) continue;
        // Companion window should only render on monitors other than the game monitor.
        if (monitor.maskBitIndex == gameMonitorMaskBit) continue;
        desiredMonitorIndices.insert(monitor.maskBitIndex);
    }

    {
        static std::string s_lastTopology;
        static auto s_lastTopologyLogTime = std::chrono::steady_clock::time_point{};
        static bool s_hasLastTopologyLogTime = false;
        std::ostringstream topo;
        topo << "enabled=" << snap.enabled << " visible=" << snap.visible << " companion=" << snap.renderCompanionOverlay
             << " mode=" << snap.renderMonitorMode << " mask=0x" << std::hex
             << snap.renderMonitorMask << std::dec << " monitors=" << monitors.size() << " game=display" << gameDisplayNumber
             << " bit=" << gameMonitorMaskBit << " targets=";
        bool first = true;
        for (int bitIndex : desiredMonitorIndices) {
            int displayNumber = bitIndex + 1;
            for (const MonitorInfo& monitor : monitors) {
                if (monitor.maskBitIndex == bitIndex) {
                    displayNumber = monitor.displayNumber;
                    break;
                }
            }
            if (!first) topo << ",";
            topo << "display" << displayNumber << "(bit" << bitIndex << ")";
            first = false;
        }
        if (desiredMonitorIndices.empty()) topo << "none";

        const std::string topoStr = topo.str();
        const auto nowLog = std::chrono::steady_clock::now();
        if (topoStr != s_lastTopology || !s_hasLastTopologyLogTime || (nowLog - s_lastTopologyLogTime) > std::chrono::seconds(3)) {
            Log("Stronghold companion topology: " + topoStr);
            s_lastTopology = topoStr;
            s_lastTopologyLogTime = nowLog;
            s_hasLastTopologyLogTime = true;
        }
    }

    if (desiredMonitorIndices.empty()) {
        DestroyAllCompanionWindows();
        return;
    }

    std::vector<int> stale;
    stale.reserve(s_companionWindows.size());
    for (const auto& kv : s_companionWindows) {
        if (desiredMonitorIndices.count(kv.first) == 0) stale.push_back(kv.first);
    }
    for (int index : stale) { DestroyCompanionWindow(index); }

    for (const MonitorInfo& monitor : monitors) {
        if (desiredMonitorIndices.count(monitor.maskBitIndex) == 0) continue;
        HWND hwnd = EnsureCompanionWindowForMonitor(monitor.maskBitIndex);
        if (!hwnd) continue;
        auto it = s_companionWindows.find(monitor.maskBitIndex);
        if (it == s_companionWindows.end()) continue;
        if (!RenderSnapshotToWindow(it->second, monitor.rect, snap)) {
            // Keep previous frame visible if a single render pass fails.
            continue;
        }
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
}

void ShutdownStrongholdCompanionOverlays() {
    DestroyAllCompanionWindows();

    if (s_gdiplusInitialized) {
        Gdiplus::GdiplusShutdown(s_gdiplusToken);
        s_gdiplusToken = 0;
        s_gdiplusInitialized = false;
    }
}
