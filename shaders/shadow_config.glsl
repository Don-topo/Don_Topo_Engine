// Sizes of the UBO's shadow block, in a SINGLE place on the GLSL side.
//
// These three numbers were written by hand in the SIX shaders that declare the
// block (pbr.frag, triangle.frag, triangle.vert, outline.vert, shadow.vert and
// fog.comp). Changing them required getting it right six times, and failing gives no
// error: std140 silently shifts everything behind lightSpaceMatrix and
// the shader reads the shifted UBO. It is the H76 pattern (constants duplicated between
// consumers of the same block) applied to the shader side.
//
// The other unavoidable copy is the C++ one (UniformBufferObject.h): a shader cannot
// include a C++ header. Two places, not eight, and the two documented in
// each other.
#ifndef DT_SHADOW_CONFIG_GLSL
#define DT_SHADOW_CONFIG_GLSL

// Cascades of the key light when it is directional.
#define SHADOW_CASCADES 4

// Slots reserved for the KEY light: 4 if it is directional (one per cascade), 6 if it is
// a point light or a very wide spot (one per cubemap face), 1 if it is a normal
// spot. They never coexist: there is only one key and it has only one type.
#define SHADOW_KEY_MATRICES 6

// Layers for the SECONDARY lights, behind the key's. One per narrow
// spot, six per point light or very wide spot. With six, a point light
// OR six spots fit.
//
// The limit is MEMORY: these layers live in the same array as the key's,
// that is, at the same resolution, and at 2048 each one is 16 MB. Giving them
// a separate smaller array would allow raising them without paying for it, but it requires a new
// binding in the six shaders of the block.
#define SHADOW_EXTRA_LAYERS 6

// Total of the UBO's array. It has to have the same value as SHADOW_MATRICES in
// UniformBufferObject.h.
#define SHADOW_MATRICES (SHADOW_KEY_MATRICES + SHADOW_EXTRA_LAYERS)

#endif
