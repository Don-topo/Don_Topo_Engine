#pragma once
#include <memory>
#include <string>
#include <glm/glm.hpp>
#include "DonTopo/Editor/UndoManager.h" // BoxColliderState, SphereColliderState, CapsuleColliderState, PlaneColliderState

namespace IGFD { class FileDialog; }

namespace DonTopo {

class GameObject;
class BoxCollider;
class SphereCollider;
class CapsuleCollider;
class PlaneCollider;
class Rigidbody;
struct EditorContext;

// Ventana "Properties" — transform, colliders (Box/Sphere/Capsule/Plane),
// Mesh, Audio Clip, Scripts y el botón "Add" con su popup "Nuevo Script".
class PropertiesPanel {
public:
    PropertiesPanel();
    ~PropertiesPanel();
    PropertiesPanel(const PropertiesPanel&) = delete;
    PropertiesPanel& operator=(const PropertiesPanel&) = delete;

    void draw(EditorContext& ctx);
    bool* GetOpenPtr() { return &m_open; }
    // Invalida los caches de Transform y Box Collider cuando ScenePanel borra
    // el nodo seleccionado (llamado desde EditorUI::draw() cuando
    // ScenePanel::selectionWasDeletedThisFrame() es true), para no arrastrar
    // punteros colgantes (GameObject/BoxCollider ya liberados) hasta la
    // próxima selección real. Mismo alcance que el fix de Task 3 (solo
    // Transform + Box Collider, no Sphere/Capsule/Plane — esos se
    // resincronizan solos vía sus propios cachedFor al perder el collider).
    void invalidateCaches();

private:
    void drawBoxColliderSection(EditorContext& ctx);
    void drawSphereColliderSection(EditorContext& ctx);
    void drawCapsuleColliderSection(EditorContext& ctx);
    void drawPlaneColliderSection(EditorContext& ctx);
    void drawRigidbodySection(EditorContext& ctx);
    void drawMeshSection(EditorContext& ctx);
    void drawMeshDialog(EditorContext& ctx);
    void drawAudioClipSection(EditorContext& ctx);
    void drawAudioClipDialog(EditorContext& ctx);
    void drawScriptsSection(EditorContext& ctx);
    void drawAddComponentButton(EditorContext& ctx);
    void drawNewScriptPopup(EditorContext& ctx);
    void loadMeshForSelected(EditorContext& ctx, const std::string& path);
    void loadAudioClipForSelected(EditorContext& ctx, const std::string& path);

    bool m_open = true;

    // Properties – cache de edición del nodo seleccionado (persiste entre
    // frames para que DragFloat pueda acumular el delta del arrastre; solo
    // se re-sincroniza con localTransform al cambiar de selección).
    GameObject* m_propsCachedFor = nullptr;
    glm::vec3   m_editPosition{0.0f};
    glm::vec3   m_editRotationDeg{0.0f};
    glm::vec3   m_editScale{1.0f};
    // true si el frame anterior el usuario tenía el mouse presionado sobre
    // algún DragFloat de Position/Rotation/Scale (evita que el refresco en
    // vivo de BoxCollider dinámico pelee con el drag, y delimita la sesión de
    // edición pa el snapshot de Undo de abajo).
    bool        m_transformDragActive = false;
    // Snapshot de localTransform tomado al iniciar un drag de Position/
    // Rotation/Scale (primer IsItemActivated de la sesión) — "before" del
    // PropertyCommand<glm::mat4> que se empuja al confirmar (commit).
    glm::mat4   m_transformBeforeEdit{1.0f};

    // Box Collider – mismo patrón de cache que Transform: persiste entre
    // frames para que los DragFloat acumulen el delta del arrastre, y se
    // resincroniza con el BoxCollider real al cambiar de selección o (si es
    // dinámico y no se está arrastrando) cada frame para reflejar cambios
    // externos de tamaño/gravedad.
    BoxCollider* m_colliderCachedFor = nullptr;
    glm::vec3    m_editColliderCenter{0.0f};
    glm::vec3    m_editColliderSize{50.0f};
    bool         m_editIsTrigger = false;
    bool         m_colliderDragActive = false;
    // Snapshot tomado al iniciar un drag de Center/Size — "before" del
    // PropertyCommand<BoxColliderState> que se empuja al confirmar.
    BoxColliderState m_boxColliderBeforeEdit{};

