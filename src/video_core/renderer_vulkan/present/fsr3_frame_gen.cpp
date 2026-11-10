// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>

#include "common/logging.h"

#include "video_core/renderer_vulkan/present/fsr3_frame_gen.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"

#include "video_core/host_shaders/fsr3_frame_interpolation_comp_spv.h"
#include "video_core/host_shaders/fsr3_optical_flow_comp_spv.h"

namespace Vulkan {

namespace {

// ΓöÇΓöÇ Precise pipeline barriers ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ

VkImageMemoryBarrier MakeBarrier(VkImage image, VkAccessFlags src_access,
                                 VkAccessFlags dst_access, VkImageLayout old_layout,
                                 VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
}

void BarrierComputeWriteToRead(vk::CommandBuffer cmdbuf, VkImage image) {
    const auto barrier = MakeBarrier(image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                     VK_IMAGE_LAYOUT_GENERAL,
                                     VK_IMAGE_LAYOUT_GENERAL);
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barrier);
}

void DiscardForComputeWrite(vk::CommandBuffer cmdbuf, VkImage image) {
    const auto barrier = MakeBarrier(image, 0, VK_ACCESS_SHADER_WRITE_BIT,
                                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barrier);
}

// ΓöÇΓöÇ Batched dual-copy ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
// Copy one source image into TWO destinations with only 3 barrier calls
// instead of 6 (avoiding redundant source transitions).
void CopyImageToTwo(vk::CommandBuffer cmdbuf, VkImage source,
                    VkImage dest_a, VkImage dest_b, VkExtent2D extent) {
    const VkImageCopy region{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {extent.width, extent.height, 1},
    };

    // 1. Transition source ΓåÆ TRANSFER_SRC, both dests ΓåÆ TRANSFER_DST (batched)
    const std::array before{
        MakeBarrier(source, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        MakeBarrier(dest_a, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        MakeBarrier(dest_b, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);

    // 2. Copy source ΓåÆ dest_a, then source ΓåÆ dest_b (source stays TRANSFER_SRC)
    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest_a,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest_b,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);

    // 3. Transition all three back to GENERAL (batched)
    const std::array after{
        MakeBarrier(source, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        MakeBarrier(dest_a, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        MakeBarrier(dest_b, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, {}, {}, after);
}

void CopyImageToImage(vk::CommandBuffer cmdbuf, VkImage source, VkImage destination,
                      VkExtent2D extent) {
    const VkImageCopy region{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {extent.width, extent.height, 1},
    };

    const std::array before{
        MakeBarrier(source, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        MakeBarrier(destination, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);

    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);

    const std::array after{
        MakeBarrier(source, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        MakeBarrier(destination, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, {}, {}, after);
}

} // anonymous namespace

struct FlowPushConstants {
    float inv_render_w;
    float inv_render_h;
    float render_w;
    float render_h;
    float search_radius;
    float block_size;
};

struct InterpPushConstants {
    float inv_render_w;
    float inv_render_h;
    float render_w;
    float render_h;
    float blend_factor;
    float flow_scale;
    // Converts the sampled motion to texture coordinates.
    float motion_scale_x;
    float motion_scale_y;
    // 1 when BackwardMotion holds the reverse flow, on the previous frame's pixels.
    float has_backward;
};

Fsr3FrameGen::Fsr3FrameGen(const Device& device, MemoryAllocator& memory_allocator,
                           VkExtent2D extent, VkFormat format)
    : m_allocator{memory_allocator}, m_extent{extent},
      m_flow_extent{(std::max)(1u, extent.width / 2), (std::max)(1u, extent.height / 2)},
      m_format{format} {
    // The device enables optical flow only when frame_gen_motion asks for it.
    const bool try_nvof = device.HasOpticalFlow();
    if (try_nvof && device.HasFrameGenCompute()) {
        m_try_async = true;
        if (device.GetFrameGenComputeFamily() != device.GetGraphicsFamily()) {
            m_shared_families = {device.GetGraphicsFamily(), device.GetFrameGenComputeFamily()};
        }
    }
    CreateResources(device);
    CreateShaders(device);
    CreatePipelines(device);
    m_initialized = true;

    if (try_nvof) {
        try {
            m_nvof = std::make_unique<NvOpticalFlow>(device, memory_allocator, extent);
        } catch (const vk::Exception& exception) {
            LOG_ERROR(Render_Vulkan, "Optical flow session failed: {}", exception.what());
            m_nvof.reset();
        }
        if (m_nvof && !m_nvof->IsValid()) {
            m_nvof.reset();
        }
        m_nearest_sampler = CreateNearestNeighborSampler(device);
    }
    if (m_nvof && m_try_async) {
        CreateAsyncResources(device);
        m_nvof->UseTimeline();
    }
    LOG_INFO(Render_Vulkan, "Frame generation motion: {}{}",
             m_nvof ? "NVIDIA optical flow hardware" : "flow shader",
             m_async ? ", generated on a separate queue" : "");
}

Fsr3FrameGen::~Fsr3FrameGen() {
    if (m_async) {
        void(m_device->GetFrameGenComputeQueue().WaitIdle());
    }
}

void Fsr3FrameGen::CreateAsyncResources(const Device& device) {
    m_device = &device;
    constexpr u32 slots = MaxAsyncFrames;
    m_async_pool = device.GetLogical().CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.GetFrameGenComputeFamily(),
    });
    m_async_cmdbufs = m_async_pool.Allocate(slots);
    const std::array pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * slots},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, slots},
    };
    m_async_desc_pool = device.GetLogical().CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = slots,
        .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    });
    std::vector<VkDescriptorSetLayout> layouts(slots, *m_interp_desc_layout);
    m_async_sets = CreateWrappedDescriptorSets(m_async_desc_pool, layouts);
    m_async = true;
}

void Fsr3FrameGen::CreateResources(const Device& device) {
    m_linear_sampler = CreateBilinearSampler(device);

    // The previous and current frames, read on the frame generation compute queue when shared.
    const bool shared = !m_shared_families.empty();
    const auto create_frame_copy = [&] {
        return m_allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = m_format,
            .extent = {m_extent.width, m_extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            .sharingMode = shared ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = shared ? static_cast<u32>(m_shared_families.size()) : 0U,
            .pQueueFamilyIndices = shared ? m_shared_families.data() : nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
    };
    m_history_image = create_frame_copy();
    m_history_view = CreateWrappedImageView(device, m_history_image, m_format);
    m_current_image = create_frame_copy();
    m_current_view = CreateWrappedImageView(device, m_current_image, m_format);

    // Half-resolution motion vector image
    m_motion_image = CreateWrappedImage(m_allocator, m_flow_extent, VK_FORMAT_R16G16_SFLOAT);
    m_motion_view = CreateWrappedImageView(device, m_motion_image, VK_FORMAT_R16G16_SFLOAT);

    const std::array pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
    };
    m_desc_pool = device.GetLogical().CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    });
}

