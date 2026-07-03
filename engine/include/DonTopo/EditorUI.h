#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <filesystem>

namespace DonTopo {

class EditorUI {
public:
    EditorUI()                           = default;
    EditorUI(const EditorUI&)            = delete;
    EditorUI& operator=(const EditorUI&) = delete;

    void draw(VkDescriptorSet viewportTexture,
              const std::vector<std::string>& staticNames,
              const std::vector<std::string>& skinnedNames);

    bool isViewportHovered() const { return m_viewportHovered; }

private:
    void drawDockSpace();
    void drawScene(const std::vector<std::string>& staticNames,
                   const std::vector<std::string>& skinnedNames);
    void drawViewport(VkDescriptorSet viewportTexture);
    void drawProperties();
    void drawContentBrowser();

    // Viewport
    bool m_viewportHovered = false;

    // Content Browser
    bool m_dlgOpen = false;
    bool m_scanned = false;
    std::vector<std::filesystem::path> m_assets;

    // Properties – Transform
    float m_pos[3] = {0.f, 0.f, 0.f};
    float m_rot[3] = {0.f, 0.f, 0.f};
    float m_scl[3] = {1.f, 1.f, 1.f};
};

} // namespace DonTopo
