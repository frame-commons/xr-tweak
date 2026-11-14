// SPDX-License-Identifier: MIT
// A stand-in for Valve's XrApiLayer_VALVE_fdm_injection in the off-headset tests: a pure passthrough
// OpenXR layer with Valve's file name, so the REAL Khronos loader places it in the chain by its own rules.
// It prints, on stderr, the layers the loader put below it, which is an independent witness of the
// order (the test does not take xr-tweak's own valve_fdm= claim as the oracle).
//
// What the real layer does beyond passing through is unknown (it also hooks Vulkan) — this models only
// its position.
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cstdio>
#include <cstring>
#include <string>

#define EXPORT extern "C" __attribute__((visibility("default")))
#define FAKE_NAME "XR_APILAYER_VALVE_fdm_injection"

namespace {
PFN_xrGetInstanceProcAddr g_next = nullptr;

XrResult XRAPI_CALL gipa(XrInstance i, const char *name, PFN_xrVoidFunction *fn) {
    if (!g_next) return XR_ERROR_HANDLE_INVALID;
    return g_next(i, name, fn);
}

XrResult XRAPI_CALL create(const XrInstanceCreateInfo *info, const XrApiLayerCreateInfo *li, XrInstance *inst) {
    std::string below;
    for (const XrApiLayerNextInfo *n = li->nextInfo->next; n; n = n->next) below += std::string(below.empty() ? "" : ",") + n->layerName;
    fprintf(stderr, "fake-valve: below=[%s]\n", below.c_str());
    XrApiLayerCreateInfo next = *li;
    next.nextInfo = li->nextInfo->next;
    g_next = li->nextInfo->nextGetInstanceProcAddr;
    return li->nextInfo->nextCreateApiLayerInstance(info, &next, inst);
}
}  // namespace

EXPORT XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo *li, const char *name,
                                                              XrNegotiateApiLayerRequest *req) {
    if (name && strcmp(name, FAKE_NAME) != 0) return XR_ERROR_INITIALIZATION_FAILED;
    req->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    req->layerApiVersion = XR_CURRENT_API_VERSION > li->maxApiVersion ? li->maxApiVersion : XR_CURRENT_API_VERSION;
    req->getInstanceProcAddr = gipa;
    req->createApiLayerInstance = create;
    return XR_SUCCESS;
}
