#include "effect_reshade.hpp"

#include <cstring>
#include <climits>
#include <cstdlib>
#include <cassert>

#include <set>
#include <algorithm>
#include <filesystem>
#include <unordered_map>

#include "image_view.hpp"
#include "descriptor_set.hpp"
#include "buffer.hpp"
#include "renderpass.hpp"
#include "graphics_pipeline.hpp"
#include "framebuffer.hpp"
#include "shader.hpp"
#include "sampler.hpp"
#include "image.hpp"
#include "format.hpp"
#include "config_serializer.hpp"
#include "settings_manager.hpp"
#include "reshade/reshade_depth_macros.hpp"

#include "util.hpp"

#include "stb_image.h"
#include "stb_image_dds.h"
#include "stb_image_resize.h"

namespace VKIntox
{
    namespace
    {
        bool hasFatalCompilerDiagnostics(const std::string& diagnostics)
        {
            return diagnostics.find(" error ") != std::string::npos ||
                   diagnostics.find(": error ") != std::string::npos ||
                   diagnostics.find("error X") != std::string::npos ||
                   diagnostics.find("preprocessor error:") != std::string::npos;
        }

        struct RuntimeCodegenPolicy
        {
            bool useLocalSizeId = true;
            bool useUniformSpecConstants = true;
            bool emitDebugInfo = true;
            bool disableComputePipelineOptimization = false;
        };

        bool getLocalSizeIdEnabledOverride(bool& hasOverride, bool defaultValue)
        {
            hasOverride = false;
            const char* value = std::getenv("VKINTOX_ENABLE_LOCAL_SIZE_ID");
            if (value == nullptr || *value == '\0')
                return defaultValue;

            hasOverride = true;
            if (value[0] == '0')
                return false;
            if (value[0] == '1')
                return true;

            Logger::warn(std::string("Invalid VKINTOX_ENABLE_LOCAL_SIZE_ID value '") + value +
                         "'. Expected 0 or 1.");
            return defaultValue;
        }

        RuntimeCodegenPolicy selectRuntimeCodegenPolicy(const LogicalDevice* logicalDevice)
        {
            RuntimeCodegenPolicy policy;
            const bool isNvidiaGpu = (logicalDevice != nullptr) && logicalDevice->isNvidiaGpu;
            policy.useUniformSpecConstants = !isNvidiaGpu;
            policy.emitDebugInfo = !isNvidiaGpu;
            policy.disableComputePipelineOptimization = isNvidiaGpu;

            bool hasLocalSizeIdOverride = false;
            policy.useLocalSizeId = getLocalSizeIdEnabledOverride(hasLocalSizeIdOverride, !isNvidiaGpu);
            return policy;
        }

        bool hasSourceAnnotation(const reshadefx::uniform_info& uniform)
        {
            return std::any_of(uniform.annotations.begin(), uniform.annotations.end(), [](const auto& a) {
                return a.name == "source";
            });
        }

        bool isReshadeUniformLogEnabled()
        {
            const char* value = std::getenv("VKINTOX_DEBUG_LOG_RESHADE_UI_UNIFORMS");
            if (value == nullptr || *value == '\0')
                return false;
            return value[0] != '0';
        }

        bool shouldLogReshadeUiUniform(const std::string& effectName, const std::string& uniformName)
        {
            if (!isReshadeUniformLogEnabled() || effectName != "DisplayDepth")
                return false;

            return uniformName == "fUIFarPlane"
                || uniformName == "iUIUpsideDown"
                || uniformName == "iUIReversed"
                || uniformName == "iUILogarithmic"
                || uniformName == "iUIPresentType"
                || uniformName == "bUIUsePreprocessorDefs";
        }

        std::string describeReshadeUiUniformValue(const EffectParam& param)
        {
            if (const auto* p = dynamic_cast<const FloatParam*>(&param))
                return std::to_string(p->value);
            if (const auto* p = dynamic_cast<const IntParam*>(&param))
                return std::to_string(p->value);
            if (const auto* p = dynamic_cast<const BoolParam*>(&param))
                return p->value ? "true" : "false";
            if (const auto* p = dynamic_cast<const FloatVecParam*>(&param))
            {
                std::string value = "[";
                for (uint32_t i = 0; i < p->componentCount; ++i)
                {
                    if (i != 0)
                        value += ", ";
                    value += std::to_string(p->value[i]);
                }
                value += "]";
                return value;
            }
            if (const auto* p = dynamic_cast<const IntVecParam*>(&param))
            {
                std::string value = "[";
                for (uint32_t i = 0; i < p->componentCount; ++i)
                {
                    if (i != 0)
                        value += ", ";
                    value += std::to_string(p->value[i]);
                }
                value += "]";
                return value;
            }
            if (const auto* p = dynamic_cast<const UintParam*>(&param))
                return std::to_string(p->value);
            if (const auto* p = dynamic_cast<const UintVecParam*>(&param))
            {
                std::string value = "[";
                for (uint32_t i = 0; i < p->componentCount; ++i)
                {
                    if (i != 0)
                        value += ", ";
                    value += std::to_string(p->value[i]);
                }
                value += "]";
                return value;
            }
            return "<unsupported>";
        }

        void maybeLogReshadeUiUniformWrite(const std::string& effectName, const std::string& uniformName, const EffectParam& param)
        {
            if (!shouldLogReshadeUiUniform(effectName, uniformName))
                return;

            static std::unordered_map<std::string, std::string> lastValues;
            const std::string key = effectName + "::" + uniformName;
            const std::string value = describeReshadeUiUniformValue(param);

            auto it = lastValues.find(key);
            if (it != lastValues.end() && it->second == value)
                return;

            lastValues[key] = value;
            Logger::debug("reshade ui uniform write: effect=" + effectName + " uniform=" + uniformName + " value=" + value);
        }

        void writeDefaultUniformValue(void* mappedBuffer, const reshadefx::uniform_info& uniform)
        {
            if (!mappedBuffer || uniform.offset == 0xFFFFFFFFu || !uniform.has_initializer_value)
                return;

            const uint32_t componentCount = std::min<uint32_t>(uniform.type.components(), 16u);
            if (componentCount == 0)
                return;

            uint8_t* dst = static_cast<uint8_t*>(mappedBuffer) + uniform.offset;
            switch (uniform.type.base)
            {
                case reshadefx::type::t_float:
                    std::memcpy(dst, uniform.initializer_value.as_float, componentCount * sizeof(float));
                    break;
                case reshadefx::type::t_bool:
                {
                    VkBool32 values[16] = {};
                    for (uint32_t i = 0; i < componentCount; ++i)
                        values[i] = uniform.initializer_value.as_uint[i] != 0 ? VK_TRUE : VK_FALSE;
                    std::memcpy(dst, values, componentCount * sizeof(VkBool32));
                    break;
                }
                case reshadefx::type::t_int:
                    std::memcpy(dst, uniform.initializer_value.as_int, componentCount * sizeof(int32_t));
                    break;
                case reshadefx::type::t_uint:
                    std::memcpy(dst, uniform.initializer_value.as_uint, componentCount * sizeof(uint32_t));
                    break;
                default:
                    break;
            }
        }

        void writeConfiguredUniformValue(void* mappedBuffer, const reshadefx::uniform_info& uniform, const EffectParam* param)
        {
            if (!mappedBuffer || uniform.offset == 0xFFFFFFFFu || param == nullptr)
                return;

            uint8_t* dst = static_cast<uint8_t*>(mappedBuffer) + uniform.offset;

            if (const auto* p = dynamic_cast<const FloatParam*>(param))
            {
                std::memcpy(dst, &p->value, sizeof(float));
                return;
            }
            if (const auto* p = dynamic_cast<const FloatVecParam*>(param))
            {
                const uint32_t count = std::min<uint32_t>(p->componentCount, std::min<uint32_t>(uniform.type.components(), 4u));
                std::memcpy(dst, p->value, count * sizeof(float));
                return;
            }
            if (const auto* p = dynamic_cast<const IntParam*>(param))
            {
                const int32_t value = p->value;
                std::memcpy(dst, &value, sizeof(int32_t));
                return;
            }
            if (const auto* p = dynamic_cast<const IntVecParam*>(param))
            {
                const uint32_t count = std::min<uint32_t>(p->componentCount, std::min<uint32_t>(uniform.type.components(), 4u));
                std::memcpy(dst, p->value, count * sizeof(int32_t));
                return;
            }
            if (const auto* p = dynamic_cast<const UintParam*>(param))
            {
                const uint32_t value = p->value;
                std::memcpy(dst, &value, sizeof(uint32_t));
                return;
            }
            if (const auto* p = dynamic_cast<const UintVecParam*>(param))
            {
                const uint32_t count = std::min<uint32_t>(p->componentCount, std::min<uint32_t>(uniform.type.components(), 4u));
                std::memcpy(dst, p->value, count * sizeof(uint32_t));
                return;
            }
            if (const auto* p = dynamic_cast<const BoolParam*>(param))
            {
                const VkBool32 value = p->value ? VK_TRUE : VK_FALSE;
                std::memcpy(dst, &value, sizeof(VkBool32));
                return;
            }
        }
    }

