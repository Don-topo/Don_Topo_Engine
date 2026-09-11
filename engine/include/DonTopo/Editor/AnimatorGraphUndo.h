#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include "DonTopo/Core/AnimatorComponent.h"

namespace DonTopo {

class ICommand;
class Scene;

// Convierte en comandos de undo, uno por gesto, las ediciones que el
// AnimatorPanel hace EN VIVO sobre el grafo.
//
// El panel muta el componente en ~22 sitios. Envolver cada uno con su
// antes/después serían 22 obligaciones, y el sitio 23 se la saltaría sin
// avisar. En su lugar, el panel llama a beginFrame al empezar a dibujar y a
// endFrame al terminar, y esto compara el grafo entre las dos llamadas con la
// clave del .scene (animatorGraphKey). Así, cualquier cosa que se guarde y
// cambie entra en el undo, venga del sitio que venga.
//
// Sin ImGui a propósito: quién está activo y en qué revisión va el historial
// llegan como parámetros, así que se prueba sin GUI.
class AnimatorGraphUndoTracker {
public:
    // Abre una sesión si no hay una abierta para este mismo GameObject: un drag
    // que sigue de un frame anterior conserva su 'before'. anim nulo descarta.
    void beginFrame(uint64_t goId, const AnimatorComponent* anim, uint64_t undoRevision);

    // Devuelve el comando del gesto cuando el grafo ha cambiado y ya no queda
    // ningún widget activo, con el cambio YA aplicado: el llamante lo empuja
    // sin execute(). Si el historial se movió durante el gesto (undoRevision
    // distinta), no emite nada y toma una nueva línea base: la diferencia
    // incluiría lo que hizo otro comando.
    std::unique_ptr<ICommand> endFrame(Scene& scene, const AnimatorComponent* anim,
                                       bool anyItemActive, uint64_t undoRevision);

    // Opcional: nombre del gesto en curso para la Log Console ("Borrar
    // estado"). Si ningún sitio lo llama, el comando se llama "Editar Animator".
    void setLabel(std::string label) { m_label = std::move(label); }

    // El panel ha dejado de dibujar el grafo (cerrado, colapsado, sin
    // Animator): una sesión no puede sobrevivir a eso.
    void discard();

    bool sessionOpen() const { return m_open; }

private:
    static constexpr const char* kEtiquetaPorDefecto = "Editar Animator";

    void open(uint64_t goId, const AnimatorComponent& anim, uint64_t undoRevision);
    void close();

    bool                     m_open     = false;
    uint64_t                 m_id       = 0;
    uint64_t                 m_revision = 0;
    AnimatorComponent::Graph m_before;
    nlohmann::json           m_beforeKey;
    std::string              m_label    = kEtiquetaPorDefecto;
};

} // namespace DonTopo
