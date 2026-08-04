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
    // Área de imagen del panel en píxeles, la del último draw(). El Renderer
    // renderiza EXACTAMENTE a este tamaño: si renderizara al de la ventana,
    // ImGui reescalaría la imagen al dibujarla y ese filtrado bilineal se
    // comería el escalonado (y con él la diferencia entre modos de
    // anti-aliasing), además de deformar la escena cuando el aspect del panel
    // no coincide con el de la ventana. (0,0) mientras el panel esté cerrado.
    uint32_t contentWidth()  const { return m_contentWidth; }
    uint32_t contentHeight() const { return m_contentHeight; }

private:
    void drawSelectionGizmo(EditorContext& ctx);
    // Wireframe del frustum de la cámara de la escena, siempre visible en
    // edición (no solo al seleccionarla). Solo el frustum: los ejes del
    // transform ya los dibuja drawSelectionGizmo al seleccionar cualquier
    // objeto, y repetirlos aquí daría dos juegos de ejes superpuestos de
    // distinta longitud.
    void drawCameraGizmo(EditorContext& ctx);
    // Gizmo de TODAS las luces de la escena (no solo la seleccionada), en
    // edición y en Play. Vive en el editor a propósito: es lo que garantiza que
    // no salga en el juego exportado, que no compila este panel.
    void drawLightGizmos(EditorContext& ctx);
    // Longitud de eje proporcional al bbox local del mesh de node (mitad
    // del eje más largo); si node no tiene mesh (o el mesh no tiene
    // vértices), valor fijo de repliegue.
    float selectionAxisScale(GameObject* node) const;
    // Picking por rayo en CPU: desproyecta mousePx (píxeles RELATIVOS a la
    // esquina superior izquierda de la imagen del viewport, no de la ventana
    // ImGui) con la cámara del frame —la de vuelo del editor o la de la escena
    // en Play— y devuelve el objeto con malla cuya esfera envolvente corta el
    // rayo más cerca de la cámara. nullptr si no corta ninguna.
    GameObject* pickObject(EditorContext& ctx, const glm::mat4& cameraView,
                           const glm::vec2& mousePx, const glm::vec2& imageSize) const;

    bool m_open = true;
    bool m_hovered = false;
    uint32_t m_contentWidth  = 0;
    uint32_t m_contentHeight = 0;
};

} // namespace DonTopo
