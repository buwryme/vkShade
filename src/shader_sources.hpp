#pragma once

#include <vector>
#include <cstdint>

namespace vkShade
{
    const std::vector<uint32_t> cas_frag = {
#include "cas.frag.h"
    };

    const std::vector<uint32_t> depth_resolve_frag = {
#include "depth_resolve.frag.h"
    };

    const std::vector<uint32_t> depth_resolve_ms_frag = {
#include "depth_resolve_ms.frag.h"
    };

    const std::vector<uint32_t> depth_resolve_alpha_frag = {
#include "depth_resolve_alpha.frag.h"
    };

    const std::vector<uint32_t> depth_resolve_alpha_ms_frag = {
#include "depth_resolve_alpha_ms.frag.h"
    };

    const std::vector<uint32_t> depth_resolve_inverted_frag = {
#include "depth_resolve_inverted.frag.h"
    };

    const std::vector<uint32_t> depth_resolve_inverted_ms_frag = {
#include "depth_resolve_inverted_ms.frag.h"
    };

    // Universal depth resolve shaders (comprehensive mode support)
    const std::vector<uint32_t> depth_resolve_universal_frag = {
#include "depth_resolve_universal.frag.h"
    };

    const std::vector<uint32_t> depth_resolve_universal_ms_frag = {
#include "depth_resolve_universal_ms.frag.h"
    };

    const std::vector<uint32_t> deband_frag = {
#include "deband.frag.h"
    };

    const std::vector<uint32_t> dls_frag = {
#include "dls.frag.h"
    };

    const std::vector<uint32_t> full_screen_triangle_vert = {
#include "full_screen_triangle.vert.h"
    };

    const std::vector<uint32_t> fxaa_frag = {
#include "fxaa.frag.h"
    };

    const std::vector<uint32_t> lut_frag = {
#include "lut.frag.h"
    };

    const std::vector<uint32_t> smaa_blend_frag = {
#include "smaa_blend.frag.h"
    };

    const std::vector<uint32_t> smaa_blend_vert = {
#include "smaa_blend.vert.h"
    };

    const std::vector<uint32_t> smaa_edge_color_frag = {
#include "smaa_edge_color.frag.h"
    };

    const std::vector<uint32_t> smaa_edge_luma_frag = {
#include "smaa_edge_luma.frag.h"
    };

    const std::vector<uint32_t> smaa_edge_vert = {
#include "smaa_edge.vert.h"
    };

    const std::vector<uint32_t> smaa_neighbor_frag = {
#include "smaa_neighbor.frag.h"
    };

    const std::vector<uint32_t> smaa_neighbor_vert = {
#include "smaa_neighbor.vert.h"
    };
} // namespace vkShade
