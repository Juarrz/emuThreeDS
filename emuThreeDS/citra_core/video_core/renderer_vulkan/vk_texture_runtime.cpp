// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/microprofile.h"
#include "video_core/rasterizer_cache/custom_tex_manager.h"
#include "video_core/rasterizer_cache/texture_codec.h"
#include "video_core/rasterizer_cache/utils.h"
#include "video_core/renderer_vulkan/pica_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_renderpass_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_texture_runtime.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan_format_traits.hpp>

MICROPROFILE_DEFINE(Vulkan_ImageAlloc, "Vulkan", "Texture Allocation", MP_RGB(192, 52, 235));

namespace Vulkan {

namespace {

using VideoCore::GetFormatType;
using VideoCore::MipLevels;
using VideoCore::StagingData;
using VideoCore::TextureType;

struct RecordParams {
    vk::ImageAspectFlags aspect;
    vk::Filter filter;
    vk::PipelineStageFlags pipeline_flags;
    vk::AccessFlags src_access;
    vk::AccessFlags dst_access;
    vk::Image src_image;
    vk::Image dst_image;
};

vk::Filter MakeFilter(VideoCore::PixelFormat pixel_format) {
    switch (pixel_format) {
    case VideoCore::PixelFormat::D16:
    case VideoCore::PixelFormat::D24:
    case VideoCore::PixelFormat::D24S8:
        return vk::Filter::eNearest;
    default:
        return vk::Filter::eLinear;
    }
}

[[nodiscard]] vk::ClearValue MakeClearValue(VideoCore::ClearValue clear) {
    static_assert(sizeof(VideoCore::ClearValue) == sizeof(vk::ClearValue));

    vk::ClearValue value{};
    std::memcpy(&value, &clear, sizeof(vk::ClearValue));
    return value;
}

[[nodiscard]] vk::ClearColorValue MakeClearColorValue(Common::Vec4f color) {
    return vk::ClearColorValue{
        .float32 = std::array{color[0], color[1], color[2], color[3]},
    };
}

[[nodiscard]] vk::ClearDepthStencilValue MakeClearDepthStencilValue(VideoCore::ClearValue clear) {
    return vk::ClearDepthStencilValue{
        .depth = clear.depth,
        .stencil = clear.stencil,
    };
}

u32 UnpackDepthStencil(const StagingData& data, vk::Format dest) {
    u32 depth_offset = 0;
    u32 stencil_offset = 4 * data.size / 5;
    const auto& mapped = data.mapped;

    switch (dest) {
    case vk::Format::eD24UnormS8Uint: {
        for (; stencil_offset < data.size; depth_offset += 4) {
            u8* ptr = mapped.data() + depth_offset;
            const u32 d24s8 = VideoCore::MakeInt<u32>(ptr);
            const u32 d24 = d24s8 >> 8;
            mapped[stencil_offset] = d24s8 & 0xFF;
            std::memcpy(ptr, &d24, 4);
            stencil_offset++;
        }
        break;
    }
    case vk::Format::eD32SfloatS8Uint: {
        for (; stencil_offset < data.size; depth_offset += 4) {
            u8* ptr = mapped.data() + depth_offset;
            const u32 d24s8 = VideoCore::MakeInt<u32>(ptr);
            const float d32 = (d24s8 >> 8) / 16777215.f;
            mapped[stencil_offset] = d24s8 & 0xFF;
            std::memcpy(ptr, &d32, 4);
            stencil_offset++;
        }
        break;
    }
    default:
        LOG_ERROR(Render_Vulkan, "Unimplemented convertion for depth format {}",
                  vk::to_string(dest));
        UNREACHABLE();
    }

    ASSERT(depth_offset == 4 * data.size / 5);
    return depth_offset;
}

constexpr u64 UPLOAD_BUFFER_SIZE = 128 * 1024 * 1024;
constexpr u64 DOWNLOAD_BUFFER_SIZE = 16 * 1024 * 1024;

} // Anonymous namespace

TextureRuntime::TextureRuntime(const Instance& instance, Scheduler& scheduler,
                               RenderpassCache& renderpass_cache, DescriptorManager& desc_manager)
    : instance{instance}, scheduler{scheduler}, renderpass_cache{renderpass_cache},
      blit_helper{instance, scheduler, desc_manager, renderpass_cache},
      upload_buffer{instance, scheduler, vk::BufferUsageFlagBits::eTransferSrc, UPLOAD_BUFFER_SIZE,
                    BufferType::Upload},
      download_buffer{instance, scheduler, vk::BufferUsageFlagBits::eTransferDst,
                      DOWNLOAD_BUFFER_SIZE, BufferType::Download} {

    auto Register = [this](VideoCore::PixelFormat dest,
                           std::unique_ptr<FormatReinterpreterBase>&& obj) {
        const u32 dst_index = static_cast<u32>(dest);
        return reinterpreters[dst_index].push_back(std::move(obj));
    };

    Register(VideoCore::PixelFormat::RGBA8,
             std::make_unique<D24S8toRGBA8>(instance, scheduler, desc_manager, *this));
}

TextureRuntime::~TextureRuntime() {
    Clear();
}

StagingData TextureRuntime::FindStaging(u32 size, bool upload) {
    auto& buffer = upload ? upload_buffer : download_buffer;
    auto [data, offset, invalidate] = buffer.Map(size, 16);

    return StagingData{
        .size = size,
        .mapped = std::span<u8>{data, size},
        .buffer_offset = offset,
    };
}

void TextureRuntime::Finish() {
    scheduler.Finish();
}

void TextureRuntime::Clear() {
    scheduler.Finish();

    VmaAllocator allocator = instance.GetAllocator();
    renderpass_cache.ClearFramebuffers();
    for (const auto& [key, alloc] : texture_recycler) {
        vmaDestroyImage(allocator, alloc.image, alloc.allocation);
    }

    texture_recycler.clear();
}

Allocation TextureRuntime::Allocate(u32 width, u32 height, u32 levels,
                                    VideoCore::PixelFormat format, VideoCore::TextureType type) {
    const FormatTraits traits = instance.GetTraits(format);
    const bool is_mutable = format == VideoCore::PixelFormat::RGBA8;
    return Allocate(width, height, levels, is_mutable, type, traits.native, traits.usage,
                    traits.aspect);
}

Allocation TextureRuntime::Allocate(u32 width, u32 height, u32 levels, bool is_mutable,
                                    VideoCore::TextureType type, vk::Format format,
                                    vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect) {
    MICROPROFILE_SCOPE(Vulkan_ImageAlloc);

    ASSERT(format != vk::Format::eUndefined && levels >= 1);
    const HostTextureTag key = {
        .format = format,
        .type = type,
        .width = width,
        .height = height,
        .levels = levels,
        .is_mutable = is_mutable,
    };

    if (auto it = texture_recycler.find(key); it != texture_recycler.end()) {
        Allocation alloc = std::move(it->second);
        texture_recycler.erase(it);
        return alloc;
    }

    const u32 layers = type == VideoCore::TextureType::CubeMap ? 6 : 1;

    vk::ImageCreateFlags flags;
    if (type == VideoCore::TextureType::CubeMap) {
        flags |= vk::ImageCreateFlagBits::eCubeCompatible;
    }
    if (is_mutable) {
        flags |= vk::ImageCreateFlagBits::eMutableFormat;
    }

    const bool need_format_list = is_mutable && instance.IsImageFormatListSupported();
    const std::array format_list = {
        vk::Format::eR8G8B8A8Unorm,
        vk::Format::eR32Uint,
    };
    const vk::ImageFormatListCreateInfo image_format_list = {
        .viewFormatCount = static_cast<u32>(format_list.size()),
        .pViewFormats = format_list.data(),
    };

    const vk::ImageCreateInfo image_info = {
        .pNext = need_format_list ? &image_format_list : nullptr,
        .flags = flags,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = levels,
        .arrayLayers = layers,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = usage,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);
    VmaAllocation allocation{};

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }
    const vk::Image image{unsafe_image};

