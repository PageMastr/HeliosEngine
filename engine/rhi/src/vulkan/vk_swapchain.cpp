// Swapchains on SDL3 windows. SDL creates the VkSurfaceKHR (it knows the window system); the RHI
// enabled every surface extension the loader offers at instance creation.
//
// Binary semaphores never leak out: after vkAcquireNextImageKHR an empty submit waits on the
// acquire semaphore and signals the graphics timeline (SwapchainImage::ready); present() does an
// empty submit that waits on the caller's timeline point and signals the per-image present
// semaphore that vkQueuePresentKHR waits on.

#include "vk_device.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <format>
#include <optional>

namespace helios::rhi::vk {
namespace {

/// Preferred format first, then the common 8-bit ones; only formats the RHI can name qualify, so
/// swapchain textures never end up with Format::Unknown. nullopt if the surface offers none.
std::optional<VkSurfaceFormatKHR> chooseFormat(const std::vector<VkSurfaceFormatKHR>& formats, Format requested) {
    const VkFormat wanted[] = {toVkFormat(requested), VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
                               VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    for (VkFormat f : wanted) {
        for (const VkSurfaceFormatKHR& s : formats) {
            if (s.format == f && s.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && fromVkFormat(f) != Format::Unknown) {
                return s;
            }
        }
    }
    for (const VkSurfaceFormatKHR& s : formats) {
        if (fromVkFormat(s.format) != Format::Unknown) return s;
    }
    return std::nullopt;
}

VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes, PresentMode requested) {
    const VkPresentModeKHR wanted = toVkPresentMode(requested);
    return std::find(modes.begin(), modes.end(), wanted) != modes.end() ? wanted : VK_PRESENT_MODE_FIFO_KHR;
}

PresentMode fromVkPresentMode(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_MAILBOX_KHR: return PresentMode::Mailbox;
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return PresentMode::Immediate;
    default: return PresentMode::Fifo;
    }
}

} // namespace

Result<SwapchainH> VulkanDevice::createSwapchain(const SwapchainDesc& desc) {
    if (isDeviceLost()) return Error{ErrorCode::InvalidState, "device lost"};
    if (!m_swapchainExtension) {
        return Error{ErrorCode::Unsupported, "swapchains are unavailable (DeviceDesc::enableSwapchain or no WSI)"};
    }
    if (!desc.sdlWindow) return Error{ErrorCode::InvalidArgument, "createSwapchain: SwapchainDesc::sdlWindow is null"};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(desc.sdlWindow), m_instance, nullptr, &surface)) {
        return makeError(ErrorCode::Unsupported, "SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
    }
    VkBool32 supported = VK_FALSE;
    m_vki.vkGetPhysicalDeviceSurfaceSupportKHR(m_physical, m_queues[0].family, surface, &supported);
    if (!supported) {
        m_vki.vkDestroySurfaceKHR(m_instance, surface, nullptr);
        return Error{ErrorCode::Unsupported, "the graphics queue cannot present to this window"};
    }
    SwapchainRes sc;
    sc.name = desc.name.empty() ? std::string("swapchain") : std::string(desc.name);
    sc.window = desc.sdlWindow;
    sc.surface = surface;
    sc.requestedFormat = desc.format;
    sc.requestedMode = desc.presentMode;
    sc.requestedImageCount = std::max<u32>(desc.imageCount, 2);
    if (auto r = buildSwapchain(sc, desc.width, desc.height); !r) {
        releaseSwapchainImages(sc);
        if (sc.swapchain) m_vk.vkDestroySwapchainKHR(m_device, sc.swapchain, nullptr);
        m_vki.vkDestroySurfaceKHR(m_instance, surface, nullptr);
        return r.error();
    }
    std::lock_guard lock(m_swapchainMutex);
    return m_swapchains.create(std::move(sc));
}

