// SPDX-License-Identifier: MIT
// xr-tweak: a per-app OpenXR API layer for the Steam Frame (Linux aarch64; builds for x86_64 too).
//
// It does four things and passes everything else through untouched:
//   1. resolution override  -- rewrites recommendedImageRectWidth/Height in
//      xrEnumerateViewConfigurationViews (scale, or explicit width/height; clamped to the max rect)
//   2. RCAS sharpening      -- on every swapchain the app submits in a projection layer, in place, at
//      xrReleaseSwapchainImage, on the app's own Vulkan queue (src/sharpen.cpp)
//   3. foveation level      -- for apps that call XR_FB_foveation themselves: rewrites the level in
//      xrCreateFoveationProfileFB (off/low/medium/high), adding XR_FB_foveation_configuration when the app
//      did not. Default "valve" = untouched. Apps without XR_FB_foveation are out of reach of an OpenXR
//      layer (Valve's own fdm_injection layer is what foveates them); the start log line says which case
//      an app is, and where Valve's layer sits in the chain relative to ours.
//   4. a kill switch        -- XR_TWEAK_DISABLE=1 (the manifest's disable_environment: the loader then
//      never loads us) and enabled=0 per app (loaded, but every hook forwards untouched)
//
// Which swapchains get sharpened: only those named by an XrCompositionLayerProjection in a previous
// xrEndFrame. Quad layers (menus, text) and depth swapchains are never touched, and the first frame
// of a new swapchain is not sharpened (we do not know yet what it is for).
//
// Logging: one line per event of note to stderr ("xr-tweak: ...") and appended to
// $XDG_STATE_HOME/xr-tweak/xr-tweak.log (~/.local/state/...) -- which is how `xr-tweak apps`
// learns the applicationName to key a config section on. Nothing leaves the machine.
#define XR_USE_GRAPHICS_API_VULKAN
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "sharpen.h"

#define LAYER_NAME "XR_APILAYER_FRAMECOMMONS_xr_tweak"
#define EXPORT extern "C" __attribute__((visibility("default")))

namespace {

std::mutex g_mu;

struct Next {
    PFN_xrGetInstanceProcAddr gipa = nullptr;
    PFN_xrDestroyInstance DestroyInstance = nullptr;
    PFN_xrEnumerateViewConfigurationViews EnumerateViewConfigurationViews = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrDestroySession DestroySession = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
    PFN_xrCreateVulkanInstanceKHR CreateVulkanInstanceKHR = nullptr;
    PFN_xrCreateFoveationProfileFB CreateFoveationProfileFB = nullptr;
};

struct Instance {
    Next next;
    std::string app;
    TweakSettings cfg;
    PFN_vkGetInstanceProcAddr vkGipa = nullptr;   // from xrCreateVulkanInstanceKHR, if the app used it
    bool fbFoveation = false;      // the app enabled XR_FB_foveation (as it reached us)
    bool fbConfiguration = false;  // XR_FB_foveation_configuration is enabled on the instance (app's or ours)
    bool addedConfiguration = false;
};

struct Session {
    XrInstance instance = XR_NULL_HANDLE;
    SharpenDevicePtr dev;
    bool sharpenBroken = false;
};

struct Swapchain {
    XrSession session = XR_NULL_HANDLE;
    XrSwapchainCreateInfo info{};
    bool eligible = false;                 // colour, 2D, 1 sample, we added TRANSFER_SRC
    bool projection = false;               // seen in a projection layer
    std::vector<VkImage> images;
    std::deque<uint32_t> acquired;
    SharpenChainPtr chain;
    bool chainFailed = false;
};

std::map<XrInstance, std::unique_ptr<Instance>> g_instances;
std::map<XrSession, std::unique_ptr<Session>> g_sessions;
std::map<XrSwapchain, std::unique_ptr<Swapchain>> g_swapchains;
std::set<std::string> g_logged_once;

void log_line(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "xr-tweak: %s\n", buf);
    if (getenv("XR_TWEAK_NO_LOGFILE")) return;
    std::string dir;
    if (const char *s = getenv("XDG_STATE_HOME"); s && *s) dir = s;
    else if (const char *h = getenv("HOME")) dir = std::string(h) + "/.local/state";
    else return;
    dir += "/xr-tweak";
    std::string mk;
    for (size_t i = 1; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == '/') { mk = dir.substr(0, i); mkdir(mk.c_str(), 0755); }
    FILE *f = fopen((dir + "/xr-tweak.log").c_str(), "a");
    if (!f) return;
    char ts[32];
    time_t t = time(nullptr);
    struct tm tmv;
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", localtime_r(&t, &tmv));
    fprintf(f, "%s pid=%d %s\n", ts, (int)getpid(), buf);
    fclose(f);
}

