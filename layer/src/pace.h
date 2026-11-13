/* pace.h - XMX_LAYER_PACE: even spacing of frame generation's images at vkQueuePresentKHR (pace.c). */
#pragma once
#include <vulkan/vulkan.h>

struct xpace;

/* NULL unless XMX_LAYER_PACE is set (1 = pace, log = measure only) */
struct xpace *xpace_create(VkDevice device, PFN_vkGetDeviceProcAddr gdpa);
void xpace_destroy(struct xpace *p);
/* the layer's vkQueuePresentKHR of a device with pacing */
VkResult xpace_present(struct xpace *p, VkQueue queue, const VkPresentInfoKHR *pi);
/* the queue and swap chain commands pace.c takes over on a device with pacing, NULL for any other */
PFN_vkVoidFunction xpace_proc(VkDevice device, const char *name);
