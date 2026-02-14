#include "obs_thread.h"
#include "profiler.h"
#include "render_thread.h"
#include "utils.h"
#include <mutex>
#include <thread>
#include <vector>

// MinHook for API hooking
#include "MinHook.h"

std::atomic<bool> g_obsOverrideEnabled{ false };
std::atomic<GLuint> g_obsOverrideTexture{ 0 };
std::atomic<int> g_obsOverrideWidth{ 0 };
std::atomic<int> g_obsOverrideHeight{ 0 };

// Pre-1.13 windowed mode coordinate remapping
std::atomic<bool> g_obsPre113Windowed{ false };
std::atomic<int> g_obsPre113OffsetX{ 0 };
std::atomic<int> g_obsPre113OffsetY{ 0 };
std::atomic<int> g_obsPre113ContentW{ 0 };
std::atomic<int> g_obsPre113ContentH{ 0 };

static std::atomic<bool> g_obsHookInitialized{ false };
static std::atomic<bool> g_obsHookActive{ false };

// Original function pointer for glBlitFramebuffer
typedef void(APIENTRY* PFN_glBlitFramebuffer)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                                              GLint dstY1, GLbitfield mask, GLenum filter);
static PFN_glBlitFramebuffer Real_glBlitFramebuffer = nullptr;

// FBO for redirecting OBS capture
static GLuint g_obsRedirectFBO = 0;
static std::mutex g_obsHookMutex;

// Used to capture the backbuffer after animated rendering (before user screen rendering)
static GLuint g_obsCaptureFBO = 0;
static GLuint g_obsCaptureTexture = 0;
static int g_obsCaptureWidth = 0;
static int g_obsCaptureHeight = 0;

// When OBS calls glBlitFramebuffer with READ_FRAMEBUFFER=0 (backbuffer),
// we redirect it to read from our animated OBS texture instead
static void APIENTRY Hook_glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                                            GLint dstY1, GLbitfield mask, GLenum filter) {
    // Check if we should override OBS capture
    if (g_obsOverrideEnabled.load(std::memory_order_acquire)) {
        // Check if this is OBS reading from backbuffer (FBO 0)
        GLint readFBO = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFBO);

        if (readFBO == 0) {
            // This is OBS trying to capture from backbuffer
            // First try the render thread's animated texture
            GLuint obsTexture = GetCompletedObsTexture();

            // Fall back to the captured backbuffer texture if render thread texture isn't ready
            if (obsTexture == 0) { obsTexture = g_obsOverrideTexture.load(std::memory_order_acquire); }

            // If no texture is ready yet, wait briefly for the first OBS frame to be rendered
            // This fixes the issue where the background doesn't show on the first animation
            // mode transition because the render thread hasn't completed yet
            if (obsTexture == 0) {
                static bool s_firstFrameLogged = false;
                // Wait up to 10ms for the first OBS frame to be ready
                for (int i = 0; i < 10 && obsTexture == 0; i++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    obsTexture = GetCompletedObsTexture();
                    if (obsTexture == 0) { obsTexture = g_obsOverrideTexture.load(std::memory_order_acquire); }
                }
                if (obsTexture != 0 && !s_firstFrameLogged) {
                    s_firstFrameLogged = true;
                    // First OBS frame ready after waiting
                }
            }

            if (obsTexture != 0) {
                PROFILE_SCOPE_CAT("OBS Capture Redirect", "OBS Hook");

                // Wait on the render thread's fence to ensure texture is fully rendered
                // glWaitSync is a GPU-side wait that doesn't block the CPU like glFinish
                GLsync fence = GetCompletedObsFence();
                if (fence) { glWaitSync(fence, 0, GL_TIMEOUT_IGNORED); }

                // Memory barrier to ensure we see the latest texture data from render thread
                // This is critical for cross-context texture sharing under GPU load
                glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);

                // Create redirect FBO if needed
                if (g_obsRedirectFBO == 0) { glGenFramebuffers(1, &g_obsRedirectFBO); }

                // Bind our texture as the read source
                glBindFramebuffer(GL_READ_FRAMEBUFFER, g_obsRedirectFBO);
                glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, obsTexture, 0);

                // Check framebuffer completeness
                GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
                if (status != GL_FRAMEBUFFER_COMPLETE) {
                    // Log once per unique status to avoid spam
                    static GLenum lastLoggedStatus = GL_FRAMEBUFFER_COMPLETE;
                    if (status != lastLoggedStatus) {
                        Log("[OBS Hook] WARNING: Redirect FBO incomplete! Status: " + std::to_string(status) +
                            ", Texture: " + std::to_string(obsTexture));
                        lastLoggedStatus = status;
                    }
                    // Fall back to original blit
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                    Real_glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
                    return;
                }

                // Call original blit with our FBO as source
                // For pre-1.13 windowed mode, remap source coordinates:
                // OBS expects content at (0,0)→(windowW,windowH) but our FBO has
                // content centered at (offsetX,offsetY)→(offsetX+contentW,offsetY+contentH)
                GLint blitSrcX0 = srcX0, blitSrcY0 = srcY0, blitSrcX1 = srcX1, blitSrcY1 = srcY1;
                if (g_obsPre113Windowed.load(std::memory_order_acquire)) {
                    int offsetX = g_obsPre113OffsetX.load(std::memory_order_acquire);
                    int offsetY = g_obsPre113OffsetY.load(std::memory_order_acquire);
                    // Remap: translate OBS's (0,0) based coords to our centered position
                    blitSrcX0 = srcX0 + offsetX;
                    blitSrcY0 = srcY0 + offsetY;
                    blitSrcX1 = srcX1 + offsetX;
                    blitSrcY1 = srcY1 + offsetY;
                }
                Real_glBlitFramebuffer(blitSrcX0, blitSrcY0, blitSrcX1, blitSrcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);

                // Restore to backbuffer (FBO 0)
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                return;
            }
        }
    }

    // Default: call original function unchanged
    Real_glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

