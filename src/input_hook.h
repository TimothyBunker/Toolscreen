#pragma once

#include <Windows.h>
#include <string>

// Forward declaration
extern WNDPROC g_originalWndProc;

// Handler result indicating whether the message was consumed
struct InputHandlerResult {
    bool consumed;  // If true, message was handled and should not be passed to game
    LRESULT result; // Return value if consumed
};

// Individual message handlers - each returns whether it consumed the message
// All handlers include profiling for performance monitoring

// Handle WM_MOUSEMOVE coordinate translation for viewport offset
InputHandlerResult HandleMouseMoveViewportOffset(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM& lParam);

// Check if we should early-exit during shutdown
InputHandlerResult HandleShutdownCheck(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Validate this is our subclassed window
InputHandlerResult HandleWindowValidation(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Handle non-fullscreen mode (pass through to original)
InputHandlerResult HandleNonFullscreenCheck(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Handle WM_CHAR logging
void HandleCharLogging(UINT uMsg, WPARAM wParam, LPARAM lParam);

// Handle WM_WINDOWPOSCHANGED for resize management
InputHandlerResult HandleWindowPosChanged(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Handle Alt+F4 passthrough
InputHandlerResult HandleAltF4(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Handle config load failure state
InputHandlerResult HandleConfigLoadFailure(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Handle WM_SETCURSOR message
InputHandlerResult HandleSetCursor(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& gameState);

// Handle WM_DESTROY message
InputHandlerResult HandleDestroy(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Handle ImGui input when GUI is open
InputHandlerResult HandleImGuiInput(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Handle GUI toggle hotkey
InputHandlerResult HandleGuiToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Handle keyboard input for focused overlay
InputHandlerResult HandleWindowOverlayKeyboard(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Handle mouse input for window overlay interaction
InputHandlerResult HandleWindowOverlayMouse(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Block all input when GUI is open
InputHandlerResult HandleGuiInputBlocking(UINT uMsg);

// Handle WM_ACTIVATE for game focus changes
InputHandlerResult HandleActivate(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& currentModeId);

// Handle hotkey processing
InputHandlerResult HandleHotkeys(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& currentModeId,
                                 const std::string& gameState);

// Handle mouse coordinate translation
InputHandlerResult HandleMouseCoordinateTranslation(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Handle key rebinding for WM_KEYDOWN/WM_KEYUP
InputHandlerResult HandleKeyRebinding(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Handle WM_CHAR key rebinding
InputHandlerResult HandleCharRebinding(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Runtime macro engine status (Ctrl+Shift+M toggle).
bool AreMacrosRuntimeEnabled();

// The refactored SubclassedWndProc that delegates to the handlers above
LRESULT CALLBACK SubclassedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
