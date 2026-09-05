#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <string>

namespace DonTopo {

class GameObject;
class Camera;
class Scene;
struct EditorContext;

// Qué manipula el gizmo del viewport sobre el objeto seleccionado. Uno de los
// tres a la vez, como en Unity: los tres juegos de handles a la vez serían
// imposibles de clicar.
//
// Enum propio y no `ImGuizmo::OPERATION` para no meter ImGuizmo.h —y con él
// imgui.h— en un header que incluyen la toolbar y los tests. La traducción a
// los enums de la librería vive en el .cpp, en un solo sitio.
enum class GizmoMode { Translate, Rotate, Scale };

// Etiqueta del canal que edita cada modo, tal y como la escriben el Log Console
// y el panel Properties: "Position", "Rotation", "Scale". Es lo que hace que la
// línea del log del gizmo sea indistinguible de la que emite Properties al
// editar el mismo valor a mano.
const char* gizmoChannelLabel(GizmoMode mode);

// Los tres números que enseña el log tras un arrastre, sacados de la matriz
// LOCAL resultante: la posición en unidades de mundo, la rotación en GRADOS
// (no radianes: el inspector enseña grados) o la escala como factor.
//
// Va junto a gizmoChannelLabel y no dentro de él porque es lo que de verdad se
// puede equivocar — un copia-pega que deje el modo Rotate informando de la
// posición compila igual de bien y solo se nota leyendo el log.
glm::vec3 gizmoLoggedValue(GizmoMode mode, const glm::mat4& localTransform);

// Qué se le pide a ImGuizmo para cada modo: `outOperation` es un
// `ImGuizmo::OPERATION` y `outSpace` un `ImGuizmo::MODE`, los dos como int para
// no arrastrar ImGuizmo.h —y con él imgui.h— hasta este header. Quien los use
// los vuelve a castear; quien los pruebe los compara contra los enums de
// verdad.
//
// El espacio NO es el mismo en los tres, y es una decisión, no un descuido:
//   - Translate → WORLD: arrastrar "X" mueve en la X del mundo.
//   - Rotate    → LOCAL: los anillos salen pegados a los ejes del objeto; en
//     WORLD se dibujan alineados al mundo y, con el objeto inclinado, no se
//     corresponden con nada de lo que se ve.
//   - Scale     → LOCAL obligatorio. ImGuizmo hace
//     `ComputeContext(..., (operation & SCALE) ? LOCAL : mode)` y descarta lo
//     que se le pase; se le pasa LOCAL para que la llamada no mienta.
//
// Está aquí fuera porque es lo ÚNICO del despacho por modo que se puede probar
// sin GUI: mandar Rotate a TRANSLATE compila, corre y solo se ve en pantalla.
void gizmoImGuizmoEnums(GizmoMode mode, int& outOperation, int& outSpace);

// Matriz LOCAL que deja al objeto exactamente en newWorld, dado el
// worldTransform de su padre (identidad si no tiene padre).
//
// ImGuizmo manipula una matriz de MUNDO, pero lo que la escena serializa, lo
// que edita el panel Properties y lo que apila el undo es `localTransform`.
// Escribir el mundo en el local funcionaría SOLO en las raíces: un hijo daría
// el salto de aplicarle el transform del padre por segunda vez.
//
// Vive fuera de la clase —y en el header— porque es el único trozo de la
// manipulación que se puede probar sin GUI: la interacción de ratón no se
// puede simular headless, la aritmética de matrices sí.
glm::mat4 localFromWorld(const glm::mat4& parentWorld, const glm::mat4& newWorld);

// Escribe t como localTransform del objeto con ese id, propaga el mundo a sus
// hijos y teletransporta su collider si tiene. No-op si el id ya no existe.
//
// Es el cuerpo del comando de undo del manipulador, aquí fuera por lo mismo que
// localFromWorld: es la parte que se puede afirmar sin GUI. El objeto se busca
// por ID y no por puntero a propósito — entre apilar el comando y deshacerlo
// caben un borrado y una carga de escena, y el GameObject reconstruido conserva
// el id pero no la dirección.
void applyLocalTransform(Scene& scene, uint64_t id, const glm::mat4& t);

// Ventana "Viewport" — render 3D embebido (textura del Renderer) + gizmo de
// ejes/wireframe de collider sobre la selección activa.
class ViewportPanel {
public:
    // viewportTexture es un handle opaco del backend activo (VkDescriptorSet
    // con Vulkan, descriptor GPU con DirectX 12): solo se reenvía a
    // ImGui::Image, que lo trata igual en los dos casos.
    void draw(EditorContext& ctx, uint64_t viewportTexture, const glm::mat4& cameraView);
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

    // Esquina superior izquierda de la IMAGEN en coordenadas de pantalla, y si
    // el ratón está sobre ella (no sobre la ventana: un popup por encima no
    // cuenta). Como la imagen se dibuja 1:1 con el render, restarle esta
    // esquina al ratón da directamente el píxel del canvas de UI.
    glm::vec2 imagePos()     const { return m_imagePos; }
    bool      imageHovered() const { return m_imageHovered; }