    const vk::ImageViewCreateInfo view_info = {
        .image = image,
        .viewType =
            type == TextureType::CubeMap ? vk::ImageViewType::eCube : vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = 0,
            .levelCount = levels,
            .baseArrayLayer = 0,
            .layerCount = layers,
        },
    };

    vk::Device device = instance.GetDevice();
    vk::UniqueImageView image_view = device.createImageViewUnique(view_info);

    renderpass_cache.EndRendering();
    scheduler.Record([image, aspect](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier init_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = aspect,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                               vk::PipelineStageFlagBits::eTopOfPipe,
                               vk::DependencyFlagBits::eByRegion, {}, {}, init_barrier);
    });

    return Allocation{
        .image = image,
        .image_view = std::move(image_view),
        .allocation = allocation,
        .aspect = aspect,
        .format = format,
        .is_mutable = is_mutable,
        .width = width,
        .height = height,
        .levels = levels,
    };
}

void TextureRuntime::Recycle(const HostTextureTag tag, Allocation&& alloc) {
    texture_recycler.emplace(tag, std::move(alloc));
}

bool TextureRuntime::ClearTexture(Surface& surface, const VideoCore::TextureClear& clear) {
    renderpass_cache.EndRendering();

    const RecordParams params = {
        .aspect = surface.alloc.aspect,
        .pipeline_flags = surface.PipelineStageFlags(),
        .src_access = surface.AccessFlags(),
        .src_image = surface.alloc.image,
    };

    if (clear.texture_rect == surface.GetScaledRect()) {
        scheduler.Record([params, clear](vk::CommandBuffer cmdbuf) {
            const vk::ImageSubresourceRange range = {
                .aspectMask = params.aspect,
                .baseMipLevel = clear.texture_level,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            };

            const vk::ImageMemoryBarrier pre_barrier = {
                .srcAccessMask = params.src_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = clear.texture_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };

            const vk::ImageMemoryBarrier post_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = params.src_access,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = clear.texture_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };

            cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);

            const bool is_color =
                static_cast<bool>(params.aspect & vk::ImageAspectFlagBits::eColor);
            if (is_color) {
                cmdbuf.clearColorImage(params.src_image, vk::ImageLayout::eTransferDstOptimal,
                                       MakeClearColorValue(clear.value.color), range);
            } else {
                cmdbuf.clearDepthStencilImage(params.src_image,
                                              vk::ImageLayout::eTransferDstOptimal,
                                              MakeClearDepthStencilValue(clear.value), range);
            }

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);
        });
        return true;
    }

    ClearTextureWithRenderpass(surface, clear);
    return true;
}

