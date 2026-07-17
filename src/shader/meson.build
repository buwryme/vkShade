shader_src = [
    'cas.frag.glsl',
    'depth_resolve.frag.glsl',
    'depth_resolve_ms.frag.glsl',
    'depth_resolve_msaa.frag.glsl',
    # Alpha channel variants
    'depth_resolve_alpha.frag.glsl',
    'depth_resolve_alpha_ms.frag.glsl',
    'depth_resolve_alpha_msaa.frag.glsl',
    # Inverted variants
    'depth_resolve_inverted.frag.glsl',
    'depth_resolve_inverted_ms.frag.glsl',
    'depth_resolve_inverted_msaa.frag.glsl',
    # Universal (comprehensive mode support)
    'depth_resolve_universal.frag.glsl',
    'depth_resolve_universal_ms.frag.glsl',
    'depth_resolve_universal_msaa.frag.glsl',
    'deband.frag.glsl',
    'dls.frag.glsl',
    'full_screen_triangle.vert.glsl',
    'fxaa.frag.glsl',
    'lut.frag.glsl',
    'smaa_blend.frag.glsl',
    'smaa_blend.vert.glsl',
    'smaa_edge_color.frag.glsl',
    'smaa_edge_luma.frag.glsl',
    'smaa_edge.vert.glsl',
    'smaa_neighbor.frag.glsl',
    'smaa_neighbor.vert.glsl',
]

glsl_compiler = find_program('glslangValidator')
glsl_generator = generator(glsl_compiler,
    output    : [ '@BASENAME@.h' ],
    arguments : [ '-V', '-x', '@INPUT@', '-o', '@OUTPUT@' ])

shader_include = glsl_generator.process(shader_src)
