#include "fake_cursor.h"
#include "gui.h"
#include "utils.h"
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <vector>
#include <windows.h>

// Forward declaration for logging
void Log(const std::string& msg);

// Helper function to resolve a path relative to current working directory
static std::wstring ResolveCwdPath(const std::wstring& relPath) {
    std::filesystem::path cwdPath = std::filesystem::current_path();
    std::filesystem::path fullPath = cwdPath / relPath;
    return fullPath.wstring();
}

namespace CursorTextures {

// Cursor definition: maps cursor name to file path and load type
struct CursorDef {
    std::string name;
    std::wstring path;
    UINT loadType;
};

// Define all available cursors in one place
static const std::vector<CursorDef> SYSTEM_CURSORS = { { "Arrow", L"C:/Windows/Cursors/aero_arrow.cur", IMAGE_CURSOR },
                                                       { "Cross (Inverted, small)", L"C:/Windows/Cursors/cross_i.cur", IMAGE_CURSOR },
                                                       { "Cross (Inverted, medium)", L"C:/Windows/Cursors/cross_im.cur", IMAGE_CURSOR },
                                                       { "Cross (Inverted, large)", L"C:/Windows/Cursors/cross_il.cur", IMAGE_CURSOR },
                                                       { "Cross (Inverted, no outline)", L"C:/Windows/Cursors/cross_l.cur", IMAGE_CURSOR },
                                                       { "Cross (Small)", L"C:/Windows/Cursors/cross_r.cur", IMAGE_CURSOR },
                                                       { "Cross (Medium)", L"C:/Windows/Cursors/cross_rm.cur", IMAGE_CURSOR },
                                                       { "Cross (Large)", L"C:/Windows/Cursors/cross_rl.cur", IMAGE_CURSOR } };

// Dynamic list that includes both system and custom cursors
static std::vector<CursorDef> AVAILABLE_CURSORS;
static bool g_cursorDefsInitialized = false;

// Build the complete cursor list from system cursors + dynamic custom cursors
void InitializeCursorDefinitions() {
    if (g_cursorDefsInitialized) return;

    LogCategory("cursor_textures", "[CursorTextures] InitializeCursorDefinitions starting...");

    // Start with system cursors
    AVAILABLE_CURSORS = SYSTEM_CURSORS;
    LogCategory("cursor_textures", "[CursorTextures] Loaded " + std::to_string(SYSTEM_CURSORS.size()) + " system cursor definitions");

    // Verify system cursors exist
    int validSystemCursors = 0;
    for (const auto& cursor : SYSTEM_CURSORS) {
        if (std::filesystem::exists(cursor.path)) {
            validSystemCursors++;
        } else {
            LogCategory("cursor_textures", "[CursorTextures] WARNING: System cursor not found: " + WideToUtf8(cursor.path));
        }
    }
    LogCategory("cursor_textures", "[CursorTextures] Verified " + std::to_string(validSystemCursors) + "/" +
                                       std::to_string(SYSTEM_CURSORS.size()) + " system cursors exist on disk");

    // Scan the .config/toolscreen/cursors folder for .cur and .ico files
    try {
        // Build path to .config/toolscreen/cursors using GetToolscreenPath()
        std::wstring toolscreenPath = GetToolscreenPath();
        if (toolscreenPath.empty()) {
            LogCategory("cursor_textures", "[CursorTextures] ERROR: Failed to get toolscreen path - custom cursors will not be available");
            g_cursorDefsInitialized = true;
            return;
        }

        std::filesystem::path cursorsPath = std::filesystem::path(toolscreenPath) / "cursors";
        LogCategory("cursor_textures", "[CursorTextures] Scanning for custom cursors at: " + cursorsPath.string());

        if (!std::filesystem::exists(cursorsPath)) {
            LogCategory("cursor_textures", "[CursorTextures] Custom cursors folder does not exist: " + cursorsPath.string());
            LogCategory("cursor_textures", "[CursorTextures] To add custom cursors, create this folder and add .cur or .ico files");
        } else if (!std::filesystem::is_directory(cursorsPath)) {
            LogCategory("cursor_textures", "[CursorTextures] ERROR: Cursors path exists but is not a directory: " + cursorsPath.string());
        } else {
            int customCursorsFound = 0;
            int filesSkipped = 0;
            for (const auto& entry : std::filesystem::directory_iterator(cursorsPath)) {
                if (entry.is_regular_file()) {
                    auto ext = entry.path().extension().string();
                    // Convert extension to lowercase for comparison
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                    if (ext == ".cur" || ext == ".ico") {
                        std::string filename = entry.path().filename().stem().string(); // filename without extension
                        std::wstring filepath = entry.path().wstring();

                        // Determine load type based on extension
                        UINT loadType = (ext == ".ico") ? IMAGE_ICON : IMAGE_CURSOR;

                        // Add to cursor definitions
                        AVAILABLE_CURSORS.push_back({ filename, filepath, loadType });
                        LogCategory("cursor_textures", "[CursorTextures] Found custom cursor: " + filename + " (" + ext + ")");
                        customCursorsFound++;
                    } else {
                        filesSkipped++;
                    }
                }
            }
            LogCategory("cursor_textures", "[CursorTextures] Found " + std::to_string(customCursorsFound) + " custom cursor(s), skipped " +
                                               std::to_string(filesSkipped) + " non-cursor file(s)");
        }
    } catch (const std::filesystem::filesystem_error& e) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: Filesystem error scanning cursors folder: " + std::string(e.what()));
        LogCategory("cursor_textures", "[CursorTextures] Error code: " + std::to_string(e.code().value()) + " - " + e.code().message());
    } catch (const std::exception& e) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: Exception scanning cursors folder: " + std::string(e.what()));
    } catch (...) { LogCategory("cursor_textures", "[CursorTextures] ERROR: Unknown exception scanning cursors folder"); }

    LogCategory("cursor_textures", "[CursorTextures] InitializeCursorDefinitions complete. Total cursors available: " +
                                       std::to_string(AVAILABLE_CURSORS.size()));
    g_cursorDefsInitialized = true;
}

