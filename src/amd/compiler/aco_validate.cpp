/*
 * Copyright © 2018 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_ir.h"

#include "util/memstream.h"
#include "util/ralloc.h"

#include <array>
#include <map>
#include <set>
#include <vector>

namespace aco {

static void
aco_log(Program* program, enum aco_compiler_debug_level level, const char* prefix, const char* file,
        unsigned line, const char* fmt, va_list args)
{
   char* msg;

   if (program->debug.shorten_messages) {
      msg = ralloc_vasprintf(NULL, fmt, args);
   } else {
      msg = ralloc_strdup(NULL, prefix);
      ralloc_asprintf_append(&msg, "    In file %s:%u\n", file, line);
      ralloc_asprintf_append(&msg, "    ");
      ralloc_vasprintf_append(&msg, fmt, args);
   }

   if (program->debug.func)
      program->debug.func(program->debug.private_data, level, msg);

   fprintf(program->debug.output, "%s\n", msg);

   ralloc_free(msg);
}

void
_aco_err(Program* program, const char* file, unsigned line, const char* fmt, ...)
{
   va_list args;

   va_start(args, fmt);
   aco_log(program, ACO_COMPILER_DEBUG_LEVEL_ERROR, "ACO ERROR:\n", file, line, fmt, args);
   va_end(args);
}

bool
validate_ir(Program* program)
{
   bool is_valid = true;
   auto check = [&program, &is_valid](bool success, const char* msg,
                                      aco::Instruction* instr) -> void
   {
      if (!success) {
         char* out;
         size_t outsize;
         struct u_memstream mem;
         u_memstream_open(&mem, &out, &outsize);
         FILE* const memf = u_memstream_get(&mem);

         fprintf(memf, "%s: ", msg);
         aco_print_instr(program->gfx_level, instr, memf);
         u_memstream_close(&mem);

         aco_err(program, "%s", out);
         free(out);

         is_valid = false;
      }
   };

   /* check reachability */
   if (program->progress < CompilationProgress::after_lower_to_hw) {
      std::map<uint32_t, std::pair<uint32_t, bool>> def_blocks;
      for (Block& block : program->blocks) {
         for (aco_ptr<Instruction>& instr : block.instructions) {
            for (Definition def : instr->definitions) {
               if (!def.isTemp())
                  continue;
               check(!def_blocks.count(def.tempId()), "Temporary defined twice", instr.get());
               def_blocks[def.tempId()] = std::make_pair(block.index, false);
            }
         }
      }

      for (Block& block : program->blocks) {
         for (aco_ptr<Instruction>& instr : block.instructions) {
            for (unsigned i = 0; i < instr->operands.size(); i++) {
               Operand op = instr->operands[i];
               if (!op.isTemp())
                  continue;

               uint32_t use_block_idx = block.index;
               if (instr->opcode == aco_opcode::p_phi || instr->opcode == aco_opcode::p_boolean_phi)
                  use_block_idx = block.logical_preds[i];
               else if (instr->opcode == aco_opcode::p_linear_phi)
                  use_block_idx = block.linear_preds[i];

               auto it = def_blocks.find(op.tempId());
               if (it != def_blocks.end()) {
                  Block& def_block = program->blocks[it->second.first];
                  Block& use_block = program->blocks[use_block_idx];
                  bool dominates =
                     def_block.index == use_block_idx
                        ? (use_block_idx == block.index ? it->second.second : true)
                        : (op.regClass().is_linear() ? dominates_linear(def_block, use_block)
                                                     : dominates_logical(def_block, use_block));
                  if (!dominates) {
                     char msg[256];
                     snprintf(msg, sizeof(msg), "Definition of %%%u does not dominate use",
                              op.tempId());
                     check(false, msg, instr.get());
                  }
               } else {
                  char msg[256];
                  snprintf(msg, sizeof(msg), "%%%u never defined", op.tempId());
                  check(false, msg, instr.get());
               }
            }

            for (Definition def : instr->definitions) {
               if (def.isTemp())
                  def_blocks[def.tempId()].second = true;
            }
         }
      }
   }

   for (Block& block : program->blocks) {
      for (aco_ptr<Instruction>& instr : block.instructions) {

         if (program->progress < CompilationProgress::after_lower_to_hw) {
            for (const Operand& op : instr->operands)
               check(!op.isTemp() || op.regClass() == program->temp_rc[op.tempId()],
                     "Operand RC not consistent.", instr.get());

            for (const Definition& def : instr->definitions)
               check(!def.isTemp() || def.regClass() == program->temp_rc[def.tempId()],
                     "Definition RC not consistent.", instr.get());
         }

         const aco_alu_opcode_info& opcode_info = instr_info.alu_opcode_infos[(int)instr->opcode];

         if (opcode_info.num_defs) {
            unsigned num_defs = opcode_info.num_defs;
            /* Before GFX10 v_cmpx also writes VCC. */
            if (instr->isVOPC() && program->gfx_level < GFX10 &&
                opcode_info.def_fixed_reg[0] == fixed_exec)
               num_defs = 2;

            check(num_defs >= instr->definitions.size(), "Too many definitions", instr.get());
            check(num_defs <= instr->definitions.size(), "Too few definitions", instr.get());
            num_defs = MIN2(num_defs, instr->definitions.size());

            for (unsigned i = 0; i < num_defs; i++) {
               aco_type type;
               fixed_reg fixed_reg;
               if (instr->isVOPC() && program->gfx_level < GFX10 &&
                   opcode_info.def_fixed_reg[0] == fixed_exec) {
                  type = opcode_info.def_types[0];
                  fixed_reg = i == 0 ? not_fixed : fixed_exec;
               } else {
                  type = opcode_info.def_types[i];
                  fixed_reg = opcode_info.def_fixed_reg[i];
               }

               if (fixed_reg == fixed_m0) {
                  check(instr->definitions[i].isFixed() && instr->definitions[i].physReg() == m0,
                        "Definition needs m0", instr.get());
               } else if (fixed_reg == fixed_scc) {
                  check(instr->definitions[i].isFixed() && instr->definitions[i].physReg() == scc,
                        "Definition needs scc", instr.get());
               } else if (fixed_reg == fixed_exec) {
                  RegClass rc = type.bit_size == 1 ? program->lane_mask
                                                   : RegClass::get(RegType::sgpr, type.bytes());
                  check(instr->definitions[i].isFixed() &&
                           instr->definitions[i].physReg() == exec &&
                           instr->definitions[i].regClass() == rc,
                        "Definition needs exec", instr.get());
               } else if (type.bit_size == 1) {
                  check(instr->definitions[i].regClass() == program->lane_mask,
                        "Definition has to be lane mask", instr.get());
                  check(!instr->definitions[i].isFixed() ||
                           instr->definitions[i].physReg() == vcc || instr->isVOP3() ||
                           instr->isSDWA(),
                        "Definition has to be vcc", instr.get());
               } else {
                  check(instr->definitions[i].size() == type.dwords(), "Definition has wrong size",
                        instr.get());
               }
            }
         }

         if (opcode_info.num_operands) {
            unsigned num_ops = opcode_info.num_operands;
            check(num_ops >= instr->operands.size(), "Too many operands", instr.get());
            check(num_ops <= instr->operands.size(), "Too few operands", instr.get());
            num_ops = MIN2(num_ops, instr->operands.size());

            for (unsigned i = 0; i < num_ops; i++) {
               aco_type type = opcode_info.op_types[i];
               fixed_reg fixed_reg = opcode_info.op_fixed_reg[i];

               if (fixed_reg == fixed_m0) {
                  check(instr->operands[i].isFixed() && instr->operands[i].physReg() == m0,
                        "Operand needs m0", instr.get());
               } else if (fixed_reg == fixed_scc) {
                  check(instr->operands[i].isFixed() && instr->operands[i].physReg() == scc,
                        "Operand needs scc", instr.get());
               } else if (fixed_reg == fixed_exec) {
                  RegClass rc = type.bit_size == 1 ? program->lane_mask
                                                   : RegClass::get(RegType::sgpr, type.bytes());
                  check(instr->operands[i].isFixed() && instr->operands[i].physReg() == exec &&
                           instr->operands[i].hasRegClass() && instr->operands[i].regClass() == rc,
                        "Operand needs exec", instr.get());
               } else if (type.bit_size == 1) {
                  check(instr->operands[i].hasRegClass() &&
                           instr->operands[i].regClass() == program->lane_mask,
                        "Operand has to be lane mask", instr.get());
                  check(!instr->operands[i].isFixed() || instr->operands[i].physReg() == vcc ||
                           instr->isVOP3(),
                        "Operand has to be vcc", instr.get());
               } else if (fixed_reg == fixed_imm) {
                  check(instr->operands[i].isLiteral(), "Operand has to be literal", instr.get());
               } else {
                  check(instr->operands[i].size() == type.dwords() ||
                           (instr->operands[i].isFixed() && instr->operands[i].physReg() >= 128 &&
                            instr->operands[i].physReg() < 256),
                        "Operand has wrong size", instr.get());
               }
            }
         }

         /* check base format */
         Format base_format = instr->format;
         base_format = (Format)((uint32_t)base_format & ~(uint32_t)Format::SDWA);
         base_format = (Format)((uint32_t)base_format & ~(uint32_t)Format::DPP16);
         base_format = (Format)((uint32_t)base_format & ~(uint32_t)Format::DPP8);
         if ((uint32_t)base_format & (uint32_t)Format::VOP1)
            base_format = Format::VOP1;
         else if ((uint32_t)base_format & (uint32_t)Format::VOP2)
            base_format = Format::VOP2;
         else if ((uint32_t)base_format & (uint32_t)Format::VOPC)
            base_format = Format::VOPC;
         else if (base_format == Format::VINTRP) {
            if (instr->opcode == aco_opcode::v_interp_p1ll_f16 ||
                instr->opcode == aco_opcode::v_interp_p1lv_f16 ||
                instr->opcode == aco_opcode::v_interp_p2_legacy_f16 ||
                instr->opcode == aco_opcode::v_interp_p2_f16 ||
                instr->opcode == aco_opcode::v_interp_p2_hi_f16) {
               /* v_interp_*_fp16 are considered VINTRP by the compiler but
                * they are emitted as VOP3.
                */
               base_format = Format::VOP3;
            } else {
               base_format = Format::VINTRP;
            }
         }
         check(base_format == instr_info.format[(int)instr->opcode],
               "Wrong base format for instruction", instr.get());

         /* check VOP3 modifiers */
         if (instr->isVOP3() && withoutDPP(instr->format) != Format::VOP3) {
            check(base_format == Format::VOP2 || base_format == Format::VOP1 ||
                     base_format == Format::VOPC || base_format == Format::VINTRP,
                  "Format cannot have VOP3/VOP3B applied", instr.get());
         }

         if (instr->isDPP()) {
            check(base_format == Format::VOP2 || base_format == Format::VOP1 ||
                     base_format == Format::VOPC || base_format == Format::VOP3 ||
                     base_format == Format::VOP3P,
                  "Format cannot have DPP applied", instr.get());
            check((!instr->isVOP3() && !instr->isVOP3P()) || program->gfx_level >= GFX11,
                  "VOP3+DPP is GFX11+ only", instr.get());

            bool fi =
               instr->isDPP8() ? instr->dpp8().fetch_inactive : instr->dpp16().fetch_inactive;
            check(!fi || program->gfx_level >= GFX10, "DPP Fetch-Inactive is GFX10+ only",
                  instr.get());
         }

         /* check SDWA */
         if (instr->isSDWA()) {
            check(base_format == Format::VOP2 || base_format == Format::VOP1 ||
                     base_format == Format::VOPC,
                  "Format cannot have SDWA applied", instr.get());

            check(program->gfx_level >= GFX8, "SDWA is GFX8 to GFX10.3 only", instr.get());
            check(program->gfx_level < GFX11, "SDWA is GFX8 to GFX10.3 only", instr.get());

            SDWA_instruction& sdwa = instr->sdwa();
            check(sdwa.omod == 0 || program->gfx_level >= GFX9, "SDWA omod only supported on GFX9+",
                  instr.get());
            if (base_format == Format::VOPC) {
               check(sdwa.clamp == false || program->gfx_level == GFX8,
                     "SDWA VOPC clamp only supported on GFX8", instr.get());
               check((instr->definitions[0].isFixed() && instr->definitions[0].physReg() == vcc) ||
                        program->gfx_level >= GFX9,
                     "SDWA+VOPC definition must be fixed to vcc on GFX8", instr.get());
            } else {
               const Definition& def = instr->definitions[0];
               check(def.bytes() <= 4, "SDWA definitions must not be larger than 4 bytes",
                     instr.get());
               check(def.bytes() >= sdwa.dst_sel.size() + sdwa.dst_sel.offset(),
                     "SDWA definition selection size must be at most definition size", instr.get());
               check(
                  sdwa.dst_sel.size() == 1 || sdwa.dst_sel.size() == 2 || sdwa.dst_sel.size() == 4,
                  "SDWA definition selection size must be 1, 2 or 4 bytes", instr.get());
               check(sdwa.dst_sel.offset() % sdwa.dst_sel.size() == 0, "Invalid selection offset",
                     instr.get());
               check(def.bytes() == 4 || def.bytes() == sdwa.dst_sel.size(),
                     "SDWA dst_sel size must be definition size for subdword definitions",
                     instr.get());
               check(def.bytes() == 4 || sdwa.dst_sel.offset() == 0,
                     "SDWA dst_sel offset must be 0 for subdword definitions", instr.get());
            }

            for (unsigned i = 0; i < std::min<unsigned>(2, instr->operands.size()); i++) {
               const Operand& op = instr->operands[i];
               check(op.bytes() <= 4, "SDWA operands must not be larger than 4 bytes", instr.get());
               check(op.bytes() >= sdwa.sel[i].size() + sdwa.sel[i].offset(),
                     "SDWA operand selection size must be at most operand size", instr.get());
               check(sdwa.sel[i].size() == 1 || sdwa.sel[i].size() == 2 || sdwa.sel[i].size() == 4,
                     "SDWA operand selection size must be 1, 2 or 4 bytes", instr.get());
               check(sdwa.sel[i].offset() % sdwa.sel[i].size() == 0, "Invalid selection offset",
                     instr.get());
            }
            if (instr->operands.size() >= 3) {
               check(instr->operands[2].isFixed() && instr->operands[2].physReg() == vcc,
                     "3rd operand must be fixed to vcc with SDWA", instr.get());
            }
            if (instr->definitions.size() >= 2) {
               check(instr->definitions[1].isFixed() && instr->definitions[1].physReg() == vcc,
                     "2nd definition must be fixed to vcc with SDWA", instr.get());
            }

            const bool sdwa_opcodes =
               instr->opcode != aco_opcode::v_fmac_f32 && instr->opcode != aco_opcode::v_fmac_f16 &&
               instr->opcode != aco_opcode::v_fmamk_f32 &&
               instr->opcode != aco_opcode::v_fmaak_f32 &&
               instr->opcode != aco_opcode::v_fmamk_f16 &&
               instr->opcode != aco_opcode::v_fmaak_f16 &&
               instr->opcode != aco_opcode::v_madmk_f32 &&
               instr->opcode != aco_opcode::v_madak_f32 &&
               instr->opcode != aco_opcode::v_madmk_f16 &&
               instr->opcode != aco_opcode::v_madak_f16 &&
               instr->opcode != aco_opcode::v_readfirstlane_b32 &&
               instr->opcode != aco_opcode::v_clrexcp && instr->opcode != aco_opcode::v_swap_b32;

            const bool feature_mac =
               program->gfx_level == GFX8 &&
               (instr->opcode == aco_opcode::v_mac_f32 && instr->opcode == aco_opcode::v_mac_f16);

            check(sdwa_opcodes || feature_mac, "SDWA can't be used with this opcode", instr.get());
         }

         /* check opsel */
         if (instr->opcode == aco_opcode::v_permlane16_b32 ||
             instr->opcode == aco_opcode::v_permlanex16_b32) {
            check(instr->valu().opsel <= 0x3, "Unexpected opsel for permlane", instr.get());
         } else if (instr->isVOP3() || instr->isVOP1() || instr->isVOP2() || instr->isVOPC()) {
            VALU_instruction& valu = instr->valu();
            check(valu.opsel == 0 || program->gfx_level >= GFX9, "Opsel is only supported on GFX9+",
                  instr.get());
            check(valu.opsel == 0 || instr->format == Format::VOP3 || program->gfx_level >= GFX11,
                  "Opsel is only supported for VOP3 before GFX11", instr.get());

            for (unsigned i = 0; i < 3; i++) {
               if (i >= instr->operands.size() ||
                   (!instr->isVOP3() && !instr->operands[i].isOfType(RegType::vgpr)) ||
                   (instr->operands[i].hasRegClass() &&
                    instr->operands[i].regClass().is_subdword() && !instr->operands[i].isFixed()))
                  check(!valu.opsel[i], "Unexpected opsel for operand", instr.get());
            }
            if (!instr->definitions.empty() && instr->definitions[0].regClass().is_subdword() &&
                !instr->definitions[0].isFixed())
               check(!valu.opsel[3], "Unexpected opsel for sub-dword definition", instr.get());
         } else if (instr->opcode == aco_opcode::v_fma_mixlo_f16 ||
                    instr->opcode == aco_opcode::v_fma_mixhi_f16 ||
                    instr->opcode == aco_opcode::v_fma_mix_f32) {
            check(instr->definitions[0].regClass() ==
                     (instr->opcode == aco_opcode::v_fma_mix_f32 ? v1 : v2b),
                  "v_fma_mix_f32/v_fma_mix_f16 must have v1/v2b definition", instr.get());
         } else if (instr->isVOP3P()) {
            VALU_instruction& vop3p = instr->valu();
            for (unsigned i = 0; i < instr->operands.size(); i++) {
               if (instr->operands[i].hasRegClass() &&
                   instr->operands[i].regClass().is_subdword() && !instr->operands[i].isFixed())
                  check(!vop3p.opsel_lo[i] && !vop3p.opsel_hi[i],
                        "Unexpected opsel for subdword operand", instr.get());
            }
            check(instr->definitions[0].regClass() == v1 ||
                     instr_info.classes[(int)instr->opcode] == instr_class::wmma,
                  "VOP3P must have v1 definition", instr.get());
         }

         /* check for undefs */
         for (unsigned i = 0; i < instr->operands.size(); i++) {
            if (instr->operands[i].isUndefined()) {
               bool flat = instr->isFlatLike();
               bool can_be_undef = is_phi(instr) || instr->isEXP() || instr->isReduction() ||
                                   instr->opcode == aco_opcode::p_create_vector ||
                                   instr->opcode == aco_opcode::p_start_linear_vgpr ||
                                   instr->opcode == aco_opcode::p_jump_to_epilog ||
                                   instr->opcode == aco_opcode::p_dual_src_export_gfx11 ||
                                   instr->opcode == aco_opcode::p_end_with_regs ||
                                   (instr->opcode == aco_opcode::p_interp_gfx11 && i == 0) ||
                                   (instr->opcode == aco_opcode::p_bpermute_permlane && i == 0) ||
                                   (flat && i == 1) || (instr->isMIMG() && (i == 1 || i == 2)) ||
                                   ((instr->isMUBUF() || instr->isMTBUF()) && i == 1) ||
                                   (instr->isScratch() && i == 0) || (instr->isDS() && i == 0) ||
                                   (instr->opcode == aco_opcode::p_init_scratch && i == 0);
               check(can_be_undef, "Undefs can only be used in certain operands", instr.get());
            } else {
               check(instr->operands[i].isFixed() || instr->operands[i].isTemp() ||
                        instr->operands[i].isConstant(),
                     "Uninitialized Operand", instr.get());
            }
         }

         for (Operand& op : instr->operands) {
            if (op.isFixed() || !op.hasRegClass() || !op.regClass().is_linear_vgpr() ||
                op.isUndefined())
               continue;

            /* Only kill linear VGPRs in top-level blocks. Otherwise, we might have to move linear
             * VGPRs to make space for normal ones and that isn't possible inside control flow. */
            if (op.isKill()) {
               check(block.kind & block_kind_top_level,
                     "Linear VGPR operands must only be killed at top-level blocks", instr.get());
            }
         }

         /* check subdword definitions */
         for (unsigned i = 0; i < instr->definitions.size(); i++) {
            if (instr->definitions[i].regClass().is_subdword())
               check(instr->definitions[i].bytes() <= 4 || instr->isPseudo() || instr->isVMEM(),
                     "Only Pseudo and VMEM instructions can write subdword registers > 4 bytes",
                     instr.get());
         }

         if ((instr->isSALU() && instr->opcode != aco_opcode::p_constaddr_addlo &&
              instr->opcode != aco_opcode::p_resumeaddr_addlo) ||
             instr->isVALU()) {
            /* check literals */
            Operand literal(s1);
            for (unsigned i = 0; i < instr->operands.size(); i++) {
               Operand op = instr->operands[i];
               if (!op.isLiteral())
                  continue;

               check(!instr->isDPP() && !instr->isSDWA() &&
                        (!instr->isVOP3() || program->gfx_level >= GFX10) &&
                        (!instr->isVOP3P() || program->gfx_level >= GFX10),
                     "Literal applied on wrong instruction format", instr.get());

               check(literal.isUndefined() || (literal.size() == op.size() &&
                                               literal.constantValue() == op.constantValue()),
                     "Only 1 Literal allowed", instr.get());
               literal = op;
               check(instr->isSALU() || instr->isVOP3() || instr->isVOP3P() || i == 0 || i == 2,
                     "Wrong source position for Literal argument", instr.get());
            }

            /* check num sgprs for VALU */
            if (instr->isVALU()) {
               bool is_shift64 = instr->opcode == aco_opcode::v_lshlrev_b64_e64 ||
                                 instr->opcode == aco_opcode::v_lshlrev_b64 ||
                                 instr->opcode == aco_opcode::v_lshrrev_b64 ||
                                 instr->opcode == aco_opcode::v_ashrrev_i64;
               unsigned const_bus_limit = 1;
               if (program->gfx_level >= GFX10 && !is_shift64)
                  const_bus_limit = 2;

               uint32_t scalar_mask;
               if (instr->isVOP3() || instr->isVOP3P())
                  scalar_mask = 0x7;
               else if (instr->isSDWA())
                  scalar_mask = program->gfx_level >= GFX9 ? 0x7 : 0x4;
               else if (instr->opcode == aco_opcode::v_movrels_b32 ||
                        instr->opcode == aco_opcode::v_movrelsd_b32 ||
                        instr->opcode == aco_opcode::v_movrelsd_2_b32)
                  scalar_mask = 0x2;
               else if (instr->isVINTERP_INREG())
                  scalar_mask = 0x0;
               else
                  scalar_mask = 0x5;

               if (instr->isDPP())
                  scalar_mask &= 0x4; /* TODO 0x6 for GFX11.5+ */

               if (instr->isVOPC() || instr->opcode == aco_opcode::v_readfirstlane_b32 ||
                   instr->opcode == aco_opcode::v_readlane_b32 ||
                   instr->opcode == aco_opcode::v_readlane_b32_e64 ||
                   instr_info.classes[(int)instr->opcode] ==
                      instr_class::valu_pseudo_scalar_trans) {
                  check(instr->definitions[0].regClass().type() == RegType::sgpr,
                        "Wrong Definition type for VALU instruction", instr.get());
               } else {
                  if (!instr->definitions.empty())
                     check(instr->definitions[0].regClass().type() == RegType::vgpr,
                           "Wrong Definition type for VALU instruction", instr.get());
               }

               unsigned num_sgprs = 0;
               unsigned sgpr[] = {0, 0};
               for (unsigned i = 0; i < instr->operands.size(); i++) {
                  Operand op = instr->operands[i];
                  if (instr->opcode == aco_opcode::v_readfirstlane_b32 ||
                      instr->opcode == aco_opcode::v_readlane_b32 ||
                      instr->opcode == aco_opcode::v_readlane_b32_e64) {
                     check(i != 1 || op.isOfType(RegType::sgpr) || op.isConstant(),
                           "Must be a SGPR or a constant", instr.get());
                     check(i == 1 || (op.isOfType(RegType::vgpr) && op.bytes() <= 4),
                           "Wrong Operand type for VALU instruction", instr.get());
                     continue;
                  }
                  if (instr->opcode == aco_opcode::v_permlane16_b32 ||
                      instr->opcode == aco_opcode::v_permlanex16_b32 ||
                      instr->opcode == aco_opcode::v_permlane64_b32) {
                     check(i != 0 || op.isOfType(RegType::vgpr),
                           "Operand 0 of v_permlane must be VGPR", instr.get());
                     check(i == 0 || op.isOfType(RegType::sgpr) || op.isConstant(),
                           "Lane select operands of v_permlane must be SGPR or constant",
                           instr.get());
                  }

                  if (instr->opcode == aco_opcode::v_writelane_b32 ||
                      instr->opcode == aco_opcode::v_writelane_b32_e64) {
                     check(i != 2 || (op.isOfType(RegType::vgpr) && op.bytes() <= 4),
                           "Wrong Operand type for VALU instruction", instr.get());
                     check(i == 2 || op.isOfType(RegType::sgpr) || op.isConstant(),
                           "Must be a SGPR or a constant", instr.get());
                     continue;
                  }
                  if (op.isOfType(RegType::sgpr)) {
                     check(scalar_mask & (1 << i), "Wrong source position for SGPR argument",
                           instr.get());

                     if (op.tempId() != sgpr[0] && op.tempId() != sgpr[1]) {
                        if (num_sgprs < 2)
                           sgpr[num_sgprs++] = op.tempId();
                     }
                  }

                  if (op.isConstant() && !op.isLiteral())
                     check(scalar_mask & (1 << i), "Wrong source position for constant argument",
                           instr.get());
               }
               check(num_sgprs + (literal.isUndefined() ? 0 : 1) <= const_bus_limit,
                     "Too many SGPRs/literals", instr.get());

               /* Validate modifiers. */
               check(!instr->valu().opsel || instr->isVOP3() || instr->isVOP1() ||
                        instr->isVOP2() || instr->isVOPC() || instr->isVINTERP_INREG(),
                     "OPSEL set for unsupported instruction format", instr.get());
               check(!instr->valu().opsel_lo || instr->isVOP3P(),
                     "OPSEL_LO set for unsupported instruction format", instr.get());
               check(!instr->valu().opsel_hi || instr->isVOP3P(),
                     "OPSEL_HI set for unsupported instruction format", instr.get());
               check(!instr->valu().omod || instr->isVOP3() || instr->isSDWA(),
                     "OMOD set for unsupported instruction format", instr.get());
               check(!instr->valu().clamp || instr->isVOP3() || instr->isVOP3P() ||
                        instr->isSDWA() || instr->isVINTERP_INREG(),
                     "CLAMP set for unsupported instruction format", instr.get());

               for (bool abs : instr->valu().abs) {
                  check(!abs || instr->isVOP3() || instr->isVOP3P() || instr->isSDWA() ||
                           instr->isDPP16(),
                        "ABS/NEG_HI set for unsupported instruction format", instr.get());
               }
               for (bool neg : instr->valu().neg) {
                  check(!neg || instr->isVOP3() || instr->isVOP3P() || instr->isSDWA() ||
                           instr->isDPP16() || instr->isVINTERP_INREG(),
                        "NEG/NEG_LO set for unsupported instruction format", instr.get());
               }
            }

            if (instr->isSOP1() || instr->isSOP2()) {
               if (!instr->definitions.empty())
                  check(instr->definitions[0].regClass().type() == RegType::sgpr,
                        "Wrong Definition type for SALU instruction", instr.get());
               for (const Operand& op : instr->operands) {
                  check(op.isConstant() || op.isOfType(RegType::sgpr),
                        "Wrong Operand type for SALU instruction", instr.get());
               }
            }
         }

         switch (instr->format) {
         case Format::PSEUDO: {
            if (instr->opcode == aco_opcode::p_create_vector ||
                instr->opcode == aco_opcode::p_start_linear_vgpr) {
               unsigned size = 0;
               for (const Operand& op : instr->operands) {
                  check(op.bytes() < 4 || size % 4 == 0, "Operand is not aligned", instr.get());
                  size += op.bytes();
               }
               if (!instr->operands.empty() || instr->opcode == aco_opcode::p_create_vector) {
                  check(size == instr->definitions[0].bytes(),
                        "Definition size does not match operand sizes", instr.get());
               }
               if (instr->definitions[0].regClass().type() == RegType::sgpr) {
                  for (const Operand& op : instr->operands) {
                     check(op.isConstant() || op.regClass().type() == RegType::sgpr,
                           "Wrong Operand type for scalar vector", instr.get());
                  }
               }
               if (instr->opcode == aco_opcode::p_start_linear_vgpr)
                  check(instr->definitions[0].regClass().is_linear_vgpr(),
                        "Definition must be linear VGPR", instr.get());
            } else if (instr->opcode == aco_opcode::p_extract_vector) {
               check(!instr->operands[0].isConstant() && instr->operands[1].isConstant(),
                     "Wrong Operand types", instr.get());
               check((instr->operands[1].constantValue() + 1) * instr->definitions[0].bytes() <=
                        instr->operands[0].bytes(),
                     "Index out of range", instr.get());
               check(instr->definitions[0].regClass().type() == RegType::vgpr ||
                        instr->operands[0].regClass().type() == RegType::sgpr,
                     "Cannot extract SGPR value from VGPR vector", instr.get());
               check(program->gfx_level >= GFX9 ||
                        !instr->definitions[0].regClass().is_subdword() ||
                        instr->operands[0].regClass().type() == RegType::vgpr,
                     "Cannot extract subdword from SGPR before GFX9+", instr.get());
            } else if (instr->opcode == aco_opcode::p_split_vector) {
               check(!instr->operands[0].isConstant(), "Operand must not be constant", instr.get());
               unsigned size = 0;
               for (const Definition& def : instr->definitions) {
                  size += def.bytes();
               }
               check(size == instr->operands[0].bytes(),
                     "Operand size does not match definition sizes", instr.get());
               if (instr->operands[0].isOfType(RegType::vgpr)) {
                  for (const Definition& def : instr->definitions)
                     check(def.regClass().type() == RegType::vgpr,
                           "Wrong Definition type for VGPR split_vector", instr.get());
               } else {
                  for (const Definition& def : instr->definitions)
                     check(program->gfx_level >= GFX9 || !def.regClass().is_subdword(),
                           "Cannot split SGPR into subdword VGPRs before GFX9+", instr.get());
               }
            } else if (instr->opcode == aco_opcode::p_parallelcopy) {
               check(instr->definitions.size() == instr->operands.size(),
                     "Number of Operands does not match number of Definitions", instr.get());
               for (unsigned i = 0; i < instr->operands.size(); i++) {
                  check(instr->definitions[i].bytes() == instr->operands[i].bytes(),
                        "Operand and Definition size must match", instr.get());
                  if (instr->operands[i].hasRegClass()) {
                     check((instr->definitions[i].regClass().type() ==
                            instr->operands[i].regClass().type()) ||
                              (instr->definitions[i].regClass().type() == RegType::vgpr &&
                               instr->operands[i].regClass().type() == RegType::sgpr),
                           "Operand and Definition types do not match", instr.get());
                     check(instr->definitions[i].regClass().is_linear_vgpr() ==
                              instr->operands[i].regClass().is_linear_vgpr(),
                           "Operand and Definition types do not match", instr.get());
                  } else {
                     check(!instr->definitions[i].regClass().is_linear_vgpr(),
                           "Can only copy linear VGPRs into linear VGPRs, not constant/undef",
                           instr.get());
                  }
               }
            } else if (instr->opcode == aco_opcode::p_phi) {
               check(instr->operands.size() == block.logical_preds.size(),
                     "Number of Operands does not match number of predecessors", instr.get());
               check(instr->definitions[0].regClass().type() == RegType::vgpr,
                     "Logical Phi Definition must be vgpr", instr.get());
               for (const Operand& op : instr->operands)
                  check(instr->definitions[0].size() == op.size(),
                        "Operand sizes must match Definition size", instr.get());
            } else if (instr->opcode == aco_opcode::p_linear_phi) {
               for (const Operand& op : instr->operands) {
                  check(!op.isTemp() || op.getTemp().is_linear(), "Wrong Operand type",
                        instr.get());
                  check(instr->definitions[0].size() == op.size(),
                        "Operand sizes must match Definition size", instr.get());
               }
               check(instr->operands.size() == block.linear_preds.size(),
                     "Number of Operands does not match number of predecessors", instr.get());
            } else if (instr->opcode == aco_opcode::p_extract ||
                       instr->opcode == aco_opcode::p_insert) {
               check(!instr->operands[0].isConstant(), "Data operand must not be constant",
                     instr.get());
               check(instr->operands[1].isConstant(), "Index must be constant", instr.get());
               if (instr->opcode == aco_opcode::p_extract)
                  check(instr->operands[3].isConstant(), "Sign-extend flag must be constant",
                        instr.get());

               check(instr->definitions[0].regClass().type() != RegType::sgpr ||
                        instr->operands[0].regClass().type() == RegType::sgpr,
                     "Can't extract/insert VGPR to SGPR", instr.get());

               if (instr->opcode == aco_opcode::p_insert)
                  check(instr->operands[0].bytes() == instr->definitions[0].bytes(),
                        "Sizes of p_insert data operand and definition must match", instr.get());

               if (instr->definitions[0].regClass().type() == RegType::sgpr)
                  check(instr->definitions.size() >= 2 && instr->definitions[1].isFixed() &&
                           instr->definitions[1].physReg() == scc,
                        "SGPR extract/insert needs an SCC definition", instr.get());

               unsigned data_bits = instr->operands[0].bytes() * 8u;
               unsigned op_bits = instr->operands[2].constantValue();

               check(op_bits == 8 || op_bits == 16, "Size must be 8 or 16", instr.get());
               if (instr->opcode == aco_opcode::p_insert) {
                  check(op_bits < data_bits, "Size must be smaller than source", instr.get());
               } else if (instr->opcode == aco_opcode::p_extract) {
                  check(data_bits >= op_bits, "Can't extract more bits than what the data has.",
                        instr.get());
               }

               unsigned comp = data_bits / MAX2(op_bits, 1);
               check(instr->operands[1].constantValue() < comp, "Index must be in-bounds",
                     instr.get());

               check(program->gfx_level >= GFX9 ||
                        !instr->definitions[0].regClass().is_subdword() ||
                        instr->operands[0].regClass().type() == RegType::vgpr,
                     "Cannot extract/insert to subdword definition from SGPR before GFX9+",
                     instr.get());
            } else if (instr->opcode == aco_opcode::p_jump_to_epilog) {
               check(instr->definitions.size() == 0, "p_jump_to_epilog must have 0 definitions",
                     instr.get());
               check(instr->operands.size() > 0 && instr->operands[0].isOfType(RegType::sgpr) &&
                        instr->operands[0].size() == 2,
                     "First operand of p_jump_to_epilog must be a SGPR", instr.get());
               for (unsigned i = 1; i < instr->operands.size(); i++) {
                  check(instr->operands[i].isOfType(RegType::vgpr) ||
                           instr->operands[i].isOfType(RegType::sgpr) ||
                           instr->operands[i].isUndefined(),
                        "Other operands of p_jump_to_epilog must be VGPRs, SGPRs or undef",
                        instr.get());
               }
            } else if (instr->opcode == aco_opcode::p_dual_src_export_gfx11) {
               check(instr->definitions.size() == 6,
                     "p_dual_src_export_gfx11 must have 6 definitions", instr.get());
               check(instr->definitions[2].regClass() == program->lane_mask,
                     "Third definition of p_dual_src_export_gfx11 must be a lane mask",
                     instr.get());
               check(instr->definitions[3].regClass() == program->lane_mask,
                     "Fourth definition of p_dual_src_export_gfx11 must be a lane mask",
                     instr.get());
               check(instr->definitions[4].physReg() == vcc,
                     "Fifth definition of p_dual_src_export_gfx11 must be vcc", instr.get());
               check(instr->definitions[5].physReg() == scc,
                     "Sixth definition of p_dual_src_export_gfx11 must be scc", instr.get());
               check(instr->operands.size() == 8, "p_dual_src_export_gfx11 must have 8 operands",
                     instr.get());
               for (unsigned i = 0; i < instr->operands.size(); i++) {
                  check(
                     instr->operands[i].isOfType(RegType::vgpr) || instr->operands[i].isUndefined(),
                     "Operands of p_dual_src_export_gfx11 must be VGPRs or undef", instr.get());
               }
            }
            break;
         }
         case Format::PSEUDO_REDUCTION: {
            for (const Operand& op : instr->operands)
               check(op.regClass().type() == RegType::vgpr,
                     "All operands of PSEUDO_REDUCTION instructions must be in VGPRs.",
                     instr.get());

            if (instr->opcode == aco_opcode::p_reduce &&
                instr->reduction().cluster_size == program->wave_size)
               check(instr->definitions[0].regClass().type() == RegType::sgpr ||
                        program->wave_size == 32,
                     "The result of unclustered reductions must go into an SGPR.", instr.get());
            else
               check(instr->definitions[0].regClass().type() == RegType::vgpr,
                     "The result of scans and clustered reductions must go into a VGPR.",
                     instr.get());

            break;
         }
         case Format::SMEM: {
            if (instr->operands.size() >= 1)
               check(instr->operands[0].isOfType(RegType::sgpr), "SMEM operands must be sgpr",
                     instr.get());
            if (instr->operands.size() >= 2)
               check(instr->operands[1].isConstant() || instr->operands[1].isOfType(RegType::sgpr),
                     "SMEM offset must be constant or sgpr", instr.get());
            if (!instr->definitions.empty())
               check(instr->definitions[0].regClass().type() == RegType::sgpr,
                     "SMEM result must be sgpr", instr.get());
            break;
         }
         case Format::MTBUF:
         case Format::MUBUF: {
            check(instr->operands.size() > 1, "VMEM instructions must have at least one operand",
                  instr.get());
            check(instr->operands[1].isOfType(RegType::vgpr),
                  "VADDR must be in vgpr for VMEM instructions", instr.get());
            check(instr->operands[0].isOfType(RegType::sgpr), "VMEM resource constant must be sgpr",
                  instr.get());
            check(instr->operands.size() < 4 || instr->operands[3].isOfType(RegType::vgpr),
                  "VMEM write data must be vgpr", instr.get());
            if (instr->operands.size() >= 3 && instr->operands[2].isConstant())
               check(program->gfx_level < GFX12 || instr->operands[2].constantValue() == 0,
                     "VMEM SOFFSET must not be non-zero constant on GFX12+", instr.get());

            const bool d16 =
               instr->opcode ==
                  aco_opcode::buffer_load_dword || // FIXME: used to spill subdword variables
               instr->opcode == aco_opcode::buffer_load_ubyte ||
               instr->opcode == aco_opcode::buffer_load_sbyte ||
               instr->opcode == aco_opcode::buffer_load_ushort ||
               instr->opcode == aco_opcode::buffer_load_sshort ||
               instr->opcode == aco_opcode::buffer_load_ubyte_d16 ||
               instr->opcode == aco_opcode::buffer_load_ubyte_d16_hi ||
               instr->opcode == aco_opcode::buffer_load_sbyte_d16 ||
               instr->opcode == aco_opcode::buffer_load_sbyte_d16_hi ||
               instr->opcode == aco_opcode::buffer_load_short_d16 ||
               instr->opcode == aco_opcode::buffer_load_short_d16_hi ||
               instr->opcode == aco_opcode::buffer_load_format_d16_x ||
               instr->opcode == aco_opcode::buffer_load_format_d16_hi_x ||
               instr->opcode == aco_opcode::buffer_load_format_d16_xy ||
               instr->opcode == aco_opcode::buffer_load_format_d16_xyz ||
               instr->opcode == aco_opcode::buffer_load_format_d16_xyzw ||
               instr->opcode == aco_opcode::tbuffer_load_format_d16_x ||
               instr->opcode == aco_opcode::tbuffer_load_format_d16_xy ||
               instr->opcode == aco_opcode::tbuffer_load_format_d16_xyz ||
               instr->opcode == aco_opcode::tbuffer_load_format_d16_xyzw;
            if (instr->definitions.size()) {
               check(instr->definitions[0].regClass().type() == RegType::vgpr,
                     "VMEM definitions[0] (VDATA) must be VGPR", instr.get());
               check(d16 || !instr->definitions[0].regClass().is_subdword(),
                     "Only D16 opcodes can load subdword values.", instr.get());
               check(instr->definitions[0].bytes() <= 8 || !d16,
                     "D16 opcodes can only load up to 8 bytes.", instr.get());
            }
            break;
         }
         case Format::MIMG: {
            check(instr->operands.size() >= 4, "MIMG instructions must have at least 4 operands",
                  instr.get());
            check(instr->operands[0].hasRegClass() &&
                     (instr->operands[0].regClass() == s4 || instr->operands[0].regClass() == s8),
                  "MIMG operands[0] (resource constant) must be in 4 or 8 SGPRs", instr.get());
            if (instr->operands[1].hasRegClass())
               check(instr->operands[1].regClass() == s4,
                     "MIMG operands[1] (sampler constant) must be 4 SGPRs", instr.get());
            if (!instr->operands[2].isUndefined()) {
               bool is_cmpswap = instr->opcode == aco_opcode::image_atomic_cmpswap ||
                                 instr->opcode == aco_opcode::image_atomic_fcmpswap;
               check(instr->definitions.empty() ||
                        (instr->definitions[0].regClass() == instr->operands[2].regClass() ||
                         is_cmpswap),
                     "MIMG operands[2] (VDATA) must be the same as definitions[0] for atomics and "
                     "TFE/LWE loads",
                     instr.get());
            }

            if (instr->mimg().strict_wqm) {
               check(instr->operands[3].hasRegClass() &&
                        instr->operands[3].regClass().is_linear_vgpr(),
                     "MIMG operands[3] must be temp linear VGPR.", instr.get());

               unsigned total_size = 0;
               for (unsigned i = 4; i < instr->operands.size(); i++) {
                  check(instr->operands[i].hasRegClass() && instr->operands[i].regClass() == v1,
                        "MIMG operands[4+] (VADDR) must be v1", instr.get());
                  total_size += instr->operands[i].bytes();
               }
               check(total_size <= instr->operands[3].bytes(),
                     "MIMG operands[4+] must fit within operands[3].", instr.get());
            } else {
               check(instr->operands.size() == 4 || program->gfx_level >= GFX10,
                     "NSA is only supported on GFX10+", instr.get());
               for (unsigned i = 3; i < instr->operands.size(); i++) {
                  check(instr->operands[i].hasRegClass() &&
                           instr->operands[i].regClass().type() == RegType::vgpr,
                        "MIMG operands[3+] (VADDR) must be VGPR", instr.get());
                  if (instr->operands.size() > 4) {
                     if (program->gfx_level < GFX11) {
                        check(instr->operands[i].regClass() == v1,
                              "GFX10 MIMG VADDR must be v1 if NSA is used", instr.get());
                     } else {
                        unsigned num_scalar =
                           program->gfx_level >= GFX12 ? (instr->operands.size() - 4) : 4;
                        if (instr->opcode != aco_opcode::image_bvh_intersect_ray &&
                            instr->opcode != aco_opcode::image_bvh64_intersect_ray &&
                            instr->opcode != aco_opcode::image_bvh_dual_intersect_ray &&
                            instr->opcode != aco_opcode::image_bvh8_intersect_ray &&
                            i < 3 + num_scalar) {
                           check(instr->operands[i].regClass() == v1,
                                 "first 4 GFX11 MIMG VADDR must be v1 if NSA is used", instr.get());
                        }
                     }
                  }
               }
            }

            if (instr->definitions.size()) {
               check(instr->definitions[0].regClass().type() == RegType::vgpr,
                     "MIMG definitions[0] (VDATA) must be VGPR", instr.get());
               check(instr->mimg().d16 || !instr->definitions[0].regClass().is_subdword(),
                     "Only D16 MIMG instructions can load subdword values.", instr.get());
               check(instr->definitions[0].bytes() <= 8 || !instr->mimg().d16,
                     "D16 MIMG instructions can only load up to 8 bytes.", instr.get());
            }
            break;
         }
         case Format::DS: {
            for (const Operand& op : instr->operands) {
               check(op.isOfType(RegType::vgpr) || op.physReg() == m0 || op.isUndefined(),
                     "Only VGPRs are valid DS instruction operands", instr.get());
            }
            for (const Definition& def : instr->definitions) {
               check(def.regClass().type() == RegType::vgpr, "DS instruction must return VGPR",
                     instr.get());
            }
            break;
         }
         case Format::EXP: {
            for (unsigned i = 0; i < 4; i++)
               check(instr->operands[i].isOfType(RegType::vgpr),
                     "Only VGPRs are valid Export arguments", instr.get());
            break;
         }
         case Format::FLAT:
            check(instr->operands[1].isUndefined(), "Flat instructions don't support SADDR",
                  instr.get());
            FALLTHROUGH;
         case Format::GLOBAL:
            check(instr->operands[0].isOfType(RegType::vgpr), "FLAT/GLOBAL address must be vgpr",
                  instr.get());
            FALLTHROUGH;
         case Format::SCRATCH: {
            check(instr->operands[0].isOfType(RegType::vgpr),
                  "FLAT/GLOBAL/SCRATCH address must be undefined or vgpr", instr.get());
            check(instr->operands[1].isOfType(RegType::sgpr),
                  "FLAT/GLOBAL/SCRATCH sgpr address must be undefined or sgpr", instr.get());
            if (instr->format == Format::SCRATCH && program->gfx_level < GFX10_3)
               check(!instr->operands[0].isUndefined() || !instr->operands[1].isUndefined(),
                     "SCRATCH must have either SADDR or ADDR operand", instr.get());
            if (!instr->definitions.empty())
               check(instr->definitions[0].regClass().type() == RegType::vgpr,
                     "FLAT/GLOBAL/SCRATCH result must be vgpr", instr.get());
            else
               check(instr->operands[2].isOfType(RegType::vgpr),
                     "FLAT/GLOBAL/SCRATCH data must be vgpr", instr.get());
            break;
         }
         case Format::LDSDIR: {
            check(instr->definitions.size() == 1 && instr->definitions[0].regClass() == v1,
                  "LDSDIR must have an v1 definition", instr.get());
            check(instr->operands.size() == 1, "LDSDIR must have an operand", instr.get());
            if (!instr->operands.empty()) {
               check(instr->operands[0].regClass() == s1, "LDSDIR must have an s1 operand",
                     instr.get());
               check(instr->operands[0].isFixed() && instr->operands[0].physReg() == m0,
                     "LDSDIR must have an operand fixed to m0", instr.get());
            }
            break;
         }
         default: break;
         }
      }
   }

   auto check_edge = [&program, &is_valid](const char* msg, const Block::edge_vec& vec,
                                           Block* block, Block* other, bool other_is_pred) -> void
   {
      if (std::find(vec.begin(), vec.end(), block->index) == vec.end()) {
         Block* pred = other_is_pred ? other : block;
         Block* succ = other_is_pred ? block : other;
         aco_err(program, "%s: BB%u->BB%u", msg, pred->index, succ->index);
         is_valid = false;
      }
   };

   for (Block& block : program->blocks) {
      for (unsigned pred_idx : block.linear_preds) {
         Block* pred = &program->blocks[pred_idx];
         check_edge("Block is missing in linear_succs", pred->linear_succs, &block, pred, true);
      }

      for (unsigned pred_idx : block.logical_preds) {
         Block* pred = &program->blocks[pred_idx];
         check_edge("Block is missing in logical_succs", pred->logical_succs, &block, pred, true);
      }

      for (unsigned succ_idx : block.linear_succs) {
         Block* succ = &program->blocks[succ_idx];
         check_edge("Block is missing in linear_preds", succ->linear_preds, &block, succ, false);
      }

      for (unsigned succ_idx : block.logical_succs) {
         Block* succ = &program->blocks[succ_idx];
         check_edge("Block is missing in logical_preds", succ->logical_preds, &block, succ, false);
      }
   }

   return is_valid;
}

