#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define APPEND(...) do { int n = snprintf(out + used, cap - used, __VA_ARGS__); if (n > 0) used += (size_t)n < cap - used ? (size_t)n : cap - used - 1; } while (0)

static int has_ext(const VkExtensionProperties *exts, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; ++i)
        if (strcmp(exts[i].extensionName, name) == 0) return 1;
    return 0;
}

JNIEXPORT jstring JNICALL
Java_com_requiemjota88_vkprobe_MainActivity_probe(JNIEnv *env, jobject self) {
    (void)self;
    const size_t cap = 24000;
    char *out = calloc(1, cap);
    size_t used = 0;
    uint32_t loader = VK_API_VERSION_1_0;
    VkResult vr = vkEnumerateInstanceVersion(&loader);
    APPEND("EoWRecomp / M23 Vulkan capability report\n");
    APPEND("======================================\n");
    APPEND("Vulkan loader: %u.%u.%u (result %d)\n",
           VK_API_VERSION_MAJOR(loader), VK_API_VERSION_MINOR(loader), VK_API_VERSION_PATCH(loader), vr);
    APPEND("Probe API request: 1.1\n\n");

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "EoW Vulkan Probe",
        .apiVersion = loader >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0
    };
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app
    };
    VkInstance instance = VK_NULL_HANDLE;
    vr = vkCreateInstance(&ci, NULL, &instance);
    if (vr != VK_SUCCESS) {
        APPEND("vkCreateInstance failed: %d\n", vr);
        jstring result = (*env)->NewStringUTF(env, out);
        free(out);
        return result;
    }

    uint32_t count = 0;
    vr = vkEnumeratePhysicalDevices(instance, &count, NULL);
    APPEND("Physical-device enumeration: result %d, count %u\n", vr, count);
    if (vr != VK_SUCCESS || count == 0) {
        APPEND("No Vulkan physical device was reported.\n");
    } else {
        VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
        vkEnumeratePhysicalDevices(instance, &count, devices);
        for (uint32_t d = 0; d < count; ++d) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(devices[d], &p);
            APPEND("\nGPU %u: %s\n", d, p.deviceName);
            APPEND("API: %u.%u.%u\n", VK_API_VERSION_MAJOR(p.apiVersion),
                   VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
            APPEND("Driver version: %u (0x%08x)\n", p.driverVersion, p.driverVersion);
            APPEND("Vendor ID: 0x%04x | Device ID: 0x%04x\n", p.vendorID, p.deviceID);
            uint32_t ne = 0;
            VkResult er = vkEnumerateDeviceExtensionProperties(devices[d], NULL, &ne, NULL);
            VkExtensionProperties *exts = er == VK_SUCCESS ? calloc(ne ? ne : 1, sizeof(*exts)) : NULL;
            if (er == VK_SUCCESS && exts) vkEnumerateDeviceExtensionProperties(devices[d], NULL, &ne, exts);
            APPEND("Device extensions (%u):\n", er == VK_SUCCESS ? ne : 0);
            const char *important[] = {
                "VK_KHR_dynamic_rendering",
                "VK_KHR_synchronization2",
                "VK_KHR_timeline_semaphore",
                "VK_KHR_shader_float16_int8",
                "VK_KHR_8bit_storage",
                "VK_EXT_scalar_block_layout",
                "VK_KHR_maintenance4",
                "VK_EXT_shader_demote_to_helper_invocation",
                "VK_KHR_shader_terminate_invocation",
                "VK_EXT_external_memory_host",
                "VK_EXT_custom_border_color",
                "VK_EXT_vertex_attribute_divisor",
                "VK_KHR_sampler_mirror_clamp_to_edge"
            };
            APPEND("\nRequired / useful extension checks:\n");
            for (size_t i = 0; i < sizeof(important) / sizeof(important[0]); ++i)
                APPEND("  %s: %s\n", important[i], has_ext(exts, ne, important[i]) ? "YES" : "NO");
            APPEND("\nFull extension list:\n");
            for (uint32_t i = 0; i < ne; ++i) APPEND("  %s\n", exts[i].extensionName);
            free(exts);
        }
        free(devices);
    }
    vkDestroyInstance(instance, NULL);
    jstring result = (*env)->NewStringUTF(env, out);
    free(out);
    return result;
}
