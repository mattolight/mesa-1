/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "tu_private.h"

#include "vk_format.h"
#include "adreno_pm4.xml.h"
#include "tu_cs.h"

void
tu_bo_list_init(struct tu_bo_list *list)
{
   list->count = list->capacity = 0;
   list->bo_infos = NULL;
}

void
tu_bo_list_destroy(struct tu_bo_list *list)
{
   free(list->bo_infos);
}

void
tu_bo_list_reset(struct tu_bo_list *list)
{
   list->count = 0;
}

/**
 * \a flags consists of MSM_SUBMIT_BO_FLAGS.
 */
static uint32_t
tu_bo_list_add_info(struct tu_bo_list *list,
                    const struct drm_msm_gem_submit_bo *bo_info)
{
   for (uint32_t i = 0; i < list->count; ++i) {
      if (list->bo_infos[i].handle == bo_info->handle) {
         assert(list->bo_infos[i].presumed == bo_info->presumed);
         list->bo_infos[i].flags |= bo_info->flags;
         return i;
      }
   }

   /* grow list->bo_infos if needed */
   if (list->count == list->capacity) {
      uint32_t new_capacity = MAX2(2 * list->count, 16);
      struct drm_msm_gem_submit_bo *new_bo_infos = realloc(
         list->bo_infos, new_capacity * sizeof(struct drm_msm_gem_submit_bo));
      if (!new_bo_infos)
         return TU_BO_LIST_FAILED;
      list->bo_infos = new_bo_infos;
      list->capacity = new_capacity;
   }

   list->bo_infos[list->count] = *bo_info;
   return list->count++;
}

uint32_t
tu_bo_list_add(struct tu_bo_list *list,
               const struct tu_bo *bo,
               uint32_t flags)
{
   return tu_bo_list_add_info(list, &(struct drm_msm_gem_submit_bo) {
                                       .flags = flags,
                                       .handle = bo->gem_handle,
                                       .presumed = bo->iova,
                                    });
}