bool
validate_cfg(Program* program)
{
   if (!(debug_flags & DEBUG_VALIDATE_IR))
      return true;

   bool is_valid = true;
   auto check_block = [&program, &is_valid](bool success, const char* msg,
                                            aco::Block* block) -> void
   {
      if (!success) {
         aco_err(program, "%s: BB%u", msg, block->index);
         is_valid = false;
      }
   };

   /* validate CFG */
   for (unsigned i = 0; i < program->blocks.size(); i++) {
      Block& block = program->blocks[i];
      check_block(block.index == i, "block.index must match actual index", &block);

      /* predecessors/successors should be sorted */
      for (unsigned j = 0; j + 1 < block.linear_preds.size(); j++)
         check_block(block.linear_preds[j] < block.linear_preds[j + 1],
                     "linear predecessors must be sorted", &block);
      for (unsigned j = 0; j + 1 < block.logical_preds.size(); j++)
         check_block(block.logical_preds[j] < block.logical_preds[j + 1],
                     "logical predecessors must be sorted", &block);
      for (unsigned j = 0; j + 1 < block.linear_succs.size(); j++)
         check_block(block.linear_succs[j] < block.linear_succs[j + 1],
                     "linear successors must be sorted", &block);
      for (unsigned j = 0; j + 1 < block.logical_succs.size(); j++)
         check_block(block.logical_succs[j] < block.logical_succs[j + 1],
                     "logical successors must be sorted", &block);

      /* critical edges are not allowed */
      if (block.linear_preds.size() > 1) {
         for (unsigned pred : block.linear_preds)
            check_block(program->blocks[pred].linear_succs.size() == 1,
                        "linear critical edges are not allowed", &program->blocks[pred]);
         for (unsigned pred : block.logical_preds)
            check_block(program->blocks[pred].logical_succs.size() == 1,
                        "logical critical edges are not allowed", &program->blocks[pred]);
      }
   }

   return is_valid;
}

