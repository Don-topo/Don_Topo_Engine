// How many lights the UBO block declares, in a SINGLE place on the GLSL side.
//
// It was written by hand in the THREE shaders that declare the array (pbr.frag,
// triangle.frag and fog.comp). Getting one wrong gives no error: std140 silently
// shifts everything behind the array (viewPos, numLights,
// ambientIntensity) and that shader reads the shifted UBO. Same pattern and same
// fix as shadow_config.glsl, which does this with the sizes of the shadow block;
// it goes in a separate file because MAX_LIGHTS is not a shadow constant and
// putting it there would leave the name lying.
//
// The other copy is unavoidable: the C++ one (UniformBufferObject.h), because a
// shader cannot include a C++ header. Two places, not four, and the two
// documented in each other.
#ifndef DT_LIGHTS_CONFIG_GLSL
#define DT_LIGHTS_CONFIG_GLSL

// It has to have the same value as MAX_LIGHTS in UniformBufferObject.h. See there
// why it is 64 and what has to be touched when changing it.
#define MAX_LIGHTS 64

#endif
