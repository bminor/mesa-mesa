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

TEST_F(CsBuilderTest, maybe_no_patch)
{
   struct cs_maybe *maybe;
   cs_move32_to(&b, cs_reg32(&b, 42), 0xabad1dea);
   cs_maybe(&b, &maybe) {
      cs_move32_to(&b, cs_reg32(&b, 42), 0xdeadbeef);
   }
   cs_finish(&b);

   uint64_t expected[] = {
      0x022a0000abad1dea, /* MOVE32 r42, #0xabad1dea */
      0x0000000000000000, /* NOP */
   };
   EXPECT_EQ(b.root_chunk.size, ARRAY_SIZE(expected));
   EXPECT_U64_ARRAY_EQUAL(output, expected, ARRAY_SIZE(expected));
}

TEST_F(CsBuilderTest, maybe_patch)
{
   struct cs_maybe *maybe;
   cs_move32_to(&b, cs_reg32(&b, 42), 0xabad1dea);
   cs_maybe(&b, &maybe) {
      cs_move32_to(&b, cs_reg32(&b, 42), 0xdeadbeef);
   }
   cs_patch_maybe(&b, maybe);
   cs_finish(&b);

   uint64_t expected_patched[] = {
      0x022a0000abad1dea, /* MOVE32 r42, #0xabad1dea */
      0x022a0000deadbeef, /* MOVE32 r42, #0xdeadbeef */
   };
   EXPECT_EQ(b.root_chunk.size, ARRAY_SIZE(expected_patched));
   EXPECT_U64_ARRAY_EQUAL(output, expected_patched, ARRAY_SIZE(expected_patched));
}

/* If cs_maybe is called inside a block, we defer calculating the patch
 * address until the outer blocks are closed. */
TEST_F(CsBuilderTest, maybe_inner_block)
{
   struct cs_maybe *maybe;
   cs_move32_to(&b, cs_reg32(&b, 42), 0xabad1dea);
   cs_if(&b, MALI_CS_CONDITION_GREATER, cs_reg32(&b, 42)) {
      cs_maybe(&b, &maybe) {
         cs_move32_to(&b, cs_reg32(&b, 42), 0xdeadbeef);
      }
      cs_move32_to(&b, cs_reg32(&b, 42), 0xabcdef01);
   }
   cs_patch_maybe(&b, maybe);
   cs_finish(&b);

   uint64_t expected_patched[] = {
      0x022a0000abad1dea, /* MOVE32 r42, #0xabad1dea */
      0x16002a0000000002, /* BRANCH le, r42, #0x2 */
      0x022a0000deadbeef, /* MOVE32 r42, #0xdeadbeef */
      0x022a0000abcdef01, /* MOVE32 r42, #0xabcdef01 */
   };
   EXPECT_EQ(b.root_chunk.size, ARRAY_SIZE(expected_patched));
   EXPECT_U64_ARRAY_EQUAL(output, expected_patched, ARRAY_SIZE(expected_patched));
}

/* If cs_patch_maybe is patched before the outer block that cs_maybe was
 * called in is closed, the instructions need to be patched in a different
 * location. */
TEST_F(CsBuilderTest, maybe_early_patch)
{
   struct cs_maybe *maybe;
   cs_move32_to(&b, cs_reg32(&b, 42), 0xabad1dea);
   cs_if(&b, MALI_CS_CONDITION_GREATER, cs_reg32(&b, 42)) {
      cs_maybe(&b, &maybe) {
         cs_move32_to(&b, cs_reg32(&b, 42), 0xdeadbeef);
      }
      cs_move32_to(&b, cs_reg32(&b, 42), 0xabcdef01);

      cs_patch_maybe(&b, maybe);
   }
   cs_finish(&b);

   uint64_t expected_patched[] = {
      0x022a0000abad1dea, /* MOVE32 r42, #0xabad1dea */
      0x16002a0000000002, /* BRANCH le, r42, #0x2 */
      0x022a0000deadbeef, /* MOVE32 r42, #0xdeadbeef */
      0x022a0000abcdef01, /* MOVE32 r42, #0xabcdef01 */
   };
   EXPECT_EQ(b.root_chunk.size, ARRAY_SIZE(expected_patched));
   EXPECT_U64_ARRAY_EQUAL(output, expected_patched, ARRAY_SIZE(expected_patched));
}
