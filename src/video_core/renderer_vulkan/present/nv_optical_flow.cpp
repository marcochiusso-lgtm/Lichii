// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/present/nv_optical_flow.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {

std::vector<VkFormat> OpticalFlowFormats(const Device& device, VkOpticalFlowUsageFlagsNV usage) {
    const auto& dld = device.GetDispatchLoader();
    if (!dld.vkGetPhysicalDeviceOpticalFlowImageFormatsNV) {
        return {};
    }
    const VkOpticalFlowImageFormatInfoNV info{
        .sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV,
        .pNext = nullptr,
        .usage = usage,
    };
    u32 count{};
    dld.vkGetPhysicalDeviceOpticalFlowImageFormatsNV(device.GetPhysical(), &info, &count, nullptr);
    std::vector<VkOpticalFlowImageFormatPropertiesNV> properties(
        count, {.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_PROPERTIES_NV});
    dld.vkGetPhysicalDeviceOpticalFlowImageFormatsNV(device.GetPhysical(), &info, &count,
                                                     properties.data());
    std::vector<VkFormat> formats;
    for (u32 i = 0; i < count; ++i) {
        formats.push_back(properties[i].format);
    }
    return formats;
}

std::string FormatList(const std::vector<VkFormat>& formats) {
    std::string text;
    for (const VkFormat format : formats) {
        text += (text.empty() ? "" : ", ") + std::to_string(static_cast<int>(format));
    }
    return text.empty() ? "none" : text;
}

VkImageMemoryBarrier Barrier(VkImage image, VkAccessFlags src, VkAccessFlags dst,
                             VkImageLayout old_layout, VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src,
        .dstAccessMask = dst,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
}

} // Anonymous namespace

