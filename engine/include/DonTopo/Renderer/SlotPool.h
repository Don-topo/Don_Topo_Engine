#pragma once
#include <cstddef>
#include <vector>

namespace DonTopo {

// Huecos libres de un vector de objetos de render.
//
// Los dos backends guardan sus objetos en un vector y anotan el indice dentro
// del GameObject (`staticRenderIndex` / `skinnedRenderIndex`). Eso impide
// COMPACTAR al borrar —mover una entrada dejaria a todos los demas apuntando a
// otra malla—, asi que el hueco se queda ahi y el vector solo crece: un ciclo
// Play/Stop sumaba ranuras para siempre (H19, H43). Con el pool, borrar
// devuelve el indice y la siguiente alta lo reutiliza, con lo que los indices
// ajenos siguen significando lo mismo.
//
// En D3D12 reciclar el indice recicla ademas su bloque de descriptores, porque
// el bloque se deriva del indice (`kSrvObjects + indice * kSrvPerObject`).
//
// Header-only y sin dependencias: lo usan los dos backends y sus tests.
class SlotPool {
public:
    // Un hueco libre, o -1 si no hay: entonces el llamante crece su vector,
    // que es lo que se hacia siempre antes de esto. LIFO — el ultimo liberado
    // es el primero en volver.
    int acquire()
    {
        if (m_free.empty())
            return -1;
        const int slot = m_free.back();
        m_free.pop_back();
        if (static_cast<size_t>(slot) < m_isFree.size())
            m_isFree[static_cast<size_t>(slot)] = false;
        return slot;
    }

    // Devuelve el hueco. Ignora los negativos (un *RenderIndex sin asignar) y
    // los que YA estan libres.
    //
    // Ese segundo caso es la razon de llevar `m_isFree` y no solo la pila: un
    // doble release —quitar un subarbol que ya se habia quitado— dejaria el
    // mismo indice dos veces en la lista, y dos objetos nuevos acabarian
    // compartiendo ranura y bloque de descriptores. No lo avisaria ni la capa
    // de validacion: los dos indices son validos, solo que son el mismo.
    void release(int slot)
    {
        if (slot < 0)
            return;
        const size_t i = static_cast<size_t>(slot);
        if (i >= m_isFree.size())
            m_isFree.resize(i + 1, false);
        if (m_isFree[i])
            return;
        m_isFree[i] = true;
        m_free.push_back(slot);
    }

    // Los vectores de objetos se han vaciado enteros (clearStaticMeshes, o el
    // apagado): ningun hueco anterior sigue siendo valido.
    void clear()
    {
        m_free.clear();
        m_isFree.clear();
    }

    size_t freeCount() const { return m_free.size(); }

private:
    std::vector<int>  m_free;
    // Indexado por hueco: true = esta en m_free. Solo para rechazar el doble
    // release; crece hasta el indice mas alto que se haya liberado.
    std::vector<bool> m_isFree;
};

} // namespace DonTopo
