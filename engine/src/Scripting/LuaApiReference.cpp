#include "DonTopo/Scripting/LuaApiReference.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <utility>

namespace DonTopo {

namespace {

// Comparación sin distinguir mayúsculas: quien escribe 'transform:' espera las
// mismas sugerencias que quien escribe 'Transform:'.
bool startsWithCaseInsensitive(const std::string& value, const std::string& prefix)
{
    if (value.size() < prefix.size())
        return false;
    return std::equal(prefix.begin(), prefix.end(), value.begin(),
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b)); });
}

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
        // un collider Is Trigger (reciben la Entity que lo provocó);
        // OnCollision* lo mismo para colisiones DE VERDAD (ninguno de los dos
        // colliders es trigger), en los dos objetos del par.
        "Awake", "Start", "Update", "FixedUpdate", "LateUpdate", "OnDestroy",
        "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit",
        "OnCollisionEnter", "OnCollisionStay", "OnCollisionExit",

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
        // Mando crudo. Lo normal es usar acciones con nombre; esto es para
        // cuando el script quiere un botón concreto. Sin mando: siempre false.
        "Input.IsPadButtonDown", "Input.IsPadButtonPressed",
        "Input.IsPadAxisDown", "Input.IsPadAxisPressed",
        "PadButton.A", "PadButton.B", "PadButton.X", "PadButton.Y",
        "PadButton.LeftBumper", "PadButton.RightBumper",
        "PadButton.Back", "PadButton.Start", "PadButton.Guide",
        "PadButton.LeftThumb", "PadButton.RightThumb",
        "PadButton.DpadUp", "PadButton.DpadRight",
        "PadButton.DpadDown", "PadButton.DpadLeft",
        // Un eje son DOS bindings (una dirección cada uno): estas constantes ya
        // vienen compuestas, y PadAxis.Code(eje, negativo) compone cualquier otra.
        "PadAxis.Code",
        "PadAxis.LeftStickUp", "PadAxis.LeftStickDown",
        "PadAxis.LeftStickLeft", "PadAxis.LeftStickRight",
        "PadAxis.RightStickUp", "PadAxis.RightStickDown",
        "PadAxis.RightStickLeft", "PadAxis.RightStickRight",
        "PadAxis.LeftTrigger", "PadAxis.RightTrigger",

        // Entity
        "Entity.name", "Entity:IsValid", "Entity:GetTransform",
        // Oculta la malla sin destruir el objeto: sigue vivo y colisionando.
        "Entity.meshVisible",
        // Luz y cámara de juego, con los mismos atajos que los de UI.
        "Entity:GetLight", "Entity:AddLight", "Entity:RemoveLight",
        "Entity:GetCamera", "Entity:AddCamera", "Entity:RemoveCamera",
        "Entity:GetLayout", "Entity:AddLayout", "Entity:RemoveLayout",
        "Entity:GetParent", "Entity:SetParent",
        "Entity:GetChildren", "Entity:GetComponent",
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
        "Entity:GetInputField", "Entity:AddInputField", "Entity:RemoveInputField",
        "Entity:GetDropdown", "Entity:AddDropdown", "Entity:RemoveDropdown",
        "Entity:GetScrollView", "Entity:AddScrollView", "Entity:RemoveScrollView",

        // Transform
        "Transform:GetPosition", "Transform:SetPosition",
        "Transform:GetRotation", "Transform:SetRotation",
        "Transform:GetScale", "Transform:SetScale",
        "Transform:GetWorldPosition", "Transform:Translate", "Transform:Rotate",
        // Posición de mundo y ejes del objeto (ya normalizados). La convención
        // es la de la cámara: se mira hacia -Z local.
        "Transform:SetWorldPosition",
        "Transform:GetForward", "Transform:GetRight", "Transform:GetUp",
        "Transform:LookAt",

        // Light / Camera (GetComponent("Light") o Entity:GetLight)
        "Light.type", "Light.intensity", "Light.range",
        "Light.innerAngle", "Light.outerAngle",
        "Light.areaWidth", "Light.areaHeight",
        "Light:GetColor", "Light:SetColor",
        "LightType.Point", "LightType.Spot", "LightType.Directional", "LightType.Area",
        "Camera.mode", "Camera.fov", "Camera.orthographicSize",
        "Camera.near", "Camera.far",
        "CameraProjection.Perspective", "CameraProjection.Orthographic",

        // Colliders (la gravedad/dinámica vive ahora en Rigidbody)
        "BoxCollider:GetHalfExtents", "BoxCollider:SetHalfExtents",
        "BoxCollider:GetCenter", "BoxCollider:SetCenter",
        "SphereCollider:GetRadius", "SphereCollider:SetRadius",
        "SphereCollider:GetCenter", "SphereCollider:SetCenter",
        "CapsuleCollider:GetRadius", "CapsuleCollider:SetRadius",
        "CapsuleCollider:GetHalfHeight", "CapsuleCollider:SetHalfHeight",
        "CapsuleCollider:GetCenter", "CapsuleCollider:SetCenter",
        "PlaneCollider:GetCenter", "PlaneCollider:SetCenter",
        // Material de física por collider (propiedades, como en Rigidbody)
        "BoxCollider.staticFriction", "BoxCollider.dynamicFriction", "BoxCollider.bounciness",
        "SphereCollider.staticFriction", "SphereCollider.dynamicFriction", "SphereCollider.bounciness",
        "CapsuleCollider.staticFriction", "CapsuleCollider.dynamicFriction", "CapsuleCollider.bounciness",
        "PlaneCollider.staticFriction", "PlaneCollider.dynamicFriction", "PlaneCollider.bounciness",
        // Is Trigger por collider (solapa sin colisionar y dispara OnTrigger*).
        // El setter es no-op fuera de Play, donde no hay PhysicsManager.
        "BoxCollider.isTrigger", "SphereCollider.isTrigger",
        "CapsuleCollider.isTrigger", "PlaneCollider.isTrigger",
        // Capa de colisión por collider (0-31). Con quién colisiona cada capa lo
        // dice la matriz global: Physics.SetLayerCollision / GetLayerCollision.
        "BoxCollider.layer", "SphereCollider.layer",
        "CapsuleCollider.layer", "PlaneCollider.layer",

        // Rigidbody (dinámica estilo Unity; GetComponent("Rigidbody"))
        "Rigidbody.mass", "Rigidbody.useGravity", "Rigidbody.isKinematic",
        "Rigidbody.drag", "Rigidbody.angularDrag",
        // Bitmask de ejes congelados: se compone con OR de las constantes de
        // la tabla RigidbodyConstraints (de abajo).
        "Rigidbody.constraints",
        // Detección continua (contra el túnel de cuerpos rápidos) e
        // interpolación visual de la pose entre pasos fijos. Ambas false por
        // defecto e independientes entre sí.
        "Rigidbody.ccd", "Rigidbody.interpolate",
        "Rigidbody.velocity", "Rigidbody.angularVelocity",
        "Rigidbody:AddForce", "Rigidbody:AddTorque", "Rigidbody:AddImpulse",
        "RigidbodyConstraints.None",
        "RigidbodyConstraints.FreezePositionX", "RigidbodyConstraints.FreezePositionY",
        "RigidbodyConstraints.FreezePositionZ",
        "RigidbodyConstraints.FreezeRotationX", "RigidbodyConstraints.FreezeRotationY",
        "RigidbodyConstraints.FreezeRotationZ",
        // Modo opcional (4º argumento) de AddForce/AddTorque. Sin él, Force.
        "ForceMode.Force", "ForceMode.Acceleration",
        "ForceMode.Impulse", "ForceMode.VelocityChange",

        // Animator (máquina de estados; GetComponent("Animator"))
        "Animator:SetBool", "Animator:GetBool", "Animator:SetTrigger",
        "Animator:SetInt", "Animator:GetInt", "Animator:SetFloat", "Animator:GetFloat",
        "Animator:GetState", "Animator:IsBlending", "Animator:GetBlendWeight",
        "Animator:GetPreviousState", "Animator:GetPoseWeight",

        // AudioClip (GetComponent("AudioClip"))
        "AudioClip:Play", "AudioClip:PlayOneShot", "AudioClip:Stop",
        "AudioClip:Pause", "AudioClip:Resume",
        "AudioClip:IsPlaying", "AudioClip:IsPaused",
        "AudioClip:SetLoop", "AudioClip:GetLoop",
        "AudioClip:SetVolume", "AudioClip:GetVolume", "AudioClip:SetPitch", "AudioClip:GetPitch",
        "AudioClip:SetIs3D", "AudioClip:GetIs3D",
        "AudioClip:SetMinDistance", "AudioClip:GetMinDistance",
        "AudioClip:SetMaxDistance", "AudioClip:GetMaxDistance",
        "AudioClip:SetPlayOnAwake", "AudioClip:GetPlayOnAwake",
        "AudioClip:SetBus", "AudioClip:GetBus",
        "AudioClip:SetLoadMode", "AudioClip:GetLoadMode",
        "AudioClip:SetRolloff", "AudioClip:GetRolloff",
        "AudioClip:SetSpread", "AudioClip:GetSpread",
        "AudioClip:SetStereoPan", "AudioClip:GetStereoPan",
        "AudioClip:SetDopplerLevel", "AudioClip:GetDopplerLevel",
        "AudioClip:SetMute", "AudioClip:GetMute",
        "AudioClip:GetTime", "AudioClip:SetTime",
        "AudioClip:GetPath",

        // Audio global (volumenes por bus: "master", "music", "sfx")
        "Audio.SetBusVolume", "Audio.GetBusVolume",
        // Sonido posicional sin GameObject. Preload evita que el primer disparo
        // se pierda por la carga diferida de FMOD.
        "Audio.PlayClipAtPoint", "Audio.Preload",
        // Efectos por bus: lowPass, highPass, echo, reverb.
        "Audio.SetBusEffect", "Audio.ClearBusEffect",
        "Audio.SetPaused", "Audio.IsPaused",

        // ReverbZone (GetComponent("ReverbZone") / AddComponent("ReverbZone"))
        "ReverbZone:SetPreset", "ReverbZone:GetPreset",
        "ReverbZone:SetMinDistance", "ReverbZone:GetMinDistance",
        "ReverbZone:SetMaxDistance", "ReverbZone:GetMaxDistance",
        "ReverbZone:SetEnabled", "ReverbZone:GetEnabled",

        // UI 2D — Canvas (Entity:GetCanvas / Entity:AddCanvas)
        "Canvas.scaleMode", "Canvas.scaleFactor", "Canvas.screenMatch",
        "Canvas.matchWidthOrHeight", "Canvas.screenDpi", "Canvas.fallbackDpi",
        "Canvas.referenceDpi", "Canvas.aspectRatio",
        "Canvas.renderMode", "Canvas.worldScale", "Canvas.billboard", "Canvas.depthTest",
        "Canvas:GetReferenceResolution", "Canvas:SetReferenceResolution",
        // SafeArea son cuatro insets sueltos: left, top, right, bottom.
        "Canvas:GetSafeArea", "Canvas:SetSafeArea",

        // UI 2D — Button (Entity:GetButton / Entity:AddButton)
        "Button.visible", "Button.atlasPath", "Button.sprite",
        "Button.interactable", "Button.selected", "Button.transition",
        "Button.normalSprite", "Button.hoverSprite", "Button.pressedSprite",
        "Button.disabledSprite", "Button.selectedSprite", "Button.fadeDuration",
        "Button.text", "Button.fontPath", "Button.fontSize", "Button.textAlign",
        "Button.textVAlign",
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
        "Text.outlineWidth", "Text.align", "Text.vAlign", "Text.overflow", "Text.wordWrap",
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


        // UI 2D — InputField
        "InputField.visible", "InputField.interactable", "InputField.readOnly",
        "InputField.text", "InputField.placeholder", "InputField.fontPath",
        "InputField.fontSize", "InputField.align", "InputField.padding",
        "InputField.characterLimit", "InputField.contentType",
        "InputField.passwordChar", "InputField.caretWidth",
        "InputField.caretBlinkRate", "InputField.atlasPath",
        "InputField.backgroundSprite",
        "InputField:GetAnchorMin", "InputField:SetAnchorMin",
        "InputField:GetAnchorMax", "InputField:SetAnchorMax",
        "InputField:GetPivot", "InputField:SetPivot",
        "InputField:GetPosition", "InputField:SetPosition",
        "InputField:GetSize", "InputField:SetSize",
        "InputField:GetColor", "InputField:SetColor",
        "InputField:GetTextColor", "InputField:SetTextColor",
        "InputField:GetPlaceholderColor", "InputField:SetPlaceholderColor",
        "InputField:GetCaretColor", "InputField:SetCaretColor",
        "InputField:GetDisplayText", "InputField:GetCaretPos", "InputField:SetCaretPos",
        "InputField:OnValueChanged", "InputField:OnEndEdit",

        // UI 2D — Dropdown
        "Dropdown.visible", "Dropdown.interactable", "Dropdown.value",
        "Dropdown.isOpen", "Dropdown.itemHeight", "Dropdown.maxVisibleItems",
        "Dropdown.fontPath", "Dropdown.fontSize", "Dropdown.padding",
        "Dropdown.atlasPath", "Dropdown.backgroundSprite",
        "Dropdown.arrowSprite", "Dropdown.itemSprite",
        "Dropdown:GetAnchorMin", "Dropdown:SetAnchorMin",
        "Dropdown:GetAnchorMax", "Dropdown:SetAnchorMax",
        "Dropdown:GetPivot", "Dropdown:SetPivot",
        "Dropdown:GetPosition", "Dropdown:SetPosition",
        "Dropdown:GetSize", "Dropdown:SetSize",
        "Dropdown:GetColor", "Dropdown:SetColor",
        "Dropdown:GetListColor", "Dropdown:SetListColor",
        "Dropdown:GetItemColor", "Dropdown:SetItemColor",
        "Dropdown:GetItemSelectedColor", "Dropdown:SetItemSelectedColor",
        "Dropdown:GetArrowColor", "Dropdown:SetArrowColor",
        "Dropdown:GetTextColor", "Dropdown:SetTextColor",
        "Dropdown:GetOptionCount", "Dropdown:GetOption", "Dropdown:GetSelectedLabel",
        "Dropdown:SetOptions", "Dropdown:AddOption", "Dropdown:ClearOptions",
        "Dropdown:OnValueChanged",

        // UI 2D — ScrollView
        "ScrollView.visible", "ScrollView.horizontal", "ScrollView.vertical",
        "ScrollView.scrollSensitivity", "ScrollView.atlasPath",
        "ScrollView.backgroundSprite",
        "ScrollView:GetAnchorMin", "ScrollView:SetAnchorMin",
        "ScrollView:GetAnchorMax", "ScrollView:SetAnchorMax",
        "ScrollView:GetPivot", "ScrollView:SetPivot",
        "ScrollView:GetPosition", "ScrollView:SetPosition",
        "ScrollView:GetSize", "ScrollView:SetSize",
        "ScrollView:GetColor", "ScrollView:SetColor",
        "ScrollView:GetContentSize", "ScrollView:SetContentSize",
        "ScrollView:GetNormalizedPosition", "ScrollView:SetNormalizedPosition",
        "ScrollView:GetScrollRange", "ScrollView:GetContentOffset",
        "ScrollView:OnValueChanged",

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
        "UiCanvasRenderMode.ScreenSpace", "UiCanvasRenderMode.World",
        "UiBillboard.None", "UiBillboard.YawOnly", "UiBillboard.Full",
        "UiTextAlign.Left", "UiTextAlign.Center", "UiTextAlign.Right",
        "UiTextAlign.Justify",
        // Vertical: la registra registerUi como cualquier otra y llevaba desde
        // entonces fuera de esta lista (el README lo documentaba como ausencia).
        "UiTextVAlign.Top", "UiTextVAlign.Middle", "UiTextVAlign.Bottom",
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
        "UiInputContentType.Standard", "UiInputContentType.IntegerNumber",
        "UiInputContentType.DecimalNumber", "UiInputContentType.Alphanumeric",
        "UiInputContentType.Password",
        "UiButtonTransition.ColorTint", "UiButtonTransition.SpriteSwap",
        "UiButtonTransition.Animation",
        "UiButtonState.Normal", "UiButtonState.Hover", "UiButtonState.Pressed",
        "UiButtonState.Disabled", "UiButtonState.Selected",

        // Scene
        "Scene.Find", "Scene.CreateGameObject", "Scene.Destroy", "Scene.Instantiate",

        // Physics — consultas de rayo (nil / false si no hay escena de física,
        // es decir fuera de Play). Los nombres sueltos son los campos de la
        // tabla 'options' y los de la tabla que devuelve Raycast.
        // RaycastAll devuelve un array de esas mismas tablas (vacío si no hay
        // impactos, nunca nil), ordenado por distancia.
        "Physics.Raycast", "Physics.RaycastAll", "Physics.RaycastHit",
        "hitTriggers", "static", "dynamic", "ignore",
        "entity", "point", "normal", "distance",

        // Physics — barrido y solapes, mismos filtros ('options') que el rayo.
        // SphereCast devuelve la misma tabla de impacto que Raycast; los dos
        // Overlap devuelven un array de Entity (vacío si nada solapa, nunca
        // nil): un solape no tiene punto, normal ni distancia.
        "Physics.SphereCast", "Physics.OverlapSphere", "Physics.OverlapBox",

        // Physics — matriz de capas de colisión (32x32, simétrica). Índice
        // fuera de [0,31]: error de Lua.
        "Physics.SetLayerCollision", "Physics.GetLayerCollision",

        // Motor (cambio de escena en runtime)
        "DonTopo.loadScene",

        // Vec3
        "Vec3.new",
        // Campos y álgebra. Los métodos van con ':' porque operan sobre una
        // instancia: 'v:Length()', 'a:Dot(b)'.
        "Vec3.x", "Vec3.y", "Vec3.z",
        "Vec3:Length", "Vec3:Normalized", "Vec3:Dot", "Vec3:Cross",
        "Vec3:Distance", "Vec3:Lerp",

        // Time — reloj de los scripts. Se reinicia en cada Play.
        "Time.deltaTime", "Time.fixedDeltaTime", "Time.time", "Time.frameCount",
    };
    return symbols;
}