void Fsr3FrameGen::CreateShaders(const Device& device) {
    m_flow_shader = BuildShader(device, FSR3_OPTICAL_FLOW_COMP_SPV);
    m_interp_shader = BuildShader(device, FSR3_FRAME_INTERPOLATION_COMP_SPV);
}

void Fsr3FrameGen::CreatePipelines(const Device& device) {
    // ΓöÇΓöÇ Optical Flow Pipeline ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    const std::array flow_bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };

    m_flow_desc_layout = device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(flow_bindings.size()),
        .pBindings = flow_bindings.data(),
    });

    const VkPushConstantRange flow_push_range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(FlowPushConstants),
    };

    m_flow_pipe_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = m_flow_desc_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &flow_push_range,
    });

    m_flow_pipeline = device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *m_flow_shader,
            .pName = "main",
        },
        .layout = *m_flow_pipe_layout,
    });

    // ΓöÇΓöÇ Frame Interpolation Pipeline ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    const std::array interp_bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };

    m_interp_desc_layout = device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(interp_bindings.size()),
        .pBindings = interp_bindings.data(),
    });

    const VkPushConstantRange interp_push_range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(InterpPushConstants),
    };

    m_interp_pipe_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = m_interp_desc_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &interp_push_range,
    });

    m_interp_pipeline = device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *m_interp_shader,
            .pName = "main",
        },
        .layout = *m_interp_pipe_layout,
    });

    // Allocate descriptor sets
    m_flow_desc_sets = CreateWrappedDescriptorSets(m_desc_pool, {*m_flow_desc_layout});
}