void CaptureBackbufferForObs(int width, int height) {
    PROFILE_SCOPE_CAT("Capture Backbuffer for OBS", "OBS");

    // Create or resize capture FBO if needed
    if (g_obsCaptureFBO == 0 || width != g_obsCaptureWidth || height != g_obsCaptureHeight) {
        // Cleanup old resources
        if (g_obsCaptureTexture != 0) { glDeleteTextures(1, &g_obsCaptureTexture); }
        if (g_obsCaptureFBO == 0) { glGenFramebuffers(1, &g_obsCaptureFBO); }

        // Create new texture
        glGenTextures(1, &g_obsCaptureTexture);
        glBindTexture(GL_TEXTURE_2D, g_obsCaptureTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Attach to FBO
        glBindFramebuffer(GL_FRAMEBUFFER, g_obsCaptureFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_obsCaptureTexture, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);

        g_obsCaptureWidth = width;
        g_obsCaptureHeight = height;
    }

    // Save current FBO bindings
    GLint prevReadFBO = 0, prevDrawFBO = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFBO);

    // Blit from backbuffer to our capture FBO
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); // Read from backbuffer
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_obsCaptureFBO);

    // Use the real glBlitFramebuffer if hooked, otherwise use GLEW's
    if (Real_glBlitFramebuffer) {
        Real_glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    } else {
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    // Restore FBO bindings
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFBO);

    // Set this texture as the OBS override
    SetObsOverrideTexture(g_obsCaptureTexture, width, height);
}

void SetObsOverrideTexture(GLuint texture, int width, int height) {
    g_obsOverrideTexture.store(texture, std::memory_order_release);
    g_obsOverrideWidth.store(width, std::memory_order_release);
    g_obsOverrideHeight.store(height, std::memory_order_release);
    g_obsOverrideEnabled.store(true, std::memory_order_release);
}

void ClearObsOverride() { g_obsOverrideEnabled.store(false, std::memory_order_release); }

void EnableObsOverride() {
    // Only enable if the hook is active (was successfully initialized)
    if (g_obsHookActive.load(std::memory_order_acquire)) { g_obsOverrideEnabled.store(true, std::memory_order_release); }
}

