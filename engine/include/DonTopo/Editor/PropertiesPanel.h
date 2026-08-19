#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <glm/glm.hpp>
#include "DonTopo/Editor/UndoManager.h" // BoxColliderState, SphereColliderState, CapsuleColliderState, PlaneColliderState
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/GameObject.h" // uiComponentsAvailable necesita el tipo completo

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

    // Tira la caché de nombres de sprite. La llama quien toque un sidecar por
    // fuera (el editor de sprites): sin esto los combos siguen enseñando la
    // lista con la que se abrió la sección.
    void invalidateSpriteNames() { m_spriteNamesValid = false; }
    bool* GetOpenPtr() { return &m_open; }
    // Olvida TODO lo que las secciones tengan cacheado. Dos llamantes:
    //   - ScenePanel borró el nodo seleccionado: los punteros apuntan a
    //     componentes ya liberados y no pueden sobrevivir al frame.
    //   - Undo/Redo: mutan los componentes en sitio, así que la sección tiene
    //     que volver a leerlos o se queda enseñando el valor deshecho.
    void invalidateCaches();

    // Un GameObject solo ofrece los componentes de UI si YA tiene Canvas: el
    // Canvas es la raíz de la que cuelgan. Es la única fuente de verdad del
    // gate (la usa el popup "Add"), y está aquí y no dentro del ImGui pa que
    // se pueda probar sin GUI.
    // Vale el Canvas del propio GameObject o el de CUALQUIER ancestro: un botón
    // cuelga normalmente del Canvas, no es el Canvas.
    static bool uiComponentsAvailable(const GameObject* go)
    {
        for (const GameObject* n = go; n; n = n->parent)
            if (n->hasCanvas()) return true;
        return false;
    }

    // Qué acepta cada caja de asset del Button: las de fuente son las que abre
    // FreeType (UiFont::loadFromFile) y las de atlas las que lee stb_image
    // (UiTextureAtlas::loadFromFile). Aquí y no dentro del ImGui para poder
    // probarlas sin GUI, igual que uiComponentsAvailable. La comparación es en
    // minúsculas: del content browser puede llegar un ".PNG".
    static bool isUiFontPath(const std::string& path)
    {
        static const char* const kExts[] = { ".ttf", ".otf", ".ttc" };
        return hasExtension(path, kExts, sizeof(kExts) / sizeof(kExts[0]));
    }

    static bool isUiAtlasPath(const std::string& path)
    {
        static const char* const kExts[] = { ".png", ".jpg", ".jpeg", ".bmp", ".tga" };
        return hasExtension(path, kExts, sizeof(kExts) / sizeof(kExts[0]));
    }

