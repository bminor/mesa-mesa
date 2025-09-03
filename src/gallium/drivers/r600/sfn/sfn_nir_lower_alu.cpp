/* -*- mesa-c++  -*-
 * Copyright 2022 Collabora LTD
 * Author: Gert Wollny <gert.wollny@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#include "sfn_nir_lower_alu.h"

#include "sfn_nir.h"

namespace r600 {

class Lower2x16 : public NirLowerInstruction {
private:
   bool filter(const nir_instr *instr) const override;
   nir_def *lower(nir_instr *instr) override;
};

bool
Lower2x16::filter(const nir_instr *instr) const
{
   if (instr->type != nir_instr_type_alu)
      return false;
   auto alu = nir_instr_as_alu(instr);
   switch (alu->op) {
   case nir_op_unpack_half_2x16:
   case nir_op_pack_half_2x16:
      return true;
   default:
      return false;
   }
}

nir_def *
Lower2x16::lower(nir_instr *instr)
{
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   switch (alu->op) {
   case nir_op_unpack_half_2x16: {
      nir_def *packed = nir_ssa_for_alu_src(b, alu, 0);
      return nir_vec2(b,
                      nir_unpack_half_2x16_split_x(b, packed),
                      nir_unpack_half_2x16_split_y(b, packed));
   }
   case nir_op_pack_half_2x16: {
      nir_def *src_vec2 = nir_ssa_for_alu_src(b, alu, 0);
      return nir_pack_half_2x16_split(b,
                                      nir_channel(b, src_vec2, 0),
                                      nir_channel(b, src_vec2, 1));
   }
   default:
      UNREACHABLE("Lower2x16 filter doesn't filter correctly");
   }
}

class LowerSinCos : public NirLowerInstruction {
public:
   LowerSinCos(amd_gfx_level gxf_level):
       m_gxf_level(gxf_level)
   {
   }

private:
   bool filter(const nir_instr *instr) const override;
   nir_def *lower(nir_instr *instr) override;
   amd_gfx_level m_gxf_level;
};

bool
LowerSinCos::filter(const nir_instr *instr) const
{
   if (instr->type != nir_instr_type_alu)
      return false;

   auto alu = nir_instr_as_alu(instr);
   switch (alu->op) {
   case nir_op_fsin:
   case nir_op_fcos:
      return true;
   default:
      return false;
   }
}

nir_def *
LowerSinCos::lower(nir_instr *instr)
{
   auto alu = nir_instr_as_alu(instr);

   assert(alu->op == nir_op_fsin || alu->op == nir_op_fcos);

   auto fract = nir_ffract(b,
                           nir_ffma_imm12(b,
                                          nir_ssa_for_alu_src(b, alu, 0),
                                          0.15915494,
                                          0.5));

   auto normalized =
      m_gxf_level != R600
         ? nir_fadd_imm(b, fract, -0.5)
         : nir_ffma_imm12(b, fract, 2.0f * M_PI, -M_PI);

   if (alu->op == nir_op_fsin)
      return nir_fsin_amd(b, normalized);
   else
      return nir_fcos_amd(b, normalized);
}

class FixKcacheIndirectRead : public NirLowerInstruction {
private:
   bool filter(const nir_instr *instr) const override;
   nir_def *lower(nir_instr *instr) override;
};

bool FixKcacheIndirectRead::filter(const nir_instr *instr) const
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   auto intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_ubo)
      return false;

   return nir_src_as_const_value(intr->src[0]) == nullptr;
}

nir_def *FixKcacheIndirectRead::lower(nir_instr *instr)
{
   auto intr = nir_instr_as_intrinsic(instr);
   assert(nir_src_as_const_value(intr->src[0]) == nullptr);

   nir_def *result = &intr->def;
   for (unsigned i = 14; i < b->shader->info.num_ubos; ++i) {
      auto test_bufid = nir_imm_int(b, i);
      auto direct_value =
	    nir_load_ubo(b, intr->num_components,
			 intr->def.bit_size,
			 test_bufid,
			 intr->src[1].ssa);
      auto direct_load = nir_def_as_intrinsic(direct_value);
      nir_intrinsic_copy_const_indices(direct_load, intr);
      result = nir_bcsel(b,
			 nir_ieq(b, test_bufid, intr->src[0].ssa),
	                 direct_value,
	                 result);
   }
   return result;
}

class OptNotFromComparison : public NirLowerInstruction {
private:
   bool filter(const nir_instr *instr) const override;
   nir_def *lower(nir_instr *instr) override;
};

bool
OptNotFromComparison::filter(const nir_instr *instr) const
{
   if (instr->type != nir_instr_type_alu)
      return false;

   auto alu = nir_instr_as_alu(instr);
   if (alu->src[0].src.ssa->parent_instr->type != nir_instr_type_alu)
      return false;

   auto p = nir_def_as_alu(alu->src[0].src.ssa);

   switch (alu->op) {
   case nir_op_inot:
      switch (p->op) {
      case nir_op_flt:
      case nir_op_fge:
      case nir_op_feq:
      case nir_op_fneu:
      case nir_op_ilt:
      case nir_op_ult:
      case nir_op_ige:
      case nir_op_uge:
      case nir_op_ieq:
      case nir_op_ine:
         return true;
      default:
         return false;
      }
   case nir_op_b2f32:
      switch (p->op) {
      case nir_op_fge:
      case nir_op_flt:
      case nir_op_feq:
      case nir_op_fneu:
         return true;
      default:
         return false;
      }
   default:
      return false;
   }

   return true;
}

nir_def *
OptNotFromComparison::lower(nir_instr *instr)
{
   auto alu = nir_instr_as_alu(instr);

   auto p = nir_def_as_alu(alu->src[0].src.ssa);

   auto src0 = nir_channel(b, p->src[0].src.ssa, p->src[0].swizzle[0]);
   auto src1 = nir_channel(b, p->src[1].src.ssa, p->src[1].swizzle[0]);

   switch (alu->op) {
   case nir_op_inot:

      switch (p->op) {
      case nir_op_flt:
         return nir_fge(b, src0, src1);
      case nir_op_fge:
         return nir_flt(b, src0, src1);
      case nir_op_feq:
         return nir_fneu(b, src0, src1);
      case nir_op_fneu:
         return nir_feq(b, src0, src1);

      case nir_op_ilt:
         return nir_ige(b, src0, src1);
      case nir_op_ult:
         return nir_uge(b, src0, src1);

      case nir_op_ige:
         return nir_ilt(b, src0, src1);
      case nir_op_uge:
         return nir_ult(b, src0, src1);

      case nir_op_ieq:
         return nir_ine(b, src0, src1);
      case nir_op_ine:
         return nir_ieq(b, src0, src1);
      default:
         return 0;
      }
   case nir_op_b2f32:
      if (p->src[0].src.ssa->bit_size != 32)
         return 0;
      switch (p->op) {
      case nir_op_fge:
         return nir_sge(b, src0, src1);
      case nir_op_flt:
         return nir_slt(b, src0, src1);
      case nir_op_feq:
         return nir_seq(b, src0, src1);
      case nir_op_fneu:
         return nir_sne(b, src0, src1);
      default:
         return 0;
      }
   default:
      return 0;
   }
   return 0;
}

} // namespace r600

bool
r600_nir_lower_pack_unpack_2x16(nir_shader *shader)
{
   return r600::Lower2x16().run(shader);
}

bool
r600_nir_lower_trigen(nir_shader *shader, amd_gfx_level gfx_level)
{
   return r600::LowerSinCos(gfx_level).run(shader);
}

bool
r600_nir_fix_kcache_indirect_access(nir_shader *shader)
{
   return shader->info.num_ubos > 14 ?
	    r600::FixKcacheIndirectRead().run(shader) : false;
}

bool
r600_nir_opt_compare_results(nir_shader *shader)
{
   return r600::OptNotFromComparison().run(shader);
}