void TextureRuntime::ClearTextureWithRenderpass(Surface& surface,
                                                const VideoCore::TextureClear& clear) {
    const bool is_color = surface.type != VideoCore::SurfaceType::Depth &&
                          surface.type != VideoCore::SurfaceType::DepthStencil;

    const vk::AccessFlags access_flag =
        is_color
            ? vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite
            : vk::AccessFlagBits::eDepthStencilAttachmentRead |
                  vk::AccessFlagBits::eDepthStencilAttachmentWrite;

    const vk::PipelineStageFlags pipeline_flags =
        is_color ? vk::PipelineStageFlagBits::eColorAttachmentOutput
                 : vk::PipelineStageFlagBits::eEarlyFragmentTests;

    const RecordParams params = {
        .aspect = surface.alloc.aspect,
        .pipeline_flags = surface.PipelineStageFlags(),
        .src_access = surface.AccessFlags(),
        .src_image = surface.alloc.image,
    };

    scheduler.Record([params, access_flag, pipeline_flags](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier pre_barrier = {
            .srcAccessMask = params.src_access,
            .dstAccessMask = access_flag,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = params.src_image,
            .subresourceRange{
                .aspectMask = params.aspect,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };

        cmdbuf.pipelineBarrier(params.pipeline_flags, pipeline_flags,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);
    });

    Surface* color_surface{};
    Surface* depth_surface{};
    if (is_color) {
        color_surface = &surface;
    } else {
        depth_surface = &surface;
    }

    const vk::Rect2D render_area = {
        .offset{
            .x = static_cast<s32>(clear.texture_rect.left),
            .y = static_cast<s32>(clear.texture_rect.bottom),
        },
        .extent{
            .width = clear.texture_rect.GetWidth(),
            .height = clear.texture_rect.GetHeight(),
        },
    };

    renderpass_cache.BeginRendering(color_surface, depth_surface, render_area, true,
                                    MakeClearValue(clear.value));
    renderpass_cache.EndRendering();

    scheduler.Record([params, access_flag, pipeline_flags](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier post_barrier = {
            .srcAccessMask = access_flag,
            .dstAccessMask = params.src_access,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = params.src_image,
            .subresourceRange{
                .aspectMask = params.aspect,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };

        cmdbuf.pipelineBarrier(pipeline_flags, params.pipeline_flags,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);
    });
}

bool TextureRuntime::CopyTextures(Surface& source, Surface& dest,
                                  const VideoCore::TextureCopy& copy) {
    renderpass_cache.EndRendering();

    const RecordParams params = {
        .aspect = source.alloc.aspect,
        .filter = MakeFilter(source.pixel_format),
        .pipeline_flags = source.PipelineStageFlags() | dest.PipelineStageFlags(),
        .src_access = source.AccessFlags(),
        .dst_access = dest.AccessFlags(),
        .src_image = source.alloc.image,
        .dst_image = dest.alloc.image,
    };

    scheduler.Record([params, copy](vk::CommandBuffer cmdbuf) {
        const vk::ImageCopy image_copy = {
            .srcSubresource{
                .aspectMask = params.aspect,
                .mipLevel = copy.src_level,
                .baseArrayLayer = copy.src_layer,
                .layerCount = 1,
            },
            .srcOffset = {static_cast<s32>(copy.src_offset.x), static_cast<s32>(copy.src_offset.y),
                          0},
            .dstSubresource{
                .aspectMask = params.aspect,
                .mipLevel = copy.dst_level,
                .baseArrayLayer = copy.dst_layer,
                .layerCount = 1,
            },
            .dstOffset = {static_cast<s32>(copy.dst_offset.x), static_cast<s32>(copy.dst_offset.y),
                          0},
            .extent = {copy.extent.width, copy.extent.height, 1},
        };

        const bool self_copy = params.src_image == params.dst_image;
        const vk::ImageLayout new_src_layout =
            self_copy ? vk::ImageLayout::eGeneral : vk::ImageLayout::eTransferSrcOptimal;
        const vk::ImageLayout new_dst_layout =
            self_copy ? vk::ImageLayout::eGeneral : vk::ImageLayout::eTransferDstOptimal;

        const std::array pre_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = params.src_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = new_src_layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = copy.src_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = params.dst_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = new_dst_layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.dst_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = copy.dst_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };
        const std::array post_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = new_src_layout,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = copy.src_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = params.dst_access,
                .oldLayout = new_dst_layout,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.dst_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = copy.dst_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };

        cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);

        cmdbuf.copyImage(params.src_image, new_src_layout, params.dst_image, new_dst_layout,
                         image_copy);

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
    });

    return true;
}

