// Vulkan for libroblox.so on the host driver (vulkan-1.dll): the Android-specific parts. Everything else is the
// generated pass-through in gen_vk.c. Modelled on Mocktail's bionic_vulkan_loader_adapter.cc.
// Active only with SKATESTRAP_VULKAN=1 (dl.c offers libvulkan.so then); otherwise Roblox falls back to GLES/ANGLE.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vulkan/vulkan.h>
#include "shim.h"

void *vk_gen_bind(const char *name, void *host);
void skate_frame(int after);

static HMODULE vk;
static PFN_vkGetInstanceProcAddr h_gipa;
static PFN_vkGetDeviceProcAddr h_gdpa;
static int load(void) {
    if (vk) return 1;
    vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { printf("[vulkan] vulkan-1.dll not found\n"); return 0; }
    h_gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    h_gdpa = (PFN_vkGetDeviceProcAddr)GetProcAddress(vk, "vkGetDeviceProcAddr");
    return h_gipa != 0;
}
// Loader trampolines work for any instance/device; fine for the few hand-written calls.
#define HOST(name) ({ static PFN_##name p_; if (!p_) p_ = (PFN_##name)GetProcAddress(vk, #name); p_; })

// loader.exe --gpu (for Skatestrap's first-run setup): prints "gpu: discrete|integrated|other|none" and the GPU names.
// Any separate card wins; built-in only = integrated.
int skate_gpu_kind(void) {
    const char *kind = "none";
    VkInstanceCreateInfo ic = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance in;
    if (load() && !HOST(vkCreateInstance)(&ic, 0, &in)) {
        VkPhysicalDevice pd[16]; uint32_t n = 16;
        HOST(vkEnumeratePhysicalDevices)(in, &n, pd);
        int dis = 0, igp = 0;
        for (uint32_t i = 0; i < n; i++) {
            VkPhysicalDeviceProperties p; HOST(vkGetPhysicalDeviceProperties)(pd[i], &p);
            printf("gpu %u: %s (type %d)\n", i, p.deviceName, p.deviceType);
            dis |= p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            igp |= p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        }
        kind = dis ? "discrete" : igp ? "integrated" : n ? "other" : "none";
        HOST(vkDestroyInstance)(in, 0);
    }
    printf("gpu: %s\n", kind);
    return 0;
}

// SKATESTRAP_FPS=1: count and time the Vulkan calls that can block or compile, printed with the fps line (vk_stats)
enum { K_FENCE, K_SUBMIT, K_QIDLE, K_DIDLE, K_ACQ, K_QUERY, K_PIPE, K_ALLOC, K_IMAGE, K_N };
static const char *k_name[K_N] = { "WaitForFences", "QueueSubmit", "QueueWaitIdle", "DeviceWaitIdle", "AcquireNextImage", "GetQueryPoolResults", "CreateGraphicsPipelines", "AllocateMemory", "CreateImage" };
static volatile LONG64 k_n[K_N], k_ns[K_N], k_max[K_N];
static int k_on = -1;
static LONGLONG k_t(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
static void k_add(int k, LONGLONG t0) {
    static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LONG64 ns = (k_t() - t0) * 1000000000 / f.QuadPart;
    InterlockedIncrement64(&k_n[k]); InterlockedAdd64(&k_ns[k], ns);
    for (LONG64 m = k_max[k]; ns > m && InterlockedCompareExchange64(&k_max[k], ns, m) != m; m = k_max[k]) {}
}
#define TIMED(k, call) ({ if (k_on < 0) k_on = getenv("SKATESTRAP_FPS") != 0; LONGLONG t0_ = k_on ? k_t() : 0; VkResult r_ = call; if (k_on) k_add(k, t0_); r_; })
void vk_stats(void) {
    int any = 0;
    for (int k = 0; k < K_N; k++) if (k_n[k]) {
        if (!any++) printf("  vulkan (count / ms total / ms longest):");
        printf(" %s %lld/%.0f/%.1f", k_name[k], (long long)k_n[k], k_ns[k] / 1e6, k_max[k] / 1e6);
        k_n[k] = k_ns[k] = k_max[k] = 0;
    }
    if (any) printf("\n");
}
static SYSV VkResult s_WaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t t) { return TIMED(K_FENCE, HOST(vkWaitForFences)(d, n, f, all, t)); }
static SYSV VkResult s_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) { return TIMED(K_SUBMIT, HOST(vkQueueSubmit)(q, n, si, f)); }
static SYSV VkResult s_QueueWaitIdle(VkQueue q) { return TIMED(K_QIDLE, HOST(vkQueueWaitIdle)(q)); }
static SYSV VkResult s_DeviceWaitIdle(VkDevice d) { return TIMED(K_DIDLE, HOST(vkDeviceWaitIdle)(d)); }
static SYSV VkResult s_GetQueryPoolResults(VkDevice d, VkQueryPool p, uint32_t first, uint32_t n, size_t sz, void *data, VkDeviceSize stride, VkQueryResultFlags fl) {
    return TIMED(K_QUERY, HOST(vkGetQueryPoolResults)(d, p, first, n, sz, data, stride, fl));
}
static SYSV VkResult s_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    return TIMED(K_PIPE, HOST(vkCreateGraphicsPipelines)(d, c, n, ci, a, p));
}
static SYSV VkResult s_AllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *m) { return TIMED(K_ALLOC, HOST(vkAllocateMemory)(d, ai, a, m)); }
static SYSV VkResult s_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *i) { return TIMED(K_IMAGE, HOST(vkCreateImage)(d, ci, a, i)); }


