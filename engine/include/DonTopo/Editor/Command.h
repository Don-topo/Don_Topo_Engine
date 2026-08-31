#pragma once
#include "DonTopo/Audio/AudioBus.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/UI/CanvasComponent.h"
#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/TextComponent.h"

namespace DonTopo {

class Scene;
class Renderer;
class EditorRenderer;
class PhysicsManager;
class AudioManager;

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void execute() = 0;   // aplica "after" (redo)
    virtual void undo() = 0;      // aplica "before"
    virtual std::string label() const = 0;   // pa Log Console
};

// Comando genérico pa cualquier propiedad value-type de un GameObject o de
// uno de sus componentes. apply() resuelve el objeto en vivo cada vez que se
// invoca (nunca captura un GameObject* crudo) — sobrevive a que el
// GameObject se haya reconstruido entretanto por un Undo de Delete.
template <typename T>
class PropertyCommand : public ICommand {
public:
    PropertyCommand(std::string label, T before, T after,
                     std::function<void(const T&)> apply)
        : m_label(std::move(label)), m_before(std::move(before)),
          m_after(std::move(after)), m_apply(std::move(apply)) {}

    void execute() override { m_apply(m_after); }
    void undo()    override { m_apply(m_before); }
    std::string label() const override { return m_label; }

private:
    std::string m_label;
    T m_before;
    T m_after;
    std::function<void(const T&)> m_apply;
};

// Comando de un AJUSTE DE RENDER (los del menú View: bloom, SSAO, niebla, AA…).
//
// Es un PropertyCommand<T> con dos diferencias que vienen de dónde vive el
// dato, no de qué tipo tiene:
//
//  1. `persist` se llama SIEMPRE junto al setter. Un ajuste de render no está
//     en la escena, está en el project.json: un undo que aplica el valor pero
//     no reescribe el fichero corrige la imagen y deja lo deshecho esperando a
//     que se reabra el proyecto. Ir emparejados en el helper es lo que impide
//     que un llamante de los 39 se deje uno.
//  2. Se empuja con `UndoManager::push(cmd, /*dirtiesScene=*/false)`: mover un
//     slider de bloom no es una edición de la escena y no puede disparar el
//     modal de «hay cambios sin guardar».
//
// El helper NO aplica nada al construirse: el widget de ImGui ya escribió el
// valor nuevo cuando devolvió true, igual que en el resto del editor.
template <typename T, typename Setter, typename Persist>
std::unique_ptr<ICommand> makeRenderSettingCommand(std::string label, T before, T after,
                                                    Setter set, Persist persist)
{
    return std::make_unique<PropertyCommand<T>>(
        std::move(label), std::move(before), std::move(after),
        [set = std::move(set), persist = std::move(persist)](const T& v) {
            set(v);
            persist();
        });
}

// Snapshots value-type pa cada tipo de collider — T de PropertyCommand<T>
// en las secciones Box/Sphere/Capsule/Plane Collider del panel Properties.
// La gravedad ya no vive en el collider (pasó al Rigidbody): ver RigidbodyState.
// staticFriction/dynamicFriction/bounciness: material de física por collider.
// Van en el snapshot para que el undo de la sección los cubra igual que
// center/size; los defaults coinciden con los de Collider (0.5 / 0.5 / 0.1).
struct BoxColliderState     { glm::vec3 center; glm::vec3 size; bool isTrigger;
                              float staticFriction; float dynamicFriction; float bounciness; };
struct SphereColliderState  { glm::vec3 center; float radius; bool isTrigger;
                              float staticFriction; float dynamicFriction; float bounciness; };
struct CapsuleColliderState { glm::vec3 center; float radius; float height; bool isTrigger;
                              float staticFriction; float dynamicFriction; float bounciness; };
struct PlaneColliderState   { glm::vec3 center; bool isTrigger;
                              float staticFriction; float dynamicFriction; float bounciness; };

// Snapshot value-type del Rigidbody — T de PropertyCommand<T> en la sección
// Rigidbody del panel Properties.
struct RigidbodyState {
    float    mass;
    bool     useGravity;
    bool     isKinematic;
    float    drag;
    float    angularDrag;
    uint32_t constraints;
    bool     ccd;
    bool     interpolate;
};