// Global cursor list and mutex
std::vector<CursorData> g_cursorList;
std::mutex g_cursorListMutex;

static const std::vector<int> STANDARD_SIZES = {
    16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80, 96, 112, 128, 144, 160, 192, 224, 256, 288, 320
};

// Helper function to load a single cursor and create all its data (texture, hotspot, etc.)
static bool LoadSingleCursor(const std::wstring& path, UINT loadType, int size, CursorData& outData) {
    // Validate parameters
    if (path.empty()) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: LoadSingleCursor called with empty path");
        return false;
    }
    if (size <= 0 || size > 512) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: LoadSingleCursor called with invalid size: " + std::to_string(size));
        return false;
    }

    // Resolve path relative to cwd if not absolute
    std::wstring resolvedPath = path;
    try {
        if (!std::filesystem::path(path).is_absolute()) { resolvedPath = ResolveCwdPath(path); }
    } catch (const std::exception& e) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: Failed to resolve path: " + std::string(e.what()));
        return false;
    }

    std::string pathStr = WideToUtf8(resolvedPath);
    LogCategory("cursor_textures", "[CursorTextures] Loading cursor: " + pathStr + " at size " + std::to_string(size) +
                                       " (type: " + (loadType == IMAGE_ICON ? "ICON" : "CURSOR") + ")");

    // Check if file exists before attempting to load
    if (!std::filesystem::exists(resolvedPath)) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: Cursor file does not exist: " + pathStr);
        return false;
    }

    // Store basic info (store original path, not resolved)
    outData.filePath = path;
    outData.size = size;
    outData.loadType = loadType;

    // Load cursor/icon from file with explicit size
    HCURSOR hCursor = (HCURSOR)LoadImageW(NULL, resolvedPath.c_str(), loadType, size, size, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (!hCursor) {
        DWORD err = GetLastError();
        std::string errMsg;
        switch (err) {
        case ERROR_FILE_NOT_FOUND:
            errMsg = "File not found";
            break;
        case ERROR_PATH_NOT_FOUND:
            errMsg = "Path not found";
            break;
        case ERROR_ACCESS_DENIED:
            errMsg = "Access denied";
            break;
        case ERROR_INVALID_PARAMETER:
            errMsg = "Invalid parameter";
            break;
        case ERROR_NOT_ENOUGH_MEMORY:
            errMsg = "Not enough memory";
            break;
        case ERROR_RESOURCE_TYPE_NOT_FOUND:
            errMsg = "Resource type not found (invalid cursor/icon format?)";
            break;
        default:
            errMsg = "Unknown error";
            break;
        }
        LogCategory("cursor_textures",
                    "[CursorTextures] ERROR: LoadImageW failed for '" + pathStr + "' - Error " + std::to_string(err) + ": " + errMsg);
        return false;
    }

    outData.hCursor = hCursor;

    // Get icon info
    ICONINFOEXW iconInfoEx = { 0 };
    iconInfoEx.cbSize = sizeof(ICONINFOEXW);
    bool hasIconInfoEx = GetIconInfoExW(hCursor, &iconInfoEx);

    if (!hasIconInfoEx) {
        DWORD err = GetLastError();
        LogCategory("cursor_textures", "[CursorTextures] ERROR: GetIconInfoExW failed with error " + std::to_string(err));
        DestroyCursor(hCursor);
        outData.hCursor = nullptr;
        return false;
    }

    // Get bitmap dimensions - handle both color and monochrome cursors
    BITMAP bmp;
    bool isMonochrome = (iconInfoEx.hbmColor == NULL);
    LogCategory("cursor_textures", "[CursorTextures] Cursor type: " + std::string(isMonochrome ? "monochrome" : "color"));

    if (isMonochrome) {
        if (!iconInfoEx.hbmMask) {
            LogCategory("cursor_textures", "[CursorTextures] ERROR: Monochrome cursor has no mask bitmap");
            DestroyCursor(hCursor);
            outData.hCursor = nullptr;
            return false;
        }
        if (!GetObject(iconInfoEx.hbmMask, sizeof(BITMAP), &bmp)) {
            DWORD err = GetLastError();
            LogCategory("cursor_textures", "[CursorTextures] ERROR: GetObject for mask bitmap failed with error " + std::to_string(err));
            DeleteObject(iconInfoEx.hbmMask);
            DestroyCursor(hCursor);
            outData.hCursor = nullptr;
            return false;
        }
    } else {
        if (!GetObject(iconInfoEx.hbmColor, sizeof(BITMAP), &bmp)) {
            DWORD err = GetLastError();
            LogCategory("cursor_textures", "[CursorTextures] ERROR: GetObject for color bitmap failed with error " + std::to_string(err));
            if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
            if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
            DestroyCursor(hCursor);
            outData.hCursor = nullptr;
            return false;
        }
    }

    int width = bmp.bmWidth;
    int height = isMonochrome ? bmp.bmHeight / 2 : bmp.bmHeight;

    // Validate bitmap dimensions
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        LogCategory("cursor_textures",
                    "[CursorTextures] ERROR: Invalid bitmap dimensions: " + std::to_string(width) + "x" + std::to_string(height));
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        DestroyCursor(hCursor);
        outData.hCursor = nullptr;
        return false;
    }

    LogCategory("cursor_textures", "[CursorTextures] Bitmap size: " + std::to_string(width) + "x" + std::to_string(height) +
                                       ", hotspot: (" + std::to_string(iconInfoEx.xHotspot) + ", " + std::to_string(iconInfoEx.yHotspot) +
                                       ")");

    // Store dimensions and hotspot
    outData.bitmapWidth = width;
    outData.bitmapHeight = height;
    outData.hotspotX = iconInfoEx.xHotspot;
    outData.hotspotY = iconInfoEx.yHotspot;

    // Create memory DC and extract bitmap data
    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        DWORD err = GetLastError();
        LogCategory("cursor_textures", "[CursorTextures] ERROR: GetDC(NULL) failed with error " + std::to_string(err));
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        DestroyCursor(hCursor);
        outData.hCursor = nullptr;
        return false;
    }

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        DWORD err = GetLastError();
        LogCategory("cursor_textures", "[CursorTextures] ERROR: CreateCompatibleDC failed with error " + std::to_string(err));
        ReleaseDC(NULL, hdcScreen);
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        DestroyCursor(hCursor);
        outData.hCursor = nullptr;
        return false;
    }

    // Allocate RGBA buffer
    std::vector<unsigned char> pixels(width * height * 4);

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    if (isMonochrome) {
        // For monochrome cursors, extract both AND and XOR masks from hbmMask
        // The mask bitmap is double height: top half = AND mask, bottom half = XOR mask
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, iconInfoEx.hbmMask);

        // Extract full mask bitmap (double height - contains both AND and XOR)
        // Need to allocate buffer for FULL double-height bitmap
        std::vector<unsigned char> maskData(width * bmp.bmHeight * 4);
        BITMAPINFO maskBmi = bmi;
        maskBmi.bmiHeader.biHeight = -bmp.bmHeight; // Full double height (negative for top-down)
        GetDIBits(hdcMem, iconInfoEx.hbmMask, 0, bmp.bmHeight, maskData.data(), &maskBmi, DIB_RGB_COLORS);

        // Create separate buffers for normal and inverted pixels
        std::vector<unsigned char> invertPixels(width * height * 4, 0);
        bool hasInverted = false;

        // Extract XOR mask (bottom half) - determines color (black/white)
        // Windows cursor mask logic:
        // AND=1 (white) -> transparent, AND=0 (black) -> opaque, XOR determines color
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                int idx = (y * width + x) * 4;
                int andIdx = (y * width + x) * 4;            // Top half of maskData (AND mask)
                int xorIdx = ((y + height) * width + x) * 4; // Bottom half of maskData (XOR mask)

                unsigned char andValue = maskData[andIdx]; // AND mask
                unsigned char xorValue = maskData[xorIdx]; // XOR mask

                // Windows monochrome cursor mask logic (complete specification):
                // AND=1, XOR=0 -> Transparent (screen shows through)
                // AND=0, XOR=0 -> Black pixel (opaque black)
                // AND=1, XOR=1 -> Inverted pixel (XOR with background)
                // AND=0, XOR=1 -> White pixel (opaque white)

                bool andBit = (andValue > 128);
                bool xorBit = (xorValue > 128);

                if (andBit && !xorBit) {
                    // AND=1, XOR=0 -> Transparent (normal texture)
                    pixels[idx + 0] = 0; // B
                    pixels[idx + 1] = 0; // G
                    pixels[idx + 2] = 0; // R
                    pixels[idx + 3] = 0; // A (transparent)
                } else if (!andBit && !xorBit) {
                    // AND=0, XOR=0 -> Black pixel (normal texture)
                    pixels[idx + 0] = 0;   // B
                    pixels[idx + 1] = 0;   // G
                    pixels[idx + 2] = 0;   // R
                    pixels[idx + 3] = 255; // A (opaque)
                } else if (andBit && xorBit) {
                    // AND=1, XOR=1 -> Inverted pixel (goes to invert mask texture)
                    pixels[idx + 0] = 0; // B (transparent in normal texture)
                    pixels[idx + 1] = 0; // G
                    pixels[idx + 2] = 0; // R
                    pixels[idx + 3] = 0; // A

                    // Mark as inverted in separate texture (white = invert this pixel)
                    invertPixels[idx + 0] = 255; // B
                    invertPixels[idx + 1] = 255; // G
                    invertPixels[idx + 2] = 255; // R
                    invertPixels[idx + 3] = 255; // A (opaque in invert mask)
                    hasInverted = true;
                } else {
                    // AND=0, XOR=1 -> White pixel (normal texture)
                    pixels[idx + 0] = 255; // B
                    pixels[idx + 1] = 255; // G
                    pixels[idx + 2] = 255; // R
                    pixels[idx + 3] = 255; // A (opaque)
                }
            }
        }

        // Store whether this cursor has inverted pixels
        outData.hasInvertedPixels = hasInverted;

        // Create invert mask texture if needed
        if (hasInverted) {
            // Clear any previous OpenGL errors
            while (glGetError() != GL_NO_ERROR) {}

            glGenTextures(1, &outData.invertMaskTexture);
            if (outData.invertMaskTexture == 0) {
                LogCategory("cursor_textures", "[CursorTextures] WARNING: Failed to create invert mask texture - glGenTextures returned 0");
                outData.hasInvertedPixels = false; // Disable inversion since we can't render it
            } else {
                glBindTexture(GL_TEXTURE_2D, outData.invertMaskTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, invertPixels.data());

                GLenum glErr = glGetError();
                if (glErr != GL_NO_ERROR) {
                    LogCategory("cursor_textures",
                                "[CursorTextures] WARNING: OpenGL error creating invert mask texture: " + std::to_string(glErr));
                    glDeleteTextures(1, &outData.invertMaskTexture);
                    outData.invertMaskTexture = 0;
                    outData.hasInvertedPixels = false;
                } else {
                    LogCategory("cursor_textures",
                                "[CursorTextures] Created invert mask texture ID " + std::to_string(outData.invertMaskTexture));
                }
                glBindTexture(GL_TEXTURE_2D, 0);
            }
        }

        SelectObject(hdcMem, hbmOld);
    } else {
        // Color cursor - extract from hbmColor
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, iconInfoEx.hbmColor);
        GetDIBits(hdcMem, iconInfoEx.hbmColor, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS);

        // Check if the color bitmap already has valid alpha values
        bool hasAlpha = false;
        if (bmp.bmBitsPixel == 32) {
            // Check if any pixel has non-zero alpha
            for (int i = 0; i < width * height; ++i) {
                if (pixels[i * 4 + 3] != 0) {
                    hasAlpha = true;
                    break;
                }
            }
        }

        // If no alpha channel in color bitmap, use mask bitmap
        if (!hasAlpha && iconInfoEx.hbmMask) {
            std::vector<unsigned char> maskPixels(width * height * 4);
            SelectObject(hdcMem, iconInfoEx.hbmMask);

            // Create proper BITMAPINFO for mask bitmap (may be different format than color)
            BITMAPINFO maskBmi = { 0 };
            maskBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            maskBmi.bmiHeader.biWidth = width;
            maskBmi.bmiHeader.biHeight = -height; // Top-down
            maskBmi.bmiHeader.biPlanes = 1;
            maskBmi.bmiHeader.biBitCount = 32;
            maskBmi.bmiHeader.biCompression = BI_RGB;

            GetDIBits(hdcMem, iconInfoEx.hbmMask, 0, height, maskPixels.data(), &maskBmi, DIB_RGB_COLORS);

            // Apply mask to alpha channel: black in mask = opaque (255), white in mask = transparent (0)
            // Use the red channel from mask (grayscale, so R=G=B anyway)
            for (int i = 0; i < width * height; ++i) {
                unsigned char maskValue = maskPixels[i * 4]; // Use red channel (same as G and B for grayscale)
                // Invert mask: black (0) becomes opaque (255), white (255) becomes transparent (0)
                pixels[i * 4 + 3] = 255 - maskValue;
            }
        } else if (!hasAlpha) {
            // No mask bitmap and no alpha - assume fully opaque
            for (int i = 0; i < width * height; ++i) { pixels[i * 4 + 3] = 255; }
        }

        SelectObject(hdcMem, hbmOld);
    }
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
    if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);

    // Clear any previous OpenGL errors before texture creation
    while (glGetError() != GL_NO_ERROR) {} // Flush error queue

    // Create OpenGL texture
    glGenTextures(1, &outData.texture);
    if (outData.texture == 0) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: glGenTextures returned 0 - OpenGL context may not be valid");
        DestroyCursor(outData.hCursor);
        outData.hCursor = nullptr;
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, outData.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels.data());

    // Check for OpenGL errors
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::string errStr;
        switch (err) {
        case GL_INVALID_ENUM:
            errStr = "GL_INVALID_ENUM";
            break;
        case GL_INVALID_VALUE:
            errStr = "GL_INVALID_VALUE";
            break;
        case GL_INVALID_OPERATION:
            errStr = "GL_INVALID_OPERATION";
            break;
        case GL_OUT_OF_MEMORY:
            errStr = "GL_OUT_OF_MEMORY";
            break;
        default:
            errStr = "Unknown (" + std::to_string(err) + ")";
            break;
        }
        LogCategory("cursor_textures", "[CursorTextures] ERROR: OpenGL error during texture creation: " + errStr);
        glDeleteTextures(1, &outData.texture);
        outData.texture = 0;
        if (outData.invertMaskTexture) {
            glDeleteTextures(1, &outData.invertMaskTexture);
            outData.invertMaskTexture = 0;
        }
        DestroyCursor(outData.hCursor);
        outData.hCursor = nullptr;
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    LogCategory("cursor_textures", "[CursorTextures] Successfully created texture ID " + std::to_string(outData.texture) + " (" +
                                       std::to_string(width) + "x" + std::to_string(height) + ") for " + WideToUtf8(path));
    return true;
}