bool
validate_live_vars(Program* program)
{
   if (!(debug_flags & DEBUG_VALIDATE_LIVE_VARS))
      return true;

   bool is_valid = true;
   const int prev_num_waves = program->num_waves;
   const monotonic_buffer_resource old_memory = std::move(program->live.memory);
   const std::vector<IDSet> prev_live_in = std::move(program->live.live_in);
   const RegisterDemand prev_max_demand = program->max_reg_demand;
   std::vector<RegisterDemand> block_demands(program->blocks.size());
   std::vector<RegisterDemand> live_in_demands(program->blocks.size());
   std::vector<std::vector<RegisterDemand>> register_demands(program->blocks.size());

   for (unsigned i = 0; i < program->blocks.size(); i++) {
      Block& b = program->blocks[i];
      block_demands[i] = b.register_demand;
      live_in_demands[i] = b.live_in_demand;
      register_demands[i].reserve(b.instructions.size());
      for (unsigned j = 0; j < b.instructions.size(); j++)
         register_demands[i].emplace_back(b.instructions[j]->register_demand);
   }

   aco::live_var_analysis(program);

   /* Validate RegisterDemand calculation */
   for (unsigned i = 0; i < program->blocks.size(); i++) {
      Block& b = program->blocks[i];

      if (!(b.register_demand == block_demands[i])) {
         is_valid = false;
         aco_err(program,
                 "Register Demand not updated correctly for BB%d: got (%3u vgpr, %3u sgpr), but "
                 "should be (%3u vgpr, %3u sgpr)",
                 i, block_demands[i].vgpr, block_demands[i].sgpr, b.register_demand.vgpr,
                 b.register_demand.sgpr);
      }
      if (!(b.live_in_demand == live_in_demands[i])) {
         is_valid = false;
         aco_err(program,
                 "Live-in Demand not updated correctly for BB%d: got (%3u vgpr, %3u sgpr), but "
                 "should be (%3u vgpr, %3u sgpr)",
                 i, live_in_demands[i].vgpr, live_in_demands[i].sgpr, b.live_in_demand.vgpr,
                 b.live_in_demand.sgpr);
      }

      for (unsigned j = 0; j < b.instructions.size(); j++) {
         if (b.instructions[j]->register_demand == register_demands[i][j])
            continue;

         char* out;
         size_t outsize;
         struct u_memstream mem;
         u_memstream_open(&mem, &out, &outsize);
         FILE* const memf = u_memstream_get(&mem);

         fprintf(memf,
                 "Register Demand not updated correctly: got (%3u vgpr, %3u sgpr), but should be "
                 "(%3u vgpr, %3u sgpr): \n\t",
                 register_demands[i][j].vgpr, register_demands[i][j].sgpr,
                 b.instructions[j]->register_demand.vgpr, b.instructions[j]->register_demand.sgpr);
         aco_print_instr(program->gfx_level, b.instructions[j].get(), memf, print_kill);
         u_memstream_close(&mem);

         aco_err(program, "%s", out);
         free(out);

         is_valid = false;
      }
   }
   if (!(program->max_reg_demand == prev_max_demand) || program->num_waves != prev_num_waves) {
      is_valid = false;
      aco_err(program,
              "Max Register Demand and Num Waves not updated correctly: got (%3u vgpr, %3u sgpr) "
              "and %2u waves, but should be (%3u vgpr, %3u sgpr) and %2u waves",
              prev_max_demand.vgpr, prev_max_demand.sgpr, prev_num_waves,
              program->max_reg_demand.vgpr, program->max_reg_demand.sgpr, program->num_waves);
   }

   /* Validate Live-in sets */
   for (unsigned i = 0; i < program->blocks.size(); i++) {
      if (prev_live_in[i] != program->live.live_in[i]) {
         char* out;
         size_t outsize;
         struct u_memstream mem;
         u_memstream_open(&mem, &out, &outsize);
         FILE* const memf = u_memstream_get(&mem);

         fprintf(memf, "Live-in set not updated correctly for BB%d:", i);
         fprintf(memf, "\nMissing values: ");
         for (unsigned t : program->live.live_in[i]) {
            if (prev_live_in[i].count(t) == 0)
               fprintf(memf, "%%%d, ", t);
         }
         fprintf(memf, "\nAdditional values: ");
         for (unsigned t : prev_live_in[i]) {
            if (program->live.live_in[i].count(t) == 0)
               fprintf(memf, "%%%d, ", t);
         }
         u_memstream_close(&mem);
         aco_err(program, "%s", out);
         free(out);
         is_valid = false;
      }
   }

   return is_valid;
}

