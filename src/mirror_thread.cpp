#include "mirror_thread.h"
#include "gui.h"
#include "logic_thread.h"
#include "profiler.h"
#include "render.h"
#include "shared_contexts.h"
#include "utils.h"
#include <algorithm>
#include <thread>

// Thread runs independently, capturing game content to back-buffer FBOs
static std::thread g_mirrorCaptureThread;
std::atomic<bool> g_mirrorCaptureRunning{ false }; // Non-static for external access
static std::atomic<bool> g_mirrorCaptureShouldStop{ false };

// Safe capture window flag: true during SwapBuffers hook execution
std::atomic<bool> g_safeToCapture{ false };

static HGLRC g_mirrorCaptureContext = NULL;
static HDC g_mirrorCaptureDC = NULL;
static bool g_mirrorContextIsShared = false; // True if using pre-shared context

// Shared capture data (main thread writes, capture thread reads)
std::vector<ThreadedMirrorConfig> g_threadedMirrorConfigs;
std::mutex g_threadedMirrorConfigMutex;

// Game state for capture thread
std::atomic<int> g_captureGameW{ 0 };
std::atomic<int> g_captureGameH{ 0 };
std::atomic<GLuint> g_captureGameTexture{ UINT_MAX };

// Screen/viewport geometry for render cache computation
std::atomic<int> g_captureScreenW{ 0 };
std::atomic<int> g_captureScreenH{ 0 };
std::atomic<int> g_captureFinalX{ 0 };
std::atomic<int> g_captureFinalY{ 0 };
std::atomic<int> g_captureFinalW{ 0 };
std::atomic<int> g_captureFinalH{ 0 };

// Lock-free SPSC ring buffer for capture notifications
FrameCaptureNotification g_captureQueue[CAPTURE_QUEUE_SIZE];
std::atomic<int> g_captureQueueHead{ 0 };
std::atomic<int> g_captureQueueTail{ 0 };

// Double-buffered shared copy textures (render thread writes, capture thread reads)
// Using double-buffering to avoid reading while writing
static GLuint g_copyFBO = 0;                          // FBO for blitting
static GLuint g_copyTextures[2] = { 0, 0 };           // Double-buffered textures
static std::atomic<int> g_copyTextureWriteIndex{ 0 }; // Which texture render thread is writing to
static std::atomic<int> g_copyTextureReadIndex{ -1 }; // Which texture capture thread should read (-1 = none ready)
static int g_copyTextureW = 0, g_copyTextureH = 0;

// Track the last frame's copy fence for render_thread to wait on
// This is separate from the queue - render_thread needs synchronous access
static std::atomic<GLsync> g_lastCopyFence{ nullptr };
static std::atomic<int> g_lastCopyReadIndex{ -1 };
static std::atomic<int> g_lastCopyWidth{ 0 };
static std::atomic<int> g_lastCopyHeight{ 0 };

// These track the LAST FULLY COMPLETED frame - GPU fence has signaled, safe to read
// Updated by mirror thread after fence wait succeeds, read by OBS without waiting
// This ensures OBS always gets a complete frame, even if 1-2 frames behind
static std::atomic<int> g_readyFrameIndex{ -1 }; // Index of guaranteed-complete texture (-1 = none ready)
static std::atomic<int> g_readyFrameWidth{ 0 };  // Width of ready frame content
static std::atomic<int> g_readyFrameHeight{ 0 }; // Height of ready frame content

// Note: OBS capture is now handled by obs_thread.cpp via glBlitFramebuffer hook

// ============================================================================
// MIRROR THREAD LOCAL SHADER PROGRAMS
// These shaders are created on the mirror thread context (not shared with main thread)
// OpenGL shader programs are NOT shared between contexts via wglShareLists
// ============================================================================

// Vertex shader (shared by all fragment shaders)
static const char* mt_passthrough_vert_shader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
})";

// Filter shader - applies color filter to captured content (supports multiple target colors)
static const char* mt_filter_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D screenTexture;
uniform vec4 u_sourceRect;
uniform vec3 u_targetColors[8];  // Support up to 8 target colors
uniform int u_targetColorCount;  // Number of active target colors
uniform vec4 outputColor;
uniform float u_sensitivity;
void main() {
    vec2 srcCoord = u_sourceRect.xy + TexCoord * u_sourceRect.zw;
    vec3 screenColorSRGB = texture(screenTexture, srcCoord).rgb;
    vec3 screenColorLinear = pow(screenColorSRGB, vec3(2.2));
    
    bool matches = false;
    for (int i = 0; i < u_targetColorCount; i++) {
        vec3 targetColorLinear = pow(u_targetColors[i], vec3(2.2));
        if (distance(screenColorLinear, targetColorLinear) < u_sensitivity) {
            matches = true;
            break;
        }
    }
    
    if (matches) {
        FragColor = outputColor;
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
    }
})";

// Color Passthrough filter shader - outputs original pixel color when matching target colors
// Unlike the regular filter shader, this preserves the original pixel color instead of replacing it
static const char* mt_filter_passthrough_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D screenTexture;
uniform vec4 u_sourceRect;
uniform vec3 u_targetColors[8];  // Support up to 8 target colors
uniform int u_targetColorCount;  // Number of active target colors
uniform float u_sensitivity;
void main() {
    vec2 srcCoord = u_sourceRect.xy + TexCoord * u_sourceRect.zw;
    vec3 screenColorSRGB = texture(screenTexture, srcCoord).rgb;
    vec3 screenColorLinear = pow(screenColorSRGB, vec3(2.2));
    
    bool matches = false;
    for (int i = 0; i < u_targetColorCount; i++) {
        vec3 targetColorLinear = pow(u_targetColors[i], vec3(2.2));
        if (distance(screenColorLinear, targetColorLinear) < u_sensitivity) {
            matches = true;
            break;
        }
    }
    
    if (matches) {
        // Output the original pixel color (passthrough)
        FragColor = vec4(screenColorSRGB, 1.0);
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
    }
})";

// Passthrough shader - just copies texture without modification
static const char* mt_passthrough_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D screenTexture;
uniform vec4 u_sourceRect;
void main() {
    vec2 srcCoord = u_sourceRect.xy + TexCoord * u_sourceRect.zw;
    FragColor = texture(screenTexture, srcCoord);
})";

// Background shader - simple texture blit with opacity
static const char* mt_background_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D backgroundTexture;
uniform float u_opacity;
void main() {
    vec4 texColor = texture(backgroundTexture, TexCoord);
    FragColor = vec4(texColor.rgb, texColor.a * u_opacity);
})";

// Render shader - brute force border rendering
static const char* mt_render_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D filterTexture;
uniform int u_borderWidth;
uniform vec4 u_outputColor;
uniform vec4 u_borderColor;
uniform vec2 u_screenPixel;
void main() {
    if (texture(filterTexture, TexCoord).a > 0.5) {
        FragColor = u_outputColor;
        return;
    }
    float maxA = 0.0;
    for (int x = -u_borderWidth; x <= u_borderWidth; x++) {
        for (int y = -u_borderWidth; y <= u_borderWidth; y++) {
            if (x == 0 && y == 0) continue;
            vec2 offset = vec2(x, y) * u_screenPixel;
            maxA = max(maxA, texture(filterTexture, TexCoord + offset).a);
        }
    }
    if (maxA > 0.5) {
        FragColor = u_borderColor;
    } else {
        discard;
    }
})";

// Render shader for color passthrough - preserves original pixel color from filter texture
static const char* mt_render_passthrough_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D filterTexture;
uniform int u_borderWidth;
uniform vec4 u_borderColor;
uniform vec2 u_screenPixel;
void main() {
    vec4 texColor = texture(filterTexture, TexCoord);
    if (texColor.a > 0.5) {
        // Output original pixel color from filter texture
        FragColor = vec4(texColor.rgb, 1.0);
        return;
    }
    float maxA = 0.0;
    for (int x = -u_borderWidth; x <= u_borderWidth; x++) {
        for (int y = -u_borderWidth; y <= u_borderWidth; y++) {
            if (x == 0 && y == 0) continue;
            vec2 offset = vec2(x, y) * u_screenPixel;
            maxA = max(maxA, texture(filterTexture, TexCoord + offset).a);
        }
    }
    if (maxA > 0.5) {
        FragColor = u_borderColor;
    } else {
        discard;
    }
})";

