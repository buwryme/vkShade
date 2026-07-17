#ifndef EFFECT_RESHADE_HPP_INCLUDED
#define EFFECT_RESHADE_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <memory>
#include <array>

#include "vulkan_include.hpp"

#include "effect.hpp"
#include "config.hpp"
#include "effect_config.hpp"
#include "effect_registry.hpp"
#include "reshade_uniforms.hpp"

#include "logical_device.hpp"

#include "reshade/effect_parser.hpp"
#include "reshade/effect_codegen.hpp"
#include "reshade/effect_preprocessor.hpp"

namespace vkShade
{
    class ReshadeEffect : public Effect
    {
    public:
        ReshadeEffect(LogicalDevice*       pLogicalDevice,
                      VkFormat             format,
                      VkExtent2D           imageExtent,
                      std::vector<VkImage> inputImages,
                      std::vector<VkImage> outputImages,
                      EffectRegistry*      pEffectRegistry,
                      std::string          effectName,
                      std::string          effectPath = "",  // Optional: explicit path to .fx file
                      std::vector<PreprocessorDefinition> customDefs = {});  // Custom preprocessor definitions
        void virtual applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;
        void virtual updateEffect() override;
        void virtual useDepthImage(uint32_t imageIndex, VkImageView depthImageView, VkImageLayout depthImageLayout) override;
        std::vector<std::unique_ptr<EffectParam>> getParameters() const override;
        int getOutputWrites() const { return outputWrites; }
        virtual ~ReshadeEffect();

    private:
        LogicalDevice*           pLogicalDevice;
        std::vector<VkImage>     inputImages;
        std::vector<VkImage>     outputImages;
        std::vector<VkImageView> inputImageViewsSRGB;
        std::vector<VkImageView> inputImageViewsUNORM;
        std::vector<VkImageView> outputImageViewsSRGB;
        std::vector<VkImageView> outputImageViewsUNORM;

        std::unordered_map<std::string, std::vector<VkImage>>     textureImages;
        std::unordered_map<std::string, std::vector<VkImageView>> textureImageViewsUNORM;
        std::unordered_map<std::string, std::vector<VkImageView>> textureImageViewsSRGB;
        std::unordered_map<std::string, std::vector<VkImageView>> renderImageViewsSRGB;
        std::unordered_map<std::string, std::vector<VkImageView>> renderImageViewsUNORM;

        std::unordered_map<std::string, VkFormat>   textureFormatsUNORM;
        std::unordered_map<std::string, VkFormat>   textureFormatsSRGB;
        std::unordered_map<std::string, uint32_t>   textureMipLevels;
        std::unordered_map<std::string, VkExtent3D> textureExtents;

        std::vector<VkDescriptorSet> inputDescriptorSets;
        std::vector<VkDescriptorSet> outputDescriptorSets;
        std::vector<VkDescriptorSet> backBufferDescriptorSets;

        struct PassRuntime
        {
            bool                        isCompute = false;
            VkPipeline                  pipeline = VK_NULL_HANDLE;
            VkRenderPass                renderPass = VK_NULL_HANDLE;
            std::vector<VkFramebuffer>  framebuffers;
            std::vector<std::string>    renderTargets;
            bool                        switchSamplers = false;
            uint32_t                    vertexCount = 3;
            uint32_t                    dispatchSizeX = 1;
            uint32_t                    dispatchSizeY = 1;
            uint32_t                    dispatchSizeZ = 1;
            VkRect2D                    renderArea = {};
            uint32_t                    clearValueCount = 0;
            std::array<VkClearValue, 9> clearValues = {};
        };
        std::vector<PassRuntime> passRuntimes;

        VkDescriptorSetLayout                 uniformDescriptorSetLayout;
        VkDescriptorSetLayout                 imageSamplerDescriptorSetLayout;
        VkShaderModule                        shaderModule;
        VkDescriptorPool                      descriptorPool;
        VkPipelineLayout                      pipelineLayout;
        VkExtent2D                            imageExtent;
        std::vector<VkSampler>                samplers;
        EffectRegistry*                       pEffectRegistry;
        std::string                           effectName;
        std::string                           effectPath;  // Path to .fx file (may differ from effectName)
        std::vector<PreprocessorDefinition>   customPreprocessorDefs;  // User-defined macros
        reshadefx::module                     module;
        std::vector<VkDeviceMemory>           textureMemory;

        VkFormat    inputOutputFormatUNORM;
        VkFormat    inputOutputFormatSRGB;
        VkFormat    stencilFormat;
        VkImage     stencilImage;
        VkImageView stencilImageView;
        // how often the shader writes to the reshade back buffer
        // we need to flip the "backbuffer" after each write if there is a next one
        int                      outputWrites = 0;
        std::vector<VkImage>     backBufferImages;
        std::vector<bool>        outputImageInitialized;
        std::vector<VkImageView> backBufferImageViewsUNORM;
        std::vector<VkImageView> backBufferImageViewsSRGB;
        VkBuffer                 stagingBuffer;
        VkDeviceMemory           stagingBufferMemory;
        uint32_t                 bufferSize;
        void*                    stagingBufferMapped = nullptr;  // Persistent map (HOST_COHERENT)
        VkDescriptorSet          bufferDescriptorSet;
        bool                     disableComputePipelineOptimization = false;

        std::vector<std::shared_ptr<ReshadeUniform>> uniforms;

        void          createReshadeModule();
        VkFormat      convertReshadeFormat(reshadefx::texture_format texFormat);
        VkCompareOp   convertReshadeCompareOp(reshadefx::pass_stencil_func compareOp);
        VkStencilOp   convertReshadeStencilOp(reshadefx::pass_stencil_op stencilOp);
        VkBlendOp     convertReshadeBlendOp(reshadefx::pass_blend_op blendOp);
        VkBlendFactor convertReshadeBlendFactor(reshadefx::pass_blend_func blendFactor);
    };
} // namespace vkShade

#endif // EFFECT_RESHADE_HPP_INCLUDED
