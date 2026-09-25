#pragma once
#include "DonTopo/Editor/Thumbnail.h"
#include "DonTopo/Renderer/ModelLoader.h"

#include <vector>

// Rasterizador por software de las miniaturas de modelos y materiales. CPU pura,
// se llama desde un worker. No es el renderer: sin IBL, sin normal map, sin
// sombras. Basta para reconocer silueta, color y brillo a 64 px (spike del
// 2026-09-25, ver docs/superpowers/specs/2026-09-25-model-material-thumbnails-design.md).
namespace DonTopo {

// Casilla kThumbCell x kThumbCell RGBA8 sRGB con alfa. Unreadable si ningun
// triangulo llega a cubrir un pixel (vacio, sin area, NaN, indices fuera de
// rango). Determinista: misma entrada, mismos bytes. dependencies vacia.
ThumbnailResult rasterizeThumbnail(const std::vector<PreviewPart>& parts);

// Esfera UV de radio 1 (48 x 24), colores blancos, uv con la textura dos veces
// alrededor. Base de las miniaturas de .mat.
PreviewPart makePreviewSphere();

} // namespace DonTopo