VkResult
tu_bo_list_merge(struct tu_bo_list *list, const struct tu_bo_list *other)
{
   for (uint32_t i = 0; i < other->count; i++) {
      if (tu_bo_list_add_info(list, other->bo_infos + i) == TU_BO_LIST_FAILED)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   return VK_SUCCESS;
}

const struct tu_dynamic_state default_dynamic_state = {
   .viewport =
     {
       .count = 0,
     },
   .scissor =
     {
       .count = 0,
     },
   .line_width = 1.0f,
   .depth_bias =
     {
       .bias = 0.0f,
       .clamp = 0.0f,
       .slope = 0.0f,
     },
   .blend_constants = { 0.0f, 0.0f, 0.0f, 0.0f },
   .depth_bounds =
     {
       .min = 0.0f,
       .max = 1.0f,
     },
   .stencil_compare_mask =
     {
       .front = ~0u,
       .back = ~0u,
     },
   .stencil_write_mask =
     {
       .front = ~0u,
       .back = ~0u,
     },
   .stencil_reference =
     {
       .front = 0u,
       .back = 0u,
     },
};

static void UNUSED /* FINISHME */
tu_bind_dynamic_state(struct tu_cmd_buffer *cmd_buffer,
                      const struct tu_dynamic_state *src)
{
   struct tu_dynamic_state *dest = &cmd_buffer->state.dynamic;
   uint32_t copy_mask = src->mask;
   uint32_t dest_mask = 0;

   tu_use_args(cmd_buffer); /* FINISHME */

   /* Make sure to copy the number of viewports/scissors because they can
    * only be specified at pipeline creation time.
    */
   dest->viewport.count = src->viewport.count;
   dest->scissor.count = src->scissor.count;
   dest->discard_rectangle.count = src->discard_rectangle.count;

   if (copy_mask & TU_DYNAMIC_VIEWPORT) {
      if (memcmp(&dest->viewport.viewports, &src->viewport.viewports,
                 src->viewport.count * sizeof(VkViewport))) {
         typed_memcpy(dest->viewport.viewports, src->viewport.viewports,
                      src->viewport.count);
         dest_mask |= TU_DYNAMIC_VIEWPORT;
      }
   }

   if (copy_mask & TU_DYNAMIC_SCISSOR) {
      if (memcmp(&dest->scissor.scissors, &src->scissor.scissors,
                 src->scissor.count * sizeof(VkRect2D))) {
         typed_memcpy(dest->scissor.scissors, src->scissor.scissors,
                      src->scissor.count);
         dest_mask |= TU_DYNAMIC_SCISSOR;
      }
   }

   if (copy_mask & TU_DYNAMIC_LINE_WIDTH) {
      if (dest->line_width != src->line_width) {
         dest->line_width = src->line_width;
         dest_mask |= TU_DYNAMIC_LINE_WIDTH;
      }
   }

   if (copy_mask & TU_DYNAMIC_DEPTH_BIAS) {
      if (memcmp(&dest->depth_bias, &src->depth_bias,
                 sizeof(src->depth_bias))) {
         dest->depth_bias = src->depth_bias;
         dest_mask |= TU_DYNAMIC_DEPTH_BIAS;
      }
   }

   if (copy_mask & TU_DYNAMIC_BLEND_CONSTANTS) {
      if (memcmp(&dest->blend_constants, &src->blend_constants,
                 sizeof(src->blend_constants))) {
         typed_memcpy(dest->blend_constants, src->blend_constants, 4);
         dest_mask |= TU_DYNAMIC_BLEND_CONSTANTS;
      }
   }

   if (copy_mask & TU_DYNAMIC_DEPTH_BOUNDS) {
      if (memcmp(&dest->depth_bounds, &src->depth_bounds,
                 sizeof(src->depth_bounds))) {
         dest->depth_bounds = src->depth_bounds;
         dest_mask |= TU_DYNAMIC_DEPTH_BOUNDS;
      }
   }

   if (copy_mask & TU_DYNAMIC_STENCIL_COMPARE_MASK) {
      if (memcmp(&dest->stencil_compare_mask, &src->stencil_compare_mask,
                 sizeof(src->stencil_compare_mask))) {
         dest->stencil_compare_mask = src->stencil_compare_mask;
         dest_mask |= TU_DYNAMIC_STENCIL_COMPARE_MASK;
      }
   }

   if (copy_mask & TU_DYNAMIC_STENCIL_WRITE_MASK) {
      if (memcmp(&dest->stencil_write_mask, &src->stencil_write_mask,
                 sizeof(src->stencil_write_mask))) {
         dest->stencil_write_mask = src->stencil_write_mask;
         dest_mask |= TU_DYNAMIC_STENCIL_WRITE_MASK;
      }
   }

   if (copy_mask & TU_DYNAMIC_STENCIL_REFERENCE) {
      if (memcmp(&dest->stencil_reference, &src->stencil_reference,
                 sizeof(src->stencil_reference))) {
         dest->stencil_reference = src->stencil_reference;
         dest_mask |= TU_DYNAMIC_STENCIL_REFERENCE;
      }
   }

   if (copy_mask & TU_DYNAMIC_DISCARD_RECTANGLE) {
      if (memcmp(&dest->discard_rectangle.rectangles,
                 &src->discard_rectangle.rectangles,
                 src->discard_rectangle.count * sizeof(VkRect2D))) {
         typed_memcpy(dest->discard_rectangle.rectangles,
                      src->discard_rectangle.rectangles,
                      src->discard_rectangle.count);
         dest_mask |= TU_DYNAMIC_DISCARD_RECTANGLE;
      }
   }
}

static VkResult
tu_create_cmd_buffer(struct tu_device *device,
                     struct tu_cmd_pool *pool,
                     VkCommandBufferLevel level,
                     VkCommandBuffer *pCommandBuffer)
{
   struct tu_cmd_buffer *cmd_buffer;
   cmd_buffer = vk_zalloc(&pool->alloc, sizeof(*cmd_buffer), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd_buffer == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   cmd_buffer->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   cmd_buffer->device = device;
   cmd_buffer->pool = pool;
   cmd_buffer->level = level;

   if (pool) {
      list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);
      cmd_buffer->queue_family_index = pool->queue_family_index;

   } else {
      /* Init the pool_link so we can safely call list_del when we destroy
       * the command buffer
       */
      list_inithead(&cmd_buffer->pool_link);
      cmd_buffer->queue_family_index = TU_QUEUE_GENERAL;
   }

   tu_bo_list_init(&cmd_buffer->bo_list);
   tu_cs_init(&cmd_buffer->cs);

   *pCommandBuffer = tu_cmd_buffer_to_handle(cmd_buffer);

   list_inithead(&cmd_buffer->upload.list);

   return VK_SUCCESS;
}

static void
tu_cmd_buffer_destroy(struct tu_cmd_buffer *cmd_buffer)
{
   list_del(&cmd_buffer->pool_link);

   for (unsigned i = 0; i < VK_PIPELINE_BIND_POINT_RANGE_SIZE; i++)
      free(cmd_buffer->descriptors[i].push_set.set.mapped_ptr);

   tu_cs_finish(cmd_buffer->device, &cmd_buffer->cs);
   tu_bo_list_destroy(&cmd_buffer->bo_list);
   vk_free(&cmd_buffer->pool->alloc, cmd_buffer);
}

static VkResult
tu_reset_cmd_buffer(struct tu_cmd_buffer *cmd_buffer)
{
   cmd_buffer->record_result = VK_SUCCESS;

   tu_bo_list_reset(&cmd_buffer->bo_list);
   tu_cs_reset(cmd_buffer->device, &cmd_buffer->cs);

   for (unsigned i = 0; i < VK_PIPELINE_BIND_POINT_RANGE_SIZE; i++) {
      cmd_buffer->descriptors[i].dirty = 0;
      cmd_buffer->descriptors[i].valid = 0;
      cmd_buffer->descriptors[i].push_dirty = false;
   }

   cmd_buffer->status = TU_CMD_BUFFER_STATUS_INITIAL;

   return cmd_buffer->record_result;
}

VkResult
tu_AllocateCommandBuffers(VkDevice _device,
                          const VkCommandBufferAllocateInfo *pAllocateInfo,
                          VkCommandBuffer *pCommandBuffers)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_cmd_pool, pool, pAllocateInfo->commandPool);

   VkResult result = VK_SUCCESS;
   uint32_t i;

   for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {

      if (!list_empty(&pool->free_cmd_buffers)) {
         struct tu_cmd_buffer *cmd_buffer = list_first_entry(
            &pool->free_cmd_buffers, struct tu_cmd_buffer, pool_link);

         list_del(&cmd_buffer->pool_link);
         list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);

         result = tu_reset_cmd_buffer(cmd_buffer);
         cmd_buffer->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
         cmd_buffer->level = pAllocateInfo->level;

         pCommandBuffers[i] = tu_cmd_buffer_to_handle(cmd_buffer);
      } else {
         result = tu_create_cmd_buffer(device, pool, pAllocateInfo->level,
                                       &pCommandBuffers[i]);
      }
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      tu_FreeCommandBuffers(_device, pAllocateInfo->commandPool, i,
                            pCommandBuffers);

      /* From the Vulkan 1.0.66 spec:
       *
       * "vkAllocateCommandBuffers can be used to create multiple
       *  command buffers. If the creation of any of those command
       *  buffers fails, the implementation must destroy all
       *  successfully created command buffer objects from this
       *  command, set all entries of the pCommandBuffers array to
       *  NULL and return the error."
       */
      memset(pCommandBuffers, 0,
             sizeof(*pCommandBuffers) * pAllocateInfo->commandBufferCount);
   }

   return result;
}