    // Sphere Collider – mismo patrón de cache que Box Collider.
    SphereCollider* m_sphereColliderCachedFor = nullptr;
    glm::vec3       m_editSphereCenter{0.0f};
    float           m_editSphereRadius{25.0f};
    bool            m_editSphereIsTrigger = false;
    bool            m_sphereColliderDragActive = false;
    SphereColliderState m_sphereColliderBeforeEdit{};

    // Capsule Collider – mismo patrón de cache que Box Collider.
    CapsuleCollider* m_capsuleColliderCachedFor = nullptr;
    glm::vec3        m_editCapsuleCenter{0.0f};
    float            m_editCapsuleRadius{15.0f};
    float            m_editCapsuleHeight{50.0f};
    bool             m_editCapsuleIsTrigger = false;
    bool             m_capsuleColliderDragActive = false;
    CapsuleColliderState m_capsuleColliderBeforeEdit{};

    // Plane Collider – solo Center (sin Size/Use Gravity, siempre estático).
    PlaneCollider* m_planeColliderCachedFor = nullptr;
    glm::vec3      m_editPlaneCenter{0.0f};
    bool           m_editPlaneIsTrigger = false;
    bool           m_planeColliderDragActive = false;
    PlaneColliderState m_planeColliderBeforeEdit{};

    // Rigidbody – mismo patrón de cache que los colliders. Los DragFloat
    // (mass/drag/angularDrag) usan begin/commit con m_rigidbodyBeforeEdit para
    // empujar un único PropertyCommand<RigidbodyState> al soltar; los checkbox
    // (gravity/kinematic/constraints) empujan comando inmediato.
    const void*    m_rigidbodyCachedFor = nullptr;
    float          m_editRbMass = 1.0f;
    bool           m_editRbUseGravity = true;
    bool           m_editRbKinematic = false;
    float          m_editRbDrag = 0.0f;
    float          m_editRbAngularDrag = 0.05f;
    uint32_t       m_editRbConstraints = 0;
    bool           m_rigidbodyDragActive = false;
    RigidbodyState m_rigidbodyBeforeEdit{};

    // Instancia propia de ImGuiFileDialog para "Add > Mesh", separada de
    // m_audioFileDialog: la librería documenta que una única instancia
    // compartida (p.ej. el singleton IGFD::FileDialog::Instance()) no
    // soporta 2 diálogos concurrentes (mismo estado interno de lista de
    // ficheros/thumbnails/columnas), y los diálogos de Mesh y Audio pueden
    // estar abiertos a la vez; compartir instancia causaba corrupción al
    // redimensionar el popup de uno mientras el otro seguía abierto el mismo
    // frame. unique_ptr porque IGFD::FileDialog es tipo incompleto aquí.
    bool m_meshDlgOpen = false;
    std::unique_ptr<IGFD::FileDialog> m_meshFileDialog;
    // Mensaje del último intento fallido de carga de Mesh (vacío si no hay
    // error pendiente); se limpia al cambiar de selección o al cargar bien.
    std::string m_meshLoadError;
    // GameObject para el que se pulsó "Add > Mesh" (revela la sección
    // Browse/drop hasta que se asigne un mesh o se pulse "x" para quitarlo).
    // nullptr = sección oculta. No se limpia al cambiar de selección: si el
    // usuario vuelve al mismo GameObject sin haber completado la carga, la
    // sección sigue visible (igual que dejar un diálogo de collider a medias).
    GameObject* m_meshAddRequestedFor = nullptr;

    // Misma razón que m_meshFileDialog: instancia propia, nunca compartida
    // con m_meshFileDialog.
    bool m_audioDlgOpen = false;
    std::unique_ptr<IGFD::FileDialog> m_audioFileDialog;
    // Mismo patrón que m_meshLoadError/m_meshAddRequestedFor pero para el
    // componente AudioClip.
    std::string m_audioLoadError;
    GameObject* m_audioClipAddRequestedFor = nullptr;

    // Popup "Nuevo Script" — disparado desde Add > Script > Nuevo Script...
    // m_newScriptTarget se captura al abrir (ctx.selected puede cambiar con
    // el popup abierto) y se revalida contra la escena antes de añadir.
    bool        m_openNewScriptPopup = false;
    char        m_newScriptNameBuffer[64] = {};
    std::string m_newScriptError;
    GameObject* m_newScriptTarget = nullptr;
};

} // namespace DonTopo
