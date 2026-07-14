#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace DonTopo {

class GameObject;
class Camera;
struct EditorContext;

// Ventana "Viewport" — render 3D embebido (textura del Renderer) + gizmo de
// ejes/wireframe de collider sobre la selección activa.
class ViewportPanel {
public:
    void draw(EditorContext& ctx, VkDescriptorSet viewportTexture, const glm::mat4& cameraView);
    // Centra la cámara en ctx.selected (no-op si no hay selección). Usado
    // por el atajo de teclado "F" en main.cpp vía EditorUI::focusSelected.
    void focusSelected(EditorContext& ctx, Camera& camera);
    bool isHovered() const { return m_hovered; }
    bool* GetOpenPtr() { return &m_open; }

private:
    void drawSelectionGizmo(EditorContext& ctx);
    // Longitud de eje proporcional al bbox local del mesh de node (mitad
    // del eje más largo); si node no tiene mesh (o el mesh no tiene
    // vértices), valor fijo de repliegue.
    float selectionAxisScale(GameObject* node) const;

    bool m_open = true;
    bool m_hovered = false;
};

} // namespace DonTopo