void
tu_FreeCommandBuffers(VkDevice device,
                      VkCommandPool commandPool,
                      uint32_t commandBufferCount,
                      const VkCommandBuffer *pCommandBuffers)
{
   for (uint32_t i = 0; i < commandBufferCount; i++) {
      TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, pCommandBuffers[i]);

      if (cmd_buffer) {
         if (cmd_buffer->pool) {
            list_del(&cmd_buffer->pool_link);
            list_addtail(&cmd_buffer->pool_link,
                         &cmd_buffer->pool->free_cmd_buffers);
         } else
            tu_cmd_buffer_destroy(cmd_buffer);
      }
   }
}

VkResult
tu_ResetCommandBuffer(VkCommandBuffer commandBuffer,
                      VkCommandBufferResetFlags flags)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   return tu_reset_cmd_buffer(cmd_buffer);
}

VkResult
tu_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                      const VkCommandBufferBeginInfo *pBeginInfo)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   VkResult result = VK_SUCCESS;

   if (cmd_buffer->status != TU_CMD_BUFFER_STATUS_INITIAL) {
      /* If the command buffer has already been resetted with
       * vkResetCommandBuffer, no need to do it again.
       */
      result = tu_reset_cmd_buffer(cmd_buffer);
      if (result != VK_SUCCESS)
         return result;
   }

   memset(&cmd_buffer->state, 0, sizeof(cmd_buffer->state));
   cmd_buffer->usage_flags = pBeginInfo->flags;

   /* setup initial configuration into command buffer */
   if (cmd_buffer->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
      switch (cmd_buffer->queue_family_index) {
      case TU_QUEUE_GENERAL:
         /* init */
         break;
      default:
         break;
      }
   }

   result = tu_cs_begin(cmd_buffer->device, &cmd_buffer->cs, 4096);
   if (result != VK_SUCCESS)
      return result;

   cmd_buffer->status = TU_CMD_BUFFER_STATUS_RECORDING;

   /* Put some stuff in so we do not have empty command buffers. */
   tu_cs_emit_pkt7(&cmd_buffer->cs, CP_NOP, 4);
   tu_cs_emit(&cmd_buffer->cs, 0);
   tu_cs_emit(&cmd_buffer->cs, 0);
   tu_cs_emit(&cmd_buffer->cs, 0);
   tu_cs_emit(&cmd_buffer->cs, 0);

   return VK_SUCCESS;
}

