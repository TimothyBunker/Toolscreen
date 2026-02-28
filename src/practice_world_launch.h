#pragma once

#include <Windows.h>
#include <string>

// Custom window message used to request direct world launch from the game thread.
UINT GetPracticeWorldLaunchMessageId();

// Queue a world launch request by save/world folder name.
bool QueuePracticeWorldLaunchRequest(const std::string& worldName, std::string* outError = nullptr);

// Handles the custom world launch message. Returns true when the message was handled.
bool TryHandlePracticeWorldLaunchWindowMessage(HWND hWnd, UINT uMsg, LRESULT& outResult);