GLuint GetObsCaptureTexture() { return g_obsCaptureTexture; }

int GetObsCaptureWidth() { return g_obsCaptureWidth; }

int GetObsCaptureHeight() { return g_obsCaptureHeight; }

bool IsObsHookDetected() {
    // Check if OBS graphics-hook64.dll is loaded
    return GetModuleHandleA("graphics-hook64.dll") != NULL;
}

void StartObsHookThread() {
    if (g_obsHookInitialized.load()) {
        return; // Already initialized
    }

    std::lock_guard<std::mutex> lock(g_obsHookMutex);
    if (g_obsHookInitialized.load()) {
        return; // Double-check after lock
    }

    Log("OBS Hook: Initializing...");

    // Get the address of glBlitFramebuffer from opengl32.dll or the game's context
    HMODULE opengl32 = GetModuleHandleA("opengl32.dll");
    if (!opengl32) {
        Log("OBS Hook: Failed to find opengl32.dll");
        return;
    }

    // glBlitFramebuffer is an extension, get it via wglGetProcAddress
    typedef PROC(WINAPI * PFN_wglGetProcAddress)(LPCSTR);
    PFN_wglGetProcAddress wglGetProcAddressPtr = (PFN_wglGetProcAddress)GetProcAddress(opengl32, "wglGetProcAddress");
    if (!wglGetProcAddressPtr) {
        Log("OBS Hook: Failed to get wglGetProcAddress");
        return;
    }

    void* blitAddr = (void*)wglGetProcAddressPtr("glBlitFramebuffer");
    if (!blitAddr) {
        Log("OBS Hook: Failed to get glBlitFramebuffer address");
        return;
    }

    // Initialize MinHook if not already done
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        Log("OBS Hook: Failed to initialize MinHook");
        return;
    }

    // Create hook for glBlitFramebuffer
    MH_STATUS status = MH_CreateHook(blitAddr, (void*)Hook_glBlitFramebuffer, (void**)&Real_glBlitFramebuffer);
    if (status != MH_OK) {
        Log("OBS Hook: Failed to create hook (status " + std::to_string(status) + ")");
        return;
    }

    // Enable the hook
    status = MH_EnableHook(blitAddr);
    if (status != MH_OK) {
        Log("OBS Hook: Failed to enable hook (status " + std::to_string(status) + ")");
        MH_RemoveHook(blitAddr);
        return;
    }

    g_obsHookActive.store(true);
    g_obsHookInitialized.store(true);

    // Enable the OBS override so the hook redirects captures to our render thread texture
    g_obsOverrideEnabled.store(true, std::memory_order_release);

    Log("OBS Hook: Successfully hooked glBlitFramebuffer");
}

void StopObsHookThread() {
    if (!g_obsHookInitialized.load()) { return; }

    std::lock_guard<std::mutex> lock(g_obsHookMutex);

    // Disable the OBS override first
    g_obsOverrideEnabled.store(false, std::memory_order_release);

    if (g_obsHookActive.load()) {
        // Disable and remove the hook
        HMODULE opengl32 = GetModuleHandleA("opengl32.dll");
        if (opengl32) {
            typedef PROC(WINAPI * PFN_wglGetProcAddress)(LPCSTR);
            PFN_wglGetProcAddress wglGetProcAddressPtr = (PFN_wglGetProcAddress)GetProcAddress(opengl32, "wglGetProcAddress");
            if (wglGetProcAddressPtr) {
                void* blitAddr = (void*)wglGetProcAddressPtr("glBlitFramebuffer");
                if (blitAddr) {
                    MH_DisableHook(blitAddr);
                    MH_RemoveHook(blitAddr);
                }
            }
        }
        g_obsHookActive.store(false);
    }

    // Cleanup redirect FBO
    if (g_obsRedirectFBO != 0) {
        glDeleteFramebuffers(1, &g_obsRedirectFBO);
        g_obsRedirectFBO = 0;
    }

    g_obsHookInitialized.store(false);
    Log("OBS Hook: Stopped");
}
