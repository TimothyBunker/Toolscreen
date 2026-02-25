#pragma once

// Notes overlay runtime controls.
// - Toggle hotkey is handled from input_hook.
// - Rendering is invoked from render_thread when visible (or when pending autosave work exists).

bool HandleNotesOverlayToggleHotkey(unsigned int keyVk, bool ctrlDown, bool shiftDown, bool altDown);
bool IsNotesOverlayVisible();
bool IsNotesOverlayInputCaptureActive();
bool HasNotesOverlayPendingWork();
void RenderNotesOverlayImGui();