private:
    static bool hasExtension(const std::string& path, const char* const* exts, size_t count)
    {
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos) return false;
        // Un punto que quede ANTES del último separador es de un directorio
        // ("C:/mis.cosas/fuente"), no una extensión.
        const size_t sep = path.find_last_of("/\\");
        if (sep != std::string::npos && dot < sep) return false;
        std::string ext = path.substr(dot);
        for (char& c : ext)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        for (size_t i = 0; i < count; i++)
            if (ext == exts[i]) return true;
        return false;
    }

    void drawBoxColliderSection(EditorContext& ctx);
    void drawSphereColliderSection(EditorContext& ctx);
    void drawCapsuleColliderSection(EditorContext& ctx);
    void drawPlaneColliderSection(EditorContext& ctx);
    void drawRigidbodySection(EditorContext& ctx);
    void drawCameraSection(EditorContext& ctx);
    void drawAnimatorSection(EditorContext& ctx);
    void drawMeshSection(EditorContext& ctx);
    // Screen Space Reflections del objeto. No es un componente y no pasa por
    // "Add": son dos campos del GameObject (como el transform), así que la
    // sección aparece sobre cualquier objeto con malla.
    void drawSsrSection(EditorContext& ctx);
    // Reflection Probe. SÍ es un componente y SÍ pasa por "Add": la sección se
    // esconde hasta que el usuario lo añade, igual que los colliders.
    void drawReflectionProbeSection(EditorContext& ctx);
    // Light. También componente y también tras "Add": la sección no existe
    // hasta que el usuario la añade, igual que los colliders.
    void drawLightSection(EditorContext& ctx);
    void drawMeshDialog(EditorContext& ctx);
    void drawAudioClipSection(EditorContext& ctx);
    // Audio Listener: sección mínima (solo Enabled y quitar). Como mucho uno por
    // escena — el gate de unicidad está en el popup "Add", contra
    // Scene::findAudioListener.
    void drawAudioListenerSection(EditorContext& ctx);
    // Canvas: raíz de la UI 2D. Sección tras "Add" como los colliders, con los
    // 10 campos de resolución que resuelve UiCanvas.
    void drawCanvasSection(EditorContext& ctx);
    void drawButtonSection(EditorContext& ctx);
    // Drena los file dialogs de las rutas del Button. Fuera de la sección y sin
    // condicionar a la selección, igual que drawMeshDialog.
    void drawButtonPathDialogs(EditorContext& ctx);
    // Escribe una ruta de asset del Button (fuente o atlas) resolviendo el
    // GameObject por id, validando la extensión y dejando el cambio en el stack
    // de undo. Es el punto único por el que pasan el drop, el file dialog y
    // cualquier otro origen futuro.
    void setButtonAssetPath(EditorContext& ctx, uint64_t ownerId, bool isFont,
                             const std::string& path);
    // Nombres de sprite del atlas de una ruta, para poder ELEGIRLOS en vez de
    // escribirlos a ciegas. Se consultan al renderer (que cachea el atlas por
    // ruta) y se guardan aquí: sin esta caché, una ruta que no existe se
    // intentaría abrir en cada frame que la sección esté visible.
    //
    // La lista se refresca sola al cambiar de ruta. Quien toque el sidecar por
    // fuera —el editor de sprites— tiene que llamar a invalidateSpriteNames().
    const std::vector<std::string>& spriteNamesFor(EditorContext& ctx, const std::string& atlasPath);

    std::string              m_spriteNamesPath;      // ruta de la que salió la lista
    std::vector<std::string> m_spriteNames;
    bool                     m_spriteNamesValid = false;
    // Text: etiqueta de la UI 2D. Sección tras "Add" como el Button, con el
    // mismo rect y TODOS los campos de Text (contorno, sombra, wrap y overflow).
    void drawTextSection(EditorContext& ctx);
    // Drena el file dialog de la fuente del Text. Fuera de la sección y sin
    // condicionar a la selección, por el mismo motivo que drawButtonPathDialogs.
    void drawTextPathDialog(EditorContext& ctx);
    // Escribe la ruta de la fuente del Text resolviendo el GameObject por id,
    // validando la extensión y dejando el cambio en el stack de undo. Punto
    // único por el que pasan el drop y el file dialog.
    void setTextFontPath(EditorContext& ctx, uint64_t ownerId, const std::string& path);
    // ProgressBar: barra de progreso de la UI 2D. Sección tras "Add" como el
    // Button y el Text, con el mismo rect, el valor y su rango, los dos colores
    // y los dos sprites (del MISMO atlas).
    void drawProgressBarSection(EditorContext& ctx);
    // Drena el file dialog del atlas de la ProgressBar. Fuera de la sección y
    // sin condicionar a la selección, por el mismo motivo que los del Button.
    void drawProgressBarPathDialog(EditorContext& ctx);
    // Escribe UNA de las tres rutas de imagen de la ProgressBar (field: 0 atlas,
    // 1 fondo, 2 relleno) resolviendo el GameObject por id, validando la
    // extensión y dejando el cambio en el stack de undo. Punto único por el que
    // pasan el drop y el file dialog de las tres cajas.
    void setProgressBarImagePath(EditorContext& ctx, uint64_t ownerId, int field,
                                  const std::string& path);
    void drawAudioClipDialog(EditorContext& ctx);
    void drawScriptsSection(EditorContext& ctx);
    void drawAddComponentButton(EditorContext& ctx);
    void drawNewScriptPopup(EditorContext& ctx);
    void loadMeshForSelected(EditorContext& ctx, const std::string& path);
    void loadAudioClipForSelected(EditorContext& ctx, const std::string& path);

    bool m_open = true;

    // A QUIÉN pertenece lo que cada sección tiene cacheado. Cada sección
    // re-sincroniza sus campos de edición cuando esto deja de coincidir con el
    // componente que está pintando.
    //
    // Van TODOS juntos en una struct a propósito: invalidateCaches() los borra
    // de una sentencia (`m_caches = {}`), así que una sección nueva queda
    // cubierta por el hecho de declarar su puntero aquí. Enumerándolos a mano
    // esta función se quedó corta CUATRO veces —sphere, capsule, plane y
    // rigidbody—, y el síntoma es de los que no cantan: el Undo cambia el
    // componente, la sección sigue enseñando el valor viejo, y el próximo drag
    // de otro campo lo reaplica y resucita lo que se acababa de deshacer.
    struct EditCaches
    {
        GameObject*      props     = nullptr;
        BoxCollider*     box       = nullptr;
        SphereCollider*  sphere    = nullptr;
        CapsuleCollider* capsule   = nullptr;
        PlaneCollider*   plane     = nullptr;
        const void*      rigidbody = nullptr;
        const void*      camera    = nullptr;
    };
    EditCaches m_caches;

    // Properties – cache de edición del nodo seleccionado (persiste entre
    // frames para que DragFloat pueda acumular el delta del arrastre; solo
    // se re-sincroniza con localTransform al cambiar de selección).
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

    // Reflection Probe – drag de Radius/Intensity. Mismo patrón que SSR: el
    // "before" se toma en IsItemActivated y el owner id evita aplicar un
    // "before" ajeno si el drag se interrumpió sin commit.
    bool     m_probeDragActive   = false;
    uint64_t m_probeDragOwnerId  = 0;
    float    m_probeDragBefore   = 0.0f;
    // Cuál de los dos sliders está en drag (el "before" es un único float).
    bool     m_probeDragIsRadius = false;

    // Light – drag de los sliders (intensity/range/ángulos/tamaño del area).
    // Mismo patrón que la sonda, pero con el campo en drag identificado por su
    // etiqueta: son seis sliders y un bool no llega.
    bool        m_lightDragActive  = false;
    uint64_t    m_lightDragOwnerId = 0;
    float       m_lightDragBefore  = 0.0f;
    const char* m_lightDragField   = nullptr;
    // El "before" del color no cabe en el float de arriba: ColorEdit3 abre un
    // popup y el commit llega frames después de tocarlo.
    glm::vec3   m_lightColorBefore {1.0f};

    // Sesión de arrastre de los campos del Canvas, mismo baile que la luz: el
    // valor de ANTES se congela en IsItemActivated y se commitea entero en
    // IsItemDeactivatedAfterEdit, así un arrastre es UN paso de undo y no
    // cientos. El campo se identifica por su etiqueta (un bool no llega pa 9).
    uint64_t    m_canvasDragOwnerId  = 0;
    const char* m_canvasDragField    = nullptr;
    float       m_canvasDragBefore   = 0.0f;
    glm::vec2   m_canvasDragBefore2 {0.0f};

    // Lo mismo para los campos del Button. Cuatro "before" porque el componente
    // tiene las cuatro familias de campo (float, vec2, color y texto) y cada
    // una commitea su propio PropertyCommand<T>.
    uint64_t    m_buttonDragOwnerId = 0;
    const char* m_buttonDragField   = nullptr;
    float       m_buttonDragBefore  = 0.0f;
    glm::vec2   m_buttonDragBefore2 {0.0f};
    glm::vec4   m_buttonDragBefore4 {1.0f};
    std::string m_buttonDragBeforeStr;

    // Y lo mismo para los campos del Text: propios y no compartidos con el
    // Button porque los dos componentes pueden estar en el MISMO GameObject, y
    // un "before" compartido mezclaría los dos arrastres.
    uint64_t    m_textDragOwnerId = 0;
    const char* m_textDragField   = nullptr;
    float       m_textDragBefore  = 0.0f;
    glm::vec2   m_textDragBefore2 {0.0f};
    glm::vec4   m_textDragBefore4 {1.0f};
    std::string m_textDragBeforeStr;

    // Y lo mismo para los campos de la ProgressBar: propios y no compartidos con
    // el Button ni con el Text, porque los TRES componentes pueden estar en el
    // MISMO GameObject y un "before" compartido mezclaría los arrastres.
    uint64_t    m_barDragOwnerId = 0;
    const char* m_barDragField   = nullptr;
    float       m_barDragBefore  = 0.0f;
    glm::vec2   m_barDragBefore2 {0.0f};
    glm::vec4   m_barDragBefore4 {1.0f};
    std::string m_barDragBeforeStr;

    // Box Collider – mismo patrón de cache que Transform: persiste entre
    // frames para que los DragFloat acumulen el delta del arrastre, y se
    // resincroniza con el BoxCollider real al cambiar de selección o (si es
    // dinámico y no se está arrastrando) cada frame para reflejar cambios
    // externos de tamaño/gravedad.
    glm::vec3    m_editColliderCenter{0.0f};
    glm::vec3    m_editColliderSize{50.0f};
    bool         m_editIsTrigger = false;
    bool         m_colliderDragActive = false;
    // Snapshot tomado al iniciar un drag de Center/Size — "before" del
    // PropertyCommand<BoxColliderState> que se empuja al confirmar.
    BoxColliderState m_boxColliderBeforeEdit{};

    // Sphere Collider – mismo patrón de cache que Box Collider.
    glm::vec3       m_editSphereCenter{0.0f};
    float           m_editSphereRadius{25.0f};
    bool            m_editSphereIsTrigger = false;
    bool            m_sphereColliderDragActive = false;
    SphereColliderState m_sphereColliderBeforeEdit{};

    // Capsule Collider – mismo patrón de cache que Box Collider.
    glm::vec3        m_editCapsuleCenter{0.0f};
    float            m_editCapsuleRadius{15.0f};
    float            m_editCapsuleHeight{50.0f};
    bool             m_editCapsuleIsTrigger = false;
    bool             m_capsuleColliderDragActive = false;
    CapsuleColliderState m_capsuleColliderBeforeEdit{};

    // Plane Collider – solo Center (sin Size/Use Gravity, siempre estático).
    glm::vec3      m_editPlaneCenter{0.0f};
    bool           m_editPlaneIsTrigger = false;
    bool           m_planeColliderDragActive = false;
    PlaneColliderState m_planeColliderBeforeEdit{};

    // Rigidbody – mismo patrón de cache que los colliders. Los DragFloat
    // (mass/drag/angularDrag) usan begin/commit con m_rigidbodyBeforeEdit para
    // empujar un único PropertyCommand<RigidbodyState> al soltar; los checkbox
    // (gravity/kinematic/constraints) empujan comando inmediato.
    float          m_editRbMass = 1.0f;
    bool           m_editRbUseGravity = true;
    bool           m_editRbKinematic = false;
    float          m_editRbDrag = 0.0f;
    float          m_editRbAngularDrag = 0.05f;
    uint32_t       m_editRbConstraints = 0;
    bool           m_rigidbodyDragActive = false;
    uint64_t       m_rigidbodyDragOwnerId = 0;
    RigidbodyState m_rigidbodyBeforeEdit{};

    // Camera – mismo patrón de cache que Rigidbody. Los DragFloat (fov/size/
    // near/far) usan begin/commit con m_cameraBeforeEdit pa empujar un único
    // PropertyCommand<CameraState> al soltar; el combo de modo empuja comando
    // inmediato.
    CameraComponent::ProjectionMode m_editCamMode = CameraComponent::ProjectionMode::Perspective;
    float       m_editCamFov = 45.0f;
    float       m_editCamOrthoSize = 100.0f;
    float       m_editCamNear = 1.0f;
    float       m_editCamFar = 2000.0f;
    bool        m_cameraDragActive = false;
    uint64_t    m_cameraDragOwnerId = 0;
    CameraState m_cameraBeforeEdit{};

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

    // Rutas del Button (fuente y atlas). Misma razón que m_meshFileDialog para
    // tener instancia propia por diálogo, nunca compartida. El id del dueño se
    // guarda al abrir: el diálogo se drena fuera de la sección y para entonces
    // la selección puede haber cambiado, así que resolver por id (y no por
    // ctx.selected) es lo que impide escribir la ruta en otro GameObject.
    bool     m_fontDlgOpen  = false;
    uint64_t m_fontDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_fontFileDialog;

    bool     m_uiAtlasDlgOpen  = false;
    uint64_t m_uiAtlasDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_uiAtlasFileDialog;

    // Último rechazo por extensión, para poder decir POR QUÉ no se aceptó el
    // fichero en vez de tragárselo en silencio. Se limpia al acertar.
    std::string m_buttonPathError;

    // Fuente del Text. Instancia de diálogo propia (nunca compartida con la del
    // Button) y su propio id de dueño, por la misma razón que las del Button.
    bool     m_textFontDlgOpen  = false;
    uint64_t m_textFontDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_textFontFileDialog;
    std::string m_textPathError;

    // Imágenes de la ProgressBar. Instancia de diálogo propia y su propio id de
    // dueño, por la misma razón que las del Button y la del Text. UNA sola
    // instancia para las tres cajas (solo puede haber un diálogo abierto a la
    // vez) más el campo que lo abrió: sin él, elegir un fichero escribiría
    // siempre en el atlas.
    bool     m_barAtlasDlgOpen  = false;
    uint64_t m_barAtlasDlgOwner = 0;
    int      m_barAtlasDlgField = 0;   // 0 atlas, 1 fondo, 2 relleno
    std::unique_ptr<IGFD::FileDialog> m_barAtlasFileDialog;
    std::string m_barPathError;
    // Mismo patrón que m_meshLoadError/m_meshAddRequestedFor pero para el
    // componente AudioClip.
    std::string m_audioLoadError;
    GameObject* m_audioClipAddRequestedFor = nullptr;

    // Snapshot al empezar el drag de los sliders de audio: un drag continuo no
    // puede empujar un comando por frame, así que se captura al activar y se
    // empuja uno solo al soltar. A diferencia de Transform/Rigidbody (que usan
    // DragFloat), aquí se usa SliderFloat: salta al valor bajo el cursor en el
    // MISMO frame en que se activa por click, así que el "before" no puede
    // releerse del componente después de dibujar el widget (ya valdría el
    // nuevo valor); por eso el .cpp hoistea las lecturas antes del slider.
    bool     m_audioDragActive = false;
    float    m_audioDragBeforeVolume = 1.0f;
    float    m_audioDragBeforePitch  = 1.0f;
    float    m_audioDragBeforeMinDistance = 1.0f;
    float    m_audioDragBeforeMaxDistance = 100.0f;
    // Dueño del snapshot en curso: si el drag se interrumpe sin commit (p.ej.
    // Ctrl+Z a mitad de arrastre reconstruye/borra el GameObject seleccionado)
    // y el siguiente commit llega para otro AudioClip, este id evita aplicar
    // un "before" que no le corresponde.
    uint64_t m_audioDragOwnerId = 0;

    // SSR — mismo patrón de snapshot que los sliders de audio (SliderFloat, no
    // DragFloat: salta al valor bajo el cursor en el mismo frame del click, así
    // que el "before" se lee antes de dibujar el widget).
    bool     m_ssrDragActive = false;
    float    m_ssrDragBeforeIntensity = 0.5f;
    uint64_t m_ssrDragOwnerId = 0;

    // Popup "Nuevo Script" — disparado desde Add > Script > Nuevo Script...
    // m_newScriptTarget se captura al abrir (ctx.selected puede cambiar con
    // el popup abierto) y se revalida contra la escena antes de añadir.
    bool        m_openNewScriptPopup = false;
    char        m_newScriptNameBuffer[64] = {};
    std::string m_newScriptError;
    GameObject* m_newScriptTarget = nullptr;
};

} // namespace DonTopo