// Snapshot value-type del AudioClipComponent — T de PropertyCommand<T> en la
// sección Audio Clip del panel Properties. Los cuatro sliders (volumen, pitch y
// las dos distancias 3D) MÁS los tres checkboxes: antes loop/is3D/playOnAwake se
// escribían directos y no tenían undo, así que desmarcar "Is 3D?" con un clip
// sonando lo cortaba en seco y Ctrl+Z no lo devolvía.
//
// Los tres van en el mismo struct que los sliders, no en uno aparte: un solo
// tipo de comando para toda la sección hace que un undo restaure el estado
// completo aunque se hayan tocado sliders y checkboxes en distinto orden.
struct AudioClipState {
    float volume;
    float pitch;
    float minDistance;
    float maxDistance;
    bool  loop;
    bool  is3D;
    bool  playOnAwake;
    // Bus de salida. Va en el mismo snapshot que el resto por lo mismo que los
    // checkboxes: un solo comando para toda la seccion.
    AudioBus bus;
    // Como loop e is3D: cambiarlo recarga el sonido.
    AudioLoadMode loadMode;
    AudioRolloff  rolloff;
    float spread;
    float stereoPan;
    float dopplerLevel;
    bool  mute;
};

// Snapshot value-type del CameraComponent — T de PropertyCommand<T> en la
// sección Camera del panel Properties.
struct CameraState {
    CameraComponent::ProjectionMode mode;
    float fov;
    float orthographicSize;
    float nearPlane;
    float farPlane;
};

class ReparentCommand : public ICommand {
public:
    ReparentCommand(Scene& scene, std::string label, uint64_t id,
                     uint64_t oldParentId, size_t oldIndex,
                     uint64_t newParentId, size_t newIndex);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void moveTo(uint64_t parentId, size_t index);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    uint64_t m_oldParentId;
    size_t m_oldIndex;
    uint64_t m_newParentId;
    size_t m_newIndex;
};

// Borra un GameObject ya existente (execute) / lo reconstruye desde un
// snapshot JSON tomado ANTES de borrarlo (undo). El snapshot conserva el id
// original (Scene::subtreeToJson/nodeToJson serializan "id"), así que
// comandos posteriores en el stack que referencien ese id lo siguen
// resolviendo tras un undo() de este comando.
class DeleteGameObjectCommand : public ICommand {
public:
    DeleteGameObjectCommand(Scene& scene, PhysicsManager& physics, AudioManager& audio, EditorRenderer& renderer,
                             std::string label, uint64_t parentId, size_t index, nlohmann::json snapshot);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    Scene& m_scene;
    PhysicsManager& m_physics;
    AudioManager& m_audio;
    EditorRenderer& m_renderer;
    std::string m_label;
    uint64_t m_parentId;
    size_t m_index;
    nlohmann::json m_snapshot;
};

// Inverso de DeleteGameObjectCommand: reconstruye desde snapshot (execute) /
// borra (undo). snapshot ya incluye el subárbol completo tal y como quedó
// justo después de crearlo (mismo formato que DeleteGameObjectCommand).
class CreateGameObjectCommand : public ICommand {
public:
    CreateGameObjectCommand(Scene& scene, PhysicsManager& physics, AudioManager& audio, EditorRenderer& renderer,
                             std::string label, uint64_t parentId, size_t index, nlohmann::json snapshot);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    Scene& m_scene;
    PhysicsManager& m_physics;
    AudioManager& m_audio;
    EditorRenderer& m_renderer;
    std::string m_label;
    uint64_t m_parentId;
    size_t m_index;
    nlohmann::json m_snapshot;
};