// Static border shader - draws a border shape (rectangle or ellipse) on top of content
// Uses SDF (Signed Distance Field) for smooth shape rendering
static const char* mt_static_border_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform int u_shape;         // 0=Rectangle (with optional rounded corners), 1=Circle/Ellipse
uniform vec4 u_borderColor;
uniform float u_thickness;   // Border thickness in pixels
uniform float u_radius;      // Corner radius for Rectangle in pixels (0 = sharp corners)
uniform vec2 u_size;         // FBO size for aspect ratio correction

// SDF for a rounded rectangle (works for sharp corners when r=0)
float sdRoundedBox(vec2 p, vec2 b, float r) {
    // Clamp radius to not exceed half of the smaller box dimension
    float maxR = min(b.x, b.y);
    r = clamp(r, 0.0, maxR);
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// SDF for an ellipse (approximation)
float sdEllipse(vec2 p, vec2 ab) {
    vec2 pn = p / ab;
    float d = length(pn) - 1.0;
    return d * min(ab.x, ab.y);
}

void main() {
    // Map TexCoord (0-1) to centered coordinates (-1 to 1)
    vec2 uv = TexCoord * 2.0 - 1.0;
    
    // Adjust for aspect ratio - ensure minimum size to avoid division issues
    float aspectRatio = max(u_size.x, 1.0) / max(u_size.y, 1.0);
    vec2 aspectUV = uv;
    if (aspectRatio > 1.0) {
        aspectUV.x *= aspectRatio;
    } else {
        aspectUV.y /= aspectRatio;
    }
    
    // Normalize thickness to work with our coordinate space
    float minSize = max(min(u_size.x, u_size.y), 1.0);
    float borderThickness = u_thickness / minSize * 2.0;
    
    float dist;
    
    if (u_shape == 0) {
        // Rectangle (with optional rounded corners via u_radius)
        vec2 boxSize = vec2(1.0, 1.0);
        if (aspectRatio > 1.0) {
            boxSize.x = aspectRatio;
        } else {
            boxSize.y = 1.0 / aspectRatio;
        }
        float cornerRadius = u_radius / minSize * 2.0;
        dist = sdRoundedBox(aspectUV, boxSize, cornerRadius);
    } else {
        // Circle/Ellipse
        vec2 ellipseSize = vec2(1.0, 1.0);
        if (aspectRatio > 1.0) {
            ellipseSize.x = aspectRatio;
        } else {
            ellipseSize.y = 1.0 / aspectRatio;
        }
        dist = sdEllipse(aspectUV, ellipseSize);
    }
    
    // Border is drawn at the shape edge (dist=0) outward to thickness
    float innerEdge = 0.0;
    float outerEdge = borderThickness;
    
    // Add small epsilon for floating-point precision at boundaries
    float epsilon = 0.01;
    
    if (dist >= innerEdge - epsilon && dist <= outerEdge + epsilon) {
        FragColor = u_borderColor;
    } else {
        discard;
    }
})";

// Local shader program handles (created on mirror thread context)
static GLuint mt_filterProgram = 0;
static GLuint mt_filterPassthroughProgram = 0; // Color passthrough filter shader
static GLuint mt_passthroughProgram = 0;
static GLuint mt_backgroundProgram = 0;
static GLuint mt_renderProgram = 0;
static GLuint mt_renderPassthroughProgram = 0; // Color passthrough render shader
static GLuint mt_staticBorderProgram = 0;      // Static border shape shader

// Uniform locations for local shaders
struct MT_FilterShaderLocs {
    GLint screenTexture = -1, sourceRect = -1;
    GLint targetColors = -1;     // Array of target colors (up to 8)
    GLint targetColorCount = -1; // Number of active target colors
    GLint outputColor = -1, sensitivity = -1;
};
// Color passthrough filter shader uniform locations (no outputColor since it uses original pixel)
struct MT_FilterPassthroughShaderLocs {
    GLint screenTexture = -1, sourceRect = -1;
    GLint targetColors = -1;     // Array of target colors (up to 8)
    GLint targetColorCount = -1; // Number of active target colors
    GLint sensitivity = -1;
};
struct MT_PassthroughShaderLocs {
    GLint screenTexture = -1, sourceRect = -1;
};
struct MT_BackgroundShaderLocs {
    GLint backgroundTexture = -1, opacity = -1;
};
struct MT_RenderShaderLocs {
    GLint filterTexture = -1, borderWidth = -1, outputColor = -1, borderColor = -1, screenPixel = -1;
};
// Color passthrough render shader uniform locations (no outputColor since it uses original pixel)
struct MT_RenderPassthroughShaderLocs {
    GLint filterTexture = -1, borderWidth = -1, borderColor = -1, screenPixel = -1;
};
// Static border shader uniform locations
struct MT_StaticBorderShaderLocs {
    GLint shape = -1, borderColor = -1, thickness = -1, radius = -1, size = -1;
};

static MT_FilterShaderLocs mt_filterShaderLocs;
static MT_PassthroughShaderLocs mt_passthroughShaderLocs;
static MT_BackgroundShaderLocs mt_backgroundShaderLocs;
static MT_RenderShaderLocs mt_renderShaderLocs;
static MT_RenderPassthroughShaderLocs mt_renderPassthroughShaderLocs;
static MT_StaticBorderShaderLocs mt_staticBorderShaderLocs;

static MT_FilterPassthroughShaderLocs mt_filterPassthroughShaderLocs;

// Shader compilation helper
static GLuint MT_CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, 512, NULL, log);
        Log("Mirror Thread: Shader compile error: " + std::string(log));
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint MT_CreateShaderProgram(const char* vertSrc, const char* fragSrc) {
    GLuint vs = MT_CompileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint fs = MT_CompileShader(GL_FRAGMENT_SHADER, fragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, 512, NULL, log);
        Log("Mirror Thread: Shader link error: " + std::string(log));
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static bool MT_InitializeShaders() {
    LogCategory("init", "Mirror Thread: Initializing local shaders...");

    mt_filterProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_filter_frag_shader);
    mt_filterPassthroughProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_filter_passthrough_frag_shader);
    mt_passthroughProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_passthrough_frag_shader);
    mt_backgroundProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_background_frag_shader);
    mt_renderProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_render_frag_shader);
    mt_renderPassthroughProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_render_passthrough_frag_shader);
    mt_staticBorderProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_static_border_frag_shader);

    if (!mt_filterProgram || !mt_filterPassthroughProgram || !mt_passthroughProgram || !mt_backgroundProgram || !mt_renderProgram ||
        !mt_renderPassthroughProgram || !mt_staticBorderProgram) {
        Log("Mirror Thread: FATAL - Failed to create basic shader programs");
        return false;
    }

    // Get uniform locations for basic shaders
    mt_filterShaderLocs.screenTexture = glGetUniformLocation(mt_filterProgram, "screenTexture");
    mt_filterShaderLocs.sourceRect = glGetUniformLocation(mt_filterProgram, "u_sourceRect");
    mt_filterShaderLocs.targetColors = glGetUniformLocation(mt_filterProgram, "u_targetColors");
    mt_filterShaderLocs.targetColorCount = glGetUniformLocation(mt_filterProgram, "u_targetColorCount");
    mt_filterShaderLocs.outputColor = glGetUniformLocation(mt_filterProgram, "outputColor");
    mt_filterShaderLocs.sensitivity = glGetUniformLocation(mt_filterProgram, "u_sensitivity");

    // Get uniform locations for color passthrough filter shader
    mt_filterPassthroughShaderLocs.screenTexture = glGetUniformLocation(mt_filterPassthroughProgram, "screenTexture");
    mt_filterPassthroughShaderLocs.sourceRect = glGetUniformLocation(mt_filterPassthroughProgram, "u_sourceRect");
    mt_filterPassthroughShaderLocs.targetColors = glGetUniformLocation(mt_filterPassthroughProgram, "u_targetColors");
    mt_filterPassthroughShaderLocs.targetColorCount = glGetUniformLocation(mt_filterPassthroughProgram, "u_targetColorCount");
    mt_filterPassthroughShaderLocs.sensitivity = glGetUniformLocation(mt_filterPassthroughProgram, "u_sensitivity");

    mt_passthroughShaderLocs.screenTexture = glGetUniformLocation(mt_passthroughProgram, "screenTexture");
    mt_passthroughShaderLocs.sourceRect = glGetUniformLocation(mt_passthroughProgram, "u_sourceRect");

    mt_backgroundShaderLocs.backgroundTexture = glGetUniformLocation(mt_backgroundProgram, "backgroundTexture");
    mt_backgroundShaderLocs.opacity = glGetUniformLocation(mt_backgroundProgram, "u_opacity");

    mt_renderShaderLocs.filterTexture = glGetUniformLocation(mt_renderProgram, "filterTexture");
    mt_renderShaderLocs.borderWidth = glGetUniformLocation(mt_renderProgram, "u_borderWidth");
    mt_renderShaderLocs.outputColor = glGetUniformLocation(mt_renderProgram, "u_outputColor");
    mt_renderShaderLocs.borderColor = glGetUniformLocation(mt_renderProgram, "u_borderColor");
    mt_renderShaderLocs.screenPixel = glGetUniformLocation(mt_renderProgram, "u_screenPixel");

    // Get uniform locations for color passthrough render shader
    mt_renderPassthroughShaderLocs.filterTexture = glGetUniformLocation(mt_renderPassthroughProgram, "filterTexture");
    mt_renderPassthroughShaderLocs.borderWidth = glGetUniformLocation(mt_renderPassthroughProgram, "u_borderWidth");
    mt_renderPassthroughShaderLocs.borderColor = glGetUniformLocation(mt_renderPassthroughProgram, "u_borderColor");
    mt_renderPassthroughShaderLocs.screenPixel = glGetUniformLocation(mt_renderPassthroughProgram, "u_screenPixel");

    // Get uniform locations for static border shader
    mt_staticBorderShaderLocs.shape = glGetUniformLocation(mt_staticBorderProgram, "u_shape");
    mt_staticBorderShaderLocs.borderColor = glGetUniformLocation(mt_staticBorderProgram, "u_borderColor");
    mt_staticBorderShaderLocs.thickness = glGetUniformLocation(mt_staticBorderProgram, "u_thickness");
    mt_staticBorderShaderLocs.radius = glGetUniformLocation(mt_staticBorderProgram, "u_radius");
    mt_staticBorderShaderLocs.size = glGetUniformLocation(mt_staticBorderProgram, "u_size");

    // Set texture sampler uniforms once
    glUseProgram(mt_filterProgram);
    glUniform1i(mt_filterShaderLocs.screenTexture, 0);

    glUseProgram(mt_filterPassthroughProgram);
    glUniform1i(mt_filterPassthroughShaderLocs.screenTexture, 0);

    glUseProgram(mt_passthroughProgram);
    glUniform1i(mt_passthroughShaderLocs.screenTexture, 0);

    glUseProgram(mt_backgroundProgram);
    glUniform1i(mt_backgroundShaderLocs.backgroundTexture, 0);

    glUseProgram(mt_renderProgram);
    glUniform1i(mt_renderShaderLocs.filterTexture, 0);

    glUseProgram(mt_renderPassthroughProgram);
    glUniform1i(mt_renderPassthroughShaderLocs.filterTexture, 0);

    glUseProgram(0);

    LogCategory("init", "Mirror Thread: Local shaders initialized successfully");
    return true;
}

