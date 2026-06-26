/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"
#include "swapchain.hpp"

using namespace lsfgvk;
using namespace lsfgvk::layer;

namespace {
inline VkImageMemoryBarrier barrierHelper(VkImage handle,
                                          VkAccessFlags srcAccess,
                                          VkAccessFlags dstAccess,
                                          VkImageLayout oldLayout,
                                          VkImageLayout newLayout) {
  return {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .pNext = nullptr,
          .srcAccessMask = srcAccess,
          .dstAccessMask = (newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) ? 0u : dstAccess,
          .oldLayout = oldLayout,
          .newLayout = newLayout,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = handle,
          .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .baseMipLevel = 0,
                               .levelCount = 1,
                               .baseArrayLayer = 0,
                               .layerCount = 1}};
}
}

void layer::context_ModifySwapchainCreateInfo(
    const ls::GameConf& profile, uint32_t maxImages,
    VkSwapchainCreateInfoKHR& createInfo) {
  createInfo.imageUsage |= (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  createInfo.minImageCount = std::max<uint32_t>(createInfo.minImageCount, profile.multiplier + 1);
  if (maxImages > 0) createInfo.minImageCount = std::min(createInfo.minImageCount, maxImages);

  switch (profile.pacing) {
    case ls::Pacing::Mailbox:
      createInfo.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
      break;
    case ls::Pacing::FIFO:
      createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
      break;
    case ls::Pacing::FIFORelaxed:
      createInfo.presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
      break;
    case ls::Pacing::Immediate:
      createInfo.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
      break;
  }
}

Swapchain::Swapchain(const vk::Vulkan& vk, backend::Instance& backend,
                     ls::GameConf profile, SwapchainInfo info)
    : instance(backend), profile(std::move(profile)), info(std::move(info)) {
  const VkExtent2D extent = this->info.extent;
  const bool hdr = this->info.format > 57;
  const VkFormat vkFormat = hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
  std::vector<int> sourceFds(2);
  std::vector<int> destinationFds(this->profile.multiplier - 1);
  this->sourceImages.reserve(2);
  for (int& fd : sourceFds)
    this->sourceImages.emplace_back(vk, extent, vkFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, std::nullopt, &fd);
  this->destinationImages.reserve(destinationFds.size());
  for (int& fd : destinationFds)
    this->destinationImages.emplace_back(vk, extent, vkFormat, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, std::nullopt, &fd);
  int syncFd{};
  this->syncSemaphore.emplace(vk, 0, std::nullopt, &syncFd);
  try {
    this->ctx = ls::owned_ptr<ls::R<backend::Context>>(
        new ls::R<backend::Context>(backend.openContext(
            {sourceFds[0], sourceFds[1]}, destinationFds, syncFd, extent.width,
            extent.height, hdr, 1.0F / this->profile.flow_scale,
            this->profile.performance_mode)),
        [backend = &backend](ls::R<backend::Context>& ctx) {
          backend->closeContext(ctx);
        });
    backend::makeLeaking();
  } catch (const std::exception& e) {
    throw ls::error("failed to create swapchain context", e);
  }
  this->renderCommandBuffer.emplace(vk);
  this->renderFence.emplace(vk);
  this->passes.reserve(this->destinationImages.size());
  for (size_t i = 0; i < this->destinationImages.size(); i++) {
    this->passes.push_back({.commandBuffer = vk::CommandBuffer(vk), .acquireSemaphore = vk::Semaphore(vk)});
  }
  const size_t semCount = std::max(this->info.images.size(), this->destinationImages.size() + 1);
  this->postCopySemaphores.reserve(semCount);
  for (size_t i = 0; i < semCount; i++) {
    this->postCopySemaphores.emplace_back(vk::Semaphore(vk), vk::Semaphore(vk));
  }
}

VkResult Swapchain::present(const vk::Vulkan& vk, VkQueue queue,
                            VkSwapchainKHR swapchain, void* next_chain,
                            uint32_t imageIdx,
                            const std::vector<VkSemaphore>& semaphores) {
  const VkImage swapImg = this->info.images[imageIdx];
  const auto& srcImg = this->sourceImages[this->fidx & 1];
  
  try {
    this->instance.get().scheduleFrames(this->ctx.get());
  } catch (const std::exception& e) {
    throw ls::error("failed to schedule frames", e);
  }

  // override present mode in next chain if the application tries to change it
  {
    VkPresentModeKHR targetMode;
    switch (this->profile.pacing) {
      case ls::Pacing::Mailbox:     targetMode = VK_PRESENT_MODE_MAILBOX_KHR; break;
      case ls::Pacing::FIFO:        targetMode = VK_PRESENT_MODE_FIFO_KHR; break;
      case ls::Pacing::FIFORelaxed: targetMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR; break;
      case ls::Pacing::Immediate:   targetMode = VK_PRESENT_MODE_IMMEDIATE_KHR; break;
    }
    auto* modeInfo = reinterpret_cast<VkSwapchainPresentModeInfoEXT*>(next_chain);
    while (modeInfo) {
      if (modeInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT) {
        for (uint32_t i = 0; i < modeInfo->swapchainCount; ++i)
          const_cast<VkPresentModeKHR*>(modeInfo->pPresentModes)[i] = targetMode;
      }
      modeInfo = reinterpret_cast<VkSwapchainPresentModeInfoEXT*>(const_cast<void*>(modeInfo->pNext));
    }
  }

  auto& cmdbuf = *this->renderCommandBuffer;
  cmdbuf.begin(vk);
  cmdbuf.copyImage(
      vk,
      {barrierHelper(swapImg, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
       barrierHelper(srcImg.handle(), 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)},
      {swapImg, srcImg.handle()}, srcImg.getExtent(),
      {barrierHelper(swapImg, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)});
  cmdbuf.end(vk);

  if (this->fidx && !this->renderFence->wait(vk, 100ULL * 1000 * 1000))
    throw ls::vulkan_error(VK_TIMEOUT, "vkWaitForFences() failed");
  
  this->renderFence->reset(vk);

  cmdbuf.submit(vk, semaphores, VK_NULL_HANDLE, 0, {}, this->syncSemaphore->handle(), this->idx++);
  
  const size_t destCount = this->destinationImages.size();
  const size_t pcCount = this->postCopySemaphores.size();
  
  static thread_local std::vector<VkSemaphore> waitSems;

  for (size_t i = 0; i < destCount; ++i) {
    auto& pcs = this->postCopySemaphores[this->idx % pcCount];
    auto& dstImg = this->destinationImages[i];
    auto& pass = this->passes[i];
    uint32_t aqIdx{};
    
    VkResult res = vk.df().AcquireNextImageKHR(vk.dev(), swapchain, UINT64_MAX, pass.acquireSemaphore.handle(), VK_NULL_HANDLE, &aqIdx);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) throw ls::vulkan_error(res, "vkAcquireNextImageKHR() failed");
    
    const VkImage acqImg = this->info.images[aqIdx];
    pass.commandBuffer.begin(vk);
    pass.commandBuffer.copyImage(
        vk,
        {barrierHelper(dstImg.handle(), 0, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
         barrierHelper(acqImg, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)},
        {dstImg.handle(), acqImg}, dstImg.getExtent(),
        {barrierHelper(acqImg, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)});
    pass.commandBuffer.end(vk);

    waitSems.clear();
    waitSems.push_back(pass.acquireSemaphore.handle());
    if (i > 0) waitSems.push_back(this->postCopySemaphores[(this->idx - 1) % pcCount].second.handle());
    
    pass.commandBuffer.submit(
        vk, waitSems, this->syncSemaphore->handle(), this->idx,
        {pcs.first.handle(), pcs.second.handle()}, VK_NULL_HANDLE, 0,
        (i == destCount - 1) ? this->renderFence->handle() : VK_NULL_HANDLE);

    VkPresentInfoKHR pInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = i ? nullptr : next_chain, .waitSemaphoreCount = 1, .pWaitSemaphores = &pcs.first.handle(), .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &aqIdx};
    res = vk.df().QueuePresentKHR(queue, &pInfo);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
      throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");
    this->idx++;
  }

  auto& lastPCS = this->postCopySemaphores[(this->idx - 1) % pcCount];
  VkPresentInfoKHR finalPresent{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &lastPCS.second.handle(), .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &imageIdx};
  VkResult finalRes = vk.df().QueuePresentKHR(queue, &finalPresent);
  if (finalRes != VK_SUCCESS && finalRes != VK_SUBOPTIMAL_KHR)
    throw ls::vulkan_error(finalRes, "vkQueuePresentKHR() failed");

  this->fidx++;
  return finalRes;
}