#version 450

layout(set = 0, binding = 0) uniform sampler2D depthTexture;

layout(location = 0) in vec2 textureCoord;
layout(location = 0) out float resolvedDepth;

void main()
{
    resolvedDepth = textureLod(depthTexture, textureCoord, 0.0).r;
}
