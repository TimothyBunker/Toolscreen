#pragma once

#include "include/GL/glew.h"
#include <atomic>
#include <windows.h>

// This thread hooks into OBS's graphics-hook to redirect capture from backbuffer to our overlay texture

void StartObsHookThread();
void StopObsHookThread();

// When enabled, OBS's glBlitFramebuffer from backbuffer is redirected to our texture
extern std::atomic<bool> g_obsOverrideEnabled;
extern std::atomic<GLuint> g_obsOverrideTexture; // The texture OBS should capture from
extern std::atomic<int> g_obsOverrideWidth;
extern std::atomic<int> g_obsOverrideHeight;

// Pre-1.13 windowed mode: offset and size for OBS blit coordinate remapping
// When isPre113Windowed is true, OBS's blit coordinates need to be remapped
// from (0,0)→(windowW,windowH) to (offsetX,offsetY)→(offsetX+contentW,offsetY+contentH)
extern std::atomic<bool> g_obsPre113Windowed;
extern std::atomic<int> g_obsPre113OffsetX;
extern std::atomic<int> g_obsPre113OffsetY;
extern std::atomic<int> g_obsPre113ContentW;
extern std::atomic<int> g_obsPre113ContentH;

// Capture the current backbuffer to an FBO for OBS to use
// Call this after rendering the animated frame with OBS-specific content
// The hook will redirect OBS's capture from backbuffer to this captured texture
void CaptureBackbufferForObs(int width, int height);

// Set the override texture (called by render_thread after compositing)
void SetObsOverrideTexture(GLuint texture, int width, int height);

// Clear the override (OBS captures backbuffer normally)
void ClearObsOverride();

// Enable the override (call when returning to fullscreen mode)
void EnableObsOverride();

// Check if OBS graphics-hook is detected
bool IsObsHookDetected();

// Get the OBS capture texture (for shared context use only - e.g., OBS hook redirect)
// Returns 0 if no capture has been made yet
GLuint GetObsCaptureTexture();
int GetObsCaptureWidth();
int GetObsCaptureHeight();
