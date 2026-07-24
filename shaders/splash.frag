#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D uLogo;

// Tres floats sueltos (no vec2) para evitar el padding std430 de vec2 a 8
// bytes: el C++ envia { float alpha; float imgAR; float screenAR; } = 12 bytes
// contiguos (ver SplashScreen::recordDraw, Task 3).
layout(push_constant) uniform Push {
    float alpha;     // fade [0,1]
    float imgAR;     // logoW/logoH
    float screenAR;  // screenW/screenH
} push;

// Color de fondo del splash (gris muy oscuro, no negro puro para que el fade
// se aprecie sobre la ventana).
const vec3 kBg = vec3(0.05, 0.05, 0.06);

void main() {
    // Letterbox: encajar el logo manteniendo su aspect ratio dentro de la
    // pantalla. scale = ratio pantalla / ratio logo por eje.
    float logoAR   = push.imgAR;
    float screenAR = push.screenAR;

    vec2 uv = inUV - 0.5;
    if (screenAR > logoAR) uv.x *= screenAR / logoAR; // pantalla mas ancha: barras laterales
    else                   uv.y *= logoAR / screenAR; // pantalla mas alta: barras arriba/abajo
    uv += 0.5;

    vec3 col = kBg;
    if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
        vec4 logo = texture(uLogo, uv);
        col = mix(kBg, logo.rgb, logo.a); // respeta el alpha del PNG del logo
    }
    outColor = vec4(col * push.alpha, 1.0); // fade a negro-fondo via alpha uniforme
}
