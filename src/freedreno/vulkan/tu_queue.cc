/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 */

#include "tu_queue.h"

#include "tu_knl.h"
#include "tu_device.h"

#include "vk_util.h"

static int
tu_get_submitqueue_priority(const struct tu_physical_device *pdevice,
                            VkQueueGlobalPriorityKHR global_priority,
                            bool global_priority_query)
{
   if (global_priority_query) {
      VkQueueFamilyGlobalPriorityPropertiesKHR props;
      tu_physical_device_get_global_priority_properties(pdevice, &props);

      bool valid = false;
      for (uint32_t i = 0; i < props.priorityCount; i++) {
         if (props.priorities[i] == global_priority) {
            valid = true;
            break;
         }
      }

      if (!valid)
         return -1;
   }

   /* Valid values are from 0 to (pdevice->submitqueue_priority_count - 1),
    * with 0 being the highest priority.  This matches what freedreno does.
    */
   int priority;
   if (global_priority == VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR)
      priority = pdevice->submitqueue_priority_count / 2;
   else if (global_priority < VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR)
      priority = pdevice->submitqueue_priority_count - 1;
   else
      priority = 0;

   return priority;
}

VkResult
tu_queue_init(struct tu_device *device,
              struct tu_queue *queue,
              int idx,
              const VkDeviceQueueCreateInfo *create_info)
{
   const VkDeviceQueueGlobalPriorityCreateInfoKHR *priority_info =
      vk_find_struct_const(create_info->pNext,
            DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR);
   const VkQueueGlobalPriorityKHR global_priority = priority_info ?
      priority_info->globalPriority :
      (TU_DEBUG(HIPRIO) ? VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR :
       VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR);

   const int priority = tu_get_submitqueue_priority(
         device->physical_device, global_priority,
         device->vk.enabled_features.globalPriorityQuery);
   if (priority < 0) {
      return vk_startup_errorf(device->instance, VK_ERROR_INITIALIZATION_FAILED,
                               "invalid global priority");
   }

   VkResult result = vk_queue_init(&queue->vk, &device->vk, create_info, idx);
   if (result != VK_SUCCESS)
      return result;

   queue->device = device;
   queue->priority = priority;
   queue->vk.driver_submit = tu_queue_submit;

   int ret = tu_drm_submitqueue_new(device, priority, &queue->msm_queue_id);
   if (ret)
      return vk_startup_errorf(device->instance, VK_ERROR_INITIALIZATION_FAILED,
                               "submitqueue create failed");

   queue->fence = -1;

   return VK_SUCCESS;
}

void
tu_queue_finish(struct tu_queue *queue)
{
   vk_queue_finish(&queue->vk);
   tu_drm_submitqueue_close(queue->device, queue->msm_queue_id);
}

