#pragma once
#include <string>
#include <vector>

namespace DonTopo
{
    class GameObject;

    // Clips de propiedades (fila 15 / C14 del audit de animación): lo que
    // permite animar con el Animator un objeto SIN esqueleto — una puerta, una
    // plataforma, una luz que parpadea, un material que se enciende.
    //
    // Cada pista anima UN escalar. Un color o una posición son tres pistas: así
    // la tabla es un float por propiedad (sin variantes de tipo) y se puede
    // animar solo la Y de una puerta sin tocar su X y su Z.
    enum class PropertyId
    {
        PositionX, PositionY, PositionZ,       // local, unidades de escena
        RotationX, RotationY, RotationZ,       // local, GRADOS (euler XYZ)
        ScaleX, ScaleY, ScaleZ,                // local
        LightColorR, LightColorG, LightColorB,
        LightIntensity, LightRange,
        MaterialMetallic, MaterialRoughness,
        Count
    };

    struct PropertyKey { float time = 0.0f; float value = 0.0f; };   // time en SEGUNDOS

    struct PropertyTrack
    {
        PropertyId               property = PropertyId::PositionX;
        std::vector<PropertyKey> keys;
        // El objeto tiene el componente que hace falta (lo fija
        // AnimatorComponent::bindProperties). Sin él, la pista no se aplica.
        bool                     resolved = false;
    };

    struct PropertyClip
    {
        std::string                name;
        float                      duration = 1.0f;   // segundos, > 0
        std::vector<PropertyTrack> tracks;
    };

    // Nombre estable de la propiedad: es lo que se guarda en el .scene y lo que
    // se ve en el panel. propertyFromName devuelve Count si no existe.
    const char* propertyName(PropertyId id);
    PropertyId  propertyFromName(const std::string& n);
    bool        propertyIsRotation(PropertyId id);

    // Valor de la pista en `tiempo` (segundos): lineal entre las dos keys que
    // lo rodean; fuera del rango, la key del extremo; sin keys, `actual`.
    float samplePropertyTrack(const PropertyTrack& t, float tiempo, float actual);

    // Una aportación a una propiedad: su valor y el peso con el que entra (el
    // del cross-fade por el de su capa).
    struct PropertyContribution { float value = 0.0f; float weight = 0.0f; };
    // Mezcla de varias aportaciones a la MISMA propiedad. Las rotaciones van
    // por el camino corto: 350 y 10 dan 0, no 180.
    float blendPropertyValues(PropertyId id, const PropertyContribution* c, int n);
}
