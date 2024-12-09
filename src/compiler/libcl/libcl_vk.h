/*
 * Copyright 2024 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "libcl.h"

typedef struct VkDrawIndexedIndirectCommand {
   uint32_t indexCount;
   uint32_t instanceCount;
   uint32_t firstIndex;
   int32_t vertexOffset;
   uint32_t firstInstance;
} VkDrawIndexedIndirectCommand __attribute__((aligned(4)));

typedef struct VkDrawIndirectCommand {
   uint32_t vertexCount;
   uint32_t instanceCount;
   uint32_t firstVertex;
   uint32_t firstInstance;
} VkDrawIndirectCommand __attribute__((aligned(4)));
