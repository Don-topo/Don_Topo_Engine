#version 450

layout(location = 0) out vec4 outColor;

void main()
{
    // Naranja: no lo usa ningun otro debug-draw (colliders amarillo, frustum
    // de camara cian, wireframe verde), asi que la seleccion no se confunde
    // con ellos ni siquiera con el modo wireframe activo.
    outColor = vec4(1.0, 0.45, 0.05, 1.0);
}