bool TextureRuntime::BlitTextures(Surface& source, Surface& dest,
                                  const VideoCore::TextureBlit& blit) {
    const bool is_depth_stencil = source.type == VideoCore::SurfaceType::DepthStencil;
    const auto& depth_traits = instance.GetTraits(source.pixel_format);
    if (is_depth_stencil && !depth_traits.blit_support) {
        return blit_helper.BlitDepthStencil(source, dest, blit);
    }

    renderpass_cache.EndRendering();

    const RecordParams params = {
        .aspect = source.alloc.aspect,
        .filter = MakeFilter(source.pixel_format),
        .pipeline_flags = source.PipelineStageFlags() | dest.PipelineStageFlags(),
        .src_access = source.AccessFlags(),
        .dst_access = dest.AccessFlags(),
        .src_image = source.alloc.image,
        .dst_image = dest.alloc.image,
    };

    scheduler.Record([params, blit](vk::CommandBuffer cmdbuf) {
        const std::array source_offsets = {
            vk::Offset3D{static_cast<s32>(blit.src_rect.left),
                         static_cast<s32>(blit.src_rect.bottom), 0},
            vk::Offset3D{static_cast<s32>(blit.src_rect.right), static_cast<s32>(blit.src_rect.top),
                         1},
        };

        const std::array dest_offsets = {
            vk::Offset3D{static_cast<s32>(blit.dst_rect.left),
                         static_cast<s32>(blit.dst_rect.bottom), 0},
            vk::Offset3D{static_cast<s32>(blit.dst_rect.right), static_cast<s32>(blit.dst_rect.top),
                         1},
        };

        const vk::ImageBlit blit_area = {
            .srcSubresource{
                .aspectMask = params.aspect,
                .mipLevel = blit.src_level,
                .baseArrayLayer = blit.src_layer,
                .layerCount = 1,
            },
            .srcOffsets = source_offsets,
            .dstSubresource{
                .aspectMask = params.aspect,
                .mipLevel = blit.dst_level,
                .baseArrayLayer = blit.dst_layer,
                .layerCount = 1,
            },
            .dstOffsets = dest_offsets,
        };

        const std::array read_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = params.src_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = blit.src_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = params.dst_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.dst_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = blit.dst_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };
        const std::array write_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = params.src_access,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = blit.src_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = params.dst_access,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.dst_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = blit.dst_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };

        cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, read_barriers);

        cmdbuf.blitImage(params.src_image, vk::ImageLayout::eTransferSrcOptimal, params.dst_image,
                         vk::ImageLayout::eTransferDstOptimal, blit_area, params.filter);

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                               vk::DependencyFlagBits::eByRegion, {}, {}, write_barriers);
    });

    return true;
}

void TextureRuntime::GenerateMipmaps(Surface& surface) {
    if (surface.custom_format != VideoCore::CustomPixelFormat::RGBA8) {
        LOG_ERROR(Render_Vulkan, "Generating mipmaps for compressed formats unsupported!");
        return;
    }

    renderpass_cache.EndRendering();

    // Always use the allocation width on custom textures
    u32 current_width = surface.alloc.width;
    u32 current_height = surface.alloc.height;
    const u32 levels = surface.levels;

    for (u32 i = 1; i < levels; i++) {
        const VideoCore::Rect2D src_rect{0, current_height, current_width, 0};
        current_width = current_width > 1 ? current_width >> 1 : 1;
        current_height = current_height > 1 ? current_height >> 1 : 1;
        const VideoCore::Rect2D dst_rect{0, current_height, current_width, 0};

        const VideoCore::TextureBlit blit = {
            .src_level = i - 1,
            .dst_level = i,
            .src_rect = src_rect,
            .dst_rect = dst_rect,
        };
        BlitTextures(surface, surface, blit);
    }
}

const ReinterpreterList& TextureRuntime::GetPossibleReinterpretations(
    VideoCore::PixelFormat dest_format) const {
    return reinterpreters[static_cast<u32>(dest_format)];
}

bool TextureRuntime::NeedsConvertion(VideoCore::PixelFormat format) const {
    const FormatTraits traits = instance.GetTraits(format);
    return traits.requires_conversion &&
           // DepthStencil formats are handled elsewhere due to de-interleaving.
           traits.aspect != (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil);
}

