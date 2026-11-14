// SPDX-License-Identifier: MIT
// In-place RCAS on an OpenXR Vulkan swapchain image, run at xrReleaseSwapchainImage time on the
// app's own queue (XR_KHR_vulkan_enable: the runtime -- and so a layer -- may use that queue inside
// xrReleaseSwapchainImage, and the app must not touch it concurrently).
//
// Per release: copy image -> layer-owned intermediate (needs TRANSFER_SRC, which the layer adds to
// the app's swapchain usage), then one full-screen fragment pass per array layer that samples the
// intermediate and writes the swapchain image as a colour attachment. A fragment pass rather than
// compute because _SRGB formats are colour-attachment-capable everywhere and storage-capable almost
// nowhere; the image is left in COLOR_ATTACHMENT_OPTIMAL, the layout the app released it in.
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SharpenDevice;    // per XrSession: device, queue, pipelines
struct SharpenChain;     // per XrSwapchain: intermediate + framebuffers
struct SharpenDeviceDeleter { void operator()(SharpenDevice *) const; };
struct SharpenChainDeleter { void operator()(SharpenChain *) const; };   // waits for its own fences
using SharpenDevicePtr = std::unique_ptr<SharpenDevice, SharpenDeviceDeleter>;
using SharpenChainPtr = std::unique_ptr<SharpenChain, SharpenChainDeleter>;

struct SharpenDeviceInfo {
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t queueFamily;
    uint32_t queueIndex;
};

// Destroy every chain of a device before the device (the layer does: swapchains die before the session).
SharpenDevicePtr sharpen_device_create(const SharpenDeviceInfo &info, std::string *err);

SharpenChainPtr sharpen_chain_create(SharpenDevice *dev, VkFormat format, uint32_t width,
                                                   uint32_t height, uint32_t layers,
                                                   const std::vector<VkImage> &images, std::string *err);

// Sharpen images[index] in place. strength 0..1. Returns VK_SUCCESS or the failing call's result.
VkResult sharpen_apply(SharpenDevice *dev, SharpenChain *chain, uint32_t index, float strength, std::string *err);

// GPU time of each finished pass (ms, start to end of our command buffer, from timestamp queries),
// collected since the last call; empty if the queue family cannot time. A pass is read back when its
// image index comes round again, so the last few of a session are never counted.
std::vector<double> sharpen_chain_take_gpu_ms(SharpenChain *chain);

bool sharpen_format_is_srgb(VkFormat f);
