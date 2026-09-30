#include "DonTopo/Scripting/LuaApiReference.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <utility>

namespace DonTopo {

namespace {

// Case-insensitive comparison: whoever types 'transform:' expects the
// same suggestions as whoever types 'Transform:'.
bool startsWithCaseInsensitive(const std::string& value, const std::string& prefix)
{
    if (value.size() < prefix.size())
        return false;
    return std::equal(prefix.begin(), prefix.end(), value.begin(),
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b)); });
}

// Static base: what ScriptBindings registers. The actions of the Input
// Actions panel do not go here; they change live, the editor publishes them.
const std::vector<std::string>& baseSymbols()
{
    static const std::vector<std::string> symbols = {
        // Keywords Lua
        "and", "break", "do", "else", "elseif", "end", "false", "for",
        "function", "goto", "if", "in", "local", "nil", "not", "or",
        "repeat", "return", "then", "true", "until", "while",

        // Globals
        "print",
        // DestroyGameObject(entity) — destroys the GameObject and its subtree in
        // Play (deferred to end of frame). See Scene / README.
        "DestroyGameObject",

        // self — script instance (implicit parameter of the
        // Script:Method functions). 'self.entity' is the only field injected by
        // ScriptManager for every script (see ScriptManager.cpp); the rest
        // of the fields are those defined by the script itself (not listable).
        "self", "self.entity",

        // Lifecycle callbacks — defined by the script (function Script:Name)
        // and called by the engine in Play Mode (see ScriptManager). Awake/Start once;
        // Update/LateUpdate every frame; FixedUpdate at a fixed step; OnDestroy
        // on destroy; OnTrigger* when another collider enters/stays/exits
        // an Is Trigger collider (they receive the Entity that caused it);
        // OnCollision* the same for REAL collisions (neither of the two
        // colliders is a trigger), on both objects of the pair; OnAnimationEvent
        // receives the event name when the object's Animator crosses it
        // (Play only).
        "Awake", "Start", "Update", "FixedUpdate", "LateUpdate", "OnDestroy",
        "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit",
        "OnCollisionEnter", "OnCollisionStay", "OnCollisionExit",
        "OnAnimationEvent",

        // Log
        "Log.Info", "Log.Warn", "Log.Error",

        // Input / Key / MouseButton
        "Input.IsKeyDown", "Input.IsKeyPressed", "Input.IsKeyReleased",
        "Input.IsMouseButtonDown",
        // Named actions of the Input Actions panel. The snippets with each
        // action's concrete name are published separately by the editor
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
        // Raw gamepad. The normal thing is to use named actions; this is for
        // when the script wants a specific button. Without a gamepad: always false.
        "Input.IsPadButtonDown", "Input.IsPadButtonPressed",
        "Input.IsPadAxisDown", "Input.IsPadAxisPressed",
        "PadButton.A", "PadButton.B", "PadButton.X", "PadButton.Y",
        "PadButton.LeftBumper", "PadButton.RightBumper",
        "PadButton.Back", "PadButton.Start", "PadButton.Guide",
        "PadButton.LeftThumb", "PadButton.RightThumb",
        "PadButton.DpadUp", "PadButton.DpadRight",
        "PadButton.DpadDown", "PadButton.DpadLeft",
        // An axis is TWO bindings (one direction each): these constants already
        // come composed, and PadAxis.Code(axis, negative) composes any other.
        "PadAxis.Code",
        "PadAxis.LeftStickUp", "PadAxis.LeftStickDown",
        "PadAxis.LeftStickLeft", "PadAxis.LeftStickRight",
        "PadAxis.RightStickUp", "PadAxis.RightStickDown",
        "PadAxis.RightStickLeft", "PadAxis.RightStickRight",
        "PadAxis.LeftTrigger", "PadAxis.RightTrigger",

        // Entity
        "Entity.name", "Entity:IsValid", "Entity:GetTransform",
        // Hides the mesh without destroying the object: it stays alive and colliding.
        "Entity.meshVisible",
        // Game light and camera, with the same shortcuts as the UI ones.
        "Entity:GetLight", "Entity:AddLight", "Entity:RemoveLight",
        "Entity:GetCamera", "Entity:AddCamera", "Entity:RemoveCamera",
        "Entity:GetLayout", "Entity:AddLayout", "Entity:RemoveLayout",
        "Entity:GetParent", "Entity:SetParent",
        "Entity:GetChildren", "Entity:GetComponent",
        "Entity:AddComponent", "Entity:RemoveComponent",
        // UI: named shortcuts for each UI component (registerUi).
        // The Get* return nil if the component is not there.
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
        // World position and axes of the object (already normalized). The convention
        // is the camera's: it looks toward local -Z.
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

        // Colliders (gravity/dynamics now live in Rigidbody)
        "BoxCollider:GetHalfExtents", "BoxCollider:SetHalfExtents",
        "BoxCollider:GetCenter", "BoxCollider:SetCenter",
        "SphereCollider:GetRadius", "SphereCollider:SetRadius",
        "SphereCollider:GetCenter", "SphereCollider:SetCenter",
        "CapsuleCollider:GetRadius", "CapsuleCollider:SetRadius",
        "CapsuleCollider:GetHalfHeight", "CapsuleCollider:SetHalfHeight",
        "CapsuleCollider:GetCenter", "CapsuleCollider:SetCenter",
        "PlaneCollider:GetCenter", "PlaneCollider:SetCenter",
        // Physics material per collider (properties, as in Rigidbody)
        "BoxCollider.staticFriction", "BoxCollider.dynamicFriction", "BoxCollider.bounciness",
        "SphereCollider.staticFriction", "SphereCollider.dynamicFriction", "SphereCollider.bounciness",
        "CapsuleCollider.staticFriction", "CapsuleCollider.dynamicFriction", "CapsuleCollider.bounciness",
        "PlaneCollider.staticFriction", "PlaneCollider.dynamicFriction", "PlaneCollider.bounciness",
        // Is Trigger per collider (overlaps without colliding and fires OnTrigger*).
        // The setter is a no-op outside Play, where there is no PhysicsManager.
        "BoxCollider.isTrigger", "SphereCollider.isTrigger",
        "CapsuleCollider.isTrigger", "PlaneCollider.isTrigger",
        // Collision layer per collider (0-31). What each layer collides with is
        // given by the global matrix: Physics.SetLayerCollision / GetLayerCollision.
        "BoxCollider.layer", "SphereCollider.layer",
        "CapsuleCollider.layer", "PlaneCollider.layer",

        // Rigidbody (Unity-style dynamics; GetComponent("Rigidbody"))
        "Rigidbody.mass", "Rigidbody.useGravity", "Rigidbody.isKinematic",
        "Rigidbody.drag", "Rigidbody.angularDrag",
        // Bitmask of frozen axes: composed with OR of the constants in
        // the RigidbodyConstraints table (below).
        "Rigidbody.constraints",
        // Continuous detection (against fast bodies tunneling) and
        // visual interpolation of the pose between fixed steps. Both false by
        // default and independent of each other.
        "Rigidbody.ccd", "Rigidbody.interpolate",
        "Rigidbody.velocity", "Rigidbody.angularVelocity",
        "Rigidbody:AddForce", "Rigidbody:AddTorque", "Rigidbody:AddImpulse",
        "RigidbodyConstraints.None",
        "RigidbodyConstraints.FreezePositionX", "RigidbodyConstraints.FreezePositionY",
        "RigidbodyConstraints.FreezePositionZ",
        "RigidbodyConstraints.FreezeRotationX", "RigidbodyConstraints.FreezeRotationY",
        "RigidbodyConstraints.FreezeRotationZ",
        // Optional mode (4th argument) of AddForce/AddTorque. Without it, Force.
        "ForceMode.Force", "ForceMode.Acceleration",
        "ForceMode.Impulse", "ForceMode.VelocityChange",

        // Animator (state machine; GetComponent("Animator"))
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

        // Global audio (volumes per bus: "master", "music", "sfx")
        "Audio.SetBusVolume", "Audio.GetBusVolume",
        // Positional sound without a GameObject. Preload prevents the first shot
        // from being lost to FMOD's deferred loading.
        "Audio.PlayClipAtPoint", "Audio.Preload",
        // Effects per bus: lowPass, highPass, echo, reverb.
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
        // SafeArea is four loose insets: left, top, right, bottom.
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
        // GetState returns a UiButtonState; OnClick/OnDoubleClick register
        // the Lua function that the canvas calls.
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
        // The already-bounded 0..1 that the sync uses for the fill rect.
        "ProgressBar:GetNormalizedValue",

        // 2D UI — Layout (the container that places the children)
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


        // 2D UI — Slider (interactive widget)
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

        // 2D UI — Checkbox (interactive widget)
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

        // 2D UI — Toggle (interactive widget)
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

        // 2D UI — Scrollbar (interactive widget)
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

        // 2D UI — Panel (the background rectangle)
        "Panel.visible", "Panel.raycastTarget", "Panel.atlasPath", "Panel.sprite",
        "Panel:GetAnchorMin", "Panel:SetAnchorMin",
        "Panel:GetAnchorMax", "Panel:SetAnchorMax",
        "Panel:GetPivot", "Panel:SetPivot",
        "Panel:GetPosition", "Panel:SetPosition",
        "Panel:GetSize", "Panel:SetSize",
        "Panel:GetColor", "Panel:SetColor",

        // 2D UI — Image (sprite with Normal/Tiled/Sliced/Filled)
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

        // 2D UI — enums (integer tables that registerUi registers)
        "UiScaleMode.ConstantPixelSize", "UiScaleMode.ScaleWithScreenSize",
        "UiScaleMode.ConstantPhysicalSize",
        "UiScreenMatch.MatchWidthOrHeight", "UiScreenMatch.Expand",
        "UiScreenMatch.Shrink",
        "UiCanvasRenderMode.ScreenSpace", "UiCanvasRenderMode.World",
        "UiBillboard.None", "UiBillboard.YawOnly", "UiBillboard.Full",
        "UiTextAlign.Left", "UiTextAlign.Center", "UiTextAlign.Right",
        "UiTextAlign.Justify",
        // Vertical: registerUi registers it like any other and it had been
        // missing from this list since then (the README documented it as an absence).
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

        // Physics — ray queries (nil / false if there is no physics scene,
        // that is outside Play). The loose names are the fields of the
        // 'options' table and those of the table that Raycast returns.
        // RaycastAll returns an array of those same tables (empty if there are no
        // hits, never nil), sorted by distance.
        "Physics.Raycast", "Physics.RaycastAll", "Physics.RaycastHit",
        "hitTriggers", "static", "dynamic", "ignore",
        "entity", "point", "normal", "distance",

        // Physics — sweep and overlaps, same filters ('options') as the ray.
        // SphereCast returns the same hit table as Raycast; both
        // Overlap return an array of Entity (empty if nothing overlaps, never
        // nil): an overlap has no point, normal or distance.
        "Physics.SphereCast", "Physics.OverlapSphere", "Physics.OverlapBox",

        // Physics — collision layer matrix (32x32, symmetric). Index
        // out of [0,31]: Lua error.
        "Physics.SetLayerCollision", "Physics.GetLayerCollision",

        // Engine (scene change at runtime)
        "DonTopo.loadScene",

        // Vec3
        "Vec3.new",
        // Fields and algebra. The methods use ':' because they operate on an
        // instance: 'v:Length()', 'a:Dot(b)'.
        "Vec3.x", "Vec3.y", "Vec3.z",
        "Vec3:Length", "Vec3:Normalized", "Vec3:Dot", "Vec3:Cross",
        "Vec3:Distance", "Vec3:Lerp",

        // Time — the scripts' clock. It restarts on every Play.
        "Time.deltaTime", "Time.fixedDeltaTime", "Time.time", "Time.frameCount",
    };
    return symbols;
}

