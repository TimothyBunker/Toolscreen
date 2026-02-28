if (ImGui::BeginTabItem("[R] MCSR")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    auto HoverHelp = [](const char* desc) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) { ImGui::SetTooltip("%s", desc); }
    };

    if (ImGui::Checkbox("[ON] Tracker", &g_config.mcsrTrackerOverlay.enabled)) { g_configIsDirty = true; }
    HoverHelp("Enable/disable the MCSR API tracker overlay.");

    ImGui::BeginDisabled(!g_config.mcsrTrackerOverlay.enabled);

    bool startHidden = !g_config.mcsrTrackerOverlay.visible;
    if (ImGui::Checkbox("[Start] Hidden", &startHidden)) {
        g_config.mcsrTrackerOverlay.visible = !startHidden;
        g_configIsDirty = true;
    }
    HoverHelp("Start tracker hidden. Use the tracker hotkey to show/hide.");

    if (ImGui::Checkbox("[HUD] Show", &g_config.mcsrTrackerOverlay.renderInGameOverlay)) { g_configIsDirty = true; }
    HoverHelp("Render tracker card on the game overlay.");

    if (ImGui::Checkbox("[Auto] Username", &g_config.mcsrTrackerOverlay.autoDetectPlayer)) { g_configIsDirty = true; }
    HoverHelp("Auto-grab Minecraft account username from latest.log.");

    if (ImGui::InputTextWithHint("[User] Search", "MCSR username", &g_config.mcsrTrackerOverlay.player)) { g_configIsDirty = true; }
    HoverHelp("Manual lookup username. Leave empty to use auto-detected account.");
    ImGui::SameLine();
    if (ImGui::Button("Clear##McsrUser")) {
        g_config.mcsrTrackerOverlay.player.clear();
        g_configIsDirty = true;
    }
    HoverHelp("Clear manual search and return to auto-detected user.");

    if (ImGui::Button("[API] Refresh")) { RequestMcsrApiTrackerRefresh(); }
    HoverHelp("Trigger an immediate MCSR API refresh.");

    if (ImGui::Checkbox("[API] Refresh-Only", &g_config.mcsrTrackerOverlay.refreshOnlyMode)) { g_configIsDirty = true; }
    HoverHelp("Only refresh on manual [API] Refresh or when tracked identity changes.");

    if (ImGui::Checkbox("[API] Key", &g_config.mcsrTrackerOverlay.useApiKey)) { g_configIsDirty = true; }
    HoverHelp("Use API key header for expanded MCSR API ratelimit.");
    ImGui::BeginDisabled(!g_config.mcsrTrackerOverlay.useApiKey);
    if (ImGui::InputTextWithHint("[API] Header", "x-api-key", &g_config.mcsrTrackerOverlay.apiKeyHeader)) { g_configIsDirty = true; }
    HoverHelp("Header name provided by MCSR (default: x-api-key).");
    if (ImGui::InputTextWithHint("[API] Value", "paste api key", &g_config.mcsrTrackerOverlay.apiKey,
                                 ImGuiInputTextFlags_Password)) {
        g_configIsDirty = true;
    }
    HoverHelp("API key value from your MCSR support ticket.");
    ImGui::EndDisabled();

    ImGui::BeginDisabled(g_config.mcsrTrackerOverlay.refreshOnlyMode);
    if (ImGui::SliderInt("[Poll] ms", &g_config.mcsrTrackerOverlay.pollIntervalMs, 10000, 3600000, "%d")) { g_configIsDirty = true; }
    HoverHelp("MCSR API polling interval. 600000 ms = 10 minutes.");
    ImGui::EndDisabled();

    if (ImGui::SliderInt("[X] Offset", &g_config.mcsrTrackerOverlay.x, -1200, 1200, "%d")) { g_configIsDirty = true; }
    HoverHelp("Horizontal offset from top-right anchor.");
    if (ImGui::SliderInt("[Y] Offset", &g_config.mcsrTrackerOverlay.y, -600, 1200, "%d")) { g_configIsDirty = true; }
    HoverHelp("Vertical offset from top-right anchor.");

    if (ImGui::SliderFloat("[Scale] UI", &g_config.mcsrTrackerOverlay.scale, 0.4f, 3.0f, "%.2f")) { g_configIsDirty = true; }
    HoverHelp("Tracker UI scale.");
    if (ImGui::SliderFloat("[A] Opacity", &g_config.mcsrTrackerOverlay.opacity, 0.0f, 1.0f, "%.2f")) { g_configIsDirty = true; }
    HoverHelp("Tracker text and border opacity.");
    if (ImGui::SliderFloat("[BG] Opacity", &g_config.mcsrTrackerOverlay.backgroundOpacity, 0.0f, 1.0f, "%.2f")) { g_configIsDirty = true; }
    HoverHelp("Tracker panel background opacity.");

    ImGui::Separator();
    ImGui::TextDisabled("[Hotkey] Toggle");
    ImGui::SameLine();
    if (ImGui::Checkbox("Ctrl##McsrHotkeyCtrl", &g_config.mcsrTrackerOverlay.hotkeyCtrl)) { g_configIsDirty = true; }
    ImGui::SameLine();
    if (ImGui::Checkbox("Shift##McsrHotkeyShift", &g_config.mcsrTrackerOverlay.hotkeyShift)) { g_configIsDirty = true; }
    ImGui::SameLine();
    if (ImGui::Checkbox("Alt##McsrHotkeyAlt", &g_config.mcsrTrackerOverlay.hotkeyAlt)) { g_configIsDirty = true; }

    static const std::vector<std::pair<std::string, DWORD>> mcsrHotkeyKeys = []() {
        std::vector<std::pair<std::string, DWORD>> keys;
        keys.reserve(220);
        auto addKey = [&](DWORD vk) {
            if (vk == 0) return;
            for (const auto& it : keys) {
                if (it.second == vk) return;
            }
            std::string label = VkToString(vk);
            if (label.empty() || label == "[None]") return;
            if (label.size() > 2 && label[0] == '0' && (label[1] == 'x' || label[1] == 'X')) return;
            keys.emplace_back(label, vk);
        };

        const DWORD preferred[] = { 'U', VK_TAB, VK_CAPITAL, VK_ESCAPE, VK_SPACE, VK_RETURN, VK_BACK, VK_LSHIFT, VK_RSHIFT,
                                    VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
                                    VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_SNAPSHOT, VK_SCROLL, VK_PAUSE,
                                    VK_NUMLOCK };
        for (DWORD vk : preferred) { addKey(vk); }

        for (DWORD vk = VK_F1; vk <= VK_F24; ++vk) { addKey(vk); }
        for (DWORD vk = 'A'; vk <= 'Z'; ++vk) { addKey(vk); }
        for (DWORD vk = '0'; vk <= '9'; ++vk) { addKey(vk); }
        for (DWORD vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk) { addKey(vk); }
        addKey(VK_MULTIPLY);
        addKey(VK_ADD);
        addKey(VK_SUBTRACT);
        addKey(VK_DECIMAL);
        addKey(VK_DIVIDE);
        addKey(VK_SEPARATOR);
        addKey(VK_OEM_1);
        addKey(VK_OEM_PLUS);
        addKey(VK_OEM_COMMA);
        addKey(VK_OEM_MINUS);
        addKey(VK_OEM_PERIOD);
        addKey(VK_OEM_2);
        addKey(VK_OEM_3);
        addKey(VK_OEM_4);
        addKey(VK_OEM_5);
        addKey(VK_OEM_6);
        addKey(VK_OEM_7);
        addKey(VK_XBUTTON1);
        addKey(VK_XBUTTON2);
        for (DWORD vk = 1; vk < 256; ++vk) { addKey(vk); }
        return keys;
    }();

    g_config.mcsrTrackerOverlay.hotkeyKey = std::clamp(g_config.mcsrTrackerOverlay.hotkeyKey, 1, 255);
    int mcsrSelectedKeyIndex = -1;
    for (int i = 0; i < static_cast<int>(mcsrHotkeyKeys.size()); ++i) {
        if (mcsrHotkeyKeys[static_cast<size_t>(i)].second == static_cast<DWORD>(g_config.mcsrTrackerOverlay.hotkeyKey)) {
            mcsrSelectedKeyIndex = i;
            break;
        }
    }
    const std::string mcsrKeyPreview =
        (mcsrSelectedKeyIndex >= 0) ? mcsrHotkeyKeys[static_cast<size_t>(mcsrSelectedKeyIndex)].first
                                    : VkToString(g_config.mcsrTrackerOverlay.hotkeyKey);
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::BeginCombo("[Key]##McsrHotkeyKey", mcsrKeyPreview.c_str())) {
        for (int i = 0; i < static_cast<int>(mcsrHotkeyKeys.size()); ++i) {
            const bool selected = (i == mcsrSelectedKeyIndex);
            const auto& keyOpt = mcsrHotkeyKeys[static_cast<size_t>(i)];
            if (ImGui::Selectable(keyOpt.first.c_str(), selected)) {
                g_config.mcsrTrackerOverlay.hotkeyKey = static_cast<int>(keyOpt.second);
                g_configIsDirty = true;
            }
            if (selected) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }

    std::string mcsrHotkeyLabel;
    if (g_config.mcsrTrackerOverlay.hotkeyCtrl) mcsrHotkeyLabel += "Ctrl+";
    if (g_config.mcsrTrackerOverlay.hotkeyShift) mcsrHotkeyLabel += "Shift+";
    if (g_config.mcsrTrackerOverlay.hotkeyAlt) mcsrHotkeyLabel += "Alt+";
    mcsrHotkeyLabel += VkToString(static_cast<DWORD>(g_config.mcsrTrackerOverlay.hotkeyKey));
    ImGui::TextDisabled("[Hotkey] %s", mcsrHotkeyLabel.c_str());
    HoverHelp("Configured combo toggles the MCSR tracker overlay.");

    ImGui::EndDisabled();
    ImGui::EndTabItem();
}