static void MT_CleanupShaders() {
    if (mt_filterProgram) {
        glDeleteProgram(mt_filterProgram);
        mt_filterProgram = 0;
    }
    if (mt_filterPassthroughProgram) {
        glDeleteProgram(mt_filterPassthroughProgram);
        mt_filterPassthroughProgram = 0;
    }
    if (mt_passthroughProgram) {
        glDeleteProgram(mt_passthroughProgram);
        mt_passthroughProgram = 0;
    }
    if (mt_backgroundProgram) {
        glDeleteProgram(mt_backgroundProgram);
        mt_backgroundProgram = 0;
    }
    if (mt_renderProgram) {
        glDeleteProgram(mt_renderProgram);
        mt_renderProgram = 0;
    }
    if (mt_renderPassthroughProgram) {
        glDeleteProgram(mt_renderPassthroughProgram);
        mt_renderPassthroughProgram = 0;
    }
    if (mt_staticBorderProgram) {
        glDeleteProgram(mt_staticBorderProgram);
        mt_staticBorderProgram = 0;
    }
}

// Get the most recent copy texture (for OBS/render_thread to use)
GLuint GetGameCopyTexture() {
    int readIndex = g_lastCopyReadIndex.load(std::memory_order_acquire);
    if (readIndex < 0 || readIndex >= 2) return 0;
    return g_copyTextures[readIndex];
}

// These return GUARANTEED COMPLETE frames - no fence wait needed
// Updated by mirror thread after fence signals, so OBS can read without waiting

GLuint GetReadyGameTexture() {
    int idx = g_readyFrameIndex.load(std::memory_order_acquire);
    if (idx < 0 || idx >= 2) return 0;
    return g_copyTextures[idx];
}

int GetReadyGameWidth() { return g_readyFrameWidth.load(std::memory_order_acquire); }

int GetReadyGameHeight() { return g_readyFrameHeight.load(std::memory_order_acquire); }

// Fallback accessors - return last copy texture info (requires fence wait before use)
GLuint GetFallbackGameTexture() {
    int idx = g_lastCopyReadIndex.load(std::memory_order_acquire);
    if (idx < 0 || idx >= 2) return 0;
    return g_copyTextures[idx];
}

int GetFallbackGameWidth() { return g_lastCopyWidth.load(std::memory_order_acquire); }

int GetFallbackGameHeight() { return g_lastCopyHeight.load(std::memory_order_acquire); }

GLsync GetFallbackCopyFence() { return g_lastCopyFence.load(std::memory_order_acquire); }

// Returns the texture that is NOT currently being written to (always safe to read)
// This is a guaranteed valid texture (may be 1 frame behind) - no fence wait needed
GLuint GetSafeReadTexture() {
    int writeIndex = g_copyTextureWriteIndex.load(std::memory_order_acquire);
    int readIndex = 1 - writeIndex; // The OTHER buffer is always safe to read
    if (g_copyTextures[readIndex] == 0) return 0;
    return g_copyTextures[readIndex];
}

void InitCaptureTexture(int width, int height) {
    // This MUST be called from the main render thread with GL context current
    // Creates an FBO and double-buffered textures for GPU-to-GPU copy

    g_copyTextureW = width;
    g_copyTextureH = height;

    // Create FBO
    glGenFramebuffers(1, &g_copyFBO);

    // Create double-buffered textures
    glGenTextures(2, g_copyTextures);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, g_copyTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    g_copyTextureWriteIndex.store(0);
    g_copyTextureReadIndex.store(-1);

    LogCategory("init", "InitCaptureTexture: Created FBO and " + std::to_string(2) + " textures of " + std::to_string(width) + "x" +
                            std::to_string(height));
}

void CleanupCaptureTexture() {
    // Cleanup capture resources - call from capture thread or main thread with GL context current
    // Drain the lock-free queue and delete any remaining fences
    FrameCaptureNotification notif;
    while (CaptureQueuePop(notif)) {
        if (notif.fence) { glDeleteSync(notif.fence); }
    }

    // Delete textures and FBO
    if (g_copyTextures[0] != 0 || g_copyTextures[1] != 0) {
        glDeleteTextures(2, g_copyTextures);
        g_copyTextures[0] = 0;
        g_copyTextures[1] = 0;
    }

    if (g_copyFBO != 0) {
        glDeleteFramebuffers(1, &g_copyFBO);
        g_copyFBO = 0;
    }

    Log("CleanupCaptureTexture: Cleaned up FBO and textures");
}

