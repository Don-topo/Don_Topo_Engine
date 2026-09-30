#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D uLogo;

// Three loose floats (not a vec2) to avoid the std430 padding of vec2 to 8
// bytes: the C++ sends { float alpha; float imgAR; float screenAR; } = 12 contiguous
// bytes (see SplashScreen::recordDraw, Task 3).
layout(push_constant) uniform Push {
    float alpha;     // fade [0,1]
    float imgAR;     // logoW/logoH
    float screenAR;  // screenW/screenH
} push;

// Splash background color (very dark gray, not pure black so that the fade
// is noticeable over the window).
const vec3 kBg = vec3(0.05, 0.05, 0.06);

void main() {
    // Letterbox: fit the logo keeping its aspect ratio inside the
    // screen. scale = screen ratio / logo ratio per axis.
    float logoAR   = push.imgAR;
    float screenAR = push.screenAR;

    vec2 uv = inUV - 0.5;
    if (screenAR > logoAR) uv.x *= screenAR / logoAR; // wider screen: side bars
    else                   uv.y *= logoAR / screenAR; // taller screen: bars on top/bottom
    uv += 0.5;

    vec3 col = kBg;
    if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
        vec4 logo = texture(uLogo, uv);
        col = mix(kBg, logo.rgb, logo.a); // respects the alpha of the logo's PNG
    }
    outColor = vec4(col * push.alpha, 1.0); // fade to background-black via uniform alpha
}
