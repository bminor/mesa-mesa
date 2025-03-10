/*
 * Copyright 2025 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "test_helpers.h"
#include "brw_builder.h"

class algebraic_test : public brw_shader_pass_test {};

TEST_F(algebraic_test, imax_a_a)
{
   brw_builder bld = make_shader(MESA_SHADER_FRAGMENT, 16);
   brw_builder exp = make_shader(MESA_SHADER_FRAGMENT, 16);

   brw_reg dst0 = vgrf(bld, exp, BRW_TYPE_D);
   brw_reg src0 = vgrf(bld, exp, BRW_TYPE_D);

   bld.emit_minmax(dst0, src0, src0, BRW_CONDITIONAL_GE);

   EXPECT_PROGRESS(brw_opt_algebraic, bld);

   exp.MOV(dst0, src0);

   EXPECT_SHADERS_MATCH(bld, exp);
}