void SubmitFrameCapture(GLuint gameTexture, int width, int height) {
    // Called from SwapBuffers hook - does ASYNC GPU blit (non-blocking)
    // The GPU executes the blit in the background while SwapBuffers continues.
    // Consumers wait on the fence before reading the copy.

    if (g_copyFBO == 0) {
        // Not initialized yet
        return;
    }

    // CRITICAL: Preserve GL state - this runs on the game's GL context from SwapBuffers.
    // Leaking state here can break older MC versions (e.g. fog/sky rendering).
    GLint prevReadFBO = 0;
    GLint prevDrawFBO = 0;
    GLint prevFramebuffer = 0;
    GLint prevActiveTexture = 0;
    GLint prevTexture2D = 0;
    GLfloat prevClearColor[4] = { 0, 0, 0, 0 };
    GLboolean prevColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFBO);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFramebuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture2D);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClearColor);
    glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);

    auto restoreState = [&]() {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, prevFramebuffer);
        glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
        glClearColor(prevClearColor[0], prevClearColor[1], prevClearColor[2], prevClearColor[3]);
        glActiveTexture(prevActiveTexture);
        glBindTexture(GL_TEXTURE_2D, prevTexture2D);
    };

    // Resize copy textures to match game content EXACTLY
    // This ensures UV coordinates work correctly for mirrors (no need to scale UVs)
    // IMPORTANT: Only resize the WRITE texture, not the read texture that other threads may be using
    int writeIndex = g_copyTextureWriteIndex.load(std::memory_order_acquire);
    bool dimensionsChanged = (width != g_copyTextureW || height != g_copyTextureH);

    if (dimensionsChanged) {
        // Resize BOTH textures since dimensions have changed
        // Note: We do NOT invalidate g_lastCopyReadIndex here - it continues pointing to
        // the old texture until a successful blit completes. This prevents getting stuck
        // in an invalid state if blits fail due to race conditions.
        for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, g_copyTextures[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }
        glBindTexture(GL_TEXTURE_2D, 0);

        // CRITICAL: Wait for GPU to complete texture resize before proceeding
        // This prevents race conditions where we try to use the texture before resize is done
        glFinish();

        g_copyTextureW = width;
        g_copyTextureH = height;
        LogCategory("texture_ops", "SubmitFrameCapture: Resized copy textures to " + std::to_string(width) + "x" + std::to_string(height));
    }

    // Create temp FBO to read from game texture
    GLuint srcFBO = 0;
    glGenFramebuffers(1, &srcFBO);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFBO);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gameTexture, 0);

    // Check if source FBO is complete - game texture might be invalid during WM_SIZE resize
    GLenum srcStatus = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    if (srcStatus != GL_FRAMEBUFFER_COMPLETE) {
        // Game texture is in a bad state (probably being recreated due to WM_SIZE)
        // Skip this frame's capture - the next frame will have a valid texture
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &srcFBO);
        restoreState();
        return;
    }

    // Bind copy FBO as draw target with the write texture
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copyFBO);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_copyTextures[writeIndex], 0);

    // Check if destination FBO is complete
    GLenum dstStatus = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (dstStatus != GL_FRAMEBUFFER_COMPLETE) {
        // Our copy texture is in a bad state - skip this frame
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &srcFBO);
        restoreState();
        return;
    }

    // Async GPU-to-GPU blit - this is queued but executed by GPU in background
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // Fix alpha values (still async - GPU does this in pipeline)
    glBindFramebuffer(GL_FRAMEBUFFER, g_copyFBO);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Cleanup temp FBO
    glDeleteFramebuffers(1, &srcFBO);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    // Create TWO fences AFTER blit commands - marks when blit is complete
    // One for mirror thread (pushed to queue, mirror thread will delete it)
    // One for render thread fallback (stored separately, render thread manages it)
    GLsync fenceForMirrorThread = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    GLsync fenceForRenderThread = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    // CRITICAL: Flush to ensure commands are submitted and fence is visible to other contexts
    glFlush();

    // Swap write index for next frame (double buffering)
    int nextWriteIndex = 1 - writeIndex;
    g_copyTextureWriteIndex.store(nextWriteIndex, std::memory_order_release);

    // Update accessor variables for render_thread/OBS to use
    // Delete old fence before storing new one (render thread fence management)
    GLsync oldFence = g_lastCopyFence.exchange(fenceForRenderThread, std::memory_order_acq_rel);
    if (oldFence) { glDeleteSync(oldFence); }
    g_lastCopyReadIndex.store(writeIndex, std::memory_order_release);
    g_lastCopyWidth.store(width, std::memory_order_release);
    g_lastCopyHeight.store(height, std::memory_order_release);

    // Notify mirror thread (lock-free queue) - include texture index so mirror thread uses correct texture
    FrameCaptureNotification notif = { 0, fenceForMirrorThread, width, height, writeIndex };
    if (!CaptureQueuePush(notif)) {
        // Queue full - delete the fence since mirror thread won't get it
        glDeleteSync(fenceForMirrorThread);
    }

    restoreState();
}

// Pre-computes NDC positions and vertex data for a mirror
// Called after capture completes to populate the back buffer cache
static void ComputeMirrorRenderCache(MirrorInstance* inst, const ThreadedMirrorConfig& conf, int gameW, int gameH, int screenW, int screenH,
                                     int finalX, int finalY, int finalW, int finalH) {
    auto& cache = inst->cachedRenderStateBack;

    // Check if cache is still valid (inputs haven't changed)
    float scaleX = conf.outputSeparateScale ? conf.outputScaleX : conf.outputScale;
    float scaleY = conf.outputSeparateScale ? conf.outputScaleY : conf.outputScale;
    if (cache.isValid && cache.outputScale == conf.outputScale && cache.outputSeparateScale == conf.outputSeparateScale &&
        cache.outputScaleX == conf.outputScaleX && cache.outputScaleY == conf.outputScaleY && cache.outputX == conf.outputX &&
        cache.outputY == conf.outputY && cache.outputRelativeTo == conf.outputRelativeTo && cache.gameW == gameW && cache.gameH == gameH &&
        cache.screenW == screenW && cache.screenH == screenH && cache.finalX == finalX && cache.finalY == finalY &&
        cache.finalW == finalW && cache.finalH == finalH && cache.fbo_w == inst->fbo_w && cache.fbo_h == inst->fbo_h) {
        // Cache is still valid, no need to recompute
        return;
    }

    // Compute output dimensions
    int outW = static_cast<int>(inst->fbo_w * scaleX);
    int outH = static_cast<int>(inst->fbo_h * scaleY);

    // Calculate final screen position using CalculateFinalScreenPos
    // Create a temporary MirrorConfig-like structure for the call
    MirrorConfig tempConf;
    tempConf.output.scale = conf.outputScale;
    tempConf.output.separateScale = conf.outputSeparateScale;
    tempConf.output.scaleX = conf.outputScaleX;
    tempConf.output.scaleY = conf.outputScaleY;
    tempConf.output.x = conf.outputX;
    tempConf.output.y = conf.outputY;
    tempConf.output.relativeTo = conf.outputRelativeTo;

    int screenX, screenY;
    CalculateFinalScreenPos(&tempConf, *inst, gameW, gameH, finalX, finalY, finalW, finalH, screenW, screenH, screenX, screenY);

    // Convert to NDC coordinates
    float nx1 = (static_cast<float>(screenX) / screenW) * 2.0f - 1.0f;
    float ny2 = 1.0f - (static_cast<float>(screenY) / screenH) * 2.0f;
    float nx2 = (static_cast<float>(screenX + outW) / screenW) * 2.0f - 1.0f;
    float ny1 = 1.0f - (static_cast<float>(screenY + outH) / screenH) * 2.0f;

    // Pre-compute vertex buffer data (6 vertices: 2 triangles)
    // Format: x, y, u, v per vertex
    float vertices[] = { nx1, ny1, 0, 0, nx2, ny1, 1, 0, nx2, ny2, 1, 1, nx1, ny1, 0, 0, nx2, ny2, 1, 1, nx1, ny2, 0, 1 };
    memcpy(cache.vertices, vertices, sizeof(vertices));

    // Store computed values and inputs for invalidation check
    cache.outputScale = conf.outputScale;
    cache.outputSeparateScale = conf.outputSeparateScale;
    cache.outputScaleX = conf.outputScaleX;
    cache.outputScaleY = conf.outputScaleY;
    cache.outputX = conf.outputX;
    cache.outputY = conf.outputY;
    cache.outputRelativeTo = conf.outputRelativeTo;
    cache.gameW = gameW;
    cache.gameH = gameH;
    cache.screenW = screenW;
    cache.screenH = screenH;
    cache.finalX = finalX;
    cache.finalY = finalY;
    cache.finalW = finalW;
    cache.finalH = finalH;
    cache.fbo_w = inst->fbo_w;
    cache.fbo_h = inst->fbo_h;
    cache.outW = outW;
    cache.outH = outH;
    cache.mirrorScreenX = screenX;
    cache.mirrorScreenY = screenY;
    cache.mirrorScreenW = outW;
    cache.mirrorScreenH = outH;
    cache.isValid = true;
}