void LoadCursorTextures() {
    std::lock_guard<std::mutex> lock(g_cursorListMutex);

    // Initialize cursor definitions (scan for custom cursors)
    InitializeCursorDefinitions();

    LogCategory("cursor_textures", "[CursorTextures] LoadCursorTextures called - loading initial cursors at default size (64px)");

    // Only load each cursor type at the default size (64px) initially
    int totalLoaded = 0;
    const int defaultSize = 64;

    for (const auto& cursorDef : AVAILABLE_CURSORS) {
        CursorData cursorData;
        if (LoadSingleCursor(cursorDef.path, cursorDef.loadType, defaultSize, cursorData)) {
            g_cursorList.push_back(cursorData);
            LogCategory("cursor_textures",
                        "[CursorTextures] Loaded " + WideToUtf8(cursorDef.path) + " at size " + std::to_string(defaultSize));
            totalLoaded++;
        } else {
            LogCategory("cursor_textures",
                        "[CursorTextures] Failed to load " + WideToUtf8(cursorDef.path) + " at size " + std::to_string(defaultSize));
        }
    }

    LogCategory("cursor_textures", "[CursorTextures] Finished loading " + std::to_string(totalLoaded) + " default cursor variants");
}

// Load a cursor at a specific size if not already loaded
// Returns pointer to cursor data, or nullptr on failure
// NOTE: Caller must NOT hold g_cursorListMutex when calling this function
const CursorData* LoadOrFindCursor(const std::wstring& path, UINT loadType, int size) {
    std::string pathStr = WideToUtf8(path);

    if (path.empty()) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: LoadOrFindCursor called with empty path");
        return nullptr;
    }

    // First check if already loaded
    {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        for (const auto& cursor : g_cursorList) {
            if (cursor.filePath == path && cursor.size == size) {
                return &cursor; // Found existing, no need to log (would spam)
            }
        }
    }

    // Not found - load it now
    LogCategory("cursor_textures", "[CursorTextures] Loading cursor on-demand: " + pathStr + " at size " + std::to_string(size));
    CursorData newCursorData;
    if (LoadSingleCursor(path, loadType, size, newCursorData)) {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        g_cursorList.push_back(newCursorData);
        LogCategory("cursor_textures",
                    "[CursorTextures] Successfully loaded on-demand cursor. Total loaded: " + std::to_string(g_cursorList.size()));
        // Return pointer to the newly added cursor (last element)
        return &g_cursorList.back();
    } else {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: Failed to load cursor on-demand: " + pathStr);
        return nullptr;
    }
}

