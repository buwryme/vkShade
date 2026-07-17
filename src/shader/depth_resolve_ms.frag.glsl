#version 450

layout(set = 0, binding = 0) uniform sampler2DMS depthTexture;

layout(location = 0) out float resolvedDepth;

void main()
{
    resolvedDepth = texelFetch(depthTexture, ivec2(gl_FragCoord.xy), 0).r;
}