// ---------------------------------------------------------------------------
// Signatures and documentation
//
// Table SEPARATE from the symbol list on purpose. The list above remains
// the authority on what exists (it is the one to touch when adding a
// binding, and the project rule does not change); this is only help text.
// A symbol without an entry here shows up in the popup all the same, without a signature: it never
// disappears for not being documented.
//
// The mechanical UI families (the ten rect accessors that the
// fourteen widgets repeat) are generated in a loop instead of being written one hundred
// and forty times: the text would be identical and copying it only guarantees that one day one of
// the fourteen is left without being updated.
// ---------------------------------------------------------------------------

struct DocEntry { const char* signature; const char* doc; };

void addRectAccessors(std::unordered_map<std::string, DocEntry>& out, const std::string& type)
{
    // The pairs of a rect come in and go out as TWO loose numbers, not as a
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

        // The fourteen widgets share the rect; the background color too.
        for (const char* type : {"Button", "Text", "ProgressBar", "Layout", "Panel",
                                 "Image", "Slider", "Checkbox", "Toggle", "Scrollbar",
                                 "InputField", "Dropdown", "ScrollView"})
        {
            addRectAccessors(t, type);
            // Toggle is the only one without a background color: it has OffColor and OnColor.
            if (std::string(type) != "Toggle")
                addColorAccessor(t, type, "Color", "Background color (rgba 0..1).");
        }
        return t;
    }();
    return table;
}

