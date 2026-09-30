#pragma once
#include <string>
#include <vector>

namespace DonTopo
{
    class GameObject;

    // Property clips (row 15 / C14 of the animation audit): what
    // makes it possible to animate with the Animator an object WITHOUT a skeleton — a door, a
    // platform, a blinking light, a material that lights up.
    //
    // Each track animates ONE scalar. A color or a position is three tracks: this way
    // the table is one float per property (no type variants) and one can
    // animate only the Y of a door without touching its X and Z.
    enum class PropertyId
    {
        PositionX, PositionY, PositionZ,       // local, scene units
        RotationX, RotationY, RotationZ,       // local, GRADOS (euler XYZ)
        ScaleX, ScaleY, ScaleZ,                // local
        LightColorR, LightColorG, LightColorB,
        LightIntensity, LightRange,
        MaterialMetallic, MaterialRoughness,
        Count
    };

    struct PropertyKey { float time = 0.0f; float value = 0.0f; };   // time in SECONDS

    // Where the track value goes. Property: a GameObject property
    // (a door, a light, a material). Parameter: a Float parameter of the
    // Animator —a "clip curve"—, which lets the animation's own time
    // drive the state machine or feed speedParam.
    enum class TrackTarget { Property, Parameter };

    struct PropertyTrack
    {
        TrackTarget              target = TrackTarget::Property;
        PropertyId               property = PropertyId::PositionX;   // si target == Property
        std::string              parameterName;                      // si target == Parameter
        std::vector<PropertyKey> keys;
        // Property: the object has the component that is needed. Parameter:
        // there is a Float parameter with that name. Set by
        // AnimatorComponent::bindProperties; without it, the track is not applied.
        bool                     resolved = false;
    };

    struct PropertyClip
    {
        std::string                name;
        float                      duration = 1.0f;   // seconds, > 0
        std::vector<PropertyTrack> tracks;
    };

    // Stable name of the property: it is what is saved in the .scene and what is
    // shown in the panel. propertyFromName returns Count if it does not exist.
    const char* propertyName(PropertyId id);
    PropertyId  propertyFromName(const std::string& n);
    bool        propertyIsRotation(PropertyId id);

    // Value of the track at `tiempo` (seconds): linear between the two keys that
    // surround it; outside the range, the end key; without keys, `actual`.
    float samplePropertyTrack(const PropertyTrack& t, float tiempo, float actual);

    // Vertical range to draw a track with: covers its keys and the values
    // of `extra` (the condition thresholds), with a margin. A flat track
    // —or one without keys— cannot give a zero height range, or the line
    // would be stuck to the edge: it opens to ±0.5.
    void curveRange(const PropertyTrack& t, const float* extra, int nExtra, float& lo, float& hi);

    // Conversion between the curve canvas and the track data, so that keys can be
    // dragged with the mouse. It lives here, and not in the panel, because it is
    // where the off-by-ones fit and it is the only part of that gesture that can be tested
    // without a window.
    //
    // `x0`/`x1` and `y0`/`y1` are the edges of the on-screen rectangle (y grows
    // DOWNWARD, opposite to the value). The time comes out bounded to
    // [0, duracion]; the value is not bounded, because the drawing range adjusts
    // to whatever there is.
    struct CurvePoint { float time = 0.0f; float value = 0.0f; };
    CurvePoint canvasToCurve(float x, float y, float x0, float x1, float y0, float y1,
                             float duracion, float lo, float hi);
    // The inverse: where a key lands on screen. With a time outside the clip
    // it returns the edge, which is where it is drawn.
    void curveToCanvas(float time, float value, float x0, float x1, float y0, float y1,
                       float duracion, float lo, float hi, float& x, float& y);

    // A contribution to a property: its value and the weight with which it enters (that
    // of the cross-fade times that of its layer).
    struct PropertyContribution { float value = 0.0f; float weight = 0.0f; };
    // Blend of several contributions to the SAME property. Rotations go
    // the short way: 350 and 10 give 0, not 180.
    float blendPropertyValues(PropertyId id, const PropertyContribution* c, int n);
    // Plain weighted average. It is what a curve uses: a parameter is not an
    // angle, so there is no short way to respect.
    float blendScalarValues(const PropertyContribution* c, int n);

    // --- Access to the properties of a GameObject ---
    // The object has what is needed for this property (the light, above
    // all). A track without it is warned about on resolve and not applied.
    bool  propertyAvailable(const GameObject& go, PropertyId id);
    float propertyGet(const GameObject& go, PropertyId id);
    // Writes ONLY the properties marked in `escritas` (a bool and a float
    // per PropertyId): what nobody animates is not touched. The transform is
    // decomposed and recomposed ONCE, not once per property.
    void  propertyApply(GameObject& go, const bool* escritas, const float* valores);
}