Result<void> VulkanDevice::buildSwapchain(SwapchainRes& sc, u32 width, u32 height) {
    VkSurfaceCapabilitiesKHR caps{};
    VkResult r = m_vki.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical, sc.surface, &caps);
    if (r != VK_SUCCESS) return vkError(r, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(width ? width : 1280u, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height ? height : 720u, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        sc.zeroExtent = true;  // minimized; acquire reports OutOfDate until the next resize
        return {};
    }
    u32 count = 0;
    m_vki.vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, sc.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    m_vki.vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, sc.surface, &count, formats.data());
    formats.resize(count);
    count = 0;
    m_vki.vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, sc.surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    m_vki.vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, sc.surface, &count, modes.data());
    modes.resize(count);

    const std::optional<VkSurfaceFormatKHR> chosen = chooseFormat(formats, sc.requestedFormat);
    if (!chosen) return Error{ErrorCode::Unsupported, "surface offers no format the RHI supports"};
    const VkSurfaceFormatKHR format = *chosen;
    const VkPresentModeKHR mode = choosePresentMode(modes, sc.requestedMode);
    u32 imageCount = std::max(sc.requestedImageCount, caps.minImageCount);
    if (caps.maxImageCount != 0) imageCount = std::min(imageCount, caps.maxImageCount);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    TextureUsage textureUsage = TextureUsage::ColorAttachment;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // screenshots
        textureUsage |= TextureUsage::TransferSrc;
    }
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // blits/clears
        textureUsage |= TextureUsage::TransferDst;
    }
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & static_cast<VkCompositeAlphaFlagsKHR>(alpha))) {
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
            if (caps.supportedCompositeAlpha & static_cast<VkCompositeAlphaFlagsKHR>(a)) {
                alpha = a;
                break;
            }
        }
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = sc.surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = usage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = sc.swapchain;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    r = m_vk.vkCreateSwapchainKHR(m_device, &ci, nullptr, &swapchain);
    if (sc.swapchain) m_vk.vkDestroySwapchainKHR(m_device, sc.swapchain, nullptr);
    sc.swapchain = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) return vkError(r, "vkCreateSwapchainKHR");
    sc.swapchain = swapchain;
    setObjectName(VK_OBJECT_TYPE_SWAPCHAIN_KHR, reinterpret_cast<u64>(swapchain), sc.name);

    m_vk.vkGetSwapchainImagesKHR(m_device, swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    m_vk.vkGetSwapchainImagesKHR(m_device, swapchain, &count, images.data());
    const Format rhiFormat = fromVkFormat(format.format);
    for (u32 i = 0; i < count; ++i) {
        TextureRes t;
        t.image = images[i];
        t.format = format.format;
        t.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        t.name = std::format("{}[{}]", sc.name, i);
        t.desc = TextureDesc::tex2D(rhiFormat, extent.width, extent.height, textureUsage);
        t.swapchainImage = true;
        t.attachmentViews.assign(1, VK_NULL_HANDLE);
        setObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(t.image), t.name);
        const TextureH h = m_textures.create(std::move(t));
        TextureRes* stored = m_textures.get(h);
        stored->desc.name = stored->name;
        sc.images.push_back(h);
    }

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sc.acquireSemaphores.resize(count + 1, VK_NULL_HANDLE);
    sc.acquireSemaphoreValues.assign(count + 1, 0);
    sc.presentSemaphores.resize(count, VK_NULL_HANDLE);
    for (VkSemaphore& s : sc.acquireSemaphores) {
        if (m_vk.vkCreateSemaphore(m_device, &si, nullptr, &s) != VK_SUCCESS) return Error{ErrorCode::Unknown, "vkCreateSemaphore"};
    }
    for (VkSemaphore& s : sc.presentSemaphores) {
        if (m_vk.vkCreateSemaphore(m_device, &si, nullptr, &s) != VK_SUCCESS) return Error{ErrorCode::Unknown, "vkCreateSemaphore"};
    }
    sc.acquireCursor = 0;
    sc.currentImage = ~0u;
    sc.zeroExtent = false;
    sc.info.format = rhiFormat;
    sc.info.width = extent.width;
    sc.info.height = extent.height;
    sc.info.imageCount = count;
    sc.info.presentMode = fromVkPresentMode(mode);
    sc.info.usage = textureUsage;
    return {};
}