Surface::Surface(TextureRuntime& runtime_, const VideoCore::SurfaceParams& params)
    : VideoCore::SurfaceBase{params}, runtime{&runtime_}, instance{&runtime_.GetInstance()},
      scheduler{&runtime_.GetScheduler()} {

    if (pixel_format == VideoCore::PixelFormat::Invalid) {
        return;
    }

    const u32 scaled_width = GetScaledWidth();
    const u32 scaled_height = GetScaledHeight();
    alloc =
        runtime->Allocate(scaled_width, scaled_height, levels, params.pixel_format, texture_type);

    if (instance->IsExtDebugUtilsSupported()) {
        const std::string name = fmt::format(
            "Surface: {}x{} {} {} levels from {:#x} to {:#x}", scaled_width, scaled_height,
            VideoCore::PixelFormatAsString(pixel_format), levels, addr, end);
        const vk::DebugUtilsObjectNameInfoEXT name_info = {
            .objectType = vk::ObjectType::eImage,
            .objectHandle = (u64) static_cast<VkImage>(alloc.image),
            .pObjectName = name.c_str(),
        };
        instance->GetDevice().setDebugUtilsObjectNameEXT(name_info);
    }
}

Surface::Surface(TextureRuntime& runtime_, const VideoCore::SurfaceParams& params,
                 vk::Format format, vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect)
    : VideoCore::SurfaceBase{params}, runtime{&runtime_}, instance{&runtime_.GetInstance()},
      scheduler{&runtime_.GetScheduler()} {
    if (format != vk::Format::eUndefined) {
        alloc = runtime->Allocate(GetScaledWidth(), GetScaledHeight(), levels, false, texture_type,
                                  format, usage, aspect);
    }
}

Surface::~Surface() {
    if (pixel_format == VideoCore::PixelFormat::Invalid || !alloc.image_view) {
        return;
    }

    const HostTextureTag tag = {
        .format = alloc.format,
        .type = texture_type,
        .width = alloc.width,
        .height = alloc.height,
        .levels = alloc.levels,
        .is_mutable = alloc.is_mutable,
    };
    runtime->Recycle(tag, std::move(alloc));
}

void Surface::Upload(const VideoCore::BufferTextureCopy& upload, const StagingData& staging) {
    runtime->renderpass_cache.EndRendering();

    const bool is_scaled = res_scale != 1;
    if (is_scaled) {
        ScaledUpload(upload, staging);
    } else {
        const RecordParams params = {
            .aspect = alloc.aspect,
            .pipeline_flags = PipelineStageFlags(),
            .src_access = AccessFlags(),
            .src_image = alloc.image,
        };

        scheduler->Record([buffer = runtime->upload_buffer.Handle(), format = alloc.format, params,
                           staging, upload](vk::CommandBuffer cmdbuf) {
            u32 num_copies = 1;
            std::array<vk::BufferImageCopy, 2> buffer_image_copies;

            const VideoCore::Rect2D rect = upload.texture_rect;
            buffer_image_copies[0] = vk::BufferImageCopy{
                .bufferOffset = staging.buffer_offset + upload.buffer_offset,
                .bufferRowLength = rect.GetWidth(),
                .bufferImageHeight = rect.GetHeight(),
                .imageSubresource{
                    .aspectMask = params.aspect,
                    .mipLevel = upload.texture_level,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .imageOffset = {static_cast<s32>(rect.left), static_cast<s32>(rect.bottom), 0},
                .imageExtent = {rect.GetWidth(), rect.GetHeight(), 1},
            };

            if (params.aspect & vk::ImageAspectFlagBits::eStencil) {
                buffer_image_copies[0].imageSubresource.aspectMask =
                    vk::ImageAspectFlagBits::eDepth;
                vk::BufferImageCopy& stencil_copy = buffer_image_copies[1];
                stencil_copy = buffer_image_copies[0];
                stencil_copy.bufferOffset += UnpackDepthStencil(staging, format);
                stencil_copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eStencil;
                num_copies++;
            }

            const vk::ImageMemoryBarrier read_barrier = {
                .srcAccessMask = params.src_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = upload.texture_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            const vk::ImageMemoryBarrier write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = params.src_access,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = upload.texture_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };

            cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);

            cmdbuf.copyBufferToImage(buffer, params.src_image, vk::ImageLayout::eTransferDstOptimal,
                                     num_copies, buffer_image_copies.data());

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, write_barrier);

            // Wait for a decode to finish if one is pending. Normally this isn't
            // needed until we actually submit the command buffer but it's safer to do it now
            // to prevent the stream buffer from reclaiming our space before we are done with it.
            staging.Wait();
        });

        runtime->upload_buffer.Commit(staging.size);
    }
}

