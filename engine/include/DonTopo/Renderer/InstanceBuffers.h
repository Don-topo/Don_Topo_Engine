#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <glm/glm.hpp>

namespace DonTopo {

class GpuDevice;

// Reparto por bump dentro de un buffer ya mapeado: quien pide sitio dice cuantas
// matrices quiere y recibe donde escribirlas, o **nullptr** si no caben.
//
// Sin una sola linea de Vulkan a proposito. Aqui vive la unica logica del
// asunto, asi que asi se puede afirmar entera sin GPU — y falta hacia, porque
// esta guarda estaba copiada a mano en TRES sitios del Renderer:
//
//     if (m_instanceCursor >= m_instanceCapacity[m_currentFrame]) { ... break; }
//     const uint32_t i = m_instanceCursor++;
//     ((glm::mat4*)m_instanceMapped[m_currentFrame])[i] = sobj.transform;
//
// Con `alloc` no hay forma de escribir sin haber mirado: el que no comprueba el
// nulo no escribe fuera del buffer, revienta en el acto. La aritmetica de
// punteros y el cursor dejan de estar al alcance del llamante.
class InstanceCursor {
public:
    // Buffer del frame y matrices que caben. `mapped` nulo = no cabe nada, que
    // es el estado valido de un frame cuyo buffer aun no existe.
    //
    // **Este es el UNICO sitio que escribe el buffer y su capacidad**, y de ahi
    // sale la invariante de la que vive todo lo demas: sin buffer, capacidad
    // CERO. Por eso ni `alloc` ni `rest` vuelven a preguntar por el puntero —
    // comprobar el tope ya cubre el caso, y una segunda guarda que no puede
    // dispararse es una que nadie sabe si sigue haciendo falta (se saboteo:
    // quitarla no ponia rojo ni un test).
    void reset(glm::mat4* mapped, uint32_t capacity)
    {
        m_mapped   = mapped;
        m_capacity = mapped ? capacity : 0;
        m_cursor   = 0;
    }

    // `n` matrices contiguas, o nullptr si no caben.
    //
    // `outBase` recibe el INDICE de la primera, que es lo que el draw pone en
    // `firstInstance` para que el shader la encuentre por `gl_InstanceIndex`.
    // Va aqui y no en un `cursor()` leido aparte porque son el mismo acto: leer
    // el indice antes de reservar y que la reserva falle deja un indice que
    // apunta a lo que escriba el siguiente.
    glm::mat4* alloc(uint32_t n, uint32_t* outBase = nullptr)
    {
        // La suma en 64 bits a proposito: con el cursor cerca del tope,
        // `m_cursor + n` en 32 bits podria dar la vuelta y pasar la comparacion.
        //
        // Sin buffer la capacidad es 0 (invariante de reset), asi que esta
        // comprobacion tambien cubre ese caso y no hace falta mirar el puntero.
        if ((uint64_t)m_cursor + n > (uint64_t)m_capacity) return nullptr;
        glm::mat4* dst = m_mapped + m_cursor;
        if (outBase) *outBase = m_cursor;
        m_cursor += n;
        return dst;
    }

    // Lo que queda libre, para quien no sabe cuantas matrices va a escribir
    // hasta que termina (el agrupado por lotes). Se cierra con commit().
    struct Span {
        glm::mat4* data     = nullptr;  // donde empieza el hueco libre
        uint32_t   capacity = 0;        // matrices que caben ahi
        uint32_t   base     = 0;        // indice de la primera, para firstInstance
    };

    Span rest() const
    {
        Span s;
        // La resta va con guarda porque es SIN SIGNO: un cursor pasado del tope
        // daria una capacidad enorme en vez de cero, y eso es escribir fuera del
        // buffer sin que nada avise. Con capacidad 0 (buffer sin mapear) tambien
        // sale por aqui, asi que no hay que mirar el puntero aparte.
        if (m_cursor >= m_capacity) return s;
        s.base     = m_cursor;
        s.data     = m_mapped + m_cursor;
        s.capacity = m_capacity - m_cursor;
        return s;
    }

