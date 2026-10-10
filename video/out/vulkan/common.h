#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>

#include "config.h"

#include "common/common.h"
#include "common/msg.h"

// We need to define all platforms we want to support. Since we have
// our own mechanism for checking this, we re-define the right symbols
#if HAVE_WAYLAND
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#if HAVE_X11
#define VK_USE_PLATFORM_XLIB_KHR
#endif
#if HAVE_WIN32_DESKTOP
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#if HAVE_COCOA
#define VK_USE_PLATFORM_METAL_EXT
#endif

#include <libplacebo/vulkan.h>

// Properties2 queries were promoted to Vulkan 1.1. Resolve them through the
// active instance so older loaders do not need to export the core symbols.
static inline PFN_vkVoidFunction mpvk_get_physical_device_proc_addr(
    pl_vk_inst inst, const char *core_name, const char *khr_name)
{
    if (!inst || !inst->get_proc_addr)
        return NULL;

    if (inst->api_version >= VK_API_VERSION_1_1) {
        PFN_vkVoidFunction proc = inst->get_proc_addr(inst->instance, core_name);
        if (proc)
            return proc;
    }

    for (int i = 0; i < inst->num_extensions; i++) {
        if (strcmp(inst->extensions[i],
                   VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0)
            return inst->get_proc_addr(inst->instance, khr_name);
    }

    return NULL;
}

// Shared struct used to hold vulkan context information
struct mpvk_ctx {
    pl_log pllog;
    pl_vk_inst vkinst;
    pl_vulkan vulkan;
    pl_gpu gpu; // points to vulkan->gpu for convenience
    pl_swapchain swapchain;
    VkSurfaceKHR surface;
};