void VulkanDevice::releaseSwapchainImages(SwapchainRes& sc) {
    // Callers made sure the GPU is idle.
    for (TextureH h : sc.images) {
        if (TextureRes* t = m_textures.get(h)) destroyTextureNow(*t);
        m_textures.destroy(h);
    }
    sc.images.clear();
    for (VkSemaphore s : sc.acquireSemaphores) {
        if (s) m_vk.vkDestroySemaphore(m_device, s, nullptr);
    }
    for (VkSemaphore s : sc.presentSemaphores) {
        if (s) m_vk.vkDestroySemaphore(m_device, s, nullptr);
    }
    sc.acquireSemaphores.clear();
    sc.acquireSemaphoreValues.clear();
    sc.presentSemaphores.clear();
}

Result<SwapchainImage> VulkanDevice::acquireNextImage(SwapchainH h) {
    if (isDeviceLost()) return Error{ErrorCode::InvalidState, "GPU device lost"};
    std::lock_guard lock(m_swapchainMutex);
    SwapchainRes* sc = m_swapchains.get(h);
    if (!sc) return Error{ErrorCode::InvalidArgument, "acquireNextImage: stale swapchain handle"};
    SwapchainImage out;
    if (sc->zeroExtent || !sc->swapchain) {
        out.status = SwapchainStatus::OutOfDate;
        return out;
    }
    if (sc->currentImage != ~0u) {
        reportError(std::format("acquireNextImage: '{}' image {} was acquired but never presented", sc->name,
                                sc->currentImage));
    }
    const u32 slot = sc->acquireCursor;
    sc->acquireCursor = (slot + 1) % static_cast<u32>(sc->acquireSemaphores.size());
    // The semaphore may be reused only once the bridge submit that waited on it has executed.
    if (sc->acquireSemaphoreValues[slot] != 0) {
        HELIOS_TRY(wait(TimelinePoint{Queue::Graphics, sc->acquireSemaphoreValues[slot]}, ~0ull));
    }
    const VkSemaphore acquired = sc->acquireSemaphores[slot];
    u32 index = 0;
    const VkResult r = m_vk.vkAcquireNextImageKHR(m_device, sc->swapchain, ~0ull, acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        out.status = SwapchainStatus::OutOfDate;
        return out;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return vkError(r, "vkAcquireNextImageKHR");

    QueueSlot& g = m_queues[queueIndex(Queue::Graphics)];
    u64 value = 0;
    {
        std::lock_guard qlock(*g.submitMutex);
        value = g.submitted.load(std::memory_order_relaxed) + 1;
        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = acquired;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = g.timeline;
        signal.value = value;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.waitSemaphoreInfoCount = 1;
        si.pWaitSemaphoreInfos = &wait;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &signal;
        const VkResult sr = m_vk.vkQueueSubmit2(g.queue, 1, &si, VK_NULL_HANDLE);
        if (sr != VK_SUCCESS) return vkError(sr, "vkQueueSubmit2(acquire bridge)");
        g.submitted.store(value, std::memory_order_release);
    }
    {
        std::lock_guard clock(m_contextMutex);
        u64& last = m_frames[m_frameIndex.load(std::memory_order_relaxed) % m_frames.size()].lastSubmitted[0];
        last = std::max(last, value);
    }
    sc->acquireSemaphoreValues[slot] = value;
    sc->currentImage = index;
    out.texture = sc->images[index];
    out.index = index;
    out.ready = TimelinePoint{Queue::Graphics, value};
    out.status = r == VK_SUBOPTIMAL_KHR ? SwapchainStatus::Suboptimal : SwapchainStatus::Ok;
    return out;
}

Result<SwapchainStatus> VulkanDevice::present(SwapchainH h, TimelinePoint after) {
    if (isDeviceLost()) return Error{ErrorCode::InvalidState, "GPU device lost"};
    std::lock_guard lock(m_swapchainMutex);
    SwapchainRes* sc = m_swapchains.get(h);
    if (!sc) return Error{ErrorCode::InvalidArgument, "present: stale swapchain handle"};
    if (sc->currentImage == ~0u) {
        reportError(std::format("present: '{}' has no acquired image", sc->name));
        return Error{ErrorCode::InvalidState, "present without an acquired image"};
    }
    if (!after.isNull() && after.value > m_queues[queueIndex(after.queue)].submitted.load(std::memory_order_acquire)) {
        return Error{ErrorCode::InvalidArgument, "present waits for a timeline value that was never submitted"};
    }
    const u32 index = sc->currentImage;
    sc->currentImage = ~0u;
    QueueSlot& g = m_queues[queueIndex(Queue::Graphics)];
    VkResult pr = VK_SUCCESS;
    u64 value = 0;
    {
        std::lock_guard qlock(*g.submitMutex);
        value = g.submitted.load(std::memory_order_relaxed) + 1;
        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        if (!after.isNull()) {
            wait.semaphore = m_queues[queueIndex(after.queue)].timeline;
            wait.value = after.value;
            wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
        VkSemaphoreSubmitInfo signals[2]{};
        signals[0] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signals[0].semaphore = sc->presentSemaphores[index];
        signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals[1] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signals[1].semaphore = g.timeline;
        signals[1].value = value;
        signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.waitSemaphoreInfoCount = after.isNull() ? 0 : 1;
        si.pWaitSemaphoreInfos = &wait;
        si.signalSemaphoreInfoCount = 2;
        si.pSignalSemaphoreInfos = signals;
        const VkResult sr = m_vk.vkQueueSubmit2(g.queue, 1, &si, VK_NULL_HANDLE);
        if (sr != VK_SUCCESS) return vkError(sr, "vkQueueSubmit2(present bridge)");
        g.submitted.store(value, std::memory_order_release);

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &sc->presentSemaphores[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &sc->swapchain;
        pi.pImageIndices = &index;
        pr = m_vk.vkQueuePresentKHR(g.queue, &pi);
    }
    {
        std::lock_guard clock(m_contextMutex);
        u64& last = m_frames[m_frameIndex.load(std::memory_order_relaxed) % m_frames.size()].lastSubmitted[0];
        last = std::max(last, value);
    }
    switch (pr) {
    case VK_SUCCESS: return SwapchainStatus::Ok;
    case VK_SUBOPTIMAL_KHR: return SwapchainStatus::Suboptimal;
    case VK_ERROR_OUT_OF_DATE_KHR: return SwapchainStatus::OutOfDate;
    default: return vkError(pr, "vkQueuePresentKHR");
    }
}

Result<void> VulkanDevice::resizeSwapchain(SwapchainH h, u32 width, u32 height) {
    if (isDeviceLost()) return Error{ErrorCode::InvalidState, "GPU device lost"};
    HELIOS_TRY(deviceWaitIdle("vkDeviceWaitIdle (resizeSwapchain)"));
    std::lock_guard lock(m_swapchainMutex);
    SwapchainRes* sc = m_swapchains.get(h);
    if (!sc) return Error{ErrorCode::InvalidArgument, "resizeSwapchain: stale swapchain handle"};
    releaseSwapchainImages(*sc);
    return buildSwapchain(*sc, width, height);
}

SwapchainInfo VulkanDevice::swapchainInfo(SwapchainH h) const {
    std::lock_guard lock(m_swapchainMutex);
    const SwapchainRes* sc = m_swapchains.get(h);
    return sc ? sc->info : SwapchainInfo{};
}

void VulkanDevice::destroy(SwapchainH h) {
    (void)deviceWaitIdle("vkDeviceWaitIdle (destroy swapchain)");  // a lost device is idle too
    std::lock_guard lock(m_swapchainMutex);
    SwapchainRes* sc = m_swapchains.get(h);
    if (!sc) return;
    releaseSwapchainImages(*sc);
    if (sc->swapchain) m_vk.vkDestroySwapchainKHR(m_device, sc->swapchain, nullptr);
    if (sc->surface) m_vki.vkDestroySurfaceKHR(m_instance, sc->surface, nullptr);
    m_swapchains.destroy(h);
}

} // namespace helios::rhi::vk