const CursorData* FindCursor(const std::wstring& path, int size) {
    if (path.empty()) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: FindCursor called with empty path");
        return nullptr;
    }

    // First try to find existing cursor
    {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        for (const auto& cursor : g_cursorList) {
            if (cursor.filePath == path && cursor.size == size) { return &cursor; }
        }
    }

    // Not found - need to determine load type from path extension
    UINT loadType = IMAGE_CURSOR;
    try {
        std::filesystem::path fsPath(path);
        std::string ext = fsPath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".ico") {
            loadType = IMAGE_ICON;
        } else if (ext != ".cur" && ext != ".ani") {
            LogCategory("cursor_textures", "[CursorTextures] WARNING: Unexpected cursor file extension: " + ext + ", treating as cursor");
        }
    } catch (const std::exception& e) {
        LogCategory("cursor_textures",
                    "[CursorTextures] WARNING: Failed to parse path extension: " + std::string(e.what()) + ", defaulting to IMAGE_CURSOR");
    }

    // Load on-demand
    return LoadOrFindCursor(path, loadType, size);
}

const CursorData* FindCursorByHandle(HCURSOR hCursor) {
    std::lock_guard<std::mutex> lock(g_cursorListMutex);

    for (const auto& cursor : g_cursorList) {
        if (cursor.hCursor == hCursor) { return &cursor; }
    }
    return nullptr;
}

