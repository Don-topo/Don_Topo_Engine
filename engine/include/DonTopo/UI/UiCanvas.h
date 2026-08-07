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
        Down
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
        KeyDown
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
        // Para el input futuro: NO afecta al dibujado.
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

        // ── Rect resuelto por el último buildDrawData ───────────────────────
        // El layout ya calcula estos tres valores por nodo; el input los REUSA
        // en vez de volver a medir el árbol (medirlo dos veces con dos códigos
        // distintos es la forma de que diverjan). rectValid queda a false en los
        // nodos que el emisor no llegó a visitar: invisibles y recortados a cero.
        mutable glm::vec2 screenPos{0.0f, 0.0f};
        mutable glm::vec2 screenSize{0.0f, 0.0f};
        mutable UiScissor screenScissor{};
        mutable bool      rectValid = false;

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
            return static_cast<T&>(*m_children.back());
        }

        void clearChildren() { m_children.clear(); }

        const std::vector<std::unique_ptr<UiElement>>& children() const { return m_children; }

    private:
        std::vector<std::unique_ptr<UiElement>> m_children;
        UiElement* m_parent = nullptr;   // nullptr solo en la raíz del canvas
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

        // Uno solo por canvas. setFocus emite Blur en el viejo y Focus en el
        // nuevo, EN ESE ORDEN. Ignora los que no son focusable (nullptr sí vale:
        // es soltar el foco).
        UiElement* focused() const { return m_focused; }
        void       setFocus(UiElement* element);

        // Qué elemento cae bajo un punto, con las mismas reglas que usa el input:
        // pre-orden INVERSO (gana lo último dibujado), respetando visible, el
        // scissor heredado y raycastTarget.
        UiElement* hitTest(const glm::vec2& point) const;

    private:
        void dispatch(UiElement* target, UiEvent& event, UiEventHandler UiElement::* slot) const;
        void moveFocus(int direction);

        UiElement m_root{"Canvas"};
        bool   m_visible = true;

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
}
