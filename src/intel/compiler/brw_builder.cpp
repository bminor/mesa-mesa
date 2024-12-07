/*
 * Copyright © 2010 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "brw_builder.h"

/**
 * Get the mask of SIMD channels enabled during dispatch and not yet disabled
 * by discard.  Due to the layout of the sample mask in the fragment shader
 * thread payload, \p bld is required to have a dispatch_width() not greater
 * than 16 for fragment shaders.
 */
brw_reg
brw_sample_mask_reg(const brw_builder &bld)
{
   const fs_visitor &s = *bld.shader;

   if (s.stage != MESA_SHADER_FRAGMENT) {
      return brw_imm_ud(0xffffffff);
   } else if (s.devinfo->ver >= 20 ||
              brw_wm_prog_data(s.prog_data)->uses_kill) {
      return brw_flag_subreg(sample_mask_flag_subreg(s) + bld.group() / 16);
   } else {
      assert(bld.dispatch_width() <= 16);
      assert(s.devinfo->ver < 20);
      return retype(brw_vec1_grf((bld.group() >= 16 ? 2 : 1), 7),
                    BRW_TYPE_UW);
   }
}

/**
 * Predicate the specified instruction on the sample mask.
 */
void
brw_emit_predicate_on_sample_mask(const brw_builder &bld, fs_inst *inst)
{
   assert(bld.shader->stage == MESA_SHADER_FRAGMENT &&
          bld.group() == inst->group &&
          bld.dispatch_width() == inst->exec_size);

   const fs_visitor &s = *bld.shader;
   const brw_reg sample_mask = brw_sample_mask_reg(bld);
   const unsigned subreg = sample_mask_flag_subreg(s);

   if (s.devinfo->ver >= 20 || brw_wm_prog_data(s.prog_data)->uses_kill) {
      assert(sample_mask.file == ARF &&
             sample_mask.nr == brw_flag_subreg(subreg).nr &&
             sample_mask.subnr == brw_flag_subreg(
                subreg + inst->group / 16).subnr);
   } else {
      bld.group(1, 0).exec_all()
         .MOV(brw_flag_subreg(subreg + inst->group / 16), sample_mask);
   }

   if (inst->predicate) {
      assert(inst->predicate == BRW_PREDICATE_NORMAL);
      assert(!inst->predicate_inverse);
      assert(inst->flag_subreg == 0);
      assert(s.devinfo->ver < 20);
      /* Combine the sample mask with the existing predicate by using a
       * vertical predication mode.
       */
      inst->predicate = BRW_PREDICATE_ALIGN1_ALLV;
   } else {
      inst->flag_subreg = subreg;
      inst->predicate = BRW_PREDICATE_NORMAL;
      inst->predicate_inverse = false;
   }
}


brw_reg
brw_fetch_payload_reg(const brw_builder &bld, uint8_t regs[2],
                      brw_reg_type type, unsigned n)
{
   if (!regs[0])
      return brw_reg();

   if (bld.dispatch_width() > 16) {
      const brw_reg tmp = bld.vgrf(type, n);
      const brw_builder hbld = bld.exec_all().group(16, 0);
      const unsigned m = bld.dispatch_width() / hbld.dispatch_width();
      brw_reg *const components = new brw_reg[m * n];

      for (unsigned c = 0; c < n; c++) {
         for (unsigned g = 0; g < m; g++)
            components[c * m + g] =
               offset(retype(brw_vec8_grf(regs[g], 0), type), hbld, c);
      }

      hbld.LOAD_PAYLOAD(tmp, components, m * n, 0);

      delete[] components;
      return tmp;

   } else {
      return brw_reg(retype(brw_vec8_grf(regs[0], 0), type));
   }
}

brw_reg
brw_fetch_barycentric_reg(const brw_builder &bld, uint8_t regs[2])
{
   if (!regs[0])
      return brw_reg();
   else if (bld.shader->devinfo->ver >= 20)
      return brw_fetch_payload_reg(bld, regs, BRW_TYPE_F, 2);

   const brw_reg tmp = bld.vgrf(BRW_TYPE_F, 2);
   const brw_builder hbld = bld.exec_all().group(8, 0);
   const unsigned m = bld.dispatch_width() / hbld.dispatch_width();
   brw_reg *const components = new brw_reg[2 * m];

   for (unsigned c = 0; c < 2; c++) {
      for (unsigned g = 0; g < m; g++)
         components[c * m + g] = offset(brw_vec8_grf(regs[g / 2], 0),
                                        hbld, c + 2 * (g % 2));
   }

   hbld.LOAD_PAYLOAD(tmp, components, 2 * m, 0);

   delete[] components;
   return tmp;
}

void
brw_check_dynamic_msaa_flag(const brw_builder &bld,
                        const struct brw_wm_prog_data *wm_prog_data,
                        enum intel_msaa_flags flag)
{
   fs_inst *inst = bld.AND(bld.null_reg_ud(),
                           brw::dynamic_msaa_flags(wm_prog_data),
                           brw_imm_ud(flag));
   inst->conditional_mod = BRW_CONDITIONAL_NZ;
}

