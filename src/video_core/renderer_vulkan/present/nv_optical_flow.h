// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// Motion between consecutive presented frames from NVIDIA's optical flow hardware
/// (VK_NV_optical_flow). It runs on the device's optical flow queue: each frame is copied into
/// one of two inputs on the graphics queue, the flow from it to the previous one is computed on
/// the optical flow queue, and the graphics work that reads the flow waits on a semaphore.
class NvOpticalFlow {
public:
    /// Throws vk::Exception or returns with IsValid() false when the hardware path is unusable.
    explicit NvOpticalFlow(const Device& device, MemoryAllocator& allocator, VkExtent2D extent);
    ~NvOpticalFlow();

    NvOpticalFlow(const NvOpticalFlow&) = delete;
    NvOpticalFlow& operator=(const NvOpticalFlow&) = delete;

    [[nodiscard]] bool IsValid() const {
        return valid;
    }

    /// Copies `frame` (in the general layout) and computes its flow to the previous frame. Returns
    /// the semaphore the next graphics submission must wait on, or a null handle for the first
    /// frame (nothing to compare with).
    [[nodiscard]] VkSemaphore Run(Scheduler& scheduler, VkImage frame);

    /// Runs signal a timeline semaphore instead (Run returns it; wait for LastFlowValue). A
    /// binary semaphore signalled on the optical flow queue and waited on a compute queue lost
    /// the device on NVIDIA.
    void UseTimeline() {
        use_timeline = true;
    }

    [[nodiscard]] u64 LastFlowValue() const {
        return flow_value;
    }

    /// Records, in the graphics submission that waits for the last Run, the copy of its flow to
    /// the image FlowView samples. Drivers sample R16G16_SFIXED5_NV as half floats, so the raw
    /// fixed-point words are read through an R16G16_SNORM image instead.
    void RecordFlowCopy(vk::CommandBuffer cmdbuf, u32 slot) const;

    /// The flow slot of the last Run, for RecordFlowCopy calls recorded on another thread.
    [[nodiscard]] u32 LastSlot() const {
        return last;
    }

    /// The flow of the last Run from the current to the previous frame, one vector per grid cell.
    /// Sampled values times FlowToPixels are pixels.
    [[nodiscard]] VkImageView FlowView() const {
        return *flow_copy_views[last];
    }

    /// The reverse flow of the last Run (from the previous frame to the current one, on the
    /// previous frame's pixels), or the forward one without bidirectional flow.
    [[nodiscard]] VkImageView BackwardFlowView() const {
        return *flow_copy_views[backward ? 2 + last : last];
    }

    [[nodiscard]] bool HasBackwardFlow() const {
        return backward;
    }

    static constexpr float FlowToPixels = 32767.0f / 32.0f;

    /// Output pixels per flow input pixel along x and y.
    [[nodiscard]] float InputScaleX() const {
        return static_cast<float>(extent.width) / static_cast<float>(input_extent.width);
    }
    [[nodiscard]] float InputScaleY() const {
        return static_cast<float>(extent.height) / static_cast<float>(input_extent.height);
    }

private:
    const Device& device;
    VkExtent2D extent{};
    VkExtent2D input_extent{};
    VkExtent2D flow_extent{};
    VkFormat input_format{};
    bool valid{};
    bool primed{};
    bool backward{};
    bool initialized_layouts{};
    u32 next{};
    u32 last{};

    std::array<vk::Image, 2> inputs;
    std::array<vk::ImageView, 2> input_views;
    std::array<vk::Image, 4> flows;
    std::array<vk::ImageView, 4> flow_views;
    std::array<vk::Image, 4> flow_copies;
    std::array<vk::ImageView, 4> flow_copy_views;
    std::array<VkOpticalFlowSessionNV, 2> sessions{};
    vk::CommandPool command_pool;
    vk::CommandBuffers command_buffers;
    std::array<vk::Fence, 2> fences;
    std::array<vk::Semaphore, 2> done;
    vk::Semaphore flow_timeline;
    u64 flow_value{};
    bool use_timeline{};
};

} // namespace Vulkan