void
tu_CmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                        uint32_t firstBinding,
                        uint32_t bindingCount,
                        const VkBuffer *pBuffers,
                        const VkDeviceSize *pOffsets)
{
}

void
tu_CmdBindIndexBuffer(VkCommandBuffer commandBuffer,
                      VkBuffer buffer,
                      VkDeviceSize offset,
                      VkIndexType indexType)
{
}

void
tu_CmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                         VkPipelineBindPoint pipelineBindPoint,
                         VkPipelineLayout _layout,
                         uint32_t firstSet,
                         uint32_t descriptorSetCount,
                         const VkDescriptorSet *pDescriptorSets,
                         uint32_t dynamicOffsetCount,
                         const uint32_t *pDynamicOffsets)
{
}

void
tu_CmdPushConstants(VkCommandBuffer commandBuffer,
                    VkPipelineLayout layout,
                    VkShaderStageFlags stageFlags,
                    uint32_t offset,
                    uint32_t size,
                    const void *pValues)
{
}

VkResult
tu_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);

   VkResult result = tu_cs_end(&cmd_buffer->cs);
   if (result != VK_SUCCESS)
      cmd_buffer->record_result = result;

   cmd_buffer->status = TU_CMD_BUFFER_STATUS_EXECUTABLE;

   return cmd_buffer->record_result;
}

void
tu_CmdBindPipeline(VkCommandBuffer commandBuffer,
                   VkPipelineBindPoint pipelineBindPoint,
                   VkPipeline _pipeline)
{
}

void
tu_CmdSetViewport(VkCommandBuffer commandBuffer,
                  uint32_t firstViewport,
                  uint32_t viewportCount,
                  const VkViewport *pViewports)
{
}

void
tu_CmdSetScissor(VkCommandBuffer commandBuffer,
                 uint32_t firstScissor,
                 uint32_t scissorCount,
                 const VkRect2D *pScissors)
{
}

void
tu_CmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{
}

void
tu_CmdSetDepthBias(VkCommandBuffer commandBuffer,
                   float depthBiasConstantFactor,
                   float depthBiasClamp,
                   float depthBiasSlopeFactor)
{
}

