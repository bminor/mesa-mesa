/*
 * Copyright 2025 Collabora Ltd
 * SPDX-License-Identifier: MIT
 */

#include "cs_builder.h"

#include <gtest/gtest.h>
#include "mesa-gtest-extras.h"

#define MAX_OUTPUT_SIZE 512

class CsBuilderTest : public ::testing::Test {
public:
   CsBuilderTest();
   ~CsBuilderTest();

   struct cs_builder b;
   uint64_t *output;
};

CsBuilderTest::CsBuilderTest()
{
   output = new uint64_t[MAX_OUTPUT_SIZE];
   struct cs_builder_conf conf = {
      .nr_registers = 96,
      .nr_kernel_registers = 4,
   };
   struct cs_buffer buffer = {
      .cpu = output,
      .gpu = 0x0,
      .capacity = MAX_OUTPUT_SIZE,
   };
   cs_builder_init(&b, &conf, buffer);
}

CsBuilderTest::~CsBuilderTest()
{
   delete output;
}

TEST_F(CsBuilderTest, basic)
{
   cs_move32_to(&b, cs_reg32(&b, 42), 0xdeadbeef);
   cs_finish(&b);

   uint64_t expected[] = {
      0x022a0000deadbeef, /* MOVE32 r42, #0xdeadbeef */
   };

   EXPECT_EQ(b.root_chunk.size, ARRAY_SIZE(expected));
   EXPECT_U64_ARRAY_EQUAL(output, expected, ARRAY_SIZE(expected));
}
