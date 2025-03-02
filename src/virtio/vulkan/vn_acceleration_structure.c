/*
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vn_acceleration_structure.h"

#include "vn_device.h"

VkResult
vn_BuildAccelerationStructuresKHR(
   VkDevice device,
   VkDeferredOperationKHR deferredOperation,
   uint32_t infoCount,
   const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
   const VkAccelerationStructureBuildRangeInfoKHR *const *ppBuildRangeInfos)
{
   struct vn_device *dev = vn_device_from_handle(device);
   unreachable("Unimplemented");
   return vn_error(dev->instance, VK_ERROR_FEATURE_NOT_PRESENT);
}

VkResult
vn_CopyAccelerationStructureKHR(
   VkDevice device,
   VkDeferredOperationKHR deferredOperation,
   const VkCopyAccelerationStructureInfoKHR *pInfo)
{
   struct vn_device *dev = vn_device_from_handle(device);
   unreachable("Unimplemented");
   return vn_error(dev->instance, VK_ERROR_FEATURE_NOT_PRESENT);
}

VkResult
vn_CopyAccelerationStructureToMemoryKHR(
   VkDevice device,
   VkDeferredOperationKHR deferredOperation,
   const VkCopyAccelerationStructureToMemoryInfoKHR *pInfo)
{
   struct vn_device *dev = vn_device_from_handle(device);
   unreachable("Unimplemented");
   return vn_error(dev->instance, VK_ERROR_FEATURE_NOT_PRESENT);
}

VkResult
vn_CopyMemoryToAccelerationStructureKHR(
   VkDevice device,
   VkDeferredOperationKHR deferredOperation,
   const VkCopyMemoryToAccelerationStructureInfoKHR *pInfo)
{
   struct vn_device *dev = vn_device_from_handle(device);
   unreachable("Unimplemented");
   return vn_error(dev->instance, VK_ERROR_FEATURE_NOT_PRESENT);
}

VkResult
vn_WriteAccelerationStructuresPropertiesKHR(
   VkDevice device,
   uint32_t accelerationStructureCount,
   const VkAccelerationStructureKHR *pAccelerationStructures,
   VkQueryType queryType,
   size_t dataSize,
   void *pData,
   size_t stride)
{
   struct vn_device *dev = vn_device_from_handle(device);
   unreachable("Unimplemented");
   return vn_error(dev->instance, VK_ERROR_FEATURE_NOT_PRESENT);
}