void
tu_CmdSetBlendConstants(VkCommandBuffer commandBuffer,
                        const float blendConstants[4])
{
}

void
tu_CmdSetDepthBounds(VkCommandBuffer commandBuffer,
                     float minDepthBounds,
                     float maxDepthBounds)
{
}

void
tu_CmdSetStencilCompareMask(VkCommandBuffer commandBuffer,
                            VkStencilFaceFlags faceMask,
                            uint32_t compareMask)
{
}

void
tu_CmdSetStencilWriteMask(VkCommandBuffer commandBuffer,
                          VkStencilFaceFlags faceMask,
                          uint32_t writeMask)
{
}

void
tu_CmdSetStencilReference(VkCommandBuffer commandBuffer,
                          VkStencilFaceFlags faceMask,
                          uint32_t reference)
{
}

void
tu_CmdExecuteCommands(VkCommandBuffer commandBuffer,
                      uint32_t commandBufferCount,
                      const VkCommandBuffer *pCmdBuffers)
{
}

VkResult
tu_CreateCommandPool(VkDevice _device,
                     const VkCommandPoolCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkCommandPool *pCmdPool)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   struct tu_cmd_pool *pool;

   pool = vk_alloc2(&device->alloc, pAllocator, sizeof(*pool), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pool == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   if (pAllocator)
      pool->alloc = *pAllocator;
   else
      pool->alloc = device->alloc;

   list_inithead(&pool->cmd_buffers);
   list_inithead(&pool->free_cmd_buffers);

   pool->queue_family_index = pCreateInfo->queueFamilyIndex;

   *pCmdPool = tu_cmd_pool_to_handle(pool);

   return VK_SUCCESS;
}

void
tu_DestroyCommandPool(VkDevice _device,
                      VkCommandPool commandPool,
                      const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_cmd_pool, pool, commandPool);

   if (!pool)
      return;

   list_for_each_entry_safe(struct tu_cmd_buffer, cmd_buffer,
                            &pool->cmd_buffers, pool_link)
   {
      tu_cmd_buffer_destroy(cmd_buffer);
   }

   list_for_each_entry_safe(struct tu_cmd_buffer, cmd_buffer,
                            &pool->free_cmd_buffers, pool_link)
   {
      tu_cmd_buffer_destroy(cmd_buffer);
   }

   vk_free2(&device->alloc, pAllocator, pool);
}

VkResult
tu_ResetCommandPool(VkDevice device,
                    VkCommandPool commandPool,
                    VkCommandPoolResetFlags flags)
{
   TU_FROM_HANDLE(tu_cmd_pool, pool, commandPool);
   VkResult result;

   list_for_each_entry(struct tu_cmd_buffer, cmd_buffer, &pool->cmd_buffers,
                       pool_link)
   {
      result = tu_reset_cmd_buffer(cmd_buffer);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

void
tu_TrimCommandPool(VkDevice device,
                   VkCommandPool commandPool,
                   VkCommandPoolTrimFlagsKHR flags)
{
   TU_FROM_HANDLE(tu_cmd_pool, pool, commandPool);

   if (!pool)
      return;

   list_for_each_entry_safe(struct tu_cmd_buffer, cmd_buffer,
                            &pool->free_cmd_buffers, pool_link)
   {
      tu_cmd_buffer_destroy(cmd_buffer);
   }
}

void
tu_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                      const VkRenderPassBeginInfo *pRenderPassBegin,
                      VkSubpassContents contents)
{
}

void
tu_CmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer,
                          const VkRenderPassBeginInfo *pRenderPassBeginInfo,
                          const VkSubpassBeginInfoKHR *pSubpassBeginInfo)
{
   tu_CmdBeginRenderPass(commandBuffer, pRenderPassBeginInfo,
                         pSubpassBeginInfo->contents);
}

void
tu_CmdNextSubpass(VkCommandBuffer commandBuffer, VkSubpassContents contents)
{
}

void
tu_CmdNextSubpass2KHR(VkCommandBuffer commandBuffer,
                      const VkSubpassBeginInfoKHR *pSubpassBeginInfo,
                      const VkSubpassEndInfoKHR *pSubpassEndInfo)
{
   tu_CmdNextSubpass(commandBuffer, pSubpassBeginInfo->contents);
}

