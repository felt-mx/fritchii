// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Swapchain;
struct Frame;

/// Copies a present frame into the swapchain's current image and converts its colour on the way
/// (vulkan_present_hdr.frag): SDR or HDR10 frames to an HDR10 or scRGB swapchain, HDR10 frames to
/// an SDR one. Used in place of the plain blit wherever a frame reaches the swapchain.
class HdrOutputPass {
public:
    explicit HdrOutputPass(const Device& device, size_t frame_count);
    ~HdrOutputPass();

    /// Records the copy; the swapchain image ends in VK_IMAGE_LAYOUT_PRESENT_SRC_KHR. The frame
    /// image is sampled in VK_IMAGE_LAYOUT_GENERAL.
    void Draw(const Device& device, vk::CommandBuffer cmdbuf, const Frame& frame,
              const Swapchain& swapchain);

private:
    /// (Re)creates the render pass and pipeline for the swapchain's format, and the image views
    /// and framebuffers of its images.
    void UpdateTargets(const Device& device, const Swapchain& swapchain);

    vk::DescriptorSetLayout descriptor_set_layout;
    vk::DescriptorPool descriptor_pool;
    vk::DescriptorSets descriptor_sets; ///< One per present frame
    vk::PipelineLayout pipeline_layout;
    vk::Sampler sampler;
    vk::ShaderModule vertex_shader;
    vk::ShaderModule fragment_shader;

    VkFormat target_format{VK_FORMAT_UNDEFINED};
    vk::RenderPass render_pass;
    vk::Pipeline pipeline;

    u64 target_generation{}; ///< Swapchain generation the views and framebuffers belong to
    std::vector<vk::ImageView> image_views;
    std::vector<vk::Framebuffer> framebuffers;
};

} // namespace Vulkan