    ReshadeEffect::ReshadeEffect(LogicalDevice*       pLogicalDevice,
                                 VkFormat             format,
                                 VkExtent2D           imageExtent,
                                 std::vector<VkImage> inputImages,
                                 std::vector<VkImage> outputImages,
                                 EffectRegistry*      pEffectRegistry,
                                 std::string          effectName,
                                 std::string          effectPath,
                                 std::vector<PreprocessorDefinition> customDefs)
    {
        Logger::debug("in creating ReshadeEffect");

        this->pLogicalDevice        = pLogicalDevice;
        this->imageExtent           = imageExtent;
        this->inputImages           = inputImages;
        this->outputImages          = outputImages;
        outputImageInitialized.assign(outputImages.size(), false);
        this->pEffectRegistry       = pEffectRegistry;
        this->effectName            = effectName;
        this->effectPath            = effectPath;
        this->customPreprocessorDefs = customDefs;
        inputOutputFormatUNORM = convertToUNORM(format);
        inputOutputFormatSRGB  = convertToSRGB(format);

        inputImageViewsSRGB  = createImageViews(pLogicalDevice, inputOutputFormatSRGB, inputImages);
        inputImageViewsUNORM = createImageViews(pLogicalDevice, inputOutputFormatUNORM, inputImages);
        Logger::debug("created input ImageViews");
        outputImageViewsSRGB  = createImageViews(pLogicalDevice, inputOutputFormatSRGB, outputImages);
        outputImageViewsUNORM = createImageViews(pLogicalDevice, inputOutputFormatUNORM, outputImages);
        Logger::debug("created ImageViews");

        createReshadeModule();

        enumerateReshadeUniforms(module);

        uniforms = createReshadeUniforms(module);

        bufferSize = module.total_uniform_size;
        if (bufferSize)
        {
            createBuffer(pLogicalDevice,
                         bufferSize,
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         stagingBuffer,
                         stagingBufferMemory);
            // Persistent map — HOST_COHERENT means no flush needed, just write directly
            VkResult mapResult = pLogicalDevice->vkd.MapMemory(pLogicalDevice->device, stagingBufferMemory, 0, bufferSize, 0, &stagingBufferMapped);
            if (mapResult != VK_SUCCESS)
            {
                Logger::err("MapMemory failed for effect " + effectName + ": " + std::to_string(mapResult));
                stagingBufferMapped = nullptr;
            }
        }

        stencilFormat = getStencilFormat(pLogicalDevice);
        Logger::debug("Stencil Format: " + std::to_string(stencilFormat));
        textureMemory.push_back(VK_NULL_HANDLE);
        stencilImage = createImages(pLogicalDevice,
                                    1,
                                    {imageExtent.width, imageExtent.height, 1},
                                    stencilFormat,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                    textureMemory.back())[0];

        stencilImageView = createImageViews(
            pLogicalDevice, stencilFormat, {stencilImage}, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)[0];

        std::vector<std::vector<VkImageView>> imageViewVector;

        // Cache shader manager config once for all texture lookups (avoids re-reading per texture)
        ShaderManagerConfig cachedShaderMgrConfig = ConfigSerializer::loadShaderManagerConfig();

        for (size_t i = 0; i < module.textures.size(); i++)
        {
            textureMipLevels[module.textures[i].unique_name] = module.textures[i].levels;
            const bool textureIs3D = module.textures[i].dimensions == 3;
            const VkImageViewType textureViewType = textureIs3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
            const uint32_t textureDepth = textureIs3D ? std::max(module.textures[i].depth, 1u) : 1u;
            textureExtents[module.textures[i].unique_name] = {module.textures[i].width, module.textures[i].height, textureDepth};
            if (module.textures[i].semantic == "COLOR")
            {
                textureImageViewsUNORM[module.textures[i].unique_name] = inputImageViewsUNORM;
                renderImageViewsUNORM[module.textures[i].unique_name]  = inputImageViewsUNORM;

                textureImageViewsSRGB[module.textures[i].unique_name] = inputImageViewsSRGB;
                renderImageViewsSRGB[module.textures[i].unique_name]  = inputImageViewsSRGB;

                textureFormatsUNORM[module.textures[i].unique_name] = inputOutputFormatUNORM;
                textureFormatsSRGB[module.textures[i].unique_name]  = inputOutputFormatSRGB;
                continue;
            }
            if (module.textures[i].semantic == "DEPTH")
            {
                textureImageViewsUNORM[module.textures[i].unique_name] = inputImageViewsUNORM;
                renderImageViewsUNORM[module.textures[i].unique_name]  = inputImageViewsUNORM;

                textureImageViewsSRGB[module.textures[i].unique_name] = inputImageViewsSRGB;
                renderImageViewsSRGB[module.textures[i].unique_name]  = inputImageViewsSRGB;

                textureFormatsUNORM[module.textures[i].unique_name] = inputOutputFormatUNORM;
                textureFormatsSRGB[module.textures[i].unique_name]  = inputOutputFormatSRGB;
                continue;
            }
            VkExtent3D textureExtent = {module.textures[i].width, module.textures[i].height, textureDepth};
            // TODO handle mip map levels correctly
            // TODO handle pooled textures better
            if (const auto source = std::find_if(
                    module.textures[i].annotations.begin(), module.textures[i].annotations.end(), [](const auto& a) { return a.name == "source"; });
                source == module.textures[i].annotations.end())
            {
                VkImageUsageFlags imageUsage =
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                if (!textureIs3D)
                    imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

                textureMemory.push_back(VK_NULL_HANDLE);
                std::vector<VkImage> images = createImages(pLogicalDevice,
                                                           1,
                                                           textureExtent,
                                                           convertReshadeFormat(module.textures[i].format),
                                                           imageUsage,
                                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                           textureMemory.back(),
                                                           module.textures[i].levels);

                textureImages[module.textures[i].unique_name] = images;
                std::vector<VkImageView> imageViewsUNORM =
                    std::vector<VkImageView>(inputImages.size(),
                                             createImageViews(pLogicalDevice,
                                                              convertToUNORM(convertReshadeFormat(module.textures[i].format)),
                                                              images,
                                                              textureViewType,
                                                              VK_IMAGE_ASPECT_COLOR_BIT,
                                                              module.textures[i].levels)[0]);

                std::vector<VkImageView> imageViewsSRGB =
                    std::vector<VkImageView>(inputImages.size(),
                                             createImageViews(pLogicalDevice,
                                                              convertToSRGB(convertReshadeFormat(module.textures[i].format)),
                                                              images,
                                                              textureViewType,
                                                              VK_IMAGE_ASPECT_COLOR_BIT,
                                                              module.textures[i].levels)[0]);

                textureImageViewsUNORM[module.textures[i].unique_name] = imageViewsUNORM;
                textureImageViewsSRGB[module.textures[i].unique_name]  = imageViewsSRGB;

                if (module.textures[i].levels > 1)
                {

                    renderImageViewsUNORM[module.textures[i].unique_name] = std::vector<VkImageView>(
                        inputImages.size(),
                        createImageViews(pLogicalDevice,
                                         convertToUNORM(convertReshadeFormat(module.textures[i].format)),
                                         images,
                                         textureViewType,
                                         VK_IMAGE_ASPECT_COLOR_BIT,
                                         module.textures[i].levels)[0]);

                    renderImageViewsSRGB[module.textures[i].unique_name] = std::vector<VkImageView>(
                        inputImages.size(),
                        createImageViews(pLogicalDevice,
                                         convertToSRGB(convertReshadeFormat(module.textures[i].format)),
                                         images,
                                         textureViewType,
                                         VK_IMAGE_ASPECT_COLOR_BIT,
                                         module.textures[i].levels)[0]);
                }
                else
                {
                    renderImageViewsUNORM[module.textures[i].unique_name] = imageViewsUNORM;
                    renderImageViewsSRGB[module.textures[i].unique_name]  = imageViewsSRGB;
                }

                textureFormatsUNORM[module.textures[i].unique_name] = convertToUNORM(convertReshadeFormat(module.textures[i].format));
                textureFormatsSRGB[module.textures[i].unique_name]  = convertToSRGB(convertReshadeFormat(module.textures[i].format));
                changeImageLayout(pLogicalDevice, images, module.textures[i].levels);
                continue;
            }
            else
            {
                textureMemory.push_back(VK_NULL_HANDLE);
                std::vector<VkImage> images =
                    createImages(pLogicalDevice,
                                 1,
                                 textureExtent,
                                 convertReshadeFormat(module.textures[i].format), // TODO search for format and save it
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                 textureMemory.back(),
                                 module.textures[i].levels);

                textureImages[module.textures[i].unique_name] = images;

                std::vector<VkImageView> imageViews = createImageViews(pLogicalDevice,
                                                                       convertToUNORM(convertReshadeFormat(module.textures[i].format)),
                                                                       images,
                                                                       textureViewType,
                                                                       VK_IMAGE_ASPECT_COLOR_BIT,
                                                                       module.textures[i].levels);

                std::vector<VkImageView> imageViewsUNORM = std::vector<VkImageView>(inputImages.size(), imageViews[0]);

                imageViews = createImageViews(pLogicalDevice,
                                              convertToSRGB(convertReshadeFormat(module.textures[i].format)),
                                              images,
                                              textureViewType,
                                              VK_IMAGE_ASPECT_COLOR_BIT,
                                              module.textures[i].levels);

                std::vector<VkImageView> imageViewsSRGB = std::vector<VkImageView>(inputImages.size(), imageViews[0]);

                textureImageViewsUNORM[module.textures[i].unique_name] = imageViewsUNORM;
                textureImageViewsSRGB[module.textures[i].unique_name]  = imageViewsSRGB;

                renderImageViewsUNORM[module.textures[i].unique_name] = imageViewsUNORM;
                renderImageViewsSRGB[module.textures[i].unique_name]  = imageViewsSRGB;

                textureFormatsUNORM[module.textures[i].unique_name] = convertToUNORM(convertReshadeFormat(module.textures[i].format));
                textureFormatsSRGB[module.textures[i].unique_name]  = convertToSRGB(convertReshadeFormat(module.textures[i].format));

                int desiredChannels;
                switch (textureFormatsUNORM[module.textures[i].unique_name])
                {
                    case VK_FORMAT_R8_UNORM: desiredChannels = STBI_grey; break;
                    case VK_FORMAT_R8G8_UNORM:
                        desiredChannels = STBI_rgb_alpha; // TODO why doesn't STBI_grey_alpha work?
                        break;
                    case VK_FORMAT_R8G8B8A8_UNORM: desiredChannels = STBI_rgb_alpha; break;
                    case VK_FORMAT_R8G8B8A8_SRGB: desiredChannels = STBI_rgb_alpha; break;
                    default:
                        Logger::err("unsupported texture upload format" + std::to_string(textureFormatsUNORM[module.textures[i].unique_name]));
                        desiredChannels = 4;
                        break;
                }

                // Search for texture in discovered paths from shader manager
                std::string textureName = source->value.string_data;
                std::string filePath;
                FILE* file = nullptr;

                for (const auto& texPath : cachedShaderMgrConfig.discoveredTexturePaths)
                {
                    filePath = texPath + "/" + textureName;
                    file = fopen(filePath.c_str(), "rb");
                    if (file != nullptr)
                        break;
                }

                // Fallback: recursive search by basename under discovered texture paths.
                // iMMERSE packages textures in subdirectories (e.g. Textures/iMMERSE),
                // while shader annotations often reference only the filename.
                if (file == nullptr)
                {
                    std::filesystem::path texturePath(textureName);
                    const std::string textureBaseName = texturePath.filename().string();

                    for (const auto& texPath : cachedShaderMgrConfig.discoveredTexturePaths)
                    {
                        std::error_code ec;
                        std::filesystem::recursive_directory_iterator it(
                            texPath, std::filesystem::directory_options::skip_permission_denied, ec);
                        std::filesystem::recursive_directory_iterator end;
                        if (ec)
                            continue;

                        for (; it != end; it.increment(ec))
                        {
                            if (ec)
                                break;
                            if (!it->is_regular_file(ec))
                                continue;
                            if (ec)
                                continue;

                            if (it->path().filename() == textureBaseName)
                            {
                                filePath = it->path().string();
                                file = fopen(filePath.c_str(), "rb");
                                if (file != nullptr)
                                    break;
                            }
                        }

                        if (file != nullptr)
                            break;
                    }
                }

                stbi_uc*             pixels;
                std::vector<stbi_uc> resizedPixels;
                uint32_t             size;
                int                  width;
                int                  height;

                size = textureExtent.width * textureExtent.height * textureExtent.depth * desiredChannels;

                if (file == nullptr)
                {
                    Logger::err("couldn't open texture: " + textureName + " (searched " +
                        std::to_string(cachedShaderMgrConfig.discoveredTexturePaths.size()) + " directories, including recursive fallback)");
                    continue;
                }

                if (stbi_dds_test_file(file))
                {
                    int channels;
                    pixels = stbi_dds_load_from_file(file, &width, &height, &channels, desiredChannels);
                }
                else
                {
                    int channels;
                    pixels = stbi_load_from_file(file, &width, &height, &channels, desiredChannels);
                }
                fclose(file);

                if (pixels == nullptr)
                {
                    Logger::err("failed to decode texture: " + textureName + " from " + filePath);
                    continue;
                }

                // change RGBA to RG
                if (textureFormatsUNORM[module.textures[i].unique_name] == VK_FORMAT_R8G8_UNORM)
                {
                    uint32_t pos = 0;
                    for (uint32_t j = 0; j < size; j += 4)
                    {
                        pixels[pos] = pixels[j];
                        pos++;
                        pixels[pos] = pixels[j + 1];
                        pos++;
                    }
                    size /= 2;
                    desiredChannels /= 2;
                }

                if (static_cast<uint32_t>(width) != textureExtent.width || static_cast<uint32_t>(height) != textureExtent.height)
                {
                    resizedPixels.resize(size);
                    stbir_resize_uint8(pixels, width, height, 0, resizedPixels.data(), textureExtent.width, textureExtent.height, 0, desiredChannels);
                }

                uploadToImage(
                    pLogicalDevice, images[0], textureExtent, size, resizedPixels.size() ? resizedPixels.data() : pixels, module.textures[i].levels);
                stbi_image_free(pixels);
            }
        }

        std::set<std::string> depthTextureNames;
        for (const auto& texture : module.textures)
        {
            if (texture.semantic == "DEPTH")
                depthTextureNames.insert(texture.unique_name);
        }

        std::vector<reshadefx::texture_filter> samplerFilters;
        samplerFilters.reserve(module.samplers.size());
        for (size_t i = 0; i < module.samplers.size(); i++)
        {
            reshadefx::sampler_info info = module.samplers[i];
            if (depthTextureNames.find(info.texture_name) != depthTextureNames.end())
                info.filter = reshadefx::texture_filter::min_mag_mip_point;
            samplerFilters.push_back(info.filter);

            VkSampler sampler = createReshadeSampler(pLogicalDevice, info);

            samplers.push_back(sampler);

            imageViewVector.push_back(info.srgb ? textureImageViewsSRGB[info.texture_name] : textureImageViewsUNORM[info.texture_name]);
        }

        std::vector<VkDescriptorType> samplerBindingTypes(module.samplers.size(), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        size_t sampledBindingCount = 0;
        size_t storageBindingCount = 0;
        for (size_t i = 0; i < module.samplers.size(); ++i)
        {
            if (module.samplers[i].storage)
            {
                samplerBindingTypes[i] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                ++storageBindingCount;
            }
            else
            {
                ++sampledBindingCount;
            }
            Logger::debug(
                "sampler binding " + std::to_string(i) + " name=" + module.samplers[i].unique_name +
                " texture=" + module.samplers[i].texture_name +
                " storage=" + (module.samplers[i].storage ? "1" : "0") +
                " filter=" + std::to_string(static_cast<uint32_t>(samplerFilters[i])));
        }

        imageSamplerDescriptorSetLayout = createImageSamplerDescriptorSetLayout(pLogicalDevice, samplerBindingTypes);
        uniformDescriptorSetLayout      = createUniformBufferDescriptorSetLayout(pLogicalDevice);
        Logger::debug("created descriptorSetLayouts");

        VkDescriptorPoolSize sampledImagePoolSize;
        sampledImagePoolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampledImagePoolSize.descriptorCount = inputImages.size() * sampledBindingCount * 3;

        VkDescriptorPoolSize storageImagePoolSize;
        storageImagePoolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        storageImagePoolSize.descriptorCount = inputImages.size() * storageBindingCount * 3;

        VkDescriptorPoolSize bufferPoolSize;
        bufferPoolSize.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bufferPoolSize.descriptorCount = 3;

        std::vector<VkDescriptorPoolSize> poolSizes = {bufferPoolSize};
        if (sampledImagePoolSize.descriptorCount > 0)
            poolSizes.push_back(sampledImagePoolSize);
        if (storageImagePoolSize.descriptorCount > 0)
            poolSizes.push_back(storageImagePoolSize);

        descriptorPool = createDescriptorPool(pLogicalDevice, poolSizes);
        Logger::debug("created descriptorPool");

        std::vector<VkDescriptorSetLayout> descriptorSetLayouts = {uniformDescriptorSetLayout, imageSamplerDescriptorSetLayout};

        pipelineLayout = createGraphicsPipelineLayout(pLogicalDevice, descriptorSetLayouts);

        Logger::debug("created Pipeline layout");

        // count the back buffer writes
        for (const auto& pass : module.techniques[0].passes)
        {
            if (pass.cs_entry_point.empty() && pass.render_target_names[0].empty())
            {
                outputWrites++;
            }
        }

        Logger::debug("output writes: " + std::to_string(outputWrites));
        if (bufferSize)
        {
            bufferDescriptorSet = writeBufferDescriptorSet(pLogicalDevice, descriptorPool, uniformDescriptorSetLayout, stagingBuffer);
        }

        inputDescriptorSets =
            allocateAndWriteImageSamplerDescriptorSets(
                pLogicalDevice, descriptorPool, imageSamplerDescriptorSetLayout, samplers, imageViewVector, samplerBindingTypes);

        // if there is only one outputWrite, we can directly write to outputImages
        if (outputWrites > 1)
        {
            textureMemory.push_back(VK_NULL_HANDLE);
            backBufferImages = createImages(pLogicalDevice,
                                            inputImages.size(),
                                            {imageExtent.width, imageExtent.height, 1},
                                            format, // TODO search for format and save it
                                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                            textureMemory.back());
            changeImageLayout(pLogicalDevice, backBufferImages, 1);

            backBufferImageViewsSRGB  = createImageViews(pLogicalDevice, inputOutputFormatSRGB, backBufferImages);
            backBufferImageViewsUNORM = createImageViews(pLogicalDevice, inputOutputFormatUNORM, backBufferImages);

            std::replace(imageViewVector.begin(), imageViewVector.end(), inputImageViewsSRGB, backBufferImageViewsSRGB);
            std::replace(imageViewVector.begin(), imageViewVector.end(), inputImageViewsUNORM, backBufferImageViewsUNORM);

            backBufferDescriptorSets = allocateAndWriteImageSamplerDescriptorSets(
                pLogicalDevice, descriptorPool, imageSamplerDescriptorSetLayout, samplers, imageViewVector, samplerBindingTypes);
        }
        if (outputWrites > 2)
        {
            std::replace(imageViewVector.begin(), imageViewVector.end(), backBufferImageViewsSRGB, outputImageViewsSRGB);
            std::replace(imageViewVector.begin(), imageViewVector.end(), backBufferImageViewsUNORM, outputImageViewsUNORM);
            outputDescriptorSets = allocateAndWriteImageSamplerDescriptorSets(
                pLogicalDevice, descriptorPool, imageSamplerDescriptorSetLayout, samplers, imageViewVector, samplerBindingTypes);
        }

        Logger::debug("after writing ImageSamplerDescriptorSets");

        bool firstTimeStencilAccess = true; // Used to clear the sttencil attachment on the first time

        auto buildSpecializationData = [&]() {
            std::vector<VkSpecializationMapEntry> specMapEntrys;
            std::vector<char>                     specData;
            const auto appendSpecValue = [&](uint32_t specId, const auto &value) {
                const uint32_t offset = static_cast<uint32_t>(specData.size());
                using ValueType = std::decay_t<decltype(value)>;
                specData.resize(offset + sizeof(ValueType));
                std::memcpy(specData.data() + offset, &value, sizeof(ValueType));
                specMapEntrys.push_back({specId, offset, sizeof(ValueType)});
            };

            // Track vector component index (for float2/float3/float4 which are split into multiple spec constants)
            std::string prevSpecName;
            int vectorComponentIndex = 0;

            for (uint32_t specId = 0; auto &opt : module.spec_constants)
            {
                if (!opt.name.empty())
                {
                    // Track which component of a vector this is (consecutive same-named spec constants = vector components)
                    if (opt.name == prevSpecName)
                    {
                        vectorComponentIndex++;
                    }
                    else
                    {
                        vectorComponentIndex = 0;
                        prevSpecName = opt.name;
                    }

                    // Get parameter from EffectRegistry (the single source of truth)
                    EffectParam* param = pEffectRegistry->getParameter(effectName, opt.name);
                    if (!param)
                    {
                        specId++;
                        continue;
                    }

                    switch (opt.type.base)
                    {
                        case reshadefx::type::t_bool:
                            if (auto* bp = dynamic_cast<BoolParam*>(param))
                            {
                                const VkBool32 value = bp->value ? VK_TRUE : VK_FALSE;
                                appendSpecValue(specId, value);
                            }
                            break;
                        case reshadefx::type::t_int:
                            // Could be IntParam or IntVecParam (vector component)
                            if (auto* ivp = dynamic_cast<IntVecParam*>(param))
                            {
                                if (vectorComponentIndex < static_cast<int>(ivp->componentCount))
                                {
                                    const int32_t value = ivp->value[vectorComponentIndex];
                                    appendSpecValue(specId, value);
                                }
                            }
                            else if (auto* ip = dynamic_cast<IntParam*>(param))
                            {
                                const int32_t value = ip->value;
                                appendSpecValue(specId, value);
                            }
                            break;
                        case reshadefx::type::t_uint:
                            // Could be UintParam or UintVecParam (vector component)
                            if (auto* uvp = dynamic_cast<UintVecParam*>(param))
                            {
                                if (vectorComponentIndex < static_cast<int>(uvp->componentCount))
                                {
                                    const uint32_t value = uvp->value[vectorComponentIndex];
                                    appendSpecValue(specId, value);
                                }
                            }
                            else if (auto* up = dynamic_cast<UintParam*>(param))
                            {
                                const uint32_t value = up->value;
                                appendSpecValue(specId, value);
                            }
                            else if (auto* ip = dynamic_cast<IntParam*>(param))
                            {
                                const uint32_t value = static_cast<uint32_t>(ip->value);
                                appendSpecValue(specId, value);
                            }
                            break;
                        case reshadefx::type::t_float:
                            // Could be FloatParam or FloatVecParam (vector component)
                            if (auto* fvp = dynamic_cast<FloatVecParam*>(param))
                            {
                                if (vectorComponentIndex < static_cast<int>(fvp->componentCount))
                                {
                                    const float value = fvp->value[vectorComponentIndex];
                                    appendSpecValue(specId, value);
                                }
                            }
                            else if (auto* fp = dynamic_cast<FloatParam*>(param))
                            {
                                const float value = fp->value;
                                appendSpecValue(specId, value);
                            }
                            break;
                        default:
                            break;
                    }
                }
                specId++;
            }

            return std::make_pair(std::move(specMapEntrys), std::move(specData));
        };

        for (bool outputToBackBuffer = outputWrites % 2 == 0; auto& pass : module.techniques[0].passes)
        {
            const bool isComputePass = !pass.cs_entry_point.empty();
            if (!isComputePass && (pass.vs_entry_point.empty() || pass.ps_entry_point.empty()))
            {
                std::string error = "unsupported pass type in effect '" + effectName +
                                    "': graphics pass requires vertex and fragment entry points";
                Logger::err(error);
                throw std::runtime_error(error);
            }

            auto [specMapEntrys, specData] = buildSpecializationData();
            VkSpecializationInfo specializationInfo = {};
            if (!specMapEntrys.empty())
            {
                specializationInfo = {.mapEntryCount = static_cast<uint32_t>(specMapEntrys.size()),
                                      .pMapEntries   = specMapEntrys.data(),
                                      .dataSize      = specData.size(),
                                      .pData         = specData.data()};
            }

            if (isComputePass)
            {
                PassRuntime runtime = {};
                runtime.isCompute = true;
                runtime.dispatchSizeX = std::max(pass.dispatch_size_x, 1u);
                runtime.dispatchSizeY = std::max(pass.dispatch_size_y, 1u);
                runtime.dispatchSizeZ = std::max(pass.dispatch_size_z, 1u);

                VkPipelineShaderStageCreateInfo shaderStageCreateInfoCompute = {};
                shaderStageCreateInfoCompute.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                shaderStageCreateInfoCompute.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
                shaderStageCreateInfoCompute.module = shaderModule;
                shaderStageCreateInfoCompute.pName  = pass.cs_entry_point.c_str();
                shaderStageCreateInfoCompute.pSpecializationInfo = specMapEntrys.empty() ? nullptr : &specializationInfo;

                VkComputePipelineCreateInfo computePipelineCreateInfo = {};
                computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                computePipelineCreateInfo.stage = shaderStageCreateInfoCompute;
                computePipelineCreateInfo.layout = pipelineLayout;
                if (disableComputePipelineOptimization)
                    computePipelineCreateInfo.flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;

                Logger::debug("creating compute pipeline entry: " + pass.cs_entry_point);
                const VkResult computeResult = pLogicalDevice->vkd.CreateComputePipelines(
                    pLogicalDevice->device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &runtime.pipeline);
                if (computeResult != VK_SUCCESS)
                {
                    std::string error = "CreateComputePipelines failed for effect '" + effectName +
                                        "', pass CS='" + pass.cs_entry_point +
                                        "', VkResult=" + std::to_string(computeResult);
                    Logger::err(error);
                    throw std::runtime_error(error);
                }

                Logger::debug("compute  entry: " + pass.cs_entry_point +
                              " dispatch(" + std::to_string(runtime.dispatchSizeX) + ", " +
                              std::to_string(runtime.dispatchSizeY) + ", " +
                              std::to_string(runtime.dispatchSizeZ) + ")");
                passRuntimes.push_back(std::move(runtime));
                continue;
            }

            std::vector<VkAttachmentReference>               attachmentReferences;
            std::vector<VkAttachmentDescription>             attachmentDescriptions;
            std::vector<VkPipelineColorBlendAttachmentState> attachmentBlendStates;
            std::vector<std::vector<VkImageView>>            attachmentImageViews;
            std::vector<std::string>                         currentRenderTargets;

            for (int i = 0; i < 8; i++)
            {
                std::string target = pass.render_target_names[i];
                Logger::debug("render target:" + target);

                VkAttachmentDescription attachmentDescription;
                attachmentDescription.flags   = 0;
                attachmentDescription.format  = pass.srgb_write_enable ? textureFormatsSRGB[target] : textureFormatsUNORM[target];
                attachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT;
                attachmentDescription.loadOp  = pass.clear_render_targets ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                                          : VK_ATTACHMENT_LOAD_OP_LOAD;

                attachmentDescription.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
                attachmentDescription.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                attachmentDescription.initialLayout  = VK_IMAGE_LAYOUT_GENERAL;
                attachmentDescription.finalLayout    = VK_IMAGE_LAYOUT_GENERAL;

                if (target.empty() && i == 0)
                {
                    attachmentDescription.format        = pass.srgb_write_enable ? inputOutputFormatSRGB : inputOutputFormatUNORM;
                    attachmentDescription.loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD;
                    attachmentDescription.storeOp       = VK_ATTACHMENT_STORE_OP_STORE;
                    attachmentDescription.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
                    attachmentDescription.finalLayout   = VK_IMAGE_LAYOUT_GENERAL;
                }
                else if (target.empty())
                {
                    break;
                }

                attachmentDescriptions.push_back(attachmentDescription);

                VkAttachmentReference attachmentReference;
                attachmentReference.attachment = i;
                attachmentReference.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

                attachmentReferences.push_back(attachmentReference);

                VkPipelineColorBlendAttachmentState colorBlendAttachment;
                colorBlendAttachment.blendEnable         = pass.blend_enable;
                colorBlendAttachment.srcColorBlendFactor = convertReshadeBlendFactor(pass.src_blend);
                colorBlendAttachment.dstColorBlendFactor = convertReshadeBlendFactor(pass.dest_blend);
                colorBlendAttachment.colorBlendOp        = convertReshadeBlendOp(pass.blend_op);
                colorBlendAttachment.srcAlphaBlendFactor = convertReshadeBlendFactor(pass.src_blend_alpha);
                colorBlendAttachment.dstAlphaBlendFactor = convertReshadeBlendFactor(pass.dest_blend_alpha);
                colorBlendAttachment.alphaBlendOp        = convertReshadeBlendOp(pass.blend_op_alpha);
                colorBlendAttachment.colorWriteMask      = pass.color_write_mask;

                attachmentBlendStates.push_back(colorBlendAttachment);

                attachmentImageViews.push_back(pass.srgb_write_enable ? renderImageViewsSRGB[target] : renderImageViewsUNORM[target]);
                if (!target.empty())
                {
                    currentRenderTargets.push_back(target);
                }
            }

            VkRect2D scissor;
            scissor.offset        = {0, 0};
            scissor.extent.width  = pass.viewport_width ? pass.viewport_width : imageExtent.width;
            scissor.extent.height = pass.viewport_height ? pass.viewport_height : imageExtent.height;

            Logger::debug(std::to_string(scissor.extent.width) + " x " + std::to_string(scissor.extent.height));

            VkViewport viewport;
            viewport.x        = 0.0f;
            viewport.y        = 0.0f;
            viewport.width    = static_cast<float>(scissor.extent.width);
            viewport.height   = static_cast<float>(scissor.extent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;

            uint32_t depthAttachmentCount = 0;

            if (scissor.extent.width == imageExtent.width && scissor.extent.height == imageExtent.height)
            {
                depthAttachmentCount = 1;

                attachmentImageViews.push_back(std::vector<VkImageView>(inputImages.size(), stencilImageView));

                VkAttachmentReference attachmentReference;
                attachmentReference.attachment = attachmentReferences.size();
                attachmentReference.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

                attachmentReferences.push_back(attachmentReference);

                VkAttachmentDescription attachmentDescription;
                attachmentDescription.flags          = 0;
                attachmentDescription.format         = stencilFormat;
                attachmentDescription.samples        = VK_SAMPLE_COUNT_1_BIT;
                attachmentDescription.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachmentDescription.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                attachmentDescription.stencilLoadOp  = firstTimeStencilAccess ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
                attachmentDescription.initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                attachmentDescription.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

                firstTimeStencilAccess = false;

                attachmentDescriptions.push_back(attachmentDescription);
            }

            VkSubpassDescription subpassDescription;
            subpassDescription.flags                   = 0;
            subpassDescription.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpassDescription.inputAttachmentCount    = 0;
            subpassDescription.pInputAttachments       = nullptr;
            subpassDescription.colorAttachmentCount    = attachmentReferences.size() - depthAttachmentCount;
            subpassDescription.pColorAttachments       = attachmentReferences.data();
            subpassDescription.pResolveAttachments     = nullptr;
            subpassDescription.pDepthStencilAttachment = depthAttachmentCount ? &attachmentReferences.back() : nullptr;
            subpassDescription.preserveAttachmentCount = 0;
            subpassDescription.pPreserveAttachments    = nullptr;

            VkSubpassDependency subpassDependencies[2] = {};
            subpassDependencies[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
            subpassDependencies[0].dstSubpass      = 0;
            subpassDependencies[0].srcStageMask    = VK_PIPELINE_STAGE_TRANSFER_BIT
                                                  | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                                  | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                                  | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            subpassDependencies[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            subpassDependencies[0].srcAccessMask   = VK_ACCESS_TRANSFER_WRITE_BIT
                                                  | VK_ACCESS_SHADER_READ_BIT
                                                  | VK_ACCESS_SHADER_WRITE_BIT
                                                  | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            subpassDependencies[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            subpassDependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

            subpassDependencies[1].srcSubpass      = 0;
            subpassDependencies[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
            subpassDependencies[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            subpassDependencies[1].dstStageMask    = VK_PIPELINE_STAGE_TRANSFER_BIT
                                                  | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                                  | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                                  | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            subpassDependencies[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            subpassDependencies[1].dstAccessMask   = VK_ACCESS_TRANSFER_READ_BIT
                                                  | VK_ACCESS_TRANSFER_WRITE_BIT
                                                  | VK_ACCESS_SHADER_READ_BIT
                                                  | VK_ACCESS_SHADER_WRITE_BIT
                                                  | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                                  | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            subpassDependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

            VkRenderPassCreateInfo renderPassCreateInfo;
            renderPassCreateInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            renderPassCreateInfo.pNext           = nullptr;
            renderPassCreateInfo.flags           = 0;
            renderPassCreateInfo.attachmentCount = attachmentDescriptions.size();
            renderPassCreateInfo.pAttachments    = attachmentDescriptions.data();
            renderPassCreateInfo.subpassCount    = 1;
            renderPassCreateInfo.pSubpasses      = &subpassDescription;
            renderPassCreateInfo.dependencyCount = 2;
            renderPassCreateInfo.pDependencies   = subpassDependencies;

            PassRuntime runtime = {};
            runtime.renderTargets = std::move(currentRenderTargets);
            runtime.vertexCount = pass.num_vertices == 0 ? 3 : pass.num_vertices;

            VkResult result = pLogicalDevice->vkd.CreateRenderPass(pLogicalDevice->device, &renderPassCreateInfo, nullptr, &runtime.renderPass);
            ASSERT_VULKAN(result);

            runtime.renderArea = scissor;
            runtime.clearValueCount = std::min<uint32_t>(attachmentDescriptions.size(), runtime.clearValues.size());

            if (pass.render_target_names[0].empty())
            {
                std::vector<VkImageView> backBufferImageViews = pass.srgb_write_enable ? backBufferImageViewsSRGB : backBufferImageViewsUNORM;
                std::vector<VkImageView> finalOutputImageViews = pass.srgb_write_enable ? outputImageViewsSRGB : outputImageViewsUNORM;
                runtime.framebuffers = createFramebuffers(
                    pLogicalDevice,
                    runtime.renderPass,
                    imageExtent,
                    {outputToBackBuffer ? backBufferImageViews : finalOutputImageViews, std::vector<VkImageView>(inputImages.size(), stencilImageView)});
                outputToBackBuffer = !outputToBackBuffer;
                runtime.switchSamplers = true;
            }
            else
            {
                runtime.framebuffers = createFramebuffers(pLogicalDevice, runtime.renderPass, scissor.extent, attachmentImageViews);
                runtime.switchSamplers = false;
            }

            VkPipelineShaderStageCreateInfo shaderStageCreateInfoVert = {};
            shaderStageCreateInfoVert.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shaderStageCreateInfoVert.stage               = VK_SHADER_STAGE_VERTEX_BIT;
            shaderStageCreateInfoVert.module              = shaderModule;
            shaderStageCreateInfoVert.pName               = pass.vs_entry_point.c_str();
            shaderStageCreateInfoVert.pSpecializationInfo = specMapEntrys.empty() ? nullptr : &specializationInfo;

            VkPipelineShaderStageCreateInfo shaderStageCreateInfoFrag = {};
            shaderStageCreateInfoFrag.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shaderStageCreateInfoFrag.stage               = VK_SHADER_STAGE_FRAGMENT_BIT;
            shaderStageCreateInfoFrag.module              = shaderModule;
            shaderStageCreateInfoFrag.pName               = pass.ps_entry_point.c_str();
            shaderStageCreateInfoFrag.pSpecializationInfo = specMapEntrys.empty() ? nullptr : &specializationInfo;

            VkPipelineShaderStageCreateInfo shaderStages[] = {shaderStageCreateInfoVert, shaderStageCreateInfoFrag};

            VkPipelineVertexInputStateCreateInfo vertexInputCreateInfo = {};
            vertexInputCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            switch (pass.topology)
            {
                case reshadefx::primitive_topology::point_list: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
                case reshadefx::primitive_topology::line_list: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
                case reshadefx::primitive_topology::line_strip: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
                case reshadefx::primitive_topology::triangle_list: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
                case reshadefx::primitive_topology::triangle_strip: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
                default: Logger::err("unsupported primitiv type" + convertToString((uint8_t) pass.topology)); break;
            }

            VkPipelineInputAssemblyStateCreateInfo inputAssemblyCreateInfo = {};
            inputAssemblyCreateInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssemblyCreateInfo.topology               = topology;
            inputAssemblyCreateInfo.primitiveRestartEnable = VK_FALSE;

            VkPipelineViewportStateCreateInfo viewportStateCreateInfo = {};
            viewportStateCreateInfo.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportStateCreateInfo.viewportCount = 1;
            viewportStateCreateInfo.pViewports    = &viewport;
            viewportStateCreateInfo.scissorCount  = 1;
            viewportStateCreateInfo.pScissors     = &scissor;

            VkPipelineRasterizationStateCreateInfo rasterizationCreateInfo = {};
            rasterizationCreateInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizationCreateInfo.depthClampEnable        = VK_FALSE;
            rasterizationCreateInfo.rasterizerDiscardEnable = VK_FALSE;
            rasterizationCreateInfo.polygonMode             = VK_POLYGON_MODE_FILL;
            rasterizationCreateInfo.cullMode                = VK_CULL_MODE_NONE;
            rasterizationCreateInfo.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterizationCreateInfo.lineWidth               = 1.0f;

            VkPipelineMultisampleStateCreateInfo multisampleCreateInfo = {};
            multisampleCreateInfo.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampleCreateInfo.rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT;
            multisampleCreateInfo.sampleShadingEnable   = VK_FALSE;
            multisampleCreateInfo.minSampleShading      = 1.0f;
            multisampleCreateInfo.alphaToCoverageEnable = VK_FALSE;
            multisampleCreateInfo.alphaToOneEnable      = VK_FALSE;

            VkPipelineColorBlendStateCreateInfo colorBlendCreateInfo = {};
            colorBlendCreateInfo.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlendCreateInfo.logicOpEnable   = VK_FALSE;
            colorBlendCreateInfo.logicOp         = VK_LOGIC_OP_NO_OP;
            colorBlendCreateInfo.attachmentCount = attachmentBlendStates.size();
            colorBlendCreateInfo.pAttachments    = attachmentBlendStates.data();

            VkPipelineDynamicStateCreateInfo dynamicStateCreateInfo = {};
            dynamicStateCreateInfo.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamicStateCreateInfo.dynamicStateCount = 0;
            dynamicStateCreateInfo.pDynamicStates    = nullptr;

            VkPipelineDepthStencilStateCreateInfo depthStencilStateCreateInfo = {};
            depthStencilStateCreateInfo.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencilStateCreateInfo.depthTestEnable       = VK_FALSE;
            depthStencilStateCreateInfo.depthWriteEnable      = VK_FALSE;
            depthStencilStateCreateInfo.depthCompareOp        = VK_COMPARE_OP_ALWAYS;
            depthStencilStateCreateInfo.depthBoundsTestEnable = VK_FALSE;
            depthStencilStateCreateInfo.stencilTestEnable     = pass.stencil_enable;
            depthStencilStateCreateInfo.front.failOp          = convertReshadeStencilOp(pass.stencil_op_fail);
            depthStencilStateCreateInfo.front.passOp          = convertReshadeStencilOp(pass.stencil_op_pass);
            depthStencilStateCreateInfo.front.depthFailOp     = convertReshadeStencilOp(pass.stencil_op_depth_fail);
            depthStencilStateCreateInfo.front.compareOp       = convertReshadeCompareOp(pass.stencil_comparison_func);
            depthStencilStateCreateInfo.front.compareMask     = pass.stencil_read_mask;
            depthStencilStateCreateInfo.front.writeMask       = pass.stencil_write_mask;
            depthStencilStateCreateInfo.front.reference       = pass.stencil_reference_value;
            depthStencilStateCreateInfo.back                  = depthStencilStateCreateInfo.front;
            depthStencilStateCreateInfo.minDepthBounds        = 0.0f;
            depthStencilStateCreateInfo.maxDepthBounds        = 1.0f;

            VkGraphicsPipelineCreateInfo pipelineCreateInfo = {};
            pipelineCreateInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineCreateInfo.stageCount          = 2;
            pipelineCreateInfo.pStages             = shaderStages;
            pipelineCreateInfo.pVertexInputState   = &vertexInputCreateInfo;
            pipelineCreateInfo.pInputAssemblyState = &inputAssemblyCreateInfo;
            pipelineCreateInfo.pViewportState      = &viewportStateCreateInfo;
            pipelineCreateInfo.pRasterizationState = &rasterizationCreateInfo;
            pipelineCreateInfo.pMultisampleState   = &multisampleCreateInfo;
            pipelineCreateInfo.pDepthStencilState  = &depthStencilStateCreateInfo;
            pipelineCreateInfo.pColorBlendState    = &colorBlendCreateInfo;
            pipelineCreateInfo.pDynamicState       = &dynamicStateCreateInfo;
            pipelineCreateInfo.layout              = pipelineLayout;
            pipelineCreateInfo.renderPass          = runtime.renderPass;
            pipelineCreateInfo.subpass             = 0;
            pipelineCreateInfo.basePipelineHandle  = VK_NULL_HANDLE;
            pipelineCreateInfo.basePipelineIndex   = -1;

            Logger::debug("creating graphics pipeline VS=" + pass.vs_entry_point + " PS=" + pass.ps_entry_point);
            result = pLogicalDevice->vkd.CreateGraphicsPipelines(
                pLogicalDevice->device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &runtime.pipeline);
            if (result != VK_SUCCESS)
            {
                std::string error = "CreateGraphicsPipelines failed for effect '" + effectName +
                                    "', pass VS='" + pass.vs_entry_point + "', PS='" + pass.ps_entry_point +
                                    "', VkResult=" + std::to_string(result);
                Logger::err(error);
                throw std::runtime_error(error);
            }

            Logger::debug("vertex   entry: " + pass.vs_entry_point);
            Logger::debug("fragment entry: " + pass.ps_entry_point);
            passRuntimes.push_back(std::move(runtime));
        }
        Logger::debug("finished creating Reshade effect");
    }

    void ReshadeEffect::updateEffect()
    {
        if (stagingBufferMapped)
        {
            for (const auto& uniform : module.uniforms)
            {
                if (hasSourceAnnotation(uniform))
                    continue;
                if (uniform.name.empty())
                    continue;

                if (EffectParam* registryParam = pEffectRegistry->getParameter(effectName, uniform.name))
                {
                    maybeLogReshadeUiUniformWrite(effectName, uniform.name, *registryParam);
                    writeConfiguredUniformValue(stagingBufferMapped, uniform, registryParam);
                }
                else
                    writeDefaultUniformValue(stagingBufferMapped, uniform);
            }

            for (auto& uniform : uniforms)
                uniform->update(stagingBufferMapped);
            // HOST_COHERENT: no flush needed, GPU sees writes automatically
        }
    }

    void ReshadeEffect::useDepthImage(uint32_t imageIndex, VkImageView depthImageView, VkImageLayout depthImageLayout)
    {
        if (imageIndex >= inputImages.size() || imageIndex >= inputDescriptorSets.size() || imageIndex >= inputImageViewsUNORM.size())
            return;

        // Update DepthUniforms so bufready_depth reports correctly
        bool hasDepth = (depthImageView != VK_NULL_HANDLE);
        for (auto& uniform : uniforms)
        {
            auto* depthUniform = dynamic_cast<DepthUniform*>(uniform.get());
            if (depthUniform)
                depthUniform->setDepthAvailable(hasDepth);
        }

        std::vector<std::string> depthTextureNames;

        for (auto& texture : module.textures)
        {
            if (texture.semantic == "DEPTH")
            {
                depthTextureNames.push_back(texture.unique_name);
            }
        }

        for (size_t i = 0; i < module.samplers.size(); i++)
        {
            reshadefx::sampler_info info = module.samplers[i];
            for (auto& name : depthTextureNames)
            {
                if (info.texture_name == name)
                {
                    VkDescriptorImageInfo imageInfo;
                    imageInfo.sampler = module.samplers[i].storage ? VK_NULL_HANDLE : samplers[i];
                    // Use a input image if there is no depth image to prevent a crash
                    imageInfo.imageView   = depthImageView ? depthImageView : inputImageViewsUNORM[imageIndex];
                    imageInfo.imageLayout = module.samplers[i].storage
                        ? VK_IMAGE_LAYOUT_GENERAL
                        : (depthImageView ? depthImageLayout : VK_IMAGE_LAYOUT_GENERAL);

                    Logger::debug("useDepthImage: effect=" + effectName
                                  + " imageIndex=" + std::to_string(imageIndex)
                                  + " binding=" + std::to_string(i)
                                  + " texture=" + name
                                  + " hasDepth=" + std::string(depthImageView ? "true" : "false")
                                  + " imageView=" + convertToString(imageInfo.imageView)
                                  + " imageLayout=" + convertToString(imageInfo.imageLayout));

                    VkWriteDescriptorSet writeDescriptorSet = {};

                    writeDescriptorSet.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writeDescriptorSet.pNext            = nullptr;
                    writeDescriptorSet.dstSet           = inputDescriptorSets[imageIndex];
                    writeDescriptorSet.dstBinding       = i;
                    writeDescriptorSet.dstArrayElement  = 0;
                    writeDescriptorSet.descriptorCount  = 1;
                    writeDescriptorSet.descriptorType =
                        module.samplers[i].storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writeDescriptorSet.pImageInfo       = &imageInfo;
                    writeDescriptorSet.pBufferInfo      = nullptr;
                    writeDescriptorSet.pTexelBufferView = nullptr;

                    pLogicalDevice->vkd.UpdateDescriptorSets(pLogicalDevice->device, 1, &writeDescriptorSet, 0, nullptr);
                    if (outputWrites > 1 && imageIndex < backBufferDescriptorSets.size())
                    {
                        writeDescriptorSet.dstSet = backBufferDescriptorSets[imageIndex];
                        pLogicalDevice->vkd.UpdateDescriptorSets(pLogicalDevice->device, 1, &writeDescriptorSet, 0, nullptr);
                    }
                    if (outputWrites > 2 && imageIndex < outputDescriptorSets.size())
                    {
                        writeDescriptorSet.dstSet = outputDescriptorSets[imageIndex];
                        pLogicalDevice->vkd.UpdateDescriptorSets(pLogicalDevice->device, 1, &writeDescriptorSet, 0, nullptr);
                    }
                    break;
                }
            }
        }
    }
    void ReshadeEffect::applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer)
    {
        const VkPipelineStageFlags shaderStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        const VkPipelineStageFlags transferStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

        VkImageCopy fullCopyRegion = {};
        fullCopyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        fullCopyRegion.srcSubresource.mipLevel = 0;
        fullCopyRegion.srcSubresource.baseArrayLayer = 0;
        fullCopyRegion.srcSubresource.layerCount = 1;
        fullCopyRegion.dstSubresource = fullCopyRegion.srcSubresource;
        fullCopyRegion.extent = {imageExtent.width, imageExtent.height, 1};

        // Transition input image for seed copy.
        VkImageMemoryBarrier memoryBarrier;
        memoryBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        memoryBarrier.pNext               = nullptr;
        memoryBarrier.srcAccessMask       = 0;
        memoryBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        memoryBarrier.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        memoryBarrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.image               = inputImages[imageIndex];

        memoryBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        memoryBarrier.subresourceRange.baseMipLevel   = 0;
        memoryBarrier.subresourceRange.levelCount     = 1;
        memoryBarrier.subresourceRange.baseArrayLayer = 0;
        memoryBarrier.subresourceRange.layerCount     = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, transferStage, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

        // Seed output image from current input so backbuffer/load-based passes never start from undefined data.
        memoryBarrier.image = outputImages[imageIndex];
        memoryBarrier.oldLayout = outputImageInitialized[imageIndex] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
        memoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        memoryBarrier.srcAccessMask = 0;
        memoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        pLogicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, transferStage, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

        pLogicalDevice->vkd.CmdCopyImage(commandBuffer,
                                         inputImages[imageIndex],
                                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         outputImages[imageIndex],
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         1,
                                         &fullCopyRegion);
        outputImageInitialized[imageIndex] = true;

        if (outputWrites > 1)
        {
            memoryBarrier.image = backBufferImages[imageIndex];
            memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            memoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            memoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            pLogicalDevice->vkd.CmdPipelineBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | shaderStages,
                transferStage,
                0,
                0,
                nullptr,
                0,
                nullptr,
                1,
                &memoryBarrier);

            pLogicalDevice->vkd.CmdCopyImage(commandBuffer,
                                             inputImages[imageIndex],
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             backBufferImages[imageIndex],
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             1,
                                             &fullCopyRegion);
        }

        // Used to make the image accessible by shaders after seed copy.
        memoryBarrier.image = inputImages[imageIndex];
        memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        memoryBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        // Reverses the first Barrier
        VkImageMemoryBarrier secondBarrier;
        secondBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        secondBarrier.pNext               = nullptr;
        secondBarrier.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        secondBarrier.dstAccessMask       = 0;
        secondBarrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        secondBarrier.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        secondBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        secondBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        secondBarrier.image               = inputImages[imageIndex];

        secondBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        secondBarrier.subresourceRange.baseMipLevel   = 0;
        secondBarrier.subresourceRange.levelCount     = 1;
        secondBarrier.subresourceRange.baseArrayLayer = 0;
        secondBarrier.subresourceRange.layerCount     = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, transferStage, shaderStages, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);
        memoryBarrier.image     = outputImages[imageIndex];
        memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        memoryBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        pLogicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, transferStage, shaderStages, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);
        if (outputWrites > 1)
        {
            memoryBarrier.image = backBufferImages[imageIndex];
            memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            memoryBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   transferStage,
                                                   shaderStages,
                                                   0,
                                                   0,
                                                   nullptr,
                                                   0,
                                                   nullptr,
                                                   1,
                                                   &memoryBarrier);
        }

        const bool hasGraphicsPass = std::any_of(passRuntimes.begin(), passRuntimes.end(), [](const PassRuntime& passRuntime) {
            return !passRuntime.isCompute;
        });
        if (hasGraphicsPass)
        {
            // stencil image
            memoryBarrier.image                       = stencilImage;
            memoryBarrier.srcAccessMask               = 0;
            memoryBarrier.dstAccessMask               = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            memoryBarrier.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
            memoryBarrier.newLayout                   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            memoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT | VK_IMAGE_ASPECT_DEPTH_BIT;

            pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                                   0,
                                                   0,
                                                   nullptr,
                                                   0,
                                                   nullptr,
                                                   1,
                                                   &memoryBarrier);
        }

        VkDescriptorSet currentSamplerSet = inputDescriptorSets[imageIndex];
        bool backBufferNext = outputWrites % 2 == 0;
        for (size_t i = 0; i < passRuntimes.size(); i++)
        {
            auto& runtime = passRuntimes[i];
            if (runtime.isCompute)
            {
                pLogicalDevice->vkd.CmdBindDescriptorSets(
                    commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 1, 1, &currentSamplerSet, 0, nullptr);
                if (bufferSize)
                {
                    pLogicalDevice->vkd.CmdBindDescriptorSets(
                        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &bufferDescriptorSet, 0, nullptr);
                }

                pLogicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, runtime.pipeline);
                pLogicalDevice->vkd.CmdDispatch(commandBuffer, runtime.dispatchSizeX, runtime.dispatchSizeY, runtime.dispatchSizeZ);
            }
            else
            {
                pLogicalDevice->vkd.CmdBindDescriptorSets(
                    commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &currentSamplerSet, 0, nullptr);
                if (bufferSize)
                {
                    pLogicalDevice->vkd.CmdBindDescriptorSets(
                        commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &bufferDescriptorSet, 0, nullptr);
                }

                VkRenderPassBeginInfo renderPassBeginInfo = {};
                renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                renderPassBeginInfo.renderPass = runtime.renderPass;
                renderPassBeginInfo.framebuffer = runtime.framebuffers[imageIndex];
                renderPassBeginInfo.renderArea = runtime.renderArea;
                renderPassBeginInfo.clearValueCount = runtime.clearValueCount;
                renderPassBeginInfo.pClearValues = runtime.clearValues.data();

                pLogicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
                pLogicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, runtime.pipeline);
                pLogicalDevice->vkd.CmdDraw(commandBuffer, runtime.vertexCount, 1, 0, 0);
                pLogicalDevice->vkd.CmdEndRenderPass(commandBuffer);

                if (runtime.switchSamplers && outputWrites > 1)
                {
                    if (backBufferNext)
                    {
                        currentSamplerSet = backBufferDescriptorSets[imageIndex];
                    }
                    else if (outputWrites > 2)
                    {
                        currentSamplerSet = outputDescriptorSets[imageIndex];
                    }
                    backBufferNext = !backBufferNext;
                }

                for (const auto& renderTarget : runtime.renderTargets)
                {
                    generateMipMaps(
                        pLogicalDevice, commandBuffer, textureImages[renderTarget][0], textureExtents[renderTarget], textureMipLevels[renderTarget]);
                }
            }

            VkMemoryBarrier passMemoryBarrier = {};
            passMemoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            passMemoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            passMemoryBarrier.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | shaderStages,
                                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | shaderStages,
                                                   0,
                                                   1,
                                                   &passMemoryBarrier,
                                                   0,
                                                   nullptr,
                                                   0,
                                                   nullptr);
        }

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer, shaderStages, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &secondBarrier);
        secondBarrier.image = outputImages[imageIndex];
        secondBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer, shaderStages, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &secondBarrier);
    }

    std::vector<std::unique_ptr<EffectParam>> ReshadeEffect::getParameters() const
    {
        std::vector<std::unique_ptr<EffectParam>> params;

        // Helper lambdas
        auto findAnnotation = [](const auto& annotations, const std::string& name) {
            return std::find_if(annotations.begin(), annotations.end(),
                [&name](const auto& a) { return a.name == name; });
        };

        auto getAnnotationFloat = [](const auto& annotation) {
            return annotation.type.is_floating_point()
                ? annotation.value.as_float[0]
                : static_cast<float>(annotation.value.as_int[0]);
        };

        auto getAnnotationInt = [](const auto& annotation) {
            return annotation.type.is_integral()
                ? annotation.value.as_int[0]
                : static_cast<int>(annotation.value.as_float[0]);
        };

        auto parseNullSeparatedString = [](const std::string& str) {
            std::vector<std::string> items;
            size_t start = 0;
            for (size_t i = 0; i <= str.size(); i++)
            {
                if (i == str.size() || str[i] == '\0')
                {
                    if (i > start)
                        items.push_back(str.substr(start, i - start));
                    start = i + 1;
                }
            }
            return items;
        };

        for (const auto& spec : module.spec_constants)
        {
            // Skip uniforms with "source" annotation (auto-updated like frametime)
            if (findAnnotation(spec.annotations, "source") != spec.annotations.end())
                continue;

            // Skip if no name (can't be configured)
            if (spec.name.empty())
                continue;
            // Skip VKIntox-internal runtime specialization constants.
            if (spec.name.rfind("__vkintox_", 0) == 0)
                continue;

            // Get common annotations
            auto labelIt = findAnnotation(spec.annotations, "ui_label");
            std::string label = (labelIt != spec.annotations.end()) ? labelIt->value.string_data : spec.name;

            auto tooltipIt = findAnnotation(spec.annotations, "ui_tooltip");
            std::string tooltip = (tooltipIt != spec.annotations.end()) ? tooltipIt->value.string_data : "";

            auto typeIt = findAnnotation(spec.annotations, "ui_type");
            std::string uiType = (typeIt != spec.annotations.end()) ? typeIt->value.string_data : "";

            // Get current value from EffectRegistry (the single source of truth)
            EffectParam* registryParam = pEffectRegistry->getParameter(effectName, spec.name);

            // Create appropriate subclass based on spec type
            if (spec.type.is_floating_point())
            {
                auto p = std::make_unique<FloatParam>();
                p->effectName = effectName;
                p->name = spec.name;
                p->label = label;
                p->tooltip = tooltip;
                p->uiType = uiType;
                p->defaultValue = spec.initializer_value.as_float[0];
                // Get value from registry if available
                if (auto* rp = dynamic_cast<FloatParam*>(registryParam))
                    p->value = rp->value;
                else
                    p->value = p->defaultValue;

                auto minIt = findAnnotation(spec.annotations, "ui_min");
                auto maxIt = findAnnotation(spec.annotations, "ui_max");
                if (minIt != spec.annotations.end())
                    p->minValue = getAnnotationFloat(*minIt);
                if (maxIt != spec.annotations.end())
                    p->maxValue = getAnnotationFloat(*maxIt);

                auto stepIt = findAnnotation(spec.annotations, "ui_step");
                if (stepIt != spec.annotations.end())
                    p->step = getAnnotationFloat(*stepIt);

                params.push_back(std::move(p));
            }
            else if (spec.type.is_boolean())
            {
                auto p = std::make_unique<BoolParam>();
                p->effectName = effectName;
                p->name = spec.name;
                p->label = label;
                p->tooltip = tooltip;
                p->uiType = uiType;
                p->defaultValue = (spec.initializer_value.as_uint[0] != 0);
                // Get value from registry if available
                if (auto* rp = dynamic_cast<BoolParam*>(registryParam))
                    p->value = rp->value;
                else
                    p->value = p->defaultValue;

                params.push_back(std::move(p));
            }
            else if (spec.type.is_integral())
            {
                auto p = std::make_unique<IntParam>();
                p->effectName = effectName;
                p->name = spec.name;
                p->label = label;
                p->tooltip = tooltip;
                p->uiType = uiType;
                p->defaultValue = spec.initializer_value.as_int[0];
                // Get value from registry if available
                if (auto* rp = dynamic_cast<IntParam*>(registryParam))
                    p->value = rp->value;
                else
                    p->value = p->defaultValue;

                auto minIt = findAnnotation(spec.annotations, "ui_min");
                auto maxIt = findAnnotation(spec.annotations, "ui_max");
                if (minIt != spec.annotations.end())
                    p->minValue = getAnnotationInt(*minIt);
                if (maxIt != spec.annotations.end())
                    p->maxValue = getAnnotationInt(*maxIt);

                auto stepIt = findAnnotation(spec.annotations, "ui_step");
                if (stepIt != spec.annotations.end())
                    p->step = getAnnotationFloat(*stepIt);

                auto itemsIt = findAnnotation(spec.annotations, "ui_items");
                if (itemsIt != spec.annotations.end())
                    p->items = parseNullSeparatedString(itemsIt->value.string_data);

                params.push_back(std::move(p));
            }
        }

        return params;
    }

    ReshadeEffect::~ReshadeEffect()
    {
        // Guard against a null or already-destroyed device pointer.  This can
        // happen when the VkDevice is destroyed before all shared_ptr<Effect>
        // references are released (e.g. during layer teardown on Wayland where
        // the swapchain is destroyed and recreated before any frame renders).
        if (!pLogicalDevice || pLogicalDevice->device == VK_NULL_HANDLE)
            return;

        Logger::info("[DESTROY-TRACE] ~ReshadeEffect start: " + effectName + " passRuntimes=" + std::to_string(passRuntimes.size()));

        // ----------------------------------------------------------------
        // 1.  Destroy pass-level GPU objects (pipelines, render passes,
        //     framebuffers).  These reference the descriptor set layouts and
        //     shader module, so destroy them first.
        // ----------------------------------------------------------------
        for (auto& passRuntime : passRuntimes)
        {
            for (auto& framebuffer : passRuntime.framebuffers)
            {
                if (framebuffer != VK_NULL_HANDLE)
                    pLogicalDevice->vkd.DestroyFramebuffer(pLogicalDevice->device, framebuffer, nullptr);
            }
            if (passRuntime.pipeline != VK_NULL_HANDLE)
                pLogicalDevice->vkd.DestroyPipeline(pLogicalDevice->device, passRuntime.pipeline, nullptr);
            if (passRuntime.renderPass != VK_NULL_HANDLE)
                pLogicalDevice->vkd.DestroyRenderPass(pLogicalDevice->device, passRuntime.renderPass, nullptr);
        }

        // ----------------------------------------------------------------
        // 2.  Destroy the uniform staging buffer (unmap → free memory →
        //     destroy buffer).
        // ----------------------------------------------------------------
        if (bufferSize)
        {
            if (stagingBufferMapped)
                pLogicalDevice->vkd.UnmapMemory(pLogicalDevice->device, stagingBufferMemory);
            if (stagingBufferMemory != VK_NULL_HANDLE)
                pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, stagingBufferMemory, nullptr);
            if (stagingBuffer != VK_NULL_HANDLE)
                pLogicalDevice->vkd.DestroyBuffer(pLogicalDevice->device, stagingBuffer, nullptr);
        }

        // ----------------------------------------------------------------
        // 3.  Destroy descriptor pool (implicitly frees all descriptor sets
        //     allocated from it) followed by descriptor set layouts and the
        //     shader module.
        // ----------------------------------------------------------------
        if (descriptorPool != VK_NULL_HANDLE)
            pLogicalDevice->vkd.DestroyDescriptorPool(pLogicalDevice->device, descriptorPool, nullptr);
        if (pipelineLayout != VK_NULL_HANDLE)
            pLogicalDevice->vkd.DestroyPipelineLayout(pLogicalDevice->device, pipelineLayout, nullptr);
        if (imageSamplerDescriptorSetLayout != VK_NULL_HANDLE)
            pLogicalDevice->vkd.DestroyDescriptorSetLayout(pLogicalDevice->device, imageSamplerDescriptorSetLayout, nullptr);
        if (uniformDescriptorSetLayout != VK_NULL_HANDLE)
            pLogicalDevice->vkd.DestroyDescriptorSetLayout(pLogicalDevice->device, uniformDescriptorSetLayout, nullptr);
        if (shaderModule != VK_NULL_HANDLE)
            pLogicalDevice->vkd.DestroyShaderModule(pLogicalDevice->device, shaderModule, nullptr);

        // ----------------------------------------------------------------
        // 4.  Collect ALL image views into a single deduplication set, then
        //     destroy each unique handle exactly once.
        //
        //     CRITICAL FIX: the constructor stores the same VkImageView
        //     handles in multiple member vectors/maps (e.g. COLOR/DEPTH
        //     semantic textures share handles with inputImageViewsSRGB).
        //     Destroying them in separate loops caused a double-free that
        //     crashed the NVIDIA driver.  The unified set prevents this.
        // ----------------------------------------------------------------
        std::set<VkImageView> allImageViews;

        auto collectViews = [&allImageViews](const std::vector<VkImageView>& views)
        {
            for (auto v : views)
                if (v != VK_NULL_HANDLE)
                    allImageViews.insert(v);
        };
        auto collectViewMap = [&allImageViews](const std::unordered_map<std::string, std::vector<VkImageView>>& map)
        {
            for (auto& kv : map)
                for (auto v : kv.second)
                    if (v != VK_NULL_HANDLE)
                        allImageViews.insert(v);
        };

        collectViews(inputImageViewsSRGB);
        collectViews(inputImageViewsUNORM);
        collectViews(outputImageViewsSRGB);
        collectViews(outputImageViewsUNORM);
        collectViews(backBufferImageViewsSRGB);
        collectViews(backBufferImageViewsUNORM);
        collectViewMap(textureImageViewsSRGB);
        collectViewMap(textureImageViewsUNORM);
        collectViewMap(renderImageViewsSRGB);
        collectViewMap(renderImageViewsUNORM);
        if (stencilImageView != VK_NULL_HANDLE)
            allImageViews.insert(stencilImageView);

        for (auto imageView : allImageViews)
            pLogicalDevice->vkd.DestroyImageView(pLogicalDevice->device, imageView, nullptr);

        // ----------------------------------------------------------------
        // 5.  Collect and destroy all images.  Use a set for the same
        //     reason — texture images may share handles with other members.
        // ----------------------------------------------------------------
        std::set<VkImage> allImages;
        for (auto& kv : textureImages)
            for (auto img : kv.second)
                if (img != VK_NULL_HANDLE)
                    allImages.insert(img);
        for (auto img : backBufferImages)
            if (img != VK_NULL_HANDLE)
                allImages.insert(img);
        if (stencilImage != VK_NULL_HANDLE)
            allImages.insert(stencilImage);

        for (auto image : allImages)
            pLogicalDevice->vkd.DestroyImage(pLogicalDevice->device, image, nullptr);

        // ----------------------------------------------------------------
        // 6.  Destroy samplers.
        // ----------------------------------------------------------------
        for (auto& sampler : samplers)
        {
            if (sampler != VK_NULL_HANDLE)
                pLogicalDevice->vkd.DestroySampler(pLogicalDevice->device, sampler, nullptr);
        }

        // ----------------------------------------------------------------
        // 7.  Free all device memory (one entry per unique allocation).
        // ----------------------------------------------------------------
        for (auto& memory : textureMemory)
        {
            if (memory != VK_NULL_HANDLE)
                pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, memory, nullptr);
        }

        Logger::info("[DESTROY-TRACE] ~ReshadeEffect complete");
    }

    void ReshadeEffect::createReshadeModule()
    {
        reshadefx::preprocessor preprocessor;
        preprocessor.add_macro_definition("__RESHADE__", std::to_string(INT_MAX));
        preprocessor.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "1");
        preprocessor.add_macro_definition("__RENDERER__", "0x20000");
        // TODO add more macros

        preprocessor.add_macro_definition("BUFFER_WIDTH", std::to_string(imageExtent.width));
        preprocessor.add_macro_definition("BUFFER_HEIGHT", std::to_string(imageExtent.height));
        preprocessor.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
        preprocessor.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
        preprocessor.add_macro_definition("BUFFER_COLOR_DEPTH", (inputOutputFormatUNORM == VK_FORMAT_A2R10G10B10_UNORM_PACK32) ? "10" : "8");
        preprocessor.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "BUFFER_COLOR_DEPTH");
        
        // Pass current depth mode settings to ReShade for proper depth interpretation
        // This ensures flawless depth passthrough to effects (DOF, SSAO, etc.)
        const int depthMode = settingsManager.getDepthSourceChannel();
        const bool depthInvert = settingsManager.getDepthInvert();
        addReshadeDepthMacros(preprocessor, depthMode, depthInvert);

        // Keep runtime compilation behavior aligned with reshade_parser compatibility shims.
        preprocessor.append_string(
            "#define tex2DgatherR(s, coords) tex2Dgather(s, coords, 0)\n"
            "#define tex2DgatherG(s, coords) tex2Dgather(s, coords, 1)\n"
            "#define tex2DgatherB(s, coords) tex2Dgather(s, coords, 2)\n"
            "#define tex2DgatherA(s, coords) tex2Dgather(s, coords, 3)\n"
            "#define storage1D storage\n"
            "#define f32tof16 _vkintox_f32tof16\n"
            "#define f16tof32 _vkintox_f16tof32\n"
            "uint _vkintox_f32tof16(float v) {\n"
            "  uint x = asuint(v);\n"
            "  uint sign = (x >> 16) & 0x8000u;\n"
            "  uint exp = (x >> 23) & 0xFFu;\n"
            "  uint mant = x & 0x7FFFFFu;\n"
            "  if (exp == 0xFFu) {\n"
            "    if (mant == 0u) return sign | 0x7C00u;\n"
            "    uint nanMant = mant >> 13;\n"
            "    if (nanMant == 0u) nanMant = 1u;\n"
            "    return sign | 0x7C00u | (nanMant & 0x03FFu);\n"
            "  }\n"
            "  int newExp = int(exp) - 127 + 15;\n"
            "  if (newExp >= 31) return sign | 0x7C00u;\n"
            "  if (newExp <= 0) {\n"
            "    if (newExp < -10) return sign;\n"
            "    mant |= 0x800000u;\n"
            "    uint shift = uint(14 - newExp);\n"
            "    uint halfMant = mant >> shift;\n"
            "    uint roundMask = (1u << shift) - 1u;\n"
            "    uint roundBits = mant & roundMask;\n"
            "    uint halfway = 1u << (shift - 1u);\n"
            "    if (roundBits > halfway || (roundBits == halfway && (halfMant & 1u) != 0u))\n"
            "      halfMant += 1u;\n"
            "    return sign | (halfMant & 0x03FFu);\n"
            "  }\n"
            "  uint halfExp = uint(newExp) << 10;\n"
            "  uint halfMant = mant >> 13;\n"
            "  uint roundBits = mant & 0x1FFFu;\n"
            "  if (roundBits > 0x1000u || (roundBits == 0x1000u && (halfMant & 1u) != 0u)) {\n"
            "    halfMant += 1u;\n"
            "    if (halfMant == 0x0400u) {\n"
            "      halfMant = 0u;\n"
            "      halfExp += 0x0400u;\n"
            "      if (halfExp >= 0x7C00u) halfExp = 0x7C00u;\n"
            "    }\n"
            "  }\n"
            "  return sign | halfExp | (halfMant & 0x03FFu);\n"
            "}\n"
            "uint2 _vkintox_f32tof16(float2 v) { return uint2(_vkintox_f32tof16(v.x), _vkintox_f32tof16(v.y)); }\n"
            "uint3 _vkintox_f32tof16(float3 v) { return uint3(_vkintox_f32tof16(v.x), _vkintox_f32tof16(v.y), _vkintox_f32tof16(v.z)); }\n"
            "uint4 _vkintox_f32tof16(float4 v) { return uint4(_vkintox_f32tof16(v.x), _vkintox_f32tof16(v.y), _vkintox_f32tof16(v.z), _vkintox_f32tof16(v.w)); }\n"
            "float _vkintox_f16tof32(uint h) {\n"
            "  uint sign = (h & 0x8000u) << 16;\n"
            "  uint exp = (h >> 10) & 0x1Fu;\n"
            "  uint mant = h & 0x03FFu;\n"
            "  if (exp == 0u) {\n"
            "    if (mant == 0u) return asfloat(sign);\n"
            "    int shift = 0;\n"
            "    while ((mant & 0x0400u) == 0u) { mant <<= 1; shift++; }\n"
            "    mant &= 0x03FFu;\n"
            "    uint fullExp = uint(127 - 15 - shift);\n"
            "    return asfloat(sign | (fullExp << 23) | (mant << 13));\n"
            "  }\n"
            "  if (exp == 0x1Fu) return asfloat(sign | 0x7F800000u | (mant << 13));\n"
            "  return asfloat(sign | ((exp + 112u) << 23) | (mant << 13));\n"
            "}\n"
            "float2 _vkintox_f16tof32(uint2 h) { return float2(_vkintox_f16tof32(h.x), _vkintox_f16tof32(h.y)); }\n"
            "float3 _vkintox_f16tof32(uint3 h) { return float3(_vkintox_f16tof32(h.x), _vkintox_f16tof32(h.y), _vkintox_f16tof32(h.z)); }\n"
            "float4 _vkintox_f16tof32(uint4 h) { return float4(_vkintox_f16tof32(h.x), _vkintox_f16tof32(h.y), _vkintox_f16tof32(h.z), _vkintox_f16tof32(h.w)); }\n"
            "#define float2x3 matrix<float, 2, 3>\n"
            "#define float2x4 matrix<float, 2, 4>\n"
            "#define float3x2 matrix<float, 3, 2>\n"
            "#define float3x4 matrix<float, 3, 4>\n"
            "#define float4x2 matrix<float, 4, 2>\n"
            "#define float4x3 matrix<float, 4, 3>\n"
            "#define ddx_fine(x) ddx(x)\n"
            "#define ddy_fine(x) ddy(x)\n"
            "#define ddx_coarse(x) ddx(x)\n"
            "#define ddy_coarse(x) ddy(x)\n");

        // Add custom preprocessor definitions (user-configurable macros)
        for (const auto& def : customPreprocessorDefs)
        {
            preprocessor.add_macro_definition(def.name, def.value);
            Logger::debug("  custom macro: " + def.name + " = " + def.value);
        }

        // Add all discovered shader paths from shader manager
        ShaderManagerConfig shaderMgrConfig = ConfigSerializer::loadShaderManagerConfig();
        for (const auto& path : shaderMgrConfig.discoveredShaderPaths)
            preprocessor.add_include_path(path);

        // Use provided effectPath, or try to find it in discovered shader paths
        std::string shaderPath = this->effectPath;
        if (shaderPath.empty())
        {
            shaderPath = pEffectRegistry->getEffectFilePath(effectName);
            if (shaderPath.empty())
            {
                // Search discovered shader paths for the effect
                for (const auto& searchPath : shaderMgrConfig.discoveredShaderPaths)
                {
                    std::string candidate = searchPath + "/" + effectName + ".fx";
                    if (std::filesystem::exists(candidate))
                    {
                        shaderPath = candidate;
                        break;
                    }
                    candidate = searchPath + "/" + effectName;
                    if (std::filesystem::exists(candidate))
                    {
                        shaderPath = candidate;
                        break;
                    }
                }
            }
        }

        if (shaderPath.empty() || !preprocessor.append_file(shaderPath))
        {
            Logger::err("failed to load shader file: " + shaderPath);
            Logger::err("Does the filepath exist and does it not include spaces?");
            throw std::runtime_error("failed to load shader: " + effectName);
        }

        reshadefx::parser parser;

        std::string errors = preprocessor.errors();
        if (!errors.empty())
            Logger::err(errors);

        const RuntimeCodegenPolicy runtimePolicy = selectRuntimeCodegenPolicy(pLogicalDevice);
        disableComputePipelineOptimization = runtimePolicy.disableComputePipelineOptimization;
        Logger::debug("runtime codegen policy: local_size_id=" + std::to_string(runtimePolicy.useLocalSizeId ? 1 : 0) +
                      " uniform_spec_constants=" + std::to_string(runtimePolicy.useUniformSpecConstants ? 1 : 0) +
                      " debug_info=" + std::to_string(runtimePolicy.emitDebugInfo ? 1 : 0) +
                      " disable_compute_opt=" + std::to_string(runtimePolicy.disableComputePipelineOptimization ? 1 : 0));

        std::unique_ptr<reshadefx::codegen> codegen(reshadefx::create_codegen_spirv(
            true /* vulkan semantics */,
            runtimePolicy.emitDebugInfo,
            runtimePolicy.useUniformSpecConstants,
            true /* flip vertex shader */,
            runtimePolicy.useLocalSizeId));

        if (!parser.parse(std::move(preprocessor.output()), codegen.get()))
        {
            errors = parser.errors();
            if (!errors.empty())
                Logger::err(errors);
            throw std::runtime_error("failed to compile shader: " + effectName);
        }

        errors = parser.errors();
        if (!errors.empty())
        {
            if (hasFatalCompilerDiagnostics(errors))
            {
                Logger::err(errors);
                throw std::runtime_error("failed to compile shader: " + effectName);
            }

            Logger::warn(errors);
        }

        codegen->write_result(module);

        if (module.techniques.empty())
        {
            Logger::err("shader has no techniques: " + effectName);
            throw std::runtime_error("shader has no techniques: " + effectName);
        }

        if (module.spirv.empty())
        {
            Logger::err("shader produced empty SPIR-V: " + effectName);
            throw std::runtime_error("shader produced empty SPIR-V: " + effectName);
        }

        VkShaderModuleCreateInfo shaderCreateInfo;
        shaderCreateInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderCreateInfo.pNext    = nullptr;
        shaderCreateInfo.flags    = 0;
        shaderCreateInfo.codeSize = module.spirv.size() * sizeof(uint32_t);
        shaderCreateInfo.pCode    = module.spirv.data();

        VkResult result = pLogicalDevice->vkd.CreateShaderModule(pLogicalDevice->device, &shaderCreateInfo, nullptr, &shaderModule);
        if (result != VK_SUCCESS)
        {
            Logger::err("failed to create shader module for: " + effectName);
            throw std::runtime_error("VkCreateShaderModule failed for: " + effectName);
        }

        Logger::debug("created reshade shaderModule");
    }

    VkFormat ReshadeEffect::convertReshadeFormat(reshadefx::texture_format texFormat)
    {
        switch (texFormat)
        {
            case reshadefx::texture_format::r8: return VK_FORMAT_R8_UNORM;
            case reshadefx::texture_format::r16: return VK_FORMAT_R16_UNORM;
            case reshadefx::texture_format::r16f: return VK_FORMAT_R16_SFLOAT;
            case reshadefx::texture_format::r32f: return VK_FORMAT_R32_SFLOAT;
            case reshadefx::texture_format::r32i: return VK_FORMAT_R32_SINT;
            case reshadefx::texture_format::rg8: return VK_FORMAT_R8G8_UNORM;
            case reshadefx::texture_format::rg16: return VK_FORMAT_R16G16_UNORM;
            case reshadefx::texture_format::rg16f: return VK_FORMAT_R16G16_SFLOAT;
            case reshadefx::texture_format::rg32f: return VK_FORMAT_R32G32_SFLOAT;
            case reshadefx::texture_format::rgba8: return VK_FORMAT_R8G8B8A8_UNORM;
            case reshadefx::texture_format::rgba16: return VK_FORMAT_R16G16B16A16_UNORM;
            case reshadefx::texture_format::rgba16f: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case reshadefx::texture_format::rgba32f: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case reshadefx::texture_format::rgb10a2: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
            case reshadefx::texture_format::r32u: return VK_FORMAT_R32_UINT;
            default: return VK_FORMAT_UNDEFINED;
        }
    }

    VkCompareOp ReshadeEffect::convertReshadeCompareOp(reshadefx::pass_stencil_func compareOp)
    {
        switch (compareOp)
        {
            case reshadefx::pass_stencil_func::never: return VK_COMPARE_OP_NEVER;
            case reshadefx::pass_stencil_func::less: return VK_COMPARE_OP_LESS;
            case reshadefx::pass_stencil_func::equal: return VK_COMPARE_OP_EQUAL;
            case reshadefx::pass_stencil_func::less_equal: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case reshadefx::pass_stencil_func::greater: return VK_COMPARE_OP_GREATER;
            case reshadefx::pass_stencil_func::not_equal: return VK_COMPARE_OP_NOT_EQUAL;
            case reshadefx::pass_stencil_func::greater_equal: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case reshadefx::pass_stencil_func::always: return VK_COMPARE_OP_ALWAYS;
            default: return VK_COMPARE_OP_ALWAYS;
        }
    }

    VkStencilOp ReshadeEffect::convertReshadeStencilOp(reshadefx::pass_stencil_op stencilOp)
    {
        switch (stencilOp)
        {
            case reshadefx::pass_stencil_op::zero: return VK_STENCIL_OP_ZERO;
            case reshadefx::pass_stencil_op::keep: return VK_STENCIL_OP_KEEP;
            case reshadefx::pass_stencil_op::replace: return VK_STENCIL_OP_REPLACE;
            case reshadefx::pass_stencil_op::incr_sat: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
            case reshadefx::pass_stencil_op::decr_sat: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
            case reshadefx::pass_stencil_op::invert: return VK_STENCIL_OP_INVERT;
            case reshadefx::pass_stencil_op::incr: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
            case reshadefx::pass_stencil_op::decr: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
            default: return VK_STENCIL_OP_KEEP;
        }
    }

    VkBlendOp ReshadeEffect::convertReshadeBlendOp(reshadefx::pass_blend_op blendOp)
    {
        switch (blendOp)
        {
            case reshadefx::pass_blend_op::add: return VK_BLEND_OP_ADD;
            case reshadefx::pass_blend_op::subtract: return VK_BLEND_OP_SUBTRACT;
            case reshadefx::pass_blend_op::rev_subtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
            case reshadefx::pass_blend_op::min: return VK_BLEND_OP_MIN;
            case reshadefx::pass_blend_op::max: return VK_BLEND_OP_MAX;
            default: return VK_BLEND_OP_ADD;
        }
    }

    VkBlendFactor ReshadeEffect::convertReshadeBlendFactor(reshadefx::pass_blend_func blendFactor)
    {
        switch (blendFactor)
        {
            case reshadefx::pass_blend_func::zero: return VK_BLEND_FACTOR_ZERO;
            case reshadefx::pass_blend_func::one: return VK_BLEND_FACTOR_ONE;
            case reshadefx::pass_blend_func::src_color: return VK_BLEND_FACTOR_SRC_COLOR;
            case reshadefx::pass_blend_func::src_alpha: return VK_BLEND_FACTOR_SRC_ALPHA;
            case reshadefx::pass_blend_func::inv_src_color: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case reshadefx::pass_blend_func::inv_src_alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case reshadefx::pass_blend_func::dst_alpha: return VK_BLEND_FACTOR_DST_ALPHA;
            case reshadefx::pass_blend_func::inv_dst_alpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case reshadefx::pass_blend_func::dst_color: return VK_BLEND_FACTOR_DST_COLOR;
            case reshadefx::pass_blend_func::inv_dst_color: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            default: return VK_BLEND_FACTOR_ZERO;
        }
    }
} // namespace VKIntox