void Surface::Download(const VideoCore::BufferTextureCopy& download, const StagingData& staging) {
    runtime->renderpass_cache.EndRendering();

    // For depth stencil downloads always use the compute shader fallback
    // to avoid having the interleave the data later. These should(?) be
    // uncommon anyways and the perf hit is very small
    if (type == VideoCore::SurfaceType::DepthStencil) {
        return /*DepthStencilDownload(download, staging)*/;
    }

    const bool is_scaled = res_scale != 1;
    if (is_scaled) {
        ScaledDownload(download, staging);
    } else {
        const RecordParams params = {
            .aspect = alloc.aspect,
            .pipeline_flags = PipelineStageFlags(),
            .src_access = AccessFlags(),
            .src_image = alloc.image,
        };

        scheduler->Record([buffer = runtime->download_buffer.Handle(), params, staging,
                           download](vk::CommandBuffer cmdbuf) {
            const VideoCore::Rect2D rect = download.texture_rect;
            const vk::BufferImageCopy buffer_image_copy = {
                .bufferOffset = staging.buffer_offset + download.buffer_offset,
                .bufferRowLength = rect.GetWidth(),
                .bufferImageHeight = rect.GetHeight(),
                .imageSubresource{
                    .aspectMask = params.aspect,
                    .mipLevel = download.texture_level,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .imageOffset = {static_cast<s32>(rect.left), static_cast<s32>(rect.bottom), 0},
                .imageExtent = {rect.GetWidth(), rect.GetHeight(), 1},
            };

            const vk::ImageMemoryBarrier read_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = download.texture_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            const vk::ImageMemoryBarrier image_write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = download.texture_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            const vk::MemoryBarrier memory_write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            };

            cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);

            cmdbuf.copyImageToBuffer(params.src_image, vk::ImageLayout::eTransferSrcOptimal, buffer,
                                     buffer_image_copy);

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                                   vk::DependencyFlagBits::eByRegion, memory_write_barrier, {},
                                   image_write_barrier);
        });
        runtime->download_buffer.Commit(staging.size);
    }
}

bool Surface::Swap(u32 width, u32 height, VideoCore::CustomPixelFormat format) {
    const FormatTraits& traits = instance->GetTraits(format);
    if (!traits.transfer_support) {
        return false;
    }

    const vk::Format custom_vk_format = traits.native;
    if (alloc.Matches(width, height, levels, custom_vk_format)) {
        return true;
    }

    const HostTextureTag tag = {
        .format = alloc.format,
        .type = texture_type,
        .width = alloc.width,
        .height = alloc.height,
        .levels = levels,
        .is_mutable = alloc.is_mutable,
    };
    runtime->Recycle(tag, std::move(alloc));

    is_custom = true;
    custom_format = format;
    alloc = runtime->Allocate(width, height, levels, false, texture_type, custom_vk_format,
                              traits.usage, traits.aspect);

    LOG_DEBUG(Render_Vulkan, "Swapped {}x{} {} surface at address {:#x} to {}x{} {}",
              GetScaledWidth(), GetScaledHeight(), VideoCore::PixelFormatAsString(pixel_format),
              addr, width, height, VideoCore::CustomPixelFormatAsString(format));

    return true;
}

u32 Surface::GetInternalBytesPerPixel() const {
    // Request 5 bytes for D24S8 as well because we can use the
    // extra space when deinterleaving the data during upload
    if (alloc.format == vk::Format::eD24UnormS8Uint) {
        return 5;
    }

    return vk::blockSize(alloc.format);
}

vk::AccessFlags Surface::AccessFlags() const noexcept {
    const bool is_color = static_cast<bool>(alloc.aspect & vk::ImageAspectFlagBits::eColor);
    const vk::AccessFlags attachment_flags =
        is_color
            ? vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite
            : vk::AccessFlagBits::eDepthStencilAttachmentRead |
                  vk::AccessFlagBits::eDepthStencilAttachmentWrite;

    return vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead |
           vk::AccessFlagBits::eTransferWrite |
           (alloc.is_framebuffer ? attachment_flags : vk::AccessFlagBits::eNone) |
           (alloc.is_storage ? vk::AccessFlagBits::eShaderWrite : vk::AccessFlagBits::eNone);
}

vk::PipelineStageFlags Surface::PipelineStageFlags() const noexcept {
    const bool is_color = static_cast<bool>(alloc.aspect & vk::ImageAspectFlagBits::eColor);
    const vk::PipelineStageFlags attachment_flags =
        is_color ? vk::PipelineStageFlagBits::eColorAttachmentOutput
                 : vk::PipelineStageFlagBits::eEarlyFragmentTests |
                       vk::PipelineStageFlagBits::eLateFragmentTests;

    return vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eFragmentShader |
           (alloc.is_framebuffer ? attachment_flags : vk::PipelineStageFlagBits::eNone) |
           (alloc.is_storage ? vk::PipelineStageFlagBits::eComputeShader
                             : vk::PipelineStageFlagBits::eNone);
}

vk::ImageView Surface::DepthView() noexcept {
    vk::UniqueImageView& depth_view = alloc.depth_view;
    if (depth_view) {
        return depth_view.get();
    }

    const vk::ImageViewCreateInfo view_info = {
        .image = alloc.image,
        .viewType = vk::ImageViewType::e2D,
        .format = instance->GetTraits(pixel_format).native,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eDepth,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    };

    depth_view = instance->GetDevice().createImageViewUnique(view_info);
    return depth_view.get();
}

vk::ImageView Surface::StencilView() noexcept {
    vk::UniqueImageView& stencil_view = alloc.stencil_view;
    if (stencil_view) {
        return stencil_view.get();
    }

    const vk::ImageViewCreateInfo view_info = {
        .image = alloc.image,
        .viewType = vk::ImageViewType::e2D,
        .format = instance->GetTraits(pixel_format).native,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eStencil,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    };

    stencil_view = instance->GetDevice().createImageViewUnique(view_info);
    return stencil_view.get();
}

