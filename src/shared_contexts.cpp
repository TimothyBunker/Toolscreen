#include "shared_contexts.h"
#include "utils.h"

#include <GL/glew.h>
#include <GL/wglew.h>

// Pre-created shared contexts
std::atomic<HGLRC> g_sharedRenderContext{ nullptr };
std::atomic<HGLRC> g_sharedMirrorContext{ nullptr };
std::atomic<HDC> g_sharedContextDC{ nullptr };
std::atomic<bool> g_sharedContextsReady{ false };

bool InitializeSharedContexts(void* gameGLContext, HDC hdc) {
    if (g_sharedContextsReady.load()) {
        return true; // Already initialized
    }

    if (!gameGLContext || !hdc) {
        Log("SharedContexts: Invalid game context or DC");
        return false;
    }

    HGLRC gameContext = (HGLRC)gameGLContext;

    LogCategory("init", "SharedContexts: Initializing all shared contexts...");

    // Store the DC for later use
    g_sharedContextDC.store(hdc);

    // Create all contexts first (before any sharing)
    HGLRC renderContext = wglCreateContext(hdc);
    if (!renderContext) {
        Log("SharedContexts: Failed to create render context (error " + std::to_string(GetLastError()) + ")");
        return false;
    }

    HGLRC mirrorContext = wglCreateContext(hdc);
    if (!mirrorContext) {
        Log("SharedContexts: Failed to create mirror context (error " + std::to_string(GetLastError()) + ")");
        wglDeleteContext(renderContext);
        return false;
    }

    LogCategory("init", "SharedContexts: Created 2 contexts, now sharing with game...");

    // Now share all contexts with the game context
    // wglShareLists must be called before any context is made current!

    // Share render context with game
    SetLastError(0);
    if (!wglShareLists(gameContext, renderContext)) {
        DWORD err = GetLastError();
        // Try reverse order
        if (!wglShareLists(renderContext, gameContext)) {
            Log("SharedContexts: Failed to share render context (error " + std::to_string(err) + ", " + std::to_string(GetLastError()) +
                ")");
            wglDeleteContext(renderContext);
            wglDeleteContext(mirrorContext);
            return false;
        }
    }
    LogCategory("init", "SharedContexts: Render context shared with game");

    // Share mirror context with game (since game is now in a share group, all must share with game)
    SetLastError(0);
    if (!wglShareLists(gameContext, mirrorContext)) {
        DWORD err = GetLastError();
        if (!wglShareLists(mirrorContext, gameContext)) {
            Log("SharedContexts: Failed to share mirror context (error " + std::to_string(err) + ", " + std::to_string(GetLastError()) +
                ")");
            wglDeleteContext(renderContext);
            wglDeleteContext(mirrorContext);
            return false;
        }
    }
    LogCategory("init", "SharedContexts: Mirror context shared with game");

    // All contexts created and shared successfully!
    g_sharedRenderContext.store(renderContext);
    g_sharedMirrorContext.store(mirrorContext);
    g_sharedContextsReady.store(true);

    LogCategory("init", "SharedContexts: All contexts initialized and shared successfully");
    return true;
}

void CleanupSharedContexts() {
    g_sharedContextsReady.store(false);

    HGLRC render = g_sharedRenderContext.exchange(nullptr);
    HGLRC mirror = g_sharedMirrorContext.exchange(nullptr);

    // Only delete if not already deleted by their respective threads
    // Note: Threads should set these to nullptr when they clean up
    if (render) { wglDeleteContext(render); }
    if (mirror) { wglDeleteContext(mirror); }

    g_sharedContextDC.store(nullptr);
    Log("SharedContexts: Cleaned up");
}

HGLRC GetSharedRenderContext() { return g_sharedRenderContext.load(); }

HGLRC GetSharedMirrorContext() { return g_sharedMirrorContext.load(); }

HDC GetSharedContextDC() { return g_sharedContextDC.load(); }

bool AreSharedContextsReady() { return g_sharedContextsReady.load(); }
