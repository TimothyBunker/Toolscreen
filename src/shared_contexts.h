#pragma once

#include <Windows.h>
#include <atomic>

// Centralized OpenGL context sharing management
// All contexts that need to share textures must be created and shared at the same time,
// before any of them are made current on their respective threads.
//
// This is required because wglShareLists fails (error 170) if either context is already
// part of a different share group.

// Pre-created shared contexts (created on main thread, used by worker threads)
extern std::atomic<HGLRC> g_sharedRenderContext;    // For render thread
extern std::atomic<HGLRC> g_sharedMirrorContext;    // For mirror capture thread
// IMPORTANT: Each worker thread must use its own DC/drawable.
// Using the same HDC for two contexts on different threads is undefined on many drivers and
// can result in black frames / invisible mirrors.
extern std::atomic<HDC> g_sharedRenderContextDC;     // DC for render thread context
extern std::atomic<HDC> g_sharedMirrorContextDC;     // DC for mirror capture thread context
// Backward-compat (legacy). Prefer the specific getters below.
extern std::atomic<HDC> g_sharedContextDC;

// Whether shared contexts have been successfully initialized
extern std::atomic<bool> g_sharedContextsReady;

// Initialize all shared contexts at once
// Must be called from main thread with a valid GL context current
// Creates render and mirror contexts and shares them all with the game context
// Returns true if all contexts were created and shared successfully
bool InitializeSharedContexts(void* gameGLContext, HDC hdc);

// Cleanup all shared contexts
// Call when DLL is unloading
void CleanupSharedContexts();

// Get a pre-created shared context for a specific purpose
// Returns the pre-shared context, or nullptr if not available
HGLRC GetSharedRenderContext();
HGLRC GetSharedMirrorContext();
HDC GetSharedRenderContextDC();
HDC GetSharedMirrorContextDC();
// Backward-compat (legacy). Returns render DC.
HDC GetSharedContextDC();

// Check if shared contexts are ready
bool AreSharedContextsReady();