vk::ImageView Surface::StorageView() noexcept {
    vk::UniqueImageView& storage_view = alloc.storage_view;
    if (storage_view) {
        return storage_view.get();
    }

    alloc.is_storage = true;
    ASSERT_MSG(pixel_format == VideoCore::PixelFormat::RGBA8,
               "Attempted to retrieve storage view from unsupported surface with format {}",
               VideoCore::PixelFormatAsString(pixel_format));

    const vk::ImageViewCreateInfo storage_view_info = {
        .image = alloc.image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR32Uint,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    };
    storage_view = instance->GetDevice().createImageViewUnique(storage_view_info);
    return storage_view.get();
}

void Surface::ScaledUpload(const VideoCore::BufferTextureCopy& upload, const StagingData& staging) {
    const u32 rect_width = upload.texture_rect.GetWidth();
    const u32 rect_height = upload.texture_rect.GetHeight();
    const auto scaled_rect = upload.texture_rect * res_scale;
    const auto unscaled_rect = VideoCore::Rect2D{0, rect_height, rect_width, 0};

    SurfaceParams unscaled_params = *this;
    unscaled_params.width = rect_width;
    unscaled_params.stride = rect_width;
    unscaled_params.height = rect_height;
    unscaled_params.res_scale = 1;
    Surface unscaled_surface{*runtime, unscaled_params};

    const VideoCore::BufferTextureCopy unscaled_upload = {
        .buffer_offset = upload.buffer_offset,
        .buffer_size = upload.buffer_size,
        .texture_rect = unscaled_rect,
    };
    unscaled_surface.Upload(unscaled_upload, staging);

    const VideoCore::TextureBlit blit = {
        .src_level = 0,
        .dst_level = upload.texture_level,
        .src_layer = 0,
        .dst_layer = 0,
        .src_rect = unscaled_rect,
        .dst_rect = scaled_rect,
    };
    runtime->BlitTextures(unscaled_surface, *this, blit);
}

void Surface::ScaledDownload(const VideoCore::BufferTextureCopy& download,
                             const StagingData& staging) {
    const u32 rect_width = download.texture_rect.GetWidth();
    const u32 rect_height = download.texture_rect.GetHeight();
    const VideoCore::Rect2D scaled_rect = download.texture_rect * res_scale;
    const VideoCore::Rect2D unscaled_rect = VideoCore::Rect2D{0, rect_height, rect_width, 0};

    // Allocate an unscaled texture that fits the download rectangle to use as a blit destination
    SurfaceParams unscaled_params = *this;
    unscaled_params.width = rect_width;
    unscaled_params.stride = rect_width;
    unscaled_params.height = rect_height;
    unscaled_params.res_scale = 1;
    Surface unscaled_surface{*runtime, unscaled_params};

    const VideoCore::TextureBlit blit = {
        .src_level = download.texture_level,
        .dst_level = 0,
        .src_layer = 0,
        .dst_layer = 0,
        .src_rect = scaled_rect,
        .dst_rect = unscaled_rect,
    };
    runtime->BlitTextures(*this, unscaled_surface, blit);

    const VideoCore::BufferTextureCopy unscaled_download = {
        .buffer_offset = download.buffer_offset,
        .buffer_size = download.buffer_size,
        .texture_rect = unscaled_rect,
        .texture_level = 0,
    };
    unscaled_surface.Download(unscaled_download, staging);
}

void Surface::DepthStencilDownload(const VideoCore::BufferTextureCopy& download,
                                   const StagingData& staging) {
    const u32 rect_width = download.texture_rect.GetWidth();
    const u32 rect_height = download.texture_rect.GetHeight();
    const VideoCore::Rect2D scaled_rect = download.texture_rect * res_scale;
    const VideoCore::Rect2D unscaled_rect = VideoCore::Rect2D{0, rect_height, rect_width, 0};
    const VideoCore::Rect2D r32_scaled_rect =
        VideoCore::Rect2D{0, scaled_rect.GetHeight(), scaled_rect.GetWidth(), 0};

    // For depth downloads create an R32UI surface and use a compute shader for convert.
    // Then we blit and download that surface.
    // NOTE: We keep the pixel format to D24S8 to avoid linear filtering during scale
    SurfaceParams r32_params = *this;
    r32_params.width = scaled_rect.GetWidth();
    r32_params.stride = scaled_rect.GetWidth();
    r32_params.height = scaled_rect.GetHeight();
    r32_params.type = VideoCore::SurfaceType::Color;
    r32_params.res_scale = 1;
    Surface r32_surface{*runtime, r32_params, vk::Format::eR32Uint,
                        vk::ImageUsageFlagBits::eTransferSrc |
                            vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eStorage,
                        vk::ImageAspectFlagBits::eColor};

    const VideoCore::TextureBlit blit = {
        .src_level = download.texture_level,
        .dst_level = 0,
        .src_layer = 0,
        .dst_layer = 0,
        .src_rect = scaled_rect,
        .dst_rect = r32_scaled_rect,
    };
    runtime->blit_helper.BlitD24S8ToR32(*this, r32_surface, blit);

    // Blit the upper mip level to the lower one to scale without additional allocations
    const bool is_scaled = res_scale != 1;
    if (is_scaled) {
        const VideoCore::TextureBlit r32_blit = {
            .src_level = 0,
            .dst_level = 1,
            .src_layer = 0,
            .dst_layer = 0,
            .src_rect = r32_scaled_rect,
            .dst_rect = unscaled_rect,
        };
        runtime->BlitTextures(r32_surface, r32_surface, r32_blit);
    }

    const VideoCore::BufferTextureCopy r32_download = {
        .buffer_offset = download.buffer_offset,
        .buffer_size = download.buffer_size,
        .texture_rect = unscaled_rect,
        .texture_level = is_scaled ? 1u : 0u,
    };
    r32_surface.Download(r32_download, staging);
}