NvOpticalFlow::NvOpticalFlow(const Device& device_, MemoryAllocator& allocator, VkExtent2D extent_)
    : device{device_}, extent{extent_} {
    const auto& dld = device.GetDispatchLoader();
    const auto& properties = device.GetOpticalFlowProperties();
    if (!device.HasOpticalFlow() || !dld.vkCreateOpticalFlowSessionNV ||
        !dld.vkCmdOpticalFlowExecuteNV) {
        return;
    }
    // Above 1920 pixels wide the flow runs on a half-size copy: the hardware's cost grows with
    // the input, and a vector per input pixel there still beats one per 2x2 output pixels.
    const bool half_input = extent.width > 1920;
    input_extent = half_input ? VkExtent2D{(std::max)(1U, extent.width / 2),
                                           (std::max)(1U, extent.height / 2)}
                              : extent;
    if (input_extent.width < properties.minWidth || input_extent.height < properties.minHeight ||
        input_extent.width > properties.maxWidth || input_extent.height > properties.maxHeight) {
        LOG_WARNING(Render_Vulkan, "Optical flow: {}x{} is outside the supported sizes",
                    input_extent.width, input_extent.height);
        return;
    }

    const auto input_formats = OpticalFlowFormats(device, VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV);
    const auto output_formats = OpticalFlowFormats(device, VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV);
    for (const VkFormat candidate : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        if (std::ranges::find(input_formats, candidate) != input_formats.end()) {
            input_format = candidate;
            break;
        }
    }
    constexpr VkFormat flow_format = VK_FORMAT_R16G16_SFIXED5_NV;
    const bool flow_sampled =
        (device.GetPhysical().GetFormatProperties(flow_format).optimalTilingFeatures &
         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0;
    LOG_INFO(Render_Vulkan, "Optical flow formats: input {}, output {}, flow sampled {}",
             FormatList(input_formats), FormatList(output_formats), flow_sampled);
    if (input_format == VK_FORMAT_UNDEFINED || !flow_sampled ||
        std::ranges::find(output_formats, flow_format) == output_formats.end()) {
        return;
    }

    // One vector per 2x2 output pixels: coarser cells mix a figure's motion with the
    // background's along its outline.
    const u32 wanted_grid = half_input ? 1 : 2;
    VkOpticalFlowGridSizeFlagsNV grid{};
    u32 grid_size{};
    const std::array grids{std::pair{VK_OPTICAL_FLOW_GRID_SIZE_1X1_BIT_NV, 1U},
                           std::pair{VK_OPTICAL_FLOW_GRID_SIZE_2X2_BIT_NV, 2U},
                           std::pair{VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV, 4U},
                           std::pair{VK_OPTICAL_FLOW_GRID_SIZE_8X8_BIT_NV, 8U}};
    for (const auto& [bit, size] : grids) {
        // The wanted size, else the next coarser one supported.
        if (size >= wanted_grid && (properties.supportedOutputGridSizes & bit) != 0) {
            grid = bit;
            grid_size = size;
            break;
        }
    }
    if (grid_size == 0) {
        return;
    }
    flow_extent = {(input_extent.width + grid_size - 1) / grid_size,
                   (input_extent.height + grid_size - 1) / grid_size};

    // The frame generation compute queue reads the flows too.
    std::vector<u32> families{device.GetGraphicsFamily(), device.GetOpticalFlowFamily()};
    if (device.HasFrameGenCompute() &&
        std::ranges::find(families, device.GetFrameGenComputeFamily()) == families.end()) {
        families.push_back(device.GetFrameGenComputeFamily());
    }
    const auto create_image = [&](VkExtent2D size, VkFormat format, VkImageUsageFlags usage,
                                  VkOpticalFlowUsageFlagsNV flow_usage) {
        VkOpticalFlowImageFormatInfoNV flow_info{
            .sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV,
            .pNext = nullptr,
            .usage = flow_usage,
        };
        return allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &flow_info,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {size.width, size.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_CONCURRENT,
            .queueFamilyIndexCount = static_cast<u32>(families.size()),
            .pQueueFamilyIndices = families.data(),
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
    };
    const auto create_view = [&](vk::Image& image, VkFormat format) {
        return device.GetLogical().CreateImageView(VkImageViewCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .components{},
            .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        });
    };
    for (size_t i = 0; i < 2; ++i) {
        inputs[i] = create_image(input_extent, input_format, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV);
        input_views[i] = create_view(inputs[i], input_format);
    }
    // Flows: [session] forward (this frame to the last), [2 + session] backward (the reverse).
    backward = properties.bidirectionalFlowSupported != VK_FALSE;
    for (size_t i = 0; i < (backward ? 4U : 2U); ++i) {
        flows[i] = create_image(flow_extent, flow_format, VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV);
        flow_views[i] = create_view(flows[i], flow_format);
        flow_copies[i] = allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R16G16_SNORM,
            .extent = {flow_extent.width, flow_extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_CONCURRENT,
            .queueFamilyIndexCount = static_cast<u32>(families.size()),
            .pQueueFamilyIndices = families.data(),
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        flow_copy_views[i] = create_view(flow_copies[i], VK_FORMAT_R16G16_SNORM);
    }

    const VkOpticalFlowPerformanceLevelNV level = [] {
        switch (Settings::values.frame_gen_flow_quality.GetValue()) {
        case Settings::FrameGenFlowQuality::Fast:
            return VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_FAST_NV;
        case Settings::FrameGenFlowQuality::Best:
            return VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_SLOW_NV;
        default:
            return VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_MEDIUM_NV;
        }
    }();
    const VkDevice logical = *device.GetLogical();
    for (size_t i = 0; i < 2; ++i) {
        VkOpticalFlowSessionCreateInfoNV session_info{
            .sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_SESSION_CREATE_INFO_NV,
            .pNext = nullptr,
            .width = input_extent.width,
            .height = input_extent.height,
            .imageFormat = input_format,
            .flowVectorFormat = flow_format,
            .costFormat = VK_FORMAT_UNDEFINED,
            .outputGridSize = grid,
            .hintGridSize = 0,
            .performanceLevel = level,
            .flags = backward ? VkOpticalFlowSessionCreateFlagsNV{
                                    VK_OPTICAL_FLOW_SESSION_CREATE_BOTH_DIRECTIONS_BIT_NV}
                              : VkOpticalFlowSessionCreateFlagsNV{},
        };
        vk::Check(dld.vkCreateOpticalFlowSessionNV(logical, &session_info, nullptr, &sessions[i]));
        // Session i computes the flow from input i (this frame) to the other input (the last).
        const std::array bindings{
            std::pair{VK_OPTICAL_FLOW_SESSION_BINDING_POINT_INPUT_NV, *input_views[i]},
            std::pair{VK_OPTICAL_FLOW_SESSION_BINDING_POINT_REFERENCE_NV, *input_views[1 - i]},
            std::pair{VK_OPTICAL_FLOW_SESSION_BINDING_POINT_FLOW_VECTOR_NV, *flow_views[i]},
        };
        for (const auto& [point, view] : bindings) {
            vk::Check(dld.vkBindOpticalFlowSessionImageNV(logical, sessions[i], point, view,
                                                          VK_IMAGE_LAYOUT_GENERAL));
        }
        if (backward) {
            vk::Check(dld.vkBindOpticalFlowSessionImageNV(
                logical, sessions[i], VK_OPTICAL_FLOW_SESSION_BINDING_POINT_BACKWARD_FLOW_VECTOR_NV,
                *flow_views[2 + i], VK_IMAGE_LAYOUT_GENERAL));
        }
    }

    command_pool = device.GetLogical().CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.GetOpticalFlowFamily(),
    });
    command_buffers = command_pool.Allocate(2);
    for (size_t i = 0; i < 2; ++i) {
        fences[i] = device.GetLogical().CreateFence({
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        });
        done[i] = device.GetLogical().CreateSemaphore();
    }
    const VkSemaphoreTypeCreateInfo timeline_type{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    flow_timeline = device.GetLogical().CreateSemaphore({
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timeline_type,
        .flags = 0,
    });
    valid = true;
    LOG_INFO(Render_Vulkan,
             "Optical flow: {}x{} frames, {}x{} input, {}x{} flow grid, both directions {}, "
             "performance level {}",
             extent.width, extent.height, input_extent.width, input_extent.height,
             flow_extent.width, flow_extent.height, backward, static_cast<int>(level));
}

NvOpticalFlow::~NvOpticalFlow() {
    for (const vk::Fence& fence : fences) {
        if (fence) {
            void(fence.Wait());
        }
    }
    const auto& dld = device.GetDispatchLoader();
    for (const VkOpticalFlowSessionNV session : sessions) {
        if (session != VK_NULL_HANDLE) {
            dld.vkDestroyOpticalFlowSessionNV(*device.GetLogical(), session, nullptr);
        }
    }
}

VkSemaphore NvOpticalFlow::Run(Scheduler& scheduler, VkImage frame) {
    const VkSemaphore timeline = scheduler.GetMasterSemaphore().TimelineHandle();
    if (timeline == VK_NULL_HANDLE) {
        // The optical flow queue waits for the copy through the scheduler's timeline.
        valid = false;
        return VK_NULL_HANDLE;
    }
    const u32 index = next;
    next = 1 - index;
    // The last run reads this input as its reference: the copy below must not overwrite it
    // before that run is done (nothing on the graphics queue waits for the optical flow).
    void(fences[1 - index].Wait());
    const bool first_layouts = !initialized_layouts;
    initialized_layouts = true;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, frame, index, first_layouts](vk::CommandBuffer cmdbuf) {
        if (first_layouts) {
            std::vector<VkImageMemoryBarrier> init;
            for (const vk::Image& image : inputs) {
                init.push_back(Barrier(*image, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_GENERAL));
            }
            for (size_t i = 0; i < (backward ? 4U : 2U); ++i) {
                init.push_back(Barrier(*flows[i], 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_GENERAL));
                init.push_back(Barrier(*flow_copies[i], 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_GENERAL));
            }
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, init);
        }
        const std::array before{
            Barrier(frame,
                    VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);
        const VkImageBlit region{
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{0, 0, 0},
                           {static_cast<s32>(extent.width), static_cast<s32>(extent.height), 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets = {{0, 0, 0},
                           {static_cast<s32>(input_extent.width),
                            static_cast<s32>(input_extent.height), 1}},
        };
        cmdbuf.BlitImage(frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *inputs[index],
                         VK_IMAGE_LAYOUT_GENERAL, region, VK_FILTER_LINEAR);
        const std::array after{
            Barrier(frame, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               0, {}, {}, after);
    });
    if (!primed) {
        primed = true;
        return VK_NULL_HANDLE;
    }

    // The timeline value of this submission: the optical flow queue waits for the copy.
    const u64 copied = scheduler.Flush();
    const vk::Fence& fence = fences[index];
    void(fence.Wait());
    fence.Reset();

    const auto& dld = device.GetDispatchLoader();
    const VkCommandBuffer raw = command_buffers[index];
    vk::CommandBuffer cmdbuf{raw, dld};
    cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    VkOpticalFlowExecuteInfoNV execute{
        .sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_EXECUTE_INFO_NV,
        .pNext = nullptr,
        .flags = 0,
        .regionCount = 0,
        .pRegions = nullptr,
    };
    dld.vkCmdOpticalFlowExecuteNV(raw, sessions[index], &execute);
    cmdbuf.End();

    const VkSemaphore signal = use_timeline ? *flow_timeline : *done[index];
    const u64 wait_value = copied;
    const u64 signal_value = use_timeline ? ++flow_value : 0;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
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
        .pWaitSemaphores = &timeline,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &raw,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &signal,
    };
    vk::Check(device.GetOpticalFlowQueue().Submit(submit, *fence));
    last = index;
    return signal;
}

void NvOpticalFlow::RecordFlowCopy(vk::CommandBuffer cmdbuf, u32 slot) const {
    const size_t count = backward ? 2 : 1;
    std::array<VkImageMemoryBarrier, 2> before{};
    std::array<VkImageMemoryBarrier, 2> after{};
    for (size_t direction = 0; direction < count; ++direction) {
        const VkImage destination = *flow_copies[direction * 2 + slot];
        before[direction] = Barrier(destination, VK_ACCESS_SHADER_READ_BIT,
                                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_IMAGE_LAYOUT_GENERAL);
        after[direction] = Barrier(destination, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                   VK_IMAGE_LAYOUT_GENERAL);
    }
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, {}, {}, std::span{before.data(), count});
    const VkImageCopy region{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {flow_extent.width, flow_extent.height, 1},
    };
    for (size_t direction = 0; direction < count; ++direction) {
        cmdbuf.CopyImage(*flows[direction * 2 + slot], VK_IMAGE_LAYOUT_GENERAL,
                         *flow_copies[direction * 2 + slot], VK_IMAGE_LAYOUT_GENERAL, region);
    }
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, {}, {}, std::span{after.data(), count});
}

} // namespace Vulkan