void Fsr3FrameGen::Process(const Device& device, Scheduler& scheduler, VkImage current_frame,
                           VkImageView, u64 frame_count) {
    if (!m_has_history) {
        // First frame: populate history and current-frame copy via batched dual-copy
        scheduler.Record([this, current_frame](vk::CommandBuffer cmdbuf) {
            CopyImageToTwo(cmdbuf, current_frame, *m_history_image, *m_current_image, m_extent);
            DiscardForComputeWrite(cmdbuf, *m_motion_image);
        });
        m_has_history = true;
        m_last_frame_count = frame_count;
        if (m_nvof) {
            void(m_nvof->Run(scheduler, current_frame));
        }
        return;
    }

    // ΓöÇΓöÇ Duplicate-frame detection ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    if (frame_count == m_last_frame_count) {
        return;
    }
    m_last_frame_count = frame_count;

    if (m_nvof) {
        if (m_async) {
            // The last generation reads the copies overwritten below.
            void(m_device->GetFrameGenComputeQueue().WaitIdle());
        }
        scheduler.Record([this, current_frame](vk::CommandBuffer cmdbuf) {
            CopyImageToImage(cmdbuf, *m_current_image, *m_history_image, m_extent);
            CopyImageToImage(cmdbuf, current_frame, *m_current_image, m_extent);
        });
        m_flow_wait = m_nvof->Run(scheduler, current_frame);
        if (m_nvof->IsValid()) {
            m_flow_valid = m_flow_wait != VK_NULL_HANDLE;
            m_flow_copied = false;
            return;
        }
        LOG_WARNING(Render_Vulkan, "Optical flow hardware unusable, using the flow shader");
        // The flow shader generates on the graphics queue. The wait above already finished
        // the last async generation.
        m_async = false;
        m_nvof.reset();
        m_flow_valid = false;
        return;
    }

    // ΓöÇΓöÇ Descriptor caching ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    if (!m_flow_descriptors_initialized) {
        std::vector<VkDescriptorImageInfo> image_infos;
        image_infos.reserve(4);
        std::vector<VkWriteDescriptorSet> updates{
            CreateWriteDescriptorSet(image_infos, *m_linear_sampler, *m_current_view, m_flow_desc_sets[0], 0),
            CreateWriteDescriptorSet(image_infos, *m_linear_sampler, *m_history_view, m_flow_desc_sets[0], 1),
        };

        const VkDescriptorImageInfo motion_store_info{
            .imageView = *m_motion_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        updates.push_back(VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = m_flow_desc_sets[0],
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &motion_store_info,
        });
        device.GetLogical().UpdateDescriptorSets(updates, {});
        m_flow_descriptors_initialized = true;
    }

    // Dispatch Optical Flow at half resolution
    const u32 flow_w = m_flow_extent.width;
    const u32 flow_h = m_flow_extent.height;
    const FlowPushConstants constants{
        .inv_render_w = 1.0f / static_cast<float>(flow_w),
        .inv_render_h = 1.0f / static_cast<float>(flow_h),
        .render_w = static_cast<float>(flow_w),
        .render_h = static_cast<float>(flow_h),
        .search_radius = 2.0f,
        .block_size = 2.0f,
    };

    scheduler.Record([this, constants, current_frame, flow_w, flow_h](vk::CommandBuffer cmdbuf) {
        CopyImageToImage(cmdbuf, *m_current_image, *m_history_image, m_extent);
        CopyImageToImage(cmdbuf, current_frame, *m_current_image, m_extent);

        const auto barrier = MakeBarrier(*m_motion_image,
                                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                         VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                         VK_IMAGE_LAYOUT_GENERAL);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barrier);

        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_flow_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_flow_pipe_layout, 0,
                                  m_flow_desc_sets[0], {});
        cmdbuf.PushConstants(*m_flow_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch((flow_w + 15) / 16, (flow_h + 15) / 16, 1);

        BarrierComputeWriteToRead(cmdbuf, *m_motion_image);
    });

    m_flow_valid = true;
}