// Helper function to create texture from an existing HCURSOR handle
// Does NOT take ownership of hCursor - caller keeps it
static bool CreateTextureFromHandle(HCURSOR hCursor, CursorData& outData) {
    if (!hCursor) { return false; }

    // Get icon info
    ICONINFOEXW iconInfoEx = { 0 };
    iconInfoEx.cbSize = sizeof(ICONINFOEXW);
    if (!GetIconInfoExW(hCursor, &iconInfoEx)) { return false; }

    // Get bitmap dimensions - handle both color and monochrome cursors
    BITMAP bmp;
    bool isMonochrome = (iconInfoEx.hbmColor == NULL);

    if (isMonochrome) {
        if (!iconInfoEx.hbmMask || !GetObject(iconInfoEx.hbmMask, sizeof(BITMAP), &bmp)) {
            if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
            return false;
        }
    } else {
        if (!GetObject(iconInfoEx.hbmColor, sizeof(BITMAP), &bmp)) {
            if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
            if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
            return false;
        }
    }

    int width = bmp.bmWidth;
    int height = isMonochrome ? bmp.bmHeight / 2 : bmp.bmHeight;

    // Validate dimensions
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        return false;
    }

    // Store basic info
    outData.hCursor = hCursor;
    outData.filePath = L"<system>";
    outData.size = 0;
    outData.loadType = IMAGE_CURSOR;
    outData.bitmapWidth = width;
    outData.bitmapHeight = height;
    outData.hotspotX = iconInfoEx.xHotspot;
    outData.hotspotY = iconInfoEx.yHotspot;

    // Create memory DC and extract bitmap data
    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        return false;
    }

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ReleaseDC(NULL, hdcScreen);
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        return false;
    }

    std::vector<unsigned char> pixels(width * height * 4);

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    if (isMonochrome) {
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, iconInfoEx.hbmMask);
        std::vector<unsigned char> maskData(width * bmp.bmHeight * 4);
        BITMAPINFO maskBmi = bmi;
        maskBmi.bmiHeader.biHeight = -bmp.bmHeight;
        GetDIBits(hdcMem, iconInfoEx.hbmMask, 0, bmp.bmHeight, maskData.data(), &maskBmi, DIB_RGB_COLORS);

        std::vector<unsigned char> invertPixels(width * height * 4, 0);
        bool hasInverted = false;

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                int idx = (y * width + x) * 4;
                int andIdx = (y * width + x) * 4;
                int xorIdx = ((y + height) * width + x) * 4;
                unsigned char andValue = maskData[andIdx];
                unsigned char xorValue = maskData[xorIdx];
                bool andBit = (andValue > 128);
                bool xorBit = (xorValue > 128);

                if (andBit && !xorBit) {
                    pixels[idx + 0] = pixels[idx + 1] = pixels[idx + 2] = pixels[idx + 3] = 0;
                } else if (!andBit && !xorBit) {
                    pixels[idx + 0] = pixels[idx + 1] = pixels[idx + 2] = 0;
                    pixels[idx + 3] = 255;
                } else if (andBit && xorBit) {
                    pixels[idx + 0] = pixels[idx + 1] = pixels[idx + 2] = pixels[idx + 3] = 0;
                    invertPixels[idx + 0] = invertPixels[idx + 1] = invertPixels[idx + 2] = invertPixels[idx + 3] = 255;
                    hasInverted = true;
                } else {
                    pixels[idx + 0] = pixels[idx + 1] = pixels[idx + 2] = pixels[idx + 3] = 255;
                }
            }
        }

        outData.hasInvertedPixels = hasInverted;
        if (hasInverted) {
            while (glGetError() != GL_NO_ERROR) {}
            glGenTextures(1, &outData.invertMaskTexture);
            if (outData.invertMaskTexture != 0) {
                glBindTexture(GL_TEXTURE_2D, outData.invertMaskTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, invertPixels.data());
                glBindTexture(GL_TEXTURE_2D, 0);
            }
        }
        SelectObject(hdcMem, hbmOld);
    } else {
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, iconInfoEx.hbmColor);
        GetDIBits(hdcMem, iconInfoEx.hbmColor, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS);

        bool hasAlpha = false;
        if (bmp.bmBitsPixel == 32) {
            for (int i = 0; i < width * height; ++i) {
                if (pixels[i * 4 + 3] != 0) {
                    hasAlpha = true;
                    break;
                }
            }
        }

        if (!hasAlpha && iconInfoEx.hbmMask) {
            std::vector<unsigned char> maskPixels(width * height * 4);
            SelectObject(hdcMem, iconInfoEx.hbmMask);
            BITMAPINFO maskBmi = { 0 };
            maskBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            maskBmi.bmiHeader.biWidth = width;
            maskBmi.bmiHeader.biHeight = -height;
            maskBmi.bmiHeader.biPlanes = 1;
            maskBmi.bmiHeader.biBitCount = 32;
            maskBmi.bmiHeader.biCompression = BI_RGB;
            GetDIBits(hdcMem, iconInfoEx.hbmMask, 0, height, maskPixels.data(), &maskBmi, DIB_RGB_COLORS);
            for (int i = 0; i < width * height; ++i) { pixels[i * 4 + 3] = 255 - maskPixels[i * 4]; }
        } else if (!hasAlpha) {
            for (int i = 0; i < width * height; ++i) { pixels[i * 4 + 3] = 255; }
        }
        SelectObject(hdcMem, hbmOld);
    }

    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
    if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);

    // Create OpenGL texture
    while (glGetError() != GL_NO_ERROR) {}
    glGenTextures(1, &outData.texture);
    if (outData.texture == 0) { return false; }

    glBindTexture(GL_TEXTURE_2D, outData.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels.data());

    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &outData.texture);
        outData.texture = 0;
        if (outData.invertMaskTexture) {
            glDeleteTextures(1, &outData.invertMaskTexture);
            outData.invertMaskTexture = 0;
        }
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

