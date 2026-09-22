#pragma once

namespace DonTopo
{
    // Ids del lienzo del Animator (imgui-node-editor). Viven aquí, fuera del
    // panel, porque son la pieza con riesgo del canvas: un fallo al codificar o
    // decodificar no da error de compilación, se manifiesta como "borrar un
    // nodo borra otro" o "el menú contextual abre el del estado equivocado".
    // Así se pueden probar sin ventana.
    //
    // CINCO ranuras por estado: el nodo, el par de pines normal y un par
    // SECUNDARIO que usa la transición de vuelta cuando dos estados se enlazan
    // en los dos sentidos. Sin ese segundo par las dos curvas salen de los
    // mismos dos puntos y se dibujan una sobre otra, porque la curvatura
    // (StyleVar_LinkStrength) es propiedad del PIN y no del link.
    //
    // El id que se codifica es el **editorId** del estado, no su índice: el
    // índice cambia cuando removeState reindexa y un superviviente heredaría el
    // slot visual (posición, selección) del nodo borrado, que la librería
    // cachea por id.
    namespace canvasIds
    {
        inline int node(int eid)       { return eid * 5 + 1; }
        inline int inputPin(int eid)   { return eid * 5 + 2; }
        inline int outputPin(int eid)  { return eid * 5 + 3; }
        inline int inputPin2(int eid)  { return eid * 5 + 4; }
        inline int outputPin2(int eid) { return eid * 5 + 5; }
        inline int link(int transIdx)  { return 100000 + transIdx; }

        // La misma división entera sirve para las cinco variantes.
        inline int editorIdFrom(int rawId) { return (rawId - 1) / 5; }
        // Salida: la ranura 2 (par normal) y la 4 (par secundario).
        inline bool isOutputPin(int pin) { const int r = (pin - 1) % 5; return r == 2 || r == 4; }
        // Los dos pines de entrada, para distinguir el par secundario del normal.
        inline bool isSecondaryPin(int pin) { const int r = (pin - 1) % 5; return r == 3 || r == 4; }

        // Nodo Any State: ids FUERA del esquema de los estados y de los links.
        // Se comprueban SIEMPRE antes de decodificar: pasados por editorIdFrom
        // casarían con un editorId (180000) que ningún grafo alcanza, pero
        // isOutputPin los clasificaría mal.
        inline constexpr int kAnyStateNode   = 900001;
        inline constexpr int kAnyStateOutPin = 900002;
    }
}