void Fsr3FrameGen::Generate(const Device& device, Scheduler& scheduler, VkImage destination,
                            VkImageView destination_storage_view, float blend_factor,
                            float flow_scale) {
    if (!m_has_history || !m_flow_valid) {
        return;
    }

    // ΓöÇΓöÇ Descriptor caching for interpolation ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    if (!m_interp_pool) {
        m_interp_pool.emplace(device, scheduler);
        m_interp_allocator = m_interp_pool->Allocator(device, scheduler, *m_interp_desc_layout,
                                                      DescriptorBankInfo{.textures = 4, .images = 1});
    }
    if (m_nvof && !m_flow_copied) {
        // In the submission that waits for the optical flow (the first after Process).
        m_flow_copied = true;
        // The slot is taken now: the next Run can change it before the worker records this.
        scheduler.Record([nvof = m_nvof.get(), slot = m_nvof->LastSlot()](vk::CommandBuffer cmdbuf) {
            nvof->RecordFlowCopy(cmdbuf, slot);
        });
    }
    const VkDescriptorSet set = m_interp_allocator.Commit();
    std::vector<VkDescriptorImageInfo> image_infos;
    image_infos.reserve(5);
    std::vector<VkWriteDescriptorSet> updates{
        CreateWriteDescriptorSet(image_infos, *m_linear_sampler, *m_current_view, set, 0),
        CreateWriteDescriptorSet(image_infos, *m_linear_sampler, *m_history_view, set, 1),
        m_nvof ? CreateWriteDescriptorSet(image_infos, *m_nearest_sampler, m_nvof->FlowView(), set, 2)
               : CreateWriteDescriptorSet(image_infos, *m_linear_sampler, *m_motion_view, set, 2),
        m_nvof ? CreateWriteDescriptorSet(image_infos, *m_nearest_sampler,
                                          m_nvof->BackwardFlowView(), set, 4)
               : CreateWriteDescriptorSet(image_infos, *m_linear_sampler, *m_motion_view, set, 4),
    };

    const VkDescriptorImageInfo dest_info{
        .imageView = destination_storage_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    updates.push_back(VkWriteDescriptorSet{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = 3,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .pImageInfo = &dest_info,
    });
    device.GetLogical().UpdateDescriptorSets(updates, {});

    // Interpolation dispatches at full resolution
    const u32 out_w = m_extent.width;
    const u32 out_h = m_extent.height;
    const InterpPushConstants constants{
        .inv_render_w = 1.0f / static_cast<float>(out_w),
        .inv_render_h = 1.0f / static_cast<float>(out_h),
        .render_w = static_cast<float>(out_w),
        .render_h = static_cast<float>(out_h),
        .blend_factor = blend_factor,
        .flow_scale = flow_scale,
        // The hardware flow is in pixels, the shader's in flow texels.
        .motion_scale_x = m_nvof ? NvOpticalFlow::FlowToPixels *
                                       m_nvof->InputScaleX() / static_cast<float>(m_extent.width)
                                 : 1.0f / static_cast<float>(m_flow_extent.width),
        .motion_scale_y = m_nvof ? NvOpticalFlow::FlowToPixels *
                                       m_nvof->InputScaleY() / static_cast<float>(m_extent.height)
                                 : 1.0f / static_cast<float>(m_flow_extent.height),
        .has_backward = m_nvof && m_nvof->HasBackwardFlow() ? 1.0f : 0.0f,
    };

    scheduler.Record([this, constants, out_w, out_h, set, destination](vk::CommandBuffer cmdbuf) {
        DiscardForComputeWrite(cmdbuf, destination);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_interp_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_interp_pipe_layout, 0,
                                  set, {});
        cmdbuf.PushConstants(*m_interp_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch((out_w + 15) / 16, (out_h + 15) / 16, 1);
    });
}