const CursorData* LoadOrFindCursorFromHandle(HCURSOR hCursor) {
    if (!hCursor) { return nullptr; }

    // First check if already loaded
    {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        for (const auto& cursor : g_cursorList) {
            if (cursor.hCursor == hCursor) { return &cursor; }
        }
    }

    // Not found - create texture from the handle
    CursorData newData;
    if (!CreateTextureFromHandle(hCursor, newData)) { return nullptr; }

    // Add to global list
    std::lock_guard<std::mutex> lock(g_cursorListMutex);
    g_cursorList.push_back(newData);
    return &g_cursorList.back();
}

const CursorData* GetSelectedCursor(const std::string& gameState, int size) {
    // Get cursor name and size for the current game state from config snapshot (thread-safe)
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return nullptr; // Config not yet published

    std::string selectedCursorName = ""; // Default fallback (uses first available)
    int selectedSize = size;             // Use provided size, or override from config

    if (!cfgSnap->cursors.enabled) {
        return nullptr; // Cursor customization disabled
    }

    // Map game state to cursor config
    if (gameState == "title") {
        selectedCursorName = cfgSnap->cursors.title.cursorName;
        selectedSize = cfgSnap->cursors.title.cursorSize;
    } else if (gameState == "wall") {
        selectedCursorName = cfgSnap->cursors.wall.cursorName;
        selectedSize = cfgSnap->cursors.wall.cursorSize;
    } else {
        // Everything else (ingame, waiting, generating, etc.) maps to "ingame"
        selectedCursorName = cfgSnap->cursors.ingame.cursorName;
        selectedSize = cfgSnap->cursors.ingame.cursorSize;
    }

    // Map cursor name to file path using helper
    std::wstring cursorPath;
    UINT loadType = IMAGE_CURSOR;
    GetCursorPathByName(selectedCursorName, cursorPath, loadType);

    // Find the cursor at the requested size (now using selectedSize from config)
    const CursorData* cursorData = FindCursor(cursorPath, selectedSize);
    if (cursorData) { return cursorData; }

    // Fallback: try any available cursor from the loaded list
    Log("[GetSelectedCursor] Cursor '" + selectedCursorName + "' not found at size " + std::to_string(selectedSize) + ", trying fallback");

    // Try to find any loaded cursor at the requested size first
    {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        for (const auto& cursor : g_cursorList) {
            if (cursor.size == selectedSize && cursor.texture != 0) {
                Log("[GetSelectedCursor] Fallback: using cursor from " + WideToUtf8(cursor.filePath));
                return &cursor;
            }
        }
        // If no cursor at requested size, try any loaded cursor with valid texture
        for (const auto& cursor : g_cursorList) {
            if (cursor.texture != 0) {
                Log("[GetSelectedCursor] Fallback: using cursor from " + WideToUtf8(cursor.filePath) + " at size " +
                    std::to_string(cursor.size));
                return &cursor;
            }
        }
    }

    // No cursors available at all - return nullptr (will render nothing)
    Log("[GetSelectedCursor] No fallback cursor available, rendering nothing");
    return nullptr;
}

