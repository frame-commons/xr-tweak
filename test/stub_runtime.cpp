// SPDX-License-Identifier: MIT
// A stub OpenXR runtime for testing xr-tweak's dispatch chain off-headset: the real Khronos loader
// loads it (XR_RUNTIME_JSON), and the real loader loads the layer in front of it as an implicit layer.
// Vulkan is real (lavapipe): swapchain images are VkImages created on the app's device with exactly
// the usage the swapchain create info asked for, so a layer that forgets to add TRANSFER_SRC fails
// validation-free but visibly (vkCmdCopyImage on lavapipe does not check, so the test also reads
// stub_last_usage() to assert the bit arrived).
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace {
struct Chain {
    XrSwapchainCreateInfo info{};
    std::vector<VkImage> images;
    std::vector<VkDeviceMemory> mem;
    uint32_t next = 0;
    std::deque<uint32_t> acquired;
};
VkDevice g_dev = VK_NULL_HANDLE;
VkPhysicalDevice g_phys = VK_NULL_HANDLE;
std::map<XrSwapchain, Chain> g_chains;
uint64_t g_handle = 0x1000;
XrSwapchainUsageFlags g_last_usage = 0;
uint32_t g_end_frames = 0, g_proj_layers = 0;
std::string g_enabled_exts;   // what xrCreateInstance received, comma-separated
bool g_fov_config = false;    // XR_FB_foveation_configuration enabled on the instance
std::string g_fov_json = "null";   // what the last xrCreateFoveationProfileFB received

// The extension list. STUB_NO_FOV_CONFIG=1 models a runtime without XR_FB_foveation_configuration.
std::vector<const char *> stub_exts() {
    std::vector<const char *> e = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
                                   XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME, XR_FB_FOVEATION_EXTENSION_NAME};
    if (!getenv("STUB_NO_FOV_CONFIG")) e.push_back(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
    return e;
}

template <typename H> H mk() { return (H)(++g_handle); }

XrResult XRAPI_CALL EnumerateInstanceExtensionProperties(const char *, uint32_t cap, uint32_t *n,
                                                         XrExtensionProperties *p) {
    auto ext = stub_exts();
    *n = (uint32_t)ext.size();
    if (!cap) return XR_SUCCESS;
    if (cap < ext.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    for (size_t i = 0; i < ext.size(); ++i) {
        strcpy(p[i].extensionName, ext[i]);
        p[i].extensionVersion = 1;
    }
    return XR_SUCCESS;
}
// Like a real runtime: refuse an extension it does not have, and enforce the FB dependency rules
// (the strings in SteamVR's vrclient: "attempted to enable XR_FB_foveation_configuration without enabling
// required XR_FB_foveation", and XR_FB_foveation needs XR_FB_swapchain_update_state).
XrResult XRAPI_CALL CreateInstance(const XrInstanceCreateInfo *ci, XrInstance *i) {
    auto ext = stub_exts();
    bool fb = false, upd = false, conf = false;
    g_enabled_exts.clear();
    for (uint32_t k = 0; k < ci->enabledExtensionCount; ++k) {
        const char *e = ci->enabledExtensionNames[k];
        bool known = false;
        for (auto *x : ext) known |= !strcmp(x, e);
        if (!known) return XR_ERROR_EXTENSION_NOT_PRESENT;
        fb |= !strcmp(e, XR_FB_FOVEATION_EXTENSION_NAME);
        upd |= !strcmp(e, XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME);
        conf |= !strcmp(e, XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
        g_enabled_exts += (g_enabled_exts.empty() ? "" : ",") + std::string(e);
    }
    if ((fb && !upd) || (conf && !fb)) return XR_ERROR_VALIDATION_FAILURE;
    g_fov_config = conf;
    *i = mk<XrInstance>();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL CreateFoveationProfileFB(XrSession, const XrFoveationProfileCreateInfoFB *ci, XrFoveationProfileFB *out) {
    if (!ci || ci->type != XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB) return XR_ERROR_VALIDATION_FAILURE;
    int level = -1, dyn = -1, nlevel = 0, eye = 0, other = 0;
    for (auto *p = (const XrBaseInStructure *)ci->next; p; p = p->next) {
        if (p->type == XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB) {
            auto *l = (const XrFoveationLevelProfileCreateInfoFB *)p;
            level = (int)l->level;
            dyn = (int)l->dynamic;
            ++nlevel;
        } else if (p->type == XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META) ++eye;
        else ++other;
    }
    // valid usage: the level struct needs XR_FB_foveation_configuration enabled
    if (nlevel && !g_fov_config) return XR_ERROR_VALIDATION_FAILURE;
    char b[160];
    snprintf(b, sizeof b, "{\"level\": %d, \"dynamic\": %d, \"n_level\": %d, \"eye\": %d, \"other\": %d}", level, dyn,
             nlevel, eye, other);
    g_fov_json = b;
    *out = mk<XrFoveationProfileFB>();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL DestroyFoveationProfileFB(XrFoveationProfileFB) { return XR_SUCCESS; }
XrResult XRAPI_CALL DestroyInstance(XrInstance) { return XR_SUCCESS; }
XrResult XRAPI_CALL GetInstanceProperties(XrInstance, XrInstanceProperties *p) {
    p->runtimeVersion = XR_MAKE_VERSION(0, 0, 1);
    strcpy(p->runtimeName, "xr-tweak stub runtime");
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetSystem(XrInstance, const XrSystemGetInfo *, XrSystemId *s) { *s = 1; return XR_SUCCESS; }
XrResult XRAPI_CALL EnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t cap,
                                                    uint32_t *n, XrViewConfigurationView *v) {
    *n = 2;
    if (!cap) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < 2; ++i) {
        v[i].recommendedImageRectWidth = 1000;
        v[i].recommendedImageRectHeight = 800;
        v[i].maxImageRectWidth = 2000;
        v[i].maxImageRectHeight = 1600;
        v[i].recommendedSwapchainSampleCount = v[i].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetVulkanGraphicsRequirementsKHR(XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR *r) {
    r->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    r->maxApiVersionSupported = XR_MAKE_VERSION(1, 3, 0);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL CreateSession(XrInstance, const XrSessionCreateInfo *info, XrSession *s) {
    for (auto *p = (const XrBaseInStructure *)info->next; p; p = p->next)
        if (p->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
            auto *b = (const XrGraphicsBindingVulkanKHR *)p;
            g_dev = b->device;
            g_phys = b->physicalDevice;
        }
    if (!g_dev) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    *s = mk<XrSession>();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL DestroySession(XrSession) { return XR_SUCCESS; }
XrResult XRAPI_CALL EnumerateSwapchainFormats(XrSession, uint32_t cap, uint32_t *n, int64_t *f) {
    *n = 2;
    if (cap >= 2) { f[0] = VK_FORMAT_R8G8B8A8_SRGB; f[1] = VK_FORMAT_R8G8B8A8_UNORM; }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL CreateSwapchain(XrSession, const XrSwapchainCreateInfo *ci, XrSwapchain *out) {
    g_last_usage = ci->usageFlags;
    Chain c;
    c.info = *ci;
    VkImageUsageFlags u = 0;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) u |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) u |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    for (int i = 0; i < 3; ++i) {
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = (VkFormat)ci->format;
        ic.extent = {ci->width, ci->height, 1};
        ic.mipLevels = 1;
        ic.arrayLayers = ci->arraySize;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = u;
        VkImage img;
        if (vkCreateImage(g_dev, &ic, nullptr, &img) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(g_dev, img, &mr);
        uint32_t t = 0;
        while (!(mr.memoryTypeBits & (1u << t))) ++t;
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size, t};
        VkDeviceMemory m;
        vkAllocateMemory(g_dev, &ma, nullptr, &m);
        vkBindImageMemory(g_dev, img, m, 0);
        c.images.push_back(img);
        c.mem.push_back(m);
    }
    *out = mk<XrSwapchain>();
    g_chains[*out] = c;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL DestroySwapchain(XrSwapchain s) {
    auto &c = g_chains[s];
    vkDeviceWaitIdle(g_dev);
    for (size_t i = 0; i < c.images.size(); ++i) {
        vkDestroyImage(g_dev, c.images[i], nullptr);
        vkFreeMemory(g_dev, c.mem[i], nullptr);
    }
    g_chains.erase(s);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL EnumerateSwapchainImages(XrSwapchain s, uint32_t cap, uint32_t *n, XrSwapchainImageBaseHeader *b) {
    auto &c = g_chains[s];
    *n = (uint32_t)c.images.size();
    if (!cap) return XR_SUCCESS;
    auto *v = (XrSwapchainImageVulkanKHR *)b;
    for (uint32_t i = 0; i < *n && i < cap; ++i) v[i].image = c.images[i];
    return XR_SUCCESS;
}
XrResult XRAPI_CALL AcquireSwapchainImage(XrSwapchain s, const XrSwapchainImageAcquireInfo *, uint32_t *idx) {
    auto &c = g_chains[s];
    *idx = c.next;
    c.acquired.push_back(c.next);
    c.next = (c.next + 1) % c.images.size();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL WaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo *) { return XR_SUCCESS; }
XrResult XRAPI_CALL ReleaseSwapchainImage(XrSwapchain s, const XrSwapchainImageReleaseInfo *) {
    auto &c = g_chains[s];
    if (c.acquired.empty()) return XR_ERROR_CALL_ORDER_INVALID;
    c.acquired.pop_front();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL WaitFrame(XrSession, const XrFrameWaitInfo *, XrFrameState *st) {
    st->predictedDisplayTime = 1;
    st->predictedDisplayPeriod = 11111111;
    st->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL BeginFrame(XrSession, const XrFrameBeginInfo *) { return XR_SUCCESS; }
XrResult XRAPI_CALL EndFrame(XrSession, const XrFrameEndInfo *fi) {
    ++g_end_frames;
    for (uint32_t i = 0; i < fi->layerCount; ++i)
        if (fi->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) ++g_proj_layers;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL PollEvent(XrInstance, XrEventDataBuffer *) { return XR_EVENT_UNAVAILABLE; }
XrResult XRAPI_CALL ResultToString(XrInstance, XrResult r, char buf[XR_MAX_RESULT_STRING_SIZE]) {
    snprintf(buf, XR_MAX_RESULT_STRING_SIZE, "XR_RESULT_%d", (int)r);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL StructureTypeToString(XrInstance, XrStructureType t, char buf[XR_MAX_STRUCTURE_NAME_SIZE]) {
    snprintf(buf, XR_MAX_STRUCTURE_NAME_SIZE, "XR_TYPE_%d", (int)t);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL GetInstanceProcAddr(XrInstance, const char *name, PFN_xrVoidFunction *fn) {
#define F(n) if (!strcmp(name, "xr" #n)) { *fn = (PFN_xrVoidFunction)n; return XR_SUCCESS; }
    F(EnumerateInstanceExtensionProperties) F(CreateInstance) F(DestroyInstance) F(GetInstanceProperties)
    F(GetSystem) F(EnumerateViewConfigurationViews) F(CreateSession) F(DestroySession)
    F(EnumerateSwapchainFormats) F(CreateSwapchain) F(DestroySwapchain) F(EnumerateSwapchainImages)
    F(AcquireSwapchainImage) F(WaitSwapchainImage) F(ReleaseSwapchainImage) F(WaitFrame) F(BeginFrame)
    F(EndFrame) F(PollEvent) F(ResultToString) F(StructureTypeToString)
    F(CreateFoveationProfileFB) F(DestroyFoveationProfileFB)
    if (!strcmp(name, "xrGetVulkanGraphicsRequirementsKHR")) { *fn = (PFN_xrVoidFunction)GetVulkanGraphicsRequirementsKHR; return XR_SUCCESS; }
#undef F
    *fn = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
}  // namespace

EXPORT XrSwapchainUsageFlags stub_last_usage() { return g_last_usage; }
EXPORT uint32_t stub_end_frames() { return g_end_frames; }
EXPORT const char *stub_enabled_exts() { return g_enabled_exts.c_str(); }
EXPORT const char *stub_fov_json() { return g_fov_json.c_str(); }

EXPORT XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *li,
                                                             XrNegotiateRuntimeRequest *rr) {
    if (!li || !rr) return XR_ERROR_INITIALIZATION_FAILED;
    rr->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    rr->runtimeApiVersion = XR_CURRENT_API_VERSION > li->maxApiVersion ? li->maxApiVersion : XR_CURRENT_API_VERSION;
    rr->getInstanceProcAddr = GetInstanceProcAddr;
    return XR_SUCCESS;
}
