#include "DonTopo/Scripting/LuaApiReference.h"

#include <utility>

namespace DonTopo {

namespace {

// Base estática: lo que registra ScriptBindings. Las acciones del panel Input
// Actions no van aquí — cambian en caliente, las publica el editor.
const std::vector<std::string>& baseSymbols()
{
    static const std::vector<std::string> symbols = {
        // Keywords Lua
        "and", "break", "do", "else", "elseif", "end", "false", "for",
        "function", "goto", "if", "in", "local", "nil", "not", "or",
        "repeat", "return", "then", "true", "until", "while",

        // Globals
        "print",
        // DestroyGameObject(entity) — destruye el GameObject y su subtree en
        // Play (diferido a fin de frame). Ver Scene / README.
        "DestroyGameObject",

        // self — instancia del script (parámetro implícito de las funciones
        // Script:Método). 'self.entity' es el único campo inyectado por
        // ScriptManager para todo script (ver ScriptManager.cpp); el resto
        // de campos son los definidos por el propio script (no listables).
        "self", "self.entity",

        // Callbacks de lifecycle — los define el script (function Script:Nombre)
        // y el motor los llama en Play Mode (ver ScriptManager). Awake/Start una
        // vez; Update/LateUpdate cada frame; FixedUpdate a paso fijo; OnDestroy
        // al destruir; OnTrigger* cuando otro collider entra/permanece/sale de
        // un collider Is Trigger (reciben la Entity que lo provocó).
        "Awake", "Start", "Update", "FixedUpdate", "LateUpdate", "OnDestroy",
        "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit",

        // Log
        "Log.Info", "Log.Warn", "Log.Error",

        // Input / Key / MouseButton
        "Input.IsKeyDown", "Input.IsKeyPressed", "Input.IsKeyReleased",
        "Input.IsMouseButtonDown",
        // Acciones con nombre del panel Input Actions. Los snippets con el
        // nombre concreto de cada acción los publica el editor aparte
        // (setLuaApiActionSymbols).
        "Input.IsActionDown", "Input.IsActionPressed", "Input.IsActionReleased",
        "Key.Space", "Key.Enter", "Key.Escape", "Key.Tab",
        "Key.LeftShift", "Key.LeftControl",
        "Key.Up", "Key.Down", "Key.Left", "Key.Right",
        "Key.A", "Key.B", "Key.C", "Key.D", "Key.E", "Key.F", "Key.G",
        "Key.H", "Key.I", "Key.J", "Key.K", "Key.L", "Key.M", "Key.N",
        "Key.O", "Key.P", "Key.Q", "Key.R", "Key.S", "Key.T", "Key.U",
        "Key.V", "Key.W", "Key.X", "Key.Y", "Key.Z",
        "Key.Num0", "Key.Num1", "Key.Num2", "Key.Num3", "Key.Num4",
        "Key.Num5", "Key.Num6", "Key.Num7", "Key.Num8", "Key.Num9",
        "MouseButton.Left", "MouseButton.Right", "MouseButton.Middle",

        // Entity
        "Entity.name", "Entity:IsValid", "Entity:GetTransform",
        "Entity:GetParent", "Entity:GetChildren", "Entity:GetComponent",
        "Entity:AddComponent", "Entity:RemoveComponent",
        // UI: atajos con nombre para cada componente de UI (registerUi).
        // Los Get* devuelven nil si el componente no está.
        "Entity:GetCanvas", "Entity:GetButton", "Entity:GetText", "Entity:GetProgressBar",
        "Entity:AddCanvas", "Entity:AddButton", "Entity:AddText", "Entity:AddProgressBar",
        "Entity:RemoveCanvas", "Entity:RemoveButton", "Entity:RemoveText",
        "Entity:RemoveProgressBar",
        "Entity:GetPanel", "Entity:AddPanel", "Entity:RemovePanel",
        "Entity:GetImage", "Entity:AddImage", "Entity:RemoveImage",
        "Entity:GetSlider", "Entity:AddSlider", "Entity:RemoveSlider",
        "Entity:GetCheckbox", "Entity:AddCheckbox", "Entity:RemoveCheckbox",
        "Entity:GetToggle", "Entity:AddToggle", "Entity:RemoveToggle",
        "Entity:GetScrollbar", "Entity:AddScrollbar", "Entity:RemoveScrollbar",

        // Transform
        "Transform:GetPosition", "Transform:SetPosition",
        "Transform:GetRotation", "Transform:SetRotation",
        "Transform:GetScale", "Transform:SetScale",
        "Transform:GetWorldPosition", "Transform:Translate", "Transform:Rotate",

        // Colliders (la gravedad/dinámica vive ahora en Rigidbody)
        "BoxCollider:GetHalfExtents", "BoxCollider:SetHalfExtents",
        "BoxCollider:GetCenter", "BoxCollider:SetCenter",
        "SphereCollider:GetRadius", "SphereCollider:SetRadius",
        "SphereCollider:GetCenter", "SphereCollider:SetCenter",
        "CapsuleCollider:GetRadius", "CapsuleCollider:SetRadius",
        "CapsuleCollider:GetHalfHeight", "CapsuleCollider:SetHalfHeight",
        "CapsuleCollider:GetCenter", "CapsuleCollider:SetCenter",
        "PlaneCollider:GetCenter", "PlaneCollider:SetCenter",

        // Rigidbody (dinámica estilo Unity; GetComponent("Rigidbody"))
        "Rigidbody.mass", "Rigidbody.useGravity", "Rigidbody.isKinematic",
        "Rigidbody.drag", "Rigidbody.angularDrag",
        "Rigidbody.velocity", "Rigidbody.angularVelocity",
        "Rigidbody:AddForce", "Rigidbody:AddTorque", "Rigidbody:AddImpulse",

        // Animator (máquina de estados; GetComponent("Animator"))
        "Animator:SetBool", "Animator:GetBool", "Animator:SetTrigger",
        "Animator:SetInt", "Animator:GetInt", "Animator:SetFloat", "Animator:GetFloat",
        "Animator:GetState", "Animator:IsBlending", "Animator:GetBlendWeight",
        "Animator:GetPreviousState", "Animator:GetPoseWeight",

        // AudioClip
        "AudioClip:Play", "AudioClip:Stop", "AudioClip:SetLoop", "AudioClip:GetLoop",
        "AudioClip:SetVolume", "AudioClip:GetVolume", "AudioClip:SetPitch", "AudioClip:GetPitch",
        "AudioClip:SetIs3D", "AudioClip:GetIs3D",

        // UI 2D — Canvas (Entity:GetCanvas / Entity:AddCanvas)
        "Canvas.scaleMode", "Canvas.scaleFactor", "Canvas.screenMatch",
        "Canvas.matchWidthOrHeight", "Canvas.screenDpi", "Canvas.fallbackDpi",
        "Canvas.referenceDpi", "Canvas.aspectRatio",
        "Canvas:GetReferenceResolution", "Canvas:SetReferenceResolution",
        // SafeArea son cuatro insets sueltos: left, top, right, bottom.
        "Canvas:GetSafeArea", "Canvas:SetSafeArea",

        // UI 2D — Button (Entity:GetButton / Entity:AddButton)
        "Button.visible", "Button.atlasPath", "Button.sprite",
        "Button.interactable", "Button.selected", "Button.transition",
        "Button.normalSprite", "Button.hoverSprite", "Button.pressedSprite",
        "Button.disabledSprite", "Button.selectedSprite", "Button.fadeDuration",
        "Button.text", "Button.fontPath", "Button.fontSize", "Button.textAlign",
        "Button:GetAnchorMin", "Button:SetAnchorMin",
        "Button:GetAnchorMax", "Button:SetAnchorMax",
        "Button:GetPivot", "Button:SetPivot",
        "Button:GetPosition", "Button:SetPosition",
        "Button:GetSize", "Button:SetSize",
        "Button:GetColor", "Button:SetColor",
        "Button:GetNormalColor", "Button:SetNormalColor",
        "Button:GetHoverColor", "Button:SetHoverColor",
        "Button:GetPressedColor", "Button:SetPressedColor",
        "Button:GetDisabledColor", "Button:SetDisabledColor",
        "Button:GetSelectedColor", "Button:SetSelectedColor",
        "Button:GetTextColor", "Button:SetTextColor",
        // GetState devuelve un UiButtonState; OnClick/OnDoubleClick registran
        // la función Lua que llama el canvas.
        "Button:GetState", "Button:OnClick", "Button:OnDoubleClick",

        // UI 2D — Text (Entity:GetText / Entity:AddText)
        "Text.visible", "Text.text", "Text.fontPath", "Text.fontSize",
        "Text.outlineWidth", "Text.align", "Text.overflow", "Text.wordWrap",
        "Text.boldStrength", "Text.italicSkew",
        "Text:GetAnchorMin", "Text:SetAnchorMin",
        "Text:GetAnchorMax", "Text:SetAnchorMax",
        "Text:GetPivot", "Text:SetPivot",
        "Text:GetPosition", "Text:SetPosition",
        "Text:GetSize", "Text:SetSize",
        "Text:GetShadowOffset", "Text:SetShadowOffset",
        "Text:GetColor", "Text:SetColor",
        "Text:GetOutlineColor", "Text:SetOutlineColor",
        "Text:GetShadowColor", "Text:SetShadowColor",

        // UI 2D — ProgressBar (Entity:GetProgressBar / Entity:AddProgressBar)
        "ProgressBar.visible", "ProgressBar.value", "ProgressBar.minValue",
        "ProgressBar.maxValue", "ProgressBar.fillDirection",
        "ProgressBar.atlasPath", "ProgressBar.backgroundPath", "ProgressBar.fillPath",
        "ProgressBar:GetAnchorMin", "ProgressBar:SetAnchorMin",
        "ProgressBar:GetAnchorMax", "ProgressBar:SetAnchorMax",
        "ProgressBar:GetPivot", "ProgressBar:SetPivot",
        "ProgressBar:GetPosition", "ProgressBar:SetPosition",
        "ProgressBar:GetSize", "ProgressBar:SetSize",
        "ProgressBar:GetColor", "ProgressBar:SetColor",
        "ProgressBar:GetFillColor", "ProgressBar:SetFillColor",
        // El 0..1 ya acotado que usa el sync para el rect del relleno.
        "ProgressBar:GetNormalizedValue",

        // UI 2D — Layout (el contenedor que coloca a los hijos)
        "Layout.visible", "Layout.mode", "Layout.crossAlign",
        "Layout.paddingLeft", "Layout.paddingRight", "Layout.paddingTop",
        "Layout.paddingBottom", "Layout.columns",
        "Layout.fitWidth", "Layout.fitHeight",
        "Layout.ignoreLayout", "Layout.clipChildren",
        "Layout:GetPosition", "Layout:SetPosition",
        "Layout:GetSize", "Layout:SetSize",
        "Layout:GetAnchorMin", "Layout:SetAnchorMin",
        "Layout:GetAnchorMax", "Layout:SetAnchorMax",
        "Layout:GetPivot", "Layout:SetPivot",
        "Layout:GetSpacing", "Layout:SetSpacing",
        "Layout:GetCellSize", "Layout:SetCellSize",


        // UI 2D — Slider (widget interactivo)
        "Slider.visible", "Slider.interactable", "Slider.value",
        "Slider.minValue", "Slider.maxValue", "Slider.wholeNumbers",
        "Slider.direction", "Slider.handleSize", "Slider.atlasPath",
        "Slider.backgroundSprite", "Slider.fillSprite", "Slider.handleSprite",
        "Slider:GetAnchorMin", "Slider:SetAnchorMin",
        "Slider:GetAnchorMax", "Slider:SetAnchorMax",
        "Slider:GetPivot", "Slider:SetPivot",
        "Slider:GetPosition", "Slider:SetPosition",
        "Slider:GetSize", "Slider:SetSize",
        "Slider:GetColor", "Slider:SetColor",
        "Slider:GetFillColor", "Slider:SetFillColor",
        "Slider:GetHandleColor", "Slider:SetHandleColor",
        "Slider:GetNormalizedValue", "Slider:OnValueChanged",

        // UI 2D — Checkbox (widget interactivo)
        "Checkbox.visible", "Checkbox.interactable", "Checkbox.isOn",
        "Checkbox.checkPadding", "Checkbox.atlasPath",
        "Checkbox.backgroundSprite", "Checkbox.checkmarkSprite",
        "Checkbox:GetAnchorMin", "Checkbox:SetAnchorMin",
        "Checkbox:GetAnchorMax", "Checkbox:SetAnchorMax",
        "Checkbox:GetPivot", "Checkbox:SetPivot",
        "Checkbox:GetPosition", "Checkbox:SetPosition",
        "Checkbox:GetSize", "Checkbox:SetSize",
        "Checkbox:GetColor", "Checkbox:SetColor",
        "Checkbox:GetCheckColor", "Checkbox:SetCheckColor",
        "Checkbox:OnValueChanged",

        // UI 2D — Toggle (widget interactivo)
        "Toggle.visible", "Toggle.interactable", "Toggle.isOn",
        "Toggle.knobSize", "Toggle.knobPadding", "Toggle.atlasPath",
        "Toggle.backgroundSprite", "Toggle.knobSprite",
        "Toggle:GetAnchorMin", "Toggle:SetAnchorMin",
        "Toggle:GetAnchorMax", "Toggle:SetAnchorMax",
        "Toggle:GetPivot", "Toggle:SetPivot",
        "Toggle:GetPosition", "Toggle:SetPosition",
        "Toggle:GetSize", "Toggle:SetSize",
        "Toggle:GetOffColor", "Toggle:SetOffColor",
        "Toggle:GetOnColor", "Toggle:SetOnColor",
        "Toggle:GetKnobColor", "Toggle:SetKnobColor",
        "Toggle:OnValueChanged",

        // UI 2D — Scrollbar (widget interactivo)
        "Scrollbar.visible", "Scrollbar.interactable", "Scrollbar.value",
        "Scrollbar.handleFraction", "Scrollbar.direction",
        "Scrollbar.numberOfSteps", "Scrollbar.scrollStep",
        "Scrollbar.atlasPath", "Scrollbar.backgroundSprite", "Scrollbar.handleSprite",
        "Scrollbar:GetAnchorMin", "Scrollbar:SetAnchorMin",
        "Scrollbar:GetAnchorMax", "Scrollbar:SetAnchorMax",
        "Scrollbar:GetPivot", "Scrollbar:SetPivot",
        "Scrollbar:GetPosition", "Scrollbar:SetPosition",
        "Scrollbar:GetSize", "Scrollbar:SetSize",
        "Scrollbar:GetColor", "Scrollbar:SetColor",
        "Scrollbar:GetHandleColor", "Scrollbar:SetHandleColor",
        "Scrollbar:SnapValue", "Scrollbar:OnValueChanged",

        // UI 2D — Panel (el rectángulo de fondo)
        "Panel.visible", "Panel.raycastTarget", "Panel.atlasPath", "Panel.sprite",
        "Panel:GetAnchorMin", "Panel:SetAnchorMin",
        "Panel:GetAnchorMax", "Panel:SetAnchorMax",
        "Panel:GetPivot", "Panel:SetPivot",
        "Panel:GetPosition", "Panel:SetPosition",
        "Panel:GetSize", "Panel:SetSize",
        "Panel:GetColor", "Panel:SetColor",

        // UI 2D — Image (sprite con Normal/Tiled/Sliced/Filled)
        "Image.visible", "Image.raycastTarget", "Image.atlasPath", "Image.sprite",
        "Image.mode", "Image.borderLeft", "Image.borderRight", "Image.borderTop",
        "Image.borderBottom", "Image.fillCenter", "Image.maxTiles",
        "Image.fillDirection", "Image.fillOrigin", "Image.fillAmount",
        "Image:GetAnchorMin", "Image:SetAnchorMin",
        "Image:GetAnchorMax", "Image:SetAnchorMax",
        "Image:GetPivot", "Image:SetPivot",
        "Image:GetPosition", "Image:SetPosition",
        "Image:GetSize", "Image:SetSize",
        "Image:GetColor", "Image:SetColor",

        // UI 2D — enums (tablas de enteros que registra registerUi)
        "UiScaleMode.ConstantPixelSize", "UiScaleMode.ScaleWithScreenSize",
        "UiScaleMode.ConstantPhysicalSize",
        "UiScreenMatch.MatchWidthOrHeight", "UiScreenMatch.Expand",
        "UiScreenMatch.Shrink",
        "UiTextAlign.Left", "UiTextAlign.Center", "UiTextAlign.Right",
        "UiTextAlign.Justify",
        "UiTextOverflow.Overflow", "UiTextOverflow.Clip", "UiTextOverflow.Ellipsis",
        "UiProgressFillDirection.LeftToRight", "UiProgressFillDirection.RightToLeft",
        "UiProgressFillDirection.BottomToTop", "UiProgressFillDirection.TopToBottom",
        "UiLayoutMode.None", "UiLayoutMode.Horizontal", "UiLayoutMode.Vertical",
        "UiLayoutMode.Grid",
        "UiCrossAlign.Start", "UiCrossAlign.Center", "UiCrossAlign.End",
        "UiImageMode.Normal", "UiImageMode.Tiled", "UiImageMode.Sliced",
        "UiImageMode.Filled",
        "UiFillDirection.Horizontal", "UiFillDirection.Vertical",
        "UiFillOrigin.Start", "UiFillOrigin.End",
        "UiSliderDirection.LeftToRight", "UiSliderDirection.RightToLeft",
        "UiSliderDirection.BottomToTop", "UiSliderDirection.TopToBottom",
        "UiScrollbarDirection.LeftToRight", "UiScrollbarDirection.RightToLeft",
        "UiScrollbarDirection.TopToBottom", "UiScrollbarDirection.BottomToTop",
        "UiButtonTransition.ColorTint", "UiButtonTransition.SpriteSwap",
        "UiButtonTransition.Animation",
        "UiButtonState.Normal", "UiButtonState.Hover", "UiButtonState.Pressed",
        "UiButtonState.Disabled", "UiButtonState.Selected",

        // Scene
        "Scene.Find", "Scene.CreateGameObject", "Scene.Destroy", "Scene.Instantiate",

        // Physics — consultas de rayo (nil / false si no hay escena de física,
        // es decir fuera de Play). Los nombres sueltos son los campos de la
        // tabla 'options' y los de la tabla que devuelve Raycast.
        "Physics.Raycast", "Physics.RaycastHit",
        "hitTriggers", "static", "dynamic", "ignore",
        "entity", "point", "normal", "distance",

        // Motor (cambio de escena en runtime)
        "DonTopo.loadScene",

        // Vec3
        "Vec3.new",
    };
    return symbols;
}

// Snippets por acción publicados por el editor; vacío en el runtime exportado.
std::vector<std::string> g_actionSymbols;
// base + dinámicas; se reconstruye solo cuando cambian las dinámicas.
std::vector<std::string> g_combined;
bool g_combinedDirty = true;

} // namespace

const std::vector<std::string>& luaApiSymbols()
{
    if (g_combinedDirty)
    {
        g_combined = baseSymbols();
        g_combined.insert(g_combined.end(), g_actionSymbols.begin(), g_actionSymbols.end());
        g_combinedDirty = false;
    }
    return g_combined;
}

void setLuaApiActionSymbols(std::vector<std::string> symbols)
{
    g_actionSymbols = std::move(symbols);
    g_combinedDirty = true;
}

} // namespace DonTopo