bool GetCursorPathByName(const std::string& cursorName, std::wstring& outPath, UINT& outLoadType) {
    // Ensure cursor definitions are initialized
    if (!g_cursorDefsInitialized) { InitializeCursorDefinitions(); }

    // Find cursor definition by name
    const CursorDef* selectedDef = nullptr;
    for (const auto& def : AVAILABLE_CURSORS) {
        if (def.name == cursorName) {
            selectedDef = &def;
            break;
        }
    }

    if (selectedDef) {
        outPath = selectedDef->path;
        outLoadType = selectedDef->loadType;
        // Log("[CursorTextures] GetCursorPathByName: Resolved '" + cursorName + "' to " + std::string(outPath.begin(), outPath.end()));
        return true;
    } else {
        // Unknown cursor name - try to use first available cursor as fallback
        LogCategory("cursor_textures", "[CursorTextures] WARNING: Unknown cursor name '" + cursorName + "'");
        LogCategory("cursor_textures", "[CursorTextures] Available cursors: " + std::to_string(AVAILABLE_CURSORS.size()));
        for (const auto& def : AVAILABLE_CURSORS) { LogCategory("cursor_textures", "[CursorTextures]   - " + def.name); }

        // Use first available cursor as fallback if any exist
        if (!AVAILABLE_CURSORS.empty()) {
            outPath = AVAILABLE_CURSORS[0].path;
            outLoadType = AVAILABLE_CURSORS[0].loadType;
            LogCategory("cursor_textures", "[CursorTextures] Using first available cursor as fallback: " + AVAILABLE_CURSORS[0].name);
            return false; // Still return false to indicate original cursor wasn't found
        }

        // No cursors available at all
        outPath = L"";
        outLoadType = IMAGE_CURSOR;
        LogCategory("cursor_textures", "[CursorTextures] ERROR: No cursors available for fallback");
        return false;
    }
}

// Check if a cursor file exists at the given path
bool IsCursorFileValid(const std::string& cursorName) {
    // Ensure cursor definitions are initialized
    if (!g_cursorDefsInitialized) { InitializeCursorDefinitions(); }

    if (cursorName.empty()) {
        LogCategory("cursor_textures", "[CursorTextures] IsCursorFileValid: Empty cursor name provided");
        return false;
    }

    // Find cursor definition by name
    const CursorDef* selectedDef = nullptr;
    for (const auto& def : AVAILABLE_CURSORS) {
        if (def.name == cursorName) {
            selectedDef = &def;
            break;
        }
    }

    if (!selectedDef) {
        LogCategory("cursor_textures", "[CursorTextures] IsCursorFileValid: Cursor '" + cursorName + "' not found in definitions");
        return false;
    }

    // Resolve path
    std::wstring resolvedPath = selectedDef->path;
    try {
        if (!std::filesystem::path(selectedDef->path).is_absolute()) { resolvedPath = ResolveCwdPath(selectedDef->path); }
    } catch (const std::exception& e) {
        LogCategory("cursor_textures",
                    "[CursorTextures] IsCursorFileValid: Failed to resolve path for '" + cursorName + "': " + std::string(e.what()));
        return false;
    }

    // Check if file exists
    bool exists = std::filesystem::exists(resolvedPath);
    if (!exists) {
        LogCategory("cursor_textures", "[CursorTextures] IsCursorFileValid: Cursor file does not exist: " + WideToUtf8(resolvedPath));
    }
    return exists;
}

void Cleanup() {
    std::lock_guard<std::mutex> lock(g_cursorListMutex);

    LogCategory("cursor_textures",
                "[CursorTextures] Cleanup: Starting cleanup of " + std::to_string(g_cursorList.size()) + " cursor entries");

    int texturesDeleted = 0;
    int invertMasksDeleted = 0;
    int cursorsDestroyed = 0;

    for (auto& cursor : g_cursorList) {
        if (cursor.texture) {
            glDeleteTextures(1, &cursor.texture);
            cursor.texture = 0;
            texturesDeleted++;
        }
        if (cursor.invertMaskTexture) {
            glDeleteTextures(1, &cursor.invertMaskTexture);
            cursor.invertMaskTexture = 0;
            invertMasksDeleted++;
        }
        if (cursor.hCursor) {
            DestroyCursor(cursor.hCursor);
            cursor.hCursor = nullptr;
            cursorsDestroyed++;
        }
    }

    g_cursorList.clear();
    LogCategory("cursor_textures", "[CursorTextures] Cleanup complete: " + std::to_string(texturesDeleted) + " textures, " +
                                       std::to_string(invertMasksDeleted) + " invert masks, " + std::to_string(cursorsDestroyed) +
                                       " cursor handles");
}

std::vector<std::string> GetAvailableCursorNames() {
    // Ensure cursor definitions are initialized
    if (!g_cursorDefsInitialized) { InitializeCursorDefinitions(); }

    std::vector<std::string> names;
    for (const auto& cursor : AVAILABLE_CURSORS) { names.push_back(cursor.name); }
    return names;
}

} // namespace CursorTextures

// Static counter for rate-limited logging in RenderFakeCursor
static int s_fakeCursorLogCounter = 0;
static const int FAKE_CURSOR_LOG_INTERVAL = 300; // Log every N frames

