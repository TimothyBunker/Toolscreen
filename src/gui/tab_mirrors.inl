if (ImGui::BeginTabItem("[R] Mirrors")) {
    g_currentlyEditingMirror = ""; // Disable image drag mode in other tabs
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    // Track selected mirror for visual feedback
    static std::string selectedMirrorName = "";

    int mirror_to_remove = -1;
    for (size_t i = 0; i < g_config.mirrors.size(); ++i) {
        auto& mirror = g_config.mirrors[i];
        ImGui::PushID((int)i);

        bool is_selected = (selectedMirrorName == mirror.name);
        ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_SpanAvailWidth | (is_selected ? ImGuiTreeNodeFlags_Selected : 0);

        // X button on the left
        std::string delete_button_label = "X##delete_mirror_" + std::to_string(i);
        if (ImGui::Button(delete_button_label.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
            std::string popup_id = "Delete Mirror?##" + std::to_string(i);
            ImGui::OpenPopup(popup_id.c_str());
        }

        // Popup modal outside of node_open block so it can be displayed even when collapsed
        std::string popup_id = "Delete Mirror?##" + std::to_string(i);
        if (ImGui::BeginPopupModal(popup_id.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Are you sure you want to delete mirror '%s'?\nThis cannot be undone.", mirror.name.c_str());
            ImGui::Separator();
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                if (selectedMirrorName == mirror.name) selectedMirrorName = "";
                mirror_to_remove = (int)i;
                g_configIsDirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        ImGui::SameLine();

        // Capture old name at frame start, before any ImGui input modifies it
        std::string oldMirrorName = mirror.name;

        bool node_open = ImGui::TreeNodeEx("##mirror_node", node_flags, "%s", mirror.name.c_str());
        if (ImGui::IsItemClicked(0)) { selectedMirrorName = mirror.name; }

        if (node_open) {
            ImGui::Text("Name");
            ImGui::SameLine();

            // Check for duplicate names
            bool hasDuplicate = HasDuplicateMirrorName(mirror.name, i);
            if (hasDuplicate) {
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
            }

            if (ImGui::InputText("##Name", &mirror.name)) {
                // Check if the new name is a duplicate
                if (!HasDuplicateMirrorName(mirror.name, i)) {
                    g_configIsDirty = true;
                    // Update all mode references to the renamed mirror
                    if (oldMirrorName != mirror.name) {
                        for (auto& mode : g_config.modes) {
                            for (auto& mirrorId : mode.mirrorIds) {
                                if (mirrorId == oldMirrorName) { mirrorId = mirror.name; }
                            }
                        }
                        for (auto& group : g_config.mirrorGroups) {
                            for (auto& item : group.mirrors) {
                                if (item.mirrorId == oldMirrorName) { item.mirrorId = mirror.name; }
                            }
                        }
                        // Update g_mirrorInstances map key so the mirror continues rendering
                        {
                            std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                            auto it = g_mirrorInstances.find(oldMirrorName);
                            if (it != g_mirrorInstances.end()) {
                                MirrorInstance inst = std::move(it->second);
                                g_mirrorInstances.erase(it);
                                inst.cachedRenderState.isValid = false;
                                inst.forceUpdateFrames = 3;
                                g_mirrorInstances[mirror.name] = std::move(inst);
                            }
                        }
                        // Update g_threadedMirrorConfigs so capture thread uses new name
                        {
                            std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
                            for (auto& conf : g_threadedMirrorConfigs) {
                                if (conf.name == oldMirrorName) {
                                    conf.name = mirror.name;
                                    break;
                                }
                            }
                        }
                        // Update selected mirror name if it was the one being renamed
                        if (selectedMirrorName == oldMirrorName) { selectedMirrorName = mirror.name; }
                    }
                } else {
                    // Revert the change if it creates a duplicate
                    mirror.name = oldMirrorName;
                }
            }

            if (hasDuplicate) { ImGui::PopStyleColor(3); }

            if (hasDuplicate) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Name already exists!");
            }

            ImGui::Separator();
            ImGui::Columns(2, "mirror_props", false);
            ImGui::SetColumnWidth(0, 150);
            ImGui::Text("FPS");
            ImGui::NextColumn();
            if (Spinner("##fps", &mirror.fps, 1, 1)) {
                g_configIsDirty = true;
                // Sync FPS to mirror thread immediately
                UpdateMirrorFPS(mirror.name, mirror.fps);
                // Force next frames to bypass throttling
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
            }
            ImGui::NextColumn();

            // ========== Border Settings Section ==========
            ImGui::Text("Border Settings");
            ImGui::NextColumn();

            // Helper lambda to update border settings
            auto updateBorderSettings = [&]() {
                UpdateMirrorCaptureSettings(mirror.name, mirror.captureWidth, mirror.captureHeight, mirror.border, mirror.colors,
                                            mirror.colorSensitivity, mirror.rawOutput, mirror.colorPassthrough);
                // Force re-render with new border
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
            };

            // Border Type dropdown
            const char* borderTypes[] = { "Dynamic (around content)", "Static (shape overlay)" };
            int currentType = static_cast<int>(mirror.border.type);
            ImGui::PushItemWidth(180);
            if (ImGui::Combo("##borderType", &currentType, borderTypes, IM_ARRAYSIZE(borderTypes))) {
                mirror.border.type = static_cast<MirrorBorderType>(currentType);
                g_configIsDirty = true;
                updateBorderSettings();
            }
            ImGui::PopItemWidth();

            if (mirror.border.type == MirrorBorderType::Dynamic) {
                // Dynamic border settings
                ImGui::Columns(1);
                ImGui::Indent(20);
                ImGui::TextDisabled("Draws border around matching pixels");

                ImGui::Columns(2, nullptr, false);
                ImGui::SetColumnWidth(0, 150);
                ImGui::Text("Thickness");
                ImGui::NextColumn();
                if (Spinner("##dynamicThickness", &mirror.border.dynamicThickness, 1, 0)) {
                    g_configIsDirty = true;
                    updateBorderSettings();
                }
                ImGui::NextColumn();

                ImGui::Text("Border Color");
                ImGui::NextColumn();
                float dynColorArr[4] = { mirror.colors.border.r, mirror.colors.border.g, mirror.colors.border.b, mirror.colors.border.a };
                ImGui::PushItemWidth(110);
                if (ImGui::ColorEdit4("##dynBorderColor", dynColorArr, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
                    mirror.colors.border = { dynColorArr[0], dynColorArr[1], dynColorArr[2], dynColorArr[3] };
                    g_configIsDirty = true;
                    updateBorderSettings();
                }
                ImGui::PopItemWidth();
                ImGui::Columns(1);
                ImGui::Unindent(20);
            } else {
                // Static border settings
                ImGui::Columns(1);
                ImGui::Indent(20);
                ImGui::TextDisabled("Static shape overlay (set Thickness to 0 to disable)");

                ImGui::Columns(2, nullptr, false);
                ImGui::SetColumnWidth(0, 150);

                // Thickness spinner (0 = disabled)
                ImGui::Text("Thickness");
                ImGui::NextColumn();
                if (Spinner("##staticThickness", &mirror.border.staticThickness, 1, 0)) {
                    g_configIsDirty = true;
                    updateBorderSettings();
                }
                ImGui::NextColumn();

                // Only show other controls if thickness > 0
                if (mirror.border.staticThickness > 0) {
                    // Shape dropdown
                    ImGui::Text("Shape");
                    ImGui::NextColumn();
                    const char* shapes[] = { "Rectangle", "Circle/Ellipse" };
                    int currentShape = static_cast<int>(mirror.border.staticShape);
                    ImGui::PushItemWidth(140);
                    if (ImGui::Combo("##staticShape", &currentShape, shapes, IM_ARRAYSIZE(shapes))) {
                        mirror.border.staticShape = static_cast<MirrorBorderShape>(currentShape);
                        g_configIsDirty = true;
                        updateBorderSettings();
                    }
                    ImGui::PopItemWidth();
                    ImGui::NextColumn();

                    // Color picker
                    ImGui::Text("Color");
                    ImGui::NextColumn();
                    float staticColorArr[4] = { mirror.border.staticColor.r, mirror.border.staticColor.g, mirror.border.staticColor.b,
                                                mirror.border.staticColor.a };
                    ImGui::PushItemWidth(110);
                    if (ImGui::ColorEdit4("##staticColor", staticColorArr, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
                        mirror.border.staticColor = { staticColorArr[0], staticColorArr[1], staticColorArr[2], staticColorArr[3] };
                        g_configIsDirty = true;
                        updateBorderSettings();
                    }
                    ImGui::PopItemWidth();
                    ImGui::NextColumn();

                    // Corner Radius (only for Rectangle)
                    if (mirror.border.staticShape == MirrorBorderShape::Rectangle) {
                        ImGui::Text("Corner Radius");
                        ImGui::NextColumn();
                        if (Spinner("##staticRadius", &mirror.border.staticRadius, 1, 0)) {
                            g_configIsDirty = true;
                            updateBorderSettings();
                        }
                        ImGui::NextColumn();
                    }

                    // Position/Size Offsets section
                    ImGui::Columns(1);
                    ImGui::Spacing();
                    ImGui::TextDisabled("Position/Size Offsets (relative to mirror)");
                    ImGui::Columns(2, nullptr, false);
                    ImGui::SetColumnWidth(0, 150);

                    // X offset
                    ImGui::Text("X Offset");
                    ImGui::NextColumn();
                    if (Spinner("##staticOffsetX", &mirror.border.staticOffsetX, 1)) {
                        g_configIsDirty = true;
                        updateBorderSettings();
                    }
                    ImGui::NextColumn();

                    // Y offset
                    ImGui::Text("Y Offset");
                    ImGui::NextColumn();
                    if (Spinner("##staticOffsetY", &mirror.border.staticOffsetY, 1)) {
                        g_configIsDirty = true;
                        updateBorderSettings();
                    }
                    ImGui::NextColumn();

                    // Width (0 = use mirror width)
                    ImGui::Text("Width (0=auto)");
                    ImGui::NextColumn();
                    if (Spinner("##staticWidth", &mirror.border.staticWidth, 1, 0)) {
                        g_configIsDirty = true;
                        updateBorderSettings();
                    }
                    ImGui::NextColumn();

                    // Height (0 = use mirror height)
                    ImGui::Text("Height (0=auto)");
                    ImGui::NextColumn();
                    if (Spinner("##staticHeight", &mirror.border.staticHeight, 1, 0)) {
                        g_configIsDirty = true;
                        updateBorderSettings();
                    }
                    ImGui::NextColumn();
                }
                ImGui::Columns(1);
                ImGui::Unindent(20);
            }

            // Restore columns for remaining content
            ImGui::Columns(2, nullptr, false);
            ImGui::SetColumnWidth(0, 150);
            ImGui::Text("Output Scale");
            ImGui::NextColumn();
            ImGui::TextDisabled("Note: Mirror output settings here do not affect mirrors rendered inside a Mirror Group.");

            // Checkbox to enable separate X/Y scaling
            if (ImGui::Checkbox("Separate X/Y##scale", &mirror.output.separateScale)) {
                g_configIsDirty = true;
                // When toggling, sync the values
                if (mirror.output.separateScale) {
                    // Copy unified scale to both X and Y
                    mirror.output.scaleX = mirror.output.scale;
                    mirror.output.scaleY = mirror.output.scale;
                }
                UpdateMirrorOutputPosition(mirror.name, mirror.output.x, mirror.output.y, mirror.output.scale, mirror.output.separateScale,
                                           mirror.output.scaleX, mirror.output.scaleY, mirror.output.relativeTo);
            }
            ImGui::SameLine();

            if (!mirror.output.separateScale) {
                // Single scale slider
                float scalePercent = mirror.output.scale * 100.0f;
                ImGui::SetNextItemWidth(200);
                if (ImGui::SliderFloat("##scale", &scalePercent, 10.0f, 2000.0f, "%.0f%%")) {
                    mirror.output.scale = scalePercent / 100.0f;
                    g_configIsDirty = true;
                    // Sync scale to mirror thread for cache computation
                    UpdateMirrorOutputPosition(mirror.name, mirror.output.x, mirror.output.y, mirror.output.scale,
                                               mirror.output.separateScale, mirror.output.scaleX, mirror.output.scaleY,
                                               mirror.output.relativeTo);
                }
            } else {
                // Separate X and Y scale sliders
                float scaleXPercent = mirror.output.scaleX * 100.0f;
                float scaleYPercent = mirror.output.scaleY * 100.0f;
                ImGui::SetNextItemWidth(100);
                if (ImGui::SliderFloat("X##scaleX", &scaleXPercent, 10.0f, 2000.0f, "%.0f%%")) {
                    mirror.output.scaleX = scaleXPercent / 100.0f;
                    g_configIsDirty = true;
                    UpdateMirrorOutputPosition(mirror.name, mirror.output.x, mirror.output.y, mirror.output.scale,
                                               mirror.output.separateScale, mirror.output.scaleX, mirror.output.scaleY,
                                               mirror.output.relativeTo);
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                if (ImGui::SliderFloat("Y##scaleY", &scaleYPercent, 10.0f, 2000.0f, "%.0f%%")) {
                    mirror.output.scaleY = scaleYPercent / 100.0f;
                    g_configIsDirty = true;
                    UpdateMirrorOutputPosition(mirror.name, mirror.output.x, mirror.output.y, mirror.output.scale,
                                               mirror.output.separateScale, mirror.output.scaleX, mirror.output.scaleY,
                                               mirror.output.relativeTo);
                }
            }
            ImGui::NextColumn();
            ImGui::Text("Capture Width");
            ImGui::NextColumn();
            if (Spinner("##cap_w", &mirror.captureWidth, 1, 1)) {
                g_configIsDirty = true;
                UpdateMirrorCaptureSettings(mirror.name, mirror.captureWidth, mirror.captureHeight, mirror.border, mirror.colors,
                                            mirror.colorSensitivity, mirror.rawOutput, mirror.colorPassthrough);
                // Force next 3 mirror updates to bypass FPS throttling
                // CRITICAL: Lock mutex before accessing g_mirrorInstances
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex); // Write lock
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
            }
            ImGui::NextColumn();
            ImGui::Text("Capture Height");
            ImGui::NextColumn();
            if (Spinner("##cap_h", &mirror.captureHeight, 1, 1)) {
                g_configIsDirty = true;
                UpdateMirrorCaptureSettings(mirror.name, mirror.captureWidth, mirror.captureHeight, mirror.border, mirror.colors,
                                            mirror.colorSensitivity, mirror.rawOutput, mirror.colorPassthrough);
                // Force next 3 mirror updates to bypass FPS throttling
                // CRITICAL: Lock mutex before accessing g_mirrorInstances
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex); // Write lock
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
            }
            ImGui::NextColumn();
            ImGui::Columns(1);
            ImGui::Separator();

            // Target Colors section - multiple colors can be matched
            ImGui::Text("Target Colors (max 8)");
            int target_color_to_remove = -1;
            for (size_t j = 0; j < mirror.colors.targetColors.size(); ++j) {
                ImGui::PushID(static_cast<int>(j));

                // Color picker for this target color
                std::string color_label = "Color " + std::to_string(j + 1);
                float targetColorArr[3] = {
                    mirror.colors.targetColors[j].r,
                    mirror.colors.targetColors[j].g,
                    mirror.colors.targetColors[j].b,
                };
                bool targetColorChangedByWidget = ImGui::ColorEdit3(color_label.c_str(), targetColorArr);
                bool targetColorChanged = targetColorChangedByWidget || targetColorArr[0] != mirror.colors.targetColors[j].r ||
                                          targetColorArr[1] != mirror.colors.targetColors[j].g ||
                                          targetColorArr[2] != mirror.colors.targetColors[j].b;
                if (targetColorChanged) {
                    mirror.colors.targetColors[j].r = targetColorArr[0];
                    mirror.colors.targetColors[j].g = targetColorArr[1];
                    mirror.colors.targetColors[j].b = targetColorArr[2];
                    g_configIsDirty = true;
                    UpdateMirrorCaptureSettings(mirror.name, mirror.captureWidth, mirror.captureHeight, mirror.border, mirror.colors,
                                                mirror.colorSensitivity, mirror.rawOutput, mirror.colorPassthrough);
                    // Force re-capture with new target color
                    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                    auto it = g_mirrorInstances.find(mirror.name);
                    if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
                }

                // Delete button (only show if more than 1 color)
                if (mirror.colors.targetColors.size() > 1) {
                    ImGui::SameLine();
                    if (ImGui::Button("X")) { target_color_to_remove = static_cast<int>(j); }
                }

                ImGui::PopID();
            }

            // Remove deleted target color
            if (target_color_to_remove != -1) {
                mirror.colors.targetColors.erase(mirror.colors.targetColors.begin() + target_color_to_remove);
                g_configIsDirty = true;
                UpdateMirrorCaptureSettings(mirror.name, mirror.captureWidth, mirror.captureHeight, mirror.border, mirror.colors,
                                            mirror.colorSensitivity, mirror.rawOutput, mirror.colorPassthrough);
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
            }

            // Add new target color button (max 8)
            if (mirror.colors.targetColors.size() < 8) {
                if (ImGui::Button("+ Add Target Color")) {
                    // Add a new default color (green)
                    Color newColor = { 0.0f, 1.0f, 0.0f };
                    mirror.colors.targetColors.push_back(newColor);
                    g_configIsDirty = true;
                    UpdateMirrorCaptureSettings(mirror.name, mirror.captureWidth, mirror.captureHeight, mirror.border, mirror.colors,
                                                mirror.colorSensitivity, mirror.rawOutput, mirror.colorPassthrough);
                    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                    auto it = g_mirrorInstances.find(mirror.name);
                    if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
                }
            }

            if (ImGui::SliderFloat("Color Sensitivity", &mirror.colorSensitivity, 0.001f, 1.0f)) {
                g_configIsDirty = true;
                UpdateMirrorCaptureSettings(mirror.name, mirror.captureWidth, mirror.captureHeight, mirror.border, mirror.colors,
                                            mirror.colorSensitivity, mirror.rawOutput, mirror.colorPassthrough);
                // Force re-capture with new sensitivity
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
            }
            // Color Passthrough checkbox - when enabled, output original pixel color instead of Output Color
            if (ImGui::Checkbox("Color Passthrough", &mirror.colorPassthrough)) {
                g_configIsDirty = true;
                UpdateMirrorCaptureSettings(mirror.name, mirror.captureWidth, mirror.captureHeight, mirror.border, mirror.colors,
                                            mirror.colorSensitivity, mirror.rawOutput, mirror.colorPassthrough);
                // Force re-capture with new mode
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
            }

            if (ImGui::SliderFloat("Opacity", &mirror.opacity, 0.0f, 1.0f)) {
                g_configIsDirty = true;
                // No need to update capture settings, opacity is applied at render time
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, matching pixels output their original color instead of the Output Color");
            }

            // Disable Output Color picker when Color Passthrough is enabled
            if (mirror.colorPassthrough) { ImGui::BeginDisabled(); }
            float outputColorArr[4] = { mirror.colors.output.r, mirror.colors.output.g, mirror.colors.output.b, mirror.colors.output.a };
            bool outputColorChangedByWidget = ImGui::ColorEdit4("Output Color", outputColorArr, ImGuiColorEditFlags_AlphaBar);
            bool outputColorChanged = outputColorChangedByWidget || outputColorArr[0] != mirror.colors.output.r ||
                                      outputColorArr[1] != mirror.colors.output.g || outputColorArr[2] != mirror.colors.output.b ||
                                      outputColorArr[3] != mirror.colors.output.a;
            if (outputColorChanged) {
                mirror.colors.output = { outputColorArr[0], outputColorArr[1], outputColorArr[2], outputColorArr[3] };
                g_configIsDirty = true;
                UpdateMirrorCaptureSettings(mirror.name, mirror.captureWidth, mirror.captureHeight, mirror.border, mirror.colors,
                                            mirror.colorSensitivity, mirror.rawOutput, mirror.colorPassthrough);
                // Force re-render with new output color
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
            }
            if (mirror.colorPassthrough) { ImGui::EndDisabled(); }
            float borderColorArr[4] = { mirror.colors.border.r, mirror.colors.border.g, mirror.colors.border.b, mirror.colors.border.a };
            bool borderColorChangedByWidget = ImGui::ColorEdit4("Border Color", borderColorArr, ImGuiColorEditFlags_AlphaBar);
            bool borderColorChanged = borderColorChangedByWidget || borderColorArr[0] != mirror.colors.border.r ||
                                      borderColorArr[1] != mirror.colors.border.g || borderColorArr[2] != mirror.colors.border.b ||
                                      borderColorArr[3] != mirror.colors.border.a;
            if (borderColorChanged) {
                mirror.colors.border = { borderColorArr[0], borderColorArr[1], borderColorArr[2], borderColorArr[3] };
                g_configIsDirty = true;
                UpdateMirrorCaptureSettings(mirror.name, mirror.captureWidth, mirror.captureHeight, mirror.border, mirror.colors,
                                            mirror.colorSensitivity, mirror.rawOutput, mirror.colorPassthrough);
                // Force re-render with new border color
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
            }
            if (ImGui::Checkbox("Raw Output", &mirror.rawOutput)) {
                g_configIsDirty = true;
                UpdateMirrorCaptureSettings(mirror.name, mirror.captureWidth, mirror.captureHeight, mirror.border, mirror.colors,
                                            mirror.colorSensitivity, mirror.rawOutput, mirror.colorPassthrough);
                // Force mirror to re-capture with new mode
                // CRITICAL: Lock mutex before accessing g_mirrorInstances
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex); // Write lock
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) {
                    it->second.forceUpdateFrames = 3;
                    // Immediately set desiredRawOutput for capture thread
                    it->second.desiredRawOutput.store(mirror.rawOutput, std::memory_order_release);
                    // Invalidate content so render waits for fresh capture with new mode
                    it->second.hasValidContent = false;
                }
            }
            if (ImGui::Checkbox("Only on my screen", &mirror.onlyOnMyScreen)) g_configIsDirty = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, this mirror will only be visible to you and not captured by OBS");
            }
            ImGui::Separator();

            ImGui::Text("Output Position");

            // Relative position toggle
            if (ImGui::Checkbox("Relative to screen##MirrorPos", &mirror.output.useRelativePosition)) {
                g_configIsDirty = true;
                if (mirror.output.useRelativePosition) {
                    // Convert current pixel values to percentages
                    int screenWidth = GetCachedScreenWidth();
                    int screenHeight = GetCachedScreenHeight();
                    mirror.output.relativeX = static_cast<float>(mirror.output.x) / screenWidth;
                    mirror.output.relativeY = static_cast<float>(mirror.output.y) / screenHeight;
                }
            }
            ImGui::SameLine();
            HelpMarker("When enabled, position is stored as percentages of screen size.\n"
                       "This makes configs portable across different screen resolutions.");

            ImGui::Columns(3, "output_pos_cols", false);
            const char* output_rel_to_preview = getFriendlyName(mirror.output.relativeTo, relativeToOptions);
            if (ImGui::BeginCombo("Relative To", output_rel_to_preview)) {
                for (const auto& option : relativeToOptions) {
                    if (ImGui::Selectable(option.second, mirror.output.relativeTo == option.first)) {
                        mirror.output.relativeTo = option.first;
                        g_configIsDirty = true;
                        // Sync position to mirror thread and invalidate cache
                        UpdateMirrorOutputPosition(mirror.name, mirror.output.x, mirror.output.y, mirror.output.scale,
                                                   mirror.output.separateScale, mirror.output.scaleX, mirror.output.scaleY,
                                                   mirror.output.relativeTo);
                        std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                        auto it = g_mirrorInstances.find(mirror.name);
                        if (it != g_mirrorInstances.end()) { it->second.cachedRenderState.isValid = false; }
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Columns(1);

            if (mirror.output.useRelativePosition) {
                // Percentage sliders mode
                int screenWidth = GetCachedScreenWidth();
                int screenHeight = GetCachedScreenHeight();

                ImGui::Text("X %%");
                ImGui::SameLine();
                float xPercent = mirror.output.relativeX * 100.0f;
                ImGui::SetNextItemWidth(200);
                if (ImGui::SliderFloat("##out_x_pct", &xPercent, -100.0f, 200.0f, "%.1f%%")) {
                    mirror.output.relativeX = xPercent / 100.0f;
                    int newX = static_cast<int>(mirror.output.relativeX * screenWidth);
                    mirror.output.x = newX;
                    g_configIsDirty = true;
                    UpdateMirrorOutputPosition(mirror.name, mirror.output.x, mirror.output.y, mirror.output.scale,
                                               mirror.output.separateScale, mirror.output.scaleX, mirror.output.scaleY,
                                               mirror.output.relativeTo);
                    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                    auto it = g_mirrorInstances.find(mirror.name);
                    if (it != g_mirrorInstances.end()) { it->second.cachedRenderState.isValid = false; }
                }
                ImGui::NextColumn();
                ImGui::Text("Y %%");
                ImGui::SameLine();
                float yPercent = mirror.output.relativeY * 100.0f;
                ImGui::SetNextItemWidth(200);
                if (ImGui::SliderFloat("##out_y_pct", &yPercent, -100.0f, 200.0f, "%.1f%%")) {
                    mirror.output.relativeY = yPercent / 100.0f;
                    int newY = static_cast<int>(mirror.output.relativeY * screenHeight);
                    mirror.output.y = newY;
                    g_configIsDirty = true;
                    UpdateMirrorOutputPosition(mirror.name, mirror.output.x, mirror.output.y, mirror.output.scale,
                                               mirror.output.separateScale, mirror.output.scaleX, mirror.output.scaleY,
                                               mirror.output.relativeTo);
                    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                    auto it = g_mirrorInstances.find(mirror.name);
                    if (it != g_mirrorInstances.end()) { it->second.cachedRenderState.isValid = false; }
                }
            } else {
                // Absolute pixel spinners mode (original behavior)
                ImGui::Text("X Offset");
                ImGui::SameLine();
                if (Spinner("##out_x", &mirror.output.x)) {
                    g_configIsDirty = true;
                    // Sync position to mirror thread and invalidate cache
                    UpdateMirrorOutputPosition(mirror.name, mirror.output.x, mirror.output.y, mirror.output.scale,
                                               mirror.output.separateScale, mirror.output.scaleX, mirror.output.scaleY,
                                               mirror.output.relativeTo);
                    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                    auto it = g_mirrorInstances.find(mirror.name);
                    if (it != g_mirrorInstances.end()) { it->second.cachedRenderState.isValid = false; }
                }
                ImGui::NextColumn();
                ImGui::Text("Y Offset");
                ImGui::SameLine();
                if (Spinner("##out_y", &mirror.output.y)) {
                    g_configIsDirty = true;
                    // Sync position to mirror thread and invalidate cache
                    UpdateMirrorOutputPosition(mirror.name, mirror.output.x, mirror.output.y, mirror.output.scale,
                                               mirror.output.separateScale, mirror.output.scaleX, mirror.output.scaleY,
                                               mirror.output.relativeTo);
                    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                    auto it = g_mirrorInstances.find(mirror.name);
                    if (it != g_mirrorInstances.end()) { it->second.cachedRenderState.isValid = false; }
                }
            }

            ImGui::SeparatorText("Input/Capture Zones");
            int zone_to_remove = -1;
            if (ImGui::BeginTable("zones_table", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("Relative To", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("##delete_col", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() + 4.0f);
                ImGui::TableHeadersRow();
                for (size_t j = 0; j < mirror.input.size(); ++j) {
                    ImGui::PushID((int)j);
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    if (Spinner("##X", &mirror.input[j].x)) {
                        g_configIsDirty = true;
                        UpdateMirrorInputRegions(mirror.name, mirror.input);
                        std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                        auto it = g_mirrorInstances.find(mirror.name);
                        if (it != g_mirrorInstances.end()) { it->second.forceUpdateFrames = 3; }
                    }
                    ImGui::TableSetColumnIndex(1);
                    if (Spinner("##Y", &mirror.input[j].y)) {
                        g_configIsDirty = true;
                        UpdateMirrorInputRegions(mirror.name, mirror.input);
                        std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                        auto it = g_mirrorInstances.find(mirror.name);
                        if (it != g_mirrorInstances.end()) { it->second.forceUpdateFrames = 3; }
                    }
                    ImGui::TableSetColumnIndex(2);
                    ImGui::SetNextItemWidth(-1);
                    const char* zone_rel_to_preview = getFriendlyName(mirror.input[j].relativeTo, relativeToOptions);
                    if (ImGui::BeginCombo("##zonerelto", zone_rel_to_preview)) {
                        for (const auto& option : relativeToOptions) {
                            if (ImGui::Selectable(option.second, mirror.input[j].relativeTo == option.first)) {
                                mirror.input[j].relativeTo = option.first;
                                g_configIsDirty = true;
                                UpdateMirrorInputRegions(mirror.name, mirror.input);
                                // Force mirror re-capture with new position
                                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                                auto it = g_mirrorInstances.find(mirror.name);
                                if (it != g_mirrorInstances.end()) { it->second.forceUpdateFrames = 3; }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TableSetColumnIndex(3);
                    std::string delete_zone_label = "X##delete_zone_" + std::to_string(j);
                    if (ImGui::Button(delete_zone_label.c_str(), ImVec2(-1, ImGui::GetFrameHeight()))) {
                        std::string zone_popup_id = "Delete Zone?##" + std::to_string(j);
                        ImGui::OpenPopup(zone_popup_id.c_str());
                    }
                    std::string zone_popup_id = "Delete Zone?##" + std::to_string(j);
                    if (ImGui::BeginPopupModal(zone_popup_id.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                        ImGui::Text("Delete capture zone %zu?", j + 1);
                        ImGui::Separator();
                        if (ImGui::Button("OK")) {
                            zone_to_remove = (int)j;
                            g_configIsDirty = true;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel")) { ImGui::CloseCurrentPopup(); }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            if (zone_to_remove != -1) {
                mirror.input.erase(mirror.input.begin() + zone_to_remove);
                UpdateMirrorInputRegions(mirror.name, mirror.input);
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) { it->second.forceUpdateFrames = 3; }
            }
            if (ImGui::Button("Add New Capture Zone")) {
                MirrorCaptureConfig newZone;
                newZone.relativeTo = "centerViewport";
                mirror.input.push_back(newZone);
                g_configIsDirty = true;
                UpdateMirrorInputRegions(mirror.name, mirror.input);
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) { it->second.forceUpdateFrames = 3; }
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (mirror_to_remove != -1) {
        std::string deletedMirrorName = g_config.mirrors[mirror_to_remove].name;
        g_config.mirrors.erase(g_config.mirrors.begin() + mirror_to_remove);
        // Remove deleted mirror from all modes
        for (auto& mode : g_config.modes) {
            auto it = std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), deletedMirrorName);
            while (it != mode.mirrorIds.end()) {
                mode.mirrorIds.erase(it);
                it = std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), deletedMirrorName);
            }
        }
        // Remove deleted mirror from all groups
        for (auto& group : g_config.mirrorGroups) {
            group.mirrors.erase(
                std::remove_if(group.mirrors.begin(), group.mirrors.end(),
                               [&deletedMirrorName](const MirrorGroupItem& item) { return item.mirrorId == deletedMirrorName; }),
                group.mirrors.end());
        }
        g_configIsDirty = true;
    }
    ImGui::Separator();
    if (ImGui::Button("Add New Mirror")) {
        MirrorConfig newMirror;
        newMirror.name = "New Mirror " + std::to_string(g_config.mirrors.size() + 1);
        newMirror.output.relativeTo = "centerViewport";
        MirrorCaptureConfig newZone;
        newZone.relativeTo = "centerViewport";
        newMirror.input.push_back(newZone);
        g_config.mirrors.push_back(newMirror);
        g_configIsDirty = true;
        // Create GPU resources immediately so the mirror renders without requiring a restart
        CreateMirrorGPUResources(newMirror);
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults##mirrors")) { ImGui::OpenPopup("Reset Mirrors to Defaults?"); }

    if (ImGui::BeginPopupModal("Reset Mirrors to Defaults?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "WARNING:");
        ImGui::Text("This will delete ALL custom mirrors and restore the default mirrors.");
        ImGui::Text("This action cannot be undone.");
        ImGui::Separator();
        if (ImGui::Button("Confirm Reset", ImVec2(120, 0))) {
            g_config.mirrors = GetDefaultMirrors();
            g_configIsDirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::SeparatorText("Mirror Groups");

    static std::string selectedGroupName = "";
    int group_to_remove = -1;
    for (size_t i = 0; i < g_config.mirrorGroups.size(); ++i) {
        auto& group = g_config.mirrorGroups[i];
        ImGui::PushID((int)i + 100000);

        bool is_selected = (selectedGroupName == group.name);
        ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_SpanAvailWidth | (is_selected ? ImGuiTreeNodeFlags_Selected : 0);

        std::string delete_group_label = "X##delete_mirror_group_" + std::to_string(i);
        if (ImGui::Button(delete_group_label.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
            std::string popup_id_group = "Delete Mirror Group?##" + std::to_string(i);
            ImGui::OpenPopup(popup_id_group.c_str());
        }

        std::string popup_id_group = "Delete Mirror Group?##" + std::to_string(i);
        if (ImGui::BeginPopupModal(popup_id_group.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Are you sure you want to delete mirror group '%s'?\nThis cannot be undone.", group.name.c_str());
            ImGui::Separator();
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                if (selectedGroupName == group.name) selectedGroupName = "";
                group_to_remove = (int)i;
                g_configIsDirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        bool node_open = ImGui::TreeNodeEx("##mirror_group_node", node_flags, "%s", group.name.c_str());
        if (ImGui::IsItemClicked(0)) { selectedGroupName = group.name; }

        if (node_open) {
            ImGui::Text("Name");
            ImGui::SameLine();
            std::string oldGroupName = group.name;

            bool hasDuplicateGroup = HasDuplicateMirrorGroupName(group.name, i);
            if (hasDuplicateGroup) {
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
            }

            if (ImGui::InputText("##GroupName", &group.name)) {
                if (!HasDuplicateMirrorGroupName(group.name, i)) {
                    g_configIsDirty = true;
                    if (oldGroupName != group.name) {
                        for (auto& mode : g_config.modes) {
                            for (auto& groupId : mode.mirrorGroupIds) {
                                if (groupId == oldGroupName) { groupId = group.name; }
                            }
                        }
                        if (selectedGroupName == oldGroupName) { selectedGroupName = group.name; }
                    }
                } else {
                    group.name = oldGroupName;
                }
            }

            if (hasDuplicateGroup) { ImGui::PopStyleColor(3); }
            if (hasDuplicateGroup) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Name already exists!");
            }

            ImGui::Separator();
            ImGui::Text("Group Output Position");

            // Relative position toggle
            if (ImGui::Checkbox("Relative to screen##GroupPos", &group.output.useRelativePosition)) {
                g_configIsDirty = true;
                if (group.output.useRelativePosition) {
                    // Convert current pixel values to percentages
                    int screenWidth = GetCachedScreenWidth();
                    int screenHeight = GetCachedScreenHeight();
                    group.output.relativeX = static_cast<float>(group.output.x) / screenWidth;
                    group.output.relativeY = static_cast<float>(group.output.y) / screenHeight;
                }
            }
            ImGui::SameLine();
            HelpMarker("When enabled, position is stored as percentages of screen size.\n"
                       "This makes configs portable across different screen resolutions.");

            ImGui::Columns(3, "group_output_pos_cols", false);
            const char* group_output_rel_to_preview = getFriendlyName(group.output.relativeTo, relativeToOptions);
            if (ImGui::BeginCombo("Relative To##group_output", group_output_rel_to_preview)) {
                for (const auto& option : relativeToOptions) {
                    if (ImGui::Selectable(option.second, group.output.relativeTo == option.first)) {
                        group.output.relativeTo = option.first;
                        g_configIsDirty = true;
                        // Extract mirror IDs from mirrors vector for the helper function
                        std::vector<std::string> groupMirrorIds;
                        for (const auto& item : group.mirrors) { groupMirrorIds.push_back(item.mirrorId); }
                        UpdateMirrorGroupOutputPosition(groupMirrorIds, group.output.x, group.output.y, group.output.scale,
                                                        group.output.separateScale, group.output.scaleX, group.output.scaleY,
                                                        group.output.relativeTo);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Columns(1);

            if (group.output.useRelativePosition) {
                // Percentage sliders mode
                int screenWidth = GetCachedScreenWidth();
                int screenHeight = GetCachedScreenHeight();

                ImGui::Text("X %%");
                ImGui::SameLine();
                float xPercent = group.output.relativeX * 100.0f;
                ImGui::SetNextItemWidth(200);
                if (ImGui::SliderFloat("##group_out_x_pct", &xPercent, -100.0f, 200.0f, "%.1f%%")) {
                    group.output.relativeX = xPercent / 100.0f;
                    int newX = static_cast<int>(group.output.relativeX * screenWidth);
                    group.output.x = newX;
                    g_configIsDirty = true;
                    std::vector<std::string> groupMirrorIds;
                    for (const auto& item : group.mirrors) { groupMirrorIds.push_back(item.mirrorId); }
                    UpdateMirrorGroupOutputPosition(groupMirrorIds, group.output.x, group.output.y, group.output.scale,
                                                    group.output.separateScale, group.output.scaleX, group.output.scaleY,
                                                    group.output.relativeTo);
                }
                ImGui::NextColumn();
                ImGui::Text("Y %%");
                ImGui::SameLine();
                float yPercent = group.output.relativeY * 100.0f;
                ImGui::SetNextItemWidth(200);
                if (ImGui::SliderFloat("##group_out_y_pct", &yPercent, -100.0f, 200.0f, "%.1f%%")) {
                    group.output.relativeY = yPercent / 100.0f;
                    int newY = static_cast<int>(group.output.relativeY * screenHeight);
                    group.output.y = newY;
                    g_configIsDirty = true;
                    std::vector<std::string> groupMirrorIds;
                    for (const auto& item : group.mirrors) { groupMirrorIds.push_back(item.mirrorId); }
                    UpdateMirrorGroupOutputPosition(groupMirrorIds, group.output.x, group.output.y, group.output.scale,
                                                    group.output.separateScale, group.output.scaleX, group.output.scaleY,
                                                    group.output.relativeTo);
                }
            } else {
                // Absolute pixel spinners mode (original behavior)
                ImGui::Text("X Offset");
                ImGui::SameLine();
                if (Spinner("##group_out_x", &group.output.x)) {
                    g_configIsDirty = true;
                    std::vector<std::string> groupMirrorIds;
                    for (const auto& item : group.mirrors) { groupMirrorIds.push_back(item.mirrorId); }
                    UpdateMirrorGroupOutputPosition(groupMirrorIds, group.output.x, group.output.y, group.output.scale,
                                                    group.output.separateScale, group.output.scaleX, group.output.scaleY,
                                                    group.output.relativeTo);
                }
                ImGui::NextColumn();
                ImGui::Text("Y Offset");
                ImGui::SameLine();
                if (Spinner("##group_out_y", &group.output.y)) {
                    g_configIsDirty = true;
                    std::vector<std::string> groupMirrorIds;
                    for (const auto& item : group.mirrors) { groupMirrorIds.push_back(item.mirrorId); }
                    UpdateMirrorGroupOutputPosition(groupMirrorIds, group.output.x, group.output.y, group.output.scale,
                                                    group.output.separateScale, group.output.scaleX, group.output.scaleY,
                                                    group.output.relativeTo);
                }
            }
            ImGui::Columns(1);

            ImGui::Separator();
            ImGui::Text("Group Mirrors (Per-Item Sizing)");
            ImGui::SameLine();
            HelpMarker("Width%% and Height%% only affect mirrors when rendered as part of this group.\\n"
                       "Mirrors used outside this group will render at their original size.");

            int group_mirror_to_remove = -1;
            for (size_t j = 0; j < group.mirrors.size(); ++j) {
                ImGui::PushID((int)j + 200000);
                MirrorGroupItem& item = group.mirrors[j];

                std::string del_group_mirror_label = "X##del_group_mirror_" + std::to_string(j);
                if (ImGui::Button(del_group_mirror_label.c_str())) { group_mirror_to_remove = (int)j; }
                ImGui::SameLine();
                if (ImGui::Checkbox("##enabled", &item.enabled)) { g_configIsDirty = true; }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Enable/disable this mirror in the group"); }
                ImGui::SameLine();
                if (!item.enabled) { ImGui::BeginDisabled(); }
                ImGui::Text("%s", item.mirrorId.c_str());

                // Per-item sizing controls
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                float widthPct = item.widthPercent * 100.0f;
                if (ImGui::SliderFloat("##width_pct", &widthPct, 10.0f, 200.0f, "W:%.0f%%")) {
                    item.widthPercent = widthPct / 100.0f;
                    g_configIsDirty = true;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                float heightPct = item.heightPercent * 100.0f;
                if (ImGui::SliderFloat("##height_pct", &heightPct, 10.0f, 200.0f, "H:%.0f%%")) {
                    item.heightPercent = heightPct / 100.0f;
                    g_configIsDirty = true;
                }
                ImGui::SameLine();
                ImGui::Text("X:");
                ImGui::SameLine();
                if (Spinner("##offset_x", &item.offsetX, 1, INT_MIN, INT_MAX, 40.0f, 0.0f)) { g_configIsDirty = true; }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("X Offset (pixels)"); }
                ImGui::SameLine();
                ImGui::Text("Y:");
                ImGui::SameLine();
                if (Spinner("##offset_y", &item.offsetY, 1, INT_MIN, INT_MAX, 40.0f, 0.0f)) { g_configIsDirty = true; }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Y Offset (pixels)"); }
                if (!item.enabled) { ImGui::EndDisabled(); }

                ImGui::PopID();
            }
            if (group_mirror_to_remove != -1) {
                group.mirrors.erase(group.mirrors.begin() + group_mirror_to_remove);
                g_configIsDirty = true;
            }

            // Check which mirrors are already in this group
            std::vector<std::string> existingMirrorIds;
            for (const auto& item : group.mirrors) { existingMirrorIds.push_back(item.mirrorId); }

            if (ImGui::BeginCombo("Add Mirror##add_mirror_to_group", "[Select Mirror]")) {
                for (const auto& mirrorConf : g_config.mirrors) {
                    if (std::find(existingMirrorIds.begin(), existingMirrorIds.end(), mirrorConf.name) == existingMirrorIds.end()) {
                        if (ImGui::Selectable(mirrorConf.name.c_str())) {
                            MirrorGroupItem newItem;
                            newItem.mirrorId = mirrorConf.name;
                            newItem.widthPercent = 1.0f;  // Default 100%
                            newItem.heightPercent = 1.0f; // Default 100%
                            group.mirrors.push_back(newItem);
                            g_configIsDirty = true;
                        }
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
    }
    if (group_to_remove != -1) {
        std::string deletedGroupName = g_config.mirrorGroups[group_to_remove].name;
        g_config.mirrorGroups.erase(g_config.mirrorGroups.begin() + group_to_remove);
        for (auto& mode : g_config.modes) {
            auto it = std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), deletedGroupName);
            while (it != mode.mirrorGroupIds.end()) {
                mode.mirrorGroupIds.erase(it);
                it = std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), deletedGroupName);
            }
        }
        g_configIsDirty = true;
    }

    if (ImGui::Button("Add New Mirror Group")) {
        MirrorGroupConfig newGroup;
        newGroup.name = "New Group " + std::to_string(g_config.mirrorGroups.size() + 1);
        newGroup.output.relativeTo = "centerViewport";
        g_config.mirrorGroups.push_back(newGroup);
        g_configIsDirty = true;
    }

    ImGui::EndTabItem();
}
