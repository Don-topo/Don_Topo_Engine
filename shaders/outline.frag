#version 450

layout(location = 0) out vec4 outColor;

void main()
{
    // Orange: no other debug-draw uses it (colliders yellow, camera frustum
    // cyan, wireframe green), so the selection is not confused
    // with them even with wireframe mode active.
    outColor = vec4(1.0, 0.45, 0.05, 1.0);
}