Framebuffer::Framebuffer(Surface* const color, Surface* const depth_stencil,
                         vk::Rect2D render_area_)
    : render_area{render_area_} {
    PrepareImages(color, depth_stencil);
}

Framebuffer::Framebuffer(TextureRuntime& runtime, Surface* const color,
                         Surface* const depth_stencil, const Pica::Regs& regs,
                         Common::Rectangle<u32> surfaces_rect)
    : VideoCore::FramebufferBase{regs, color, depth_stencil, surfaces_rect} {

    // Update render area
    render_area.offset.x = draw_rect.left;
    render_area.offset.y = draw_rect.bottom;
    render_area.extent.width = draw_rect.GetWidth();
    render_area.extent.height = draw_rect.GetHeight();

    PrepareImages(color, depth_stencil);
}

Framebuffer::~Framebuffer() = default;

void Framebuffer::PrepareImages(Surface* const color, Surface* const depth_stencil) {
    u32 cursor{0};
    width = height = std::numeric_limits<u32>::max();

    const auto Prepare = [&](Surface* const surface) {
        if (!surface) {
            formats[cursor++] = VideoCore::PixelFormat::Invalid;
            return;
        }

        width = std::min(width, surface->GetScaledWidth());
        height = std::min(height, surface->GetScaledHeight());
        formats[cursor] = surface->pixel_format;
        images[cursor] = surface->Image();
        image_views[cursor++] = surface->FramebufferView();
    };

    // Setup image handles
    Prepare(color);
    Prepare(depth_stencil);
}

Sampler::Sampler(TextureRuntime& runtime, const VideoCore::SamplerParams& params) {
    using TextureConfig = VideoCore::SamplerParams::TextureConfig;

    const Instance& instance = runtime.GetInstance();
    const vk::PhysicalDeviceProperties properties = instance.GetPhysicalDevice().getProperties();
    const bool use_border_color =
        instance.IsCustomBorderColorSupported() && (params.wrap_s == TextureConfig::ClampToBorder ||
                                                    params.wrap_t == TextureConfig::ClampToBorder);

    const Common::Vec4f color = PicaToVK::ColorRGBA8(params.border_color);
    const vk::SamplerCustomBorderColorCreateInfoEXT border_color_info = {
        .customBorderColor = MakeClearColorValue(color),
        .format = vk::Format::eUndefined,
    };

    const vk::Filter mag_filter = PicaToVK::TextureFilterMode(params.mag_filter);
    const vk::Filter min_filter = PicaToVK::TextureFilterMode(params.min_filter);
    const vk::SamplerMipmapMode mipmap_mode = PicaToVK::TextureMipFilterMode(params.mip_filter);
    const vk::SamplerAddressMode wrap_u = PicaToVK::WrapMode(params.wrap_s);
    const vk::SamplerAddressMode wrap_v = PicaToVK::WrapMode(params.wrap_t);
    const float lod_min = static_cast<float>(params.lod_min);
    const float lod_max = static_cast<float>(params.lod_max);

    const vk::SamplerCreateInfo sampler_info = {
        .pNext = use_border_color ? &border_color_info : nullptr,
        .magFilter = mag_filter,
        .minFilter = min_filter,
        .mipmapMode = mipmap_mode,
        .addressModeU = wrap_u,
        .addressModeV = wrap_v,
        .mipLodBias = 0,
        .anisotropyEnable = instance.IsAnisotropicFilteringSupported(),
        .maxAnisotropy = properties.limits.maxSamplerAnisotropy,
        .compareEnable = false,
        .compareOp = vk::CompareOp::eAlways,
        .minLod = lod_min,
        .maxLod = lod_max,
        .borderColor =
            use_border_color ? vk::BorderColor::eFloatCustomEXT : vk::BorderColor::eIntOpaqueBlack,
        .unnormalizedCoordinates = false,
    };

    vk::Device device = runtime.GetInstance().GetDevice();
    sampler = device.createSamplerUnique(sampler_info);
}

Sampler::~Sampler() = default;

} // namespace Vulkan
