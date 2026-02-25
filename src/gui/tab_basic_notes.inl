if (ImGui::BeginTabItem("[N] Notes")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    if (ImGui::Checkbox("[ON] Notes", &g_config.notesOverlay.enabled)) { g_configIsDirty = true; }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", "Enable/disable notes overlay feature.");
    }

    ImGui::BeginDisabled(!g_config.notesOverlay.enabled);

    bool notesStartHidden = !g_config.notesOverlay.visible;
    if (ImGui::Checkbox("[Start] Hidden", &notesStartHidden)) {
        g_config.notesOverlay.visible = !notesStartHidden;
        g_configIsDirty = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", "Start notes overlay hidden.");
    }

    if (ImGui::SliderFloat("[BG] Dim", &g_config.notesOverlay.backgroundOpacity, 0.10f, 0.95f, "%.2f")) { g_configIsDirty = true; }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", "Backdrop dim level for notes overlay.");
    }

    if (ImGui::SliderFloat("[Scale] Notes", &g_config.notesOverlay.panelScale, 0.75f, 1.50f, "%.2f")) { g_configIsDirty = true; }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", "Notes panel scale.");
    }

    if (ImGui::InputTextWithHint("[MD] Dir", "notes/General", &g_config.notesOverlay.markdownDirectory)) { g_configIsDirty = true; }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", "Markdown notes directory. Relative paths are from the toolscreen folder.");
    }

    if (ImGui::InputTextWithHint("[PDF] Dir", "notes/PDF", &g_config.notesOverlay.pdfDirectory)) { g_configIsDirty = true; }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", "PDF export directory. Relative paths are from the toolscreen folder.");
    }

    if (ImGui::Checkbox("[PDF] Open Dir", &g_config.notesOverlay.openPdfFolderAfterExport)) { g_configIsDirty = true; }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", "Open the PDF folder automatically after exporting.");
    }

    ImGui::Separator();
    ImGui::TextDisabled("[Hotkey] Toggle");
    ImGui::SameLine();
    if (ImGui::Checkbox("Ctrl##NotesHotkeyCtrl", &g_config.notesOverlay.hotkeyCtrl)) { g_configIsDirty = true; }
    ImGui::SameLine();
    if (ImGui::Checkbox("Shift##NotesHotkeyShift", &g_config.notesOverlay.hotkeyShift)) { g_configIsDirty = true; }
    ImGui::SameLine();
    if (ImGui::Checkbox("Alt##NotesHotkeyAlt", &g_config.notesOverlay.hotkeyAlt)) { g_configIsDirty = true; }

    static const std::vector<std::pair<std::string, DWORD>> notesHotkeyKeys = []() {
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

        const DWORD preferred[] = { 'N', VK_TAB, VK_CAPITAL, VK_ESCAPE, VK_SPACE, VK_RETURN, VK_BACK, VK_LSHIFT, VK_RSHIFT,
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

    g_config.notesOverlay.hotkeyKey = std::clamp(g_config.notesOverlay.hotkeyKey, 1, 255);
    int notesSelectedKeyIndex = -1;
    for (int i = 0; i < static_cast<int>(notesHotkeyKeys.size()); ++i) {
        if (notesHotkeyKeys[static_cast<size_t>(i)].second == static_cast<DWORD>(g_config.notesOverlay.hotkeyKey)) {
            notesSelectedKeyIndex = i;
            break;
        }
    }
    const std::string notesKeyPreview =
        (notesSelectedKeyIndex >= 0) ? notesHotkeyKeys[static_cast<size_t>(notesSelectedKeyIndex)].first : VkToString(g_config.notesOverlay.hotkeyKey);
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::BeginCombo("[Key]##NotesHotkeyKey", notesKeyPreview.c_str())) {
        for (int i = 0; i < static_cast<int>(notesHotkeyKeys.size()); ++i) {
            const bool selected = (i == notesSelectedKeyIndex);
            const auto& keyOpt = notesHotkeyKeys[static_cast<size_t>(i)];
            if (ImGui::Selectable(keyOpt.first.c_str(), selected)) {
                g_config.notesOverlay.hotkeyKey = static_cast<int>(keyOpt.second);
                g_configIsDirty = true;
            }
            if (selected) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }

    std::string notesHotkeyLabel;
    if (g_config.notesOverlay.hotkeyCtrl) notesHotkeyLabel += "Ctrl+";
    if (g_config.notesOverlay.hotkeyShift) notesHotkeyLabel += "Shift+";
    if (g_config.notesOverlay.hotkeyAlt) notesHotkeyLabel += "Alt+";
    notesHotkeyLabel += VkToString(static_cast<DWORD>(g_config.notesOverlay.hotkeyKey));
    ImGui::TextDisabled("[Hotkey] %s", notesHotkeyLabel.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", "Configured combo toggles notes overlay.");
    }

    ImGui::EndDisabled();
    ImGui::EndTabItem();
}
