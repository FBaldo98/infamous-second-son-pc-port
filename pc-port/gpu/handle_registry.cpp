// SPDX-License-Identifier: GPL-2.0-or-later
// Diagnostics: which image views and samplers exist. The NVIDIA driver faulted on handles pushed
// by native pass draws (vkCmdPushDescriptorSetKHR / vkUpdateDescriptorSets, 2026-10-20); a fault
// report looks each handle up here to tell a live one from a destroyed one. The create and
// destroy entry points of the default dispatcher are wrapped once the device exists (vulkan-hpp
// handles, including their unique deleters, call through it). Bookkeeping happens on create and
// destroy only; draws never look anything up.
#include "handle_registry.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "video_core/renderer_vulkan/vk_common.h"

namespace iss::handles {
namespace {

struct Entry {
    bool live;
    u64 changed_ns;
};

std::shared_mutex mutex;
std::unordered_map<u64, Entry> views, samplers;

PFN_vkCreateImageView create_view;
PFN_vkDestroyImageView destroy_view;
PFN_vkCreateSampler create_sampler;
PFN_vkDestroySampler destroy_sampler;

u64 NowNs() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

void Mark(std::unordered_map<u64, Entry>& map, u64 handle, bool live) {
    if (!handle) return;
    std::unique_lock lock{mutex};
    map[handle] = Entry{live, NowNs()};
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device, const VkImageViewCreateInfo* info,
                                               const VkAllocationCallbacks* allocator,
                                               VkImageView* view) {
    const VkResult result = create_view(device, info, allocator, view);
    if (result == VK_SUCCESS) Mark(views, reinterpret_cast<u64>(*view), true);
    return result;
}
VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice device, VkImageView view,
                                            const VkAllocationCallbacks* allocator) {
    Mark(views, reinterpret_cast<u64>(view), false);
    destroy_view(device, view, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(VkDevice device, const VkSamplerCreateInfo* info,
                                             const VkAllocationCallbacks* allocator,
                                             VkSampler* sampler) {
    const VkResult result = create_sampler(device, info, allocator, sampler);
    if (result == VK_SUCCESS) Mark(samplers, reinterpret_cast<u64>(*sampler), true);
    return result;
}
VKAPI_ATTR void VKAPI_CALL DestroySampler(VkDevice device, VkSampler sampler,
                                          const VkAllocationCallbacks* allocator) {
    Mark(samplers, reinterpret_cast<u64>(sampler), false);
    destroy_sampler(device, sampler, allocator);
}

const char* Describe(const std::unordered_map<u64, Entry>& map, u64 handle, char* text,
                     size_t size) {
    if (!handle) return "null";
    std::shared_lock lock{mutex};
    const auto it = map.find(handle);
    if (it == map.end()) return "UNKNOWN";
    const double ms = double(NowNs() - it->second.changed_ns) / 1e6;
    std::snprintf(text, size, "%s %.1f ms ago", it->second.live ? "LIVE, created" : "DESTROYED", ms);
    return text;
}

} // namespace

void Install() {
    auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    if (create_view || !d.vkCreateImageView || !d.vkDestroyImageView || !d.vkCreateSampler ||
        !d.vkDestroySampler)
        return;
    create_view = d.vkCreateImageView;
    destroy_view = d.vkDestroyImageView;
    create_sampler = d.vkCreateSampler;
    destroy_sampler = d.vkDestroySampler;
    d.vkCreateImageView = CreateImageView;
    d.vkDestroyImageView = DestroyImageView;
    d.vkCreateSampler = CreateSampler;
    d.vkDestroySampler = DestroySampler;
}

bool IsLiveView(u64 handle) {
    if (!handle || !create_view) return true;
    std::shared_lock lock{mutex};
    const auto it = views.find(handle);
    return it == views.end() || it->second.live;
}

const char* DescribeView(u64 handle, char* text, size_t size) {
    return Describe(views, handle, text, size);
}
const char* DescribeSampler(u64 handle, char* text, size_t size) {
    return Describe(samplers, handle, text, size);
}

} // namespace iss::handles
