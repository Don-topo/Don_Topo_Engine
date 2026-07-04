#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <filesystem>

namespace DonTopo {

class GameObject;

class EditorUI {
public:
    EditorUI()                           = default;
    EditorUI(const EditorUI&)            = delete;
    EditorUI& operator=(const EditorUI&) = delete;

    void draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot);

    bool isViewportHovered() const { return m_viewportHovered; }

private:
    void drawDockSpace();
    void drawScene(GameObject* sceneRoot);
    void drawSceneNode(GameObject* node);
    void drawViewport(VkDescriptorSet viewportTexture);
    void drawProperties();
    void drawContentBrowser();

    // Viewport
    bool m_viewportHovered = false;

    // Content Browser
    bool m_dlgOpen = false;
    bool m_scanned = false;
    std::vector<std::filesystem::path> m_assets;

    // Scene selection
    GameObject* m_selected = nullptr;
};

} // namespace DonTopo