static int hidden(const char *ext) {
    return !strcmp(ext, "VK_KHR_win32_surface") || !strcmp(ext, "VK_EXT_debug_report") || !strcmp(ext, "VK_EXT_debug_utils");
}

static SYSV VkResult s_EnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *props) {
    if (!load()) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer) { *count = 0; return VK_SUCCESS; }
    static VkExtensionProperties host[64], mine[65]; static uint32_t n;
    if (!n) {
        uint32_t hn = 64; HOST(vkEnumerateInstanceExtensionProperties)(0, &hn, host);
        for (uint32_t i = 0; i < hn; i++) if (!hidden(host[i].extensionName)) mine[n++] = host[i];
        strcpy(mine[n].extensionName, "VK_KHR_android_surface"); mine[n++].specVersion = 6;
    }
    if (!props) { *count = n; return VK_SUCCESS; }
    uint32_t k = *count < n ? *count : n;
    memcpy(props, mine, k * sizeof *props); *count = k;
    return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}
// No layers: Android layer names don't exist here, and host ones (validation) would only slow things down.
static SYSV VkResult s_EnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *props) { *count = 0; return VK_SUCCESS; }

static SYSV VkResult s_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *inst) {
    if (!load()) return VK_ERROR_INITIALIZATION_FAILED;
    const char *ext[80]; uint32_t n = 0; int surf = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount && n < 78; i++) {
        const char *e = ci->ppEnabledExtensionNames[i];
        if (!strcmp(e, "VK_KHR_android_surface")) { ext[n++] = "VK_KHR_win32_surface"; continue; }
        if (!strcmp(e, "VK_KHR_surface")) surf = 1;
        if (!hidden(e)) ext[n++] = e;
    }
    if (!surf) ext[n++] = "VK_KHR_surface";
    // Debug-messenger structs in pNext carry SysV callbacks: unlink them (they are the caller's, on its stack).
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)ci; p->pNext;)
        if (p->pNext->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT ||
            p->pNext->sType == VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT) p->pNext = p->pNext->pNext;
        else p = p->pNext;
    VkInstanceCreateInfo h = *ci;
    h.enabledExtensionCount = n; h.ppEnabledExtensionNames = ext;
    h.enabledLayerCount = 0; h.ppEnabledLayerNames = 0;
    VkResult r = HOST(vkCreateInstance)(&h, 0, inst);
    printf("[vulkan] host VkInstance %s (%u extensions, api %u.%u)\n", r ? "FAILED" : "created", n,
           ci->pApplicationInfo ? VK_API_VERSION_MAJOR(ci->pApplicationInfo->apiVersion) : 1,
           ci->pApplicationInfo ? VK_API_VERSION_MINOR(ci->pApplicationInfo->apiVersion) : 0);
    return r;
}

typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; SkateWindow *window; } AndroidSurfaceInfo;
static SYSV VkResult s_CreateAndroidSurfaceKHR(VkInstance inst, const AndroidSurfaceInfo *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *s) {
    VkWin32SurfaceCreateInfoKHR w = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, 0, 0, GetModuleHandleA(0), (HWND)ci->window->hwnd };
    VkResult r = ((PFN_vkCreateWin32SurfaceKHR)h_gipa(inst, "vkCreateWin32SurfaceKHR"))(inst, &w, 0, s);
    printf("[vulkan] surface on window %dx%d: %d\n", ci->window->width, ci->window->height, r);
    return r;
}

// Present modes the host offered on the last query; the swapchain picks a tear-free-or-uncapped one from them.
static int has_immediate, has_mailbox;
static SYSV VkResult s_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *count, VkPresentModeKHR *modes) {
    VkPresentModeKHR all[16]; uint32_t n = 16;
    if (HOST(vkGetPhysicalDeviceSurfacePresentModesKHR)(pd, s, &n, all) >= 0)
        for (uint32_t i = 0; i < n; i++) {
            if (all[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) has_immediate = 1;
            if (all[i] == VK_PRESENT_MODE_MAILBOX_KHR) has_mailbox = 1;
        }
    return HOST(vkGetPhysicalDeviceSurfacePresentModesKHR)(pd, s, count, modes);
}

// --- SKATESTRAP_RENDER_SCALE: the game gets images of the scaled size; each present blits (stretches) one onto the
// real swapchain image of the same index. Fewer pixels shaded = less GPU work on built-in graphics. ---
float skate_render_scale(void);
static VkPhysicalDevice g_pd;  // from the surface-capabilities query that comes before any swapchain
static struct { VkQueue q; uint32_t fam; } queues[16]; static int nqueues;
static void note_queue(VkQueue q, uint32_t fam) { for (int i = 0; i < nqueues; i++) if (queues[i].q == q) return; if (nqueues < 16) { queues[nqueues].q = q; queues[nqueues++].fam = fam; } }
static SYSV void s_GetDeviceQueue(VkDevice d, uint32_t fam, uint32_t idx, VkQueue *q) { HOST(vkGetDeviceQueue)(d, fam, idx, q); note_queue(*q, fam); }
static SYSV void s_GetDeviceQueue2(VkDevice d, const VkDeviceQueueInfo2 *qi, VkQueue *q) { HOST(vkGetDeviceQueue2)(d, qi, q); note_queue(*q, qi->queueFamilyIndex); }

static SYSV VkResult s_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c) {
    g_pd = pd;
    VkResult r = HOST(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(pd, s, c);
    float k = skate_render_scale();
    if (!r && k < 1 && c->currentExtent.width != 0xFFFFFFFFu) {
        c->currentExtent.width = c->currentExtent.width * k + 0.5f; c->currentExtent.height = c->currentExtent.height * k + 0.5f;
        if (!c->currentExtent.width) c->currentExtent.width = 1;
        if (!c->currentExtent.height) c->currentExtent.height = 1;
        c->minImageExtent = (VkExtent2D){ 1, 1 };
    }
    return r;
}

enum { MAXI = 8 };
typedef struct {
    VkSwapchainKHR sc; VkDevice d; uint32_t n, layers; VkExtent2D lo, big; VkFilter filter;
    VkImage real[MAXI], fake[MAXI]; VkDeviceMemory mem[MAXI];
    VkCommandPool pool; VkCommandBuffer cb[MAXI]; VkFence fence[MAXI]; VkSemaphore done[MAXI];
    // FSR 1 (default; SKATESTRAP_UPSCALE=linear = plain blit): EASU fake[i] -> mid, RCAS mid -> out, blit out -> real[i]
    int fsr; VkImage mid, out; VkDeviceMemory midm, outm; VkImageView midv, outv, fakev[MAXI]; VkSampler smp;
    VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline easu, rcas; VkDescriptorPool dp; VkDescriptorSet ds[MAXI + 1];
    uint32_t easuc[16], rcasc[4];
} Stretch;
static const uint32_t easu_spv[] =
#include "fsr/easu.inc"
;
static const uint32_t rcas_spv[] =
#include "fsr/rcas.inc"
;
static Stretch st[4];
static Stretch *find(VkSwapchainKHR sc) { for (int i = 0; i < 4; i++) if (sc && st[i].sc == sc) return &st[i]; return 0; }
static void stretch_free(Stretch *f) {
    if (f->pool) { HOST(vkWaitForFences)(f->d, f->n, f->fence, VK_TRUE, UINT64_MAX); HOST(vkDestroyCommandPool)(f->d, f->pool, 0); }
    for (uint32_t i = 0; i < f->n; i++) {
        if (f->fence[i]) HOST(vkDestroyFence)(f->d, f->fence[i], 0);
        if (f->done[i]) HOST(vkDestroySemaphore)(f->d, f->done[i], 0);
        if (f->fakev[i]) HOST(vkDestroyImageView)(f->d, f->fakev[i], 0);
        if (f->fake[i]) HOST(vkDestroyImage)(f->d, f->fake[i], 0);
        if (f->mem[i]) HOST(vkFreeMemory)(f->d, f->mem[i], 0);
    }
    if (f->dp) HOST(vkDestroyDescriptorPool)(f->d, f->dp, 0);
    if (f->easu) HOST(vkDestroyPipeline)(f->d, f->easu, 0);
    if (f->rcas) HOST(vkDestroyPipeline)(f->d, f->rcas, 0);
    if (f->pl) HOST(vkDestroyPipelineLayout)(f->d, f->pl, 0);
    if (f->dsl) HOST(vkDestroyDescriptorSetLayout)(f->d, f->dsl, 0);
    if (f->smp) HOST(vkDestroySampler)(f->d, f->smp, 0);
    if (f->midv) HOST(vkDestroyImageView)(f->d, f->midv, 0);
    if (f->outv) HOST(vkDestroyImageView)(f->d, f->outv, 0);
    if (f->mid) HOST(vkDestroyImage)(f->d, f->mid, 0);
    if (f->out) HOST(vkDestroyImage)(f->d, f->out, 0);
    if (f->midm) HOST(vkFreeMemory)(f->d, f->midm, 0);
    if (f->outm) HOST(vkFreeMemory)(f->d, f->outm, 0);
    memset(f, 0, sizeof *f);
}
static int bind_mem(VkDevice d, VkImage im, VkDeviceMemory *m) {
    VkPhysicalDeviceMemoryProperties mp; HOST(vkGetPhysicalDeviceMemoryProperties)(g_pd, &mp);
    VkMemoryRequirements mr; HOST(vkGetImageMemoryRequirements)(d, im, &mr);
    uint32_t t = 0; while (t < mp.memoryTypeCount && !((mr.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) t++;
    if (t == mp.memoryTypeCount) return 0;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, t };
    return !HOST(vkAllocateMemory)(d, &ai, 0, m) && !HOST(vkBindImageMemory)(d, im, *m, 0);
}
static VkImageView view(VkDevice d, VkImage im, VkFormat fmt) {
    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, im, VK_IMAGE_VIEW_TYPE_2D, fmt, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView v = 0; HOST(vkCreateImageView)(d, &vc, 0, &v); return v;
}
static uint32_t f2u(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
// The two FSR passes: full-size RGBA8 work images, pipelines, one descriptor set per game image + one for RCAS. 0 = failed.
static int fsr_make(Stretch *f, VkFormat fmt) {
    VkDevice d = f->d; VkFormatProperties fp; HOST(vkGetPhysicalDeviceFormatProperties)(g_pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return 0;
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { f->big.width, f->big.height, 1 },
                             1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED };
    if (HOST(vkCreateImage)(d, &ic, 0, &f->mid) || !bind_mem(d, f->mid, &f->midm)) return 0;
    ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (HOST(vkCreateImage)(d, &ic, 0, &f->out) || !bind_mem(d, f->out, &f->outm)) return 0;
    if (!(f->midv = view(d, f->mid, VK_FORMAT_R8G8B8A8_UNORM)) || !(f->outv = view(d, f->out, VK_FORMAT_R8G8B8A8_UNORM))) return 0;
    for (uint32_t i = 0; i < f->n; i++) if (!(f->fakev[i] = view(d, f->fake[i], fmt))) return 0;
    VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, 0, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    if (HOST(vkCreateSampler)(d, &sc, 0, &f->smp)) return 0;
    VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
                                           { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT } };
    VkDescriptorSetLayoutCreateInfo lc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 2, lb };
    if (HOST(vkCreateDescriptorSetLayout)(d, &lc, 0, &f->dsl)) return 0;
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 64 };
    VkPipelineLayoutCreateInfo plc = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &f->dsl, 1, &pr };
    if (HOST(vkCreatePipelineLayout)(d, &plc, 0, &f->pl)) return 0;
    for (int k = 0; k < 2; k++) {
        VkShaderModuleCreateInfo mc = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, k ? sizeof rcas_spv : sizeof easu_spv, k ? rcas_spv : easu_spv };
        VkShaderModule m; if (HOST(vkCreateShaderModule)(d, &mc, 0, &m)) return 0;
        VkComputePipelineCreateInfo pc = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0,
                                           { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main" }, f->pl };
        VkResult r = HOST(vkCreateComputePipelines)(d, 0, 1, &pc, 0, k ? &f->rcas : &f->easu);
        HOST(vkDestroyShaderModule)(d, m, 0);
        if (r) return 0;
    }
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, f->n + 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, f->n + 1 } };
    VkDescriptorPoolCreateInfo dpc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, f->n + 1, 2, ps };
    if (HOST(vkCreateDescriptorPool)(d, &dpc, 0, &f->dp)) return 0;
    VkDescriptorSetLayout ls[MAXI + 1]; for (uint32_t i = 0; i <= f->n; i++) ls[i] = f->dsl;
    VkDescriptorSetAllocateInfo da = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, f->dp, f->n + 1, ls };
    if (HOST(vkAllocateDescriptorSets)(d, &da, f->ds)) return 0;
    for (uint32_t i = 0; i <= f->n; i++) {  // set i < n: game image i -> mid (EASU); set n: mid -> out (RCAS)
        VkDescriptorImageInfo in = { f->smp, i < f->n ? f->fakev[i] : f->midv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo o = { 0, i < f->n ? f->midv : f->outv, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, f->ds[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &in },
                                      { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, f->ds[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &o } };
        HOST(vkUpdateDescriptorSets)(d, 2, w, 0, 0);
    }
    // FsrEasuCon / FsrRcasCon from ffx_fsr1.h (input viewport = input size); sharpness 0.2 stops
    float iw = f->lo.width, ih = f->lo.height, ow = f->big.width, oh = f->big.height;
    float c[16] = { iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, 1 / iw, 1 / ih, 1 / iw, -1 / ih,
                    -1 / iw, 2 / ih, 1 / iw, 2 / ih, 0, 4 / ih, 0, 0 };
    for (int k = 0; k < 16; k++) f->easuc[k] = f2u(c[k]);
    f->easuc[14] = f->easuc[15] = 0;
    f->rcasc[0] = f2u(exp2f(-0.2f)); f->rcasc[1] = f->rcasc[2] = f->rcasc[3] = 0;
    return 1;
}
// Small images like the swapchain's, plus the sync objects for the blit. 0 = failed (caller frees).
static int stretch_make(Stretch *f, const VkSwapchainCreateInfoKHR *ci) {
    uint32_t n = MAXI;
    if (HOST(vkGetSwapchainImagesKHR)(f->d, f->sc, &n, f->real) < 0 || n > MAXI) return 0;  // VK_INCOMPLETE: more than MAXI images
    f->n = n; f->layers = ci->imageArrayLayers;
    VkFormatProperties fp; HOST(vkGetPhysicalDeviceFormatProperties)(g_pd, ci->imageFormat, &fp);
    f->filter = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, ci->imageFormat, { f->lo.width, f->lo.height, 1 },
                             1, ci->imageArrayLayers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             ci->imageSharingMode, ci->queueFamilyIndexCount, ci->pQueueFamilyIndices, VK_IMAGE_LAYOUT_UNDEFINED };
    for (uint32_t i = 0; i < n; i++) {
        if (HOST(vkCreateImage)(f->d, &ic, 0, &f->fake[i])) return 0;
        if (!bind_mem(f->d, f->fake[i], &f->mem[i])) return 0;
        VkFenceCreateInfo fc = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, VK_FENCE_CREATE_SIGNALED_BIT };
        VkSemaphoreCreateInfo sc = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (HOST(vkCreateFence)(f->d, &fc, 0, &f->fence[i]) || HOST(vkCreateSemaphore)(f->d, &sc, 0, &f->done[i])) return 0;
    }
    const char *up = getenv("SKATESTRAP_UPSCALE");
    if (!(up && !strcmp(up, "linear")) && f->filter == VK_FILTER_LINEAR) {
        f->fsr = fsr_make(f, ci->imageFormat);
        if (!f->fsr) printf("[vulkan] render scale: FSR setup failed, plain stretch\n");  // its leftovers are freed with the swapchain
    }
    return 1;
}
static VkImageMemoryBarrier bar(VkImage im, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst, uint32_t layers) {
    return (VkImageMemoryBarrier){ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, src, dst, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im,
                                   { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers } };
}
// Blit image i after the game's render (its present wait semaphores); returns the semaphore present must wait on.
static VkSemaphore stretch_blit(Stretch *f, VkQueue q, uint32_t i, const VkPresentInfoKHR *pi) {
    if (!f->pool) {
        uint32_t fam = 0; for (int k = 0; k < nqueues; k++) if (queues[k].q == q) fam = queues[k].fam;  // shortcut: family 0 if the queue was never seen
        VkCommandPoolCreateInfo pc = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, fam };
        VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, 0, VK_COMMAND_BUFFER_LEVEL_PRIMARY, f->n };
        if (HOST(vkCreateCommandPool)(f->d, &pc, 0, &f->pool)) { f->pool = 0; return 0; }
        ca.commandPool = f->pool;
        if (HOST(vkAllocateCommandBuffers)(f->d, &ca, f->cb)) return 0;
    }
    HOST(vkWaitForFences)(f->d, 1, &f->fence[i], VK_TRUE, UINT64_MAX);
    HOST(vkResetFences)(f->d, 1, &f->fence[i]);
    VkCommandBuffer cb = f->cb[i];
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    HOST(vkBeginCommandBuffer)(cb, &bi);
    if (f->fsr) {
        const VkPipelineStageFlags CT = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, CS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        uint32_t gx = (f->big.width + 15) / 16, gy = (f->big.height + 15) / 16;
        VkImageMemoryBarrier a[2] = { bar(f->fake[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT, 1),
                                      bar(f->mid, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, 1) };
        HOST(vkCmdPipelineBarrier)(cb, CT, CS, 0, 0, 0, 0, 0, 2, a);
        HOST(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, f->easu);
        HOST(vkCmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, f->pl, 0, 1, &f->ds[i], 0, 0);
        HOST(vkCmdPushConstants)(cb, f->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, f->easuc);
        HOST(vkCmdDispatch)(cb, gx, gy, 1);
        VkImageMemoryBarrier b[2] = { bar(f->mid, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 1),
                                      bar(f->out, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, 1) };
        HOST(vkCmdPipelineBarrier)(cb, CT, CS, 0, 0, 0, 0, 0, 2, b);
        HOST(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, f->rcas);
        HOST(vkCmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, f->pl, 0, 1, &f->ds[f->n], 0, 0);
        HOST(vkCmdPushConstants)(cb, f->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, f->rcasc);
        HOST(vkCmdDispatch)(cb, gx, gy, 1);
        VkImageMemoryBarrier c[3] = { bar(f->out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 1),
                                      bar(f->real[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, 1),
                                      bar(f->fake[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0, 1) };
        HOST(vkCmdPipelineBarrier)(cb, CT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 3, c);
        VkImageBlit r = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { (int32_t)f->big.width, (int32_t)f->big.height, 1 } },
                          { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { (int32_t)f->big.width, (int32_t)f->big.height, 1 } } };
        // same size: the blit only converts RGBA8 to the swapchain's format
        HOST(vkCmdBlitImage)(cb, f->out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f->real[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_NEAREST);
        VkImageMemoryBarrier e = bar(f->real[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1);
        HOST(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &e);
    } else {
    VkImageMemoryBarrier b0[2] = { bar(f->fake[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT, f->layers),
                                   bar(f->real[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, 1) };
    HOST(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 2, b0);
    VkImageBlit r = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { (int32_t)f->lo.width, (int32_t)f->lo.height, 1 } },
                      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { (int32_t)f->big.width, (int32_t)f->big.height, 1 } } };
    HOST(vkCmdBlitImage)(cb, f->fake[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f->real[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, f->filter);
    // the game's image goes back to the layout it left it in
    VkImageMemoryBarrier b1[2] = { bar(f->fake[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0, f->layers),
                                   bar(f->real[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1) };
    HOST(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 2, b1);
    }
    HOST(vkEndCommandBuffer)(cb);
    VkPipelineStageFlags ws[8]; uint32_t nw = pi->waitSemaphoreCount < 8 ? pi->waitSemaphoreCount : 8;
    for (uint32_t k = 0; k < nw; k++) ws[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, nw, pi->pWaitSemaphores, ws, 1, &cb, 1, &f->done[i] };
    return HOST(vkQueueSubmit)(q, 1, &si, f->fence[i]) ? 0 : f->done[i];
}
static SYSV VkResult s_GetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR sc, uint32_t *count, VkImage *images) {
    Stretch *f = find(sc);
    if (!f) return HOST(vkGetSwapchainImagesKHR)(d, sc, count, images);
    if (!images) { *count = f->n; return VK_SUCCESS; }
    uint32_t k = *count < f->n ? *count : f->n;
    memcpy(images, f->fake, k * sizeof *images); *count = k;
    return k < f->n ? VK_INCOMPLETE : VK_SUCCESS;
}
static SYSV void s_DestroySwapchainKHR(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks *a) {
    Stretch *f = find(sc); if (f) stretch_free(f);
    HOST(vkDestroySwapchainKHR)(d, sc, 0);
}

static SYSV VkResult s_CreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *sc) {
    VkSwapchainCreateInfoKHR h = *ci;
    VkSurfaceCapabilitiesKHR cap; VkExtent2D big = { 0, 0 };
    if (skate_render_scale() < 1 && g_pd && !HOST(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(g_pd, h.surface, &cap) &&
        cap.currentExtent.width != 0xFFFFFFFFu && (cap.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
        (cap.currentExtent.width != h.imageExtent.width || cap.currentExtent.height != h.imageExtent.height)) {
        big = cap.currentExtent;
        h.imageExtent = big; h.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    // Uncapped fps (SKATESTRAP_FPS_CAP set): FIFO would hold us at the refresh rate, as Mocktail's present_mode_policy does.
    // shortcut: relies on Roblox having queried present modes first; query here if a log shows FIFO kept.
    if (getenv("SKATESTRAP_FPS_CAP") && h.presentMode == VK_PRESENT_MODE_FIFO_KHR) {
        if (has_immediate) h.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (has_mailbox) { h.presentMode = VK_PRESENT_MODE_MAILBOX_KHR; if (h.minImageCount < 3) h.minImageCount = 3; }
    }
    VkResult r = HOST(vkCreateSwapchainKHR)(d, &h, 0, sc);
    printf("[vulkan] swapchain %ux%u, %u images, present mode %d (asked %d): %d\n", h.imageExtent.width, h.imageExtent.height,
           h.minImageCount, h.presentMode, ci->presentMode, r);
    if (!r && big.width) {
        Stretch *f = find(0); for (int i = 0; i < 4 && !f; i++) if (!st[i].sc) f = &st[i];
        if (f) { f->sc = *sc; f->d = d; f->lo = ci->imageExtent; f->big = big; }
        if (!f || !stretch_make(f, ci)) {
            printf("[vulkan] render scale: could not make the small images, drawing full size\n");
            if (f) stretch_free(f);
            HOST(vkDestroySwapchainKHR)(d, *sc, 0);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;  // shortcut: the game sees a failed swapchain; retry at scale 1 by hand
        }
        printf("[vulkan] render scale: game draws %ux%u, stretched to %ux%u (%s)\n", f->lo.width, f->lo.height, big.width, big.height,
               f->fsr ? "FSR 1" : f->filter == VK_FILTER_LINEAR ? "linear" : "nearest");
    }
    return r;
}
static SYSV VkResult s_AcquireNextImageKHR(VkDevice d, VkSwapchainKHR sc, uint64_t t, VkSemaphore sem, VkFence f, uint32_t *i) {
    VkResult r = TIMED(K_ACQ, HOST(vkAcquireNextImageKHR)(d, sc, t, sem, f, i));
    return r == VK_SUBOPTIMAL_KHR ? VK_SUCCESS : r;  // Mocktail NormalizeSwapchainResult: Android rarely sees it, Roblox may recreate in a loop
}
static SYSV VkResult s_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) {
    skate_frame(0);
    Stretch *f = pi->swapchainCount == 1 ? find(pi->pSwapchains[0]) : 0;  // Roblox presents one swapchain
    VkPresentInfoKHR p2;
    if (f) {
        VkSemaphore s = stretch_blit(f, q, pi->pImageIndices[0], pi);
        if (s) { p2 = *pi; p2.waitSemaphoreCount = 1; p2.pWaitSemaphores = &f->done[pi->pImageIndices[0]]; pi = &p2; }
    }
    VkResult r = HOST(vkQueuePresentKHR)(q, pi);
    skate_frame(1);
    return r == VK_SUBOPTIMAL_KHR ? VK_SUCCESS : r;
}

static void *hand(const char *name);
static SYSV PFN_vkVoidFunction s_GetInstanceProcAddr(VkInstance inst, const char *name) {
    if (!load()) return 0;
    void *r = hand(name);
    return r ? r : vk_gen_bind(name, (void *)h_gipa(inst, name));
}
static SYSV PFN_vkVoidFunction s_GetDeviceProcAddr(VkDevice d, const char *name) {
    void *r = hand(name);
    return r ? r : vk_gen_bind(name, (void *)h_gdpa(d, name));
}

static const struct { const char *name; void *at; } table[] = {
    { "vkGetInstanceProcAddr", s_GetInstanceProcAddr }, { "vkGetDeviceProcAddr", s_GetDeviceProcAddr },
    { "vkEnumerateInstanceExtensionProperties", s_EnumerateInstanceExtensionProperties },
    { "vkEnumerateInstanceLayerProperties", s_EnumerateInstanceLayerProperties },
    { "vkCreateInstance", s_CreateInstance }, { "vkCreateAndroidSurfaceKHR", s_CreateAndroidSurfaceKHR },
    { "vkGetPhysicalDeviceSurfacePresentModesKHR", s_GetPhysicalDeviceSurfacePresentModesKHR },
    { "vkCreateSwapchainKHR", s_CreateSwapchainKHR }, { "vkAcquireNextImageKHR", s_AcquireNextImageKHR },
    { "vkQueuePresentKHR", s_QueuePresentKHR },
    { "vkGetDeviceQueue", s_GetDeviceQueue }, { "vkGetDeviceQueue2", s_GetDeviceQueue2 },
    { "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", s_GetPhysicalDeviceSurfaceCapabilitiesKHR },
    { "vkGetSwapchainImagesKHR", s_GetSwapchainImagesKHR }, { "vkDestroySwapchainKHR", s_DestroySwapchainKHR },
    { "vkWaitForFences", s_WaitForFences }, { "vkQueueSubmit", s_QueueSubmit }, { "vkQueueWaitIdle", s_QueueWaitIdle },
    { "vkDeviceWaitIdle", s_DeviceWaitIdle }, { "vkGetQueryPoolResults", s_GetQueryPoolResults },
    { "vkCreateGraphicsPipelines", s_CreateGraphicsPipelines }, { "vkAllocateMemory", s_AllocateMemory }, { "vkCreateImage", s_CreateImage },
};
static void *hand(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(table[i].name, name)) return table[i].at;
    return 0;
}

// dlsym(libvulkan.so, "vk...") lands here through shim_lookup.
void *shim_lookup_vk(const char *name) {
    if (strncmp(name, "vk", 2) || !getenv("SKATESTRAP_VULKAN") || !load()) return 0;
    void *r = hand(name);
    return r ? r : vk_gen_bind(name, (void *)GetProcAddress(vk, name));
}
