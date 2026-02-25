if (ImGui::BeginTabItem("[K] Macros")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    auto MacroHoverHelp = [](const char* desc) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) { ImGui::SetTooltip("%s", desc); }
    };

    const std::string macroGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
    const bool macroInWorld = macroGameState.find("inworld") != std::string::npos;
    const bool macroRuntimeEnabled = AreMacrosRuntimeEnabled();
    const bool macroBlockedByState = g_config.keyRebinds.globalOnlyInWorld && !macroInWorld;
    const bool macroActiveNow = macroRuntimeEnabled && !macroBlockedByState;

    ImGui::TextUnformatted("[M] Engine");
    MacroHoverHelp("Macro engine runtime status.");
    ImGui::SameLine();
    ImGui::TextColored(macroActiveNow ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "[%s]",
                       macroActiveNow ? "ACTIVE" : "BLOCKED");

    if (!macroRuntimeEnabled) {
        ImGui::TextDisabled("[M] off (Ctrl+Shift+M)");
    } else if (macroBlockedByState) {
        ImGui::TextDisabled("[Gate] blocked");
    } else {
        ImGui::TextDisabled("[M] Ctrl+Shift+M");
    }

    if (ImGui::Checkbox("[G] In-Game", &g_config.keyRebinds.globalOnlyInWorld)) { g_configIsDirty = true; }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", "Enable macros only while in-game states are active.");
    }

    ImGui::SeparatorText("[F3] Rebind");

    auto isF3TargetRebind = [](const KeyRebind& rebind) {
        return rebind.toKey == VK_F3 && !rebind.useCustomOutput;
    };

    int f3RebindIndex = -1;
    for (int i = 0; i < static_cast<int>(g_config.keyRebinds.rebinds.size()); ++i) {
        if (isF3TargetRebind(g_config.keyRebinds.rebinds[static_cast<size_t>(i)])) {
            f3RebindIndex = i;
            break;
        }
    }

    bool f3MacroEnabled = (f3RebindIndex >= 0);
    if (ImGui::Checkbox("[ON]##f3macro", &f3MacroEnabled)) {
        if (f3MacroEnabled) {
            if (f3RebindIndex < 0) {
                KeyRebind rebind{};
                rebind.fromKey = VK_TAB;
                rebind.toKey = VK_F3;
                rebind.enabled = true;
                rebind.onlyInWorld = true;
                rebind.useCustomOutput = false;
                rebind.customOutputVK = 0;
                rebind.customOutputScanCode = 0;
                g_config.keyRebinds.rebinds.push_back(rebind);
            } else {
                g_config.keyRebinds.rebinds[static_cast<size_t>(f3RebindIndex)].enabled = true;
                g_config.keyRebinds.rebinds[static_cast<size_t>(f3RebindIndex)].onlyInWorld = true;
            }
            g_config.keyRebinds.enabled = true;
        } else {
            g_config.keyRebinds.rebinds.erase(
                std::remove_if(g_config.keyRebinds.rebinds.begin(), g_config.keyRebinds.rebinds.end(), isF3TargetRebind),
                g_config.keyRebinds.rebinds.end());
            if (g_config.keyRebinds.rebinds.empty()) { g_config.keyRebinds.enabled = false; }
        }
        g_configIsDirty = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) { ImGui::SetTooltip("%s", "Enable F3 macro remap."); }

    f3RebindIndex = -1;
    for (int i = 0; i < static_cast<int>(g_config.keyRebinds.rebinds.size()); ++i) {
        if (isF3TargetRebind(g_config.keyRebinds.rebinds[static_cast<size_t>(i)])) {
            f3RebindIndex = i;
            break;
        }
    }

    if (f3RebindIndex >= 0) {
        static const std::vector<std::pair<std::string, DWORD>> macroKeys = []() {
            std::vector<std::pair<std::string, DWORD>> keys;
            keys.reserve(200);
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

            const DWORD preferred[] = { VK_TAB, VK_CAPITAL, VK_ESCAPE, VK_SPACE, VK_RETURN, VK_BACK, VK_LSHIFT, VK_RSHIFT,
                                        VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
                                        VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_SNAPSHOT, VK_SCROLL,
                                        VK_PAUSE, VK_NUMLOCK };
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

        auto& rebind = g_config.keyRebinds.rebinds[static_cast<size_t>(f3RebindIndex)];
        int selectedIndex = -1;
        for (int i = 0; i < static_cast<int>(macroKeys.size()); ++i) {
            if (macroKeys[static_cast<size_t>(i)].second == rebind.fromKey) {
                selectedIndex = i;
                break;
            }
        }

        const std::string previewLabel = (selectedIndex >= 0) ? macroKeys[static_cast<size_t>(selectedIndex)].first : VkToString(rebind.fromKey);
        ImGui::SetNextItemWidth(250.0f);
        if (ImGui::BeginCombo("[Key]", previewLabel.c_str())) {
            for (int i = 0; i < static_cast<int>(macroKeys.size()); ++i) {
                const bool selected = (i == selectedIndex);
                const auto& keyOpt = macroKeys[static_cast<size_t>(i)];
                if (ImGui::Selectable(keyOpt.first.c_str(), selected)) {
                    rebind.fromKey = keyOpt.second;
                    rebind.toKey = VK_F3;
                    rebind.enabled = true;
                    rebind.useCustomOutput = false;
                    rebind.customOutputVK = 0;
                    rebind.customOutputScanCode = 0;
                    g_config.keyRebinds.enabled = true;
                    g_configIsDirty = true;
                }
                if (selected) { ImGui::SetItemDefaultFocus(); }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (ImGui::Checkbox("[W]", &rebind.onlyInWorld)) { g_configIsDirty = true; }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", "Only fire trigger while in-world game state is active.");
        }

        ImGui::TextDisabled("[F3] %s", previewLabel.c_str());
        MacroHoverHelp("Current trigger key mapped to F3.");
    } else {
        ImGui::TextDisabled("[F3] set [ON] to bind");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Stronghold hotkeys: H / Shift+H / Ctrl+Shift+H / Num8/2/4/5/6");
    ImGui::TextDisabled("Notes hotkey: Ctrl+Shift+N");

    ImGui::EndTabItem();
}