/* RA validation */
namespace {

struct Location {
   Location() : block(NULL), instr(NULL) {}

   Block* block;
   Instruction* instr; // NULL if it's the block's live-in
};

struct Assignment {
   Location defloc;
   Location firstloc;
   PhysReg reg;
   bool valid;
};

bool
ra_fail(Program* program, Location loc, Location loc2, const char* fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   char msg[1024];
   vsprintf(msg, fmt, args);
   va_end(args);

   char* out;
   size_t outsize;
   struct u_memstream mem;
   u_memstream_open(&mem, &out, &outsize);
   FILE* const memf = u_memstream_get(&mem);

   fprintf(memf, "RA error found at instruction in BB%d:\n", loc.block->index);
   if (loc.instr) {
      aco_print_instr(program->gfx_level, loc.instr, memf);
      fprintf(memf, "\n%s", msg);
   } else {
      fprintf(memf, "%s", msg);
   }
   if (loc2.block) {
      fprintf(memf, " in BB%d:\n", loc2.block->index);
      aco_print_instr(program->gfx_level, loc2.instr, memf);
   }
   fprintf(memf, "\n\n");
   u_memstream_close(&mem);

   aco_err(program, "%s", out);
   free(out);

   return true;
}

bool
validate_subdword_operand(amd_gfx_level gfx_level, const aco_ptr<Instruction>& instr,
                          unsigned index)
{
   Operand op = instr->operands[index];
   unsigned byte = op.physReg().byte();

   if (instr->opcode == aco_opcode::p_as_uniform)
      return byte == 0;
   if (instr->isPseudo() && gfx_level >= GFX8)
      return true;
   if (instr->isSDWA())
      return byte + instr->sdwa().sel[index].offset() + instr->sdwa().sel[index].size() <= 4 &&
             byte % instr->sdwa().sel[index].size() == 0;
   if (instr->isVOP3P()) {
      bool fma_mix = instr->opcode == aco_opcode::v_fma_mixlo_f16 ||
                     instr->opcode == aco_opcode::v_fma_mixhi_f16 ||
                     instr->opcode == aco_opcode::v_fma_mix_f32;
      return instr->valu().opsel_lo[index] == (byte >> 1) &&
             instr->valu().opsel_hi[index] == (fma_mix || (byte >> 1));
   }
   if (byte == 2 && can_use_opsel(gfx_level, instr->opcode, index))
      return true;

   switch (instr->opcode) {
   case aco_opcode::v_cvt_f32_ubyte1:
      if (byte == 1)
         return true;
      break;
   case aco_opcode::v_cvt_f32_ubyte2:
      if (byte == 2)
         return true;
      break;
   case aco_opcode::v_cvt_f32_ubyte3:
      if (byte == 3)
         return true;
      break;
   case aco_opcode::ds_write_b8_d16_hi:
   case aco_opcode::ds_write_b16_d16_hi:
      if (byte == 2 && index == 1)
         return true;
      break;
   case aco_opcode::buffer_store_byte_d16_hi:
   case aco_opcode::buffer_store_short_d16_hi:
   case aco_opcode::buffer_store_format_d16_hi_x:
      if (byte == 2 && index == 3)
         return true;
      break;
   case aco_opcode::flat_store_byte_d16_hi:
   case aco_opcode::flat_store_short_d16_hi:
   case aco_opcode::scratch_store_byte_d16_hi:
   case aco_opcode::scratch_store_short_d16_hi:
   case aco_opcode::global_store_byte_d16_hi:
   case aco_opcode::global_store_short_d16_hi:
      if (byte == 2 && index == 2)
         return true;
      break;
   default: break;
   }

   return byte == 0;
}

bool
validate_subdword_definition(amd_gfx_level gfx_level, const aco_ptr<Instruction>& instr)
{
   Definition def = instr->definitions[0];
   unsigned byte = def.physReg().byte();

   if (instr->isPseudo() && gfx_level >= GFX8)
      return true;
   if (instr->isSDWA())
      return byte + instr->sdwa().dst_sel.offset() + instr->sdwa().dst_sel.size() <= 4 &&
             byte % instr->sdwa().dst_sel.size() == 0;
   if (byte == 2 && can_use_opsel(gfx_level, instr->opcode, -1))
      return true;

   switch (instr->opcode) {
   case aco_opcode::v_interp_p2_hi_f16:
   case aco_opcode::v_fma_mixhi_f16:
   case aco_opcode::buffer_load_ubyte_d16_hi:
   case aco_opcode::buffer_load_sbyte_d16_hi:
   case aco_opcode::buffer_load_short_d16_hi:
   case aco_opcode::buffer_load_format_d16_hi_x:
   case aco_opcode::flat_load_ubyte_d16_hi:
   case aco_opcode::flat_load_short_d16_hi:
   case aco_opcode::scratch_load_ubyte_d16_hi:
   case aco_opcode::scratch_load_short_d16_hi:
   case aco_opcode::global_load_ubyte_d16_hi:
   case aco_opcode::global_load_short_d16_hi:
   case aco_opcode::ds_read_u8_d16_hi:
   case aco_opcode::ds_read_u16_d16_hi: return byte == 2;
   default: break;
   }

   return byte == 0;
}

unsigned
get_subdword_bytes_written(Program* program, const aco_ptr<Instruction>& instr, unsigned index)
{
   amd_gfx_level gfx_level = program->gfx_level;
   Definition def = instr->definitions[index];

   if (instr->isPseudo())
      return gfx_level >= GFX8 ? def.bytes() : def.size() * 4u;

   if (instr->isVALU() || instr->isVINTRP()) {
      if (instr->isSDWA())
         return instr->sdwa().dst_sel.size();

      if (instr_is_16bit(gfx_level, instr->opcode))
         return 2;

      return 4;
   }

   if (instr->isMIMG()) {
      assert(instr->mimg().d16);
      return program->dev.sram_ecc_enabled ? def.size() * 4u : def.bytes();
   }

   switch (instr->opcode) {
   case aco_opcode::buffer_load_ubyte_d16:
   case aco_opcode::buffer_load_sbyte_d16:
   case aco_opcode::buffer_load_short_d16:
   case aco_opcode::buffer_load_format_d16_x:
   case aco_opcode::tbuffer_load_format_d16_x:
   case aco_opcode::flat_load_ubyte_d16:
   case aco_opcode::flat_load_short_d16:
   case aco_opcode::scratch_load_ubyte_d16:
   case aco_opcode::scratch_load_short_d16:
   case aco_opcode::global_load_ubyte_d16:
   case aco_opcode::global_load_short_d16:
   case aco_opcode::ds_read_u8_d16:
   case aco_opcode::ds_read_u16_d16:
   case aco_opcode::buffer_load_ubyte_d16_hi:
   case aco_opcode::buffer_load_sbyte_d16_hi:
   case aco_opcode::buffer_load_short_d16_hi:
   case aco_opcode::buffer_load_format_d16_hi_x:
   case aco_opcode::flat_load_ubyte_d16_hi:
   case aco_opcode::flat_load_short_d16_hi:
   case aco_opcode::scratch_load_ubyte_d16_hi:
   case aco_opcode::scratch_load_short_d16_hi:
   case aco_opcode::global_load_ubyte_d16_hi:
   case aco_opcode::global_load_short_d16_hi:
   case aco_opcode::ds_read_u8_d16_hi:
   case aco_opcode::ds_read_u16_d16_hi: return program->dev.sram_ecc_enabled ? 4 : 2;
   case aco_opcode::buffer_load_format_d16_xyz:
   case aco_opcode::tbuffer_load_format_d16_xyz: return program->dev.sram_ecc_enabled ? 8 : 6;
   default: return def.size() * 4;
   }
}

bool
validate_instr_defs(Program* program, std::array<unsigned, 2048>& regs,
                    const std::vector<Assignment>& assignments, const Location& loc,
                    aco_ptr<Instruction>& instr)
{
   bool err = false;

   for (unsigned i = 0; i < instr->definitions.size(); i++) {
      Definition& def = instr->definitions[i];
      if (!def.isTemp())
         continue;
      Temp tmp = def.getTemp();
      PhysReg reg = assignments[tmp.id()].reg;
      for (unsigned j = 0; j < tmp.bytes(); j++) {
         if (regs[reg.reg_b + j])
            err |=
               ra_fail(program, loc, assignments[regs[reg.reg_b + j]].defloc,
                       "Assignment of element %d of %%%d already taken by %%%d from instruction", i,
                       tmp.id(), regs[reg.reg_b + j]);
         regs[reg.reg_b + j] = tmp.id();
      }
      if (def.regClass().is_subdword() && def.bytes() < 4) {
         unsigned written = get_subdword_bytes_written(program, instr, i);
         /* If written=4, the instruction still might write the upper half. In that case, it's
          * the lower half that isn't preserved */
         for (unsigned j = reg.byte() & ~(written - 1); j < written; j++) {
            unsigned written_reg = reg.reg() * 4u + j;
            if (regs[written_reg] && regs[written_reg] != def.tempId())
               err |= ra_fail(program, loc, assignments[regs[written_reg]].defloc,
                              "Assignment of element %d of %%%d overwrites the full register "
                              "taken by %%%d from instruction",
                              i, tmp.id(), regs[written_reg]);
         }
      }
   }

   for (const Definition& def : instr->definitions) {
      if (!def.isTemp())
         continue;
      if (def.isKill()) {
         for (unsigned j = 0; j < def.getTemp().bytes(); j++)
            regs[def.physReg().reg_b + j] = 0;
      }
   }

   return err;
}

} /* end namespace */

