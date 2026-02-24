if (ImGui::BeginTabItem("[I] Images")) {
    g_currentlyEditingMirror = "";

    // Enable image drag mode when Images tab is active
    g_imageDragMode.store(true);
    // Disable window overlay drag mode in Images tab
    g_windowOverlayDragMode.store(false);

    // Show instructions
    ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "[Drag] Move images directly in game while this tab is open.");
    ImGui::Separator();

    int image_to_remove = -1;
    for (size_t i = 0; i < g_config.images.size(); ++i) {
        auto& img = g_config.images[i];
        ImGui::PushID(static_cast<int>(i));

        // X button on the left
        std::string delete_img_label = "X##delete_image_" + std::to_string(i);
        if (ImGui::Button(delete_img_label.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
            std::string img_popup_id = "Delete Image?##" + std::to_string(i);
            ImGui::OpenPopup(img_popup_id.c_str());
        }

        // Popup modal outside of node_open block so it can be displayed even when collapsed
        std::string img_popup_id = "Delete Image?##" + std::to_string(i);
        if (ImGui::BeginPopupModal(img_popup_id.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Are you sure you want to delete image '%s'?", img.name.c_str());
            ImGui::Separator();
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                image_to_remove = (int)i;
                g_configIsDirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        ImGui::SameLine();

        // Capture old name at frame start, before any ImGui input modifies it
        std::string oldImageName = img.name;

        bool node_open = ImGui::TreeNodeEx("##image_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", img.name.c_str());

        if (node_open) {

            // Check for duplicate names
            bool hasDuplicate = HasDuplicateImageName(img.name, i);
            if (hasDuplicate) {
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
            }

            if (ImGui::InputText("Name", &img.name)) {
                // Check if the new name is a duplicate
                if (!HasDuplicateImageName(img.name, i)) {
                    g_configIsDirty = true;
                    // Update all mode references to the renamed image
                    if (oldImageName != img.name) {
                        for (auto& mode : g_config.modes) {
                            for (auto& imageId : mode.imageIds) {
                                if (imageId == oldImageName) { imageId = img.name; }
                            }
                        }
                    }
                } else {
                    // Revert the change if it creates a duplicate
                    img.name = oldImageName;
                }
            }

            if (hasDuplicate) { ImGui::PopStyleColor(3); }

            if (hasDuplicate) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Name already exists!");
            }

            std::string imgErrorKey = "img_" + img.name;
            if (ImGui::InputText("Path", &img.path)) {
                ClearImageError(imgErrorKey);
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(("Browse...##img_" + img.name).c_str())) {
                // Use the validated image picker
                ImagePickerResult result = OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);

                if (result.completed) {
                    if (result.success) {
                        img.path = result.path;
                        ClearImageError(imgErrorKey);
                        g_configIsDirty = true;
                        LoadImageAsync(DecodedImageData::UserImage, img.name, img.path, g_toolscreenPath);
                    } else if (!result.error.empty()) {
                        SetImageError(imgErrorKey, result.error);
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(("Validate##img_val_" + img.name).c_str())) {
                // Validate manually-entered path
                std::string error = ValidateImageFile(img.path, g_toolscreenPath);
                if (error.empty()) {
                    ClearImageError(imgErrorKey);
                    // Reload the image if it's valid
                    LoadImageAsync(DecodedImageData::UserImage, img.name, img.path, g_toolscreenPath);
                } else {
                    SetImageError(imgErrorKey, error);
                }
            }

            // Show error message if any
            std::string imgError = GetImageError(imgErrorKey);
            if (!imgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", imgError.c_str()); }

            ImGui::SeparatorText("Rendering");
            ;
            if (ImGui::SliderFloat("Opacity", &img.opacity, 0.0f, 1.0f)) g_configIsDirty = true;
            if (ImGui::Checkbox("Pixelated Scaling", &img.pixelatedScaling)) g_configIsDirty = true;
            if (ImGui::Checkbox("Only on my screen", &img.onlyOnMyScreen)) g_configIsDirty = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, this image will only be visible to you and not captured by OBS");
            }

            ImGui::Columns(2, "img_render", false);
            ImGui::SetColumnWidth(0, 120);
            ImGui::Text("X");
            ImGui::NextColumn();
            if (Spinner("##img_x", &img.x)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text("Y");
            ImGui::NextColumn();
            if (Spinner("##img_y", &img.y)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text("Scale");
            ImGui::NextColumn();
            float scalePercent = img.scale * 100.0f;
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##img_scale", &scalePercent, 10.0f, 200.0f, "%.0f%%")) {
                img.scale = scalePercent / 100.0f;
                g_configIsDirty = true;
            }
            ImGui::NextColumn();
            ImGui::Text("Relative To");
            ImGui::NextColumn();
            const char* current_rel_to = getFriendlyName(img.relativeTo, imageRelativeToOptions);
            ImGui::SetNextItemWidth(150);
            if (ImGui::BeginCombo("##img_rel_to", current_rel_to)) {
                for (const auto& option : imageRelativeToOptions) {
                    if (ImGui::Selectable(option.second, img.relativeTo == option.first)) {
                        img.relativeTo = option.first;
                        g_configIsDirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Columns(1);

            ImGui::SeparatorText("Cropping (from source image, in pixels)");
            ImGui::Columns(2, "img_crop", false);
            ImGui::SetColumnWidth(0, 120);
            ImGui::Text("Crop Top");
            ImGui::NextColumn();
            if (Spinner("##img_crop_t", &img.crop_top, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text("Crop Bottom");
            ImGui::NextColumn();
            if (Spinner("##img_crop_b", &img.crop_bottom, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text("Crop Left");
            ImGui::NextColumn();
            if (Spinner("##img_crop_l", &img.crop_left, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text("Crop Right");
            ImGui::NextColumn();
            if (Spinner("##img_crop_r", &img.crop_right, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Columns(1);

            ImGui::SeparatorText("Background");
            if (ImGui::Checkbox("Enable Background", &img.background.enabled)) g_configIsDirty = true;
            ImGui::BeginDisabled(!img.background.enabled);
            if (ImGui::ColorEdit3("BG Color", &img.background.color.r)) g_configIsDirty = true;
            if (ImGui::SliderFloat("BG Opacity", &img.background.opacity, 0.0f, 1.0f)) g_configIsDirty = true;
            ImGui::EndDisabled();

            ImGui::SeparatorText("Color Keying");
            if (ImGui::Checkbox("Enable Color Key", &img.enableColorKey)) g_configIsDirty = true;
            ImGui::BeginDisabled(!img.enableColorKey);

            // Multiple color keys UI
            int imgColorKeyToRemove = -1;
            for (size_t k = 0; k < img.colorKeys.size(); k++) {
                ImGui::PushID(static_cast<int>(k));
                auto& ck = img.colorKeys[k];

                ImGui::Text("Key %zu:", k + 1);
                ImGui::SameLine();
                ImGui::PushItemWidth(150.0f);
                if (ImGui::ColorEdit3("##color", &ck.color.r, ImGuiColorEditFlags_NoLabel)) g_configIsDirty = true;
                ImGui::PopItemWidth();
                ImGui::SameLine();
                ImGui::PushItemWidth(80.0f);
                if (ImGui::SliderFloat("##sens", &ck.sensitivity, 0.001f, 1.0f, "%.3f")) g_configIsDirty = true;
                ImGui::PopItemWidth();
                ImGui::SameLine();
                if (ImGui::Button("X##remove")) { imgColorKeyToRemove = static_cast<int>(k); }
                ImGui::PopID();
            }

            // Remove color key if requested
            if (imgColorKeyToRemove >= 0) {
                img.colorKeys.erase(img.colorKeys.begin() + imgColorKeyToRemove);
                g_configIsDirty = true;
            }

            // Add new color key button
            if (ImGui::Button("+ Add Color Key")) {
                ColorKeyConfig newKey;
                newKey.color = { 0.0f, 0.0f, 0.0f };
                newKey.sensitivity = 0.05f;
                img.colorKeys.push_back(newKey);
                g_configIsDirty = true;
            }

            ImGui::EndDisabled();

            ImGui::SeparatorText("Border");
            if (ImGui::Checkbox("Enable Border##Image", &img.border.enabled)) { g_configIsDirty = true; }
            ImGui::SameLine();
            HelpMarker("Draw a border around the image overlay.");

            if (img.border.enabled) {
                ImGui::Text("Color:");
                ImVec4 borderCol = ImVec4(img.border.color.r, img.border.color.g, img.border.color.b, 1.0f);
                if (ImGui::ColorEdit3("##BorderColorImage", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                    img.border.color = { borderCol.x, borderCol.y, borderCol.z };
                    g_configIsDirty = true;
                }

                ImGui::Text("Width:");
                ImGui::SetNextItemWidth(100);
                if (Spinner("##BorderWidthImage", &img.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                ImGui::SameLine();
                ImGui::TextDisabled("px");

                ImGui::Text("Corner Radius:");
                ImGui::SetNextItemWidth(100);
                if (Spinner("##BorderRadiusImage", &img.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                ImGui::SameLine();
                ImGui::TextDisabled("px");
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (image_to_remove != -1) {
        std::string deletedImageName = g_config.images[image_to_remove].name;
        g_config.images.erase(g_config.images.begin() + image_to_remove);
        // Remove deleted image from all modes
        for (auto& mode : g_config.modes) {
            auto it = std::find(mode.imageIds.begin(), mode.imageIds.end(), deletedImageName);
            while (it != mode.imageIds.end()) {
                mode.imageIds.erase(it);
                it = std::find(mode.imageIds.begin(), mode.imageIds.end(), deletedImageName);
            }
        }
        g_configIsDirty = true;
    }

    ImGui::Separator();
    if (ImGui::Button("Add New Image")) {
        ImageConfig newImg;
        newImg.name = "New Image " + std::to_string(g_config.images.size() + 1);
        newImg.relativeTo = "centerViewport";
        g_config.images.push_back(newImg);
        g_configIsDirty = true;

        // Automatically add to currently selected mode
        if (!g_currentModeId.empty()) {
            for (auto& mode : g_config.modes) {
                if (mode.id == g_currentModeId) {
                    // Check if not already added
                    if (std::find(mode.imageIds.begin(), mode.imageIds.end(), newImg.name) == mode.imageIds.end()) {
                        mode.imageIds.push_back(newImg.name);
                    }
                    break;
                }
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults##images")) { ImGui::OpenPopup("Reset Images to Defaults?"); }

    if (ImGui::BeginPopupModal("Reset Images to Defaults?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "WARNING:");
        ImGui::Text("This will delete ALL custom images and restore the default images.");
        ImGui::Text("This action cannot be undone.");
        ImGui::Separator();
        if (ImGui::Button("Confirm Reset", ImVec2(120, 0))) {
            g_config.images = GetDefaultImages();
            g_configIsDirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::EndTabItem();
}
