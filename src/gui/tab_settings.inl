if (ImGui::BeginTabItem("Settings")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    ImGui::SeparatorText("Performance");

    ImGui::Text("FPS Limit:");
    ImGui::SetNextItemWidth(600);
    int fpsLimitValue = (g_config.fpsLimit == 0) ? 1001 : g_config.fpsLimit;
    // Track when slider is being actively dragged
    bool sliderActive = ImGui::IsItemActive() || ImGui::IsItemHovered();
    if (ImGui::SliderInt("##fpsLimit", &fpsLimitValue, 30, 1001, fpsLimitValue == 1001 ? "Unlimited" : "%d fps")) {
        // Set to 0 (unlimited) if slider is at max, otherwise use the slider value
        g_config.fpsLimit = (fpsLimitValue == 1001) ? 0 : fpsLimitValue;
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Limits the game's maximum frame rate.\n"
               "Not quite as precise as NVIDIA Control Panel but close enough.\n"
               "Lower FPS can reduce GPU load and power consumption.");

    ImGui::Spacing();
    ImGui::SeparatorText("Capture/Streaming");
    if (ImGui::Checkbox("Hide animations in game", &g_config.hideAnimationsInGame)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("When enabled, mode transitions appear instant on your screen,\n"
               "but OBS Game Capture will show the animations.");

    if (ImGui::Checkbox("Disable Fullscreen Prompt", &g_config.disableFullscreenPrompt)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("Disables the fullscreen toast prompt (toast2).\n"
               "When disabled, toast2 appears in fullscreen and fades out after 3 seconds.");

    if (ImGui::Checkbox("Disable Configure Prompt", &g_config.disableConfigurePrompt)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("Disables the configure toast prompt (toast1) shown in windowed mode.");

    ImGui::Spacing();
    ImGui::SeparatorText("Mirrors");
    {
        const char* gammaModes[] = { "Auto", "Assume sRGB", "Assume Linear" };
        int gm = static_cast<int>(g_config.mirrorGammaMode);
        ImGui::SetNextItemWidth(250);
        if (ImGui::Combo("Match Colorspace", &gm, gammaModes, IM_ARRAYSIZE(gammaModes))) {
            g_config.mirrorGammaMode = static_cast<MirrorGammaMode>(gm);
            g_configIsDirty = true;

            // Apply immediately for runtime (does not require saving)
            SetGlobalMirrorGammaMode(g_config.mirrorGammaMode);

            // Force mirrors to recapture with the new mode
            std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
            for (auto& kv : g_mirrorInstances) {
                kv.second.forceUpdateFrames = 3;
                kv.second.hasValidContent = false;
            }
        }
        ImGui::SameLine();
        HelpMarker("Controls how mirror color matching interprets the captured pixels.\n"
                   "Applies globally to all mirrors.\n\n"
                   "Auto: tries both sRGB-space and linear-space matching and uses the better match.\n"
                   "Assume sRGB: converts sampled pixels + target colors to linear for matching.\n"
                   "Assume Linear: treats sampled pixels as linear; converts only target colors.");
    }

    // Virtual Camera
    bool driverInstalled = IsVirtualCameraDriverInstalled();
    bool inUseByOBS = driverInstalled && IsVirtualCameraInUseByOBS();
    ImGui::BeginDisabled(!driverInstalled || inUseByOBS);
    bool vcEnabled = g_config.debug.virtualCameraEnabled;
    if (ImGui::Checkbox("Enable Virtual Camera", &vcEnabled)) {
        g_config.debug.virtualCameraEnabled = vcEnabled;
        g_configIsDirty = true;
        // Start or stop based on new state
        if (vcEnabled) {
            // Get current screen dimensions for initial startup
            int screenW = GetSystemMetrics(SM_CXSCREEN);
            int screenH = GetSystemMetrics(SM_CYSCREEN);
            StartVirtualCamera(screenW, screenH, g_config.debug.virtualCameraFps);
        } else {
            StopVirtualCamera();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!driverInstalled) {
        ImGui::TextDisabled("(OBS Virtual Camera not installed)");
    } else if (inUseByOBS) {
        ImGui::TextDisabled("(In use by OBS)");
    } else {
        HelpMarker("Outputs the game, including overlays, to the OBS Virtual Camera.\n"
                   "You can use this to screenshare in Discord, or to capture in OBS.\n\n"
                   "Requires OBS Virtual Camera driver to be installed.\n"
                   "Works independently of OBS being open.");
    }

    // Virtual Camera performance settings (restart camera to apply)
    ImGui::BeginDisabled(!driverInstalled || inUseByOBS || !vcEnabled);
    ImGui::Indent();
    if (ImGui::SliderInt("Camera FPS", &g_config.debug.virtualCameraFps, 15, 120, "%d fps")) { g_configIsDirty = true; }
    ImGui::Unindent();
    ImGui::EndDisabled();

    ImGui::Spacing();

    // Debug Options - Protected by passcode
    static bool s_debugUnlocked = false;
    static char s_passcodeInput[16] = "";

    if (!s_debugUnlocked) {
        if (ImGui::Button("Debug")) {
            ImGui::OpenPopup("Debug Passcode");
            memset(s_passcodeInput, 0, sizeof(s_passcodeInput));
        }

        if (ImGui::BeginPopupModal("Debug Passcode", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter passcode to access debug options:");
            ImGui::Spacing();

            ImGui::SetNextItemWidth(150);
            bool enterPressed = ImGui::InputText("##passcode", s_passcodeInput, sizeof(s_passcodeInput),
                                                 ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);

            ImGui::Spacing();

            if (ImGui::Button("OK", ImVec2(80, 0)) || enterPressed) {
                if (strcmp(s_passcodeInput, "5739") == 0) {
                    s_debugUnlocked = true;
                    ImGui::CloseCurrentPopup();
                } else {
                    memset(s_passcodeInput, 0, sizeof(s_passcodeInput));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 0))) { ImGui::CloseCurrentPopup(); }

            ImGui::EndPopup();
        }
    } else {
        ImGui::SeparatorText("Debug Options");
        if (ImGui::Checkbox("Delay Rendering Until Finished", &g_config.debug.delayRenderingUntilFinished)) { g_configIsDirty = true; }
        ImGui::SameLine();
        HelpMarker("Calls glFinish() before SwapBuffers to ensure all GPU rendering\n"
                   "is complete before presenting the frame.\n\n"
                   "May help with screen tearing or capture timing issues,\n"
                   "but can reduce performance.");
        if (ImGui::Checkbox("Delay Rendering Until Blitted", &g_config.debug.delayRenderingUntilBlitted)) { g_configIsDirty = true; }
        ImGui::SameLine();
        HelpMarker("Waits for the async overlay blit fence to complete before SwapBuffers.\n\n"
                   "This is a lighter-weight alternative to 'Delay Rendering Until Finished'\n"
                   "that only waits for the overlay blit operation specifically.\n\n"
                   "May help with capture timing issues while having less performance impact.");
        ImGui::Spacing();
        if (ImGui::Checkbox("Show Performance Overlay", &g_config.debug.showPerformanceOverlay)) { g_configIsDirty = true; }
        if (ImGui::Checkbox("Show Profiler", &g_config.debug.showProfiler)) { g_configIsDirty = true; }
        ImGui::SetNextItemWidth(300);
        if (ImGui::SliderFloat("Profiler Scale", &g_config.debug.profilerScale, 0.25f, 2.0f, "%.2f")) { g_configIsDirty = true; }
        ImGui::SameLine();
        HelpMarker("Scale of the profiler overlay\n25% = tiny, 50% = half size, 100% = normal, 200% = double size");
        if (ImGui::Checkbox("Show Hotkey Debug", &g_config.debug.showHotkeyDebug)) { g_configIsDirty = true; }
        if (ImGui::Checkbox("Fake Cursor Overlay", &g_config.debug.fakeCursor)) { g_configIsDirty = true; }
        ImGui::SameLine();
        HelpMarker("Renders a fake cursor overlay at the current mouse position, \n"
                   "for when OBS Window Captures make your cursor invisible. ");
        if (ImGui::Checkbox("Show Texture Grid (BUGGY)", &g_config.debug.showTextureGrid)) { g_configIsDirty = true; }
        ImGui::SameLine();
        HelpMarker("Displays a grid of all game OpenGL textures on screen for debugging.");

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Advanced Logging")) {
            ImGui::Indent();
            ImGui::TextDisabled("Enable verbose logging for specific categories:");
            ImGui::Spacing();
            if (ImGui::Checkbox("Mode Switch", &g_config.debug.logModeSwitch)) { g_configIsDirty = true; }
            if (ImGui::Checkbox("Animation", &g_config.debug.logAnimation)) { g_configIsDirty = true; }
            if (ImGui::Checkbox("Hotkey", &g_config.debug.logHotkey)) { g_configIsDirty = true; }
            if (ImGui::Checkbox("OBS", &g_config.debug.logObs)) { g_configIsDirty = true; }
            if (ImGui::Checkbox("Window Overlay", &g_config.debug.logWindowOverlay)) { g_configIsDirty = true; }
            if (ImGui::Checkbox("File Monitor", &g_config.debug.logFileMonitor)) { g_configIsDirty = true; }
            if (ImGui::Checkbox("Image Monitor", &g_config.debug.logImageMonitor)) { g_configIsDirty = true; }
            if (ImGui::Checkbox("Performance", &g_config.debug.logPerformance)) { g_configIsDirty = true; }
            if (ImGui::Checkbox("Texture Ops", &g_config.debug.logTextureOps)) { g_configIsDirty = true; }
            if (ImGui::Checkbox("GUI", &g_config.debug.logGui)) { g_configIsDirty = true; }
            if (ImGui::Checkbox("Initialization", &g_config.debug.logInit)) { g_configIsDirty = true; }
            if (ImGui::Checkbox("Cursor Textures", &g_config.debug.logCursorTextures)) { g_configIsDirty = true; }
            ImGui::Unindent();
        }
    }
    ImGui::EndTabItem();
}
