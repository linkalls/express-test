// SPDX-License-Identifier: MPL-2.0
// Included at the end of VulkanRenderer.cpp.
// Zero-wait native Vulkan GamePad readback prototype.
//
// Reuses Cemu's command-buffer lifecycle and HasCommandBufferFinished()
// instead of calling SubmitCommandBuffer() or WaitCommandBufferFinished().
//
// Supported native source formats: RGBA8 UNORM/SRGB; unsupported formats
// are skipped, never altered. Future support for BGRA, BC, mip views and
// GPU shader conversion requires separate validated paths.
#include "remote/RemoteGamePadVideo.h"

namespace RemoteGamePadAsyncVk
{
struct Slot {
    VkDevice device{VK_NULL_HANDLE};
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    uint8_t* mapped{};
    VkDeviceSize allocationSize{};
    uint64 commandId{};
    int width{};
    int height{};
    bool pending{};
};
inline std::array<Slot,4>& slots() {
    static std::array<Slot,4> ring{};
    return ring;
}
inline uint64 nowUs() {
    using namespace std::chrono;
    return static_cast<uint64>(duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count());
}
inline void destroy(Slot& slot) {
    if (slot.device != VK_NULL_HANDLE) {
        if (slot.mapped) vkUnmapMemory(slot.device, slot.memory);
        if (slot.buffer) vkDestroyBuffer(slot.device, slot.buffer, nullptr);
        if (slot.memory) vkFreeMemory(slot.device, slot.memory, nullptr);
    }
    slot = {};
}
inline bool allocate(VkDevice device, VkPhysicalDevice physicalDevice,
                     Slot& slot, VkDeviceSize bytes) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &slot.buffer) != VK_SUCCESS)
        return false;
    slot.device = device;

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, slot.buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    uint32 type = UINT32_MAX;
    const VkMemoryPropertyFlags wanted =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32 i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) { destroy(slot); return false; }

    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &allocation, nullptr, &slot.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, slot.buffer, slot.memory, 0) != VK_SUCCESS) {
        destroy(slot);
        return false;
    }
    void* data{};
    if (vkMapMemory(device, slot.memory, 0, requirements.size, 0, &data) != VK_SUCCESS) {
        destroy(slot);
        return false;
    }
    slot.mapped = static_cast<uint8_t*>(data);
    slot.allocationSize = bytes;
    return true;
}
} // namespace RemoteGamePadAsyncVk

void VulkanRenderer::RemoteGamePadCaptureAsync(LatteTextureView* textureView, bool padView)
{
    if (!padView || !RemoteGamePadVideo::Connected() || !textureView)
        return;

    const VkDevice device = m_logicalDevice;
    auto& ring = RemoteGamePadAsyncVk::slots();

    // Poll only Cemu's completed command-buffer serials. NO GPU WAIT.
    for (auto& slot : ring) {
        if (!slot.pending) continue;
        if (slot.device != device || !HasCommandBufferFinished(slot.commandId))
            continue;
        if (slot.mapped && slot.width > 0 && slot.height > 0) {
            const size_t pixels = static_cast<size_t>(slot.width) * slot.height;
            std::vector<uint8_t> rgb(pixels * 3);
            for (size_t pixel = 0; pixel < pixels; ++pixel) {
                const uint8_t* source = slot.mapped + pixel * 4;
                uint8_t* destination = rgb.data() + pixel * 3;
                destination[0] = source[0];
                destination[1] = source[1];
                destination[2] = source[2];
            }
            RemoteGamePadVideo::OfferFrame(std::move(rgb), slot.width, slot.height);
        }
        slot.pending = false;
    }

    static uint64 lastCaptureUs = 0;
    const uint64 currentUs = RemoteGamePadAsyncVk::nowUs();
    if (currentUs - lastCaptureUs < 16'000)
        return;
    lastCaptureUs = currentUs;

    auto* view = static_cast<LatteTextureViewVk*>(textureView);
    if (view->firstMip != 0)
        return;
    auto base = view->GetBaseImage();
    if (!base)
        return;
    const VkFormat format = base->GetFormat();
    if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB)
        return;

    int width = 0, height = 0;
    base->GetEffectiveSize(width, height, 0);
    if (width < 2 || height < 2 || width > 1920 || height > 1080)
        return;

    RemoteGamePadAsyncVk::Slot* slot = nullptr;
    for (auto& candidate : ring) {
        if (!candidate.pending) { slot = &candidate; break; }
    }
    if (!slot)
        return; // Skip capture rather than blocking Cemu's GPU thread.

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;
    if (slot->device != device || slot->allocationSize < bytes) {
        RemoteGamePadAsyncVk::destroy(*slot);
        if (!RemoteGamePadAsyncVk::allocate(device, m_physicalDevice, *slot, bytes))
            return;
    }

    auto imageObj = base->GetImageObj();
    if (!imageObj)
        return;
    imageObj->flagForCurrentCommandBuffer();
    const VkImage image = imageObj->m_image;

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = 0;
    copy.imageSubresource.baseArrayLayer = view->firstSlice;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {static_cast<uint32>(width), static_cast<uint32>(height), 1};

    // These transitions mirror VulkanRenderer::HandleScreenshotRequest().
    // The source texture belongs to Cemu; restore its original layout.
    barrier_image<IMAGE_WRITE | TRANSFER_WRITE, TRANSFER_READ>(
        base, copy.imageSubresource, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdCopyImageToBuffer(m_state.currentCommandBuffer, image,
                           VK_IMAGE_LAYOUT_GENERAL, slot->buffer, 1, &copy);
    barrier_image<TRANSFER_READ, TRANSFER_WRITE | IMAGE_WRITE>(
        base, copy.imageSubresource, base->GetDefaultLayout());
    slot->commandId = GetCurrentCommandBufferId();
    slot->width = width;
    slot->height = height;
    slot->pending = true;
}