// ---------------------------------------------------------------------------
// Firmas y documentación
//
// Tabla SEPARADA de la lista de símbolos a propósito. La lista de arriba sigue
// siendo la autoridad sobre qué existe —es la que hay que tocar al añadir un
// binding, y la regla del proyecto no cambia—; esto es solo texto de ayuda.
// Un símbolo sin entrada aquí sale en el popup igual, sin firma: nunca
// desaparece por no estar documentado.
//
// Las familias mecánicas de la UI (los diez accessors de rect que repiten los
// catorce widgets) se generan en bucle en vez de escribirse ciento cuarenta
// veces: el texto sería idéntico y copiarlo solo garantiza que un día uno de
// los catorce se quede sin actualizar.
// ---------------------------------------------------------------------------

struct DocEntry { const char* signature; const char* doc; };

void addRectAccessors(std::unordered_map<std::string, DocEntry>& out, const std::string& type)
{
    // Los pares de un rect llegan y salen como DOS números sueltos, no como un
    // Vec3: 'local x, y = b:GetSize()'.
    out[type + ":GetAnchorMin"] = { "() -> x, y", "Ancla inferior-izquierda, en fracción del padre (0..1)." };
    out[type + ":SetAnchorMin"] = { "(x, y)", "Fija el ancla inferior-izquierda, en fracción del padre (0..1)." };
    out[type + ":GetAnchorMax"] = { "() -> x, y", "Ancla superior-derecha, en fracción del padre (0..1)." };
    out[type + ":SetAnchorMax"] = { "(x, y)", "Fija el ancla superior-derecha, en fracción del padre (0..1)." };
    out[type + ":GetPivot"]     = { "() -> x, y", "Punto del propio rect que se coloca en la posición (0..1)." };
    out[type + ":SetPivot"]     = { "(x, y)", "Fija el punto del rect que se coloca en la posición (0..1)." };
    out[type + ":GetPosition"]  = { "() -> x, y", "Desplazamiento en píxeles respecto al ancla." };
    out[type + ":SetPosition"]  = { "(x, y)", "Fija el desplazamiento en píxeles respecto al ancla." };
    out[type + ":GetSize"]      = { "() -> ancho, alto", "Tamaño en píxeles." };
    out[type + ":SetSize"]      = { "(ancho, alto)", "Fija el tamaño en píxeles." };
}