// Añade (add=true) o quita (add=false) el CameraComponent del GameObject id;
// undo() hace lo contrario.
//
// A diferencia de los Add de collider/Rigidbody (que no pasan por el stack),
// el de cámara SÍ: sin esto se puede llegar a dos cámaras en escena — Add a X,
// Delete X (el snapshot se lleva la cámara), Add a Z (permitido, findCamera()
// es nullptr), Ctrl+Z resucita X CON su cámara. Con el Add en el stack, para
// deshacer el Delete de X hay que deshacer antes el Add de Z, y el orden impone
// el invariante sin descartar nada.
//
// Resuelve el GameObject por id en cada execute()/undo() (nunca puntero crudo),
// mismo contrato que PropertyCommand. m_state conserva los valores pa que un
// Add-undo-redo no los devuelva a los defaults.
class CameraComponentCommand : public ICommand {
public:
    CameraComponentCommand(Scene& scene, std::string label, uint64_t id,
                            bool add, CameraState state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    CameraState m_state;
};

// Add/Remove del CanvasComponent, mismo contrato que CameraComponentCommand:
// resuelve el GameObject por id en cada execute()/undo() (nunca puntero crudo),
// y m_state es una COPIA del componente entero (10 campos, todo POD) pa que un
// Add-undo-redo no devuelva la resolución a los defaults.
class CanvasComponentCommand : public ICommand {
public:
    CanvasComponentCommand(Scene& scene, std::string label, uint64_t id,
                            bool add, CanvasComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    CanvasComponent m_state;
};

// Add/Remove del AudioClipComponent. Mismo patrón que CanvasComponentCommand
// (resuelve el GameObject por id en cada execute()/undo(), nunca puntero crudo)
// con una diferencia obligada: AudioClipComponent NO es copiable —envuelve un
// soundId de FMOD y su destructor descarga el sonido—, así que el snapshot son
// datos planos (path + AudioClipState) y rehacer el Add recrea el componente
// con createAudioClipComponent, la misma factory que usa Scene::fromJson.
//
// Sin esto, quitar un Audio Clip perdía para siempre volumen, pitch y las dos
// distancias ajustadas a mano, y Ctrl+Z no devolvía nada.
class AudioClipComponentCommand : public ICommand {
public:
    AudioClipComponentCommand(Scene& scene, AudioManager& audio, std::string label,
                               uint64_t id, bool add, std::string path, AudioClipState state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene&         m_scene;
    AudioManager&  m_audio;
    std::string    m_label;
    uint64_t       m_id;
    bool           m_add;
    std::string    m_path;
    AudioClipState m_state;
};

// Add/Remove del AudioListenerComponent. Su estado entero es un bool, pero el
// comando existe por la misma razón que los demás: que añadirlo y quitarlo pase
// por el stack de undo como todo lo demás del panel.
class AudioListenerComponentCommand : public ICommand {
public:
    AudioListenerComponentCommand(Scene& scene, std::string label, uint64_t id,
                                   bool add, bool enabled);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene&      m_scene;
    std::string m_label;
    uint64_t    m_id;
    bool        m_add;
    bool        m_enabled;
};

// Add/Remove del ButtonComponent, calcado de CanvasComponentCommand: resuelve el
// GameObject por id en cada execute()/undo() (nunca puntero crudo), y m_state es
// una COPIA del componente entero pa que un Add-undo-redo no devuelva los
// colores, el texto ni las rutas a los defaults.
class ButtonComponentCommand : public ICommand {
public:
    ButtonComponentCommand(Scene& scene, std::string label, uint64_t id,
                            bool add, ButtonComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    ButtonComponent m_state;
};

// Add/Remove del TextComponent, calcado de ButtonComponentCommand: resuelve el
// GameObject por id en cada execute()/undo() (nunca puntero crudo), y m_state es
// una COPIA del componente entero pa que un Add-undo-redo no devuelva el texto,
// los colores ni la ruta de la fuente a los defaults.
class TextComponentCommand : public ICommand {
public:
    TextComponentCommand(Scene& scene, std::string label, uint64_t id,
                          bool add, TextComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    TextComponent m_state;
};

// Add/Remove del ProgressBarComponent, calcado de TextComponentCommand: resuelve
// el GameObject por id en cada execute()/undo() (nunca puntero crudo), y m_state
// es una COPIA del componente entero pa que un Add-undo-redo no devuelva el
// valor, los colores ni las rutas de los sprites a los defaults.
class ProgressBarComponentCommand : public ICommand {
public:
    ProgressBarComponentCommand(Scene& scene, std::string label, uint64_t id,
                                 bool add, ProgressBarComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    ProgressBarComponent m_state;
};

// Add/Remove del LayoutComponent, calcado de ProgressBarComponentCommand:
// resuelve el GameObject por id en cada execute()/undo() (nunca puntero crudo),
// y m_state es una COPIA del componente entero pa que un Add-undo-redo no
// devuelva el modo, el padding ni la celda a los defaults.
class LayoutComponentCommand : public ICommand {
public:
    LayoutComponentCommand(Scene& scene, std::string label, uint64_t id,
                            bool add, LayoutComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    LayoutComponent m_state;
};

// Add/Remove del PanelComponent, calcado de LayoutComponentCommand: resuelve el
// GameObject por id en cada execute()/undo() (nunca puntero crudo), y m_state es
// una COPIA del componente entero pa que un Add-undo-redo no devuelva el rect,
// el color ni el sprite a los defaults.
class PanelComponentCommand : public ICommand {
public:
    PanelComponentCommand(Scene& scene, std::string label, uint64_t id,
                          bool add, PanelComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    PanelComponent m_state;
};

// Add/Remove del ImageComponent, mismo patrón: m_state es una COPIA del
// componente entero pa que un Add-undo-redo no devuelva el modo, los bordes del
// 9-slice ni el bloque de Filled a los defaults.
class ImageComponentCommand : public ICommand {
public:
    ImageComponentCommand(Scene& scene, std::string label, uint64_t id,
                          bool add, ImageComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    ImageComponent m_state;
};

// Add/Remove del SliderComponent, mismo patron: m_state es una COPIA del
// componente entero pa que un Add-undo-redo no devuelva el valor, el rango ni
// los colores a los defaults. La copia ESTRENA callbacks (UiSliderCallbackSlot
// no los copia), asi que un undo no revive el handler de un script muerto.
class SliderComponentCommand : public ICommand {
public:
    SliderComponentCommand(Scene& scene, std::string label, uint64_t id,
                           bool add, SliderComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    SliderComponent m_state;
};

// Add/Remove del CheckboxComponent, mismo patron que los demas: m_state es una COPIA
// del componente entero pa que un Add-undo-redo no devuelva sus campos a los
// defaults, y la copia ESTRENA callbacks (el slot no los copia) asi que un undo
// no revive el handler de un script muerto.
class CheckboxComponentCommand : public ICommand {
public:
    CheckboxComponentCommand(Scene& scene, std::string label, uint64_t id,
                        bool add, CheckboxComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    CheckboxComponent m_state;
};
// Add/Remove del ToggleComponent, mismo patron que los demas: m_state es una COPIA
// del componente entero pa que un Add-undo-redo no devuelva sus campos a los
// defaults, y la copia ESTRENA callbacks (el slot no los copia) asi que un undo
// no revive el handler de un script muerto.
class ToggleComponentCommand : public ICommand {
public:
    ToggleComponentCommand(Scene& scene, std::string label, uint64_t id,
                        bool add, ToggleComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    ToggleComponent m_state;
};
// Add/Remove del ScrollbarComponent, mismo patron que los demas: m_state es una COPIA
// del componente entero pa que un Add-undo-redo no devuelva sus campos a los
// defaults, y la copia ESTRENA callbacks (el slot no los copia) asi que un undo
// no revive el handler de un script muerto.
class ScrollbarComponentCommand : public ICommand {
public:
    ScrollbarComponentCommand(Scene& scene, std::string label, uint64_t id,
                        bool add, ScrollbarComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    ScrollbarComponent m_state;
};

// Add/Remove del InputFieldComponent, mismo patron que los demas: m_state es una COPIA
// del componente entero pa que un Add-undo-redo no devuelva sus campos a los
// defaults, y la copia ESTRENA callbacks (el slot no los copia) asi que un undo
// no revive el handler de un script muerto.
class InputFieldComponentCommand : public ICommand {
public:
    InputFieldComponentCommand(Scene& scene, std::string label, uint64_t id,
                        bool add, InputFieldComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    InputFieldComponent m_state;
};
// Add/Remove del DropdownComponent, mismo patron que los demas: m_state es una COPIA
// del componente entero pa que un Add-undo-redo no devuelva sus campos a los
// defaults, y la copia ESTRENA callbacks (el slot no los copia) asi que un undo
// no revive el handler de un script muerto.
class DropdownComponentCommand : public ICommand {
public:
    DropdownComponentCommand(Scene& scene, std::string label, uint64_t id,
                        bool add, DropdownComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    DropdownComponent m_state;
};
// Add/Remove del ScrollViewComponent, mismo patron que los demas: m_state es una COPIA
// del componente entero pa que un Add-undo-redo no devuelva sus campos a los
// defaults, y la copia ESTRENA callbacks (el slot no los copia) asi que un undo
// no revive el handler de un script muerto.
class ScrollViewComponentCommand : public ICommand {
public:
    ScrollViewComponentCommand(Scene& scene, std::string label, uint64_t id,
                        bool add, ScrollViewComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    ScrollViewComponent m_state;
};

// Add/Remove del AnimatorComponent, mismo contrato que CameraComponentCommand:
// resuelve el GameObject por id en cada execute()/undo() (nunca puntero crudo),
// y m_state conserva el grafo pa que un Add-undo-redo no lo devuelva vacío.
//
// El estado es una COPIA del componente entero, no un POD de campos como
// CameraState: el "estado" de un Animator es el grafo completo, y
// AnimatorComponent es copiable (solo vectores, mapas y PODs). Serializarlo a
// JSON pa esto no compraría nada — las funciones de JSON viven en el anon
// namespace de Scene.cpp y no son accesibles desde aquí.
class AnimatorComponentCommand : public ICommand {
public:
    AnimatorComponentCommand(Scene& scene, std::string label, uint64_t id,
                              bool add, AnimatorComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    AnimatorComponent m_state;
};

// Añade (add=true) o quita (add=false) una fuente de animación del SkinnedMesh
// del GameObject id; undo() hace lo contrario. Mismo contrato que el resto:
// resuelve el GameObject por id en cada execute()/undo().
//
// m_clipNames guarda los nombres que la fuente aportó, y hace dos trabajos.
// Uno: sin él, deshacer un Remove reimportaría el fichero con los nombres del
// FBX y se perdería cualquier rename — dejando huérfanos los estados del grafo
// que los usaban. Dos: es la IDENTIDAD con la que applyRemove localiza su
// fuente. Los nombres de clip son únicos dentro del mesh (uniqueClipName lo
// garantiza) y viajan CON la fuente, mientras que una posición describe dónde
// estaba: applyAdd re-añade al final, así que cualquier ordinal guardado por
// otro comando del stack cambia de significado en cuanto se deshace un Remove.
//
// renderer puede ser nullptr (tests headless). Cuando no lo es, los SSBOs del
// objeto skinned se rehacen: la lista de clips ha cambiado y la GPU tiene la
// vieja.
class AnimationSourceCommand : public ICommand {
public:
    // pathOccurrence: FALLBACK posicional para applyRemove — qué fuente
    // no-builtin con ese path quitar, CONTADO DESDE EL FINAL del vector
    // (0 = la más reciente/última). Solo se usa cuando la búsqueda por
    // identidad (m_clipNames) no encuentra nada, p.ej. con m_clipNames vacío.
    // Importar el mismo FBX dos veces es legal, y AnimatorPanel distingue las
    // filas con este mismo ordinal. 0 por defecto vale tanto para un Add real
    // (nada que desambiguar, la fuente nueva siempre va al final) como para el
    // undo de un Add.
    AnimationSourceCommand(Scene& scene, EditorRenderer* renderer, std::string label,
                            uint64_t id, bool add, std::string path,
                            std::vector<std::string> clipNames,
                            size_t pathOccurrence = 0);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void applyAdd();
    void applyRemove();

    Scene& m_scene;
    EditorRenderer* m_renderer;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    std::string m_path;
    std::vector<std::string> m_clipNames;
    size_t m_pathOccurrence;
};

// Renombra un clip del mesh y arrastra los estados del Animator que lo usaban.
// No toca la GPU: los buffers van por índice de clip, y renombrar no reordena
// nada.
class ClipRenameCommand : public ICommand {
public:
    ClipRenameCommand(Scene& scene, std::string label, uint64_t id,
                       std::string oldName, std::string newName);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(const std::string& from, const std::string& to);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    std::string m_oldName;
    std::string m_newName;
};

} // namespace DonTopo