// Helper: Render a single mirror to its back buffer
// Returns true if rendering succeeded
static bool RenderMirrorToBackBuffer(MirrorInstance* inst, const ThreadedMirrorConfig& conf, GLuint validCopyTexture, GLuint captureVAO,
                                     GLuint captureVBO, int gameW, int gameH) {
    PROFILE_SCOPE_CAT("Capture Single Mirror", "Mirror Thread");

    // Capture to back buffer
    glBindFramebuffer(GL_FRAMEBUFFER, inst->fboBack);
    if (oglViewport)
        oglViewport(0, 0, inst->fbo_w, inst->fbo_h);
    else
        glViewport(0, 0, inst->fbo_w, inst->fbo_h);

    // Ensure proper GL state for texture capture
    // These settings ensure all RGBA channels are written and no depth/stencil interference
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Bind VALID COPIED texture (the last known good copy)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, validCopyTexture);

    // Read rawOutput state directly from instance
    bool useRawOutput = inst->desiredRawOutput.load(std::memory_order_acquire);
    bool useColorPassthrough = conf.colorPassthrough;

    // Use appropriate shader (local shader programs - not shared between GL contexts)
    if (useRawOutput) {
        glUseProgram(mt_passthroughProgram);
        glUniform1i(mt_passthroughShaderLocs.screenTexture, 0);
    } else if (useColorPassthrough) {
        // Color passthrough mode: output original pixel color when matching target colors
        glUseProgram(mt_filterPassthroughProgram);
        glUniform1i(mt_filterPassthroughShaderLocs.screenTexture, 0);

        // Pass multiple target colors to shader (max 8)
        int colorCount = (std::min)(static_cast<int>(conf.targetColors.size()), 8);
        glUniform1i(mt_filterPassthroughShaderLocs.targetColorCount, colorCount);

        // Set each target color in the array
        for (int i = 0; i < colorCount; i++) {
            glUniform3f(mt_filterPassthroughShaderLocs.targetColors + i, conf.targetColors[i].r, conf.targetColors[i].g,
                        conf.targetColors[i].b);
        }

        glUniform1f(mt_filterPassthroughShaderLocs.sensitivity, conf.colorSensitivity);
    } else {
        glUseProgram(mt_filterProgram);
        glUniform1i(mt_filterShaderLocs.screenTexture, 0);

        // Pass multiple target colors to shader (max 8)
        int colorCount = (std::min)(static_cast<int>(conf.targetColors.size()), 8);
        glUniform1i(mt_filterShaderLocs.targetColorCount, colorCount);

        // Set each target color in the array
        for (int i = 0; i < colorCount; i++) {
            glUniform3f(mt_filterShaderLocs.targetColors + i, conf.targetColors[i].r, conf.targetColors[i].g, conf.targetColors[i].b);
        }

        glUniform4f(mt_filterShaderLocs.outputColor, conf.outputColor.r, conf.outputColor.g, conf.outputColor.b, conf.outputColor.a);
        glUniform1f(mt_filterShaderLocs.sensitivity, conf.colorSensitivity);
    }

    glBindVertexArray(captureVAO);
    glBindBuffer(GL_ARRAY_BUFFER, captureVBO);

    // Clear FBO - alpha=1 since game texture now has alpha=1 from the alpha-fix pass
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (useRawOutput) {
        // Raw output: straight copy from game texture (which now has alpha=1)
        glDisable(GL_BLEND);
    } else {
        // Non-raw: additive blending for multiple input regions
        // Filter shader outputs its own alpha (1 where color matches, 0 elsewhere)
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
    }

    // Padding only applies for dynamic border (static border is rendered on top, no padding needed)
    int padding = (conf.borderType == MirrorBorderType::Dynamic) ? conf.dynamicBorderThickness : 0;
    if (oglViewport)
        oglViewport(padding, padding, conf.captureWidth, conf.captureHeight);
    else
        glViewport(padding, padding, conf.captureWidth, conf.captureHeight);

    for (const auto& r : conf.input) {
        int capX, capY;
        GetRelativeCoords(r.relativeTo, r.x, r.y, conf.captureWidth, conf.captureHeight, gameW, gameH, capX, capY);
        int capY_gl = gameH - capY - conf.captureHeight;
        float sx = static_cast<float>(capX) / gameW;
        float sy = static_cast<float>(capY_gl) / gameH;
        float sw = static_cast<float>(conf.captureWidth) / gameW;
        float sh = static_cast<float>(conf.captureHeight) / gameH;

        if (useRawOutput) {
            glUniform4f(mt_passthroughShaderLocs.sourceRect, sx, sy, sw, sh);
        } else if (useColorPassthrough) {
            glUniform4f(mt_filterPassthroughShaderLocs.sourceRect, sx, sy, sw, sh);
        } else {
            glUniform4f(mt_filterShaderLocs.sourceRect, sx, sy, sw, sh);
        }
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // Reset GL state after pass 1
    glDisable(GL_BLEND);

    // === Content Detection: Check if any pixels have non-zero alpha ===
    // This is used by static borders to avoid rendering when mirror has no matching pixels
    // Only needed for non-raw output (filter mode) - raw output always has content
    bool hasContent = useRawOutput; // Raw output always has content
    if (!useRawOutput) {
        // Read back pixels from the filter FBO to check for content
        // Sample the entire texture to detect any matching pixels
        int fboW = inst->fbo_w;
        int fboH = inst->fbo_h;
        std::vector<unsigned char> pixels(fboW * fboH * 4);
        glReadPixels(0, 0, fboW, fboH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        // Check if any pixel has non-zero alpha
        for (size_t i = 3; i < pixels.size(); i += 4) { // Check alpha channel (every 4th byte starting at index 3)
            if (pixels[i] > 0) {
                hasContent = true;
                break;
            }
        }
    }
    inst->hasFrameContentBack = hasContent;

    // === PASS 2: Apply border shader and render to finalFboBack ===
    // This produces screen-ready content so render thread just needs to blit
    if (inst->finalFboBack != 0 && inst->finalTextureBack != 0) {
        PROFILE_SCOPE_CAT("Apply Border Shader", "Mirror Thread");

        if (useRawOutput) {
            // Raw output: just passthrough, no borders
            glBindFramebuffer(GL_FRAMEBUFFER, inst->finalFboBack);
            if (oglViewport)
                oglViewport(0, 0, inst->final_w_back, inst->final_h_back);
            else
                glViewport(0, 0, inst->final_w_back, inst->final_h_back);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // Opaque for raw output
            glClear(GL_COLOR_BUFFER_BIT);

            glBindTexture(GL_TEXTURE_2D, inst->fboTextureBack);
            glUseProgram(mt_backgroundProgram);
            glUniform1i(mt_backgroundShaderLocs.backgroundTexture, 0);
            glUniform1f(mt_backgroundShaderLocs.opacity, 1.0f);

            static const float fullscreenVerts[] = { -1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, -1, 0, 0, 1, 1, 1, 1, -1, 1, 0, 1 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(fullscreenVerts), fullscreenVerts);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        } else if (conf.borderType == MirrorBorderType::Static) {
            // Static border mode: just passthrough the filter output (no dynamic border shader)
            // Static border will be rendered later in render_thread.cpp on top of the mirror
            glBindFramebuffer(GL_FRAMEBUFFER, inst->finalFboBack);
            if (oglViewport)
                oglViewport(0, 0, inst->final_w_back, inst->final_h_back);
            else
                glViewport(0, 0, inst->final_w_back, inst->final_h_back);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // Transparent
            glClear(GL_COLOR_BUFFER_BIT);

            glBindTexture(GL_TEXTURE_2D, inst->fboTextureBack);
            glUseProgram(mt_backgroundProgram);
            glUniform1i(mt_backgroundShaderLocs.backgroundTexture, 0);
            glUniform1f(mt_backgroundShaderLocs.opacity, 1.0f);

            static const float fullscreenVerts[] = { -1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, -1, 0, 0, 1, 1, 1, 1, -1, 1, 0, 1 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(fullscreenVerts), fullscreenVerts);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        } else {
            // Dynamic border mode: apply the border render shader
            glBindFramebuffer(GL_FRAMEBUFFER, inst->finalFboBack);
            if (oglViewport)
                oglViewport(0, 0, inst->final_w_back, inst->final_h_back);
            else
                glViewport(0, 0, inst->final_w_back, inst->final_h_back);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // Transparent for non-raw
            glClear(GL_COLOR_BUFFER_BIT);

            glBindTexture(GL_TEXTURE_2D, inst->fboTextureBack);
            if (useColorPassthrough) {
                // Use passthrough render shader - preserves original pixel color
                glUseProgram(mt_renderPassthroughProgram);
                glUniform1i(mt_renderPassthroughShaderLocs.borderWidth, conf.dynamicBorderThickness);
                glUniform4f(mt_renderPassthroughShaderLocs.borderColor, conf.borderColor.r, conf.borderColor.g, conf.borderColor.b,
                            conf.borderColor.a);
                glUniform2f(mt_renderPassthroughShaderLocs.screenPixel, 1.0f / inst->final_w_back, 1.0f / inst->final_h_back);
            } else {
                // Use normal render shader - replaces pixel color with outputColor
                glUseProgram(mt_renderProgram);
                glUniform1i(mt_renderShaderLocs.borderWidth, conf.dynamicBorderThickness);
                glUniform4f(mt_renderShaderLocs.outputColor, conf.outputColor.r, conf.outputColor.g, conf.outputColor.b,
                            conf.outputColor.a);
                glUniform4f(mt_renderShaderLocs.borderColor, conf.borderColor.r, conf.borderColor.g, conf.borderColor.b,
                            conf.borderColor.a);
                glUniform2f(mt_renderShaderLocs.screenPixel, 1.0f / inst->final_w_back, 1.0f / inst->final_h_back);
            }

            static const float fullscreenVerts[] = { -1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, -1, 0, 0, 1, 1, 1, 1, -1, 1, 0, 1 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(fullscreenVerts), fullscreenVerts);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        // NOTE: Static border is rendered in render_thread.cpp after mirror compositing
        // to allow the border to extend beyond the mirror bounds
    }

    return true;
}

static void MirrorCaptureThreadFunc(void* unused) {
    _set_se_translator(SEHTranslator);

    try {
        Log("Mirror Capture Thread: Starting thread loop...");

        // Context should already be created and shared by StartMirrorCaptureThread on main thread
        if (!g_mirrorCaptureDC || !g_mirrorCaptureContext) {
            Log("Mirror Capture Thread: Missing pre-created context or DC");
            g_mirrorCaptureRunning.store(false);
            return;
        }

        // Make context current on this thread
        if (!wglMakeCurrent(g_mirrorCaptureDC, g_mirrorCaptureContext)) {
            Log("Mirror Capture Thread: Failed to make context current (error " + std::to_string(GetLastError()) + ")");
            g_mirrorCaptureRunning.store(false);
            return;
        }

        // Initialize GLEW on this thread's context
        if (glewInit() != GLEW_OK) {
            Log("Mirror Capture Thread: GLEW init failed");
            wglMakeCurrent(NULL, NULL);
            g_mirrorCaptureRunning.store(false);
            return;
        }

        // Initialize local shader programs (shaders are NOT shared between GL contexts)
        if (!MT_InitializeShaders()) {
            Log("Mirror Capture Thread: Failed to initialize shaders");
            wglMakeCurrent(NULL, NULL);
            g_mirrorCaptureRunning.store(false);
            return;
        }

        Log("Mirror Capture Thread: Thread loop running");

        // Create local VAO/VBO for rendering
        GLuint captureVAO = 0, captureVBO = 0;
        glGenVertexArrays(1, &captureVAO);
        glGenBuffers(1, &captureVBO);
        glBindVertexArray(captureVAO);
        glBindBuffer(GL_ARRAY_BUFFER, captureVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        static const float verts[] = { -1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, -1, 0, 0, 1, 1, 1, 1, -1, 1, 0, 1 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

        // Texture to use for mirrors - directly read from shared copy texture (no upload needed!)
        // The render thread already blitted the game texture to g_copyTextures via GPU-to-GPU copy
        GLuint validTexture = 0;
        int validW = 0, validH = 0;
        bool hasValidTexture = false;

        while (!g_mirrorCaptureShouldStop.load()) {
            PROFILE_SCOPE_CAT("Mirror Capture Thread Frame", "Mirror Thread");

            auto now = std::chrono::steady_clock::now();

            // === PHASE 1: Check for new frame captures from render thread ===
            FrameCaptureNotification notif = {};
            bool hasNotification = false;
            {
                PROFILE_SCOPE_CAT("Check Queue", "Mirror Thread");
                // Lock-free pop from ring buffer
                hasNotification = CaptureQueuePop(notif);
            }

            if (hasNotification) {
                PROFILE_SCOPE_CAT("Process Frame Capture", "Mirror Thread");

                // Wait for the async blit to complete (fence created by SubmitFrameCapture)
                GLenum waitResult;
                {
                    PROFILE_SCOPE_CAT("Waiting for GPU Blit", "Mirror Thread");
                    // Loop on timeout to handle GPU load - keep waiting until complete
                    do {
                        waitResult = glClientWaitSync(notif.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL); // 1 sec timeout
                    } while (waitResult == GL_TIMEOUT_EXPIRED);
                    glDeleteSync(notif.fence);
                }

                if (waitResult == GL_WAIT_FAILED) {
                    Log("Mirror Capture Thread: Fence wait failed");
                } else {
                    // Memory barrier to ensure texture writes are visible
                    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);

                    // Blit is complete - copy texture is ready to use
                    // Use the texture index from the notification (fixes race condition where
                    // g_lastCopyReadIndex gets overwritten by a newer frame before we read it)
                    int readIndex = notif.textureIndex;
                    if (readIndex >= 0 && readIndex < 2) {
                        validTexture = g_copyTextures[readIndex];
                        validW = notif.width;
                        validH = notif.height;
                        hasValidTexture = true;

                        // === CRITICAL: Publish ready frame for OBS ===
                        // This must happen HERE, immediately after fence signals,
                        // NOT after mirror processing. This ensures OBS works even without mirrors.
                        g_readyFrameIndex.store(readIndex, std::memory_order_release);
                        g_readyFrameWidth.store(notif.width, std::memory_order_release);
                        g_readyFrameHeight.store(notif.height, std::memory_order_release);
                    }
                }
            }

            // Skip mirror processing if we don't have valid texture data yet
            if (!hasValidTexture) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            int gameW = validW;
            int gameH = validH;

            // === PHASE 2: Process mirrors using the valid texture ===
            std::vector<ThreadedMirrorConfig> configs;
            {
                PROFILE_SCOPE_CAT("Get Mirror Configs", "Mirror Thread");
                std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
                configs = g_threadedMirrorConfigs;
            }

            if (configs.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // Process each mirror using the copied texture
            bool didCapture = false;
            for (auto& conf : configs) {
                PROFILE_SCOPE_CAT("Process Mirror", "Mirror Thread");
                // Check FPS throttling for this mirror
                if (conf.fps > 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - conf.lastCaptureTime).count();
                    if (elapsed < (1000 / conf.fps)) { continue; }
                }

                // Get mirror instance (unique lock - capture thread writes to instance)
                MirrorInstance* inst = nullptr;
                {
                    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                    auto it = g_mirrorInstances.find(conf.name);
                    if (it == g_mirrorInstances.end()) continue;
                    inst = &it->second;

                    // === FBO RESIZE: Handle FBO resize in capture thread (moved from main thread) ===
                    int borderPadding = (conf.borderType == MirrorBorderType::Dynamic) ? conf.dynamicBorderThickness : 0;
                    int requiredFboW = conf.captureWidth + 2 * borderPadding;
                    int requiredFboH = conf.captureHeight + 2 * borderPadding;

                    if (inst->fbo_w != requiredFboW || inst->fbo_h != requiredFboH) {
                        // Resize both front and back buffers
                        inst->fbo_w = requiredFboW;
                        inst->fbo_h = requiredFboH;
                        inst->forceUpdateFrames = 3;

                        // Resize front texture - use NEAREST for sharp pixel-perfect scaling (front/back get swapped)
                        glBindTexture(GL_TEXTURE_2D, inst->fboTexture);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, inst->fbo_w, inst->fbo_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                        // Resize back texture
                        // Use GL_NEAREST for sharp pixel-perfect scaling
                        glBindTexture(GL_TEXTURE_2D, inst->fboTextureBack);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, inst->fbo_w, inst->fbo_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                        glBindTexture(GL_TEXTURE_2D, 0);
                    }

                    // === FINAL FBO RESIZE: Also resize the final (screen-ready) FBOs ===
                    // These are sized to match output dimensions (fbo_w * scaleX, fbo_h * scaleY)
                    float finalScaleX = conf.outputSeparateScale ? conf.outputScaleX : conf.outputScale;
                    float finalScaleY = conf.outputSeparateScale ? conf.outputScaleY : conf.outputScale;
                    int requiredFinalW = static_cast<int>(inst->fbo_w * finalScaleX);
                    int requiredFinalH = static_cast<int>(inst->fbo_h * finalScaleY);

                    if (inst->final_w_back != requiredFinalW || inst->final_h_back != requiredFinalH) {
                        // Only resize BACK buffer now - front buffer keeps old content to avoid flicker
                        // Front buffer dimensions are preserved, will be updated in SwapMirrorBuffers

                        // Resize back final texture only
                        glBindTexture(GL_TEXTURE_2D, inst->finalTextureBack);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, requiredFinalW, requiredFinalH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        glBindTexture(GL_TEXTURE_2D, 0);

                        // Track back buffer dimensions separately
                        inst->final_w_back = requiredFinalW;
                        inst->final_h_back = requiredFinalH;

                        // Invalidate back cache since dimensions changed (front cache stays valid until swap)
                        inst->cachedRenderStateBack.isValid = false;
                    }
                }

                // Validate instance
                if (!inst || !inst->fboBack || !inst->fboTextureBack) continue;

                // Skip if previous capture not yet consumed
                if (inst->captureReady.load(std::memory_order_acquire)) continue;

                // NOTE: desiredRawOutput is set directly by GUI (immediate) and UpdateMirrorCaptureConfigs (sync).
                // Do NOT overwrite it here from conf.rawOutput - that causes race condition where
                // stale config value overwrites the GUI's immediate update.

                // Render the mirror
                RenderMirrorToBackBuffer(inst, conf, validTexture, captureVAO, captureVBO, gameW, gameH);

                // Pre-compute render cache for the render thread
                // Read current screen geometry from atomics
                int screenW = g_captureScreenW.load(std::memory_order_acquire);
                int screenH = g_captureScreenH.load(std::memory_order_acquire);
                int finalX = g_captureFinalX.load(std::memory_order_acquire);
                int finalY = g_captureFinalY.load(std::memory_order_acquire);
                int finalW = g_captureFinalW.load(std::memory_order_acquire);
                int finalH = g_captureFinalH.load(std::memory_order_acquire);

                if (screenW > 0 && screenH > 0) {
                    ComputeMirrorRenderCache(inst, conf, gameW, gameH, screenW, screenH, finalX, finalY, finalW, finalH);
                }

                // Record how this capture was made
                inst->capturedAsRawOutputBack = inst->desiredRawOutput.load(std::memory_order_acquire);

                // Create GPU fence for cross-context synchronization
                // This fence will be swapped along with the texture and waited on by the render thread
                // before it reads from the texture. This ensures the GPU has finished rendering
                // even across different OpenGL contexts (which glFinish doesn't guarantee).
                if (inst->gpuFenceBack) { glDeleteSync(inst->gpuFenceBack); }
                inst->gpuFenceBack = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

                // CRITICAL: Use glFinish() to ensure all GPU work is complete before signaling
                // that the buffer is ready. This is more aggressive than glFlush() but guarantees
                // no race conditions with the render thread reading the texture.
                {
                    PROFILE_SCOPE_CAT("glFinish", "Mirror Thread");
                    glFinish();
                }

                // Signal that back buffer is ready
                inst->captureReady.store(true, std::memory_order_release);
                conf.lastCaptureTime = now;
                didCapture = true;
            }

            // Update only lastCaptureTime in global configs, NOT the full config
            // This preserves FPS and other settings that may have been updated by GUI during capture
            if (didCapture) {
                std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
                for (const auto& localConf : configs) {
                    for (auto& globalConf : g_threadedMirrorConfigs) {
                        if (globalConf.name == localConf.name) {
                            globalConf.lastCaptureTime = localConf.lastCaptureTime;
                            break;
                        }
                    }
                }
            }

            // Note: OBS capture is done synchronously in CaptureToObsFBO (dllmain.cpp)
            // because it needs to capture the complete rendered frame from the backbuffer
            // which includes animations and overlays applied by the game thread

            // Ensure work is submitted
            glFlush();

            {
                PROFILE_SCOPE_CAT("Sleeping", "Mirror Thread");
                // Sleep briefly to avoid spinning
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        // Cleanup local GPU resources
        // Note: validTexture is a shared texture (g_copyTextures), don't delete it here
        if (captureVAO) glDeleteVertexArrays(1, &captureVAO);
        if (captureVBO) glDeleteBuffers(1, &captureVBO);

        // Cleanup local shader programs (created on this thread's context)
        MT_CleanupShaders();

        // Cleanup shared capture textures (requires GL context current)
        CleanupCaptureTexture();

        wglMakeCurrent(NULL, NULL);
        if (g_mirrorCaptureContext) {
            // Only delete context if we created it (not if using pre-shared context)
            if (!g_mirrorContextIsShared) { wglDeleteContext(g_mirrorCaptureContext); }
            g_mirrorCaptureContext = NULL;
        }

        g_mirrorCaptureRunning.store(false);
        Log("Mirror Capture Thread: Stopped");
    } catch (const SE_Exception& e) {
        LogException("MirrorCaptureThreadFunc (SEH)", e.getCode(), e.getInfo());
        g_mirrorCaptureRunning.store(false);
    } catch (const std::exception& e) {
        LogException("MirrorCaptureThreadFunc", e);
        g_mirrorCaptureRunning.store(false);
    } catch (...) {
        Log("EXCEPTION in MirrorCaptureThreadFunc: Unknown exception");
        g_mirrorCaptureRunning.store(false);
    }
}

// Start the mirror capture thread (call from main thread after GPU init)
// MUST be called from main thread where game context is current
void StartMirrorCaptureThread(void* gameGLContext) {
    // If thread is already running, don't start another
    if (g_mirrorCaptureThread.joinable()) {
        if (g_mirrorCaptureRunning.load()) {
            // Thread object exists and is still running
            Log("Mirror Capture Thread: Already running");
            return;
        } else {
            // Thread object exists but finished - join it before starting new one
            Log("Mirror Capture Thread: Joining finished thread...");
            g_mirrorCaptureThread.join();
        }
    }

    // Check if pre-shared context is available (from InitializeSharedContexts)
    HGLRC sharedContext = GetSharedMirrorContext();
    HDC sharedDC = GetSharedContextDC();

    if (sharedContext && sharedDC) {
        // Use the pre-shared context (GPU sharing enabled for all threads)
        g_mirrorCaptureContext = sharedContext;
        g_mirrorCaptureDC = sharedDC;
        g_mirrorContextIsShared = true;
        Log("Mirror Capture Thread: Using pre-shared context (GPU texture sharing enabled)");
    } else {
        // Fallback: Create and share context now
        g_mirrorContextIsShared = false;

        // Get current DC for context creation
        HDC hdc = wglGetCurrentDC();
        if (!hdc) {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd) { hdc = GetDC(hwnd); }
        }

        if (!hdc) {
            Log("Mirror Capture Thread: No DC available");
            return;
        }

        // Store DC for thread use
        g_mirrorCaptureDC = hdc;

        // Create the capture context on main thread
        g_mirrorCaptureContext = wglCreateContext(hdc);
        if (!g_mirrorCaptureContext) {
            Log("Mirror Capture Thread: Failed to create GL context (error " + std::to_string(GetLastError()) + ")");
            return;
        }

        // Share OpenGL objects with game context - MUST happen on main thread while game context is current
        // wglShareLists(src, dst) - shares display lists and textures from src to dst
        // Try src=game, dst=capture first (game resources become available in capture context)
        if (!wglShareLists((HGLRC)gameGLContext, g_mirrorCaptureContext)) {
            DWORD err1 = GetLastError();
            // Try reverse order
            if (!wglShareLists(g_mirrorCaptureContext, (HGLRC)gameGLContext)) {
                DWORD err2 = GetLastError();
                Log("Mirror Capture Thread: wglShareLists failed (errors " + std::to_string(err1) + ", " + std::to_string(err2) + ")");
                wglDeleteContext(g_mirrorCaptureContext);
                g_mirrorCaptureContext = NULL;
                return;
            }
        }

        Log("Mirror Capture Thread: Context created and shared on main thread (fallback mode)");
    }

    // Initialize capture textures AFTER context is ready - this is critical!
    // OpenGL only shares objects created AFTER the share is established
    // Get screen dimensions for initial texture size
    int screenW = GetCachedScreenWidth();
    int screenH = GetCachedScreenHeight();
    if (g_copyTextures[0] == 0) { // Only init if not already initialized
        InitCaptureTexture(screenW, screenH);
    }

    g_mirrorCaptureShouldStop.store(false);
    g_mirrorCaptureRunning.store(true); // Mark as running BEFORE starting thread
    g_mirrorCaptureThread = std::thread(MirrorCaptureThreadFunc, gameGLContext);
    LogCategory("init", "Mirror Capture Thread: Started");
}

// Stop the mirror capture thread
void StopMirrorCaptureThread() {
    if (!g_mirrorCaptureRunning.load() && !g_mirrorCaptureThread.joinable()) { return; }

    Log("Mirror Capture Thread: Stopping...");
    g_mirrorCaptureShouldStop.store(true);

    if (g_mirrorCaptureThread.joinable()) { g_mirrorCaptureThread.join(); }

    Log("Mirror Capture Thread: Joined");
}

// Swap buffers for all mirrors that have new captures ready
// Call this from main render thread each frame
// Double buffering: Front (read) ↔ Back (writing)
// GPU fence synchronization ensures capture thread's work completes before render reads
void SwapMirrorBuffers() {
    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex); // Write lock - swapping buffers

    for (auto& [name, inst] : g_mirrorInstances) {
        if (inst.captureReady.load(std::memory_order_acquire)) {
            // Double buffer swap: Back ↔ Front
            // Front becomes the new Back for capture thread to write to
            // Back becomes the new Front for render thread to read from
            std::swap(inst.fbo, inst.fboBack);
            std::swap(inst.fboTexture, inst.fboTextureBack);
            std::swap(inst.capturedAsRawOutput, inst.capturedAsRawOutputBack);
            std::swap(inst.cachedRenderState, inst.cachedRenderStateBack);
            std::swap(inst.finalFbo, inst.finalFboBack);
            std::swap(inst.finalTexture, inst.finalTextureBack);
            std::swap(inst.final_w, inst.final_w_back); // Swap dimensions with textures
            std::swap(inst.final_h, inst.final_h_back);
            std::swap(inst.hasFrameContent, inst.hasFrameContentBack); // Swap content presence flag
            std::swap(inst.gpuFence, inst.gpuFenceBack);               // Swap fence with texture

            inst.hasValidContent = true; // Front now has renderable content

            // Clear captureReady so capture thread can write to back again
            inst.captureReady.store(false, std::memory_order_release);
            inst.lastUpdateTime = std::chrono::steady_clock::now();
        }
    }
}

// Update capture configs from main thread (call when active mirrors change)
void UpdateMirrorCaptureConfigs(const std::vector<MirrorConfig>& activeMirrors) {
    std::vector<ThreadedMirrorConfig> configs;
    configs.reserve(activeMirrors.size());

    // Get existing configs to preserve timing data (lastCaptureTime)
    std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);

    for (const auto& m : activeMirrors) {
        ThreadedMirrorConfig conf;
        conf.name = m.name;
        conf.captureWidth = m.captureWidth;
        conf.captureHeight = m.captureHeight;
        // Border configuration
        conf.borderType = m.border.type;
        conf.dynamicBorderThickness = m.border.dynamicThickness;
        conf.staticBorderShape = m.border.staticShape;
        conf.staticBorderColor = m.border.staticColor;
        conf.staticBorderThickness = m.border.staticThickness;
        conf.staticBorderRadius = m.border.staticRadius;
        conf.staticBorderOffsetX = m.border.staticOffsetX;
        conf.staticBorderOffsetY = m.border.staticOffsetY;
        conf.staticBorderWidth = m.border.staticWidth;
        conf.staticBorderHeight = m.border.staticHeight;
        conf.fps = m.fps;
        conf.rawOutput = m.rawOutput;
        conf.colorPassthrough = m.colorPassthrough;
        conf.targetColors = m.colors.targetColors; // Copy vector of target colors
        conf.outputColor = m.colors.output;
        conf.borderColor = m.colors.border;
        conf.colorSensitivity = m.colorSensitivity;
        conf.input = m.input;
        // Output positioning config for render cache computation
        conf.outputScale = m.output.scale;
        conf.outputSeparateScale = m.output.separateScale;
        conf.outputScaleX = m.output.scaleX;
        conf.outputScaleY = m.output.scaleY;
        conf.outputX = m.output.x;
        conf.outputY = m.output.y;
        conf.outputRelativeTo = m.output.relativeTo;

        // Preserve lastCaptureTime from existing config to maintain FPS throttling
        for (const auto& existingConf : g_threadedMirrorConfigs) {
            if (existingConf.name == m.name) {
                conf.lastCaptureTime = existingConf.lastCaptureTime;
                break;
            }
        }

        configs.push_back(conf);
    }

    // Clear captureReady for all mirrors to allow capture thread to start fresh
    // This prevents freeze when switching to a mode without a mirror, then back
    // (captureReady would stay true if main thread never consumed the capture)
    // Also invalidate cached render state to force recompute with new output positions
    // (needed for group output positions to take effect on startup)
    {
        std::unique_lock<std::shared_mutex> clearLock(g_mirrorInstancesMutex);
        for (auto& [name, inst] : g_mirrorInstances) {
            inst.captureReady.store(false, std::memory_order_release);
            inst.cachedRenderState.isValid = false;
            inst.cachedRenderStateBack.isValid = false;
        }
    }

    g_threadedMirrorConfigs = configs;
}

void UpdateMirrorFPS(const std::string& mirrorName, int fps) {
    std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
    for (auto& conf : g_threadedMirrorConfigs) {
        if (conf.name == mirrorName) {
            conf.fps = fps;
            break;
        }
    }
}

void UpdateMirrorOutputPosition(const std::string& mirrorName, int x, int y, float scale, bool separateScale, float scaleX, float scaleY,
                                const std::string& relativeTo) {
    // Update the threaded config
    {
        std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
        for (auto& conf : g_threadedMirrorConfigs) {
            if (conf.name == mirrorName) {
                conf.outputX = x;
                conf.outputY = y;
                conf.outputScale = scale;
                conf.outputSeparateScale = separateScale;
                conf.outputScaleX = scaleX;
                conf.outputScaleY = scaleY;
                conf.outputRelativeTo = relativeTo;
                break;
            }
        }
    }

    // Invalidate cached render state in mirror instance
    // This ensures the render thread recalculates positions immediately
    {
        std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
        auto it = g_mirrorInstances.find(mirrorName);
        if (it != g_mirrorInstances.end()) {
            // Invalidate BOTH front and back caches
            // Front cache: render thread will recalculate immediately
            // Back cache: capture thread will recompute on next capture
            it->second.cachedRenderState.isValid = false;
            it->second.cachedRenderStateBack.isValid = false;
        }
    }
}

void UpdateMirrorGroupOutputPosition(const std::vector<std::string>& mirrorIds, int x, int y, float scale, bool separateScale, float scaleX,
                                     float scaleY, const std::string& relativeTo) {
    // Update the threaded config for all mirrors in the group
    // NOTE: We intentionally do NOT update scale here. The mirror thread should always use
    // the mirror's own scale for FBO sizing. Group scale is applied at render time only.
    // This prevents group scale from affecting all instances of a mirror (including ones
    // used outside the group).
    {
        std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
        for (auto& conf : g_threadedMirrorConfigs) {
            // Check if this mirror is in the group
            if (std::find(mirrorIds.begin(), mirrorIds.end(), conf.name) != mirrorIds.end()) {
                conf.outputX = x;
                conf.outputY = y;
                // Scale is NOT updated here - only position and relativeTo
                conf.outputRelativeTo = relativeTo;
            }
        }
    }

    // Invalidate cached render state for all mirrors in the group
    {
        std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
        for (const auto& mirrorName : mirrorIds) {
            auto it = g_mirrorInstances.find(mirrorName);
            if (it != g_mirrorInstances.end()) {
                it->second.cachedRenderState.isValid = false;
                it->second.cachedRenderStateBack.isValid = false;
            }
        }
    }
}

void UpdateMirrorInputRegions(const std::string& mirrorName, const std::vector<MirrorCaptureConfig>& inputRegions) {
    std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
    for (auto& conf : g_threadedMirrorConfigs) {
        if (conf.name == mirrorName) {
            conf.input = inputRegions;
            break;
        }
    }
}

void UpdateMirrorCaptureSettings(const std::string& mirrorName, int captureWidth, int captureHeight, const MirrorBorderConfig& border,
                                 const MirrorColors& colors, float colorSensitivity, bool rawOutput, bool colorPassthrough) {
    std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
    for (auto& conf : g_threadedMirrorConfigs) {
        if (conf.name == mirrorName) {
            conf.captureWidth = captureWidth;
            conf.captureHeight = captureHeight;

            // Border configuration
            conf.borderType = border.type;
            conf.dynamicBorderThickness = border.dynamicThickness;
            conf.staticBorderShape = border.staticShape;
            conf.staticBorderColor = border.staticColor;
            conf.staticBorderThickness = border.staticThickness;
            conf.staticBorderRadius = border.staticRadius;
            conf.staticBorderOffsetX = border.staticOffsetX;
            conf.staticBorderOffsetY = border.staticOffsetY;
            conf.staticBorderWidth = border.staticWidth;
            conf.staticBorderHeight = border.staticHeight;

            conf.targetColors = colors.targetColors; // Copy vector of target colors
            conf.outputColor = colors.output;
            conf.borderColor = colors.border;
            conf.colorSensitivity = colorSensitivity;
            conf.rawOutput = rawOutput;
            conf.colorPassthrough = colorPassthrough;
            break;
        }
    }
}