struct tu_draw_info
{
   /**
    * Number of vertices.
    */
   uint32_t count;

   /**
    * Index of the first vertex.
    */
   int32_t vertex_offset;

   /**
    * First instance id.
    */
   uint32_t first_instance;

   /**
    * Number of instances.
    */
   uint32_t instance_count;

   /**
    * First index (indexed draws only).
    */
   uint32_t first_index;

   /**
    * Whether it's an indexed draw.
    */
   bool indexed;

   /**
    * Indirect draw parameters resource.
    */
   struct tu_buffer *indirect;
   uint64_t indirect_offset;
   uint32_t stride;

   /**
    * Draw count parameters resource.
    */
   struct tu_buffer *count_buffer;
   uint64_t count_buffer_offset;
};

static void
tu_draw(struct tu_cmd_buffer *cmd_buffer, const struct tu_draw_info *info)
{
}

void
tu_CmdDraw(VkCommandBuffer commandBuffer,
           uint32_t vertexCount,
           uint32_t instanceCount,
           uint32_t firstVertex,
           uint32_t firstInstance)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   struct tu_draw_info info = {};

   info.count = vertexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.vertex_offset = firstVertex;

   tu_draw(cmd_buffer, &info);
}

void
tu_CmdDrawIndexed(VkCommandBuffer commandBuffer,
                  uint32_t indexCount,
                  uint32_t instanceCount,
                  uint32_t firstIndex,
                  int32_t vertexOffset,
                  uint32_t firstInstance)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   struct tu_draw_info info = {};

   info.indexed = true;
   info.count = indexCount;
   info.instance_count = instanceCount;
   info.first_index = firstIndex;
   info.vertex_offset = vertexOffset;
   info.first_instance = firstInstance;

   tu_draw(cmd_buffer, &info);
}

void
tu_CmdDrawIndirect(VkCommandBuffer commandBuffer,
                   VkBuffer _buffer,
                   VkDeviceSize offset,
                   uint32_t drawCount,
                   uint32_t stride)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   TU_FROM_HANDLE(tu_buffer, buffer, _buffer);
   struct tu_draw_info info = {};

   info.count = drawCount;
   info.indirect = buffer;
   info.indirect_offset = offset;
   info.stride = stride;

   tu_draw(cmd_buffer, &info);
}

void
tu_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer,
                          VkBuffer _buffer,
                          VkDeviceSize offset,
                          uint32_t drawCount,
                          uint32_t stride)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   TU_FROM_HANDLE(tu_buffer, buffer, _buffer);
   struct tu_draw_info info = {};

   info.indexed = true;
   info.count = drawCount;
   info.indirect = buffer;
   info.indirect_offset = offset;
   info.stride = stride;

   tu_draw(cmd_buffer, &info);
}

struct tu_dispatch_info
{
   /**
    * Determine the layout of the grid (in block units) to be used.
    */
   uint32_t blocks[3];

   /**
    * A starting offset for the grid. If unaligned is set, the offset
    * must still be aligned.
    */
   uint32_t offsets[3];
   /**
    * Whether it's an unaligned compute dispatch.
    */
   bool unaligned;

   /**
    * Indirect compute parameters resource.
    */
   struct tu_buffer *indirect;
   uint64_t indirect_offset;
};

static void
tu_dispatch(struct tu_cmd_buffer *cmd_buffer,
            const struct tu_dispatch_info *info)
{
}

void
tu_CmdDispatchBase(VkCommandBuffer commandBuffer,
                   uint32_t base_x,
                   uint32_t base_y,
                   uint32_t base_z,
                   uint32_t x,
                   uint32_t y,
                   uint32_t z)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   struct tu_dispatch_info info = {};

   info.blocks[0] = x;
   info.blocks[1] = y;
   info.blocks[2] = z;

   info.offsets[0] = base_x;
   info.offsets[1] = base_y;
   info.offsets[2] = base_z;
   tu_dispatch(cmd_buffer, &info);
}

