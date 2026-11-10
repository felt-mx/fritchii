// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <tuple>

#include "common/settings.h"
#include "video_core/host_shaders/full_screen_triangle_vert_spv.h"
#include "video_core/host_shaders/vulkan_present_hdr_frag_spv.h"
#include "video_core/renderer_vulkan/present/hdr_output_pass.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {

/// full_screen_triangle.vert's tex_scale and tex_offset, then vulkan_present_hdr.frag's block.
struct PushConstants {
    std::array<f32, 2> tex_scale{1.0f, 1.0f};
    std::array<f32, 2> tex_offset{0.0f, 0.0f};
    u32 source{}; ///< 0: SDR, 1: HDR10
    u32 target{}; ///< SwapchainOutput
    f32 paper_white{};
    f32 peak{};
};
static_assert(sizeof(PushConstants) == 32);

constexpr VkShaderStageFlags PushConstantStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

vk::RenderPass CreateSwapchainRenderPass(const Device& device, VkFormat format) {
    const VkAttachmentDescription attachment{
        .flags = 0,
        .format = format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    constexpr VkAttachmentReference color_attachment_ref{
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    const VkSubpassDescription subpass{
        .flags = 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment_ref,
        .pResolveAttachments = nullptr,
        .pDepthStencilAttachment = nullptr,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments = nullptr,
    };
    // The layout transition waits for the acquire semaphore, which is waited on at this stage.
    constexpr VkSubpassDependency dependency{
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dependencyFlags = 0,
    };
    return device.GetLogical().CreateRenderPass(VkRenderPassCreateInfo{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    });
}

} // Anonymous namespace

HdrOutputPass::HdrOutputPass(const Device& device, size_t frame_count) {
    descriptor_set_layout =
        CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
    descriptor_pool = CreateWrappedDescriptorPool(device, frame_count, frame_count);
    const std::vector layouts(frame_count, *descriptor_set_layout);
    descriptor_sets = CreateWrappedDescriptorSets(descriptor_pool, layouts);

    const VkPushConstantRange range{
        .stageFlags = PushConstantStages,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    pipeline_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = descriptor_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    });
    sampler = CreateWrappedSampler(device, VK_FILTER_LINEAR);
    vertex_shader = BuildShader(device, FULL_SCREEN_TRIANGLE_VERT_SPV);
    fragment_shader = BuildShader(device, VULKAN_PRESENT_HDR_FRAG_SPV);
}

HdrOutputPass::~HdrOutputPass() = default;

void HdrOutputPass::UpdateTargets(const Device& device, const Swapchain& swapchain) {
    const VkFormat format = swapchain.GetImageFormat();
    if (format != target_format) {
        target_format = format;
        render_pass = CreateSwapchainRenderPass(device, format);
        pipeline = CreateWrappedPipeline(device, render_pass, pipeline_layout,
                                         std::tie(vertex_shader, fragment_shader));
        target_generation = 0;
    }
    if (target_generation == swapchain.GetGeneration()) {
        return;
    }
    // The swapchain was recreated, after a device wait: nothing uses the old views any more.
    target_generation = swapchain.GetGeneration();
    framebuffers.clear();
    image_views.clear();
    const VkExtent2D extent = swapchain.GetExtent();
    for (size_t i = 0; i < swapchain.GetImageCount(); ++i) {
        image_views.push_back(device.GetLogical().CreateImageView(VkImageViewCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = swapchain.GetImageIndex(i),
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .components =
                {
                    .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .a = VK_COMPONENT_SWIZZLE_IDENTITY,
                },
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        }));
        const VkImageView view = *image_views.back();
        framebuffers.push_back(device.GetLogical().CreateFramebuffer(VkFramebufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .renderPass = *render_pass,
            .attachmentCount = 1,
            .pAttachments = &view,
            .width = extent.width,
            .height = extent.height,
            .layers = 1,
        }));
    }
}

void HdrOutputPass::Draw(const Device& device, vk::CommandBuffer cmdbuf, const Frame& frame,
                         const Swapchain& swapchain) {
    UpdateTargets(device, swapchain);

    // The frame's previous copy finished before it was handed out again (present_done).
    const VkDescriptorSet set = descriptor_sets[frame.index];
    std::vector<VkDescriptorImageInfo> image_infos;
    image_infos.reserve(1);
    const VkWriteDescriptorSet write =
        CreateWriteDescriptorSet(image_infos, *sampler, *frame.image_view, set, 0);
    device.GetLogical().UpdateDescriptorSets(std::array{write}, {});

    const PushConstants push{
        .source = frame.pq_content ? 1U : 0U,
        .target = static_cast<u32>(swapchain.GetOutput()),
        .paper_white = static_cast<f32>(Settings::values.hdr_paper_white.GetValue()),
        .peak = static_cast<f32>(Settings::values.hdr_peak_luminance.GetValue()),
    };
    BeginRenderPass(cmdbuf, *render_pass, *framebuffers[swapchain.GetImageIndex()],
                    swapchain.GetExtent());
    cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline);
    cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline_layout, 0, set, {});
    cmdbuf.PushConstants(*pipeline_layout, PushConstantStages, push);
    cmdbuf.Draw(3, 1, 0, 0);
    cmdbuf.EndRenderPass();
}

} // namespace Vulkan