// Per-action snippets published by the editor; empty in the exported runtime.
std::vector<std::string> g_actionSymbols;
// base + dynamic; it is rebuilt only when the dynamic ones change.
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

    // The fragment is split at the LAST separator: "Entity:GetT" -> receiver
    // "Entity", separator ':', member "GetT". Without a separator, everything is member.
    const std::size_t sep = fragment.find_last_of(".:");
    const bool hasSeparator = (sep != std::string::npos);
    const std::string receiver = hasSeparator ? fragment.substr(0, sep) : std::string();
    const char separator = hasSeparator ? fragment[sep] : '\0';
    const std::string member = hasSeparator ? fragment.substr(sep + 1) : fragment;
    // Where the replacement starts when the receiver is NOT a known type:
    // right after the separator, to keep what the user wrote.
    const std::size_t memberOffset = hasSeparator ? sep + 1 : 0;

    // Rank 0: the whole symbol starts with what was typed. It is what there was before
    // and what a user expects when typing the type name.
    // Rank 1: the receiver is a local variable ("t:Get"), so it is searched
    // by the MEMBER name in any type. Without this, the normal case
    // (calling through a variable) offered nothing.
    struct Scored { int rank; const std::string* symbol; std::size_t offset; };
    std::vector<Scored> scored;

    for (const std::string& symbol : luaApiSymbols())
    {
        if (startsWithCaseInsensitive(symbol, fragment))
        {
            scored.push_back({0, &symbol, 0});
            continue;
        }

        // For rank 1 the symbol has to be split at ITS separator and only the
        // member part compared. A symbol without a separator (a Lua
        // keyword, a global) has no member to offer here.
        const std::size_t symSep = symbol.find_last_of(".:");
        if (symSep == std::string::npos) continue;
        // The separator has to match: '.' is a property and ':' is a method,
        // and offering a method where a dot was typed would be a suggestion
        // that does not compile.
        if (hasSeparator && symbol[symSep] != separator) continue;
        // With nothing typed after the separator ("t:") there is no member to
        // filter: all those of the requested separator are offered.
        if (!member.empty() &&
            !startsWithCaseInsensitive(symbol.substr(symSep + 1), member)) continue;
        // Without a separator in what was typed, this is typing "Get" bare: it counts as a
        // member search, but the whole fragment is replaced.
        scored.push_back({1, &symbol, hasSeparator ? memberOffset : 0});
    }

    // Total, deterministic order: rank, then length (the shortest is usually
    // the most general), then alphabetical. Without the name tie-break the order
    // would depend on the table's and a test could not pin it down.
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
        // With an offset the receiver written by the user is kept and only the
        // member is written; without it, the whole fragment is replaced.
        m.insert = (s.offset == 0) ? m.symbol
                                   : m.symbol.substr(m.symbol.find_last_of(".:") + 1);
        out.push_back(std::move(m));
    }
    return out;
}

} // namespace DonTopo