void
tu_CmdDispatch(VkCommandBuffer commandBuffer,
               uint32_t x,
               uint32_t y,
               uint32_t z)
{
   tu_CmdDispatchBase(commandBuffer, 0, 0, 0, x, y, z);
}

void
tu_CmdDispatchIndirect(VkCommandBuffer commandBuffer,
                       VkBuffer _buffer,
                       VkDeviceSize offset)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   TU_FROM_HANDLE(tu_buffer, buffer, _buffer);
   struct tu_dispatch_info info = {};

   info.indirect = buffer;
   info.indirect_offset = offset;

   tu_dispatch(cmd_buffer, &info);
}

void
tu_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
}

void
tu_CmdEndRenderPass2KHR(VkCommandBuffer commandBuffer,
                        const VkSubpassEndInfoKHR *pSubpassEndInfo)
{
   tu_CmdEndRenderPass(commandBuffer);
}

struct tu_barrier_info
{
   uint32_t eventCount;
   const VkEvent *pEvents;
   VkPipelineStageFlags srcStageMask;
};

static void
tu_barrier(struct tu_cmd_buffer *cmd_buffer,
           uint32_t memoryBarrierCount,
           const VkMemoryBarrier *pMemoryBarriers,
           uint32_t bufferMemoryBarrierCount,
           const VkBufferMemoryBarrier *pBufferMemoryBarriers,
           uint32_t imageMemoryBarrierCount,
           const VkImageMemoryBarrier *pImageMemoryBarriers,
           const struct tu_barrier_info *info)
{
}

void
tu_CmdPipelineBarrier(VkCommandBuffer commandBuffer,
                      VkPipelineStageFlags srcStageMask,
                      VkPipelineStageFlags destStageMask,
                      VkBool32 byRegion,
                      uint32_t memoryBarrierCount,
                      const VkMemoryBarrier *pMemoryBarriers,
                      uint32_t bufferMemoryBarrierCount,
                      const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                      uint32_t imageMemoryBarrierCount,
                      const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   struct tu_barrier_info info;

   info.eventCount = 0;
   info.pEvents = NULL;
   info.srcStageMask = srcStageMask;

   tu_barrier(cmd_buffer, memoryBarrierCount, pMemoryBarriers,
              bufferMemoryBarrierCount, pBufferMemoryBarriers,
              imageMemoryBarrierCount, pImageMemoryBarriers, &info);
}

static void
write_event(struct tu_cmd_buffer *cmd_buffer,
            struct tu_event *event,
            VkPipelineStageFlags stageMask,
            unsigned value)
{
}

void
tu_CmdSetEvent(VkCommandBuffer commandBuffer,
               VkEvent _event,
               VkPipelineStageFlags stageMask)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   TU_FROM_HANDLE(tu_event, event, _event);

   write_event(cmd_buffer, event, stageMask, 1);
}

void
tu_CmdResetEvent(VkCommandBuffer commandBuffer,
                 VkEvent _event,
                 VkPipelineStageFlags stageMask)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   TU_FROM_HANDLE(tu_event, event, _event);

   write_event(cmd_buffer, event, stageMask, 0);
}

void
tu_CmdWaitEvents(VkCommandBuffer commandBuffer,
                 uint32_t eventCount,
                 const VkEvent *pEvents,
                 VkPipelineStageFlags srcStageMask,
                 VkPipelineStageFlags dstStageMask,
                 uint32_t memoryBarrierCount,
                 const VkMemoryBarrier *pMemoryBarriers,
                 uint32_t bufferMemoryBarrierCount,
                 const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                 uint32_t imageMemoryBarrierCount,
                 const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   struct tu_barrier_info info;

   info.eventCount = eventCount;
   info.pEvents = pEvents;
   info.srcStageMask = 0;

   tu_barrier(cmd_buffer, memoryBarrierCount, pMemoryBarriers,
              bufferMemoryBarrierCount, pBufferMemoryBarriers,
              imageMemoryBarrierCount, pImageMemoryBarriers, &info);
}

void
tu_CmdSetDeviceMask(VkCommandBuffer commandBuffer, uint32_t deviceMask)
{
   /* No-op */
}