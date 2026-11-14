// SPDX-License-Identifier: MIT
// The "app" side of the off-headset test: a minimal OpenXR + Vulkan client that goes through the real
// Khronos loader (so implicit-layer discovery, disable_environment and the dispatch chain are the real
// ones), renders a known pattern into a swapchain, submits it, and reads the result back.
//
//   test_app --name <applicationName> [--format 37|43] [--layers 1|2] [--quad] [--no-session] [--frames N]
//            [--fov none|fb|conf|conf-nolevel|conf-eye|conf-odd]
//
// --fov: how the app uses XR_FB_foveation. none = not at all; fb = XR_FB_foveation without _configuration,
// profile with no level struct; conf = _configuration too, level LOW + dynamic (what a typical Godot project asks);
// conf-nolevel = _configuration but no level struct; conf-eye = level + META eye-tracked struct;
// conf-odd = an unrecognised struct before the level struct. The runtime-side view of the profile is
// printed as "fov", and the extensions the runtime received as "runtime_exts".
//
// Prints one JSON object: the recommended view size the app saw, the usage flags the runtime received,
// and per array layer the middle row's red channel and alpha after the last release.
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define XRC(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { fprintf(stderr, "%s -> %d\n", #x, (int)r_); exit(2); } } while (0)
#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s -> %d\n", #x, (int)r_); exit(3); } } while (0)

static const uint32_t W = 48, H = 8;
// a blurred step, 8-bit: flat 64 on the left, flat 192 on the right, an S-shaped edge in between
static const uint8_t EDGE[] = {64, 64, 66, 72, 88, 112, 144, 168, 184, 190, 192, 192};
static uint8_t pattern(uint32_t x, uint32_t layer) {
    int i = (int)x - 18;  // edge occupies x = 18..29
    uint8_t v = i < 0 ? EDGE[0] : (i >= (int)sizeof EDGE ? EDGE[sizeof EDGE - 1] : EDGE[i]);
    return layer == 0 ? v : (uint8_t)(256 - v);   // layer 1: the mirror edge (192 -> 64)
}

struct Vk {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue q;
    uint32_t fam = 0;
    VkCommandPool pool;
    VkPhysicalDeviceMemoryProperties mp;
};

static uint32_t memtype(Vk &v, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < v.mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (v.mp.memoryTypes[i].propertyFlags & want) == want) return i;
    exit(4);
}

static VkCommandBuffer begin(Vk &v) {
    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, v.pool,
                                  VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer c;
    VKC(vkAllocateCommandBuffers(v.dev, &a, &c));
    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(c, &b);
    return c;
}
static void submit(Vk &v, VkCommandBuffer c) {
    vkEndCommandBuffer(c);
    VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    s.commandBufferCount = 1;
    s.pCommandBuffers = &c;
    VKC(vkQueueSubmit(v.q, 1, &s, VK_NULL_HANDLE));
    vkQueueWaitIdle(v.q);
    vkFreeCommandBuffers(v.dev, v.pool, 1, &c);
}
static void barrier(VkCommandBuffer c, VkImage img, uint32_t layers, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

struct Buf { VkBuffer b; VkDeviceMemory m; void *p; };
static Buf mkbuf(Vk &v, VkDeviceSize n, VkBufferUsageFlags u) {
    Buf r;
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = n;
    bc.usage = u;
    VKC(vkCreateBuffer(v.dev, &bc, nullptr, &r.b));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(v.dev, r.b, &mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size,
                            memtype(v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKC(vkAllocateMemory(v.dev, &ma, nullptr, &r.m));
    vkBindBufferMemory(v.dev, r.b, r.m, 0);
    vkMapMemory(v.dev, r.m, 0, n, 0, &r.p);
    return r;
}

int main(int argc, char **argv) {
    std::string name = "xrtweak-test";
    int64_t format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t layers = 1, frames = 3;
    bool quad = false, nosession = false;
    std::string fov = "none";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--name") name = argv[++i];
        else if (a == "--format") format = atoll(argv[++i]);
        else if (a == "--layers") layers = (uint32_t)atoi(argv[++i]);
        else if (a == "--frames") frames = (uint32_t)atoi(argv[++i]);
        else if (a == "--quad") quad = true;
        else if (a == "--no-session") nosession = true;
        else if (a == "--fov") fov = argv[++i];
    }

    // ---- OpenXR instance through the real loader
    std::vector<const char *> exts = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    if (fov != "none") {
        exts.push_back(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME);
        exts.push_back(XR_FB_FOVEATION_EXTENSION_NAME);
    }
    if (fov.rfind("conf", 0) == 0) exts.push_back(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", name.c_str());
    snprintf(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "xr-tweak-test");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.enabledExtensionNames = exts.data();
    XrInstance xi;
    XRC(xrCreateInstance(&ici, &xi));
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    XRC(xrGetSystem(xi, &sgi, &sys));
    uint32_t nv = 0;
    XRC(xrEnumerateViewConfigurationViews(xi, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, nullptr));
    std::vector<XrViewConfigurationView> views(nv, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XRC(xrEnumerateViewConfigurationViews(xi, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nv, &nv, views.data()));
    printf("{\"recommended\": [%u, %u], \"max\": [%u, %u]", views[0].recommendedImageRectWidth,
           views[0].recommendedImageRectHeight, views[0].maxImageRectWidth, views[0].maxImageRectHeight);
    if (nosession) {
        printf("}\n");
        xrDestroyInstance(xi);
        return 0;
    }

    // ---- Vulkan (lavapipe in the test image)
    Vk v;
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo vci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vci.pApplicationInfo = &ai;
    VKC(vkCreateInstance(&vci, nullptr, &v.inst));
    uint32_t np = 1;
    vkEnumeratePhysicalDevices(v.inst, &np, &v.phys);
    vkGetPhysicalDeviceMemoryProperties(v.phys, &v.mp);
    float prio = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, v.fam, 1, &prio};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VKC(vkCreateDevice(v.phys, &dci, nullptr, &v.dev));
    vkGetDeviceQueue(v.dev, v.fam, 0, &v.q);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, v.fam};
    vkCreateCommandPool(v.dev, &pci, nullptr, &v.pool);

    XrGraphicsBindingVulkanKHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    gb.instance = v.inst;
    gb.physicalDevice = v.phys;
    gb.device = v.dev;
    gb.queueFamilyIndex = v.fam;
    gb.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = sys;
    XrSession s;
    XRC(xrCreateSession(xi, &sci, &s));
    void *stub = dlopen(getenv("STUB_RUNTIME_SO"), RTLD_NOW | RTLD_NOLOAD);
    if (stub)
        if (auto f = (const char *(*)())dlsym(stub, "stub_enabled_exts")) printf(", \"runtime_exts\": \"%s\"", f());

    if (fov != "none") {
        PFN_xrCreateFoveationProfileFB create = nullptr;
        PFN_xrDestroyFoveationProfileFB destroy = nullptr;
        XRC(xrGetInstanceProcAddr(xi, "xrCreateFoveationProfileFB", (PFN_xrVoidFunction *)&create));
        XRC(xrGetInstanceProcAddr(xi, "xrDestroyFoveationProfileFB", (PFN_xrVoidFunction *)&destroy));
        // const, as an app might keep it: the layer must not write into these
        static const XrFoveationEyeTrackedProfileCreateInfoMETA eye{XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META};
        static const XrFoveationLevelProfileCreateInfoFB lvl_eye{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB,
                                                                 (void *)&eye, XR_FOVEATION_LEVEL_LOW_FB, 0.f,
                                                                 XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB};
        static const XrFoveationLevelProfileCreateInfoFB lvl{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB, nullptr,
                                                             XR_FOVEATION_LEVEL_LOW_FB, 0.f,
                                                             XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB};
        // an unrelated struct type the layer does not know, placed before the level struct
        static const XrBaseInStructure odd{XR_TYPE_SPACE_LOCATION, (const XrBaseInStructure *)&lvl};
        XrFoveationProfileCreateInfoFB pci{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
        if (fov == "conf") pci.next = (void *)&lvl;
        else if (fov == "conf-eye") pci.next = (void *)&lvl_eye;
        else if (fov == "conf-odd") pci.next = (void *)&odd;
        XrFoveationProfileFB prof;
        XrResult r = create(s, &pci, &prof);
        printf(", \"fov_result\": %d", (int)r);
        if (XR_SUCCEEDED(r)) destroy(prof);
        const char *fj = "null";
        if (stub)
            if (auto f = (const char *(*)())dlsym(stub, "stub_fov_json")) fj = f();
        printf(", \"fov\": %s, \"app_struct_level\": %d", fj, (int)lvl.level);
    }

    XrSwapchainCreateInfo swci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swci.format = format;
    swci.sampleCount = 1;
    swci.width = W;
    swci.height = H;
    swci.faceCount = 1;
    swci.arraySize = layers;
    swci.mipCount = 1;
    XrSwapchain sc;
    XRC(xrCreateSwapchain(s, &swci, &sc));
    uint64_t usage = 0;
    if (stub)
        if (auto f = (uint64_t(*)())dlsym(stub, "stub_last_usage")) usage = f();
    printf(", \"runtime_usage\": %llu, \"app_usage\": %llu", (unsigned long long)usage,
           (unsigned long long)swci.usageFlags);

    uint32_t ni = 0;
    XRC(xrEnumerateSwapchainImages(sc, 0, &ni, nullptr));
    std::vector<XrSwapchainImageVulkanKHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    XRC(xrEnumerateSwapchainImages(sc, ni, &ni, (XrSwapchainImageBaseHeader *)imgs.data()));

    VkDeviceSize n = (VkDeviceSize)W * H * 4 * layers;
    Buf up = mkbuf(v, n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT), down = mkbuf(v, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto *px = (uint8_t *)up.p;
    for (uint32_t l = 0; l < layers; ++l)
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                uint8_t *p = px + ((l * H + y) * W + x) * 4;
                p[0] = p[1] = p[2] = pattern(x, l);
                p[3] = (uint8_t)(100 + x);   // alpha varies so a layer that rewrites it would show
            }

    uint32_t last = 0;
    for (uint32_t f = 0; f < frames; ++f) {
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XRC(xrWaitFrame(s, nullptr, &fs));
        XRC(xrBeginFrame(s, nullptr));
        XrSwapchainImageAcquireInfo aci{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XRC(xrAcquireSwapchainImage(sc, &aci, &last));
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        XRC(xrWaitSwapchainImage(sc, &wi));
        VkCommandBuffer c = begin(v);
        barrier(c, imgs[last].image, layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
        bic.imageExtent = {W, H, 1};
        vkCmdCopyBufferToImage(c, up.b, imgs[last].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        barrier(c, imgs[last].image, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        submit(v, c);
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        XRC(xrReleaseSwapchainImage(sc, &ri));

        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (int e = 0; e < 2; ++e) {
            pv[e].pose.orientation.w = 1;
            pv[e].subImage.swapchain = sc;
            pv[e].subImage.imageRect = {{0, 0}, {(int32_t)W, (int32_t)H}};
            pv[e].subImage.imageArrayIndex = layers > 1 ? (uint32_t)e : 0;
        }
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        proj.viewCount = 2;
        proj.views = pv;
        XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.subImage = pv[0].subImage;
        q.pose.orientation.w = 1;
        q.size = {1, 1};
        const XrCompositionLayerBaseHeader *lay = quad ? (const XrCompositionLayerBaseHeader *)&q
                                                       : (const XrCompositionLayerBaseHeader *)&proj;
        XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
        fe.displayTime = fs.predictedDisplayTime;
        fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fe.layerCount = 1;
        fe.layers = &lay;
        XRC(xrEndFrame(s, &fe));
    }
    vkQueueWaitIdle(v.q);
    VkCommandBuffer c = begin(v);
    barrier(c, imgs[last].image, layers, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy bic{};
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    bic.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(c, imgs[last].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.b, 1, &bic);
    barrier(c, imgs[last].image, layers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    submit(v, c);

    auto *out = (uint8_t *)down.p;
    printf(", \"layers\": [");
    for (uint32_t l = 0; l < layers; ++l) {
        printf("%s{\"in\": [", l ? ", " : "");
        for (uint32_t x = 0; x < W; ++x) printf("%s%u", x ? "," : "", pattern(x, l));
        printf("], \"out\": [");
        uint32_t y = H / 2;
        for (uint32_t x = 0; x < W; ++x) printf("%s%u", x ? "," : "", out[((l * H + y) * W + x) * 4]);
        printf("], \"rows_equal\": %s, \"gb_equal_r\": %s, \"alpha_in\": [", [&] {
            for (uint32_t yy = 0; yy < H; ++yy)
                if (memcmp(out + ((l * H + yy) * W) * 4, out + ((l * H + y) * W) * 4, W * 4)) return "false";
            return "true";
        }(), [&] {
            for (uint32_t x = 0; x < W; ++x) {
                uint8_t *p = out + ((l * H + y) * W + x) * 4;
                if (p[1] != p[0] || p[2] != p[0]) return "false";
            }
            return "true";
        }());
        for (uint32_t x = 0; x < W; ++x) printf("%s%u", x ? "," : "", 100 + x);
        printf("], \"alpha_out\": [");
        for (uint32_t x = 0; x < W; ++x) printf("%s%u", x ? "," : "", out[((l * H + y) * W + x) * 4 + 3]);
        printf("]}");
    }
    printf("]}\n");
    XRC(xrDestroySwapchain(sc));
    XRC(xrDestroySession(s));
    XRC(xrDestroyInstance(xi));
    vkDestroyDevice(v.dev, nullptr);
    vkDestroyInstance(v.inst, nullptr);
    return 0;
}