    // Cierra un `rest()`: `used` es lo que de verdad se escribio. Se recorta al
    // tope por si el llamante miente; pasarse aqui movia el cursor fuera del
    // buffer y el siguiente pase escribia en tierra de nadie.
    void commit(uint32_t used)
    {
        const uint32_t libre = m_capacity - m_cursor;
        m_cursor += (used < libre) ? used : libre;
    }

    // Matrices ya escritas en el frame. Es la base de los `firstInstance` del
    // siguiente pase, que comparte buffer con este.
    uint32_t cursor() const { return m_cursor; }
    uint32_t capacity() const { return m_capacity; }

private:
    glm::mat4* m_mapped   = nullptr;
    uint32_t   m_capacity = 0;
    uint32_t   m_cursor   = 0;
};

// SSBO de transforms por instancia (set 1, binding 0): el buffer del que
// triangle.vert y shadow.vert sacan el model matrix por `gl_InstanceIndex`.
//
// Uno por frame-in-flight y mapeado en persistente, porque el frame anterior
// puede seguir en vuelo leyendo el suyo. Los pases del frame COMPARTEN el
// buffer: sombras escribe primero, la escena detras, y el cursor marca donde
// acaba lo ya escrito para que el siguiente no lo pise.
//
// Era estado suelto del Renderer —ocho miembros y tres metodos— y sale por lo
// mismo que salieron los trece pases: los recursos y su destruccion viajan
// juntos, y el que escribe ya no tiene que acordarse de nada.
class InstanceBuffers {
public:
    // Frames en vuelo. Renderer::MAX_FRAMES tiene que valer lo mismo, y hay un
    // static_assert en Renderer.h que lo comprueba: si alguien sube uno y no el
    // otro, los descriptor sets de los frames de mas nacerian sin buffer.
    static constexpr int kFrames = 2;

    // Matrices del buffer inicial. Se duplica al crecer, asi que instanciar un
    // objeto mas por frame (scripts Lua) no recrea el buffer en cada uno.
    static constexpr uint32_t kInitialCapacity = 1024;

    struct Context {
        GpuDevice& gpu;
    };

    InstanceBuffers()                                  = default;
    InstanceBuffers(const InstanceBuffers&)            = delete;
    InstanceBuffers& operator=(const InstanceBuffers&) = delete;

    // Set layout, pool, los kFrames sets y un buffer inicial para CADA frame.
    // Los dos, no solo el actual: el descriptor set de cada frame tiene que
    // apuntar a algo valido desde el primer draw.
    void create(const Context& ctx);
    void destroy(const Context& ctx);

    // Principio del frame: asegura sitio para `matrices` y pone el cursor a 0.
    // Crecer recrea el buffer, asi que esto va ANTES de grabar nada del frame,
    // nunca en mitad.
    void beginFrame(const Context& ctx, int frame, uint32_t matrices);

    // El reparto del frame en curso. Todo lo que escribe matrices pasa por aqui.
    InstanceCursor&       cur()       { return m_cur; }
    const InstanceCursor& cur() const { return m_cur; }

    VkDescriptorSetLayout descLayout() const { return m_descLayout; }
    VkDescriptorSet       set(int frame) const { return m_descSets[frame]; }

private:
    // Crece el buffer de `frame` hasta que quepan `matrices`, si no cabian ya.
    void ensureCapacity(const Context& ctx, int frame, uint32_t matrices);
    void destroyBuffer(const Context& ctx, int frame);

    VkDescriptorSetLayout m_descLayout        = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool          = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSets[kFrames] = {};
    VkBuffer              m_buffers[kFrames]  = {};
    VkDeviceMemory        m_memory[kFrames]   = {};
    void*                 m_mapped[kFrames]   = {};
    uint32_t              m_capacity[kFrames] = {};   // en matrices
    // El cursor NO es por frame: solo hay uno vivo cada vez, y beginFrame lo
    // reapunta al buffer de ese frame.
    InstanceCursor        m_cur;
};

}
