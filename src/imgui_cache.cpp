#include "imgui_cache.h"
#include <cstring>

// Global cache instance
ImGuiDrawDataCache g_imguiCache;

ImGuiDrawDataCache::ImGuiDrawDataCache() {
    m_lastUpdateTime = std::chrono::steady_clock::now();
}

ImGuiDrawDataCache::~ImGuiDrawDataCache() {
    Clear();
}

void ImGuiDrawDataCache::Clear() {
    // Free all owned draw lists
    for (ImDrawList* list : m_ownedDrawLists) {
        IM_DELETE(list);
    }
    m_ownedDrawLists.clear();
    m_cachedDrawData.Clear();
    m_valid = false;
}

ImDrawList* ImGuiDrawDataCache::CloneDrawList(const ImDrawList* src) {
    if (!src) return nullptr;
    
    ImDrawList* dst = IM_NEW(ImDrawList)(src->_Data);
    
    // Copy command buffer - contains all draw commands
    dst->CmdBuffer.resize(src->CmdBuffer.Size);
    if (src->CmdBuffer.Size > 0) {
        memcpy(dst->CmdBuffer.Data, src->CmdBuffer.Data, src->CmdBuffer.Size * sizeof(ImDrawCmd));
    }
    
    // Copy index buffer - contains triangle indices
    dst->IdxBuffer.resize(src->IdxBuffer.Size);
    if (src->IdxBuffer.Size > 0) {
        memcpy(dst->IdxBuffer.Data, src->IdxBuffer.Data, src->IdxBuffer.Size * sizeof(ImDrawIdx));
    }
    
    // Copy vertex buffer - contains vertex positions, UVs, colors
    dst->VtxBuffer.resize(src->VtxBuffer.Size);
    if (src->VtxBuffer.Size > 0) {
        memcpy(dst->VtxBuffer.Data, src->VtxBuffer.Data, src->VtxBuffer.Size * sizeof(ImDrawVert));
    }
    
    // Copy flags - affects rendering behavior
    dst->Flags = src->Flags;
    
    // Note: We only copy the essential buffers needed for rendering.
    // Internal state like _VtxWritePtr, _ClipRectStack etc. are not needed
    // since we're not building new commands, just rendering cached ones.
    
    return dst;
}

void ImGuiDrawDataCache::CacheFromCurrent() {
    ImDrawData* src = ImGui::GetDrawData();
    if (!src || !src->Valid) {
        Clear();
        return;
    }
    
    // Clear previous cache
    Clear();
    
    // Copy scalar members
    m_cachedDrawData.Valid = src->Valid;
    m_cachedDrawData.CmdListsCount = src->CmdListsCount;
    m_cachedDrawData.TotalIdxCount = src->TotalIdxCount;
    m_cachedDrawData.TotalVtxCount = src->TotalVtxCount;
    m_cachedDrawData.DisplayPos = src->DisplayPos;
    m_cachedDrawData.DisplaySize = src->DisplaySize;
    m_cachedDrawData.FramebufferScale = src->FramebufferScale;
    m_cachedDrawData.OwnerViewport = src->OwnerViewport;
    
    // Deep copy all draw lists
    m_ownedDrawLists.reserve(src->CmdListsCount);
    for (int i = 0; i < src->CmdListsCount; i++) {
        ImDrawList* cloned = CloneDrawList(src->CmdLists[i]);
        if (cloned) {
            m_ownedDrawLists.push_back(cloned);
            m_cachedDrawData.CmdLists.push_back(cloned);
        }
    }
    
    // Update counts to match actual cloned lists
    m_cachedDrawData.CmdListsCount = static_cast<int>(m_cachedDrawData.CmdLists.Size);
    
    m_valid = true;
}

ImDrawData* ImGuiDrawDataCache::GetCachedDrawData() {
    if (!m_valid) return nullptr;
    return &m_cachedDrawData;
}

bool ImGuiDrawDataCache::ShouldUpdate() const {
    if (m_forceUpdate) return true;
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastUpdateTime).count();
    return elapsed >= UPDATE_INTERVAL_MS;
}

void ImGuiDrawDataCache::MarkUpdated() {
    m_lastUpdateTime = std::chrono::steady_clock::now();
    m_forceUpdate = false;
}

void ImGuiDrawDataCache::Invalidate() {
    m_forceUpdate = true;
}
