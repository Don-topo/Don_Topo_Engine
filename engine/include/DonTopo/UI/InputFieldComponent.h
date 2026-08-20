#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiFont.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    class InputFieldComponent;

    // Qué se deja teclear. El filtro va donde se ESCRIBE, no donde se dibuja:
    // filtrando solo la pintura, el componente guardaría basura y un script la
    // leería tal cual.
    enum class UiInputContentType
    {
        Standard,        // cualquier carácter imprimible
        IntegerNumber,   // dígitos y un signo al principio
        DecimalNumber,   // lo anterior más UN separador decimal
        Alphanumeric,    // letras y dígitos, sin espacios ni puntuación
        Password         // se guarda tal cual y se ENSEÑA enmascarado
    };

    // Lo que un campo tiene EN VIVO y no se serializa. Mismo papel y mismos
    // motivos que UiButtonRuntime: el nodo del canvas lo destruye clearChildren()
    // en cada reconstrucción, así que el dueño del callback es el COMPONENTE.
    struct UiInputFieldRuntime
    {
        std::function<void(const std::string&)> onValueChanged;
        // Al confirmar (Enter) o al perder el foco. Es el momento en el que un
        // formulario valida, no cada tecla.
        std::function<void(const std::string&)> onEndEdit;
        InputFieldComponent*                    owner = nullptr;
    };

    struct UiInputFieldCallbackSlot
    {
        std::shared_ptr<UiInputFieldRuntime> ptr = std::make_shared<UiInputFieldRuntime>();

        UiInputFieldCallbackSlot() = default;
        UiInputFieldCallbackSlot(const UiInputFieldCallbackSlot&) {}
        UiInputFieldCallbackSlot& operator=(const UiInputFieldCallbackSlot&) { return *this; }
        UiInputFieldCallbackSlot(UiInputFieldCallbackSlot&&) = default;
        UiInputFieldCallbackSlot& operator=(UiInputFieldCallbackSlot&&) = default;

        bool operator==(const UiInputFieldCallbackSlot&) const { return true; }
    };

    // Un campo de texto de la UI 2D como componente de GameObject, con el MISMO
    // contrato que el resto: SOLO DATOS. El InputField del núcleo (UiWidgets.h)
    // es un stub SIN campos, así que el widget se monta por COMPOSICIÓN: la caja
    // es el nodo raíz (de tipo InputField) y de ella cuelgan el texto y el
    // cursor.
    //
    // Es el único widget que necesitaba algo que el core NO tenía: un canal de
    // CARACTERES. UiKey nombra teclas físicas con significado propio (Tab, Enter,
    // flechas) y una 'a' no es una de esas: sale del layout del teclado y de las
    // muertas. Por eso se añadió UiInputState::chars + UiElement::onTextInput,
    // que es infraestructura del canvas y no de este componente — cualquier cosa
    // futura que reciba texto (una consola, un chat, un buscador) usa la misma.
    class InputFieldComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};    // px, relativa al ancla
            glm::vec2 size{200.0f, 32.0f};     // px
            glm::vec4 color{0.15f, 0.15f, 0.15f, 1.0f};   // color de la CAJA
            bool      visible = true;

            // A false ni siquiera toma el foco. readOnly SÍ lo toma y deja mover
            // el cursor, pero no cambiar el texto: es lo que quiere un campo de
            // "copia esto de aquí".
            bool interactable = true;
            bool readOnly     = false;

            // --- Texto ----------------------------------------------------------
            // UTF-8, igual que TextComponent. El cursor se cuenta en CODEPOINTS
            // (ver caretPos).
            std::string text;
            std::string placeholder;

            std::string fontPath;   // TTF; vacía = la fuente por defecto
            float       fontSize = 16.0f;
            glm::vec4   textColor{1.0f, 1.0f, 1.0f, 1.0f};
            glm::vec4   placeholderColor{0.6f, 0.6f, 0.6f, 1.0f};
            UiTextAlign align = UiTextAlign::Left;

            // Píxeles que el texto y el cursor se meten hacia dentro de la caja
            // por izquierda y derecha.
            float padding = 6.0f;

            // 0 = sin límite. Cuenta CARACTERES, no bytes: con UTF-8 un límite en
            // bytes daría un máximo distinto según lo que se escriba.
            uint32_t characterLimit = 0;

            UiInputContentType contentType = UiInputContentType::Standard;

            // Con qué se enmascara en Password. Vacío cae al asterisco: un campo
            // de contraseña que no enseña NADA parece roto.
            std::string passwordChar = "*";

            // --- Cursor ---------------------------------------------------------
            glm::vec4 caretColor{1.0f, 1.0f, 1.0f, 1.0f};
            float     caretWidth     = 1.0f;
            float     caretBlinkRate = 0.5f;   // segundos por medio ciclo; 0 = fijo

            // Posición del cursor en CODEPOINTS desde el principio. NO se
            // serializa (un campo cargado empieza con el cursor donde lo ponga el
            // sync), pero SÍ entra en operator==: es lo que hace que el sync
            // vuelva a colocar el nodo del cursor cuando se mueve.
            int caretPos = 0;

            // --- Sprites --------------------------------------------------------
            std::string atlasPath;
            std::string backgroundSprite;

            // --- Runtime (no se serializa) --------------------------------------
            UiInputFieldCallbackSlot callbacks;

            // --- Utilidades de texto --------------------------------------------
            std::vector<uint32_t> codepoints() const { return UiFont::decodeUtf8(text); }
            int codepointCount() const { return (int)UiFont::decodeUtf8(text).size(); }

            bool isShowingPlaceholder() const { return text.empty(); }

            // Lo que se DIBUJA. En Password devuelve un símbolo por CARÁCTER (no
            // por byte) y nunca la contraseña: guardar el enmascarado sería
            // perderla.
            std::string displayText() const
            {
                if (text.empty()) return placeholder;
                if (contentType != UiInputContentType::Password) return text;

                const std::string mask = passwordChar.empty() ? std::string("*") : passwordChar;
                std::string out;
                const size_t n = UiFont::decodeUtf8(text).size();
                out.reserve(mask.size() * n);
                for (size_t i = 0; i < n; i++) out += mask;
                return out;
            }

            // ¿Se deja teclear este carácter? El signo y el separador decimal
            // dependen de lo que YA hay y de dónde está el cursor, así que esto
            // no es una tabla: "1-2" no es un entero y "1.5.5" no es un decimal.
            bool accepts(uint32_t cp) const
            {
                // Los de control NUNCA: un '\n' o un tabulador dentro de una
                // línea no se ve y desplaza todo lo que venga detrás.
                if (cp < 0x20u || cp == 0x7Fu) return false;

                const bool digito = (cp >= '0' && cp <= '9');

                switch (contentType)
                {
                    case UiInputContentType::IntegerNumber:
                        if (digito) return true;
                        // El signo solo pegado al principio.
                        return (cp == '-' || cp == '+') && caretPos == 0 && !hasSign();

                    case UiInputContentType::DecimalNumber:
                        if (digito) return true;
                        if (cp == '-' || cp == '+') return caretPos == 0 && !hasSign();
                        // UN solo separador decimal.
                        return (cp == '.' || cp == ',') && !hasDecimalSeparator();

                    case UiInputContentType::Alphanumeric:
                        // Solo ASCII a propósito: "alfanumérico" fuera del ASCII
                        // no tiene una respuesta única (¿la eñe? ¿los ideogramas?)
                        // y adivinarla sería peor que no ofrecer el modo.
                        return digito || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');

                    default:
                        return true;
                }
            }

            // Mete el carácter en el cursor. false = no se aceptó (filtro, límite
            // o solo lectura) y NADA cambió.
            bool insertCodepoint(uint32_t cp)
            {
                if (readOnly) return false;
                if (!accepts(cp)) return false;

                std::vector<uint32_t> cps = UiFont::decodeUtf8(text);
                if (characterLimit != 0 && cps.size() >= (size_t)characterLimit) return false;

                const int pos = std::clamp(caretPos, 0, (int)cps.size());
                cps.insert(cps.begin() + pos, cp);
                text     = encodeUtf8(cps);
                caretPos = pos + 1;
                return true;
            }

            bool backspace()
            {
                if (readOnly) return false;
                std::vector<uint32_t> cps = UiFont::decodeUtf8(text);
                const int pos = std::clamp(caretPos, 0, (int)cps.size());
                if (pos == 0) return false;
                cps.erase(cps.begin() + (pos - 1));
                text     = encodeUtf8(cps);
                caretPos = pos - 1;
                return true;
            }

            bool deleteForward()
            {
                if (readOnly) return false;
                std::vector<uint32_t> cps = UiFont::decodeUtf8(text);
                const int pos = std::clamp(caretPos, 0, (int)cps.size());
                if (pos >= (int)cps.size()) return false;
                cps.erase(cps.begin() + pos);
                text     = encodeUtf8(cps);
                caretPos = pos;
                return true;
            }

            void moveCaret(int delta)
            {
                caretPos = std::clamp(caretPos + delta, 0, codepointCount());
            }
            void caretHome() { caretPos = 0; }
            void caretEnd()  { caretPos = codepointCount(); }

            // Vuelca el rect y la caja en el nodo vivo. NO toca `atlas` (es un
            // puntero a GPU: lo resuelve el sync).
            void applyTo(InputField& f) const
            {
                f.anchorMin = anchorMin;
                f.anchorMax = anchorMax;
                f.pivot     = pivot;
                f.position  = position;
                f.size      = size;
                f.color     = color;
                f.visible   = visible;
                f.sprite    = backgroundSprite;
                f.raycastTarget = true;
                // Sin foco no hay donde escribir. A false ni se enfoca con el
                // ratón ni entra en el recorrido del Tab, que es exactamente lo
                // que quiere un campo deshabilitado.
                f.focusable = interactable;
            }

            // El nodo del texto. La fuente la resuelve el sync (es un recurso de
            // GPU), y el color sale de si hay texto o placeholder.
            void applyToText(Text& t) const
            {
                t.anchorMin = glm::vec2(0.0f);
                t.anchorMax = glm::vec2(0.0f);
                t.pivot     = glm::vec2(0.0f);
                t.position  = glm::vec2(padding, 0.0f);
                t.size      = glm::vec2(std::max(size.x - 2.0f * padding, 0.0f), size.y);
                t.text      = displayText();
                t.fontSize  = fontSize;
                t.color     = isShowingPlaceholder() ? placeholderColor : textColor;
                t.align     = align;
                t.vAlign    = UiTextVAlign::Middle;
                // Lo que no cabe se recorta contra el rect del texto: sin esto,
                // escribir de más se sale de la caja y pinta por encima de lo que
                // haya al lado.
                t.overflow  = UiTextOverflow::Clip;
                t.wordWrap  = false;
                t.visible   = true;
                t.raycastTarget = false;
            }

            // El cursor. `x` en píxeles desde el borde izquierdo del rect lo mide
            // el sync con la fuente resuelta: el componente no sabe de métricas.
            // `mostrar` es el foco más la fase del parpadeo.
            void applyToCaret(UiElement& c, float x, bool mostrar) const
            {
                c.anchorMin = glm::vec2(0.0f);
                c.anchorMax = glm::vec2(0.0f);
                c.pivot     = glm::vec2(0.0f);
                c.position  = glm::vec2(padding + x, size.y * 0.15f);
                c.size      = glm::vec2(std::max(caretWidth, 0.0f), size.y * 0.7f);
                c.color     = caretColor;
                c.visible   = true;
                // Existe SIEMPRE aunque no se vea: si apareciera y desapareciera
                // cambiaría la forma del subárbol y habría que reconstruir la
                // raíz del canvas en cada parpadeo.
                c.drawable  = mostrar && c.size.x > 0.0f && c.size.y > 0.0f;
                c.raycastTarget = false;
            }

            // El sync lo usa para saber si hay algo que volcar.
            bool operator==(const InputFieldComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       interactable == o.interactable && readOnly == o.readOnly &&
                       text == o.text && placeholder == o.placeholder &&
                       fontPath == o.fontPath && fontSize == o.fontSize &&
                       textColor == o.textColor && placeholderColor == o.placeholderColor &&
                       align == o.align && padding == o.padding &&
                       characterLimit == o.characterLimit && contentType == o.contentType &&
                       passwordChar == o.passwordChar &&
                       caretColor == o.caretColor && caretWidth == o.caretWidth &&
                       caretBlinkRate == o.caretBlinkRate && caretPos == o.caretPos &&
                       atlasPath == o.atlasPath && backgroundSprite == o.backgroundSprite;
            }
            bool operator!=(const InputFieldComponent& o) const { return !(*this == o); }

        private:
            bool hasSign() const
            {
                return !text.empty() && (text[0] == '-' || text[0] == '+');
            }

            bool hasDecimalSeparator() const
            {
                return text.find('.') != std::string::npos ||
                       text.find(',') != std::string::npos;
            }

            // Codepoints -> UTF-8. UiFont trae el camino de ida (decodeUtf8) pero
            // no el de vuelta: hasta ahora nadie CONSTRUÍA texto, solo lo leía.
            static std::string encodeUtf8(const std::vector<uint32_t>& cps)
            {
                std::string out;
                out.reserve(cps.size());
                for (uint32_t cp : cps)
                {
                    // Los sustitutos y lo que pase de U+10FFFF no son codepoints
                    // válidos: se cambian por U+FFFD en vez de emitir bytes que
                    // ningún decodificador aceptaría.
                    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFDu;

                    if (cp < 0x80u)
                    {
                        out += (char)cp;
                    }
                    else if (cp < 0x800u)
                    {
                        out += (char)(0xC0u | (cp >> 6));
                        out += (char)(0x80u | (cp & 0x3Fu));
                    }
                    else if (cp < 0x10000u)
                    {
                        out += (char)(0xE0u | (cp >> 12));
                        out += (char)(0x80u | ((cp >> 6) & 0x3Fu));
                        out += (char)(0x80u | (cp & 0x3Fu));
                    }
                    else
                    {
                        out += (char)(0xF0u | (cp >> 18));
                        out += (char)(0x80u | ((cp >> 12) & 0x3Fu));
                        out += (char)(0x80u | ((cp >> 6) & 0x3Fu));
                        out += (char)(0x80u | (cp & 0x3Fu));
                    }
                }
                return out;
            }
    };

    // Nombre del nodo vivo de un InputField dentro del canvas. Prefijo DISTINTO
    // al de los demás, por lo de siempre.
    inline std::string uiInputFieldNodeName(uint64_t ownerId)
    {
        return "inp:" + std::to_string(ownerId);
    }

    // Inversa de uiInputFieldNodeName. Devuelve 0 si el nombre no es de un campo.
    // El corte por '/' hace que el texto y el cursor devuelvan también a su dueño.
    inline uint64_t uiInputFieldOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("inp:", 0) != 0) return 0;
        uint64_t id = 0;
        for (size_t i = 4; i < nodeName.size(); i++)
        {
            const char c = nodeName[i];
            if (c == '/') break;
            if (c < '0' || c > '9') return 0;
            id = id * 10 + (uint64_t)(c - '0');
        }
        return id;
    }
}