bool
validate_ra(Program* program)
{
   if (!(debug_flags & DEBUG_VALIDATE_RA))
      return false;

   bool err = false;
   aco::live_var_analysis(program);
   std::vector<std::vector<Temp>> phi_sgpr_ops(program->blocks.size());
   uint16_t sgpr_limit = get_addr_regs_from_waves(program, program->num_waves).sgpr;

   std::vector<Assignment> assignments(program->peekAllocationId());
   for (Block& block : program->blocks) {
      Location loc;
      loc.block = &block;
      for (aco_ptr<Instruction>& instr : block.instructions) {
         if (instr->opcode == aco_opcode::p_phi) {
            for (unsigned i = 0; i < instr->operands.size(); i++) {
               if (instr->operands[i].isTemp() &&
                   instr->operands[i].getTemp().type() == RegType::sgpr &&
                   instr->operands[i].isFirstKill())
                  phi_sgpr_ops[block.logical_preds[i]].emplace_back(instr->operands[i].getTemp());
            }
         }

         loc.instr = instr.get();
         for (unsigned i = 0; i < instr->operands.size(); i++) {
            Operand& op = instr->operands[i];
            if (!op.isTemp())
               continue;
            if (!op.isFixed())
               err |= ra_fail(program, loc, Location(), "Operand %d is not assigned a register", i);
            if (assignments[op.tempId()].valid && assignments[op.tempId()].reg != op.physReg())
               err |=
                  ra_fail(program, loc, assignments[op.tempId()].firstloc,
                          "Operand %d has an inconsistent register assignment with instruction", i);
            if ((op.getTemp().type() == RegType::vgpr &&
                 op.physReg().reg_b + op.bytes() > (256 + program->config->num_vgprs) * 4) ||
                (op.getTemp().type() == RegType::sgpr &&
                 op.physReg() + op.size() > program->config->num_sgprs &&
                 op.physReg() < sgpr_limit))
               err |= ra_fail(program, loc, assignments[op.tempId()].firstloc,
                              "Operand %d has an out-of-bounds register assignment", i);
            if (op.physReg() == vcc && !program->needs_vcc)
               err |= ra_fail(program, loc, Location(),
                              "Operand %d fixed to vcc but needs_vcc=false", i);
            if (op.regClass().is_subdword() &&
                !validate_subdword_operand(program->gfx_level, instr, i))
               err |= ra_fail(program, loc, Location(), "Operand %d not aligned correctly", i);
            if (op.isVectorAligned() &&
                op.physReg().advance(op.bytes()) != instr->operands[i + 1].physReg())
               err |= ra_fail(
                  program, loc, assignments[instr->operands[i + 1].tempId()].firstloc,
                  "Operand %d forms part of a vector but has misaligned register assignment.",
                  i + 1);
            if (!assignments[op.tempId()].firstloc.block)
               assignments[op.tempId()].firstloc = loc;
            if (!assignments[op.tempId()].defloc.block) {
               assignments[op.tempId()].reg = op.physReg();
               assignments[op.tempId()].valid = true;
            }
         }

         for (unsigned i = 0; i < instr->definitions.size(); i++) {
            Definition& def = instr->definitions[i];
            if (!def.isTemp())
               continue;
            if (!def.isFixed())
               err |=
                  ra_fail(program, loc, Location(), "Definition %d is not assigned a register", i);
            if (assignments[def.tempId()].defloc.block)
               err |= ra_fail(program, loc, assignments[def.tempId()].defloc,
                              "Temporary %%%d also defined by instruction", def.tempId());
            if ((def.getTemp().type() == RegType::vgpr &&
                 def.physReg().reg_b + def.bytes() > (256 + program->config->num_vgprs) * 4) ||
                (def.getTemp().type() == RegType::sgpr &&
                 def.physReg() + def.size() > program->config->num_sgprs &&
                 def.physReg() < sgpr_limit))
               err |= ra_fail(program, loc, assignments[def.tempId()].firstloc,
                              "Definition %d has an out-of-bounds register assignment", i);
            if (def.physReg() == vcc && !program->needs_vcc)
               err |= ra_fail(program, loc, Location(),
                              "Definition %d fixed to vcc but needs_vcc=false", i);
            if (def.regClass().is_subdword() &&
                !validate_subdword_definition(program->gfx_level, instr))
               err |= ra_fail(program, loc, Location(), "Definition %d not aligned correctly", i);
            if (!assignments[def.tempId()].firstloc.block)
               assignments[def.tempId()].firstloc = loc;
            assignments[def.tempId()].defloc = loc;
            assignments[def.tempId()].reg = def.physReg();
            assignments[def.tempId()].valid = true;
         }

         unsigned fixed_def_idx = 0;
         for (auto op_idx : get_tied_defs(instr.get())) {
            if (instr->definitions[fixed_def_idx++].physReg() !=
                instr->operands[op_idx].physReg()) {
               err |= ra_fail(program, loc, Location(),
                              "Operand %d must have the same register as definition", op_idx);
            }
         }
      }
   }

   for (Block& block : program->blocks) {
      Location loc;
      loc.block = &block;

      std::array<unsigned, 2048> regs; /* register file in bytes */
      regs.fill(0);

      /* check live in */
      for (unsigned id : program->live.live_in[block.index]) {
         Temp tmp(id, program->temp_rc[id]);
         PhysReg reg = assignments[id].reg;
         for (unsigned i = 0; i < tmp.bytes(); i++) {
            if (regs[reg.reg_b + i]) {
               err |= ra_fail(program, loc, Location(),
                              "Assignment of element %d of %%%d already taken by %%%d in live-in",
                              i, id, regs[reg.reg_b + i]);
            }
            regs[reg.reg_b + i] = id;
         }
      }

      for (aco_ptr<Instruction>& instr : block.instructions) {
         loc.instr = instr.get();

         /* remove killed p_phi operands from regs */
         if (instr->opcode == aco_opcode::p_logical_end) {
            for (Temp tmp : phi_sgpr_ops[block.index]) {
               PhysReg reg = assignments[tmp.id()].reg;
               for (unsigned i = 0; i < tmp.bytes(); i++)
                  regs[reg.reg_b + i] = 0;
            }
         }

         if (instr->opcode != aco_opcode::p_phi && instr->opcode != aco_opcode::p_linear_phi) {
            for (const Operand& op : instr->operands) {
               if (!op.isTemp())
                  continue;
               if (op.isFirstKillBeforeDef()) {
                  for (unsigned j = 0; j < op.getTemp().bytes(); j++)
                     regs[op.physReg().reg_b + j] = 0;
               }
            }
         }

         err |= validate_instr_defs(program, regs, assignments, loc, instr);

         if (!is_phi(instr)) {
            for (const Operand& op : instr->operands) {
               if (!op.isTemp())
                  continue;
               if (op.isLateKill() && op.isFirstKill()) {
                  for (unsigned j = 0; j < op.getTemp().bytes(); j++)
                     regs[op.physReg().reg_b + j] = 0;
               }
            }
         }
      }
   }

   return err;
}
} // namespace aco
