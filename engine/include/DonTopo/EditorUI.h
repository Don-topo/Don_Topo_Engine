#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <filesystem>
#include <functional>
#include <glm/glm.hpp>

namespace DonTopo {

class GameObject;
class PhysicsManager;
class BoxCollider;

class EditorUI {
public:
    EditorUI()                           = default;
    EditorUI(const EditorUI&)            = delete;
    EditorUI& operator=(const EditorUI&) = delete;

    void draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView);

    bool isViewportHovered() const { return m_viewportHovered; }

    // Notificado justo antes de desenganchar node de su padre (node sigue
    // siendo válido y su subárbol completo también), para que el dueño
    // pueda liberar recursos externos (meshes/texturas en GPU) asociados.
    void setOnDelete(std::function<void(GameObject*)> cb) { m_onDelete = std::move(cb); }
    // Llamado con el eje mundo (1,0,0 / 0,1,0 / 0,0,1) al clicar la bola del axis gizmo.
    void setOnAxisSelected(std::function<void(const glm::vec3&)> cb) { m_onAxisSelected = std::move(cb); }
    // Puntero no-propietario: PhysicsManager vive en main.cpp, fuera del
    // ciclo de vida del EditorUI. Necesario para crear el actor PhysX al
    // pulsar "Add > Box Collider" desde el panel Properties.
    void setPhysicsManager(PhysicsManager* physics) { m_physics = physics; }

private:
    void drawDockSpace();
    void drawScene(GameObject* sceneRoot);
    void drawSceneNode(GameObject* node);
    // Abre el popup de rename pa node (root, parent==nullptr, no se puede renombrar).
    void beginRename(GameObject* node);
    void drawViewport(VkDescriptorSet viewportTexture, const glm::mat4& cameraView);
    void drawProperties();
    void drawBoxColliderSection();
    void drawAddComponentButton();
    void drawContentBrowser();

    // Viewport
    bool m_viewportHovered = false;

    // Content Browser
    bool m_dlgOpen = false;
    bool m_scanned = false;
    std::string m_currentDir;
    std::vector<std::filesystem::path> m_assets;

    // Scene selection
    GameObject* m_selected = nullptr;
    // Borrado diferido al final del frame: el árbol se recorre con
    // recursión sobre std::vector<unique_ptr<GameObject>>, borrar en medio
    // de esa recursión invalidaría los iteradores de los for-range activos.
    GameObject* m_pendingDelete = nullptr;
    std::function<void(GameObject*)> m_onDelete;
    std::function<void(const glm::vec3&)> m_onAxisSelected;

    // Reorder por drag&drop, diferido al final del frame por la misma razón
    // que m_pendingDelete: mutar children en medio de la recursión del árbol
    // invalidaría el for-range de un ancestro que sigue en la pila.
    GameObject* m_pendingMoveSource = nullptr;
    GameObject* m_pendingMoveTarget = nullptr;

    // Rename – popup modal disparado por "Rename" (click derecho) o F2.
    GameObject* m_renameTarget = nullptr;
    char        m_renameBuffer[128] = {};
    bool        m_openRenamePopup = false;

    // Properties – cache de edición del nodo seleccionado (persiste entre
    // frames para que DragFloat pueda acumular el delta del arrastre;
    // solo se re-sincroniza con localTransform al cambiar de selección).
    GameObject* m_propsCachedFor = nullptr;
    glm::vec3   m_editPosition{0.0f};
    glm::vec3   m_editRotationDeg{0.0f};
    glm::vec3   m_editScale{1.0f};
    // true si el frame anterior el usuario tenía el mouse presionado sobre
    // algún DragFloat de Position/Rotation (evita que el refresco en vivo de
    // RigidBody pelee con el drag).
    bool        m_transformDragActive = false;

    // Box Collider – mismo patrón de cache que Transform: persiste entre
    // frames para que los DragFloat acumulen el delta del arrastre, y se
    // resincroniza con el BoxCollider real al cambiar de selección o (si es
    // dinámico y no se está arrastrando) cada frame para reflejar cambios
    // externos de tamaño/gravedad.
    BoxCollider* m_colliderCachedFor = nullptr;
    glm::vec3    m_editColliderCenter{0.0f};
    glm::vec3    m_editColliderSize{50.0f};
    bool         m_editUseGravity = false;
    bool         m_colliderDragActive = false;

    PhysicsManager* m_physics = nullptr;
};

} // namespace DonTopo