void RenderFakeCursor(HWND hwnd, int windowWidth, int windowHeight) {
    s_fakeCursorLogCounter++;
    bool shouldLog = (s_fakeCursorLogCounter % FAKE_CURSOR_LOG_INTERVAL == 0);

    // Get current cursor
    CURSORINFO cursorInfo = { 0 };
    cursorInfo.cbSize = sizeof(CURSORINFO);
    if (!GetCursorInfo(&cursorInfo)) {
        if (shouldLog) {
            DWORD err = GetLastError();
            Log("[FakeCursor] GetCursorInfo failed with error " + std::to_string(err));
        }
        return;
    }
    if (!cursorInfo.hCursor) { return; }                  // Normal when cursor is hidden
    if (!(cursorInfo.flags & CURSOR_SHOWING)) { return; } // Normal when cursor is hidden

    // Get cursor data from our preloaded list by HCURSOR handle
    const CursorTextures::CursorData* cursorData = CursorTextures::FindCursorByHandle(cursorInfo.hCursor);

    if (!cursorData) {
        // This is expected for system cursors we haven't loaded - not necessarily an error
        // Only log periodically to avoid spam
        if (shouldLog) {
            Log("[FakeCursor] Cursor handle 0x" + std::to_string(reinterpret_cast<uintptr_t>(cursorInfo.hCursor)) +
                " not found in loaded cursors (may be a system cursor)");
        }
        return;
    }

    // Get cursor position in screen coordinates
    POINT cursorPos;
    if (!GetCursorPos(&cursorPos)) {
        if (shouldLog) {
            DWORD err = GetLastError();
            Log("[FakeCursor] GetCursorPos failed with error " + std::to_string(err));
        }
        return;
    }

    // Convert to window client coordinates
    if (!ScreenToClient(hwnd, &cursorPos)) {
        if (shouldLog) {
            DWORD err = GetLastError();
            Log("[FakeCursor] ScreenToClient failed with error " + std::to_string(err));
        }
        return;
    }

    // Get actual game window dimensions (what the user sees)
    RECT gameRect;
    if (!GetClientRect(hwnd, &gameRect)) {
        if (shouldLog) {
            DWORD err = GetLastError();
            Log("[FakeCursor] GetClientRect failed with error " + std::to_string(err));
        }
        return;
    }
    int gameWidth = gameRect.right - gameRect.left;
    int gameHeight = gameRect.bottom - gameRect.top;

    // Guard against divide-by-zero
    if (gameWidth == 0 || gameHeight == 0) { return; }

    // Calculate scaled cursor size using the actual bitmap dimensions
    // The bitmap is already at the user's desired cursor size (includes Windows cursor scaling)
    // We just need to scale it by the window-to-game ratio

    float offset = cursorData->loadType == IMAGE_CURSOR ? 1.5f : 1.0f;

    int systemCursorWidth = cursorData->bitmapWidth;
    int systemCursorHeight = cursorData->bitmapHeight;
    int scaledCursorWidth = (systemCursorWidth * windowWidth) / gameWidth;
    int scaledCursorHeight = (systemCursorHeight * windowHeight) / gameHeight;

    // Scale hotspot proportionally (hotspot is already in bitmap space)
    int scaledHotspotX = static_cast<int>((cursorData->hotspotX * scaledCursorWidth * offset) / systemCursorWidth);
    int scaledHotspotY = static_cast<int>((cursorData->hotspotY * scaledCursorHeight * offset) / systemCursorHeight);

    // Use scaled dimensions for rendering
    int renderWidth = static_cast<int>(scaledCursorWidth * offset);
    int renderHeight = static_cast<int>(scaledCursorHeight * offset);

    // Adjust cursor position to account for scaled hotspot
    int cursorX = cursorPos.x - scaledHotspotX;
    int cursorY = cursorPos.y - scaledHotspotY;

    // Render cursor using OpenGL at native bitmap resolution
    // First render at normal position, then also render at (0,0) for debugging
    auto RenderCursorQuad = [&](int x, int y) {
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0);
        glVertex2i(x, y);
        glTexCoord2f(1, 0);
        glVertex2i(x + renderWidth, y);
        glTexCoord2f(1, 1);
        glVertex2i(x + renderWidth, y + renderHeight);
        glTexCoord2f(0, 1);
        glVertex2i(x, y + renderHeight);
        glEnd();
    };

    if (renderWidth > 0 && renderHeight > 0 && renderWidth < 512 && renderHeight < 512) {
        // Save extensive GL state
        GLboolean oldBlend = glIsEnabled(GL_BLEND);
        GLboolean oldDepth = glIsEnabled(GL_DEPTH_TEST);
        GLboolean oldTexture2D = glIsEnabled(GL_TEXTURE_2D);
        GLboolean oldScissor = glIsEnabled(GL_SCISSOR_TEST);
        GLboolean oldCullFace = glIsEnabled(GL_CULL_FACE);
        GLint oldBlendSrc, oldBlendDst;
        glGetIntegerv(GL_BLEND_SRC, &oldBlendSrc);
        glGetIntegerv(GL_BLEND_DST, &oldBlendDst);

        GLint oldProgram;
        glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);

        // Ensure we're drawing to the back buffer (framebuffer 0)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Disable scissor test in case it's cutting off our rendering
        glDisable(GL_SCISSOR_TEST);

        // Set up for 2D overlay rendering
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(0); // Fixed function pipeline

        // Set up orthographic projection (pixel coordinates)
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0, windowWidth, windowHeight, 0, -1, 1); // Y down

        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();

        // Render normal cursor pixels first (with alpha blending)
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, cursorData->texture);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Render at normal position
        RenderCursorQuad(cursorX, cursorY);

        // Render inverted pixels if this cursor has them (with XOR blending)
        if (cursorData->hasInvertedPixels && cursorData->invertMaskTexture != 0) {
            glBindTexture(GL_TEXTURE_2D, cursorData->invertMaskTexture);

            // Use XOR blend function to invert background colors
            // GL_ONE_MINUS_DST_COLOR inverts the destination color
            // GL_ONE_MINUS_SRC_ALPHA respects the mask's alpha channel (transparent where alpha=0, invert where alpha=255)
            glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);

            // Render inverted regions at same position
            RenderCursorQuad(cursorX, cursorY);
        }

        // Restore matrices
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);

        // Restore all saved state
        if (!oldTexture2D) glDisable(GL_TEXTURE_2D);
        if (!oldBlend) glDisable(GL_BLEND);
        if (oldDepth) glEnable(GL_DEPTH_TEST);
        if (oldScissor) glEnable(GL_SCISSOR_TEST);
        if (oldCullFace) glEnable(GL_CULL_FACE);
        glBlendFunc(oldBlendSrc, oldBlendDst);
        glUseProgram(oldProgram);

        // Force flush to ensure rendering happens
        glFlush();
    }
}
