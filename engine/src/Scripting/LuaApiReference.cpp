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
        // colliders es trigger), en los dos objetos del par; OnAnimationEvent
        // recibe el nombre del evento cuando el Animator del objeto lo cruza
        // (solo en Play).
        "Awake", "Start", "Update", "FixedUpdate", "LateUpdate", "OnDestroy",
        "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit",
        "OnCollisionEnter", "OnCollisionStay", "OnCollisionExit",
        "OnAnimationEvent",

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
        "Animator:Play", "Animator:CrossFade", "Animator:ResetTrigger", "Animator:GetNormalizedTime",
        "Animator:SetSpeed", "Animator:GetSpeed",
        "Animator:SetLayerWeight", "Animator:GetLayerWeight", "Animator:GetLayerCount",
        "Animator:SetIkWeight", "Animator:GetIkWeight", "Animator:SetIkTarget",
        "Animator:SetIkPole", "Animator:GetIkCount",

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
    out[type + ":GetAnchorMin"] = { "() -> x, y", "Bottom-left anchor, as a fraction of the parent (0..1)." };
    out[type + ":SetAnchorMin"] = { "(x, y)", "Sets the bottom-left anchor, as a fraction of the parent (0..1)." };
    out[type + ":GetAnchorMax"] = { "() -> x, y", "Top-right anchor, as a fraction of the parent (0..1)." };
    out[type + ":SetAnchorMax"] = { "(x, y)", "Sets the top-right anchor, as a fraction of the parent (0..1)." };
    out[type + ":GetPivot"]     = { "() -> x, y", "Point of the rect itself placed at the position (0..1)." };
    out[type + ":SetPivot"]     = { "(x, y)", "Sets the point of the rect placed at the position (0..1)." };
    out[type + ":GetPosition"]  = { "() -> x, y", "Offset in pixels from the anchor." };
    out[type + ":SetPosition"]  = { "(x, y)", "Sets the offset in pixels from the anchor." };
    out[type + ":GetSize"]      = { "() -> width, height", "Size in pixels." };
    out[type + ":SetSize"]      = { "(width, height)", "Sets the size in pixels." };
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
            {"Vec3.new", {"(x, y, z) -> Vec3", "New vector; without arguments, the zero vector. Vec3(x,y,z) does the same."}},
            {"Vec3.x", {"", "X component."}},
            {"Vec3.y", {"", "Y component."}},
            {"Vec3.z", {"", "Z component."}},
            {"Vec3:Length", {"() -> number", "Length of the vector."}},
            {"Vec3:Normalized", {"() -> Vec3", "Copy with length 1. The zero vector is returned as is, without NaN."}},
            {"Vec3:Dot", {"(other: Vec3) -> number", "Dot product."}},
            {"Vec3:Cross", {"(other: Vec3) -> Vec3", "Cross product."}},
            {"Vec3:Distance", {"(other: Vec3) -> number", "Distance between the two points."}},
            {"Vec3:Lerp", {"(target: Vec3, t) -> Vec3", "Linear interpolation; t outside [0,1] extrapolates."}},

            // --- Time ---
            {"Time.deltaTime", {"", "Seconds of the last frame. Same as Update's argument."}},
            {"Time.fixedDeltaTime", {"", "FixedUpdate's fixed step, in seconds (constant)."}},
            {"Time.time", {"", "Seconds since the current Play started."}},
            {"Time.frameCount", {"", "Frames elapsed since the current Play started."}},

            // --- Log ---
            {"Log.Info", {"(message)", "Writes to the Log Console."}},
            {"Log.Warn", {"(message)", "Writes to the Log Console with a [WARN] prefix."}},
            {"Log.Error", {"(message)", "Writes to the Log Console with an [ERROR] prefix."}},
            {"print", {"(...)", "Lua's print, redirected to the Log Console."}},
            {"DestroyGameObject", {"(entity: Entity)", "Destroys the object and its subtree at the end of the frame."}},

            // --- Input ---
            {"Input.IsKeyDown", {"(key) -> boolean", "Key held. Use the Key table."}},
            {"Input.IsKeyPressed", {"(key) -> boolean", "Only on the frame it is pressed."}},
            {"Input.IsKeyReleased", {"(key) -> boolean", "Only on the frame it is released."}},
            {"Input.IsMouseButtonDown", {"(button) -> boolean", "Mouse button held. Use the MouseButton table."}},
            {"Input.IsActionDown", {"(name) -> boolean", "Action from the Input Actions panel, held."}},
            {"Input.IsActionPressed", {"(name) -> boolean", "Action from the Input Actions panel, only on the frame it is pressed."}},
            {"Input.IsActionReleased", {"(name) -> boolean", "Action from the Input Actions panel, only on the frame it is released."}},
            {"Input.IsPadButtonDown", {"(button) -> boolean", "Gamepad button held. Without a gamepad, false."}},
            {"Input.IsPadButtonPressed", {"(button) -> boolean", "Gamepad button, only on the frame it is pressed."}},
            {"Input.IsPadAxisDown", {"(code) -> boolean", "Stick or trigger direction held. Use the PadAxis table."}},
            {"Input.IsPadAxisPressed", {"(code) -> boolean", "Stick or trigger direction, only on the frame of the edge."}},
            {"PadAxis.Code", {"(axis, negative) -> number", "Builds the code of an axis direction."}},

            // --- Entity ---
            {"Entity.name", {"", "Name of the GameObject; readable and writable."}},
            {"Entity.meshVisible", {"", "Shows or hides the mesh. The object stays alive and keeps colliding."}},
            {"Entity:IsValid", {"() -> boolean", "false if the object was already destroyed. Does not throw."}},
            {"Entity:GetTransform", {"() -> Transform", "The object's Transform."}},
            {"Entity:GetParent", {"() -> Entity", "Parent, or nil if it hangs from the root."}},
            {"Entity:SetParent", {"(parent: Entity?, keepWorldPose?) -> boolean", "Changes the parent; nil = the root. By default the object stays where it is."}},
            {"Entity:GetChildren", {"() -> table of Entity", "Direct children, in scene order."}},
            {"Entity:GetComponent", {"(name) -> component", "Component by name, or nil. \"Script:Name\" gives a script's instance."}},
            {"Entity:AddComponent", {"(name, arg?) -> component", "Adds the component with the editor's defaults; if it is already there, returns the existing one."}},
            {"Entity:RemoveComponent", {"(name)", "Removes the component. Scripts are removed at the end of the frame."}},
            {"Entity:GetLight", {"() -> Light", "Light component, or nil if it has none."}},
            {"Entity:AddLight", {"() -> Light", "Adds a light (point, white) if there was none."}},
            {"Entity:RemoveLight", {"()", "Removes the light."}},
            {"Entity:GetCamera", {"() -> Camera", "Game camera, or nil if it has none."}},
            {"Entity:AddCamera", {"() -> Camera", "Adds a game camera if there was none."}},
            {"Entity:RemoveCamera", {"()", "Removes the game camera."}},

            // --- Transform ---
            {"Transform:GetPosition", {"() -> Vec3", "LOCAL position, relative to the parent."}},
            {"Transform:SetPosition", {"(pos: Vec3)", "Sets the local position."}},
            {"Transform:GetRotation", {"() -> Vec3", "Local rotation in degrees (XYZ euler)."}},
            {"Transform:SetRotation", {"(euler: Vec3)", "Sets the local rotation in degrees."}},
            {"Transform:GetScale", {"() -> Vec3", "Local scale."}},
            {"Transform:SetScale", {"(scale: Vec3)", "Sets the local scale."}},
            {"Transform:GetWorldPosition", {"() -> Vec3", "World position, with the parent's already applied."}},
            {"Transform:SetWorldPosition", {"(pos: Vec3)", "Places the object at that world position."}},
            {"Transform:Translate", {"(delta: Vec3)", "Adds the offset to the local position."}},
            {"Transform:Rotate", {"(euler: Vec3)", "Incremental rotation in degrees, composed as a quaternion."}},
            {"Transform:GetForward", {"() -> Vec3", "The object's -Z axis in world space, normalized."}},
            {"Transform:GetRight", {"() -> Vec3", "The object's +X axis in world space, normalized."}},
            {"Transform:GetUp", {"() -> Vec3", "The object's +Y axis in world space, normalized."}},
            {"Transform:LookAt", {"(target: Vec3, up: Vec3?)", "Rotates the object to look at the point. up defaults to (0,1,0)."}},

            // --- Light / Camera ---
            {"Light.type", {"", "Point, Spot, Directional or Area. Use the LightType table."}},
            {"Light.intensity", {"", "Color multiplier, 0..100."}},
            {"Light.range", {"", "Range of point and spot lights. Directional ignores it."}},
            {"Light.innerAngle", {"", "Inner half-angle of the spot cone, in degrees."}},
            {"Light.outerAngle", {"", "Outer half-angle of the spot cone, in degrees."}},
            {"Light.areaWidth", {"", "Width of the area light's rectangle."}},
            {"Light.areaHeight", {"", "Height of the area light's rectangle."}},
            {"Light:GetColor", {"() -> Vec3", "RGB color, without the intensity applied."}},
            {"Light:SetColor", {"(color: Vec3)", "Sets the RGB color, clamped to 0..1."}},
            {"Camera.mode", {"", "Perspective or Orthographic. Use the CameraProjection table."}},
            {"Camera.fov", {"", "Field of view in degrees; perspective only."}},
            {"Camera.orthographicSize", {"", "Visible half-height in world units; orthographic only."}},
            {"Camera.near", {"", "Near clipping plane."}},
            {"Camera.far", {"", "Far clipping plane."}},

            // --- Scene / motor ---
            {"Scene.Find", {"(name) -> Entity", "First object with that name, or nil."}},
            {"Scene.CreateGameObject", {"(name, parent: Entity?) -> Entity", "Creates an empty object."}},
            {"Scene.Destroy", {"(entity: Entity)", "Destroys the object and its subtree at the end of the frame."}},
            {"Scene.Instantiate", {"(source: Entity, parent: Entity?) -> Entity", "Clones the object with its components and children."}},
            {"DonTopo.loadScene", {"(path) -> boolean", "Queues the scene change; the bool is only the file validation."}},

            // --- Physics ---
            {"Physics.Raycast", {"(origin: Vec3, dir: Vec3, dist, options?) -> table", "First hit, or nil. Always nil outside Play."}},
            {"Physics.RaycastAll", {"(origin: Vec3, dir: Vec3, dist, options?) -> table", "All hits sorted by distance; empty array if none."}},
            {"Physics.SphereCast", {"(origin: Vec3, dir: Vec3, radius, dist, options?) -> table", "Sweeps a sphere; same hit table as Raycast."}},
            {"Physics.OverlapSphere", {"(center: Vec3, radius, options?) -> table", "Entities overlapping the sphere; an overlap has no point or normal."}},
            {"Physics.OverlapBox", {"(center: Vec3, halfExtents: Vec3, options?) -> table", "Entities overlapping the box."}},
            {"Physics.SetLayerCollision", {"(a, b, enabled)", "Enables or disables collision between two layers (0..31)."}},
            {"Physics.GetLayerCollision", {"(a, b) -> boolean", "Whether the two layers collide with each other."}},

            // --- Rigidbody ---
            {"Rigidbody.mass", {"", "Mass in kg. Acceleration and VelocityChange ignore it."}},
            {"Rigidbody.useGravity", {"", "Whether the scene's gravity affects it."}},
            {"Rigidbody.isKinematic", {"", "Body moved by hand: the solver does not push it."}},
            {"Rigidbody.drag", {"", "Linear drag."}},
            {"Rigidbody.angularDrag", {"", "Angular drag."}},
            {"Rigidbody.constraints", {"", "Bitmask of frozen axes; built by OR-ing RigidbodyConstraints."}},
            {"Rigidbody.ccd", {"", "Continuous detection, against fast bodies tunneling. Not valid for kinematic."}},
            {"Rigidbody.interpolate", {"", "Smooths the VISUAL pose between fixed steps."}},
            {"Rigidbody.velocity", {"", "Linear velocity, in units per second."}},
            {"Rigidbody.angularVelocity", {"", "Angular velocity, in radians per second."}},
            {"Rigidbody:AddForce", {"(force: Vec3, mode?)", "Applies a force. The mode comes from the ForceMode table; Force by default."}},
            {"Rigidbody:AddTorque", {"(par: Vec3, modo?)", "Applies a torque."}},
            {"Rigidbody:AddImpulse", {"(impulse: Vec3)", "Shorthand for AddForce with ForceMode.Impulse."}},

            // --- Animator ---
            {"Animator:SetBool", {"(name, value)", "Sets a boolean parameter of the state machine."}},
            {"Animator:GetBool", {"(name) -> boolean", "Reads a boolean parameter."}},
            {"Animator:SetTrigger", {"(name)", "Fires a trigger; it is consumed by the first transition that uses it."}},
            {"Animator:SetInt", {"(name, value)", "Sets an integer parameter."}},
            {"Animator:GetInt", {"(name) -> number", "Reads an integer parameter."}},
            {"Animator:SetFloat", {"(name, value)", "Sets a float parameter."}},
            {"Animator:GetFloat", {"(name) -> number", "Reads a float parameter."}},
            {"Animator:GetState", {"([layer]) -> string", "Name of the layer's current state (0 = base, the default)."}},
            {"Animator:GetPreviousState", {"() -> string", "State being left during a cross-fade."}},
            {"Animator:IsBlending", {"([layer]) -> boolean", "Whether there is a cross-fade in progress on the layer (0 = base, the default)."}},
            {"Animator:GetBlendWeight", {"() -> number", "Weight 0..1 of the cross-fade in progress."}},
            {"Animator:GetPoseWeight", {"() -> number", "Weight sent to the GPU: the cross-fade's if there is one, otherwise the parameter blend's."}},
            {"Animator:Play", {"(state, [layer]) -> boolean", "Enters that state of the layer (0 = base) now, without blending. false if it does not exist."}},
            {"Animator:CrossFade", {"(state, seconds, [layer]) -> boolean", "Blends into that state of the layer (0 = base) over the given seconds. false if it does not exist."}},
            {"Animator:ResetTrigger", {"(name)", "Disarms a trigger that has not been consumed yet."}},
            {"Animator:SetSpeed", {"(speed)", "The Animator's global speed (1 = normal, 0 = frozen). Negative is clamped to 0."}},
            {"Animator:GetSpeed", {"() -> number", "The Animator's current global speed."}},
            {"Animator:GetNormalizedTime", {"([layer]) -> number", "Normalized time of the layer's current state: 1 = one loop; it keeps growing when looping."}},
            {"Animator:SetLayerWeight", {"(layer, weight)", "Weight 0..1 of an upper layer (the base is always 1)."}},
            {"Animator:GetLayerWeight", {"(layer) -> number", "Weight of the layer; 0 if it does not exist."}},
            {"Animator:GetLayerCount", {"() -> integer", "Number of layers, base included."}},
            {"Animator:SetIkWeight", {"(name, weight)", "Weight 0..1 of an IK constraint; a name that does not exist is ignored."}},
            {"Animator:GetIkWeight", {"(name) -> number", "Weight of the constraint; 0 if it does not exist."}},
            {"Animator:SetIkTarget", {"(name, entity)", "Target of the constraint; nil removes it and the IK stops applying."}},
            {"Animator:SetIkPole", {"(name, entity)", "Pole of a two-bone IK: where the elbow or knee points."}},
            {"Animator:GetIkCount", {"() -> integer", "Number of the Animator's IK constraints."}},

            // --- Audio ---
            {"Audio.SetBusVolume", {"(bus, volume)", "Volume 0..1 of \"master\", \"music\" or \"sfx\"."}},
            {"Audio.GetBusVolume", {"(bus) -> number", "Current volume of the bus."}},
            {"Audio.PlayClipAtPoint", {"(path, pos: Vec3, volume?)", "Fire-and-forget positional sound, without a GameObject."}},
            {"Audio.Preload", {"(path)", "Loads the clip now, so the first play is not lost."}},
            {"Audio.SetBusEffect", {"(bus, effect, params...)", "Per-bus effect: lowPass, highPass, echo or reverb."}},
            {"Audio.ClearBusEffect", {"(bus)", "Removes the effect from the bus."}},
            {"Audio.SetPaused", {"(paused)", "Pauses or resumes all audio."}},
            {"Audio.IsPaused", {"() -> boolean", "Whether audio is paused globally."}},
        };

        // Los catorce widgets comparten rect; el color de fondo también.
        for (const char* type : {"Button", "Text", "ProgressBar", "Layout", "Panel",
                                 "Image", "Slider", "Checkbox", "Toggle", "Scrollbar",
                                 "InputField", "Dropdown", "ScrollView"})
        {
            addRectAccessors(t, type);
            // Toggle es el único sin color de fondo: tiene OffColor y OnColor.
            if (std::string(type) != "Toggle")
                addColorAccessor(t, type, "Color", "Background color (rgba 0..1).");
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