    // Modo del gizmo. Lo escriben los tres botones de la toolbar y los atajos
    // W/E/R, los dos en EditorUI; el estado vive aquí porque es de este panel
    // —quien lo lee es el manipulador— y así no hace falta un campo más en
    // EditorContext ni que el panel y la toolbar se pasen el dato cada frame.
    GizmoMode gizmoMode() const     { return m_gizmoMode; }
    void setGizmoMode(GizmoMode m)  { m_gizmoMode = m; }

private:
    void drawSelectionGizmo(EditorContext& ctx);
    // Manipulador de transformación (ImGuizmo) sobre el objeto seleccionado, en
    // el modo que diga m_gizmoMode. Es lo único de este panel que EDITA la
    // escena: drawSelectionGizmo y los otros trece solo pintan.
    //
    // imagePos/imageSize son el rect de la IMAGEN, no el de la ventana: el
    // manipulador tiene que caer sobre el mismo pixel que el objeto, y la
    // ventana lleva encima la barra de título y los bordes del dock.
    void drawTransformGizmo(EditorContext& ctx, const glm::mat4& cameraView,
                             const glm::vec2& imagePos, const glm::vec2& imageSize);
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
    // Rectángulo del ÁREA ÚTIL del Canvas seleccionado, en 2D sobre la imagen
    // del viewport (la UI es espacio de pantalla, no mundo: no pasa por
    // Gizmos). El rect SALE del canvas vivo (uiOrigin/uiScale/referenceSize),
    // no se recalcula aquí: safe area y aspect ratio ya vienen aplicados.
    void drawCanvasGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                          const glm::vec2& imageSize);
    // Rect + ejes X/Y del Button seleccionado, en 2D sobre la imagen igual que
    // el gizmo del Canvas. El rect sale del nodo VIVO del canvas (anclas y
    // escala ya aplicadas), no de los campos del componente.
    void drawButtonGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                          const glm::vec2& imageSize);
    // Lo mismo para el Text seleccionado. Es otro nodo del canvas (nombre con
    // otro prefijo), así que un GameObject con Button y Text pinta los dos.
    void drawTextGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                        const glm::vec2& imageSize);
    // Y lo mismo para la ProgressBar seleccionada. El rect es el del FONDO (el
    // nodo raíz de la barra), no el del relleno: el relleno se encoge con el
    // valor y el gizmo mide el widget, no el dato.
    void drawProgressBarGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                               const glm::vec2& imageSize);
    // Y el del contenedor de Layout, que es el ÚNICO que no se puede clicar en
    // el viewport (no es raycastTarget: un grupo que no pinta no debe comerse
    // los clics). Se selecciona desde el Hierarchy, y este gizmo es lo único que
    // enseña dónde está su rect.
    void drawLayoutGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                          const glm::vec2& imageSize);
    // Y los del resto de widgets de UI. Mismo criterio que los de arriba: el
    // rect sale del nodo VIVO, no de los campos del componente, asi que ya trae
    // aplicadas las anclas, la escala del canvas y el layout.
    //
    // El del ScrollView mide el VIEWPORT (el nodo que recorta) y no el
    // contenido: el contenido se mueve y es mas grande que la vista, asi que su
    // rect no dice donde esta el widget.
    void drawInputFieldGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                       const glm::vec2& imageSize);
    void drawDropdownGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                       const glm::vec2& imageSize);
    void drawScrollViewGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                       const glm::vec2& imageSize);
    void drawSliderGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                       const glm::vec2& imageSize);
    void drawCheckboxGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                       const glm::vec2& imageSize);
    void drawToggleGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                       const glm::vec2& imageSize);
    void drawScrollbarGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                       const glm::vec2& imageSize);
    void drawPanelGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                         const glm::vec2& imageSize);
    void drawImageGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                         const glm::vec2& imageSize);
    // Selección por clic de un widget de UI: hit test 2D del canvas, que manda
    // sobre el raycast 3D porque la UI se dibuja ENCIMA de la escena. mousePx
    // va en píxeles relativos a la esquina de la imagen, igual que en
    // pickObject. nullptr si el clic no cae en ningún widget.
    GameObject* pickUiObject(EditorContext& ctx, const glm::vec2& mousePx,
                              const glm::vec2& imageSize) const;
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
    // La `view` de la cámara con la que se dibujó el último frame. La rellena
    // draw() nada más entrar, igual que ya rellena m_imagePos y m_contentWidth.
    //
    // Existe porque los gizmos de widget de un canvas de MUNDO tienen que
    // PROYECTAR su rect, y proyectar pide la vista por partida doble: para la
    // matriz de cámara y para el billboard del canvas. Los trece drawXGizmo la
    // leen de aquí y se la pasan a drawUiNodeGizmo. Un parámetro en esas trece
    // firmas sería lo mismo con más ruido; lo que NO vale es un estático de
    // fichero, que dejaría el dato fuera del alcance del panel.
    glm::mat4 m_cameraView{1.0f};

    // Estado del arrastre del manipulador de traslación. Existe para que un
    // arrastre entero deje UN solo comando en el undo, no uno por frame: el
    // `before` se captura en el flanco de entrada y el comando se apila en el
    // de salida, igual que PropertiesPanel hace con IsItemActivated /
    // IsItemDeactivatedAfterEdit en sus DragFloat.
    GizmoMode m_gizmoMode  = GizmoMode::Translate;
    // El modo con el que EMPEZÓ el arrastre en curso, para la línea del log.
    // Los atajos siguen respondiendo mientras se arrastra.
    GizmoMode m_gizmoModeAtGrab = GizmoMode::Translate;
    bool      m_gizmoUsing = false;
    glm::mat4 m_gizmoBefore{1.0f};
    // Y el objeto se recuerda por ID, no por puntero: entre el flanco de
    // entrada y el de salida caben una carga de escena y un borrado, y un
    // GameObject* guardado se quedaría colgando. Mismo criterio que la lambda
    // del PropertyCommand, que también resuelve por findById.
    uint64_t    m_gizmoId = 0;
    std::string m_gizmoName;

    bool m_hovered = false;
    bool m_imageHovered = false;
    glm::vec2 m_imagePos{0.0f};
    uint32_t m_contentWidth  = 0;
    uint32_t m_contentHeight = 0;
};

} // namespace DonTopo
