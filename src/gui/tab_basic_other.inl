if (ImGui::BeginTabItem("[O] Other")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    // --- GUI HOTKEY SECTION ---
    ImGui::SeparatorText("[G] GUI");
    ImGui::PushID("basic_gui_hotkey");
    std::string guiKeyStr = GetKeyComboString(g_config.guiHotkey);

    ImGui::Text("Toggle:");
    ImGui::SameLine();

    bool isBindingGui = (s_mainHotkeyToBind == -999);
    const char* guiButtonLabel = isBindingGui ? "[Press Keys...]" : (guiKeyStr.empty() ? "[Click to Bind]" : guiKeyStr.c_str());
    if (ImGui::Button(guiButtonLabel, ImVec2(150, 0))) {
        s_mainHotkeyToBind = -999; // Special ID for GUI hotkey
        s_altHotkeyToBind = { -1, -1 };
        s_exclusionToBind = { -1, -1 };
    }
    ImGui::PopID();

    // --- DISPLAY SETTINGS ---
    ImGui::SeparatorText("[D] Display");

    ImGui::Text("FPS Limit:");
    ImGui::SetNextItemWidth(300);
    int fpsLimitValue = (g_config.fpsLimit == 0) ? 1001 : g_config.fpsLimit;
    if (ImGui::SliderInt("##FpsLimit", &fpsLimitValue, 30, 1001, fpsLimitValue == 1001 ? "Unlimited" : "%d fps")) {
        g_config.fpsLimit = (fpsLimitValue == 1001) ? 0 : fpsLimitValue;
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Limits the game's maximum frame rate.\n"
               "Lower FPS can reduce GPU load and power consumption.");

    if (ImGui::Checkbox("Hide In-Game Animations", &g_config.hideAnimationsInGame)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("When enabled, mode transitions appear instant on your screen,\n"
               "but OBS Game Capture will show the animations.");

/*    if (ImGui::Checkbox("Disable Fullscreen Prompt", &g_config.disableFullscreenPrompt)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("Disables the fullscreen toast prompt (toast2).\n"
               "When disabled, toast2 appears in fullscreen and fades out after 3 seconds.");

    if (ImGui::Checkbox("Disable Configure Prompt", &g_config.disableConfigurePrompt)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("Disables the configure toast prompt (toast1) shown in windowed mode.");*/

    // --- FONT SETTINGS ---
    ImGui::SeparatorText("[F] Font");

    ImGui::Text("Path:");
    ImGui::SetNextItemWidth(300);
    if (ImGui::InputText("##FontPath", &g_config.fontPath)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("Path to a .ttf font file for the GUI. Restart required for changes to take effect.");

    ImGui::EndTabItem();
}
