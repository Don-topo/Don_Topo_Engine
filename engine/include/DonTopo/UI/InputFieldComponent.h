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

    // What is allowed to be typed. The filter goes where it is WRITTEN, not where it is drawn:
    // if only the painting were filtered, the component would store garbage and a script would
    // read it as is.
    enum class UiInputContentType
    {
        Standard,        // any printable character
        IntegerNumber,   // digits and a sign at the start
        DecimalNumber,   // the above plus ONE decimal separator
        Alphanumeric,    // letters and digits, no spaces or punctuation
        Password         // stored as is and SHOWN masked
    };

    // What a field has LIVE and is not serialized. Same role and same
    // reasons as UiButtonRuntime: the canvas node is destroyed by clearChildren()
    // on every rebuild, so the owner of the callback is the COMPONENT.
    struct UiInputFieldRuntime
    {
        std::function<void(const std::string&)> onValueChanged;
        // On confirm (Enter) or on losing focus. It is the moment at which a
        // form validates, not every key.
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

    // A 2D UI text field as a GameObject component, with the SAME
    // contract as the rest: DATA ONLY. The core InputField (UiWidgets.h)
    // is a stub with NO fields, so the widget is assembled by COMPOSITION: the box
    // is the root node (of type InputField) and the text and the
    // caret hang from it.
    //
    // It is the only widget that needed something the core did NOT have: a
    // CHARACTER channel. UiKey names physical keys with a meaning of their own (Tab, Enter,
    // arrows) and an 'a' is not one of those: it comes from the keyboard layout and from
    // dead keys. That is why UiInputState::chars + UiElement::onTextInput were added,
    // which is canvas infrastructure and not this component's: anything
    // future that receives text (a console, a chat, a search box) uses the same one.
    class InputFieldComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};    // px, relative to the anchor
            glm::vec2 size{200.0f, 32.0f};     // px
            glm::vec4 color{0.15f, 0.15f, 0.15f, 1.0f};   // color of the BOX
            bool      visible = true;

            // When false it does not even take focus. readOnly DOES take it and lets the
            // caret move, but not the text change: it is what a "copy this from
            // here" field wants.
            bool interactable = true;
            bool readOnly     = false;

            // --- Text -----------------------------------------------------------
            // UTF-8, same as TextComponent. The caret is counted in CODEPOINTS
            // (see caretPos).
            std::string text;
            std::string placeholder;

            std::string fontPath;   // TTF; empty = the default font
            float       fontSize = 16.0f;
            glm::vec4   textColor{1.0f, 1.0f, 1.0f, 1.0f};
            glm::vec4   placeholderColor{0.6f, 0.6f, 0.6f, 1.0f};
            UiTextAlign align = UiTextAlign::Left;

            // Pixels that the text and the caret are inset into the box
            // on the left and right.
            float padding = 6.0f;

            // 0 = no limit. It counts CHARACTERS, not bytes: with UTF-8 a limit in
            // bytes would give a different maximum depending on what is typed.
            uint32_t characterLimit = 0;

            UiInputContentType contentType = UiInputContentType::Standard;

            // What is used to mask in Password. Empty falls back to the asterisk: a password
            // field that shows NOTHING looks broken.
            std::string passwordChar = "*";

            // --- Cursor ---------------------------------------------------------
            glm::vec4 caretColor{1.0f, 1.0f, 1.0f, 1.0f};
            float     caretWidth     = 1.0f;
            float     caretBlinkRate = 0.5f;   // seconds per half cycle; 0 = fixed

            // Caret position in CODEPOINTS from the start. It is NOT
            // serialized (a loaded field starts with the caret wherever the
            // sync puts it), but it DOES go into operator==: this is what makes the sync
            // place the caret node again when it moves.
            int caretPos = 0;

            // --- Sprites --------------------------------------------------------
            std::string atlasPath;
            std::string backgroundSprite;

            // --- Runtime (not serialized) ---------------------------------------
            UiInputFieldCallbackSlot callbacks;

            // --- Text utilities -------------------------------------------------
            std::vector<uint32_t> codepoints() const { return UiFont::decodeUtf8(text); }
            int codepointCount() const { return (int)UiFont::decodeUtf8(text).size(); }

            bool isShowingPlaceholder() const { return text.empty(); }

            // What is DRAWN. In Password it returns one symbol per CHARACTER (not
            // per byte) and never the password: storing the masked version would
            // lose it.
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

            // Is this character allowed to be typed? The sign and the decimal separator
            // depend on what is ALREADY there and where the caret is, so this
            // is not a table: "1-2" is not an integer and "1.5.5" is not a decimal.
            bool accepts(uint32_t cp) const
            {
                // Control characters NEVER: a '\n' or a tab inside a
                // line is not visible and shifts everything that comes after it.
                if (cp < 0x20u || cp == 0x7Fu) return false;

                const bool digito = (cp >= '0' && cp <= '9');

                switch (contentType)
                {
                    case UiInputContentType::IntegerNumber:
                        if (digito) return true;
                        // The sign only stuck to the start.
                        return (cp == '-' || cp == '+') && caretPos == 0 && !hasSign();

                    case UiInputContentType::DecimalNumber:
                        if (digito) return true;
                        if (cp == '-' || cp == '+') return caretPos == 0 && !hasSign();
                        // A single decimal separator.
                        return (cp == '.' || cp == ',') && !hasDecimalSeparator();

                    case UiInputContentType::Alphanumeric:
                        // ASCII only on purpose: "alphanumeric" outside ASCII
                        // has no single answer (the eñe? ideograms?)
                        // and guessing it would be worse than not offering the mode.
                        return digito || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');

                    default:
                        return true;
                }
            }

            // Puts the character at the caret. false = it was not accepted (filter, limit
            // or read-only) and NOTHING changed.
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

            // Dumps the rect and the box into the live node. Does NOT touch `atlas` (it is a
            // GPU pointer: the sync resolves it).
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
                // Without focus there is nowhere to type. When false it is neither focused with the
                // mouse nor part of the Tab traversal, which is exactly what
                // a disabled field wants.
                f.focusable = interactable;
            }

            // The text node. The font is resolved by the sync (it is a GPU
            // resource), and the color depends on whether there is text or a placeholder.
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
                // What does not fit is clipped against the text rect: without this,
                // typing too much sticks out of the box and paints over whatever is
                // next to it.
                t.overflow  = UiTextOverflow::Clip;
                t.wordWrap  = false;
                t.visible   = true;
                t.raycastTarget = false;
            }

            // The caret. `x` in pixels from the left edge of the rect is measured by the
            // sync with the resolved font: the component knows nothing about metrics.
            // `mostrar` is the focus plus the blink phase.
            void applyToCaret(UiElement& c, float x, bool mostrar) const
            {
                c.anchorMin = glm::vec2(0.0f);
                c.anchorMax = glm::vec2(0.0f);
                c.pivot     = glm::vec2(0.0f);
                c.position  = glm::vec2(padding + x, size.y * 0.15f);
                c.size      = glm::vec2(std::max(caretWidth, 0.0f), size.y * 0.7f);
                c.color     = caretColor;
                c.visible   = true;
                // It ALWAYS exists even if it is not seen: if it appeared and disappeared
                // it would change the shape of the subtree and the canvas root would have to be
                // rebuilt on every blink.
                c.drawable  = mostrar && c.size.x > 0.0f && c.size.y > 0.0f;
                c.raycastTarget = false;
            }

            // The sync uses it to know whether there is anything to dump.
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

            // Codepoints -> UTF-8. UiFont has the way there (decodeUtf8) but
            // not the way back: until now nobody BUILT text, only read it.
            static std::string encodeUtf8(const std::vector<uint32_t>& cps)
            {
                std::string out;
                out.reserve(cps.size());
                for (uint32_t cp : cps)
                {
                    // Surrogates and anything above U+10FFFF are not valid
                    // codepoints: they are replaced by U+FFFD instead of emitting bytes that
                    // no decoder would accept.
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

    // Name of an InputField's live node inside the canvas. DIFFERENT prefix
    // from the others, for the usual reason.
    inline std::string uiInputFieldNodeName(uint64_t ownerId)
    {
        return "inp:" + std::to_string(ownerId);
    }

    // Inverse of uiInputFieldNodeName. Returns 0 if the name is not a field's.
    // Cutting at '/' makes the text and the caret also return their owner.
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
