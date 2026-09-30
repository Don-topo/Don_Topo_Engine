#pragma once
#include "DonTopo/Editor/Thumbnail.h"
#include "DonTopo/Renderer/ModelLoader.h"

#include <vector>

// Software rasterizer for model and material thumbnails. Pure CPU, called
// from a worker. It is not the renderer: no IBL, no normal map, no shadows.
// Enough to recognize silhouette, color and brightness at 64 px (spike of
// 2026-09-25, see docs/superpowers/specs/2026-09-25-model-material-thumbnails-design.md).
namespace DonTopo {

// kThumbCell x kThumbCell RGBA8 sRGB cell with alpha. Unreadable if no
// triangle covers a pixel (empty, zero area, NaN, indices out of range).
// Deterministic: same input, same bytes. dependencies empty.
ThumbnailResult rasterizeThumbnail(const std::vector<PreviewPart>& parts);

// UV sphere of radius 1 (48 x 24), white colors, uv with the texture twice
// around. Base of the .mat thumbnails.
PreviewPart makePreviewSphere();

} // namespace DonTopo