void log_once(const std::string &key, const char *msg) {
    if (g_logged_once.insert(key).second) log_line("%s", msg);
}

Instance *inst_of(XrInstance i) {
    auto it = g_instances.find(i);
    return it == g_instances.end() ? nullptr : it->second.get();
}

bool killed() {
    // same rule as the loader's disable_environment: SET means disabled, whatever the value
    return getenv("XR_TWEAK_DISABLE") != nullptr;
}

template <typename T> void resolve(PFN_xrGetInstanceProcAddr gipa, XrInstance i, const char *name, T &out) {
    PFN_xrVoidFunction f = nullptr;
    if (XR_SUCCEEDED(gipa(i, name, &f))) out = reinterpret_cast<T>(f);
}

// ---------------------------------------------------------------------------------------------- hooks

XRAPI_ATTR XrResult XRAPI_CALL hook_DestroyInstance(XrInstance instance) {
    PFN_xrDestroyInstance next = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Instance *in = inst_of(instance);
        if (!in) return XR_ERROR_HANDLE_INVALID;
        next = in->next.DestroyInstance;
        g_instances.erase(instance);
    }
    return next(instance);
}

XRAPI_ATTR XrResult XRAPI_CALL hook_EnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId,
                                                                    XrViewConfigurationType type, uint32_t cap,
                                                                    uint32_t *count, XrViewConfigurationView *views) {
    Instance *in;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in = inst_of(instance);
    }
    if (!in) return XR_ERROR_HANDLE_INVALID;
    XrResult r = in->next.EnumerateViewConfigurationViews(instance, systemId, type, cap, count, views);
    const TweakSettings &c = in->cfg;
    if (r != XR_SUCCESS || !views || cap == 0 || !c.enabled) return r;
    if (c.scale == 1.0 && c.width <= 0 && c.height <= 0) return r;
    for (uint32_t i = 0; i < *count && i < cap; ++i) {
        XrViewConfigurationView &v = views[i];
        uint32_t ow = v.recommendedImageRectWidth, oh = v.recommendedImageRectHeight;
        double w = c.width > 0 ? c.width : ow * c.scale;
        double h = c.height > 0 ? c.height : oh * c.scale;
        uint32_t nw = (uint32_t)(w + 0.5), nh = (uint32_t)(h + 0.5);
        if (nw < 16) nw = 16;
        if (nh < 16) nh = 16;
        if (v.maxImageRectWidth && nw > v.maxImageRectWidth) nw = v.maxImageRectWidth;
        if (v.maxImageRectHeight && nh > v.maxImageRectHeight) nh = v.maxImageRectHeight;
        v.recommendedImageRectWidth = nw;
        v.recommendedImageRectHeight = nh;
        char msg[256];
        snprintf(msg, sizeof msg, "app=\"%s\" view%u recommended %ux%u -> %ux%u (max %ux%u)", in->app.c_str(), i, ow,
                 oh, nw, nh, v.maxImageRectWidth, v.maxImageRectHeight);
        std::lock_guard<std::mutex> lk(g_mu);
        log_once(std::string("res") + std::to_string(i) + msg, msg);
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_CreateVulkanInstanceKHR(XrInstance instance,
                                                            const XrVulkanInstanceCreateInfoKHR *info,
                                                            VkInstance *vkInstance, VkResult *vkResult) {
    Instance *in;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in = inst_of(instance);
    }
    if (!in || !in->next.CreateVulkanInstanceKHR) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (info && info->pfnGetInstanceProcAddr) in->vkGipa = info->pfnGetInstanceProcAddr;
    return in->next.CreateVulkanInstanceKHR(instance, info, vkInstance, vkResult);
}

