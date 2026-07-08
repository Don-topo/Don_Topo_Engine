#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <filesystem>
#include <functional>
#include <memory>
#include <glm/glm.hpp>

namespace DonTopo {

class GameObject;
class Mesh;
class PhysicsManager;
class BoxCollider;
class SphereCollider;
class CapsuleCollider;
class PlaneCollider;
class Renderer;
class Camera;

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
    // Puntero no-propietario: Renderer es dueño de este EditorUI y se pasa a sí
    // mismo desde setSceneRoot. Necesario para registrar el mesh GPU (addStaticMesh)
    // al crear un shape desde el menú "Basic Shapes".
    void setRenderer(Renderer* renderer) { m_renderer = renderer; }
    // Centra la cámara en m_selected (no-op si no hay selección). Usado por
    // el atajo de teclado "F" en main.cpp.
    void focusSelected(Camera& camera);

private:
    void drawDockSpace();
    void drawScene(GameObject* sceneRoot);
    void drawSceneNode(GameObject* node);
    // Dibuja los ejes RGB de Gizmos sobre m_selected (si hay selección),
    // cada frame — desaparecen solos cuando m_selected pasa a nullptr.
    void drawSelectionGizmo();
    // Longitud de eje proporcional al bbox local del mesh de node (mitad
    // del eje más largo); si node no tiene mesh (o el mesh no tiene
    // vértices), valor fijo de repliegue.
    float selectionAxisScale(GameObject* node) const;
    // Abre el popup de rename pa node (root, parent==nullptr, no se puede renombrar).
    void beginRename(GameObject* node);
    void drawViewport(VkDescriptorSet viewportTexture, const glm::mat4& cameraView);
    void drawProperties();
    void drawBoxColliderSection();
    void drawSphereColliderSection();
    void drawCapsuleColliderSection();
    void drawPlaneColliderSection();
    void drawAddComponentButton();
    // Crea un GameObject hijo de parent con el mesh dado, lo registra en el
    // Renderer (staticRenderIndex) y lo deja sin collider. No-op si parent o
    // m_renderer son nullptr.
    void createBasicShape(GameObject* parent, const std::string& name, std::shared_ptr<Mesh> mesh);
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
    // BoxCollider dinámico pelee con el drag).
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

    // Sphere Collider – mismo patrón de cache que Box Collider.
    SphereCollider* m_sphereColliderCachedFor = nullptr;
    glm::vec3       m_editSphereCenter{0.0f};
    float           m_editSphereRadius{25.0f};
    bool            m_editSphereUseGravity = false;
    bool            m_sphereColliderDragActive = false;

    // Capsule Collider – mismo patrón de cache que Box Collider.
    CapsuleCollider* m_capsuleColliderCachedFor = nullptr;
    glm::vec3        m_editCapsuleCenter{0.0f};
    float            m_editCapsuleRadius{15.0f};
    float            m_editCapsuleHeight{50.0f};
    bool             m_editCapsuleUseGravity = false;
    bool             m_capsuleColliderDragActive = false;

    // Plane Collider – solo Center (sin Size/Use Gravity, siempre estático).
    PlaneCollider* m_planeColliderCachedFor = nullptr;
    glm::vec3      m_editPlaneCenter{0.0f};
    bool           m_planeColliderDragActive = false;

    PhysicsManager* m_physics = nullptr;
    Renderer*       m_renderer = nullptr;
};

} // namespace DonTopo