void addColorAccessor(std::unordered_map<std::string, DocEntry>& out,
                      const std::string& type, const std::string& name, const char* que)
{
    out[type + ":Get" + name] = { "() -> r, g, b, a", que };
    out[type + ":Set" + name] = { "(r, g, b, a)", que };
}

const std::unordered_map<std::string, DocEntry>& docTable()
{
    static const std::unordered_map<std::string, DocEntry> table = [] {
        std::unordered_map<std::string, DocEntry> t = {
            // --- Vec3 ---
            {"Vec3.new", {"(x, y, z) -> Vec3", "Vector nuevo; sin argumentos, el vector cero. Vec3(x,y,z) hace lo mismo."}},
            {"Vec3.x", {"", "Componente X."}},
            {"Vec3.y", {"", "Componente Y."}},
            {"Vec3.z", {"", "Componente Z."}},
            {"Vec3:Length", {"() -> number", "Longitud del vector."}},
            {"Vec3:Normalized", {"() -> Vec3", "Copia de longitud 1. El vector cero se devuelve tal cual, sin NaN."}},
            {"Vec3:Dot", {"(otro: Vec3) -> number", "Producto escalar."}},
            {"Vec3:Cross", {"(otro: Vec3) -> Vec3", "Producto vectorial."}},
            {"Vec3:Distance", {"(otro: Vec3) -> number", "Distancia entre los dos puntos."}},
            {"Vec3:Lerp", {"(destino: Vec3, t) -> Vec3", "Interpolación lineal; t fuera de [0,1] extrapola."}},

            // --- Time ---
            {"Time.deltaTime", {"", "Segundos del último frame. Igual que el argumento de Update."}},
            {"Time.fixedDeltaTime", {"", "Paso fijo de FixedUpdate, en segundos (constante)."}},
            {"Time.time", {"", "Segundos desde que empezó el Play actual."}},
            {"Time.frameCount", {"", "Frames transcurridos desde que empezó el Play actual."}},

            // --- Log ---
            {"Log.Info", {"(mensaje)", "Escribe en el Log Console."}},
            {"Log.Warn", {"(mensaje)", "Escribe en el Log Console con prefijo [WARN]."}},
            {"Log.Error", {"(mensaje)", "Escribe en el Log Console con prefijo [ERROR]."}},
            {"print", {"(...)", "print de Lua, redirigido al Log Console."}},
            {"DestroyGameObject", {"(entity: Entity)", "Destruye el objeto y su subárbol al final del frame."}},

            // --- Input ---
            {"Input.IsKeyDown", {"(tecla) -> boolean", "Tecla mantenida. Usa la tabla Key."}},
            {"Input.IsKeyPressed", {"(tecla) -> boolean", "Solo el frame en que se pulsa."}},
            {"Input.IsKeyReleased", {"(tecla) -> boolean", "Solo el frame en que se suelta."}},
            {"Input.IsMouseButtonDown", {"(boton) -> boolean", "Botón de ratón mantenido. Usa la tabla MouseButton."}},
            {"Input.IsActionDown", {"(nombre) -> boolean", "Acción del panel Input Actions, mantenida."}},
            {"Input.IsActionPressed", {"(nombre) -> boolean", "Acción del panel Input Actions, solo el frame de la pulsación."}},
            {"Input.IsActionReleased", {"(nombre) -> boolean", "Acción del panel Input Actions, solo el frame en que se suelta."}},
            {"Input.IsPadButtonDown", {"(boton) -> boolean", "Botón de mando mantenido. Sin mando, false."}},
            {"Input.IsPadButtonPressed", {"(boton) -> boolean", "Botón de mando, solo el frame de la pulsación."}},
            {"Input.IsPadAxisDown", {"(codigo) -> boolean", "Dirección de stick o gatillo mantenida. Usa la tabla PadAxis."}},
            {"Input.IsPadAxisPressed", {"(codigo) -> boolean", "Dirección de stick o gatillo, solo el frame del flanco."}},
            {"PadAxis.Code", {"(eje, negativo) -> number", "Compone el código de una dirección de eje."}},

            // --- Entity ---
            {"Entity.name", {"", "Nombre del GameObject; se puede leer y escribir."}},
            {"Entity.meshVisible", {"", "Dibuja o esconde la malla. El objeto sigue vivo y colisionando."}},
            {"Entity:IsValid", {"() -> boolean", "false si el objeto ya fue destruido. No lanza error."}},
            {"Entity:GetTransform", {"() -> Transform", "Transform del objeto."}},
            {"Entity:GetParent", {"() -> Entity", "Padre, o nil si cuelga de la raíz."}},
            {"Entity:SetParent", {"(padre: Entity?, mantenerPoseDeMundo?) -> boolean", "Cambia de padre; nil = la raíz. Por defecto el objeto se queda donde está."}},
            {"Entity:GetChildren", {"() -> tabla de Entity", "Hijos directos, en orden de escena."}},
            {"Entity:GetComponent", {"(nombre) -> componente", "Componente por nombre, o nil. \"Script:Nombre\" da la instancia de un script."}},
            {"Entity:AddComponent", {"(nombre, arg?) -> componente", "Añade el componente con los defaults del editor; si ya está, devuelve el que hay."}},
            {"Entity:RemoveComponent", {"(nombre)", "Quita el componente. Los scripts se quitan al final del frame."}},
            {"Entity:GetLight", {"() -> Light", "Componente de luz, o nil si no lo tiene."}},
            {"Entity:AddLight", {"() -> Light", "Añade una luz (point, blanca) si no había."}},
            {"Entity:RemoveLight", {"()", "Quita la luz."}},
            {"Entity:GetCamera", {"() -> Camera", "Cámara de juego, o nil si no la tiene."}},
            {"Entity:AddCamera", {"() -> Camera", "Añade una cámara de juego si no había."}},
            {"Entity:RemoveCamera", {"()", "Quita la cámara de juego."}},

            // --- Transform ---
            {"Transform:GetPosition", {"() -> Vec3", "Posición LOCAL, relativa al padre."}},
            {"Transform:SetPosition", {"(pos: Vec3)", "Fija la posición local."}},
            {"Transform:GetRotation", {"() -> Vec3", "Rotación local en grados (euler XYZ)."}},
            {"Transform:SetRotation", {"(euler: Vec3)", "Fija la rotación local en grados."}},
            {"Transform:GetScale", {"() -> Vec3", "Escala local."}},
            {"Transform:SetScale", {"(escala: Vec3)", "Fija la escala local."}},
            {"Transform:GetWorldPosition", {"() -> Vec3", "Posición en mundo, ya con la del padre aplicada."}},
            {"Transform:SetWorldPosition", {"(pos: Vec3)", "Coloca el objeto en esa posición de mundo."}},
            {"Transform:Translate", {"(delta: Vec3)", "Suma el desplazamiento a la posición local."}},
            {"Transform:Rotate", {"(euler: Vec3)", "Rotación incremental en grados, compuesta como quaternion."}},
            {"Transform:GetForward", {"() -> Vec3", "Eje -Z del objeto en mundo, normalizado."}},
            {"Transform:GetRight", {"() -> Vec3", "Eje +X del objeto en mundo, normalizado."}},
            {"Transform:GetUp", {"() -> Vec3", "Eje +Y del objeto en mundo, normalizado."}},
            {"Transform:LookAt", {"(objetivo: Vec3, up: Vec3?)", "Gira el objeto para mirar al punto. up por defecto (0,1,0)."}},

            // --- Light / Camera ---
            {"Light.type", {"", "Point, Spot, Directional o Area. Usa la tabla LightType."}},
            {"Light.intensity", {"", "Multiplicador del color, 0..100."}},
            {"Light.range", {"", "Alcance de point y spot. Directional lo ignora."}},
            {"Light.innerAngle", {"", "Semiángulo interior del cono del spot, en grados."}},
            {"Light.outerAngle", {"", "Semiángulo exterior del cono del spot, en grados."}},
            {"Light.areaWidth", {"", "Ancho del rectángulo de la luz de área."}},
            {"Light.areaHeight", {"", "Alto del rectángulo de la luz de área."}},
            {"Light:GetColor", {"() -> Vec3", "Color rgb, sin la intensidad aplicada."}},
            {"Light:SetColor", {"(color: Vec3)", "Fija el color rgb, acotado a 0..1."}},
            {"Camera.mode", {"", "Perspective u Orthographic. Usa la tabla CameraProjection."}},
            {"Camera.fov", {"", "Campo de visión en grados; solo en perspectiva."}},
            {"Camera.orthographicSize", {"", "Semi-altura visible en unidades de mundo; solo en ortográfica."}},
            {"Camera.near", {"", "Plano de recorte cercano."}},
            {"Camera.far", {"", "Plano de recorte lejano."}},

            // --- Scene / motor ---
            {"Scene.Find", {"(nombre) -> Entity", "Primer objeto con ese nombre, o nil."}},
            {"Scene.CreateGameObject", {"(nombre, padre: Entity?) -> Entity", "Crea un objeto vacío."}},
            {"Scene.Destroy", {"(entity: Entity)", "Destruye el objeto y su subárbol al final del frame."}},
            {"Scene.Instantiate", {"(origen: Entity, padre: Entity?) -> Entity", "Clona el objeto con sus componentes e hijos."}},
            {"DonTopo.loadScene", {"(ruta) -> boolean", "Encola el cambio de escena; el bool es solo la validación del fichero."}},

            // --- Physics ---
            {"Physics.Raycast", {"(origen: Vec3, dir: Vec3, dist, options?) -> tabla", "Primer impacto, o nil. Fuera de Play siempre nil."}},
            {"Physics.RaycastAll", {"(origen: Vec3, dir: Vec3, dist, options?) -> tabla", "Todos los impactos ordenados por distancia; array vacío si ninguno."}},
            {"Physics.SphereCast", {"(origen: Vec3, dir: Vec3, radio, dist, options?) -> tabla", "Barrido de una esfera; misma tabla de impacto que Raycast."}},
            {"Physics.OverlapSphere", {"(centro: Vec3, radio, options?) -> tabla", "Entities que solapan la esfera; un solape no tiene punto ni normal."}},
            {"Physics.OverlapBox", {"(centro: Vec3, semiejes: Vec3, options?) -> tabla", "Entities que solapan la caja."}},
            {"Physics.SetLayerCollision", {"(a, b, activo)", "Activa o corta la colisión entre dos capas (0..31)."}},
            {"Physics.GetLayerCollision", {"(a, b) -> boolean", "Si las dos capas colisionan entre sí."}},

            // --- Rigidbody ---
            {"Rigidbody.mass", {"", "Masa en kg. Acceleration y VelocityChange la ignoran."}},
            {"Rigidbody.useGravity", {"", "Si la gravedad de la escena le afecta."}},
            {"Rigidbody.isKinematic", {"", "Cuerpo movido a mano: el solver no lo empuja."}},
            {"Rigidbody.drag", {"", "Rozamiento lineal."}},
            {"Rigidbody.angularDrag", {"", "Rozamiento angular."}},
            {"Rigidbody.constraints", {"", "Bitmask de ejes congelados; se compone con OR de RigidbodyConstraints."}},
            {"Rigidbody.ccd", {"", "Detección continua, contra el túnel de cuerpos rápidos. No vale en kinematic."}},
            {"Rigidbody.interpolate", {"", "Suaviza la pose VISUAL entre pasos fijos."}},
            {"Rigidbody.velocity", {"", "Velocidad lineal, en unidades por segundo."}},
            {"Rigidbody.angularVelocity", {"", "Velocidad angular, en radianes por segundo."}},
            {"Rigidbody:AddForce", {"(fuerza: Vec3, modo?)", "Aplica una fuerza. El modo sale de la tabla ForceMode; por defecto Force."}},
            {"Rigidbody:AddTorque", {"(par: Vec3, modo?)", "Aplica un par de giro."}},
            {"Rigidbody:AddImpulse", {"(impulso: Vec3)", "Azúcar de AddForce con ForceMode.Impulse."}},

            // --- Animator ---
            {"Animator:SetBool", {"(nombre, valor)", "Fija un parámetro booleano de la máquina de estados."}},
            {"Animator:GetBool", {"(nombre) -> boolean", "Lee un parámetro booleano."}},
            {"Animator:SetTrigger", {"(nombre)", "Dispara un trigger; se consume en la primera transición que lo use."}},
            {"Animator:SetInt", {"(nombre, valor)", "Fija un parámetro entero."}},
            {"Animator:GetInt", {"(nombre) -> number", "Lee un parámetro entero."}},
            {"Animator:SetFloat", {"(nombre, valor)", "Fija un parámetro float."}},
            {"Animator:GetFloat", {"(nombre) -> number", "Lee un parámetro float."}},
            {"Animator:GetState", {"() -> string", "Nombre del estado actual."}},
            {"Animator:GetPreviousState", {"() -> string", "Estado del que se viene durante un cross-fade."}},
            {"Animator:IsBlending", {"() -> boolean", "Si hay un cross-fade en curso."}},
            {"Animator:GetBlendWeight", {"() -> number", "Peso 0..1 del cross-fade en curso."}},
            {"Animator:GetPoseWeight", {"() -> number", "Peso del blend de dos clips por parámetro."}},

            // --- Audio ---
            {"Audio.SetBusVolume", {"(bus, volumen)", "Volumen 0..1 de \"master\", \"music\" o \"sfx\"."}},
            {"Audio.GetBusVolume", {"(bus) -> number", "Volumen actual del bus."}},
            {"Audio.PlayClipAtPoint", {"(ruta, pos: Vec3, volumen?)", "Sonido posicional de usar y tirar, sin GameObject."}},
            {"Audio.Preload", {"(ruta)", "Carga el clip ya, para que el primer disparo no se pierda."}},
            {"Audio.SetBusEffect", {"(bus, efecto, parametros...)", "Efecto por bus: lowPass, highPass, echo o reverb."}},
            {"Audio.ClearBusEffect", {"(bus)", "Quita el efecto del bus."}},
            {"Audio.SetPaused", {"(pausado)", "Pausa o reanuda todo el audio."}},
            {"Audio.IsPaused", {"() -> boolean", "Si el audio está pausado globalmente."}},
        };

        // Los catorce widgets comparten rect; el color de fondo también.
        for (const char* type : {"Button", "Text", "ProgressBar", "Layout", "Panel",
                                 "Image", "Slider", "Checkbox", "Toggle", "Scrollbar",
                                 "InputField", "Dropdown", "ScrollView"})
        {
            addRectAccessors(t, type);
            // Toggle es el único sin color de fondo: tiene OffColor y OnColor.
            if (std::string(type) != "Toggle")
                addColorAccessor(t, type, "Color", "Color de fondo (rgba 0..1).");
        }
        return t;
    }();
    return table;
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

void luaApiDoc(const std::string& symbol, std::string& outSignature, std::string& outDoc)
{
    outSignature.clear();
    outDoc.clear();
    auto it = docTable().find(symbol);
    if (it == docTable().end()) return;
    outSignature = it->second.signature;
    outDoc       = it->second.doc;
}

std::vector<LuaApiMatch> luaApiMatches(const std::string& fragment, std::size_t maxResults)
{
    std::vector<LuaApiMatch> out;
    if (fragment.empty()) return out;

    // El fragmento se parte por el ÚLTIMO separador: "Entity:GetT" -> receptor
    // "Entity", separador ':', miembro "GetT". Sin separador, todo es miembro.
    const std::size_t sep = fragment.find_last_of(".:");
    const bool hasSeparator = (sep != std::string::npos);
    const std::string receiver = hasSeparator ? fragment.substr(0, sep) : std::string();
    const char separator = hasSeparator ? fragment[sep] : '\0';
    const std::string member = hasSeparator ? fragment.substr(sep + 1) : fragment;
    // Dónde empieza a sustituirse cuando el receptor NO es un tipo conocido:
    // justo después del separador, para conservar lo que el usuario escribió.
    const std::size_t memberOffset = hasSeparator ? sep + 1 : 0;

    // Rango 0: el símbolo entero empieza por lo escrito. Es lo que había antes
    // y lo que un usuario espera al teclear el nombre del tipo.
    // Rango 1: el receptor es una variable local ("t:Get"), así que se busca
    // por el nombre del MIEMBRO en cualquier tipo. Sin esto, el caso normal
    // —llamar a través de una variable— no ofrecía nada.
    struct Scored { int rank; const std::string* symbol; std::size_t offset; };
    std::vector<Scored> scored;

    for (const std::string& symbol : luaApiSymbols())
    {
        if (startsWithCaseInsensitive(symbol, fragment))
        {
            scored.push_back({0, &symbol, 0});
            continue;
        }

        // Para el rango 1 hace falta partir el símbolo por SU separador y
        // comparar solo la parte del miembro. Un símbolo sin separador (una
        // keyword de Lua, un global) no tiene miembro que ofrecer aquí.
        const std::size_t symSep = symbol.find_last_of(".:");
        if (symSep == std::string::npos) continue;
        // El separador tiene que coincidir: '.' es propiedad y ':' es método,
        // y ofrecer un método donde se escribió un punto sería una sugerencia
        // que no compila.
        if (hasSeparator && symbol[symSep] != separator) continue;
        // Sin nada escrito después del separador ("t:") no hay miembro que
        // filtrar: se ofrecen todos los del separador pedido.
        if (!member.empty() &&
            !startsWithCaseInsensitive(symbol.substr(symSep + 1), member)) continue;
        // Sin separador en lo escrito, esto es teclear "Get" a pelo: vale como
        // búsqueda por miembro, pero se sustituye el fragmento entero.
        scored.push_back({1, &symbol, hasSeparator ? memberOffset : 0});
    }

    // Orden total y determinista: rango, luego longitud (lo más corto suele ser
    // lo más general), luego alfabético. Sin el desempate por nombre el orden
    // dependería del de la tabla y un test no podría fijarlo.
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.symbol->size() != b.symbol->size()) return a.symbol->size() < b.symbol->size();
        return *a.symbol < *b.symbol;
    });

    for (const Scored& s : scored)
    {
        if (maxResults != 0 && out.size() >= maxResults) break;
        LuaApiMatch m;
        m.symbol = *s.symbol;
        luaApiDoc(m.symbol, m.signature, m.doc);
        m.replaceOffset = s.offset;
        // Con offset se conserva el receptor escrito por el usuario y solo se
        // escribe el miembro; sin él, se sustituye el fragmento entero.
        m.insert = (s.offset == 0) ? m.symbol
                                   : m.symbol.substr(m.symbol.find_last_of(".:") + 1);
        out.push_back(std::move(m));
    }
    return out;
}

} // namespace DonTopo