PFN_vkGetInstanceProcAddr system_vk_gipa() {
    static PFN_vkGetInstanceProcAddr p = [] {
        void *h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        return h ? (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr") : nullptr;
    }();
    return p;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_CreateSession(XrInstance instance, const XrSessionCreateInfo *info,
                                                  XrSession *session) {
    Instance *in;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in = inst_of(instance);
    }
    if (!in) return XR_ERROR_HANDLE_INVALID;
    XrResult r = in->next.CreateSession(instance, info, session);
    if (r != XR_SUCCESS || !in->cfg.enabled) return r;
    auto s = std::make_unique<Session>();
    s->instance = instance;
    if (in->cfg.sharpen > 0) {
        const XrGraphicsBindingVulkanKHR *vk = nullptr;
        for (auto *p = (const XrBaseInStructure *)info->next; p; p = p->next)
            if (p->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) vk = (const XrGraphicsBindingVulkanKHR *)p;
        if (!vk) {
            log_line("app=\"%s\" sharpen requested but the session is not Vulkan; sharpening off", in->app.c_str());
        } else {
            SharpenDeviceInfo di{in->vkGipa ? in->vkGipa : system_vk_gipa(), vk->instance, vk->physicalDevice,
                                 vk->device, vk->queueFamilyIndex, vk->queueIndex};
            std::string err;
            s->dev = sharpen_device_create(di, &err);
            if (!s->dev) log_line("app=\"%s\" sharpen init failed (%s); sharpening off", in->app.c_str(), err.c_str());
            else log_line("app=\"%s\" sharpen ready, strength %.2f, queue family %u index %u", in->app.c_str(),
                          in->cfg.sharpen, vk->queueFamilyIndex, vk->queueIndex);
        }
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_sessions[*session] = std::move(s);
    return r;
}

Swapchain *chain_of(XrSwapchain sc) {
    auto it = g_swapchains.find(sc);
    return it == g_swapchains.end() ? nullptr : it->second.get();
}

Session *session_of(XrSession s) {
    auto it = g_sessions.find(s);
    return it == g_sessions.end() ? nullptr : it->second.get();
}

// the Next table of whatever instance owns this session (layers only ever see one or two)
Instance *instance_for_session(XrSession s) {
    Session *se = session_of(s);
    return se ? inst_of(se->instance) : nullptr;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_DestroySession(XrSession session) {
    PFN_xrDestroySession next = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Session *se = session_of(session);
        Instance *in = se ? inst_of(se->instance) : nullptr;
        if (!in) {
            // a session we did not track (enabled=0): find any instance's next pointer
            if (g_instances.empty()) return XR_ERROR_HANDLE_INVALID;
            next = g_instances.begin()->second->next.DestroySession;
        } else {
            next = in->next.DestroySession;
            for (auto it = g_swapchains.begin(); it != g_swapchains.end();) {
                if (it->second->session == session) {
                    it->second->chain.reset();
                    it = g_swapchains.erase(it);
                } else ++it;
            }
            se->dev.reset();
            g_sessions.erase(session);
        }
    }
    return next(session);
}

XRAPI_ATTR XrResult XRAPI_CALL hook_CreateSwapchain(XrSession session, const XrSwapchainCreateInfo *info,
                                                    XrSwapchain *swapchain) {
    Instance *in;
    Session *se;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        se = session_of(session);
        in = se ? inst_of(se->instance) : nullptr;
        if (!in && !g_instances.empty()) in = g_instances.begin()->second.get();
    }
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!se || !se->dev || !info) return in->next.CreateSwapchain(session, info, swapchain);
    bool eligible = (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
                    !(info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && info->faceCount == 1 &&
                    info->sampleCount == 1 && info->mipCount == 1;
    XrSwapchainCreateInfo mod = *info;
    if (eligible) mod.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
    XrResult r = in->next.CreateSwapchain(session, &mod, swapchain);
    if (r != XR_SUCCESS && eligible) {
        // the runtime refused the extra usage bit: fall back to exactly what the app asked for
        log_line("app=\"%s\" swapchain with TRANSFER_SRC refused (%d); this one is not sharpened", in->app.c_str(), r);
        eligible = false;
        r = in->next.CreateSwapchain(session, info, swapchain);
    }
    if (r != XR_SUCCESS) return r;
    auto sc = std::make_unique<Swapchain>();
    sc->session = session;
    sc->info = mod;
    sc->info.next = nullptr;
    sc->eligible = eligible;
    std::lock_guard<std::mutex> lk(g_mu);
    g_swapchains[*swapchain] = std::move(sc);
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_DestroySwapchain(XrSwapchain swapchain) {
    PFN_xrDestroySwapchain next = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Swapchain *sc = chain_of(swapchain);
        Instance *in = sc ? instance_for_session(sc->session) : nullptr;
        if (!in && !g_instances.empty()) in = g_instances.begin()->second.get();
        if (!in) return XR_ERROR_HANDLE_INVALID;
        next = in->next.DestroySwapchain;
        if (sc) {
            sc->chain.reset();
            g_swapchains.erase(swapchain);
        }
    }
    return next(swapchain);
}

Instance *any_instance_for_swapchain(XrSwapchain s, Swapchain **out) {
    Swapchain *sc = chain_of(s);
    if (out) *out = sc;
    Instance *in = sc ? instance_for_session(sc->session) : nullptr;
    if (!in && !g_instances.empty()) in = g_instances.begin()->second.get();
    return in;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_EnumerateSwapchainImages(XrSwapchain swapchain, uint32_t cap, uint32_t *count,
                                                             XrSwapchainImageBaseHeader *images) {
    Swapchain *sc;
    Instance *in;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in = any_instance_for_swapchain(swapchain, &sc);
    }
    if (!in) return XR_ERROR_HANDLE_INVALID;
    XrResult r = in->next.EnumerateSwapchainImages(swapchain, cap, count, images);
    if (r == XR_SUCCESS && sc && images && cap && images->type == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
        auto *vk = (XrSwapchainImageVulkanKHR *)images;
        std::lock_guard<std::mutex> lk(g_mu);
        sc->images.clear();
        for (uint32_t i = 0; i < *count && i < cap; ++i) sc->images.push_back(vk[i].image);
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_AcquireSwapchainImage(XrSwapchain swapchain,
                                                          const XrSwapchainImageAcquireInfo *info, uint32_t *index) {
    Swapchain *sc;
    Instance *in;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in = any_instance_for_swapchain(swapchain, &sc);
    }
    if (!in) return XR_ERROR_HANDLE_INVALID;
    XrResult r = in->next.AcquireSwapchainImage(swapchain, info, index);
    if (r == XR_SUCCESS && sc) {
        std::lock_guard<std::mutex> lk(g_mu);
        sc->acquired.push_back(*index);
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_ReleaseSwapchainImage(XrSwapchain swapchain,
                                                          const XrSwapchainImageReleaseInfo *info) {
    Swapchain *sc;
    Instance *in;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in = any_instance_for_swapchain(swapchain, &sc);
        if (!in) return XR_ERROR_HANDLE_INVALID;
        if (sc && !sc->acquired.empty()) {
            uint32_t idx = sc->acquired.front();   // release always releases the oldest acquired image
            sc->acquired.pop_front();
            Session *se = session_of(sc->session);
            if (se && se->dev && !se->sharpenBroken && sc->eligible && sc->projection && in->cfg.sharpen > 0 &&
                !sc->chainFailed && !sc->images.empty()) {
                std::string err;
                if (!sc->chain) {
                    sc->chain = sharpen_chain_create(se->dev.get(), (VkFormat)sc->info.format, sc->info.width,
                                                     sc->info.height, sc->info.arraySize, sc->images, &err);
                    if (!sc->chain) {
                        sc->chainFailed = true;
                        log_line("app=\"%s\" sharpen setup failed for a %ux%u x%u swapchain (format %lld): %s",
                                 in->app.c_str(), sc->info.width, sc->info.height, sc->info.arraySize,
                                 (long long)sc->info.format, err.c_str());
                    } else {
                        log_line("app=\"%s\" sharpening a %ux%u x%u swapchain (format %lld%s)", in->app.c_str(),
                                 sc->info.width, sc->info.height, sc->info.arraySize, (long long)sc->info.format,
                                 sharpen_format_is_srgb((VkFormat)sc->info.format) ? ", sRGB" : "");
                    }
                }
                if (sc->chain && sharpen_apply(se->dev.get(), sc->chain.get(), idx, (float)in->cfg.sharpen, &err) !=
                                     VK_SUCCESS) {
                    se->sharpenBroken = true;
                    log_line("app=\"%s\" sharpen failed (%s); off for the rest of this session", in->app.c_str(),
                             err.c_str());
                }
            }
        }
    }
    return in->next.ReleaseSwapchainImage(swapchain, info);
}

XRAPI_ATTR XrResult XRAPI_CALL hook_EndFrame(XrSession session, const XrFrameEndInfo *info) {
    Instance *in;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in = instance_for_session(session);
        if (!in && !g_instances.empty()) in = g_instances.begin()->second.get();
        if (!in) return XR_ERROR_HANDLE_INVALID;
        if (info && session_of(session))
            for (uint32_t i = 0; i < info->layerCount; ++i) {
                const XrCompositionLayerBaseHeader *l = info->layers[i];
                if (!l || l->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
                auto *p = (const XrCompositionLayerProjection *)l;
                for (uint32_t v = 0; v < p->viewCount; ++v)
                    if (Swapchain *sc = chain_of(p->views[v].subImage.swapchain)) sc->projection = true;
            }
    }
    return in->next.EndFrame(session, info);
}

// ------------------------------------------------------------------------------------- foveation

// XR_FB_foveation profile creation: replace the level the app asked for with the configured one.
// We never write into the app's structs (they may be const data): the head struct and every struct up to
// and including the level struct are copied, the rest of the chain is shared. If a struct we do not know
// sits before the level struct we cannot relink it without writing to it, so the call goes through
// unchanged and the log says so.
XRAPI_ATTR XrResult XRAPI_CALL hook_CreateFoveationProfileFB(XrSession session,
                                                             const XrFoveationProfileCreateInfoFB *info,
                                                             XrFoveationProfileFB *profile) {
    Instance *in;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in = instance_for_session(session);
        if (!in && !g_instances.empty()) in = g_instances.begin()->second.get();
    }
    if (!in || !in->next.CreateFoveationProfileFB) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const int want = in->cfg.foveation;
    if (!info || want < 0 || !in->cfg.enabled) return in->next.CreateFoveationProfileFB(session, info, profile);

    XrFoveationProfileCreateInfoFB head = *info;
    XrFoveationLevelProfileCreateInfoFB level{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
    XrFoveationEyeTrackedProfileCreateInfoMETA eye{XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META};
    XrBaseInStructure *tail = (XrBaseInStructure *)&head;   // last copied struct; its next is relinked
    const XrFoveationLevelProfileCreateInfoFB *appLevel = nullptr;
    bool appEye = false, unknownFirst = false;
    for (auto *p = (const XrBaseInStructure *)info->next; p; p = p->next) {
        if (p->type == XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB) {
            appLevel = (const XrFoveationLevelProfileCreateInfoFB *)p;
            level = *appLevel;
            tail->next = (const XrBaseInStructure *)&level;
            tail = (XrBaseInStructure *)&level;
            break;   // level.next still points at the app's remainder of the chain
        }
        if (p->type == XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META && !appEye) {
            appEye = true;
            eye = *(const XrFoveationEyeTrackedProfileCreateInfoMETA *)p;
            tail->next = (const XrBaseInStructure *)&eye;
            tail = (XrBaseInStructure *)&eye;
            continue;
        }
        unknownFirst = true;
        break;
    }
    for (auto *p = (const XrBaseInStructure *)(appLevel ? appLevel->next : nullptr); p; p = p->next)
        if (p->type == XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META) appEye = true;

    char msg[384];
    const char *what = tweak_foveation_name(want);
    if (unknownFirst) {
        snprintf(msg, sizeof msg, "app=\"%s\" foveation=%s NOT applied: an unrecognised struct precedes the level "
                 "struct in xrCreateFoveationProfileFB; passed through unchanged", in->app.c_str(), what);
        std::lock_guard<std::mutex> lk(g_mu);
        log_once(msg, msg);
        return in->next.CreateFoveationProfileFB(session, info, profile);
    }
    if (!appLevel) {
        if (!in->fbConfiguration) {
            snprintf(msg, sizeof msg, "app=\"%s\" foveation=%s NOT applied: the runtime refused "
                     "XR_FB_foveation_configuration, so no level can be named", in->app.c_str(), what);
            std::lock_guard<std::mutex> lk(g_mu);
            log_once(msg, msg);
            return in->next.CreateFoveationProfileFB(session, info, profile);
        }
        level.verticalOffset = 0;
        level.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;
        // put our level struct right after the head; anything the app chained follows it
        level.next = head.next;
        head.next = &level;
    }
    level.level = (XrFoveationLevelFB)want;
    if (want == 0) level.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;   // off means off, not "up to none"
    XrResult r = in->next.CreateFoveationProfileFB(session, &head, profile);
    snprintf(msg, sizeof msg,
             "app=\"%s\" foveation profile: app asked level=%s dynamic=%s%s -> xr-tweak level=%d (%s) dynamic=%d%s "
             "result=%d",
             in->app.c_str(), appLevel ? std::to_string((int)appLevel->level).c_str() : "none",
             appLevel ? (appLevel->dynamic ? "1" : "0") : "-", appEye ? " eye_tracked=1" : "", (int)level.level, what,
             level.dynamic ? 1 : 0, appLevel ? "" : " (level struct added)", (int)r);
    std::lock_guard<std::mutex> lk(g_mu);
    log_once(msg, msg);
    return r;
}

bool has_ext(const XrInstanceCreateInfo *info, const char *name) {
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        if (info->enabledExtensionNames[i] && strcmp(info->enabledExtensionNames[i], name) == 0) return true;
    return false;
}

// Where Valve's fdm_injection layer sits relative to us. "below" = the loader put it between us and the
// runtime (it is in our nextInfo chain). Otherwise, if its manifest is in one of the system implicit-layer
// directories, the Khronos loader (manifest_file.cpp, ReadDataFilesInSearchPaths) lists those BEFORE
// $XDG_DATA_HOME, where xr-tweak installs ours, and the first-listed layer is the one nearest the
// app: "above". Neither = "absent" (as far as this process can see; a container may hide /usr/share).
std::string valve_fdm_position(const std::vector<std::string> &below) {
    for (auto &n : below) {
        std::string l;
        for (char c : n) l += (char)tolower((unsigned char)c);
        if (l.find("fdm") != std::string::npos) return "below";
    }
    auto env_or = [](const char *k, const char *d) {
        const char *v = getenv(k);
        return std::string(v && *v ? v : d);
    };
    std::string dirs = env_or("XDG_CONFIG_DIRS", "/etc/xdg") + ":/etc:" + env_or("XDG_DATA_DIRS", "/usr/local/share:/usr/share");
    size_t a = 0;
    while (a <= dirs.size()) {
        size_t b = dirs.find(':', a);
        if (b == std::string::npos) b = dirs.size();
        std::string d = dirs.substr(a, b - a);
        a = b + 1;
        if (d.empty()) continue;
        d += "/openxr/1/api_layers/implicit.d";
        if (DIR *dp = opendir(d.c_str())) {
            bool hit = false;
            while (dirent *e = readdir(dp)) {
                std::string f = e->d_name;
                if (f.find("fdm_injection") != std::string::npos && f.size() > 5 &&
                    f.compare(f.size() - 5, 5, ".json") == 0)
                    hit = true;
            }
            closedir(dp);
            if (hit) return "above(" + d + ")";
        }
    }
    return "absent";
}

// ------------------------------------------------------------------------------------- dispatch

XRAPI_ATTR XrResult XRAPI_CALL layer_GetInstanceProcAddr(XrInstance instance, const char *name,
                                                         PFN_xrVoidFunction *fn) {
    Instance *in;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in = inst_of(instance);
    }
    if (!in) {
        // before our xrCreateApiLayerInstance ran: the loader resolves global functions itself
        return XR_ERROR_HANDLE_INVALID;
    }
    XrResult r = in->next.gipa(instance, name, fn);
    if (r != XR_SUCCESS || !in->cfg.enabled) return r;
#define HOOK(n)                                                    \
    if (strcmp(name, "xr" #n) == 0) {                              \
        *fn = reinterpret_cast<PFN_xrVoidFunction>(hook_##n);      \
        return XR_SUCCESS;                                         \
    }
    HOOK(DestroyInstance)
    HOOK(EnumerateViewConfigurationViews)
    if (in->cfg.foveation >= 0 && in->fbFoveation) {
        HOOK(CreateFoveationProfileFB)
        // profile creation finds its instance through the session, so track sessions too
        HOOK(CreateSession)
        HOOK(DestroySession)
    }
    if (in->cfg.sharpen > 0) {
        HOOK(CreateVulkanInstanceKHR)
        HOOK(CreateSession)
        HOOK(DestroySession)
        HOOK(CreateSwapchain)
        HOOK(DestroySwapchain)
        HOOK(EnumerateSwapchainImages)
        HOOK(AcquireSwapchainImage)
        HOOK(ReleaseSwapchainImage)
        HOOK(EndFrame)
    }
#undef HOOK
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_CreateApiLayerInstance(const XrInstanceCreateInfo *info,
                                                            const XrApiLayerCreateInfo *layerInfo,
                                                            XrInstance *instance) {
    if (!layerInfo || !layerInfo->nextInfo || !info) return XR_ERROR_INITIALIZATION_FAILED;
    XrApiLayerCreateInfo next = *layerInfo;
    next.nextInfo = layerInfo->nextInfo->next;

    auto in = std::make_unique<Instance>();
    in->app = info->applicationInfo.applicationName;
    std::string path;
    in->cfg = tweak_load(in->app, &path);
    if (killed()) in->cfg.enabled = false;   // the manifest's disable_environment normally stops us earlier
    in->fbFoveation = has_ext(info, XR_FB_FOVEATION_EXTENSION_NAME);
    in->fbConfiguration = has_ext(info, XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
    std::vector<std::string> below;
    for (const XrApiLayerNextInfo *ni = layerInfo->nextInfo->next; ni; ni = ni->next) below.push_back(ni->layerName);

    // A level can only be named through XR_FB_foveation_configuration. If the app uses XR_FB_foveation
    // without it and a level is configured, enable it for the app; if the runtime refuses, retry exactly
    // as the app asked (the level is then not reachable, and the log says so).
    XrResult r;
    if (in->cfg.enabled && in->cfg.foveation >= 0 && in->fbFoveation && !in->fbConfiguration) {
        std::vector<const char *> exts(info->enabledExtensionNames, info->enabledExtensionNames + info->enabledExtensionCount);
        exts.push_back(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
        XrInstanceCreateInfo mod = *info;
        mod.enabledExtensionCount = (uint32_t)exts.size();
        mod.enabledExtensionNames = exts.data();
        r = layerInfo->nextInfo->nextCreateApiLayerInstance(&mod, &next, instance);
        if (r == XR_SUCCESS) {
            in->fbConfiguration = in->addedConfiguration = true;
        } else {
            log_line("app=\"%s\" runtime refused added XR_FB_foveation_configuration (%d); retrying as the app asked",
                     in->app.c_str(), (int)r);
            r = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &next, instance);
        }
    } else {
        r = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &next, instance);
    }
    if (r != XR_SUCCESS) return r;
    in->next.gipa = layerInfo->nextInfo->nextGetInstanceProcAddr;
    Next &n = in->next;
    resolve(n.gipa, *instance, "xrDestroyInstance", n.DestroyInstance);
    resolve(n.gipa, *instance, "xrEnumerateViewConfigurationViews", n.EnumerateViewConfigurationViews);
    resolve(n.gipa, *instance, "xrCreateSession", n.CreateSession);
    resolve(n.gipa, *instance, "xrDestroySession", n.DestroySession);
    resolve(n.gipa, *instance, "xrCreateSwapchain", n.CreateSwapchain);
    resolve(n.gipa, *instance, "xrDestroySwapchain", n.DestroySwapchain);
    resolve(n.gipa, *instance, "xrEnumerateSwapchainImages", n.EnumerateSwapchainImages);
    resolve(n.gipa, *instance, "xrAcquireSwapchainImage", n.AcquireSwapchainImage);
    resolve(n.gipa, *instance, "xrReleaseSwapchainImage", n.ReleaseSwapchainImage);
    resolve(n.gipa, *instance, "xrEndFrame", n.EndFrame);
    resolve(n.gipa, *instance, "xrCreateVulkanInstanceKHR", n.CreateVulkanInstanceKHR);
    if (in->fbFoveation) resolve(n.gipa, *instance, "xrCreateFoveationProfileFB", n.CreateFoveationProfileFB);

    const TweakSettings &c = in->cfg;
    std::string belowList;
    for (auto &b : below) belowList += (belowList.empty() ? "" : ",") + b;
    log_line("app=\"%s\" engine=\"%s\" config=%s sections=%s enabled=%d scale=%.3f width=%d height=%d sharpen=%.2f "
             "foveation=%s fb_foveation=%d fb_configuration=%s valve_fdm=%s layers_below=[%s]",
             in->app.c_str(), info->applicationInfo.engineName, path.c_str(),
             c.source.empty() ? "none" : c.source.c_str(), c.enabled ? 1 : 0, c.scale, c.width, c.height, c.sharpen,
             tweak_foveation_name(c.foveation), in->fbFoveation ? 1 : 0,
             in->addedConfiguration ? "added" : (in->fbConfiguration ? "1" : "0"), valve_fdm_position(below).c_str(),
             belowList.c_str());
    if (c.enabled && c.foveation >= 0 && !in->fbFoveation)
        log_line("app=\"%s\" foveation=%s cannot apply: the app does not use XR_FB_foveation, and an OpenXR layer "
                 "cannot foveate an app that does not bind a density map itself. Valve's fdm_injection layer is what "
                 "foveates it: Steam > the game's Properties > Foveated Rendering (on/off)",
                 in->app.c_str(), tweak_foveation_name(c.foveation));
    std::lock_guard<std::mutex> lk(g_mu);
    g_instances[*instance] = std::move(in);
    return r;
}

}  // namespace

EXPORT XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo *loaderInfo,
                                                              const char *layerName,
                                                              XrNegotiateApiLayerRequest *req) {
    if (!loaderInfo || !req || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        req->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST)
        return XR_ERROR_INITIALIZATION_FAILED;
    if (layerName && strcmp(layerName, LAYER_NAME) != 0) return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    req->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    // we forward everything we do not hook, so claim the newest API the loader allows
    XrVersion v = XR_CURRENT_API_VERSION;
    if (v > loaderInfo->maxApiVersion) v = loaderInfo->maxApiVersion;
    if (v < loaderInfo->minApiVersion) return XR_ERROR_INITIALIZATION_FAILED;
    req->layerApiVersion = v;
    req->getInstanceProcAddr = layer_GetInstanceProcAddr;
    req->createApiLayerInstance = layer_CreateApiLayerInstance;
    return XR_SUCCESS;
}
