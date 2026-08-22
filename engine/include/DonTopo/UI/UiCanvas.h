#pragma once

// Jerarquía de la UI de juego, al estilo Unity: Canvas -> Panel -> hijos.
//
// Todo en PÍXELES y con el origen (0,0) arriba a la izquierda, +X a la derecha
// y +Y hacia abajo. La posición de un nodo es relativa a la esquina superior
// izquierda de su padre, y la escala del padre se acumula en el hijo.
//
// UiElement es la base de TODOS los widgets: los tipos concretos (Panel, Image,
// Button, ...) viven en UiWidgets.h y de momento no añaden ni un campo ni un
// comportamiento propio, solo su typeName(). Un panel es un elemento sin atlas
// (color plano) y una imagen es el mismo elemento con atlas y sprite.
//
// Esto vive en DonTopoCore, no en el editor: el juego exportado dibuja el mismo
// canvas con el mismo código.

#include "DonTopo/UI/UiSpriteBatch.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace DonTopo
{
    struct Text;
    struct Image;
    struct Button;
    class  UiElement;

    // ── Eventos ─────────────────────────────────────────────────────────────
    // Todo el sistema de input es CPU pura y DETERMINISTA: no consulta reloj ni
    // ventana. El tiempo y el estado del ratón ENTRAN por parámetro, así que la
    // misma secuencia de UiInputState da siempre la misma secuencia de eventos.

    enum class UiMouseButton : uint32_t
    {
        Left   = 0,
        Right  = 1,
        Middle = 2,
        Count  = 3
    };

    // Solo las teclas que la UI necesita distinguir. El texto que se escribe no
    // pasa por aquí: eso es otra fase.
    enum class UiKey : uint32_t
    {
        None = 0,
        Tab,
        Enter,
        Escape,
        Left,
        Right,
        Up,
        Down,

        // Edicion de texto. Van al final a proposito: los valores de las de
        // arriba no se mueven, asi que nada de lo ya guardado o mapeado cambia
        // de significado.
        Backspace,
        Delete,
        Home,
        End
    };

    // Lo rellena el CALLER (GLFW, el editor, un test). El Core no conoce ni
    // GLFW ni ImGui: aquí solo entran números.
    struct UiInputState
    {
        glm::vec2 mousePos{0.0f, 0.0f};       // px, mismo espacio que el canvas
        bool      mouseDown[3] = {false, false, false};   // estado SOSTENIDO por botón
        float     scrollDelta  = 0.0f;        // + hacia arriba; 0 = no hubo rueda

        // Teclas pulsadas ESTE frame (flanco, no sostenido): repetir una tecla
        // frame a frame es cosa del caller, no del canvas.
        std::vector<UiKey> keys;

        // CARACTERES tecleados este frame, en codepoints Unicode y en orden.
        // Es un canal APARTE de `keys` y no una tecla mas porque no son lo
        // mismo: UiKey nombra teclas fisicas con significado propio (Tab,
        // Enter, flechas), y una 'a' no es una tecla nombrada — sale del layout
        // del teclado, de las muertas y del metodo de entrada, cosa que el core
        // no sabe ni tiene por que saber. Lo rellena el caller (GLFW via
        // UiInputBridge, el editor, un test), igual que la posicion del raton.
        std::vector<uint32_t> chars;

        bool shift = false;
        bool ctrl  = false;
        bool alt   = false;

        // Segundos. Solo se compara consigo mismo (doble click), así que el
        // origen da igual mientras sea monótono.
        float timeSeconds = 0.0f;
    };

    enum class UiEventType : uint32_t
    {
        MouseMove,
        MouseEnter,
        MouseExit,
        MouseDown,
        MouseUp,
        Click,
        DoubleClick,
        DragBegin,
        Drag,
        DragEnd,
        Drop,
        Scroll,
        Focus,
        Blur,
        KeyDown,
        // Un caracter tecleado. Distinto de KeyDown por lo mismo que `chars` es
        // distinto de `keys`: aqui lo que llega es texto, no una tecla.
        TextInput
    };

    struct UiEvent
    {
        UiEventType type = UiEventType::MouseMove;

        // Dónde SE ORIGINÓ el evento. No cambia al burbujear: un padre que
        // recibe el click de un hijo sigue viendo al hijo aquí.
        UiElement* target = nullptr;

        glm::vec2 mousePos{0.0f, 0.0f};
        glm::vec2 delta{0.0f, 0.0f};       // movimiento respecto al frame anterior
        glm::vec2 dragStart{0.0f, 0.0f};   // dónde bajó el botón que arrastra

        UiMouseButton button = UiMouseButton::Left;
        float scrollDelta = 0.0f;

        UiKey key = UiKey::None;
        // Solo lo rellena TextInput: el codepoint Unicode del caracter.
        uint32_t codepoint = 0;
        bool  shift = false;
        bool  ctrl  = false;
        bool  alt   = false;

        float time = 0.0f;

        // Quién empezó el arrastre. Solo lo rellenan DragBegin/Drag/DragEnd/Drop,
        // y en un Drop puede NO ser el mismo que target.
        UiElement* dragSource = nullptr;

        // Mientras siga false el evento sigue subiendo al padre. Ponerlo a true
        // corta la burbuja ahí mismo.
        bool consumed = false;
    };

    using UiEventHandler = std::function<void(UiEvent&)>;

    // Auto-layout: con un modo distinto de None el contenedor COLOCA a sus
    // hijos y estos dejan de anclarse por su cuenta.
    enum class UiLayoutMode
    {
        None,
        Horizontal,
        Vertical,
        Grid
    };

    // Alineación en el eje TRANSVERSAL al del layout (la Y de un Horizontal y la
    // X de un Vertical). El Grid no la usa: la celda ya fija las dos.
    enum class UiCrossAlign
    {
        Start,
        Center,
        End
    };

    // ── Animación de propiedades ────────────────────────────────────────────
    // Propiedad que anima el elemento. UNA a la vez: no es un sistema de
    // pistas, es un campo. Fade escribe opacity, y NO hay un modo "Opacity"
    // aparte: la opacidad SE ANIMA CON Fade.
    enum class UiAnim
    {
        None,
        Fade,       // opacity      <- animFrom.x .. animTo.x
        Scale,      // scale        <- .xy
        Move,       // position     <- .xy
        Rotation,   // rotation     <- .x (radianes)
        Color       // color        <- los 4 canales
    };

    // Todas son funciones PURAS de t en [0,1] con f(0)=0 y f(1)=1 exactos.
    // Bounce y Elastic no son monótonas y Elastic se pasa de 1 a mitad de
    // camino: eso es lo que hacen, no un fallo que haya que recortar.
    enum class UiAnimCurve
    {
        Linear,
        EaseIn,
        EaseOut,
        Bounce,
        Elastic
    };

    enum class UiAnimLoop
    {
        Once,       // al llegar al final se queda en animTo y se para
        Loop,       // vuelve a empezar en animFrom
        PingPong    // vuelve por donde vino
    };

    class UiElement
    {
    public:
        explicit UiElement(std::string nodeName = {}) : name(std::move(nodeName)) {}
        virtual ~UiElement() = default;

        // Identidad del tipo sin RTTI ni enum paralelo: cada derivado devuelve
        // su literal y no hay nada más que mantener sincronizado.
        virtual const char* typeName() const { return "UiElement"; }

        // Widgets que emiten algo que NO es un único quad propio. El batcher los
        // pregunta por aquí, no con dynamic_cast: la identidad de tipo en este
        // árbol va sin RTTI, y un enum paralelo sería otra cosa que mantener.
        virtual const Text* asText() const { return nullptr; }

        // Un Image puede emitir N quads (Tiled, Sliced, Filled), todos con el
        // MISMO atlas y el MISMO scissor, así que ninguno parte el lote.
        virtual const Image* asImage() const { return nullptr; }

        // El Button NO lo mira el batcher: lo mira updateInput, que necesita
        // ESCRIBIR en él (color, sprite, estado), de ahí la versión no const.
        virtual Button*       asButton()       { return nullptr; }
        virtual const Button* asButton() const { return nullptr; }

        std::string name;

        glm::vec2 position{0.0f, 0.0f};   // px, relativa al ancla dentro del padre
        glm::vec2 size{0.0f, 0.0f};       // px, antes de la escala heredada
        glm::vec2 scale{1.0f, 1.0f};
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};

        // Normalizados 0..1 sobre el rect DEL PADRE. IGUALES en un eje = punto
        // de ancla: position cuenta desde ahí y pivot es el punto DE ESTE
        // elemento que cae encima ({0,0} y {0,0} = esquina superior izquierda
        // contra esquina superior izquierda). DISTINTOS en un eje = ESTIRADO en
        // ese eje: mandan los márgenes y se ignoran size y pivot de ese eje.
        glm::vec2 anchorMin{0.0f, 0.0f};
        glm::vec2 anchorMax{0.0f, 0.0f};
        glm::vec2 pivot{0.0f, 0.0f};

        // Píxeles hacia DENTRO desde el borde correspondiente del padre. Solo
        // los lee el eje estirado; en un eje anclado a un punto no pintan nada.
        float marginLeft   = 0.0f;
        float marginRight  = 0.0f;
        float marginTop    = 0.0f;
        float marginBottom = 0.0f;

        // ── Auto-layout ─────────────────────────────────────────────────────
        // Con mode != None este elemento coloca a sus hijos y de ellos se
        // ignoran anchorMin/Max, márgenes y position. Horizontal y Vertical
        // respetan el size del hijo (por su scale); Grid lo fuerza a cellSize.
        UiLayoutMode layoutMode = UiLayoutMode::None;

        float paddingLeft   = 0.0f;
        float paddingRight  = 0.0f;
        float paddingTop    = 0.0f;
        float paddingBottom = 0.0f;

        glm::vec2 spacing{0.0f, 0.0f};    // hueco entre celdas: .x entre columnas, .y entre filas
        glm::vec2 cellSize{0.0f, 0.0f};   // solo Grid
        uint32_t  columns = 0;            // solo Grid; 0 = las que quepan en el ancho

        UiCrossAlign crossAlign = UiCrossAlign::Start;

        // Este hijo NO ocupa hueco en el layout del padre: se ancla por su
        // cuenta, como si el padre no tuviera layout.
        bool ignoreLayout = false;

        // Content size fitter: ese eje del size pasa a ser la extensión de los
        // hijos COLOCADOS más el padding. Sin layoutMode no hay colocación, así
        // que sin él no hacen nada.
        bool fitWidth  = false;
        bool fitHeight = false;

        // Radianes, en sentido horario en pantalla (+Y va hacia abajo). Rota
        // las 4 esquinas de lo que emite ESTE elemento alrededor de su pivot;
        // no se hereda a los descendientes.
        // El scissor de clipChildren sigue siendo el AABB SIN rotar: la
        // máscara de un elemento rotado recorta por su rectángulo derecho, que
        // es lo único que un VkRect2D sabe expresar.
        // A 0.0f no se toca ni una coordenada: los vértices salen bit a bit
        // como salían antes de que la rotación se aplicara.
        float rotation = 0.0f;

        // Se multiplica por la del padre por todo el árbol y acaba en el alfa
        // del color del vértice.
        float opacity = 1.0f;

        bool visible  = true;
        // Solo para el input: NO afecta al dibujado. Un elemento deshabilitado
        // (y su subárbol) queda fuera del recorrido del foco y de la navegación.
        bool enabled  = true;
        // Un elemento puede ser solo un contenedor (agrupa y recorta) sin pintar.
        bool drawable = true;

        const UiTextureAtlas* atlas = nullptr;
        std::string sprite;

        // Recorta A ESTE ELEMENTO y a sus descendientes contra su propio rect.
        // El scissor resultante es la INTERSECCIÓN con el que ya venía del
        // padre, nunca un reemplazo.
        bool clipChildren = false;

        // ── Máscara rectangular ─────────────────────────────────────────────
        // Todo esto MODULA a clipChildren y nada más: sin clipChildren ninguno
        // de estos campos hace absolutamente nada, así que un árbol que no los
        // toca sale con los mismos vértices y los mismos lotes de siempre.

        // Píxeles de pantalla que el rect de recorte se mete hacia DENTRO del
        // rect del elemento, cada uno por su lado. Insets que se cruzan dan una
        // máscara VACÍA (width/height a 0), nunca negativa.
        float maskInsetLeft   = 0.0f;
        float maskInsetRight  = 0.0f;
        float maskInsetTop    = 0.0f;
        float maskInsetBottom = 0.0f;

        // A false el elemento se dibuja ENTERO con el scissor que heredó y la
        // máscara solo recorta a sus descendientes: es lo que deja el marco de
        // una ventana fuera de su propia máscara. A true (por defecto) el
        // comportamiento es el de siempre: se recorta él también.
        bool maskSelf = true;

        // Apaga la máscara sin sacar el elemento del árbol: con false el nodo
        // se comporta como si no tuviera clipChildren.
        bool maskEnabled = true;

        // ── Animación ───────────────────────────────────────────────────────
        // Todo en CPU y determinista. Quien mueve esto es UiCanvas::updateInput
        // y NADIE más: un canvas que no llama a updateInput no anima ni un
        // píxel, y con anim a None estos campos no escriben nada, así que un
        // árbol que no los toca sale con los mismos vértices de siempre.
        UiAnim      anim      = UiAnim::None;
        UiAnimCurve animCurve = UiAnimCurve::Linear;

        // Un vec4 para las cinco propiedades: cubre los 4 canales de Color y le
        // sobran componentes para las vec2 (Scale, Move) y para el float
        // (Fade, Rotation). Lo que no lee el modo no se mira.
        glm::vec4 animFrom{0.0f, 0.0f, 0.0f, 0.0f};
        glm::vec4 animTo  {0.0f, 0.0f, 0.0f, 0.0f};

        float      animDuration = 1.0f;             // segundos; <= 0 no avanza
        UiAnimLoop animLoop     = UiAnimLoop::Once;
        bool       animPlaying  = false;            // a false CONGELA: ni se avanza ni se escribe

        // Segundos ya recorridos. Lo lleva updateInput sumando el DELTA entre
        // frames de UiInputState::timeSeconds; no se acumula ningún dt de
        // fuera, y por eso pedir el mismo instante dos veces da lo mismo.
        float animTime = 0.0f;

        // ── Input ───────────────────────────────────────────────────────────
        // Nada de esto afecta al dibujado: quien no llame a UiCanvas::updateInput
        // no ve ni un cambio en los vértices ni en los lotes.

        // A false el elemento es INVISIBLE PARA EL RATÓN, pero sus hijos no: el
        // hit test los sigue probando (es un contenedor que deja pasar).
        bool raycastTarget = true;

        // Puede tomar el foco con un Down encima y entra en el recorrido del Tab.
        bool focusable = false;

        // ESTADO, no evento: lo mantiene updateInput y de él se DERIVAN
        // MouseEnter y MouseExit comparando el hit de este frame con el anterior.
        bool hovered = false;
        bool focused = false;

        // Overrides de la navegación direccional (mando). Nulos por defecto:
        // manda la geometría. Puestos MANDAN sobre ella, aunque apunten al lado
        // contrario. El destino tiene que ser focusable o el foco no se mueve.
        UiElement* navUp    = nullptr;
        UiElement* navDown  = nullptr;
        UiElement* navLeft  = nullptr;
        UiElement* navRight = nullptr;

        // Handlers. TODOS nulos por defecto: un canvas sin handlers no hace nada
        // al recibir input, solo mueve su estado interno.
        UiEventHandler onMouseMove;
        UiEventHandler onMouseEnter;
        UiEventHandler onMouseExit;
        UiEventHandler onMouseDown;
        UiEventHandler onMouseUp;
        UiEventHandler onClick;
        UiEventHandler onDoubleClick;
        UiEventHandler onDragBegin;
        UiEventHandler onDrag;
        UiEventHandler onDragEnd;
        UiEventHandler onDrop;
        UiEventHandler onScroll;
        UiEventHandler onFocus;
        UiEventHandler onBlur;
        UiEventHandler onKeyDown;
        UiEventHandler onTextInput;

        // ── Rect resuelto por el último buildDrawData ───────────────────────
        // El layout ya calcula estos tres valores por nodo; el input los REUSA
        // en vez de volver a medir el árbol (medirlo dos veces con dos códigos
        // distintos es la forma de que diverjan). rectValid queda a false en los
        // nodos que el emisor no llegó a visitar: invisibles y recortados a cero.
        mutable glm::vec2 screenPos{0.0f, 0.0f};
        mutable glm::vec2 screenSize{0.0f, 0.0f};
        mutable UiScissor screenScissor{};
        mutable bool      rectValid = false;

        // ── Dirty flags ─────────────────────────────────────────────────────
        // Qué ha cambiado en este nodo desde el último buildDrawData. Un nodo
        // sin ni un bit encendido NO se vuelve a emitir: se copian tal cual los
        // vértices que dejó la vez anterior. TODO nace sucio, así que un árbol
        // recién montado se emite entero igual que siempre.
        //
        //   Transform  posición, escala, rotación heredada y OPACIDAD: todo lo
        //              que mueve o atenúa también a los descendientes.
        //   Layout     anclas, márgenes, padding, spacing, modo de layout,
        //              tamaño, fitters: lo que recoloca el subárbol.
        //   Material   color, sprite, atlas: solo cambia lo que este nodo pinta.
        //   Vertex     la geometría del quad propio (rotación propia, insets del
        //              sliced, texto): tampoco sale del nodo.
        //
        // Transform y Layout SUBEN por la cadena de padres (un hijo que crece
        // puede cambiar la medida del padre con fitter) y BAJAN a todos los
        // descendientes (mueven sus rects). Material y Vertex se quedan donde
        // están. Esa asimetría es justo lo que hace que mover una hoja NO
        // reemita a sus hermanos.
        enum : uint32_t
        {
            DirtyTransform = 1u << 0,
            DirtyLayout    = 1u << 1,
            DirtyMaterial  = 1u << 2,
            DirtyVertex    = 1u << 3,
            DirtyAll       = DirtyTransform | DirtyLayout | DirtyMaterial | DirtyVertex
        };

        mutable uint32_t dirty = DirtyAll;

        // ÚNICO modo de ensuciar. Los campos siguen siendo públicos y se tocan a
        // pelo: quien los toque llama a esto justo después, con lo que ha tocado
        // y nada más. Es const porque el emisor y el input trabajan sobre
        // referencias const y el estado sucio no es estado observable del árbol.
        void markDirty(uint32_t flags) const
        {
            dirty |= flags;

            const uint32_t prop = flags & (DirtyTransform | DirtyLayout);
            if (prop == 0) return;

            // Arriba: solo la cadena de padres, sin volver a bajar por ellos (si
            // bajara, mover una hoja ensuciaría a todos sus hermanos).
            for (const UiElement* p = m_parent; p != nullptr; p = p->m_parent)
                p->dirty |= prop;

            markSubtreeDirty(prop);
        }

        // ── Caché de emisión ────────────────────────────────────────────────
        // Lo que este nodo emitió en el último build, troceado en los mismos
        // tramos (atlas + scissor) con los que se cortaron los lotes. Los
        // índices van RELATIVOS al primer vértice del nodo: al recolocarlos se
        // les suma la base de destino y salen los mismos uint16 de siempre.
        struct CacheSegment
        {
            const UiTextureAtlas* atlas = nullptr;
            UiScissor scissor{};
            uint32_t  vertexCount = 0;
            uint32_t  indexCount  = 0;
        };

        mutable std::vector<UiVertex>     cacheVertices;
        mutable std::vector<uint16_t>     cacheIndices;
        mutable std::vector<CacheSegment> cacheSegments;
        mutable bool                      cacheValid = false;

        // Colocación resuelta del nodo. Se reutiliza cuando ni Transform ni
        // Layout están sucios: cambiar SOLO el color no vuelve a calcular ni un
        // rect. Está en unidades de mundo (lo que ven los hijos) y en píxeles
        // (lo que ve el emisor).
        mutable glm::vec2 cacheWorldPos{0.0f, 0.0f};
        mutable glm::vec2 cacheWorldSize{0.0f, 0.0f};
        mutable glm::vec2 cacheWorldScale{1.0f, 1.0f};
        mutable glm::vec2 cacheChildArea{0.0f, 0.0f};
        mutable glm::vec2 cacheScreenPos{0.0f, 0.0f};
        mutable glm::vec2 cacheScreenSize{0.0f, 0.0f};
        mutable UiScissor cacheSelfScissor{};
        mutable UiScissor cacheChildScissor{};
        mutable float     cacheOpacity   = 1.0f;
        mutable bool      cacheSelfCulled = false;   // máscara propia vacía
        mutable bool      cacheGeomValid  = false;

        // Cuántas veces ha REEMITIDO este nodo. Solo para medir: nadie lo lee
        // dentro del motor. Sube en el build que reconstruye sus vértices y no
        // en el que se los copia de la caché.
        mutable uint32_t rebuildCount = 0;

        UiElement*       parent()       { return m_parent; }
        const UiElement* parent() const { return m_parent; }

        // Único modo de crear hijos: el árbol es dueño de ellos y devuelve el
        // tipo concreto, no la base.
        template <class T = UiElement>
        T& add(std::string childName = {})
        {
            static_assert(std::is_base_of<UiElement, T>::value,
                          "Un hijo del canvas tiene que derivar de UiElement");
            m_children.push_back(std::make_unique<T>(std::move(childName)));
            // El padre se cablea AQUÍ y en ningún otro sitio: es lo que permite
            // que un evento burbujee sin que el canvas lleve un mapa aparte.
            m_children.back()->m_parent = this;
            // Un hijo de más cambia la medida y el reparto del padre: sin esto
            // el layout se quedaría con el del árbol anterior.
            markDirty(DirtyLayout);
            return static_cast<T&>(*m_children.back());
        }

        void clearChildren()
        {
            m_children.clear();
            markDirty(DirtyLayout);
        }

        const std::vector<std::unique_ptr<UiElement>>& children() const { return m_children; }

    private:
        void markSubtreeDirty(uint32_t flags) const
        {
            for (const auto& child : m_children)
            {
                child->dirty |= flags;
                child->markSubtreeDirty(flags);
            }
        }

        std::vector<std::unique_ptr<UiElement>> m_children;
        UiElement* m_parent = nullptr;   // nullptr solo en la raíz del canvas
    };

    // Direcciones de navegación del foco. Next/Previous recorren el árbol;
    // las otras cuatro se resuelven por geometría.
    enum class UiNavDir : uint32_t
    {
        Next,
        Previous,
        Left,
        Right,
        Up,
        Down
    };

    // ── Resolución del canvas ───────────────────────────────────────────────
    // Cómo se convierte el árbol (que se resuelve SIEMPRE en unidades de
    // referencia) a los píxeles del render. Todo CPU y determinista: vale igual
    // en el editor en Play y en el juego exportado.
    enum class UiScaleMode : uint32_t
    {
        ConstantPixelSize,     // 1 unidad = 1 píxel (por scaleFactor)
        ScaleWithScreenSize,   // la escala sale del área útil contra la referencia
        ConstantPhysicalSize   // la escala sale del DPI
    };

    // Cómo se combinan el ratio en X y el ratio en Y en ScaleWithScreenSize.
    enum class UiScreenMatch : uint32_t
    {
        MatchWidthOrHeight,   // lerp logarítmico entre los dos, por matchWidthOrHeight
        Expand,               // el MENOR: nada se sale, pueden sobrar márgenes
        Shrink                // el MAYOR: no sobra nada, puede salirse
    };

    // Insets del safe area, en PÍXELES REALES del render (no en unidades de
    // referencia): quien los rellena los recibe así del SO.
    struct UiSafeArea
    {
        float left   = 0.0f;
        float top    = 0.0f;
        float right  = 0.0f;
        float bottom = 0.0f;
    };

    class UiCanvas
    {
    public:
        UiCanvas();

        UiElement&       root()       { return m_root; }
        const UiElement& root() const { return m_root; }

        bool visible() const { return m_visible; }
        void setVisible(bool v) { m_visible = v; }

        // Vacía la jerarquía. El canvas vuelve a costar cero.
        void clear();

        // width/height son el tamaño del render en píxeles: fijan el scissor
        // raíz y la ortográfica.
        void buildDrawData(uint32_t width, uint32_t height, UiDrawData& out) const;

        // ── Resolución ──────────────────────────────────────────────────────
        // El área útil sale SIEMPRE en este orden: (a) el render entero, (b) se
        // le restan los insets del safe area, (c) si aspectRatio > 0 se recorta
        // CENTRADO a esa relación. De ahí sale una escala ÚNICA y UNIFORME, y
        // esa escala entra una sola vez, al pasar el rect de cada nodo de
        // unidades de referencia a píxeles. El scissor raíz es el área útil:
        // lo que cae en las barras o en el inset NO se dibuja.
        UiScaleMode   scaleMode           = UiScaleMode::ConstantPixelSize;
        float         scaleFactor         = 1.0f;               // multiplica a los tres modos
        glm::vec2     referenceResolution{1920.0f, 1080.0f};    // ScaleWithScreenSize
        UiScreenMatch screenMatch         = UiScreenMatch::MatchWidthOrHeight;
        float         matchWidthOrHeight  = 0.5f;               // 0 = ancho, 1 = alto (se clampa)
        float         screenDpi           = 0.0f;               // 0 = desconocido
        float         fallbackDpi         = 96.0f;              // el que se usa si no se sabe
        float         referenceDpi        = 96.0f;              // ConstantPhysicalSize
        UiSafeArea    safeArea{};                               // en píxeles reales
        float         aspectRatio         = 0.0f;               // 0 = apagado (16/9 = 1.777…)

        // Lo que dejó el último buildDrawData. Solo lectura: para poder probarlo
        // y para que el editor lo pueda enseñar algún día.
        float     uiScale()       const { return m_uiScale; }
        glm::vec2 uiOrigin()      const { return m_uiOrigin; }
        glm::vec2 referenceSize() const { return m_referenceSize; }

        // Cuántos nodos REEMITIERON sus vértices en el último buildDrawData. Los
        // demás se copiaron de su caché. Es la única medida honesta de lo que se
        // está ahorrando: no depende del reloj ni de la máquina.
        uint32_t rebuiltNodes() const { return m_rebuiltNodes; }

        // El reloj del último updateInput. Lo necesita quien anime FUERA del
        // canvas y tenga que ir a compás con lo que anima dentro: el parpadeo
        // del cursor de un campo de texto es el primer caso. Tener un reloj
        // propio ahí seria uno que se separa de este en cuanto los dos avancen
        // por caminos distintos.
        float lastTimeSeconds() const { return m_lastTime; }

        // ── Input ───────────────────────────────────────────────────────────
        // ÚNICO punto de entrada. Va DESPUÉS del layout, o sea después de un
        // buildDrawData: reutiliza los rects que ese dejó en cada elemento y no
        // vuelve a medir nada. Sin buildDrawData previo no hay rects y el hit
        // test no encuentra a nadie (no es un fallo: es un canvas sin colocar).
        void updateInput(const UiInputState& input);

        // Umbrales, aquí y no escondidos en constantes del .cpp: un juego con el
        // ratón y otro con un mando no quieren los mismos números.
        float doubleClickTime     = 0.35f;   // s entre los dos clicks
        float doubleClickDistance = 8.0f;    // px entre los dos clicks
        float dragThreshold       = 5.0f;    // px desde el Down para que sea arrastre

        // Elemento bajo el cursor en el último updateInput.
        UiElement* hovered() const { return m_hovered; }

        // ¿Hay un botón del ratón BAJADO sobre un elemento de este canvas y sin
        // soltar todavía? Es la captura del puntero: mientras dure, el cursor
        // puede irse fuera del widget (y hasta encima de OTRO canvas) sin que el
        // arrastre se corte, que es como se comporta un slider de toda la vida.
        // Lo lee dispatchUiInput para repartir el ratón entre varios canvas.
        bool pointerCaptured() const
        {
            return m_pressTarget[0] != nullptr || m_pressTarget[1] != nullptr ||
                   m_pressTarget[2] != nullptr;
        }

        // Suelta el hover, la captura del puntero y el foco de este canvas, con
        // su MouseExit y su Blur — igual que si el ratón se hubiera ido fuera y
        // el foco a otra parte. NO toca el árbol ni las animaciones.
        //
        // Lo llama quien saca el canvas del reparto de input a media pulsación:
        // hoy, cambiarle `renderMode` a World (es escribible desde Lua). Un
        // canvas que se va con un botón bajado nunca ve el MouseUp y se queda con
        // `m_pressTarget`: al volver entraría con pointerCaptured() en true SIN
        // ningún botón bajado, se llevaría el ratón en el paso 1 de
        // dispatchUiInput y emitiría un MouseUp/Click que nadie pidió. Y sin
        // soltar el hover se quedaría además PEGADO en el último que vio, que es
        // el mismo fallo que dispatchUiInput evita para los que pierden el
        // puntero — aquí no puede evitarlo porque el canvas ya no está en su
        // lista.
        void releaseInput();

        // Uno solo por canvas. setFocus emite Blur en el viejo y Focus en el
        // nuevo, EN ESE ORDEN. Ignora los que no son focusable (nullptr sí vale:
        // es soltar el foco).
        UiElement* focused() const { return m_focused; }
        void       setFocus(UiElement* element);

        // Mueve el foco. Devuelve si CAMBIÓ. Aquí no entra ni el teclado ni el
        // mando: quien los lea llama a esto. CPU pura y determinista, así que
        // vale igual en el editor en Play y en el juego exportado.
        //
        // Next/Previous recorren el MISMO orden que el Tab (pre-orden del árbol,
        // saltando lo no focusable, lo invisible y lo deshabilitado) y DAN LA
        // VUELTA. Left/Right/Up/Down salen de los rects que dejó el último
        // buildDrawData: sin un buildDrawData previo no hay geometría y la
        // direccional no encuentra a nadie (igual que el hit test), mientras que
        // Next/Previous siguen funcionando. La direccional NO da la vuelta.
        //
        // Sin foco previo, cualquier dirección entra por el primer focusable en
        // pre-orden. Sin candidato el foco no se mueve y devuelve false.
        bool navigate(UiNavDir dir);

        // ── Navegación por teclado y mando ──────────────────────────────────
        // Con esto encendido, una tecla que NADIE haya consumido mueve el foco
        // (flechas) o activa el elemento enfocado (Enter), igual que el Tab y el
        // Escape ya hacían. Se apaga para un juego que quiera leer las flechas
        // por su cuenta sin que el canvas se le adelante.
        bool keyboardNavigation = true;

        // Dispara el Click del elemento con foco, como si lo hubieran pulsado
        // con el ratón encima. Es lo que permite jugar con MANDO: sin esto el
        // foco se podía mover pero no se podía pulsar nada.
        //
        // Devuelve si llegó a emitirse. No lo hace sin foco, ni sobre un
        // elemento invisible o deshabilitado, ni sobre un botón que no sea
        // interactable — las mismas reglas que se le aplican al ratón.
        bool submitFocused();

        // Qué elemento cae bajo un punto, con las mismas reglas que usa el input:
        // pre-orden INVERSO (gana lo último dibujado), respetando visible, el
        // scissor heredado y raycastTarget.
        UiElement* hitTest(const glm::vec2& point) const;

    private:
        // El único que resuelve la escala es el emisor, y buildDrawData es
        // const: por eso los tres resultados son mutable y él es amigo.
        friend class UiSpriteBatch;

        void dispatch(UiElement* target, UiEvent& event, UiEventHandler UiElement::* slot) const;
        void moveFocus(int direction);

        UiElement m_root{"Canvas"};
        bool   m_visible = true;

        // Resultado del último buildDrawData. Neutros mientras no haya habido
        // ninguno: escala 1, sin desplazamiento y sin área.
        mutable float     m_uiScale = 1.0f;
        mutable glm::vec2 m_uiOrigin{0.0f, 0.0f};
        mutable glm::vec2 m_referenceSize{0.0f, 0.0f};

        // Resolución del build anterior. Si cualquiera de estos cambia, la
        // colocación de TODOS los nodos cambia y la caché entera sobra.
        mutable uint32_t m_lastWidth  = 0;
        mutable uint32_t m_lastHeight = 0;

        mutable uint32_t m_rebuiltNodes = 0;

        // Estado del input entre frames. Todo puntero aquí apunta DENTRO de
        // m_root, así que clear() los tiene que soltar.
        UiElement* m_hovered = nullptr;
        UiElement* m_focused = nullptr;

        bool       m_buttonDown[3]  = {false, false, false};
        UiElement* m_pressTarget[3] = {nullptr, nullptr, nullptr};
        glm::vec2  m_pressPos[3]    = {};
        bool       m_dragging[3]    = {false, false, false};

        glm::vec2  m_lastMousePos{0.0f, 0.0f};
        bool       m_hasLastMouse = false;

        // Primer click de un posible doble.
        UiElement* m_lastClickTarget = nullptr;
        glm::vec2  m_lastClickPos{0.0f, 0.0f};
        float      m_lastClickTime = 0.0f;

        // Reloj de las animaciones. El avance es el DELTA contra el frame
        // anterior; sin frame anterior (el primer updateInput) no hay avance,
        // que es lo que impide que un timeSeconds grande de arranque se coma
        // una animación entera en el primer frame.
        float m_lastTime    = 0.0f;
        bool  m_hasLastTime = false;
    };

    // Dónde se deja el ratón de un canvas que NO tiene el puntero este frame.
    // Un punto que no cae dentro de ningún rect imaginable: el hit test de ese
    // canvas devuelve nullptr y su hover se limpia solo, con su MouseExit y con
    // sus colores de estado de vuelta a Normal. No vale con "no llamarle": un
    // canvas al que se le deja de dar input se queda PEGADO en el último hover
    // que vio, para siempre.
    inline glm::vec2 uiPointerAway() { return glm::vec2(-1.0e6f, -1.0e6f); }

    // Reparte UN estado de input entre TODOS los canvas de pantalla de la
    // escena. `canvases` va en orden de PRIORIDAD: el de más arriba primero (o
    // sea, el ÚLTIMO que se dibuja, que es el que el usuario ve encima).
    //
    // Llamar a updateInput con el mismo estado en los N canvas NO vale: dos
    // canvas solapados dejarían LOS DOS un widget en hover y un clic activaría
    // dos botones a la vez. El reparto es:
    //
    //   - RATÓN a UNO solo. Se lo queda el que tenga la captura del puntero
    //     (un botón bajado sin soltar, o sea un arrastre en curso); si no hay
    //     ninguno, el primero de la lista que tenga algo bajo el cursor. Los
    //     demás reciben el ratón en uiPointerAway() y con los botones sueltos,
    //     que es lo que les limpia el hover en vez de dejárselo pegado.
    //   - TECLADO y MANDO a UNO solo, y NO tiene por qué ser el mismo: el foco
    //     no lo mueve el cursor. Las teclas van al primer canvas de la lista
    //     que TENGA foco, así que escribir en un campo de texto sigue llegando
    //     aunque el ratón se pasee por encima de otro canvas. Si ninguno tiene
    //     foco van al de más arriba (hoy eso no se nota: updateInput ignora las
    //     teclas sin foco).
    //   - El FOCO no salta solo entre canvas: se mueve al clicar. El Tab y las
    //     flechas dan la vuelta DENTRO del canvas que lo tiene, como siempre.
    //     Cuando el canvas que tiene el puntero coge foco, los demás lo sueltan,
    //     que es lo que impide dos anillos de foco a la vez.
    //
    // Todos los canvas reciben updateInput, incluidos los que no ven nada: es
    // ahí donde corren sus animaciones y el fundido de color de sus botones.
    void dispatchUiInput(const std::vector<UiCanvas*>& canvases, const UiInputState& input);

    // Nodo vivo por NOMBRE en TODO el subárbol de `node`, o nullptr si no hay
    // ninguno. Recorrido COMPLETO y no solo los hijos directos: desde que el
    // sync respeta la jerarquía de la escena, el nodo de un widget anidado
    // cuelga del de su padre, y buscarlo a un solo nivel dejaría sin
    // encontrar nada que no fuera de primer nivel.
    //
    // Vivía como función local (findUiNodeNamed) en ViewportPanel.cpp, que
    // solo miraba el canvas de pantalla. Se mueve aquí, libre, para que la
    // use también el Renderer (findUiNode, que recorre TODOS los canvas de
    // la escena, no solo el de pantalla) sin que el editor y el motor tengan
    // cada uno su copia.
    inline const UiElement* findUiNodeIn(const UiElement& node, const std::string& wanted)
    {
        for (const auto& child : node.children())
        {
            if (child->name == wanted) return child.get();
            if (const UiElement* hit = findUiNodeIn(*child, wanted)) return hit;
        }
        return nullptr;
    }
}