void Fsr3FrameGen::GenerateAsync(const Device& device, Frame& destination, float blend_factor,
                                 float flow_scale) {
    if (!m_has_history || !m_flow_valid) {
        return;
    }
    // The frame (and so this slot) was acquired after its last presentation, which waited for
    // the last generation into it.
    const u32 slot = destination.index % MaxAsyncFrames;
    const VkDescriptorSet set = m_async_sets[slot];
    std::vector<VkDescriptorImageInfo> image_infos;
    image_infos.reserve(5);
    const VkDescriptorImageInfo destination_info{
        .sampler = VK_NULL_HANDLE,
        .imageView = *destination.storage_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const std::array updates{
        CreateWriteDescriptorSet(image_infos, *m_linear_sampler, *m_current_view, set, 0),
        CreateWriteDescriptorSet(image_infos, *m_linear_sampler, *m_history_view, set, 1),
        CreateWriteDescriptorSet(image_infos, *m_nearest_sampler, m_nvof->FlowView(), set, 2),
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = set,
            .dstBinding = 3,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &destination_info,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
        CreateWriteDescriptorSet(image_infos, *m_nearest_sampler, m_nvof->BackwardFlowView(), set,
                                 4),
    };
    device.GetLogical().UpdateDescriptorSets(updates, {});

    const InterpPushConstants constants{
        .inv_render_w = 1.0f / static_cast<float>(m_extent.width),
        .inv_render_h = 1.0f / static_cast<float>(m_extent.height),
        .render_w = static_cast<float>(m_extent.width),
        .render_h = static_cast<float>(m_extent.height),
        .blend_factor = blend_factor,
        .flow_scale = flow_scale,
        .motion_scale_x = NvOpticalFlow::FlowToPixels *
                          m_nvof->InputScaleX() / static_cast<float>(m_extent.width),
        .motion_scale_y = NvOpticalFlow::FlowToPixels *
                          m_nvof->InputScaleY() / static_cast<float>(m_extent.height),
        .has_backward = m_nvof->HasBackwardFlow() ? 1.0f : 0.0f,
    };

    const VkCommandBuffer raw = m_async_cmdbufs[slot];
    vk::CommandBuffer cmdbuf{raw, device.GetDispatchLoader()};
    cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    if (!m_flow_copied) {
        m_flow_copied = true;
        m_nvof->RecordFlowCopy(cmdbuf, m_nvof->LastSlot());
    }
    DiscardForComputeWrite(cmdbuf, *destination.image);
    cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_interp_pipeline);
    cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_interp_pipe_layout, 0, set, {});
    cmdbuf.PushConstants(*m_interp_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
    cmdbuf.Dispatch((m_extent.width + 15) / 16, (m_extent.height + 15) / 16, 1);
    cmdbuf.End();

    // Waits for the optical flow (which waited for the frame copies) on its timeline.
    const VkSemaphore wait = m_flow_wait;
    const u64 wait_value = m_nvof->LastFlowValue();
    const u64 signal_value = 0;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                            VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSemaphore signal = *destination.render_ready;
    const VkTimelineSemaphoreSubmitInfo timeline_info{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &wait_value,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &signal_value,
    };
    const VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline_info,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &wait,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &raw,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &signal,
    };
    destination.generated_ready.Reset();
    vk::Check(device.GetFrameGenComputeQueue().Submit(submit, *destination.generated_ready));
    destination.generated_async = true;
}


} // namespace Vulkan
