/*
 * Copyright © 2018 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_builder.h"
#include "aco_ir.h"

#include "util/half_float.h"
#include "util/memstream.h"

#include <algorithm>
#include <array>
#include <vector>

namespace aco {

namespace {
/**
 * The optimizer works in 4 phases:
 * (1) The first pass collects information for each ssa-def,
 *     propagates reg->reg operands of the same type, inline constants
 *     and neg/abs input modifiers.
 * (2) The second pass combines instructions like mad, omod, clamp and
 *     propagates sgpr's on VALU instructions.
 *     This pass depends on information collected in the first pass.
 * (3) The third pass goes backwards, and selects instructions,
 *     i.e. decides if a mad instruction is profitable and eliminates dead code.
 * (4) The fourth pass cleans up the sequence: literals get applied and dead
 *     instructions are removed from the sequence.
 */

struct mad_info {
   aco_ptr<Instruction> add_instr;
   uint32_t mul_temp_id;

   mad_info(aco_ptr<Instruction> instr, uint32_t id) : add_instr(std::move(instr)), mul_temp_id(id)
   {}
};

enum Label {
   label_constant_32bit = 1 << 1,
   /* label_{abs,neg,mul,omod2,omod4,omod5,clamp} are used for both 16 and
    * 32-bit operations but this shouldn't cause any issues because we don't
    * look through any conversions */
   label_abs = 1 << 2,
   label_neg = 1 << 3,
   label_temp = 1 << 5,
   label_literal = 1 << 6,
   label_mad = 1 << 7,
   label_omod2 = 1 << 8,
   label_omod4 = 1 << 9,
   label_omod5 = 1 << 10,
   label_clamp = 1 << 12,
   label_b2f = 1 << 16,
   /* This label means that it's either 0 or -1, and the ssa_info::temp is an s1 which is 0 or 1. */
   label_uniform_bool = 1 << 21,
   label_constant_64bit = 1 << 22,
   /* This label is added to the first definition of s_not/s_or/s_xor/s_and when all operands are
    * uniform_bool or uniform_bitwise. The first definition of ssa_info::instr would be 0 or -1 and
    * the second is SCC.
    */
   label_uniform_bitwise = 1 << 23,
   /* This label means that it's either 0 or 1 and ssa_info::temp is the inverse. */
   label_scc_invert = 1 << 24,
   label_scc_needed = 1 << 26,
   label_b2i = 1 << 27,
   label_fcanonicalize = 1 << 28,
   label_constant_16bit = 1 << 29,
   label_canonicalized = 1ull << 32, /* 1ull to prevent sign extension */
   label_extract = 1ull << 33,
   label_insert = 1ull << 34,
   label_f2f16 = 1ull << 38,
};

static constexpr uint64_t instr_mod_labels =
   label_omod2 | label_omod4 | label_omod5 | label_clamp | label_insert | label_f2f16;

static constexpr uint64_t temp_labels = label_abs | label_neg | label_temp | label_b2f |
                                        label_uniform_bool | label_scc_invert | label_b2i |
                                        label_fcanonicalize;
static constexpr uint32_t val_labels =
   label_constant_32bit | label_constant_64bit | label_constant_16bit | label_literal | label_mad;

static_assert((instr_mod_labels & temp_labels) == 0, "labels cannot intersect");
static_assert((instr_mod_labels & val_labels) == 0, "labels cannot intersect");
static_assert((temp_labels & val_labels) == 0, "labels cannot intersect");

struct ssa_info {
   uint64_t label;
   union {
      uint32_t val;
      Temp temp;
      Instruction* mod_instr;
   };
   Instruction* parent_instr;

   ssa_info() : label(0) {}

   void add_label(Label new_label)
   {
      if (new_label & instr_mod_labels) {
         label &= ~instr_mod_labels;
         label &= ~(temp_labels | val_labels); /* instr, temp and val alias */
      }

      if (new_label & temp_labels) {
         label &= ~temp_labels;
         label &= ~(instr_mod_labels | val_labels); /* instr, temp and val alias */
      }

      uint32_t const_labels =
         label_literal | label_constant_32bit | label_constant_64bit | label_constant_16bit;
      if (new_label & const_labels) {
         label &= ~val_labels | const_labels;
         label &= ~(instr_mod_labels | temp_labels); /* instr, temp and val alias */
      } else if (new_label & val_labels) {
         label &= ~val_labels;
         label &= ~(instr_mod_labels | temp_labels); /* instr, temp and val alias */
      }

      label |= new_label;
   }

   void set_constant(amd_gfx_level gfx_level, uint64_t constant)
   {
      Operand op16 = Operand::c16(constant);
      Operand op32 = Operand::get_const(gfx_level, constant, 4);
      add_label(label_literal);
      val = constant;

      /* check that no upper bits are lost in case of packed 16bit constants */
      if (gfx_level >= GFX8 && !op16.isLiteral() &&
          op16.constantValue16(true) == ((constant >> 16) & 0xffff))
         add_label(label_constant_16bit);

      if (!op32.isLiteral())
         add_label(label_constant_32bit);

      if (Operand::is_constant_representable(constant, 8))
         add_label(label_constant_64bit);

      if (label & label_constant_64bit) {
         val = Operand::c64(constant).constantValue();
         if (val != constant)
            label &= ~(label_literal | label_constant_16bit | label_constant_32bit);
      }
   }

   bool is_constant(unsigned bits)
   {
      switch (bits) {
      case 8: return label & label_literal;
      case 16: return label & label_constant_16bit;
      case 32: return label & label_constant_32bit;
      case 64: return label & label_constant_64bit;
      }
      return false;
   }

   bool is_literal(unsigned bits)
   {
      bool is_lit = label & label_literal;
      switch (bits) {
      case 8: return false;
      case 16: return is_lit && ~(label & label_constant_16bit);
      case 32: return is_lit && ~(label & label_constant_32bit);
      case 64: return false;
      }
      return false;
   }

   bool is_constant_or_literal(unsigned bits)
   {
      if (bits == 64)
         return label & label_constant_64bit;
      else
         return label & label_literal;
   }

   void set_abs(Temp abs_temp)
   {
      add_label(label_abs);
      temp = abs_temp;
   }

   bool is_abs() { return label & label_abs; }

   void set_neg(Temp neg_temp)
   {
      add_label(label_neg);
      temp = neg_temp;
   }

   bool is_neg() { return label & label_neg; }

   void set_neg_abs(Temp neg_abs_temp)
   {
      add_label((Label)((uint32_t)label_abs | (uint32_t)label_neg));
      temp = neg_abs_temp;
   }

   void set_temp(Temp tmp)
   {
      add_label(label_temp);
      temp = tmp;
   }

   bool is_temp() { return label & label_temp; }

   void set_mad(uint32_t mad_info_idx)
   {
      add_label(label_mad);
      val = mad_info_idx;
   }

   bool is_mad() { return label & label_mad; }

   void set_omod2(Instruction* mul)
   {
      if (label & temp_labels)
         return;
      add_label(label_omod2);
      mod_instr = mul;
   }

   bool is_omod2() { return label & label_omod2; }

   void set_omod4(Instruction* mul)
   {
      if (label & temp_labels)
         return;
      add_label(label_omod4);
      mod_instr = mul;
   }

   bool is_omod4() { return label & label_omod4; }

   void set_omod5(Instruction* mul)
   {
      if (label & temp_labels)
         return;
      add_label(label_omod5);
      mod_instr = mul;
   }

   bool is_omod5() { return label & label_omod5; }

   void set_clamp(Instruction* med3)
   {
      if (label & temp_labels)
         return;
      add_label(label_clamp);
      mod_instr = med3;
   }

   bool is_clamp() { return label & label_clamp; }

   void set_f2f16(Instruction* conv)
   {
      if (label & temp_labels)
         return;
      add_label(label_f2f16);
      mod_instr = conv;
   }

   bool is_f2f16() { return label & label_f2f16; }

   void set_b2f(Temp b2f_val)
   {
      add_label(label_b2f);
      temp = b2f_val;
   }

   bool is_b2f() { return label & label_b2f; }

   void set_uniform_bitwise() { add_label(label_uniform_bitwise); }

   bool is_uniform_bitwise() { return label & label_uniform_bitwise; }

   void set_scc_needed() { add_label(label_scc_needed); }

   bool is_scc_needed() { return label & label_scc_needed; }

   void set_scc_invert(Temp scc_inv)
   {
      add_label(label_scc_invert);
      temp = scc_inv;
   }

   bool is_scc_invert() { return label & label_scc_invert; }

   void set_uniform_bool(Temp uniform_bool)
   {
      add_label(label_uniform_bool);
      temp = uniform_bool;
   }

   bool is_uniform_bool() { return label & label_uniform_bool; }

   void set_b2i(Temp b2i_val)
   {
      add_label(label_b2i);
      temp = b2i_val;
   }

   bool is_b2i() { return label & label_b2i; }

   void set_fcanonicalize(Temp tmp)
   {
      add_label(label_fcanonicalize);
      temp = tmp;
   }

   bool is_fcanonicalize() { return label & label_fcanonicalize; }

   void set_canonicalized() { add_label(label_canonicalized); }

   bool is_canonicalized() { return label & label_canonicalized; }

   void set_extract() { add_label(label_extract); }

   bool is_extract() { return label & label_extract; }

   void set_insert(Instruction* insert)
   {
      if (label & temp_labels)
         return;
      add_label(label_insert);
      mod_instr = insert;
   }

   bool is_insert() { return label & label_insert; }
};

struct opt_ctx {
   Program* program;
   float_mode fp_mode;
   std::vector<aco_ptr<Instruction>> instructions;
   std::vector<ssa_info> info;
   std::pair<uint32_t, Temp> last_literal;
   std::vector<mad_info> mad_infos;
   std::vector<uint16_t> uses;
};

aco_type
get_canonical_operand_type(aco_opcode opcode, unsigned idx)
{
   aco_type type = instr_info.alu_opcode_infos[(int)opcode].op_types[idx];

   if (type.bit_size == 8 && type.num_components > 1) {
      /* Handling packed fp8/bf8 as non vector is easier. */
      type.bit_size *= type.num_components;
      type.num_components = 1;
      type.base_type = aco_base_type_none;
   }

   return type;
}

bool
dpp16_ctrl_uses_bc(uint16_t dpp_ctrl)
{
   if (dpp_ctrl >= dpp_row_sl(1) && dpp_ctrl <= dpp_row_sl(15))
      return true;
   if (dpp_ctrl >= dpp_row_sr(1) && dpp_ctrl <= dpp_row_sr(15))
      return true;
   if (dpp_ctrl == dpp_wf_sl1 || dpp_ctrl == dpp_wf_sr1)
      return true;
   if (dpp_ctrl == dpp_row_bcast15 || dpp_ctrl == dpp_row_bcast31)
      return true;
   return false;
}

struct alu_opt_op {
   Operand op;
   SubdwordSel extract[2] = {SubdwordSel::dword, SubdwordSel::dword};
   union {
      uint16_t _modifiers = 0;
      bitfield_array8<uint16_t, 0, 2> neg;
      bitfield_array8<uint16_t, 2, 2> abs;
      bitfield_bool<uint16_t, 4> f16_to_f32;
      bitfield_bool<uint16_t, 5> dot_sext;
      bitfield_bool<uint16_t, 6> dpp16;
      bitfield_bool<uint16_t, 7> dpp8;
      bitfield_bool<uint16_t, 8> bc;
      bitfield_bool<uint16_t, 9> fi;
   };
   uint32_t dpp_ctrl = 0;

   alu_opt_op& operator=(const alu_opt_op& other)
   {
      memmove((void*)this, &other, sizeof(*this));

      return *this;
   }

   alu_opt_op() = default;
   alu_opt_op(Operand _op) : op(_op) {};
   alu_opt_op(const alu_opt_op& other) { *this = other; }

   uint64_t constant_after_mods(opt_ctx& ctx, aco_type type) const
   {
      assert(this->op.isConstant());
      uint64_t res = 0;
      for (unsigned comp = 0; comp < type.num_components; comp++) {
         uint64_t part = this->op.constantValue64();
         /* 16bit negative int inline constants are sign extended, constantValue16 handles that. */
         if (this->op.bytes() == 2)
            part = this->op.constantValue16(false) | (this->op.constantValue16(true) << 16);

         if (type.bytes() <= 4) {
            SubdwordSel sel = this->extract[comp];
            part = part >> (sel.offset() * 8);
            if (sel.size() < 4) {
               part &= BITFIELD_MASK(sel.size() * 8);
               part = sel.sign_extend() ? util_sign_extend(part, sel.size() * 8) : part;
            }
         }

         if (this->f16_to_f32) {
            if (!(ctx.fp_mode.denorm16_64 & fp_denorm_keep_in)) {
               uint32_t absv = part & 0x7fff;
               if (absv <= 0x3ff)
                  part &= 0x8000;
            }
            part = fui(_mesa_half_to_float(part));
         }

         part &= BITFIELD64_MASK(type.bit_size - this->abs[comp]);
         part ^= this->neg[comp] ? BITFIELD64_BIT(type.bit_size - 1) : 0;
         res |= part << (type.bit_size * comp);
      }

      return res;
   }
};

struct alu_opt_info {
   aco::small_vec<Definition, 2> defs;
   aco::small_vec<alu_opt_op, 5> operands;
   aco_opcode opcode;
   Format format;
   uint32_t imm;
   uint32_t pass_flags; /* exec id */

   /* defs[0] modifiers */
   uint8_t omod;
   bool clamp;
   bool f32_to_f16;
   SubdwordSel insert;

   bool try_swap_operands(unsigned idx0, unsigned idx1)
   {
      aco_opcode new_opcode = get_swapped_opcode(opcode, idx0, idx1);
      if (new_opcode != aco_opcode::num_opcodes) {
         opcode = new_opcode;
         std::swap(operands[idx0], operands[idx1]);
         return true;
      }
      return false;
   }
};

bool
at_most_6lsb_used(aco_opcode op, unsigned idx)
{
   if (op == aco_opcode::v_writelane_b32 || op == aco_opcode::v_writelane_b32_e64 ||
       op == aco_opcode::v_readlane_b32 || op == aco_opcode::v_readlane_b32_e64)
      return idx == 1;
   return false;
}

unsigned
bytes_used(opt_ctx& ctx, alu_opt_info& info, unsigned idx)
{
   unsigned used = 4;
   aco_type type = get_canonical_operand_type(info.opcode, idx);
   if (type.bytes() == 0)
      return 4;
   used = MIN2(used, type.bytes());
   if (info.opcode == aco_opcode::v_lshlrev_b32 && idx == 1 && info.operands[0].op.isConstant()) {
      unsigned shift = info.operands[0].op.constantValue() & 0x1f;
      if (shift >= 16)
         used = MIN2(used, 2);
      if (shift >= 24)
         used = MIN2(used, 1);
   }
   return used;
}

bool
optimize_constants(opt_ctx& ctx, alu_opt_info& info)
{
   /* inline constants, pack literals */
   uint32_t literal = 0;
   unsigned litbits_used = 0;
   bool force_f2f32 = false;
   for (unsigned i = 0; i < info.operands.size(); i++) {
      auto& op_info = info.operands[i];
      assert(!op_info.op.isUndefined());
      if (!op_info.op.isConstant())
         continue;

      aco_type type = get_canonical_operand_type(info.opcode, i);

      if (type.num_components != 1 && type.num_components != 2)
         return false;
      if (!type.constant_bits())
         return false;

      if (type.bytes() > 4)
         continue;

      /* remove modifiers on constants: apply extract, f2f32, abs, neg */
      assert(op_info.op.size() == 1);
      uint32_t constant = op_info.constant_after_mods(ctx, type);
      op_info.op = Operand();
      for (unsigned comp = 0; comp < type.num_components; comp++) {
         op_info.extract[comp] = SubdwordSel(type.bit_size / 8, comp * type.bit_size / 8, false);
         op_info.f16_to_f32 = false;
         op_info.neg[comp] = false;
         op_info.abs[comp] = false;
      }

      if (at_most_6lsb_used(info.opcode, i))
         constant &= 0x3f;

      bool can_use_mods = can_use_input_modifiers(ctx.program->gfx_level, info.opcode, i);

      /* inline constants */
      if (type.num_components == 1) {
         Operand new_op =
            Operand::get_const(ctx.program->gfx_level, constant, type.constant_bits() / 8);
         Operand neg_op =
            Operand::get_const(ctx.program->gfx_level, BITFIELD_BIT(type.bit_size - 1) ^ constant,
                               type.constant_bits() / 8);
         Operand sext_op = Operand::get_const(ctx.program->gfx_level, 0xffff0000 | constant,
                                              type.constant_bits() / 8);
         if (!new_op.isLiteral()) {
            op_info.op = new_op;
         } else if (can_use_mods && !neg_op.isLiteral()) {
            op_info.op = neg_op;
            op_info.neg[0] = true;
         } else if (type.bit_size == 16 && !sext_op.isLiteral()) {
            op_info.op = sext_op;
         }
         // TODO opsel?
      } else if (info.format == Format::VOP3P) {
         assert(!can_use_mods || type.constant_bits() == 16);
         unsigned num_methods = (type.constant_bits() == 32 ? 5 : 1);
         for (unsigned hi = 0; op_info.op.isUndefined() && hi < 2; hi++) {
            for (unsigned negate = 0;
                 op_info.op.isUndefined() && (negate <= unsigned(can_use_mods)); negate++) {
               for (unsigned method = 0; op_info.op.isUndefined() && method < num_methods;
                    method++) {
                  uint32_t candidate = ((constant >> (hi * 16)) & 0xffff) ^ (negate ? 0x8000 : 0);
                  switch (method) {
                  case 0: break;                                /* try directly as constant */
                  case 1: candidate |= 0xffff0000; break;       /* sign extend */
                  case 2: candidate |= 0x3e220000; break;       /* 0.5pi */
                  case 3: candidate = (candidate << 16); break; /* high half */
                  case 4: candidate = (candidate << 16) | 0xf983; break; /* high half, 0.5pi. */
                  default: UNREACHABLE("impossible");
                  }
                  Operand new_op = Operand::get_const(ctx.program->gfx_level, candidate,
                                                      type.constant_bits() / 8);
                  if (new_op.isLiteral())
                     continue;

                  for (unsigned opsel = 0; op_info.op.isUndefined() && opsel < 2; opsel++) {
                     uint16_t other = constant >> (!hi * 16);
                     uint16_t abs_mask = 0xffffu >> unsigned(can_use_mods);
                     if ((new_op.constantValue16(opsel) & abs_mask) != (other & abs_mask))
                        continue;
                     op_info.op = new_op;
                     op_info.extract[hi] = method >= 3 ? SubdwordSel::uword1 : SubdwordSel::uword0;
                     op_info.extract[!hi] = opsel ? SubdwordSel::uword1 : SubdwordSel::uword0;
                     op_info.neg[hi] = negate;
                     op_info.neg[!hi] = new_op.constantValue16(opsel) ^ other;
                  }
               }
            }
         }
      }

      /* we found an inline constant */
      if (!op_info.op.isUndefined())
         continue;

      bool use_swizzle = type.num_components == 2 && info.format == Format::VOP3P;
      bool try_neg = can_use_mods && (type.num_components == 1 || use_swizzle);
      unsigned comp_bits = use_swizzle ? type.bit_size : type.bytes() * 8;
      assert(comp_bits == 32 || comp_bits == 16);
      uint32_t abs_mask = BITFIELD_MASK(comp_bits - try_neg);
      for (unsigned comp = 0; comp <= unsigned(use_swizzle); comp++) {
         uint32_t part = constant >> (comp * comp_bits) & BITFIELD_MASK(comp_bits);

         /* Try to re-use another literal, or part of it. */
         bool found_part = false;
         for (unsigned litcomp = 0; litcomp < (litbits_used / comp_bits); litcomp++) {
            uint32_t litpart = literal >> (litcomp * comp_bits) & BITFIELD_MASK(comp_bits);
            if ((litpart & abs_mask) == (part & abs_mask)) {
               op_info.neg[comp] = litpart ^ part;
               op_info.extract[comp] = SubdwordSel(comp_bits / 8, litcomp * (comp_bits / 8), false);
               found_part = true;
            }
         }

         if (found_part)
            continue;

         /* If there isn't enough space for more literal data, try to use fp16 or return false. */
         litbits_used = align(litbits_used, comp_bits);
         if (litbits_used + comp_bits > 32) {
            if (comp_bits == 32 && !force_f2f32) {
               float f32s[] = {uif(literal), uif(constant)};
               literal = 0;
               for (unsigned fltidx = 0; fltidx < 2; fltidx++) {
                  uint32_t fp16_val = _mesa_float_to_half(f32s[fltidx]);
                  bool is_denorm = (fp16_val & 0x7fff) != 0 && (fp16_val & 0x7fff) <= 0x3ff;
                  if (_mesa_half_to_float(fp16_val) != f32s[fltidx] ||
                      (is_denorm && !(ctx.fp_mode.denorm16_64 & fp_denorm_keep_in)))
                     return false;
                  literal |= fp16_val << (fltidx * 16);
               }
               force_f2f32 = true;
               op_info.extract[0] = SubdwordSel::uword1;
               break;
            }
            return false;
         }

         literal |= part << litbits_used;
         op_info.extract[comp] = SubdwordSel(comp_bits / 8, litbits_used / 8, false);
         litbits_used += comp_bits;
      }
   }

   for (auto& op_info : info.operands) {
      if (!op_info.op.isUndefined())
         continue;
      op_info.op = Operand::literal32(literal);
      op_info.f16_to_f32 = force_f2f32;
   }

   return true;
}

Format
format_combine(Format f1, Format f2)
{
   return (Format)((uint32_t)f1 | (uint32_t)f2);
}

bool
format_is(Format f1, Format f2)
{
   return ((Format)((uint32_t)f1 & (uint32_t)f2)) == f2;
}

/* Determine if this alu_opt_info can be represented by a valid ACO IR instruction.
 * info is modified to not duplicate work when it's converted to an ACO IR instruction.
 * If false is returned, info must no longer be used.
 */
bool
alu_opt_info_is_valid(opt_ctx& ctx, alu_opt_info& info)
{
   info.format = instr_info.format[(int)info.opcode];

   /* remove dpp if possible, abort in some unsupported cases (bc with sgpr, constant.) */
   for (auto& op_info : info.operands) {
      if (!op_info.dpp16 && !op_info.dpp8)
         continue;
      if (op_info.op.isOfType(RegType::vgpr))
         continue;
      /* bc=0: undefined if inactive read (lane disabled, but that's not expressed in SSA)
       * if fi=1, bc only matters for a few dpp16 options
       */
      if (op_info.bc && (!op_info.fi || (op_info.dpp16 && dpp16_ctrl_uses_bc(op_info.dpp_ctrl))))
         return false;
      op_info.dpp16 = false;
      op_info.dpp8 = false;
   }

   /* if mul, push neg to constant, eliminate double negate */
   switch (info.opcode) {
   case aco_opcode::v_mul_f64_e64:
   case aco_opcode::v_mul_f64:
   case aco_opcode::v_mul_f32:
   case aco_opcode::v_mul_legacy_f32:
   case aco_opcode::v_mul_f16:
   case aco_opcode::v_mad_f32:
   case aco_opcode::v_mad_legacy_f32:
   case aco_opcode::v_mad_f16:
   case aco_opcode::v_mad_legacy_f16:
   case aco_opcode::v_fma_f64:
   case aco_opcode::v_fma_f32:
   case aco_opcode::v_fma_legacy_f32:
   case aco_opcode::v_fma_f16:
   case aco_opcode::v_fma_legacy_f16:
   case aco_opcode::v_fma_mix_f32:
   case aco_opcode::v_fma_mixlo_f16:
   case aco_opcode::v_pk_mul_f16:
   case aco_opcode::v_pk_fma_f16:
   case aco_opcode::s_mul_f32:
   case aco_opcode::s_mul_f16:
   case aco_opcode::s_fmac_f32:
   case aco_opcode::s_fmac_f16:
      for (unsigned comp = 0; comp < 2; comp++) {
         for (unsigned i = 0; i < 2; i++) {
            if (info.operands[!i].op.isConstant() || info.operands[!i].neg[comp]) {
               info.operands[!i].neg[comp] ^= info.operands[i].neg[comp];
               info.operands[i].neg[comp] = false;
            }
         }
      }
      break;
   default: break;
   }

   if (!optimize_constants(ctx, info))
      return false;

   /* check constant bus limit */
   bool is_salu = false;
   switch (info.format) {
   case Format::SOPC:
   case Format::SOPK:
   case Format::SOP1:
   case Format::SOP2:
   case Format::SOPP: is_salu = true; break;
   default: break;
   }
   int constant_limit = is_salu ? INT_MAX : (ctx.program->gfx_level >= GFX10 ? 2 : 1);

   switch (info.opcode) {
   case aco_opcode::v_writelane_b32:
   case aco_opcode::v_writelane_b32_e64: constant_limit = INT_MAX; break;
   case aco_opcode::v_lshlrev_b64:
   case aco_opcode::v_lshlrev_b64_e64:
   case aco_opcode::v_lshrrev_b64:
   case aco_opcode::v_ashrrev_i64: constant_limit = 1; break;
   default: break;
   }

   for (unsigned i = 0; i < info.operands.size(); i++) {
      const Operand& op = info.operands[i].op;
      if (!op.isLiteral() && !op.isOfType(RegType::sgpr))
         continue;

      constant_limit--;
      for (unsigned j = 0; j < i; j++) {
         const Operand& other = info.operands[j].op;
         if (op == other) {
            constant_limit++;
            break;
         } else if (op.isLiteral() && other.isLiteral()) {
            return false;
         }
      }
   }

   if (constant_limit < 0)
      return false;

   /* apply extract. */
   if (info.opcode == aco_opcode::s_pack_ll_b32_b16) {
      if (info.operands[0].extract[0].size() < 2 || info.operands[1].extract[0].size() < 2)
         return false;
      if (info.operands[0].extract[0].offset() == 2 && info.operands[1].extract[0].offset() == 2) {
         info.opcode = aco_opcode::s_pack_hh_b32_b16;
      } else if (info.operands[0].extract[0].offset() == 0 &&
                 info.operands[1].extract[0].offset() == 2) {
         info.opcode = aco_opcode::s_pack_lh_b32_b16;
      } else if (info.operands[0].extract[0].offset() == 2 &&
                 info.operands[1].extract[0].offset() == 0) {
         if (ctx.program->gfx_level < GFX11) /* TODO try shifting constant */
            return false;
         info.opcode = aco_opcode::s_pack_hl_b32_b16;
      }
      info.operands[0].extract[0] = SubdwordSel::dword;
      info.operands[1].extract[0] = SubdwordSel::dword;
   }

   for (unsigned i = 0; i < info.operands.size(); i++) {
      aco_type type = get_canonical_operand_type(info.opcode, i);
      if (type.bit_size == 16 && type.num_components == 2) {
         for (unsigned comp = 0; comp < 2; comp++) {
            SubdwordSel sel = info.operands[i].extract[comp];
            if (sel.size() < 2)
               return false;
            if (info.format != Format::VOP3P && sel.offset() != 2 * comp)
               return false;
         }
         continue;
      }
      SubdwordSel sel = info.operands[i].extract[0];
      if (sel.size() == 4) {
         continue;
      } else if (info.operands[i].f16_to_f32 && sel.size() < 2) {
         return false;
      } else if (info.operands[i].f16_to_f32 && sel.size() == 2) {
         continue;
      } else if (sel.offset() == 0 && sel.size() >= bytes_used(ctx, info, i)) {
         info.operands[i].extract[0] = SubdwordSel::dword;
      } else if ((info.opcode == aco_opcode::v_cvt_f32_u32 ||
                  info.opcode == aco_opcode::v_cvt_f32_i32) &&
                 sel.size() == 1 && !sel.sign_extend()) {
         switch (sel.offset()) {
         case 0: info.opcode = aco_opcode::v_cvt_f32_ubyte0; break;
         case 1: info.opcode = aco_opcode::v_cvt_f32_ubyte1; break;
         case 2: info.opcode = aco_opcode::v_cvt_f32_ubyte2; break;
         case 3: info.opcode = aco_opcode::v_cvt_f32_ubyte3; break;
         default: UNREACHABLE("invalid SubdwordSel");
         }
         info.operands[i].extract[0] = SubdwordSel::dword;
         continue;
      } else if (info.opcode == aco_opcode::v_mul_u32_u24 && ctx.program->gfx_level >= GFX10 &&
                 sel.size() == 2 && !sel.sign_extend() &&
                 !info.operands[!i].extract[0].sign_extend() &&
                 info.operands[!i].extract[0].size() >= 2 &&
                 (info.operands[!i].op.is16bit() || info.operands[!i].extract[0].size() == 2 ||
                  (info.operands[!i].op.isConstant() &&
                   info.operands[!i].op.constantValue() <= UINT16_MAX))) {
         info.opcode = aco_opcode::v_mad_u32_u16;
         info.format = Format::VOP3;
         info.operands.push_back(alu_opt_op{});
         info.operands[2].op = Operand::c32(0);
         continue;
      } else if (i < 2 && ctx.program->gfx_level >= GFX8 && ctx.program->gfx_level < GFX11 &&
                 (format_is(info.format, Format::VOPC) || format_is(info.format, Format::VOP2) ||
                  format_is(info.format, Format::VOP1))) {
         info.format = format_combine(info.format, Format::SDWA);
         continue;
      } else if (sel.size() == 2 && can_use_opsel(ctx.program->gfx_level, info.opcode, i)) {
         continue;
      } else if (info.opcode == aco_opcode::s_cvt_f32_f16 && sel.size() == 2 && sel.offset() == 2) {
         info.opcode = aco_opcode::s_cvt_hi_f32_f16;
         info.operands[i].extract[0] = SubdwordSel::dword;
         continue;
      } else {
         return false;
      }
   }

   /* convert to v_fma_mix */
   bool uses_f2f32 = false;
   for (auto& op_info : info.operands)
      uses_f2f32 |= op_info.f16_to_f32;

   if (uses_f2f32 || info.f32_to_f16) {
      if (ctx.program->gfx_level < GFX9)
         return false;

      /* v_mad_mix* on GFX9 always flushes denormals for 16-bit inputs/outputs */
      if (ctx.program->gfx_level == GFX9 && ctx.fp_mode.denorm16_64)
         return false;

      switch (info.opcode) {
      case aco_opcode::v_add_f32:
         info.operands.insert(info.operands.begin(), alu_opt_op{});
         info.operands[0].op = Operand::c32(0x3f800000);
         break;
      case aco_opcode::v_mul_f32:
         info.operands.push_back(alu_opt_op{});
         info.operands[2].op = Operand::c32(0);
         info.operands[2].neg[0] = true;
         break;
      case aco_opcode::v_fma_f32:
         // TODO remove precise, not clear why unfusing fma would be valid
         if (!ctx.program->dev.fused_mad_mix && info.defs[0].isPrecise())
            return false;
         break;
      case aco_opcode::v_mad_f32:
         if (ctx.program->dev.fused_mad_mix && info.defs[0].isPrecise())
            return false;
         break;
      default: return false;
      }

      info.opcode = info.f32_to_f16 ? aco_opcode::v_fma_mixlo_f16 : aco_opcode::v_fma_mix_f32;
      info.format = Format::VOP3P;
   }

   /* remove negate modifiers by converting to subtract */
   aco_opcode sub = aco_opcode::num_opcodes;
   aco_opcode subrev = aco_opcode::num_opcodes;
   switch (info.opcode) {
   case aco_opcode::v_add_f32:
      sub = aco_opcode::v_sub_f32;
      subrev = aco_opcode::v_subrev_f32;
      break;
   case aco_opcode::v_add_f16:
      sub = aco_opcode::v_sub_f16;
      subrev = aco_opcode::v_subrev_f16;
      break;
   case aco_opcode::s_add_f32: sub = aco_opcode::s_sub_f32; break;
   case aco_opcode::s_add_f16: sub = aco_opcode::s_sub_f16; break;
   default: break;
   }

   if (sub != aco_opcode::num_opcodes && (info.operands[0].neg[0] ^ info.operands[1].neg[0])) {
      if (info.operands[1].neg[0]) {
         info.opcode = sub;
      } else if (subrev != aco_opcode::num_opcodes) {
         info.opcode = subrev;
      } else {
         info.opcode = sub;
         std::swap(info.operands[0], info.operands[1]);
      }

      info.operands[0].neg[0] = false;
      info.operands[1].neg[0] = false;
   }

   /* convert to DPP */
   bool is_dpp = false;
   for (unsigned i = 0; i < info.operands.size(); i++) {
      if (info.operands[i].dpp16 || info.operands[i].dpp8) {
         if (is_dpp || !info.try_swap_operands(0, i))
            return false;

         is_dpp = true;
         if (info.operands[0].dpp16)
            info.format = format_combine(info.format, Format::DPP16);
         else if (info.operands[0].dpp8)
            info.format = format_combine(info.format, Format::DPP8);
      }
   }
   if (is_dpp && info.operands.size() > 2 && !info.operands[1].op.isOfType(RegType::vgpr) &&
       info.operands[2].op.isOfType(RegType::vgpr))
      info.try_swap_operands(1, 2);
   if (is_dpp && info.operands.size() > 1 && !info.operands[1].op.isOfType(RegType::vgpr))
      return false; /* TODO: gfx11.5 */

   /* dst SDWA */
   if (info.insert != SubdwordSel::dword) {
      if (info.insert.offset() == 0 && info.insert.size() >= info.defs[0].bytes()) {
         info.insert = SubdwordSel::dword;
      } else if (info.defs[0].bytes() != 4 ||
                 (!format_is(info.format, Format::VOP1) && !format_is(info.format, Format::VOP2))) {
         return false;
      } else {
         info.format = format_combine(info.format, Format::SDWA);
      }
   }

   /* DPP and SDWA can't be used at the same time. */
   if (is_dpp && format_is(info.format, Format::SDWA))
      return false;

   bool is_dpp_or_sdwa = is_dpp || format_is(info.format, Format::SDWA);

   bitarray8 neg = 0;
   bitarray8 abs = 0;
   bitarray8 opsel = 0;
   bitarray8 vmask = 0;
   bitarray8 smask = 0;
   bitarray8 cmask = 0;
   bitarray8 lmask = 0;

   for (unsigned i = 0; i < info.operands.size(); i++) {
      aco_type type = get_canonical_operand_type(info.opcode, i);
      bool can_use_mods = can_use_input_modifiers(ctx.program->gfx_level, info.opcode, i);
      const auto& op_info = info.operands[i];

      if (!format_is(info.format, Format::VOP3P) && type.num_components == 2 &&
          (op_info.neg[0] != op_info.neg[1] || op_info.abs[0] != op_info.abs[1]))
         return false;

      for (unsigned comp = 0; comp < type.num_components; comp++) {
         if (!can_use_mods && (op_info.neg[comp] || op_info.abs[comp]))
            return false;
         abs[i] |= op_info.abs[comp];
         neg[i] |= op_info.neg[comp];
      }
      opsel[i] = op_info.extract[0].offset();
      vmask[i] = op_info.op.isOfType(RegType::vgpr);
      smask[i] = op_info.op.isOfType(RegType::sgpr);
      cmask[i] = op_info.op.isConstant();
      lmask[i] = op_info.op.isLiteral();

      /* lane masks must be sgpr */
      if (type.bit_size == 1 && !smask[i])
         return false;

      /* DPP/SDWA doesn't allow 64bit opcodes. */
      if (is_dpp_or_sdwa && info.operands[i].op.size() != 1 && type.bit_size != 1)
         return false;
   }

   /* DPP/SDWA doesn't allow 64bit opcodes. */
   if (is_dpp_or_sdwa && !format_is(info.format, Format::VOPC) && info.defs[0].size() != 1)
      return false;

   if (format_is(info.format, Format::VOP1) || format_is(info.format, Format::VOP2) ||
       format_is(info.format, Format::VOPC) || format_is(info.format, Format::VOP3)) {
      bool needs_vop3 = false;
      if (info.omod && format_is(info.format, Format::SDWA) && ctx.program->gfx_level < GFX9)
         return false;

      if (info.omod && !format_is(info.format, Format::SDWA))
         needs_vop3 = true;

      if (info.clamp && format_is(info.format, Format::SDWA) &&
          format_is(info.format, Format::VOPC) && ctx.program->gfx_level >= GFX9)
         return false;

      if ((info.clamp || (opsel & ~vmask)) && !format_is(info.format, Format::SDWA))
         needs_vop3 = true;

      if (!format_is(info.format, Format::SDWA) && !format_is(info.format, Format::DPP16) &&
          (abs || neg))
         needs_vop3 = true;

      if (((cmask | smask) & 0x3) && format_is(info.format, Format::SDWA) &&
          ctx.program->gfx_level == GFX8)
         return false;

      aco_opcode mulk = aco_opcode::num_opcodes;
      aco_opcode addk = aco_opcode::num_opcodes;
      switch (info.opcode) {
      case aco_opcode::v_s_exp_f16:
      case aco_opcode::v_s_log_f16:
      case aco_opcode::v_s_rcp_f16:
      case aco_opcode::v_s_rsq_f16:
      case aco_opcode::v_s_sqrt_f16:
         /* These can't use inline constants on GFX12 but can use literals. We don't bother since
          * they should be constant folded anyway. */
         if (cmask)
            return false;
         FALLTHROUGH;
      case aco_opcode::v_s_exp_f32:
      case aco_opcode::v_s_log_f32:
      case aco_opcode::v_s_rcp_f32:
      case aco_opcode::v_s_rsq_f32:
      case aco_opcode::v_s_sqrt_f32:
         if (vmask)
            return false;
         break;
      case aco_opcode::v_writelane_b32:
      case aco_opcode::v_writelane_b32_e64:
         if ((vmask & 0x3) || (~vmask & 0x4))
            return false;
         if (is_dpp || format_is(info.format, Format::SDWA))
            return false;
         break;
      case aco_opcode::v_permlane16_b32:
      case aco_opcode::v_permlanex16_b32:
      case aco_opcode::v_permlane64_b32:
      case aco_opcode::v_readfirstlane_b32:
      case aco_opcode::v_readlane_b32:
      case aco_opcode::v_readlane_b32_e64:
         if ((~vmask & 0x1) || (vmask & 0x6))
            return false;
         if (is_dpp || format_is(info.format, Format::SDWA))
            return false;
         break;
      case aco_opcode::v_mul_lo_u32:
      case aco_opcode::v_mul_lo_i32:
      case aco_opcode::v_mul_hi_u32:
      case aco_opcode::v_mul_hi_i32:
         if (is_dpp)
            return false;
         break;
      case aco_opcode::v_fma_f32:
         if (ctx.program->gfx_level >= GFX10) {
            mulk = aco_opcode::v_fmamk_f32;
            addk = aco_opcode::v_fmaak_f32;
         }
         break;
      case aco_opcode::v_fma_f16:
      case aco_opcode::v_fma_legacy_f16:
         if (ctx.program->gfx_level >= GFX10) {
            mulk = aco_opcode::v_fmamk_f16;
            addk = aco_opcode::v_fmaak_f16;
         }
         break;
      case aco_opcode::v_mad_f32:
         mulk = aco_opcode::v_madmk_f32;
         addk = aco_opcode::v_madak_f32;
         break;
      case aco_opcode::v_mad_f16:
      case aco_opcode::v_mad_legacy_f16:
         mulk = aco_opcode::v_madmk_f16;
         addk = aco_opcode::v_madak_f16;
         break;
      default:
         if ((smask[1] || cmask[1]) && !needs_vop3 && !format_is(info.format, Format::VOP3) &&
             !format_is(info.format, Format::SDWA)) {
            if (is_dpp || !vmask[0] || !info.try_swap_operands(0, 1))
               needs_vop3 = true;
         }
         if (needs_vop3)
            info.format = format_combine(info.format, Format::VOP3);
      }

      if (addk != aco_opcode::num_opcodes && vmask && lmask && !needs_vop3 &&
          (vmask[2] || lmask[2]) && (!opsel || ctx.program->gfx_level >= GFX11)) {
         for (int i = 2; i >= 0; i--) {
            if (lmask[i]) {
               if (i == 0 || (i == 2 && !vmask[1]))
                  std::swap(info.operands[0], info.operands[1]);
               if (i != 2)
                  std::swap(info.operands[1], info.operands[2]);
               info.opcode = i == 2 ? addk : mulk;
               info.format = Format::VOP2;
               break;
            }
         }
      }

      bool nolit = format_is(info.format, Format::SDWA) || is_dpp ||
                   (format_is(info.format, Format::VOP3) && ctx.program->gfx_level < GFX10);
      if (nolit && lmask)
         return false;
      if (is_dpp && format_is(info.format, Format::VOP3) && ctx.program->gfx_level < GFX11)
         return false;

      /* Fix lane mask src/dst to vcc if the format requires it. */
      if (ctx.program->gfx_level < GFX11 && (is_dpp || format_is(info.format, Format::SDWA))) {
         if (format_is(info.format, Format::VOP2)) {
            if (info.operands.size() > 2)
               info.operands[2].op.setPrecolored(vcc);
            if (info.defs.size() > 1)
               info.defs[1].setPrecolored(vcc);
         }
         if (format_is(info.format, Format::VOPC) && (is_dpp || ctx.program->gfx_level < GFX9) &&
             !info.defs[0].isFixed())
            info.defs[0].setPrecolored(vcc);
      }
   } else if (format_is(info.format, Format::VOP3P)) {
      bool fmamix =
         info.opcode == aco_opcode::v_fma_mix_f32 || info.opcode == aco_opcode::v_fma_mixlo_f16;
      bool dot2_f32 =
         info.opcode == aco_opcode::v_dot2_f32_f16 || info.opcode == aco_opcode::v_dot2_f32_bf16;
      bool supports_dpp = (fmamix || dot2_f32) && ctx.program->gfx_level >= GFX11;
      if ((abs && !fmamix) || (is_dpp && !supports_dpp) || info.omod)
         return false;
      if (lmask && (ctx.program->gfx_level < GFX10 || is_dpp))
         return false;
   } else if (is_salu) {
      if (vmask)
         return false;
      if (info.opcode == aco_opcode::s_fmac_f32) {
         for (unsigned i = 0; i < 2; i++) {
            if (lmask[i]) {
               std::swap(info.operands[i], info.operands[1]);
               std::swap(info.operands[1], info.operands[2]);
               info.opcode = aco_opcode::s_fmamk_f32;
               break;
            }
         }
         if (info.opcode == aco_opcode::s_fmac_f32 && cmask[2]) {
            info.operands[2].op = Operand::literal32(info.operands[2].op.constantValue());
            lmask[2] = true;
            info.opcode = aco_opcode::s_fmaak_f32;
         }
      } else if (info.opcode == aco_opcode::s_fmac_f16 && !smask[2]) {
         return false;
      }
   }

   return true;
}

/* Gather semantic information about an alu instruction and its operands from an ACO IR Instruction.
 *
 * Some callers expect that the alu_opt_info created by alu_opt_gather_info() or the instruction
 * created by alu_opt_info_to_instr() does not have more uses of a temporary than the original
 * instruction did.
 */
bool
alu_opt_gather_info(opt_ctx& ctx, Instruction* instr, alu_opt_info& info)
{
   if (!instr->isVALU() && !instr->isSALU())
      return false;

   /* There is nothing to be gained from handling WMMA/mqsad here. */
   if (instr_info.classes[(int)instr->opcode] == instr_class::wmma ||
       instr->opcode == aco_opcode::v_mqsad_u32_u8)
      return false;

   /* TODO handle when this is used for output modifiers. */
   if (instr->isVINTERP_INREG())
      return false;

   switch (instr->opcode) {
   case aco_opcode::s_addk_i32:
   case aco_opcode::s_cmovk_i32:
   case aco_opcode::s_mulk_i32:
   case aco_opcode::v_dot2c_f32_f16:
   case aco_opcode::v_dot4c_i32_i8:
   case aco_opcode::v_fmac_f32:
   case aco_opcode::v_fmac_f16:
   case aco_opcode::v_fmac_legacy_f32:
   case aco_opcode::v_mac_f32:
   case aco_opcode::v_mac_f16:
   case aco_opcode::v_mac_legacy_f32:
   case aco_opcode::v_pk_fmac_f16: UNREACHABLE("Only created by RA."); return false;
   default: break;
   }

   info = {};

   info.opcode = instr->opcode;
   info.pass_flags = instr->pass_flags;

   if (instr->isSALU())
      info.imm = instr->salu().imm;

   bitarray8 opsel = 0;
   if (instr->isVALU()) {
      info.omod = instr->valu().omod;
      info.clamp = instr->valu().clamp;
      opsel = instr->valu().opsel;
   }

   if (instr->opcode == aco_opcode::v_permlane16_b32 ||
       instr->opcode == aco_opcode::v_permlanex16_b32) {
      info.imm = opsel;
      opsel = 0;
   }

   if (instr->opcode == aco_opcode::v_fma_mix_f32 || instr->opcode == aco_opcode::v_fma_mixlo_f16) {
      info.opcode = ctx.program->dev.fused_mad_mix ? aco_opcode::v_fma_f32 : aco_opcode::v_mad_f32;
      info.f32_to_f16 = instr->opcode == aco_opcode::v_fma_mixlo_f16;
   }

   if (instr->isSDWA())
      info.insert = instr->sdwa().dst_sel;
   else
      info.insert = SubdwordSel::dword;

   for (Definition& def : instr->definitions)
      info.defs.push_back(def);

   for (unsigned i = 0; i < instr->operands.size(); i++) {
      alu_opt_op op_info = {};
      op_info.op = instr->operands[i];
      if (instr->opcode == aco_opcode::v_fma_mix_f32 ||
          instr->opcode == aco_opcode::v_fma_mixlo_f16) {
         op_info.neg[0] = instr->valu().neg[i];
         op_info.abs[0] = instr->valu().abs[i];
         if (instr->valu().opsel_hi[i]) {
            op_info.f16_to_f32 = true;
            if (instr->valu().opsel_lo[i])
               op_info.extract[0] = SubdwordSel::uword1;
         }
      } else if (instr->isVOP3P()) {
         op_info.neg[0] = instr->valu().neg_lo[i];
         op_info.neg[1] = instr->valu().neg_hi[i];
         if (instr->valu().opsel_lo[i])
            op_info.extract[0] = SubdwordSel::uword1;
         if (instr->valu().opsel_hi[i])
            op_info.extract[1] = SubdwordSel::uword1;
      } else if (instr->isVALU() && i < 3) {
         op_info.neg[0] = instr->valu().neg[i];
         op_info.neg[1] = instr->valu().neg[i];
         op_info.abs[0] = instr->valu().abs[i];
         op_info.abs[1] = instr->valu().abs[i];
         if (opsel[i])
            op_info.extract[0] = SubdwordSel::uword1;
         op_info.extract[1] = SubdwordSel::uword1;

         if (i < 2 && instr->isSDWA())
            op_info.extract[0] = instr->sdwa().sel[i];
      }

      info.operands.push_back(op_info);
   }

   if (instr->isDPP16()) {
      info.operands[0].dpp16 = true;
      info.operands[0].dpp_ctrl = instr->dpp16().dpp_ctrl;
      info.operands[0].fi = instr->dpp16().fetch_inactive;
      info.operands[0].bc = instr->dpp16().bound_ctrl;
      assert(instr->dpp16().row_mask == 0xf && instr->dpp16().bank_mask == 0xf);
   } else if (instr->isDPP8()) {
      info.operands[0].dpp8 = true;
      info.operands[0].dpp_ctrl = instr->dpp8().lane_sel;
      info.operands[0].fi = instr->dpp8().fetch_inactive;
   }

   switch (info.opcode) {
   case aco_opcode::s_cvt_hi_f32_f16:
      info.operands[0].extract[0] = SubdwordSel::uword1;
      info.opcode = aco_opcode::s_cvt_f32_f16;
      break;
   case aco_opcode::s_pack_lh_b32_b16:
   case aco_opcode::s_pack_hl_b32_b16:
   case aco_opcode::s_pack_hh_b32_b16:
      if (info.opcode != aco_opcode::s_pack_lh_b32_b16)
         info.operands[0].extract[0] = SubdwordSel::uword1;
      if (info.opcode != aco_opcode::s_pack_hl_b32_b16)
         info.operands[1].extract[0] = SubdwordSel::uword1;
      info.opcode = aco_opcode::s_pack_ll_b32_b16;
      break;
   case aco_opcode::v_sub_f32:
   case aco_opcode::v_subrev_f32:
      info.operands[info.opcode == aco_opcode::v_sub_f32].neg[0] ^= true;
      info.opcode = aco_opcode::v_add_f32;
      break;
   case aco_opcode::v_sub_f16:
   case aco_opcode::v_subrev_f16:
      info.operands[info.opcode == aco_opcode::v_sub_f16].neg[0] ^= true;
      info.opcode = aco_opcode::v_add_f16;
      break;
   case aco_opcode::s_sub_f32:
      info.operands[1].neg[0] ^= true;
      info.opcode = aco_opcode::s_add_f32;
      break;
   case aco_opcode::s_sub_f16:
      info.operands[1].neg[0] ^= true;
      info.opcode = aco_opcode::s_add_f16;
      break;
   case aco_opcode::v_dot4_i32_iu8:
   case aco_opcode::v_dot8_i32_iu4:
      for (unsigned i = 0; i < 2; i++) {
         info.operands[i].dot_sext = info.operands[i].neg[0];
         info.operands[i].neg[0] = false;
      }
      break;
   case aco_opcode::v_mad_f32:
      if (ctx.fp_mode.denorm32)
         break;
      FALLTHROUGH;
   case aco_opcode::v_fma_f32:
      if (info.operands[2].op.constantEquals(0) && info.operands[2].neg[0]) {
         info.operands.pop_back();
         info.opcode = aco_opcode::v_mul_f32;
      } else {
         for (unsigned i = 0; i < 2; i++) {
            uint32_t one = info.operands[i].f16_to_f32 ? 0x3c00 : 0x3f800000;
            if (info.operands[i].op.constantEquals(one) && !info.operands[i].neg[0] &&
                info.operands[i].extract[0] == SubdwordSel::dword) {
               info.operands.erase(info.operands.begin() + i);
               info.opcode = aco_opcode::v_add_f32;
               break;
            }
         }
      }
      break;
   case aco_opcode::v_fmaak_f32:
   case aco_opcode::v_fmamk_f32:
      if (info.opcode == aco_opcode::v_fmamk_f32)
         std::swap(info.operands[1], info.operands[2]);
      info.opcode = aco_opcode::v_fma_f32;
      break;
   case aco_opcode::v_fmaak_f16:
   case aco_opcode::v_fmamk_f16:
      if (info.opcode == aco_opcode::v_fmamk_f16)
         std::swap(info.operands[1], info.operands[2]);
      info.opcode = aco_opcode::v_fma_f16;
      break;
   case aco_opcode::v_madak_f32:
   case aco_opcode::v_madmk_f32:
      if (info.opcode == aco_opcode::v_madmk_f32)
         std::swap(info.operands[1], info.operands[2]);
      info.opcode = aco_opcode::v_mad_f32;
      break;
   case aco_opcode::v_madak_f16:
   case aco_opcode::v_madmk_f16:
      if (info.opcode == aco_opcode::v_madmk_f16)
         std::swap(info.operands[1], info.operands[2]);
      info.opcode =
         ctx.program->gfx_level == GFX8 ? aco_opcode::v_mad_legacy_f16 : aco_opcode::v_mad_f16;
      break;
   case aco_opcode::s_fmaak_f32:
   case aco_opcode::s_fmamk_f32:
      if (info.opcode == aco_opcode::s_fmamk_f32)
         std::swap(info.operands[1], info.operands[2]);
      info.opcode = aco_opcode::s_fmac_f32;
      break;
   case aco_opcode::v_subbrev_co_u32:
      std::swap(info.operands[0], info.operands[1]);
      info.opcode = aco_opcode::v_subb_co_u32;
      break;
   case aco_opcode::v_subrev_co_u32:
      std::swap(info.operands[0], info.operands[1]);
      info.opcode = aco_opcode::v_sub_co_u32;
      break;
   case aco_opcode::v_subrev_co_u32_e64:
      std::swap(info.operands[0], info.operands[1]);
      info.opcode = aco_opcode::v_sub_co_u32_e64;
      break;
   case aco_opcode::v_subrev_u32:
      std::swap(info.operands[0], info.operands[1]);
      info.opcode = aco_opcode::v_sub_u32;
      break;
   default: break;
   }

   return true;
}

/* Convert an alu_opt_info to an ACO IR instruction.
 * alu_opt_info_is_valid must have been called and returned true before this.
 * If old_instr is large enough for the new instruction, it's reused.
 * Otherwise a new instruction is allocated.
 */
Instruction*
alu_opt_info_to_instr(opt_ctx& ctx, alu_opt_info& info, Instruction* old_instr)
{
   Instruction* instr;
   if (old_instr && old_instr->definitions.size() >= info.defs.size() &&
       old_instr->operands.size() >= info.operands.size() &&
       get_instr_data_size(old_instr->format) >= get_instr_data_size(info.format)) {
      instr = old_instr;
      while (instr->operands.size() > info.operands.size())
         instr->operands.pop_back();
      while (instr->definitions.size() > info.defs.size())
         instr->definitions.pop_back();
      instr->opcode = info.opcode;
      instr->format = info.format;

      if (instr->isVALU()) {
         instr->valu().abs = 0;
         instr->valu().neg = 0;
         instr->valu().opsel = 0;
         instr->valu().opsel_hi = 0;
         instr->valu().opsel_lo = 0;
      }
   } else {
      instr = create_instruction(info.opcode, info.format, info.operands.size(), info.defs.size());
   }

   instr->pass_flags = info.pass_flags;

   for (unsigned i = 0; i < info.defs.size(); i++) {
      instr->definitions[i] = info.defs[i];
      ctx.info[info.defs[i].tempId()].parent_instr = instr;
   }

   for (unsigned i = 0; i < info.operands.size(); i++) {
      instr->operands[i] = info.operands[i].op;
      if (instr->opcode == aco_opcode::v_fma_mix_f32 ||
          instr->opcode == aco_opcode::v_fma_mixlo_f16) {
         instr->valu().neg[i] = info.operands[i].neg[0];
         instr->valu().abs[i] = info.operands[i].abs[0];
         instr->valu().opsel_hi[i] = info.operands[i].f16_to_f32;
         instr->valu().opsel_lo[i] = info.operands[i].extract[0].offset();
      } else if (instr->isVOP3P()) {
         instr->valu().neg_lo[i] = info.operands[i].neg[0] || info.operands[i].dot_sext;
         instr->valu().neg_hi[i] = info.operands[i].neg[1];
         instr->valu().opsel_lo[i] = info.operands[i].extract[0].offset();
         instr->valu().opsel_hi[i] = info.operands[i].extract[1].offset();
      } else if (instr->isVALU()) {
         instr->valu().neg[i] = info.operands[i].neg[0];
         instr->valu().abs[i] = info.operands[i].abs[0];
         if (instr->isSDWA() && i < 2) {
            SubdwordSel sel = info.operands[i].extract[0];
            unsigned size = MIN2(sel.size(), info.operands[i].op.bytes());
            instr->sdwa().sel[i] = SubdwordSel(size, sel.offset(), sel.sign_extend());
         } else if (info.operands[i].extract[0].offset()) {
            instr->valu().opsel[i] = true;
         }
      }
   }

   if (instr->isVALU()) {
      instr->valu().omod = info.omod;
      instr->valu().clamp = info.clamp;
   }

   if (instr->isDPP16()) {
      instr->dpp16().dpp_ctrl = info.operands[0].dpp_ctrl;
      instr->dpp16().fetch_inactive = info.operands[0].fi;
      instr->dpp16().bound_ctrl = info.operands[0].bc;
      instr->dpp16().row_mask = 0xf;
      instr->dpp16().bank_mask = 0xf;
   } else if (instr->isDPP8()) {
      instr->dpp8().lane_sel = info.operands[0].dpp_ctrl;
      instr->dpp8().fetch_inactive = info.operands[0].fi;
   } else if (instr->isSDWA()) {
      instr->sdwa().dst_sel = info.insert;
      if (!instr->isVOPC() && instr->definitions[0].bytes() != 4) {
         instr->sdwa().dst_sel = SubdwordSel(instr->definitions[0].bytes(), 0, false);
         assert(instr->sdwa().dst_sel == info.insert || info.insert == SubdwordSel::dword);
      }
   } else if (instr->opcode == aco_opcode::v_permlane16_b32 ||
              instr->opcode == aco_opcode::v_permlanex16_b32) {
      instr->valu().opsel = info.imm;
   }

   if (instr->isSALU())
      instr->salu().imm = info.imm;

   return instr;
}

bool
can_use_VOP3(opt_ctx& ctx, const aco_ptr<Instruction>& instr)
{
   if (instr->isVOP3())
      return true;

   if (instr->isVOP3P() || instr->isVINTERP_INREG())
      return false;

   if (instr->operands.size() && instr->operands[0].isLiteral() && ctx.program->gfx_level < GFX10)
      return false;

   if (instr->isSDWA())
      return false;

   if (instr->isDPP() && ctx.program->gfx_level < GFX11)
      return false;

   return instr->opcode != aco_opcode::v_madmk_f32 && instr->opcode != aco_opcode::v_madak_f32 &&
          instr->opcode != aco_opcode::v_madmk_f16 && instr->opcode != aco_opcode::v_madak_f16 &&
          instr->opcode != aco_opcode::v_fmamk_f32 && instr->opcode != aco_opcode::v_fmaak_f32 &&
          instr->opcode != aco_opcode::v_fmamk_f16 && instr->opcode != aco_opcode::v_fmaak_f16 &&
          instr->opcode != aco_opcode::v_permlane64_b32 &&
          instr->opcode != aco_opcode::v_readlane_b32 &&
          instr->opcode != aco_opcode::v_writelane_b32 &&
          instr->opcode != aco_opcode::v_readfirstlane_b32;
}

bool
pseudo_propagate_temp(opt_ctx& ctx, aco_ptr<Instruction>& instr, Temp temp, unsigned index)
{
   if (instr->definitions.empty())
      return false;

   const bool vgpr =
      instr->opcode == aco_opcode::p_as_uniform ||
      std::all_of(instr->definitions.begin(), instr->definitions.end(),
                  [](const Definition& def) { return def.regClass().type() == RegType::vgpr; });

   /* don't propagate VGPRs into SGPR instructions */
   if (temp.type() == RegType::vgpr && !vgpr)
      return false;

   bool can_accept_sgpr =
      ctx.program->gfx_level >= GFX9 ||
      std::none_of(instr->definitions.begin(), instr->definitions.end(),
                   [](const Definition& def) { return def.regClass().is_subdword(); });

   switch (instr->opcode) {
   case aco_opcode::p_phi:
   case aco_opcode::p_linear_phi:
   case aco_opcode::p_parallelcopy:
   case aco_opcode::p_create_vector:
   case aco_opcode::p_start_linear_vgpr:
      if (temp.bytes() != instr->operands[index].bytes())
         return false;
      break;
   case aco_opcode::p_extract_vector:
   case aco_opcode::p_extract:
      if (temp.type() == RegType::sgpr && !can_accept_sgpr)
         return false;
      break;
   case aco_opcode::p_split_vector: {
      if (temp.type() == RegType::sgpr && !can_accept_sgpr)
         return false;
      /* don't increase the vector size */
      if (temp.bytes() > instr->operands[index].bytes())
         return false;
      /* We can decrease the vector size as smaller temporaries are only
       * propagated by p_as_uniform instructions.
       * If this propagation leads to invalid IR or hits the assertion below,
       * it means that some undefined bytes within a dword are begin accessed
       * and a bug in instruction_selection is likely. */
      int decrease = instr->operands[index].bytes() - temp.bytes();
      while (decrease > 0) {
         decrease -= instr->definitions.back().bytes();
         instr->definitions.pop_back();
      }
      assert(decrease == 0);
      break;
   }
   case aco_opcode::p_as_uniform:
      if (temp.regClass() == instr->definitions[0].regClass())
         instr->opcode = aco_opcode::p_parallelcopy;
      break;
   default: return false;
   }

   instr->operands[index].setTemp(temp);
   return true;
}

/* This expects the DPP modifier to be removed. */
bool
can_apply_sgprs(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   assert(instr->isVALU());
   if (instr->isSDWA() && ctx.program->gfx_level < GFX9)
      return false;
   return instr->opcode != aco_opcode::v_readfirstlane_b32 &&
          instr->opcode != aco_opcode::v_readlane_b32 &&
          instr->opcode != aco_opcode::v_readlane_b32_e64 &&
          instr->opcode != aco_opcode::v_writelane_b32 &&
          instr->opcode != aco_opcode::v_writelane_b32_e64 &&
          instr->opcode != aco_opcode::v_permlane16_b32 &&
          instr->opcode != aco_opcode::v_permlanex16_b32 &&
          instr->opcode != aco_opcode::v_permlane64_b32 &&
          instr->opcode != aco_opcode::v_interp_p1_f32 &&
          instr->opcode != aco_opcode::v_interp_p2_f32 &&
          instr->opcode != aco_opcode::v_interp_mov_f32 &&
          instr->opcode != aco_opcode::v_interp_p1ll_f16 &&
          instr->opcode != aco_opcode::v_interp_p1lv_f16 &&
          instr->opcode != aco_opcode::v_interp_p2_legacy_f16 &&
          instr->opcode != aco_opcode::v_interp_p2_f16 &&
          instr->opcode != aco_opcode::v_interp_p2_hi_f16 &&
          instr->opcode != aco_opcode::v_interp_p10_f32_inreg &&
          instr->opcode != aco_opcode::v_interp_p2_f32_inreg &&
          instr->opcode != aco_opcode::v_interp_p10_f16_f32_inreg &&
          instr->opcode != aco_opcode::v_interp_p2_f16_f32_inreg &&
          instr->opcode != aco_opcode::v_interp_p10_rtz_f16_f32_inreg &&
          instr->opcode != aco_opcode::v_interp_p2_rtz_f16_f32_inreg &&
          instr->opcode != aco_opcode::v_wmma_f32_16x16x16_f16 &&
          instr->opcode != aco_opcode::v_wmma_f32_16x16x16_bf16 &&
          instr->opcode != aco_opcode::v_wmma_f16_16x16x16_f16 &&
          instr->opcode != aco_opcode::v_wmma_bf16_16x16x16_bf16 &&
          instr->opcode != aco_opcode::v_wmma_i32_16x16x16_iu8 &&
          instr->opcode != aco_opcode::v_wmma_i32_16x16x16_iu4 &&
          instr->opcode != aco_opcode::v_wmma_f32_16x16x16_fp8_fp8 &&
          instr->opcode != aco_opcode::v_wmma_f32_16x16x16_fp8_bf8 &&
          instr->opcode != aco_opcode::v_wmma_f32_16x16x16_bf8_fp8 &&
          instr->opcode != aco_opcode::v_wmma_f32_16x16x16_bf8_bf8;
}

/* only covers special cases */
bool
pseudo_can_accept_constant(const aco_ptr<Instruction>& instr, unsigned operand)
{
   /* Fixed operands can't accept constants because we need them
    * to be in their fixed register.
    */
   assert(instr->operands.size() > operand);
   if (instr->operands[operand].isFixed())
      return false;

   switch (instr->opcode) {
   case aco_opcode::p_extract_vector:
   case aco_opcode::p_split_vector:
   case aco_opcode::p_extract:
   case aco_opcode::p_insert: return operand != 0;
   case aco_opcode::p_bpermute_readlane:
   case aco_opcode::p_bpermute_shared_vgpr:
   case aco_opcode::p_bpermute_permlane:
   case aco_opcode::p_permlane64_shared_vgpr:
   case aco_opcode::p_interp_gfx11:
   case aco_opcode::p_dual_src_export_gfx11: return false;
   default: return true;
   }
}

/* check constant bus and literal limitations */
bool
check_vop3_operands(opt_ctx& ctx, unsigned num_operands, Operand* operands)
{
   int limit = ctx.program->gfx_level >= GFX10 ? 2 : 1;
   Operand literal32(s1);
   Operand literal64(s2);
   unsigned num_sgprs = 0;
   unsigned sgpr[] = {0, 0};

   for (unsigned i = 0; i < num_operands; i++) {
      Operand op = operands[i];

      if (op.hasRegClass() && op.regClass().type() == RegType::sgpr) {
         /* two reads of the same SGPR count as 1 to the limit */
         if (op.tempId() != sgpr[0] && op.tempId() != sgpr[1]) {
            if (num_sgprs < 2)
               sgpr[num_sgprs++] = op.tempId();
            limit--;
            if (limit < 0)
               return false;
         }
      } else if (op.isLiteral()) {
         if (ctx.program->gfx_level < GFX10)
            return false;

         if (!literal32.isUndefined() && literal32.constantValue() != op.constantValue())
            return false;
         if (!literal64.isUndefined() && literal64.constantValue() != op.constantValue())
            return false;

         /* Any number of 32-bit literals counts as only 1 to the limit. Same
          * (but separately) for 64-bit literals. */
         if (op.size() == 1 && literal32.isUndefined()) {
            limit--;
            literal32 = op;
         } else if (op.size() == 2 && literal64.isUndefined()) {
            limit--;
            literal64 = op;
         }

         if (limit < 0)
            return false;
      }
   }

   return true;
}

bool
parse_base_offset(opt_ctx& ctx, Instruction* instr, unsigned op_index, Temp* base, uint32_t* offset,
                  bool prevent_overflow)
{
   Operand op = instr->operands[op_index];

   if (!op.isTemp())
      return false;
   Temp tmp = op.getTemp();

   Instruction* add_instr = ctx.info[tmp.id()].parent_instr;

   if (add_instr->definitions[0].getTemp() != tmp)
      return false;

   unsigned mask = 0x3;
   bool is_sub = false;
   switch (add_instr->opcode) {
   case aco_opcode::v_add_u32:
   case aco_opcode::v_add_co_u32:
   case aco_opcode::v_add_co_u32_e64:
   case aco_opcode::s_add_i32:
   case aco_opcode::s_add_u32: break;
   case aco_opcode::v_sub_u32:
   case aco_opcode::v_sub_i32:
   case aco_opcode::v_sub_co_u32:
   case aco_opcode::v_sub_co_u32_e64:
   case aco_opcode::s_sub_u32:
   case aco_opcode::s_sub_i32:
      mask = 0x2;
      is_sub = true;
      break;
   case aco_opcode::v_subrev_u32:
   case aco_opcode::v_subrev_co_u32:
   case aco_opcode::v_subrev_co_u32_e64:
      mask = 0x1;
      is_sub = true;
      break;
   default: return false;
   }
   if (prevent_overflow && !add_instr->definitions[0].isNUW())
      return false;

   if (add_instr->usesModifiers())
      return false;

   u_foreach_bit (i, mask) {
      if (add_instr->operands[i].isConstant()) {
         *offset = add_instr->operands[i].constantValue() * (uint32_t)(is_sub ? -1 : 1);
      } else if (add_instr->operands[i].isTemp() &&
                 ctx.info[add_instr->operands[i].tempId()].is_constant_or_literal(32)) {
         *offset = ctx.info[add_instr->operands[i].tempId()].val * (uint32_t)(is_sub ? -1 : 1);
      } else {
         continue;
      }
      if (!add_instr->operands[!i].isTemp())
         continue;

      uint32_t offset2 = 0;
      if (parse_base_offset(ctx, add_instr, !i, base, &offset2, prevent_overflow)) {
         *offset += offset2;
      } else {
         *base = add_instr->operands[!i].getTemp();
      }
      return true;
   }

   return false;
}

void
skip_smem_offset_align(opt_ctx& ctx, SMEM_instruction* smem, uint32_t align)
{
   bool soe = smem->operands.size() >= (!smem->definitions.empty() ? 3 : 4);
   if (soe && !smem->operands[1].isConstant())
      return;
   /* We don't need to check the constant offset because the address seems to be calculated with
    * (offset&-4 + const_offset&-4), not (offset+const_offset)&-4.
    */

   Operand& op = smem->operands[soe ? smem->operands.size() - 1 : 1];
   if (!op.isTemp())
      return;

   Instruction* bitwise_instr = ctx.info[op.tempId()].parent_instr;
   if (bitwise_instr->opcode != aco_opcode::s_and_b32 ||
       bitwise_instr->definitions[0].getTemp() != op.getTemp())
      return;

   uint32_t mask = ~(align - 1u);
   if (bitwise_instr->operands[0].constantEquals(mask) &&
       bitwise_instr->operands[1].isOfType(op.regClass().type()))
      op.setTemp(bitwise_instr->operands[1].getTemp());
   else if (bitwise_instr->operands[1].constantEquals(mask) &&
            bitwise_instr->operands[0].isOfType(op.regClass().type()))
      op.setTemp(bitwise_instr->operands[0].getTemp());
}

void
smem_combine(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   uint32_t align = 4;
   switch (instr->opcode) {
   case aco_opcode::s_load_sbyte:
   case aco_opcode::s_load_ubyte:
   case aco_opcode::s_buffer_load_sbyte:
   case aco_opcode::s_buffer_load_ubyte: align = 1; break;
   case aco_opcode::s_load_sshort:
   case aco_opcode::s_load_ushort:
   case aco_opcode::s_buffer_load_sshort:
   case aco_opcode::s_buffer_load_ushort: align = 2; break;
   default: break;
   }

   /* skip &-4 before offset additions: load((a + 16) & -4, 0) */
   if (!instr->operands.empty() && align > 1)
      skip_smem_offset_align(ctx, &instr->smem(), align);

   /* propagate constants and combine additions */
   if (!instr->operands.empty() && instr->operands[1].isTemp()) {
      SMEM_instruction& smem = instr->smem();
      ssa_info info = ctx.info[instr->operands[1].tempId()];

      Temp base;
      uint32_t offset;
      if (info.is_constant_or_literal(32) && info.val <= ctx.program->dev.smem_offset_max) {
         instr->operands[1] = Operand::c32(info.val);
      } else if (parse_base_offset(ctx, instr.get(), 1, &base, &offset, true) &&
                 base.regClass() == s1 && offset <= ctx.program->dev.smem_offset_max &&
                 ctx.program->gfx_level >= GFX9 && offset % align == 0) {
         bool soe = smem.operands.size() >= (!smem.definitions.empty() ? 3 : 4);
         if (soe) {
            if (ctx.info[smem.operands.back().tempId()].is_constant_or_literal(32) &&
                ctx.info[smem.operands.back().tempId()].val == 0) {
               smem.operands[1] = Operand::c32(offset);
               smem.operands.back() = Operand(base);
            }
         } else {
            Instruction* new_instr = create_instruction(
               smem.opcode, Format::SMEM, smem.operands.size() + 1, smem.definitions.size());
            new_instr->operands[0] = smem.operands[0];
            new_instr->operands[1] = Operand::c32(offset);
            if (smem.definitions.empty())
               new_instr->operands[2] = smem.operands[2];
            new_instr->operands.back() = Operand(base);
            if (!smem.definitions.empty())
               new_instr->definitions[0] = smem.definitions[0];
            new_instr->smem().sync = smem.sync;
            new_instr->smem().cache = smem.cache;
            instr.reset(new_instr);
         }
      }
   }

   /* skip &-4 after offset additions: load(a & -4, 16) */
   if (!instr->operands.empty() && align > 1)
      skip_smem_offset_align(ctx, &instr->smem(), align);
}

Operand
get_constant_op(opt_ctx& ctx, ssa_info info, uint32_t bits)
{
   if (bits == 64)
      return Operand::c32_or_c64(info.val, true);
   return Operand::get_const(ctx.program->gfx_level, info.val, bits / 8u);
}

bool
fixed_to_exec(Operand op)
{
   return op.isFixed() && op.physReg() == exec;
}

SubdwordSel
parse_extract(Instruction* instr)
{
   if (instr->opcode == aco_opcode::p_extract) {
      unsigned size = instr->operands[2].constantValue() / 8;
      unsigned offset = instr->operands[1].constantValue() * size;
      bool sext = instr->operands[3].constantEquals(1);
      return SubdwordSel(size, offset, sext);
   } else if (instr->opcode == aco_opcode::p_insert && instr->operands[1].constantEquals(0)) {
      return instr->operands[2].constantEquals(8) ? SubdwordSel::ubyte : SubdwordSel::uword;
   } else if (instr->opcode == aco_opcode::p_extract_vector) {
      unsigned size = instr->definitions[0].bytes();
      unsigned offset = instr->operands[1].constantValue() * size;
      if (size <= 2)
         return SubdwordSel(size, offset, false);
   } else if (instr->opcode == aco_opcode::p_split_vector) {
      assert(instr->operands[0].bytes() == 4 && instr->definitions[1].bytes() == 2);
      return SubdwordSel(2, 2, false);
   }

   return SubdwordSel();
}

SubdwordSel
parse_insert(Instruction* instr)
{
   if (instr->opcode == aco_opcode::p_extract && instr->operands[3].constantEquals(0) &&
       instr->operands[1].constantEquals(0)) {
      return instr->operands[2].constantEquals(8) ? SubdwordSel::ubyte : SubdwordSel::uword;
   } else if (instr->opcode == aco_opcode::p_insert) {
      unsigned size = instr->operands[2].constantValue() / 8;
      unsigned offset = instr->operands[1].constantValue() * size;
      return SubdwordSel(size, offset, false);
   } else {
      return SubdwordSel();
   }
}

SubdwordSel
apply_extract_twice(SubdwordSel first, Temp first_dst, SubdwordSel second, Temp second_dst)
{
   /* the outer offset must be within extracted range */
   if (second.offset() >= first.size())
      return SubdwordSel();

   /* don't remove the sign-extension when increasing the size further */
   if (second.size() > first.size() && first.sign_extend() &&
       !(second.sign_extend() ||
         (second.size() == first_dst.bytes() && second.size() == second_dst.bytes())))
      return SubdwordSel();

   unsigned size = std::min(first.size(), second.size());
   unsigned offset = first.offset() + second.offset();
   bool sign_extend = second.size() <= first.size() ? second.sign_extend() : first.sign_extend();
   return SubdwordSel(size, offset, sign_extend);
}

bool
can_apply_extract(opt_ctx& ctx, aco_ptr<Instruction>& instr, unsigned idx, ssa_info& info)
{
   Temp tmp = info.parent_instr->operands[0].getTemp();
   SubdwordSel sel = parse_extract(info.parent_instr);

   if (!sel) {
      return false;
   } else if (sel.size() == instr->operands[idx].bytes() && sel.size() == tmp.bytes() &&
              tmp.type() == instr->operands[idx].regClass().type()) {
      assert(tmp.type() != RegType::sgpr); /* No sub-dword SGPR regclasses */
      return true;
   } else if ((instr->opcode == aco_opcode::v_cvt_f32_u32 ||
               instr->opcode == aco_opcode::v_cvt_f32_i32 ||
               instr->opcode == aco_opcode::v_cvt_f32_ubyte0) &&
              sel.size() == 1 && !sel.sign_extend() && !instr->usesModifiers()) {
      return true;
   } else if (instr->opcode == aco_opcode::v_lshlrev_b32 && instr->operands[0].isConstant() &&
              sel.offset() == 0 && !instr->usesModifiers() &&
              ((sel.size() == 2 && instr->operands[0].constantValue() >= 16u) ||
               (sel.size() == 1 && instr->operands[0].constantValue() >= 24u))) {
      return true;
   } else if (instr->opcode == aco_opcode::v_mul_u32_u24 && ctx.program->gfx_level >= GFX10 &&
              !instr->usesModifiers() && sel.size() == 2 && !sel.sign_extend() &&
              (instr->operands[!idx].is16bit() ||
               (instr->operands[!idx].isConstant() &&
                instr->operands[!idx].constantValue() <= UINT16_MAX))) {
      return true;
   } else if (idx < 2 && can_use_SDWA(ctx.program->gfx_level, instr, true) &&
              (tmp.type() == RegType::vgpr || ctx.program->gfx_level >= GFX9)) {
      if (instr->isSDWA()) {
         /* TODO: if we knew how many bytes this operand actually uses, we could have smaller
          * second_dst parameter and apply more sign-extended sels.
          */
         return apply_extract_twice(sel, instr->operands[idx].getTemp(), instr->sdwa().sel[idx],
                                    Temp(0, v1)) != SubdwordSel();
      }
      return true;
   } else if (instr->isVALU() && sel.size() == 2 && !instr->valu().opsel[idx] &&
              can_use_opsel(ctx.program->gfx_level, instr->opcode, idx)) {
      return true;
   } else if (instr->opcode == aco_opcode::s_pack_ll_b32_b16 && sel.size() == 2 &&
              (idx == 1 || ctx.program->gfx_level >= GFX11 || !sel.offset())) {
      return true;
   } else if (sel.size() == 2 && ((instr->opcode == aco_opcode::s_pack_lh_b32_b16 && idx == 0) ||
                                  (instr->opcode == aco_opcode::s_pack_hl_b32_b16 && idx == 1))) {
      return true;
   } else if (instr->opcode == aco_opcode::p_extract ||
              instr->opcode == aco_opcode::p_extract_vector) {
      if (ctx.program->gfx_level < GFX9 &&
          !info.parent_instr->operands[0].isOfType(RegType::vgpr) &&
          instr->definitions[0].regClass().is_subdword())
         return false;

      SubdwordSel instrSel = parse_extract(instr.get());
      return instrSel && apply_extract_twice(sel, instr->operands[idx].getTemp(), instrSel,
                                             instr->definitions[0].getTemp());
   }

   return false;
}

/* Combine an p_extract (or p_insert, in some cases) instruction with instr.
 * instr(p_extract(...)) -> instr()
 */
void
apply_extract(opt_ctx& ctx, aco_ptr<Instruction>& instr, unsigned idx, ssa_info& info)
{
   Temp tmp = info.parent_instr->operands[0].getTemp();
   SubdwordSel sel = parse_extract(info.parent_instr);
   assert(sel);

   instr->operands[idx].set16bit(false);
   instr->operands[idx].set24bit(false);

   ctx.info[tmp.id()].label &= ~label_insert;

   if (sel.size() == instr->operands[idx].bytes() && sel.size() == tmp.bytes() &&
       tmp.type() == instr->operands[idx].regClass().type()) {
      /* extract is a no-op */
   } else if ((instr->opcode == aco_opcode::v_cvt_f32_u32 ||
               instr->opcode == aco_opcode::v_cvt_f32_i32 ||
               instr->opcode == aco_opcode::v_cvt_f32_ubyte0) &&
              sel.size() == 1 && !sel.sign_extend() && !instr->usesModifiers()) {
      switch (sel.offset()) {
      case 0: instr->opcode = aco_opcode::v_cvt_f32_ubyte0; break;
      case 1: instr->opcode = aco_opcode::v_cvt_f32_ubyte1; break;
      case 2: instr->opcode = aco_opcode::v_cvt_f32_ubyte2; break;
      case 3: instr->opcode = aco_opcode::v_cvt_f32_ubyte3; break;
      }
   } else if (instr->opcode == aco_opcode::v_lshlrev_b32 && instr->operands[0].isConstant() &&
              sel.offset() == 0 && !instr->usesModifiers() &&
              ((sel.size() == 2 && instr->operands[0].constantValue() >= 16u) ||
               (sel.size() == 1 && instr->operands[0].constantValue() >= 24u))) {
      /* The undesirable upper bits are already shifted out. */
      if (!instr->isVOP3() && !info.parent_instr->operands[0].isOfType(RegType::vgpr))
         instr->format = asVOP3(instr->format);
      return;
   } else if (instr->opcode == aco_opcode::v_mul_u32_u24 && ctx.program->gfx_level >= GFX10 &&
              !instr->usesModifiers() && sel.size() == 2 && !sel.sign_extend() &&
              (instr->operands[!idx].is16bit() ||
               instr->operands[!idx].constantValue() <= UINT16_MAX)) {
      Instruction* mad = create_instruction(aco_opcode::v_mad_u32_u16, Format::VOP3, 3, 1);
      mad->definitions[0] = instr->definitions[0];
      mad->operands[0] = instr->operands[0];
      mad->operands[1] = instr->operands[1];
      mad->operands[2] = Operand::zero();
      mad->valu().opsel[idx] = sel.offset();
      mad->pass_flags = instr->pass_flags;
      instr.reset(mad);
   } else if (can_use_SDWA(ctx.program->gfx_level, instr, true) &&
              (tmp.type() == RegType::vgpr || ctx.program->gfx_level >= GFX9)) {
      if (instr->isSDWA()) {
         instr->sdwa().sel[idx] = apply_extract_twice(sel, instr->operands[idx].getTemp(),
                                                      instr->sdwa().sel[idx], Temp(0, v1));
      } else {
         convert_to_SDWA(ctx.program->gfx_level, instr);
         instr->sdwa().sel[idx] = sel;
      }
   } else if (instr->isVALU()) {
      if (sel.offset()) {
         instr->valu().opsel[idx] = true;

         /* VOP12C cannot use opsel with SGPRs. */
         if (!instr->isVOP3() && !instr->isVINTERP_INREG() &&
             !info.parent_instr->operands[0].isOfType(RegType::vgpr))
            instr->format = asVOP3(instr->format);
      }
   } else if (instr->opcode == aco_opcode::s_pack_ll_b32_b16) {
      if (sel.offset())
         instr->opcode = idx ? aco_opcode::s_pack_lh_b32_b16 : aco_opcode::s_pack_hl_b32_b16;
   } else if (instr->opcode == aco_opcode::s_pack_lh_b32_b16 ||
              instr->opcode == aco_opcode::s_pack_hl_b32_b16) {
      if (sel.offset())
         instr->opcode = aco_opcode::s_pack_hh_b32_b16;
   } else if (instr->opcode == aco_opcode::p_extract) {
      SubdwordSel instr_sel = parse_extract(instr.get());
      SubdwordSel new_sel = apply_extract_twice(sel, instr->operands[idx].getTemp(), instr_sel,
                                                instr->definitions[0].getTemp());
      assert(new_sel.size() <= 2);

      instr->operands[1] = Operand::c32(new_sel.offset() / new_sel.size());
      instr->operands[2] = Operand::c32(new_sel.size() * 8u);
      instr->operands[3] = Operand::c32(new_sel.sign_extend());
      return;
   } else if (instr->opcode == aco_opcode::p_extract_vector) {
      SubdwordSel instrSel = parse_extract(instr.get());
      SubdwordSel new_sel = apply_extract_twice(sel, instr->operands[idx].getTemp(), instrSel,
                                                instr->definitions[0].getTemp());
      assert(new_sel.size() <= 2);

      if (new_sel.size() == instr->definitions[0].bytes()) {
         instr->operands[1] = Operand::c32(new_sel.offset() / instr->definitions[0].bytes());
         return;
      } else {
         /* parse_extract() only succeeds with p_extract_vector for VGPR definitions because there
          * are no sub-dword SGPR regclasses. */
         assert(instr->definitions[0].regClass().type() != RegType::sgpr);

         Instruction* ext = create_instruction(aco_opcode::p_extract, Format::PSEUDO, 4, 1);
         ext->definitions[0] = instr->definitions[0];
         ext->operands[0] = instr->operands[0];
         ext->operands[1] = Operand::c32(new_sel.offset() / new_sel.size());
         ext->operands[2] = Operand::c32(new_sel.size() * 8u);
         ext->operands[3] = Operand::c32(new_sel.sign_extend());
         ext->pass_flags = instr->pass_flags;
         instr.reset(ext);
      }
   }

   /* These are the only labels worth keeping at the moment. */
   for (Definition& def : instr->definitions) {
      ctx.info[def.tempId()].label &= instr_mod_labels;
      ctx.info[def.tempId()].parent_instr = instr.get();
   }
}

void
check_sdwa_extract(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   for (unsigned i = 0; i < instr->operands.size(); i++) {
      Operand op = instr->operands[i];
      if (!op.isTemp())
         continue;
      ssa_info& info = ctx.info[op.tempId()];
      if (info.is_extract() && (info.parent_instr->operands[0].getTemp().type() == RegType::vgpr ||
                                op.getTemp().type() == RegType::sgpr)) {
         if (!can_apply_extract(ctx, instr, i, info))
            info.label &= ~label_extract;
      }
   }
}

bool
does_fp_op_flush_denorms(opt_ctx& ctx, aco_opcode op)
{
   switch (op) {
   case aco_opcode::v_min_f32:
   case aco_opcode::v_max_f32:
   case aco_opcode::v_med3_f32:
   case aco_opcode::v_min3_f32:
   case aco_opcode::v_max3_f32:
   case aco_opcode::v_min_f16:
   case aco_opcode::v_max_f16: return ctx.program->gfx_level > GFX8;
   case aco_opcode::v_cndmask_b32:
   case aco_opcode::v_cndmask_b16:
   case aco_opcode::v_mov_b32:
   case aco_opcode::v_mov_b16: return false;
   default: return true;
   }
}

bool
can_eliminate_fcanonicalize(opt_ctx& ctx, aco_ptr<Instruction>& instr, Temp tmp, unsigned idx)
{
   float_mode* fp = &ctx.fp_mode;
   if (ctx.info[tmp.id()].is_canonicalized() ||
       (tmp.bytes() == 4 ? fp->denorm32 : fp->denorm16_64) == fp_denorm_keep)
      return true;

   aco_opcode op = instr->opcode;
   return can_use_input_modifiers(ctx.program->gfx_level, instr->opcode, idx) &&
          does_fp_op_flush_denorms(ctx, op);
}

bool
can_eliminate_and_exec(opt_ctx& ctx, Temp tmp, unsigned pass_flags, bool allow_cselect = false)
{
   Instruction* instr = ctx.info[tmp.id()].parent_instr;
   /* Remove superfluous s_and when the VOPC instruction uses the same exec and thus
    * already produces the same result */
   if (instr->isVOPC())
      return instr->pass_flags == pass_flags;

   if (allow_cselect && instr->pass_flags == pass_flags &&
       (instr->opcode == aco_opcode::s_cselect_b32 || instr->opcode == aco_opcode::s_cselect_b64)) {
      return (instr->operands[0].constantEquals(0) && instr->operands[1].constantEquals(-1)) ||
             (instr->operands[1].constantEquals(0) && instr->operands[0].constantEquals(-1));
   }

   if (instr->operands.size() != 2 || instr->pass_flags != pass_flags)
      return false;
   if (!(instr->operands[0].isTemp() && instr->operands[1].isTemp()))
      return false;

   switch (instr->opcode) {
   case aco_opcode::s_and_b32:
   case aco_opcode::s_and_b64:
      return can_eliminate_and_exec(ctx, instr->operands[0].getTemp(), pass_flags) ||
             can_eliminate_and_exec(ctx, instr->operands[1].getTemp(), pass_flags);
   case aco_opcode::s_or_b32:
   case aco_opcode::s_or_b64:
   case aco_opcode::s_xor_b32:
   case aco_opcode::s_xor_b64:
      return can_eliminate_and_exec(ctx, instr->operands[0].getTemp(), pass_flags) &&
             can_eliminate_and_exec(ctx, instr->operands[1].getTemp(), pass_flags);
   default: return false;
   }
}

bool
is_copy_label(opt_ctx& ctx, aco_ptr<Instruction>& instr, ssa_info& info, unsigned idx)
{
   return info.is_temp() ||
          (info.is_fcanonicalize() && can_eliminate_fcanonicalize(ctx, instr, info.temp, idx));
}

bool
is_op_canonicalized(opt_ctx& ctx, Operand op)
{
   float_mode* fp = &ctx.fp_mode;
   if ((op.isTemp() && ctx.info[op.tempId()].is_canonicalized()) ||
       (op.bytes() == 4 ? fp->denorm32 : fp->denorm16_64) == fp_denorm_keep)
      return true;

   if (op.isConstant() || (op.isTemp() && ctx.info[op.tempId()].is_constant_or_literal(32))) {
      uint32_t val = op.isTemp() ? ctx.info[op.tempId()].val : op.constantValue();
      if (op.bytes() == 2)
         return (val & 0x7fff) == 0 || (val & 0x7fff) > 0x3ff;
      else if (op.bytes() == 4)
         return (val & 0x7fffffff) == 0 || (val & 0x7fffffff) > 0x7fffff;
   }
   return false;
}

bool
is_scratch_offset_valid(opt_ctx& ctx, Instruction* instr, int64_t offset0, int64_t offset1)
{
   bool negative_unaligned_scratch_offset_bug = ctx.program->gfx_level == GFX10;
   int32_t min = ctx.program->dev.scratch_global_offset_min;
   int32_t max = ctx.program->dev.scratch_global_offset_max;

   int64_t offset = offset0 + offset1;

   bool has_vgpr_offset = instr && !instr->operands[0].isUndefined();
   if (negative_unaligned_scratch_offset_bug && has_vgpr_offset && offset < 0 && offset % 4)
      return false;

   return offset >= min && offset <= max;
}

bool
detect_clamp(Instruction* instr, unsigned* clamped_idx)
{
   VALU_instruction& valu = instr->valu();
   if (valu.omod != 0 || valu.opsel != 0)
      return false;

   unsigned idx = 0;
   bool found_zero = false, found_one = false;
   bool is_fp16 = instr->opcode == aco_opcode::v_med3_f16;
   for (unsigned i = 0; i < 3; i++) {
      if (!valu.neg[i] && instr->operands[i].constantEquals(0))
         found_zero = true;
      else if (!valu.neg[i] &&
               instr->operands[i].constantEquals(is_fp16 ? 0x3c00 : 0x3f800000)) /* 1.0 */
         found_one = true;
      else
         idx = i;
   }
   if (found_zero && found_one && instr->operands[idx].isTemp()) {
      *clamped_idx = idx;
      return true;
   } else {
      return false;
   }
}

bool
parse_operand(opt_ctx& ctx, Temp tmp, alu_opt_op& op_info, aco_type& type)
{
   ssa_info info = ctx.info[tmp.id()];
   op_info = {};
   type = {};

   if (info.parent_instr->opcode == aco_opcode::v_pk_mul_f16 &&
       (info.parent_instr->operands[0].constantEquals(0x3c00) ||
        info.parent_instr->operands[1].constantEquals(0x3c00) ||
        info.parent_instr->operands[0].constantEquals(0xbc00) ||
        info.parent_instr->operands[1].constantEquals(0xbc00))) {

      VALU_instruction* fneg = &info.parent_instr->valu();

      unsigned fneg_src =
         fneg->operands[0].constantEquals(0x3c00) || fneg->operands[0].constantEquals(0xbc00);

      if (fneg->opsel_lo[1 - fneg_src] || fneg->opsel_hi[1 - fneg_src])
         return false;

      if (fneg->clamp || fneg->isDPP())
         return false;

      type.base_type = aco_base_type_float;
      type.num_components = 2;
      type.bit_size = 16;

      op_info.op = fneg->operands[fneg_src];
      if (fneg->opsel_lo[fneg_src])
         op_info.extract[0] = SubdwordSel::uword1;
      if (fneg->opsel_hi[fneg_src])
         op_info.extract[1] = SubdwordSel::uword1;
      op_info.neg[0] =
         fneg->operands[1 - fneg_src].constantEquals(0xbc00) ^ fneg->neg_lo[0] ^ fneg->neg_lo[1];
      op_info.neg[1] =
         fneg->operands[1 - fneg_src].constantEquals(0xbc00) ^ fneg->neg_hi[0] ^ fneg->neg_hi[1];
      return true;
   }

   // TODO use parent dst type
   if (info.is_fcanonicalize() || info.is_abs() || info.is_neg()) {
      if (ctx.info[info.temp.id()].is_canonicalized() ||
          (tmp.bytes() == 4 ? ctx.fp_mode.denorm32 : ctx.fp_mode.denorm16_64) == fp_denorm_keep)
         type.base_type = aco_base_type_uint;
      else
         type.base_type = aco_base_type_float;
   } else {
      type.base_type = aco_base_type_uint;
   }
   type.num_components = 1;
   type.bit_size = tmp.bytes() * 8;

   if (info.is_constant_or_literal(type.bit_size)) {
      op_info.op = get_constant_op(ctx, info, type.bit_size);
      return true;
   }

   if (info.is_temp() || info.is_fcanonicalize() || info.is_abs() || info.is_neg()) {
      op_info.op = Operand(info.temp);
      if (info.is_abs())
         op_info.abs[0] = true;
      if (info.is_neg())
         op_info.neg[0] = true;
      return true;
   }
   return false;
}

bool
combine_operand(opt_ctx& ctx, alu_opt_op& inner, const aco_type& inner_type,
                const alu_opt_op& outer, const aco_type& outer_type, bool flushes_denorms)
{
   if (inner.op.size() != outer.op.size())
      return false;

   if (outer_type.base_type != aco_base_type_uint && !flushes_denorms)
      return false;

   bool has_imod = outer.abs[0] || outer.neg[0] || outer.abs[1] || outer.neg[1] ||
                   outer_type.base_type != aco_base_type_uint;
   if (has_imod && outer_type.bit_size != inner_type.bit_size)
      return false;

   for (unsigned i = 0; i < inner_type.num_components; i++) {
      unsigned offset = inner.extract[i].offset() * 8;
      unsigned size = MIN2(inner.extract[i].size() * 8, inner_type.bit_size);
      unsigned out_comp = offset / outer_type.bit_size;
      unsigned rem_off = offset % outer_type.bit_size;
      if (rem_off && has_imod)
         return false;
      if (out_comp > outer_type.num_components)
         return false;
      if (size > outer_type.bit_size && (out_comp + 1) != outer_type.num_components)
         return false;
      if (rem_off >= outer.extract[out_comp].size() * 8)
         return false;
      if (size < inner_type.bit_size && size > outer.extract[out_comp].size() * 8 &&
          outer.extract[out_comp].sign_extend() && !inner.extract[i].sign_extend())
         return false;

      bool sign_extend = size <= outer.extract[out_comp].size() * 8
                            ? inner.extract[i].sign_extend()
                            : outer.extract[out_comp].sign_extend();
      unsigned new_off = (rem_off / 8) + outer.extract[out_comp].offset();
      unsigned new_size = MIN2(size / 8, outer.extract[i].size());
      inner.extract[i] = SubdwordSel(new_size, new_off, sign_extend);

      if (size == outer_type.bit_size) {
         inner.neg[i] ^= !inner.abs[i] && outer.neg[out_comp];
         inner.abs[i] |= outer.abs[out_comp];
      } else if (outer_type.base_type != aco_base_type_uint) {
         return false;
      }
   }

   if (outer.op.isTemp())
      inner.op.setTemp(outer.op.getTemp());
   else if (inner.op.isFixed())
      return false;
   else
      inner.op = outer.op;
   return true;
}

void
decrease_and_dce(opt_ctx& ctx, Temp tmp)
{
   assert(ctx.uses[tmp.id()]);
   ctx.uses[tmp.id()]--;
   Instruction* instr = ctx.info[tmp.id()].parent_instr;
   if (is_dead(ctx.uses, instr)) {
      for (const Operand& op : instr->operands) {
         if (op.isTemp())
            decrease_and_dce(ctx, op.getTemp());
      }
   }
}

void
alu_propagate_temp_const(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   alu_opt_info info;
   if (!alu_opt_gather_info(ctx, instr.get(), info))
      return;

   bool had_lit = std::any_of(info.operands.begin(), info.operands.end(),
                              [](const alu_opt_op& op) { return op.op.isLiteral(); });

   const bool gfx8_min_max =
      ctx.program->gfx_level < GFX9 &&
      (instr->opcode == aco_opcode::v_min_f32 || instr->opcode == aco_opcode::v_max_f32 ||
       instr->opcode == aco_opcode::v_min_f16 || instr->opcode == aco_opcode::v_max_f16 ||
       instr->opcode == aco_opcode::v_min_f64_e64 || instr->opcode == aco_opcode::v_max_f64_e64 ||
       instr->opcode == aco_opcode::v_min3_f32 || instr->opcode == aco_opcode::v_max3_f32 ||
       instr->opcode == aco_opcode::v_med3_f32);

   bool progress = false;
   alu_opt_info result_info;
   for (unsigned i = 0; i < info.operands.size(); i++) {
      while (info.operands[i].op.isTemp()) {
         alu_opt_op outer;
         aco_type outer_type;
         if (!parse_operand(ctx, info.operands[i].op.getTemp(), outer, outer_type))
            break;

         /* Applying SGPRs to VOP1 doesn't increase code size and DCE is helped by doing it earlier,
          * otherwise we apply SGPRs later.
          */
         if (info.operands[i].op.isOfType(RegType::vgpr) && outer.op.isOfType(RegType::sgpr) &&
             !instr->isVOP1())
            break;

         alu_opt_op inner = info.operands[i];
         aco_type inner_type = get_canonical_operand_type(info.opcode, i);
         if (inner.f16_to_f32)
            inner_type.bit_size = 16;
         bool flushes_denorms = inner_type.base_type == aco_base_type_float && !gfx8_min_max;
         if (!combine_operand(ctx, inner, inner_type, outer, outer_type, flushes_denorms))
            break;

         alu_opt_info info_copy = info;
         info_copy.operands[i] = inner;
         if (!alu_opt_info_is_valid(ctx, info_copy))
            break;

         bool has_lit = std::any_of(info_copy.operands.begin(), info_copy.operands.end(),
                                    [](const alu_opt_op& op) { return op.op.isLiteral(); });

         if (!had_lit && has_lit)
            break;

         result_info = info_copy;
         info.operands[i] = inner;
         progress = true;
      }
   }

   if (!progress)
      return;

   instr.reset(alu_opt_info_to_instr(ctx, result_info, instr.release()));
   for (const Definition& def : instr->definitions)
      ctx.info[def.tempId()].label &= instr_mod_labels | label_canonicalized;
}

void
label_instruction(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (instr->isSMEM())
      smem_combine(ctx, instr);

   for (unsigned i = 0; i < instr->operands.size(); i++) {
      if (!instr->operands[i].isTemp())
         continue;

      ssa_info info = ctx.info[instr->operands[i].tempId()];
      /* propagate reg->reg of same type */
      while (info.is_temp() && info.temp.regClass() == instr->operands[i].getTemp().regClass()) {
         instr->operands[i].setTemp(ctx.info[instr->operands[i].tempId()].temp);
         info = ctx.info[info.temp.id()];
      }

      /* PSEUDO: propagate temporaries/constants */
      if (instr->isPseudo()) {
         while (info.is_temp()) {
            pseudo_propagate_temp(ctx, instr, info.temp, i);
            info = ctx.info[info.temp.id()];
         }
         unsigned bits = instr->operands[i].bytes() * 8u;
         if (info.is_constant_or_literal(bits) && pseudo_can_accept_constant(instr, i)) {
            instr->operands[i] = get_constant_op(ctx, info, bits);
            continue;
         }
      }
      /* MUBUF: propagate constants and combine additions */
      else if (instr->isMUBUF()) {
         MUBUF_instruction& mubuf = instr->mubuf();
         Temp base;
         uint32_t offset;
         while (info.is_temp())
            info = ctx.info[info.temp.id()];

         bool swizzled = ctx.program->gfx_level >= GFX12 ? mubuf.cache.gfx12.swizzled
                                                         : (mubuf.cache.value & ac_swizzled);
         /* According to AMDGPUDAGToDAGISel::SelectMUBUFScratchOffen(), vaddr
          * overflow for scratch accesses works only on GFX9+ and saddr overflow
          * never works. Since swizzling is the only thing that separates
          * scratch accesses and other accesses and swizzling changing how
          * addressing works significantly, this probably applies to swizzled
          * MUBUF accesses. */
         bool vaddr_prevent_overflow = swizzled && ctx.program->gfx_level < GFX9;

         uint32_t const_max = ctx.program->dev.buf_offset_max;

         if (mubuf.offen && mubuf.idxen && i == 1 &&
             info.parent_instr->opcode == aco_opcode::p_create_vector &&
             info.parent_instr->operands.size() == 2 && info.parent_instr->operands[0].isTemp() &&
             info.parent_instr->operands[0].regClass() == v1 &&
             info.parent_instr->operands[1].isConstant() &&
             mubuf.offset + info.parent_instr->operands[1].constantValue() <= const_max) {
            instr->operands[1] = info.parent_instr->operands[0];
            mubuf.offset += info.parent_instr->operands[1].constantValue();
            mubuf.offen = false;
            continue;
         } else if (mubuf.offen && i == 1 && info.is_constant_or_literal(32) &&
                    mubuf.offset + info.val <= const_max) {
            assert(!mubuf.idxen);
            instr->operands[1] = Operand(v1);
            mubuf.offset += info.val;
            mubuf.offen = false;
            continue;
         } else if (i == 2 && info.is_constant_or_literal(32) &&
                    mubuf.offset + info.val <= const_max) {
            instr->operands[2] = Operand::c32(0);
            mubuf.offset += info.val;
            continue;
         } else if (mubuf.offen && i == 1 &&
                    parse_base_offset(ctx, instr.get(), i, &base, &offset,
                                      vaddr_prevent_overflow) &&
                    base.regClass() == v1 && mubuf.offset + offset <= const_max) {
            assert(!mubuf.idxen);
            instr->operands[1].setTemp(base);
            mubuf.offset += offset;
            continue;
         } else if (i == 2 && parse_base_offset(ctx, instr.get(), i, &base, &offset, true) &&
                    base.regClass() == s1 && mubuf.offset + offset <= const_max && !swizzled) {
            instr->operands[i].setTemp(base);
            mubuf.offset += offset;
            continue;
         }
      }

      else if (instr->isMTBUF()) {
         MTBUF_instruction& mtbuf = instr->mtbuf();
         while (info.is_temp())
            info = ctx.info[info.temp.id()];

         if (mtbuf.offen && mtbuf.idxen && i == 1 &&
             info.parent_instr->opcode == aco_opcode::p_create_vector &&
             info.parent_instr->operands.size() == 2 && info.parent_instr->operands[0].isTemp() &&
             info.parent_instr->operands[0].regClass() == v1 &&
             info.parent_instr->operands[1].isConstant() &&
             mtbuf.offset + info.parent_instr->operands[1].constantValue() <=
                ctx.program->dev.buf_offset_max) {
            instr->operands[1] = info.parent_instr->operands[0];
            mtbuf.offset += info.parent_instr->operands[1].constantValue();
            mtbuf.offen = false;
            continue;
         }
      }

      /* SCRATCH: propagate constants and combine additions */
      else if (instr->isScratch()) {
         FLAT_instruction& scratch = instr->scratch();
         Temp base;
         uint32_t offset;
         while (info.is_temp())
            info = ctx.info[info.temp.id()];

         /* The hardware probably does: 'scratch_base + u2u64(saddr) + i2i64(offset)'. This means
          * we can't combine the addition if the unsigned addition overflows and offset is
          * positive. In theory, there is also issues if
          * 'ilt(offset, 0) && ige(saddr, 0) && ilt(saddr + offset, 0)', but that just
          * replaces an already out-of-bounds access with a larger one since 'saddr + offset'
          * would be larger than INT32_MAX.
          */
         if (i <= 1 && parse_base_offset(ctx, instr.get(), i, &base, &offset, true) &&
             base.regClass() == instr->operands[i].regClass() &&
             is_scratch_offset_valid(ctx, instr.get(), scratch.offset, (int32_t)offset)) {
            instr->operands[i].setTemp(base);
            scratch.offset += (int32_t)offset;
            continue;
         } else if (i <= 1 && parse_base_offset(ctx, instr.get(), i, &base, &offset, false) &&
                    base.regClass() == instr->operands[i].regClass() && (int32_t)offset < 0 &&
                    is_scratch_offset_valid(ctx, instr.get(), scratch.offset, (int32_t)offset)) {
            instr->operands[i].setTemp(base);
            scratch.offset += (int32_t)offset;
            continue;
         } else if (i <= 1 && info.is_constant_or_literal(32) &&
                    ctx.program->gfx_level >= GFX10_3 &&
                    is_scratch_offset_valid(ctx, NULL, scratch.offset, (int32_t)info.val)) {
            /* GFX10.3+ can disable both SADDR and ADDR. */
            instr->operands[i] = Operand(instr->operands[i].regClass());
            scratch.offset += (int32_t)info.val;
            continue;
         }
      }

      else if (instr->isBranch()) {
         if (ctx.info[instr->operands[0].tempId()].is_scc_invert()) {
            /* Flip the branch instruction to get rid of the scc_invert instruction */
            instr->opcode = instr->opcode == aco_opcode::p_cbranch_z ? aco_opcode::p_cbranch_nz
                                                                     : aco_opcode::p_cbranch_z;
            instr->operands[0].setTemp(ctx.info[instr->operands[0].tempId()].temp);
         }
      }
   }

   /* SALU / VALU: propagate inline constants, temps, and imod */
   if (instr->isSALU() || instr->isVALU()) {
      alu_propagate_temp_const(ctx, instr);
   }

   /* if this instruction doesn't define anything, return */
   if (instr->definitions.empty()) {
      check_sdwa_extract(ctx, instr);
      return;
   }

   if (instr->isVALU() || (instr->isVINTRP() && instr->opcode != aco_opcode::v_interp_mov_f32)) {
      if (instr_info.alu_opcode_infos[(int)instr->opcode].output_modifiers || instr->isVINTRP() ||
          instr->opcode == aco_opcode::v_cndmask_b32) {
         bool canonicalized = true;
         if (!does_fp_op_flush_denorms(ctx, instr->opcode)) {
            unsigned ops = instr->opcode == aco_opcode::v_cndmask_b32 ? 2 : instr->operands.size();
            for (unsigned i = 0; canonicalized && (i < ops); i++)
               canonicalized = is_op_canonicalized(ctx, instr->operands[i]);
         }
         if (canonicalized)
            ctx.info[instr->definitions[0].tempId()].set_canonicalized();
      }
   }

   switch (instr->opcode) {
   case aco_opcode::p_create_vector: {
      bool copy_prop = instr->operands.size() == 1 && instr->operands[0].isTemp() &&
                       instr->operands[0].regClass() == instr->definitions[0].regClass();
      if (copy_prop) {
         ctx.info[instr->definitions[0].tempId()].set_temp(instr->operands[0].getTemp());
         break;
      }

      /* expand vector operands */
      std::vector<Operand> ops;
      unsigned offset = 0;
      for (const Operand& op : instr->operands) {
         /* ensure that any expanded operands are properly aligned */
         bool aligned = offset % 4 == 0 || op.bytes() < 4;
         offset += op.bytes();
         if (aligned && op.isTemp() &&
             ctx.info[op.tempId()].parent_instr->opcode == aco_opcode::p_create_vector) {
            Instruction* vec = ctx.info[op.tempId()].parent_instr;
            for (const Operand& vec_op : vec->operands)
               ops.emplace_back(vec_op);
         } else {
            ops.emplace_back(op);
         }
      }

      offset = 0;
      for (unsigned i = 0; i < ops.size(); i++) {
         if (ops[i].isTemp()) {
            if (ctx.info[ops[i].tempId()].is_temp() &&
                ops[i].regClass() == ctx.info[ops[i].tempId()].temp.regClass()) {
               ops[i].setTemp(ctx.info[ops[i].tempId()].temp);
            }

            /* If this and the following operands make up all definitions of a `p_split_vector`,
             * replace them with the operand of the `p_split_vector` instruction.
             */
            Instruction* parent = ctx.info[ops[i].tempId()].parent_instr;
            if (parent->opcode == aco_opcode::p_split_vector &&
                (offset % 4 == 0 || parent->operands[0].bytes() < 4) &&
                parent->definitions.size() <= ops.size() - i) {
               copy_prop = true;
               for (unsigned j = 0; copy_prop && j < parent->definitions.size(); j++) {
                  copy_prop &= ops[i + j].isTemp() &&
                               ops[i + j].getTemp() == parent->definitions[j].getTemp();
               }

               if (copy_prop) {
                  ops.erase(ops.begin() + i + 1, ops.begin() + i + parent->definitions.size());
                  ops[i] = parent->operands[0];
               }
            }
         }

         offset += ops[i].bytes();
      }

      /* combine expanded operands to new vector */
      if (ops.size() <= instr->operands.size()) {
         while (instr->operands.size() > ops.size())
            instr->operands.pop_back();

         if (ops.size() == 1 && !ops[0].isUndefined()) {
            instr->opcode = aco_opcode::p_parallelcopy;
            if (ops[0].isTemp())
               ctx.info[instr->definitions[0].tempId()].set_temp(ops[0].getTemp());
         }
      } else {
         Definition def = instr->definitions[0];
         instr.reset(
            create_instruction(aco_opcode::p_create_vector, Format::PSEUDO, ops.size(), 1));
         instr->definitions[0] = def;
      }

      for (unsigned i = 0; i < ops.size(); i++)
         instr->operands[i] = ops[i];
      break;
   }
   case aco_opcode::p_split_vector: {
      ssa_info& info = ctx.info[instr->operands[0].tempId()];

      if (info.is_constant_or_literal(32)) {
         uint64_t val = info.val;
         for (Definition def : instr->definitions) {
            uint32_t mask = u_bit_consecutive(0, def.bytes() * 8u);
            ctx.info[def.tempId()].set_constant(ctx.program->gfx_level, val & mask);
            val >>= def.bytes() * 8u;
         }
         break;
      } else if (info.parent_instr->opcode != aco_opcode::p_create_vector) {
         if (instr->definitions.size() == 2 && instr->operands[0].isTemp() &&
             instr->definitions[0].bytes() == instr->definitions[1].bytes()) {
            if (instr->operands[0].bytes() == 4) {
               /* D16 subdword split */
               ctx.info[instr->definitions[0].tempId()].set_temp(instr->operands[0].getTemp());
               ctx.info[instr->definitions[1].tempId()].set_extract();
            }
         }
         break;
      }

      Instruction* vec = info.parent_instr;
      unsigned split_offset = 0;
      unsigned vec_offset = 0;
      unsigned vec_index = 0;
      for (unsigned i = 0; i < instr->definitions.size();
           split_offset += instr->definitions[i++].bytes()) {
         while (vec_offset < split_offset && vec_index < vec->operands.size())
            vec_offset += vec->operands[vec_index++].bytes();

         if (vec_offset != split_offset ||
             vec->operands[vec_index].bytes() != instr->definitions[i].bytes())
            continue;

         Operand vec_op = vec->operands[vec_index];
         if (vec_op.isConstant()) {
            ctx.info[instr->definitions[i].tempId()].set_constant(ctx.program->gfx_level,
                                                                  vec_op.constantValue64());
         } else if (vec_op.isTemp()) {
            ctx.info[instr->definitions[i].tempId()].set_temp(vec_op.getTemp());
         }
      }
      break;
   }
   case aco_opcode::p_extract_vector: { /* mov */
      const unsigned index = instr->operands[1].constantValue();

      if (instr->operands[0].isTemp()) {
         ssa_info& info = ctx.info[instr->operands[0].tempId()];
         const unsigned dst_offset = index * instr->definitions[0].bytes();

         if (info.parent_instr->opcode == aco_opcode::p_create_vector) {
            /* check if we index directly into a vector element */
            Instruction* vec = info.parent_instr;
            unsigned offset = 0;

            for (const Operand& op : vec->operands) {
               if (offset < dst_offset) {
                  offset += op.bytes();
                  continue;
               } else if (offset != dst_offset || op.bytes() != instr->definitions[0].bytes()) {
                  break;
               }
               instr->operands[0] = op;
               break;
            }
         } else if (info.is_constant_or_literal(32)) {
            /* propagate constants */
            uint32_t mask = u_bit_consecutive(0, instr->definitions[0].bytes() * 8u);
            uint32_t val = (info.val >> (dst_offset * 8u)) & mask;
            instr->operands[0] =
               Operand::get_const(ctx.program->gfx_level, val, instr->definitions[0].bytes());
            ;
         }
      }

      if (instr->operands[0].bytes() != instr->definitions[0].bytes()) {
         if (instr->operands[0].size() != 1 || !instr->operands[0].isTemp())
            break;

         if (index == 0)
            ctx.info[instr->definitions[0].tempId()].set_temp(instr->operands[0].getTemp());
         else
            ctx.info[instr->definitions[0].tempId()].set_extract();
         break;
      }

      /* convert this extract into a copy instruction */
      instr->opcode = aco_opcode::p_parallelcopy;
      instr->operands.pop_back();
      FALLTHROUGH;
   }
   case aco_opcode::p_parallelcopy: /* propagate */
      if (instr->operands[0].isTemp() &&
          ctx.info[instr->operands[0].tempId()].parent_instr->opcode ==
             aco_opcode::p_create_vector &&
          instr->operands[0].regClass() != instr->definitions[0].regClass()) {
         /* We might not be able to copy-propagate if it's a SGPR->VGPR copy, so
          * duplicate the vector instead.
          */
         Instruction* vec = ctx.info[instr->operands[0].tempId()].parent_instr;
         aco_ptr<Instruction> old_copy = std::move(instr);

         instr.reset(create_instruction(aco_opcode::p_create_vector, Format::PSEUDO,
                                        vec->operands.size(), 1));
         instr->definitions[0] = old_copy->definitions[0];
         std::copy(vec->operands.begin(), vec->operands.end(), instr->operands.begin());
         for (unsigned i = 0; i < vec->operands.size(); i++) {
            Operand& op = instr->operands[i];
            if (op.isTemp() && ctx.info[op.tempId()].is_temp() &&
                ctx.info[op.tempId()].temp.type() == instr->definitions[0].regClass().type())
               op.setTemp(ctx.info[op.tempId()].temp);
         }
         break;
      }
      FALLTHROUGH;
   case aco_opcode::p_as_uniform:
      if (instr->definitions[0].isFixed()) {
         /* don't copy-propagate copies into fixed registers */
      } else if (instr->operands[0].isConstant()) {
         ctx.info[instr->definitions[0].tempId()].set_constant(
            ctx.program->gfx_level, instr->operands[0].constantValue64());
      } else if (instr->operands[0].isTemp()) {
         ctx.info[instr->definitions[0].tempId()].set_temp(instr->operands[0].getTemp());
         if (ctx.info[instr->operands[0].tempId()].is_canonicalized())
            ctx.info[instr->definitions[0].tempId()].set_canonicalized();
      } else {
         assert(instr->operands[0].isFixed());
      }
      break;
   case aco_opcode::p_is_helper:
      if (!ctx.program->needs_wqm)
         ctx.info[instr->definitions[0].tempId()].set_constant(ctx.program->gfx_level, 0u);
      break;
   case aco_opcode::v_mul_f16:
   case aco_opcode::v_mul_f32:
   case aco_opcode::v_mul_legacy_f32: { /* omod */
      /* TODO: try to move the negate/abs modifier to the consumer instead */
      bool uses_mods = instr->usesModifiers();
      bool fp16 = instr->opcode == aco_opcode::v_mul_f16;
      unsigned denorm_mode = fp16 ? ctx.fp_mode.denorm16_64 : ctx.fp_mode.denorm32;

      for (unsigned i = 0; i < 2; i++) {
         if (instr->operands[!i].isConstant() && instr->operands[i].isTemp()) {
            if (!instr->isDPP() && !instr->isSDWA() && !instr->valu().opsel &&
                (instr->operands[!i].constantEquals(fp16 ? 0x3c00 : 0x3f800000) ||   /* 1.0 */
                 instr->operands[!i].constantEquals(fp16 ? 0xbc00 : 0xbf800000u))) { /* -1.0 */
               bool neg1 = instr->operands[!i].constantEquals(fp16 ? 0xbc00 : 0xbf800000u);

               VALU_instruction* valu = &instr->valu();
               if (valu->abs[!i] || valu->neg[!i] || valu->omod)
                  continue;

               bool abs = valu->abs[i];
               bool neg = neg1 ^ valu->neg[i];
               Temp other = instr->operands[i].getTemp();

               if (valu->clamp) {
                  if (!abs && !neg && other.type() == RegType::vgpr)
                     ctx.info[other.id()].set_clamp(instr.get());
                  continue;
               }

               if (abs && neg && other.type() == RegType::vgpr)
                  ctx.info[instr->definitions[0].tempId()].set_neg_abs(other);
               else if (abs && !neg && other.type() == RegType::vgpr)
                  ctx.info[instr->definitions[0].tempId()].set_abs(other);
               else if (!abs && neg && other.type() == RegType::vgpr)
                  ctx.info[instr->definitions[0].tempId()].set_neg(other);
               else if (!abs && !neg) {
                  if (denorm_mode == fp_denorm_keep || ctx.info[other.id()].is_canonicalized())
                     ctx.info[instr->definitions[0].tempId()].set_temp(other);
                  else
                     ctx.info[instr->definitions[0].tempId()].set_fcanonicalize(other);
               }
            } else if (uses_mods || (instr->definitions[0].isSZPreserve() &&
                                     instr->opcode != aco_opcode::v_mul_legacy_f32)) {
               continue; /* omod uses a legacy multiplication. */
            } else if (instr->operands[!i].constantValue() == 0u &&
                       ((!instr->definitions[0].isNaNPreserve() &&
                         !instr->definitions[0].isInfPreserve()) ||
                        instr->opcode == aco_opcode::v_mul_legacy_f32)) { /* 0.0 */
               ctx.info[instr->definitions[0].tempId()].set_constant(ctx.program->gfx_level, 0u);
            } else if (denorm_mode != fp_denorm_flush) {
               /* omod has no effect if denormals are enabled. */
               continue;
            } else if (instr->operands[!i].constantValue() ==
                       (fp16 ? 0x4000 : 0x40000000)) { /* 2.0 */
               ctx.info[instr->operands[i].tempId()].set_omod2(instr.get());
            } else if (instr->operands[!i].constantValue() ==
                       (fp16 ? 0x4400 : 0x40800000)) { /* 4.0 */
               ctx.info[instr->operands[i].tempId()].set_omod4(instr.get());
            } else if (instr->operands[!i].constantValue() ==
                       (fp16 ? 0x3800 : 0x3f000000)) { /* 0.5 */
               ctx.info[instr->operands[i].tempId()].set_omod5(instr.get());
            } else {
               continue;
            }
            break;
         }
      }
      break;
   }
   case aco_opcode::v_med3_f16:
   case aco_opcode::v_med3_f32: { /* clamp */
      unsigned idx;
      if (detect_clamp(instr.get(), &idx) && !instr->valu().abs && !instr->valu().neg)
         ctx.info[instr->operands[idx].tempId()].set_clamp(instr.get());
      break;
   }
   case aco_opcode::v_cndmask_b32:
      if (instr->operands[0].constantEquals(0) && instr->operands[1].constantEquals(0x3f800000u))
         ctx.info[instr->definitions[0].tempId()].set_b2f(instr->operands[2].getTemp());
      else if (instr->operands[0].constantEquals(0) && instr->operands[1].constantEquals(1))
         ctx.info[instr->definitions[0].tempId()].set_b2i(instr->operands[2].getTemp());

      break;
   case aco_opcode::s_not_b32:
   case aco_opcode::s_not_b64:
      if (!instr->operands[0].isTemp()) {
      } else if (ctx.info[instr->operands[0].tempId()].is_uniform_bool()) {
         ctx.info[instr->definitions[0].tempId()].set_uniform_bitwise();
         ctx.info[instr->definitions[1].tempId()].set_scc_invert(
            ctx.info[instr->operands[0].tempId()].temp);
      } else if (ctx.info[instr->operands[0].tempId()].is_uniform_bitwise()) {
         ctx.info[instr->definitions[0].tempId()].set_uniform_bitwise();
         ctx.info[instr->definitions[1].tempId()].set_scc_invert(
            ctx.info[instr->operands[0].tempId()].parent_instr->definitions[1].getTemp());
      }
      break;
   case aco_opcode::s_and_b32:
   case aco_opcode::s_and_b64:
      if (fixed_to_exec(instr->operands[1]) && instr->operands[0].isTemp()) {
         if (ctx.info[instr->operands[0].tempId()].is_uniform_bool()) {
            /* Try to get rid of the superfluous s_cselect + s_and_b64 that comes from turning a
             * uniform bool into divergent */
            ctx.info[instr->definitions[1].tempId()].set_temp(
               ctx.info[instr->operands[0].tempId()].temp);
            break;
         } else if (ctx.info[instr->operands[0].tempId()].is_uniform_bitwise()) {
            /* Try to get rid of the superfluous s_and_b64, since the uniform bitwise instruction
             * already produces the same SCC */
            ctx.info[instr->definitions[1].tempId()].set_temp(
               ctx.info[instr->operands[0].tempId()].parent_instr->definitions[1].getTemp());
            break;
         } else if ((ctx.program->stage.num_sw_stages() > 1 ||
                     ctx.program->stage.hw == AC_HW_NEXT_GEN_GEOMETRY_SHADER) &&
                    instr->pass_flags == 1) {
            /* In case of merged shaders, pass_flags=1 means that all lanes are active (exec=-1), so
             * s_and is unnecessary. */
            ctx.info[instr->definitions[0].tempId()].set_temp(instr->operands[0].getTemp());
            break;
         }
      }
      FALLTHROUGH;
   case aco_opcode::s_or_b32:
   case aco_opcode::s_or_b64:
   case aco_opcode::s_xor_b32:
   case aco_opcode::s_xor_b64:
      if (std::all_of(instr->operands.begin(), instr->operands.end(),
                      [&ctx](const Operand& op)
                      {
                         return op.isTemp() && (ctx.info[op.tempId()].is_uniform_bool() ||
                                                ctx.info[op.tempId()].is_uniform_bitwise());
                      })) {
         ctx.info[instr->definitions[0].tempId()].set_uniform_bitwise();
      }
      break;
   case aco_opcode::s_cselect_b64:
   case aco_opcode::s_cselect_b32:
      if (instr->operands[0].constantEquals((unsigned)-1) && instr->operands[1].constantEquals(0)) {
         /* Found a cselect that operates on a uniform bool that comes from eg. s_cmp */
         ctx.info[instr->definitions[0].tempId()].set_uniform_bool(instr->operands[2].getTemp());
      } else if (instr->operands[2].isTemp() && ctx.info[instr->operands[2].tempId()].is_scc_invert()) {
         /* Flip the operands to get rid of the scc_invert instruction */
         std::swap(instr->operands[0], instr->operands[1]);
         instr->operands[2].setTemp(ctx.info[instr->operands[2].tempId()].temp);
      }
      break;
   case aco_opcode::s_mul_i32:
      /* Testing every uint32_t shows that 0x3f800000*n is never a denormal.
       * This pattern is created from a uniform nir_op_b2f. */
      if (instr->operands[0].constantEquals(0x3f800000u))
         ctx.info[instr->definitions[0].tempId()].set_canonicalized();
      break;
   case aco_opcode::p_extract: {
      if (instr->operands[0].isTemp()) {
         ctx.info[instr->definitions[0].tempId()].set_extract();
         if (instr->definitions[0].bytes() == 4 && instr->operands[0].regClass() == v1 &&
             parse_insert(instr.get()))
            ctx.info[instr->operands[0].tempId()].set_insert(instr.get());
      }
      break;
   }
   case aco_opcode::p_insert: {
      if (instr->operands[0].isTemp()) {
         if (instr->operands[0].regClass() == v1)
            ctx.info[instr->operands[0].tempId()].set_insert(instr.get());
         if (parse_extract(instr.get()))
            ctx.info[instr->definitions[0].tempId()].set_extract();
      }
      break;
   }
   case aco_opcode::v_cvt_f16_f32: {
      if (instr->operands[0].isTemp())
         ctx.info[instr->operands[0].tempId()].set_f2f16(instr.get());
      break;
   }
   default: break;
   }

   /* Don't remove label_extract if we can't apply the extract to
    * neg/abs instructions because we'll likely combine it into another valu. */
   if (!(ctx.info[instr->definitions[0].tempId()].label & (label_neg | label_abs)))
      check_sdwa_extract(ctx, instr);

   /* Set parent_instr for all SSA definitions. */
   for (const Definition& def : instr->definitions)
      ctx.info[def.tempId()].parent_instr = instr.get();
}

unsigned
original_temp_id(opt_ctx& ctx, Temp tmp)
{
   if (ctx.info[tmp.id()].is_temp())
      return ctx.info[tmp.id()].temp.id();
   else
      return tmp.id();
}

Operand
copy_operand(opt_ctx& ctx, Operand op)
{
   if (op.isTemp())
      ctx.uses[op.tempId()]++;
   return op;
}

Instruction*
follow_operand(opt_ctx& ctx, Operand op, bool ignore_uses = false)
{
   if (!op.isTemp())
      return nullptr;
   if (!ignore_uses && ctx.uses[op.tempId()] > 1)
      return nullptr;

   Instruction* instr = ctx.info[op.tempId()].parent_instr;

   if (instr->definitions[0].getTemp() != op.getTemp())
      return nullptr;

   if (instr->definitions.size() == 2) {
      unsigned idx =
         instr->definitions[1].isTemp() && instr->definitions[1].tempId() == op.tempId();
      assert(instr->definitions[idx].isTemp() && instr->definitions[idx].tempId() == op.tempId());
      if (instr->definitions[!idx].isTemp() && ctx.uses[instr->definitions[!idx].tempId()])
         return nullptr;
   }

   for (Operand& operand : instr->operands) {
      if (fixed_to_exec(operand))
         return nullptr;
   }

   return instr;
}

bool
is_operand_constant(opt_ctx& ctx, Operand op, unsigned bit_size, uint64_t* value)
{
   if (op.isConstant()) {
      *value = op.constantValue64();
      return true;
   } else if (op.isTemp()) {
      unsigned id = original_temp_id(ctx, op.getTemp());
      if (!ctx.info[id].is_constant_or_literal(bit_size))
         return false;
      *value = get_constant_op(ctx, ctx.info[id], bit_size).constantValue64();
      return true;
   }
   return false;
}

/* s_not(cmp(a, b)) -> get_vcmp_inverse(cmp)(a, b) */
bool
combine_inverse_comparison(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (ctx.uses[instr->definitions[1].tempId()])
      return false;
   if (!instr->operands[0].isTemp() || ctx.uses[instr->operands[0].tempId()] != 1)
      return false;

   Instruction* cmp = follow_operand(ctx, instr->operands[0]);
   if (!cmp)
      return false;

   aco_opcode new_opcode = get_vcmp_inverse(cmp->opcode);
   if (new_opcode == aco_opcode::num_opcodes)
      return false;

   /* Invert compare instruction and assign this instruction's definition */
   cmp->opcode = new_opcode;
   ctx.info[instr->definitions[0].tempId()] = ctx.info[cmp->definitions[0].tempId()];
   std::swap(instr->definitions[0], cmp->definitions[0]);
   ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
   ctx.info[cmp->definitions[0].tempId()].parent_instr = cmp;

   ctx.uses[instr->operands[0].tempId()]--;
   return true;
}

/* op1(op2(1, 2), 0) if swap = false
 * op1(0, op2(1, 2)) if swap = true */
bool
match_op3_for_vop3(opt_ctx& ctx, aco_opcode op1, aco_opcode op2, Instruction* op1_instr, bool swap,
                   const char* shuffle_str, Operand operands[3], bitarray8& neg, bitarray8& abs,
                   bitarray8& opsel, bool* op1_clamp, uint8_t* op1_omod, bool* inbetween_neg,
                   bool* inbetween_abs, bool* inbetween_opsel, bool* precise)
{
   /* checks */
   if (op1_instr->opcode != op1)
      return false;

   Instruction* op2_instr = follow_operand(ctx, op1_instr->operands[swap]);
   if (!op2_instr || op2_instr->opcode != op2)
      return false;

   VALU_instruction* op1_valu = op1_instr->isVALU() ? &op1_instr->valu() : NULL;
   VALU_instruction* op2_valu = op2_instr->isVALU() ? &op2_instr->valu() : NULL;

   if (op1_instr->isSDWA() || op2_instr->isSDWA())
      return false;
   if (op1_instr->isDPP() || op2_instr->isDPP())
      return false;

   /* don't support inbetween clamp/omod */
   if (op2_valu && (op2_valu->clamp || op2_valu->omod))
      return false;

   /* get operands and modifiers and check inbetween modifiers */
   *op1_clamp = op1_valu ? (bool)op1_valu->clamp : false;
   *op1_omod = op1_valu ? (unsigned)op1_valu->omod : 0u;

   if (inbetween_neg)
      *inbetween_neg = op1_valu ? op1_valu->neg[swap] : false;
   else if (op1_valu && op1_valu->neg[swap])
      return false;

   if (inbetween_abs)
      *inbetween_abs = op1_valu ? op1_valu->abs[swap] : false;
   else if (op1_valu && op1_valu->abs[swap])
      return false;

   if (inbetween_opsel)
      *inbetween_opsel = op1_valu ? op1_valu->opsel[swap] : false;
   else if (op1_valu && op1_valu->opsel[swap])
      return false;

   *precise = op1_instr->definitions[0].isPrecise() || op2_instr->definitions[0].isPrecise();

   int shuffle[3];
   shuffle[shuffle_str[0] - '0'] = 0;
   shuffle[shuffle_str[1] - '0'] = 1;
   shuffle[shuffle_str[2] - '0'] = 2;

   operands[shuffle[0]] = op1_instr->operands[!swap];
   neg[shuffle[0]] = op1_valu ? op1_valu->neg[!swap] : false;
   abs[shuffle[0]] = op1_valu ? op1_valu->abs[!swap] : false;
   opsel[shuffle[0]] = op1_valu ? op1_valu->opsel[!swap] : false;

   for (unsigned i = 0; i < 2; i++) {
      operands[shuffle[i + 1]] = op2_instr->operands[i];
      neg[shuffle[i + 1]] = op2_valu ? op2_valu->neg[i] : false;
      abs[shuffle[i + 1]] = op2_valu ? op2_valu->abs[i] : false;
      opsel[shuffle[i + 1]] = op2_valu ? op2_valu->opsel[i] : false;
   }

   /* check operands */
   if (!check_vop3_operands(ctx, 3, operands))
      return false;

   return true;
}

void
create_vop3_for_op3(opt_ctx& ctx, aco_opcode opcode, aco_ptr<Instruction>& instr,
                    Operand operands[3], uint8_t neg, uint8_t abs, uint8_t opsel, bool clamp,
                    unsigned omod)
{
   Instruction* new_instr = create_instruction(opcode, Format::VOP3, 3, 1);
   new_instr->valu().neg = neg;
   new_instr->valu().abs = abs;
   new_instr->valu().clamp = clamp;
   new_instr->valu().omod = omod;
   new_instr->valu().opsel = opsel;
   new_instr->operands[0] = operands[0];
   new_instr->operands[1] = operands[1];
   new_instr->operands[2] = operands[2];
   new_instr->definitions[0] = instr->definitions[0];
   new_instr->pass_flags = instr->pass_flags;
   ctx.info[instr->definitions[0].tempId()].label = 0;
   ctx.info[instr->definitions[0].tempId()].parent_instr = new_instr;

   instr.reset(new_instr);
}

bool
combine_three_valu_op(opt_ctx& ctx, aco_ptr<Instruction>& instr, aco_opcode op2, aco_opcode new_op,
                      const char* shuffle, uint8_t ops)
{
   for (unsigned swap = 0; swap < 2; swap++) {
      if (!((1 << swap) & ops))
         continue;

      Operand operands[3];
      bool clamp, precise;
      bitarray8 neg = 0, abs = 0, opsel = 0;
      uint8_t omod = 0;
      if (match_op3_for_vop3(ctx, instr->opcode, op2, instr.get(), swap, shuffle, operands, neg,
                             abs, opsel, &clamp, &omod, NULL, NULL, NULL, &precise)) {
         ctx.uses[instr->operands[swap].tempId()]--;
         create_vop3_for_op3(ctx, new_op, instr, operands, neg, abs, opsel, clamp, omod);
         return true;
      }
   }
   return false;
}

/* creates v_lshl_add_u32, v_lshl_or_b32 or v_and_or_b32 */
bool
combine_add_or_then_and_lshl(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   bool is_or = instr->opcode == aco_opcode::v_or_b32;
   aco_opcode new_op_lshl = is_or ? aco_opcode::v_lshl_or_b32 : aco_opcode::v_lshl_add_u32;

   if (is_or && combine_three_valu_op(ctx, instr, aco_opcode::s_and_b32, aco_opcode::v_and_or_b32,
                                      "120", 1 | 2))
      return true;
   if (is_or && combine_three_valu_op(ctx, instr, aco_opcode::v_and_b32, aco_opcode::v_and_or_b32,
                                      "120", 1 | 2))
      return true;
   if (combine_three_valu_op(ctx, instr, aco_opcode::s_lshl_b32, new_op_lshl, "120", 1 | 2))
      return true;
   if (combine_three_valu_op(ctx, instr, aco_opcode::v_lshlrev_b32, new_op_lshl, "210", 1 | 2))
      return true;

   if (instr->isSDWA() || instr->isDPP())
      return false;

   /* v_or_b32(p_extract(a, 0, 8/16, 0), b) -> v_and_or_b32(a, 0xff/0xffff, b)
    * v_or_b32(p_insert(a, 0, 8/16), b) -> v_and_or_b32(a, 0xff/0xffff, b)
    * v_or_b32(p_insert(a, 24/16, 8/16), b) -> v_lshl_or_b32(a, 24/16, b)
    * v_add_u32(p_insert(a, 24/16, 8/16), b) -> v_lshl_add_b32(a, 24/16, b)
    */
   for (unsigned i = 0; i < 2; i++) {
      Instruction* extins = follow_operand(ctx, instr->operands[i]);
      if (!extins)
         continue;

      aco_opcode op;
      Operand operands[3];

      if (extins->opcode == aco_opcode::p_insert &&
          (extins->operands[1].constantValue() + 1) * extins->operands[2].constantValue() == 32) {
         op = new_op_lshl;
         operands[1] =
            Operand::c32(extins->operands[1].constantValue() * extins->operands[2].constantValue());
      } else if (is_or &&
                 (extins->opcode == aco_opcode::p_insert ||
                  (extins->opcode == aco_opcode::p_extract &&
                   extins->operands[3].constantEquals(0))) &&
                 extins->operands[1].constantEquals(0)) {
         op = aco_opcode::v_and_or_b32;
         operands[1] = Operand::c32(extins->operands[2].constantEquals(8) ? 0xffu : 0xffffu);
      } else {
         continue;
      }

      operands[0] = extins->operands[0];
      operands[2] = instr->operands[!i];

      if (!check_vop3_operands(ctx, 3, operands))
         continue;

      uint8_t neg = 0, abs = 0, opsel = 0, omod = 0;
      bool clamp = false;
      if (instr->isVOP3())
         clamp = instr->valu().clamp;

      ctx.uses[instr->operands[i].tempId()]--;
      create_vop3_for_op3(ctx, op, instr, operands, neg, abs, opsel, clamp, omod);
      return true;
   }

   return false;
}

/* v_xor(a, s_not(b)) -> v_xnor(a, b)
 * v_xor(a, v_not(b)) -> v_xnor(a, b)
 */
bool
combine_xor_not(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (instr->usesModifiers())
      return false;

   for (unsigned i = 0; i < 2; i++) {
      Instruction* op_instr = follow_operand(ctx, instr->operands[i], true);
      if (!op_instr ||
          (op_instr->opcode != aco_opcode::v_not_b32 &&
           op_instr->opcode != aco_opcode::s_not_b32) ||
          op_instr->usesModifiers() || op_instr->operands[0].isLiteral())
         continue;

      instr->opcode = aco_opcode::v_xnor_b32;
      instr->operands[i] = copy_operand(ctx, op_instr->operands[0]);
      decrease_and_dce(ctx, op_instr->definitions[0].getTemp());
      if (instr->operands[0].isOfType(RegType::vgpr))
         std::swap(instr->operands[0], instr->operands[1]);
      if (!instr->operands[1].isOfType(RegType::vgpr))
         instr->format = asVOP3(instr->format);

      return true;
   }

   return false;
}

/* v_not(v_xor(a, b)) -> v_xnor(a, b) */
bool
combine_not_xor(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (instr->usesModifiers())
      return false;

   Instruction* op_instr = follow_operand(ctx, instr->operands[0]);
   if (!op_instr || op_instr->opcode != aco_opcode::v_xor_b32 || op_instr->isSDWA())
      return false;

   ctx.uses[instr->operands[0].tempId()]--;
   std::swap(instr->definitions[0], op_instr->definitions[0]);
   op_instr->opcode = aco_opcode::v_xnor_b32;
   ctx.info[op_instr->definitions[0].tempId()].label = 0;
   ctx.info[op_instr->definitions[0].tempId()].parent_instr = op_instr;
   ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();

   return true;
}

bool
combine_minmax(opt_ctx& ctx, aco_ptr<Instruction>& instr, aco_opcode opposite, aco_opcode op3src,
               aco_opcode minmax)
{
   /* TODO: this can handle SDWA min/max instructions by using opsel */

   /* min(min(a, b), c) -> min3(a, b, c)
    * max(max(a, b), c) -> max3(a, b, c)
    * gfx11: min(-min(a, b), c) -> maxmin(-a, -b, c)
    * gfx11: max(-max(a, b), c) -> minmax(-a, -b, c)
    */
   for (unsigned swap = 0; swap < 2; swap++) {
      Operand operands[3];
      bool clamp, precise;
      bitarray8 opsel = 0, neg = 0, abs = 0;
      uint8_t omod = 0;
      bool inbetween_neg;
      if (match_op3_for_vop3(ctx, instr->opcode, instr->opcode, instr.get(), swap, "120", operands,
                             neg, abs, opsel, &clamp, &omod, &inbetween_neg, NULL, NULL,
                             &precise) &&
          (!inbetween_neg ||
           (minmax != aco_opcode::num_opcodes && ctx.program->gfx_level >= GFX11))) {
         ctx.uses[instr->operands[swap].tempId()]--;
         if (inbetween_neg) {
            neg[0] = !neg[0];
            neg[1] = !neg[1];
            create_vop3_for_op3(ctx, minmax, instr, operands, neg, abs, opsel, clamp, omod);
         } else {
            create_vop3_for_op3(ctx, op3src, instr, operands, neg, abs, opsel, clamp, omod);
         }
         return true;
      }
   }

   /* min(-max(a, b), c) -> min3(-a, -b, c)
    * max(-min(a, b), c) -> max3(-a, -b, c)
    * gfx11: min(max(a, b), c) -> maxmin(a, b, c)
    * gfx11: max(min(a, b), c) -> minmax(a, b, c)
    */
   for (unsigned swap = 0; swap < 2; swap++) {
      Operand operands[3];
      bool clamp, precise;
      bitarray8 opsel = 0, neg = 0, abs = 0;
      uint8_t omod = 0;
      bool inbetween_neg;
      if (match_op3_for_vop3(ctx, instr->opcode, opposite, instr.get(), swap, "120", operands, neg,
                             abs, opsel, &clamp, &omod, &inbetween_neg, NULL, NULL, &precise) &&
          (inbetween_neg ||
           (minmax != aco_opcode::num_opcodes && ctx.program->gfx_level >= GFX11))) {
         ctx.uses[instr->operands[swap].tempId()]--;
         if (inbetween_neg) {
            neg[0] = !neg[0];
            neg[1] = !neg[1];
            create_vop3_for_op3(ctx, op3src, instr, operands, neg, abs, opsel, clamp, omod);
         } else {
            create_vop3_for_op3(ctx, minmax, instr, operands, neg, abs, opsel, clamp, omod);
         }
         return true;
      }
   }
   return false;
}

/* s_not_b32(s_and_b32(a, b)) -> s_nand_b32(a, b)
 * s_not_b32(s_or_b32(a, b)) -> s_nor_b32(a, b)
 * s_not_b32(s_xor_b32(a, b)) -> s_xnor_b32(a, b)
 * s_not_b64(s_and_b64(a, b)) -> s_nand_b64(a, b)
 * s_not_b64(s_or_b64(a, b)) -> s_nor_b64(a, b)
 * s_not_b64(s_xor_b64(a, b)) -> s_xnor_b64(a, b) */
bool
combine_salu_not_bitwise(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   /* checks */
   if (!instr->operands[0].isTemp())
      return false;
   if (instr->definitions[1].isTemp() && ctx.uses[instr->definitions[1].tempId()])
      return false;

   Instruction* op2_instr = follow_operand(ctx, instr->operands[0]);
   if (!op2_instr)
      return false;
   switch (op2_instr->opcode) {
   case aco_opcode::s_and_b32:
   case aco_opcode::s_or_b32:
   case aco_opcode::s_xor_b32:
   case aco_opcode::s_and_b64:
   case aco_opcode::s_or_b64:
   case aco_opcode::s_xor_b64: break;
   default: return false;
   }

   /* create instruction */
   std::swap(instr->definitions[0], op2_instr->definitions[0]);
   std::swap(instr->definitions[1], op2_instr->definitions[1]);
   ctx.uses[instr->operands[0].tempId()]--;
   ctx.info[op2_instr->definitions[0].tempId()].label = 0;
   ctx.info[op2_instr->definitions[0].tempId()].parent_instr = op2_instr;
   ctx.info[op2_instr->definitions[1].tempId()].parent_instr = op2_instr;
   ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
   ctx.info[instr->definitions[1].tempId()].parent_instr = instr.get();

   switch (op2_instr->opcode) {
   case aco_opcode::s_and_b32: op2_instr->opcode = aco_opcode::s_nand_b32; break;
   case aco_opcode::s_or_b32: op2_instr->opcode = aco_opcode::s_nor_b32; break;
   case aco_opcode::s_xor_b32: op2_instr->opcode = aco_opcode::s_xnor_b32; break;
   case aco_opcode::s_and_b64: op2_instr->opcode = aco_opcode::s_nand_b64; break;
   case aco_opcode::s_or_b64: op2_instr->opcode = aco_opcode::s_nor_b64; break;
   case aco_opcode::s_xor_b64: op2_instr->opcode = aco_opcode::s_xnor_b64; break;
   default: break;
   }

   return true;
}

/* s_and_b32(a, s_not_b32(b)) -> s_andn2_b32(a, b)
 * s_or_b32(a, s_not_b32(b)) -> s_orn2_b32(a, b)
 * s_and_b64(a, s_not_b64(b)) -> s_andn2_b64(a, b)
 * s_or_b64(a, s_not_b64(b)) -> s_orn2_b64(a, b) */
bool
combine_salu_n2(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (instr->definitions[0].isTemp() && ctx.info[instr->definitions[0].tempId()].is_uniform_bool())
      return false;

   for (unsigned i = 0; i < 2; i++) {
      Instruction* op2_instr = follow_operand(ctx, instr->operands[i]);
      if (!op2_instr || (op2_instr->opcode != aco_opcode::s_not_b32 &&
                         op2_instr->opcode != aco_opcode::s_not_b64))
         continue;
      if (ctx.uses[op2_instr->definitions[1].tempId()])
         continue;

      if (instr->operands[!i].isLiteral() && op2_instr->operands[0].isLiteral() &&
          instr->operands[!i].constantValue() != op2_instr->operands[0].constantValue())
         continue;

      ctx.uses[instr->operands[i].tempId()]--;
      instr->operands[0] = instr->operands[!i];
      instr->operands[1] = op2_instr->operands[0];
      ctx.info[instr->definitions[0].tempId()].label = 0;

      switch (instr->opcode) {
      case aco_opcode::s_and_b32: instr->opcode = aco_opcode::s_andn2_b32; break;
      case aco_opcode::s_or_b32: instr->opcode = aco_opcode::s_orn2_b32; break;
      case aco_opcode::s_and_b64: instr->opcode = aco_opcode::s_andn2_b64; break;
      case aco_opcode::s_or_b64: instr->opcode = aco_opcode::s_orn2_b64; break;
      default: break;
      }

      return true;
   }
   return false;
}

/* s_add_{i32,u32}(a, s_lshl_b32(b, <n>)) -> s_lshl<n>_add_u32(a, b) */
bool
combine_salu_lshl_add(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (instr->opcode == aco_opcode::s_add_i32 && ctx.uses[instr->definitions[1].tempId()])
      return false;

   for (unsigned i = 0; i < 2; i++) {
      Instruction* op2_instr = follow_operand(ctx, instr->operands[i], true);
      if (!op2_instr || op2_instr->opcode != aco_opcode::s_lshl_b32 ||
          ctx.uses[op2_instr->definitions[1].tempId()])
         continue;
      if (!op2_instr->operands[1].isConstant())
         continue;

      uint32_t shift = op2_instr->operands[1].constantValue();
      if (shift < 1 || shift > 4)
         continue;

      if (instr->operands[!i].isLiteral() && op2_instr->operands[0].isLiteral() &&
          instr->operands[!i].constantValue() != op2_instr->operands[0].constantValue())
         continue;

      instr->operands[1] = instr->operands[!i];
      instr->operands[0] = copy_operand(ctx, op2_instr->operands[0]);
      decrease_and_dce(ctx, op2_instr->definitions[0].getTemp());
      ctx.info[instr->definitions[0].tempId()].label = 0;

      instr->opcode = std::array<aco_opcode, 4>{
         aco_opcode::s_lshl1_add_u32, aco_opcode::s_lshl2_add_u32, aco_opcode::s_lshl3_add_u32,
         aco_opcode::s_lshl4_add_u32}[shift - 1];

      return true;
   }
   return false;
}

/* s_abs_i32(s_sub_[iu]32(a, b)) -> s_absdiff_i32(a, b)
 * s_abs_i32(s_add_[iu]32(a, #b)) -> s_absdiff_i32(a, -b)
 */
bool
combine_sabsdiff(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   Instruction* op_instr = follow_operand(ctx, instr->operands[0], false);
   if (!op_instr)
      return false;

   if (op_instr->opcode == aco_opcode::s_add_i32 || op_instr->opcode == aco_opcode::s_add_u32) {
      for (unsigned i = 0; i < 2; i++) {
         uint64_t constant;
         if (op_instr->operands[!i].isLiteral() ||
             !is_operand_constant(ctx, op_instr->operands[i], 32, &constant))
            continue;

         if (op_instr->operands[i].isTemp())
            ctx.uses[op_instr->operands[i].tempId()]--;
         op_instr->operands[0] = op_instr->operands[!i];
         op_instr->operands[1] = Operand::c32(-int32_t(constant));
         goto use_absdiff;
      }
      return false;
   } else if (op_instr->opcode != aco_opcode::s_sub_i32 &&
              op_instr->opcode != aco_opcode::s_sub_u32) {
      return false;
   }

use_absdiff:
   op_instr->opcode = aco_opcode::s_absdiff_i32;
   std::swap(instr->definitions[0], op_instr->definitions[0]);
   std::swap(instr->definitions[1], op_instr->definitions[1]);
   ctx.uses[instr->operands[0].tempId()]--;
   ctx.info[op_instr->definitions[0].tempId()].label = 0;
   ctx.info[op_instr->definitions[0].tempId()].parent_instr = op_instr;
   ctx.info[op_instr->definitions[1].tempId()].parent_instr = op_instr;
   ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
   ctx.info[instr->definitions[1].tempId()].parent_instr = instr.get();

   return true;
}

bool
combine_add_sub_b2i(opt_ctx& ctx, aco_ptr<Instruction>& instr, aco_opcode new_op, uint8_t ops)
{
   if (instr->usesModifiers())
      return false;

   for (unsigned i = 0; i < 2; i++) {
      if (!((1 << i) & ops))
         continue;
      if (instr->operands[i].isTemp() && ctx.info[instr->operands[i].tempId()].is_b2i() &&
          ctx.uses[instr->operands[i].tempId()] == 1) {

         aco_ptr<Instruction> new_instr;
         if (instr->operands[!i].isTemp() &&
             instr->operands[!i].getTemp().type() == RegType::vgpr) {
            new_instr.reset(create_instruction(new_op, Format::VOP2, 3, 2));
         } else if (ctx.program->gfx_level >= GFX10 ||
                    (instr->operands[!i].isConstant() && !instr->operands[!i].isLiteral())) {
            new_instr.reset(create_instruction(new_op, asVOP3(Format::VOP2), 3, 2));
         } else {
            return false;
         }
         ctx.uses[instr->operands[i].tempId()]--;
         new_instr->definitions[0] = instr->definitions[0];
         if (instr->definitions.size() == 2) {
            new_instr->definitions[1] = instr->definitions[1];
         } else {
            new_instr->definitions[1] =
               Definition(ctx.program->allocateTmp(ctx.program->lane_mask));
            /* Make sure the uses vector is large enough and the number of
             * uses properly initialized to 0.
             */
            ctx.uses.push_back(0);
            ctx.info.push_back(ssa_info{});
         }
         new_instr->operands[0] = Operand::zero();
         new_instr->operands[1] = instr->operands[!i];
         new_instr->operands[2] = Operand(ctx.info[instr->operands[i].tempId()].temp);
         new_instr->pass_flags = instr->pass_flags;
         instr = std::move(new_instr);
         ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
         ctx.info[instr->definitions[1].tempId()].parent_instr = instr.get();
         return true;
      }
   }

   return false;
}

bool
combine_add_bcnt(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (instr->usesModifiers())
      return false;

   for (unsigned i = 0; i < 2; i++) {
      Instruction* op_instr = follow_operand(ctx, instr->operands[i]);
      if (op_instr && op_instr->opcode == aco_opcode::v_bcnt_u32_b32 &&
          !op_instr->usesModifiers() && op_instr->operands[0].isTemp() &&
          op_instr->operands[0].getTemp().type() == RegType::vgpr &&
          op_instr->operands[1].constantEquals(0)) {
         aco_ptr<Instruction> new_instr{
            create_instruction(aco_opcode::v_bcnt_u32_b32, Format::VOP3, 2, 1)};
         ctx.uses[instr->operands[i].tempId()]--;
         new_instr->operands[0] = op_instr->operands[0];
         new_instr->operands[1] = instr->operands[!i];
         new_instr->definitions[0] = instr->definitions[0];
         new_instr->pass_flags = instr->pass_flags;
         instr = std::move(new_instr);
         ctx.info[instr->definitions[0].tempId()].label = 0;
         ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();

         return true;
      }
   }

   return false;
}

bool
get_minmax_info(aco_opcode op, aco_opcode* min, aco_opcode* max, aco_opcode* min3, aco_opcode* max3,
                aco_opcode* med3, aco_opcode* minmax, bool* some_gfx9_only)
{
   switch (op) {
#define MINMAX(type, gfx9)                                                                         \
   case aco_opcode::v_min_##type:                                                                  \
   case aco_opcode::v_max_##type:                                                                  \
      *min = aco_opcode::v_min_##type;                                                             \
      *max = aco_opcode::v_max_##type;                                                             \
      *med3 = aco_opcode::v_med3_##type;                                                           \
      *min3 = aco_opcode::v_min3_##type;                                                           \
      *max3 = aco_opcode::v_max3_##type;                                                           \
      *minmax = op == *min ? aco_opcode::v_maxmin_##type : aco_opcode::v_minmax_##type;            \
      *some_gfx9_only = gfx9;                                                                      \
      return true;
#define MINMAX_INT16(type, gfx9)                                                                   \
   case aco_opcode::v_min_##type:                                                                  \
   case aco_opcode::v_max_##type:                                                                  \
      *min = aco_opcode::v_min_##type;                                                             \
      *max = aco_opcode::v_max_##type;                                                             \
      *med3 = aco_opcode::v_med3_##type;                                                           \
      *min3 = aco_opcode::v_min3_##type;                                                           \
      *max3 = aco_opcode::v_max3_##type;                                                           \
      *minmax = aco_opcode::num_opcodes;                                                           \
      *some_gfx9_only = gfx9;                                                                      \
      return true;
#define MINMAX_INT16_E64(type, gfx9)                                                               \
   case aco_opcode::v_min_##type##_e64:                                                            \
   case aco_opcode::v_max_##type##_e64:                                                            \
      *min = aco_opcode::v_min_##type##_e64;                                                       \
      *max = aco_opcode::v_max_##type##_e64;                                                       \
      *med3 = aco_opcode::v_med3_##type;                                                           \
      *min3 = aco_opcode::v_min3_##type;                                                           \
      *max3 = aco_opcode::v_max3_##type;                                                           \
      *minmax = aco_opcode::num_opcodes;                                                           \
      *some_gfx9_only = gfx9;                                                                      \
      return true;
      MINMAX(f32, false)
      MINMAX(u32, false)
      MINMAX(i32, false)
      MINMAX(f16, true)
      MINMAX_INT16(u16, true)
      MINMAX_INT16(i16, true)
      MINMAX_INT16_E64(u16, true)
      MINMAX_INT16_E64(i16, true)
#undef MINMAX_INT16_E64
#undef MINMAX_INT16
#undef MINMAX
   default: return false;
   }
}

/* when ub > lb:
 * v_min_{f,u,i}{16,32}(v_max_{f,u,i}{16,32}(a, lb), ub) -> v_med3_{f,u,i}{16,32}(a, lb, ub)
 * v_max_{f,u,i}{16,32}(v_min_{f,u,i}{16,32}(a, ub), lb) -> v_med3_{f,u,i}{16,32}(a, lb, ub)
 */
bool
combine_clamp(opt_ctx& ctx, aco_ptr<Instruction>& instr, aco_opcode min, aco_opcode max,
              aco_opcode med)
{
   /* TODO: GLSL's clamp(x, minVal, maxVal) and SPIR-V's
    * FClamp(x, minVal, maxVal)/NClamp(x, minVal, maxVal) are undefined if
    * minVal > maxVal, which means we can always select it to a v_med3_f32 */
   aco_opcode other_op;
   if (instr->opcode == min)
      other_op = max;
   else if (instr->opcode == max)
      other_op = min;
   else
      return false;

   for (unsigned swap = 0; swap < 2; swap++) {
      Operand operands[3];
      bool clamp, precise;
      bitarray8 opsel = 0, neg = 0, abs = 0;
      uint8_t omod = 0;
      if (match_op3_for_vop3(ctx, instr->opcode, other_op, instr.get(), swap, "012", operands, neg,
                             abs, opsel, &clamp, &omod, NULL, NULL, NULL, &precise)) {
         /* max(min(src, upper), lower) returns upper if src is NaN, but
          * med3(src, lower, upper) returns lower.
          */
         if (precise && instr->opcode != min &&
             (min == aco_opcode::v_min_f16 || min == aco_opcode::v_min_f32))
            continue;

         int const0_idx = -1, const1_idx = -1;
         uint32_t const0 = 0, const1 = 0;
         for (int i = 0; i < 3; i++) {
            uint32_t val;
            bool hi16 = opsel & (1 << i);
            if (operands[i].isConstant()) {
               val = hi16 ? operands[i].constantValue16(true) : operands[i].constantValue();
            } else if (operands[i].isTemp() &&
                       ctx.info[operands[i].tempId()].is_constant_or_literal(32)) {
               val = ctx.info[operands[i].tempId()].val >> (hi16 ? 16 : 0);
            } else {
               continue;
            }
            if (const0_idx >= 0) {
               const1_idx = i;
               const1 = val;
            } else {
               const0_idx = i;
               const0 = val;
            }
         }
         if (const0_idx < 0 || const1_idx < 0)
            continue;

         int lower_idx = const0_idx;
         switch (min) {
         case aco_opcode::v_min_f32:
         case aco_opcode::v_min_f16: {
            float const0_f, const1_f;
            if (min == aco_opcode::v_min_f32) {
               memcpy(&const0_f, &const0, 4);
               memcpy(&const1_f, &const1, 4);
            } else {
               const0_f = _mesa_half_to_float(const0);
               const1_f = _mesa_half_to_float(const1);
            }
            if (abs[const0_idx])
               const0_f = fabsf(const0_f);
            if (abs[const1_idx])
               const1_f = fabsf(const1_f);
            if (neg[const0_idx])
               const0_f = -const0_f;
            if (neg[const1_idx])
               const1_f = -const1_f;
            lower_idx = const0_f < const1_f ? const0_idx : const1_idx;
            break;
         }
         case aco_opcode::v_min_u32: {
            lower_idx = const0 < const1 ? const0_idx : const1_idx;
            break;
         }
         case aco_opcode::v_min_u16:
         case aco_opcode::v_min_u16_e64: {
            lower_idx = (uint16_t)const0 < (uint16_t)const1 ? const0_idx : const1_idx;
            break;
         }
         case aco_opcode::v_min_i32: {
            int32_t const0_i =
               const0 & 0x80000000u ? -2147483648 + (int32_t)(const0 & 0x7fffffffu) : const0;
            int32_t const1_i =
               const1 & 0x80000000u ? -2147483648 + (int32_t)(const1 & 0x7fffffffu) : const1;
            lower_idx = const0_i < const1_i ? const0_idx : const1_idx;
            break;
         }
         case aco_opcode::v_min_i16:
         case aco_opcode::v_min_i16_e64: {
            int16_t const0_i = const0 & 0x8000u ? -32768 + (int16_t)(const0 & 0x7fffu) : const0;
            int16_t const1_i = const1 & 0x8000u ? -32768 + (int16_t)(const1 & 0x7fffu) : const1;
            lower_idx = const0_i < const1_i ? const0_idx : const1_idx;
            break;
         }
         default: break;
         }
         int upper_idx = lower_idx == const0_idx ? const1_idx : const0_idx;

         if (instr->opcode == min) {
            if (upper_idx != 0 || lower_idx == 0)
               return false;
         } else {
            if (upper_idx == 0 || lower_idx != 0)
               return false;
         }

         ctx.uses[instr->operands[swap].tempId()]--;
         create_vop3_for_op3(ctx, med, instr, operands, neg, abs, opsel, clamp, omod);

         return true;
      }
   }

   return false;
}

void
apply_sgprs(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   bool is_shift64 = instr->opcode == aco_opcode::v_lshlrev_b64_e64 ||
                     instr->opcode == aco_opcode::v_lshlrev_b64 ||
                     instr->opcode == aco_opcode::v_lshrrev_b64 ||
                     instr->opcode == aco_opcode::v_ashrrev_i64;

   /* find candidates and create the set of sgprs already read */
   unsigned sgpr_ids[2] = {0, 0};
   uint32_t operand_mask = 0;
   bool has_literal = false;
   for (unsigned i = 0; i < instr->operands.size(); i++) {
      if (instr->operands[i].isLiteral())
         has_literal = true;
      if (!instr->operands[i].isTemp())
         continue;
      if (instr->operands[i].getTemp().type() == RegType::sgpr) {
         if (instr->operands[i].tempId() != sgpr_ids[0])
            sgpr_ids[!!sgpr_ids[0]] = instr->operands[i].tempId();
      }
      ssa_info& info = ctx.info[instr->operands[i].tempId()];
      if (is_copy_label(ctx, instr, info, i) && info.temp.type() == RegType::sgpr)
         operand_mask |= 1u << i;
      if (info.is_extract() && info.parent_instr->operands[0].getTemp().type() == RegType::sgpr)
         operand_mask |= 1u << i;
   }
   unsigned max_sgprs = 1;
   if (ctx.program->gfx_level >= GFX10 && !is_shift64)
      max_sgprs = 2;
   if (has_literal)
      max_sgprs--;

   unsigned num_sgprs = !!sgpr_ids[0] + !!sgpr_ids[1];

   /* keep on applying sgprs until there is nothing left to be done */
   while (operand_mask) {
      uint32_t sgpr_idx = 0;
      uint32_t sgpr_info_id = 0;
      uint32_t mask = operand_mask;
      /* choose a sgpr */
      while (mask) {
         unsigned i = u_bit_scan(&mask);
         uint16_t uses = ctx.uses[instr->operands[i].tempId()];
         if (sgpr_info_id == 0 || uses < ctx.uses[sgpr_info_id]) {
            sgpr_idx = i;
            sgpr_info_id = instr->operands[i].tempId();
         }
      }
      operand_mask &= ~(1u << sgpr_idx);

      ssa_info& info = ctx.info[sgpr_info_id];

      Temp sgpr = info.is_extract() ? info.parent_instr->operands[0].getTemp() : info.temp;
      bool new_sgpr = sgpr.id() != sgpr_ids[0] && sgpr.id() != sgpr_ids[1];
      if (new_sgpr && num_sgprs >= max_sgprs)
         continue;

      if (sgpr_idx == 0)
         instr->format = withoutDPP(instr->format);

      if (sgpr_idx == 1 && instr->isDPP())
         continue;

      if (sgpr_idx == 0 || instr->isVOP3() || instr->isSDWA() || instr->isVOP3P() ||
          info.is_extract()) {
         /* can_apply_extract() checks SGPR encoding restrictions */
         if (info.is_extract() && can_apply_extract(ctx, instr, sgpr_idx, info))
            apply_extract(ctx, instr, sgpr_idx, info);
         else if (info.is_extract())
            continue;
         instr->operands[sgpr_idx] = Operand(sgpr);
      } else if (can_swap_operands(instr, &instr->opcode) && !instr->valu().opsel[sgpr_idx]) {
         instr->operands[sgpr_idx] = instr->operands[0];
         instr->operands[0] = Operand(sgpr);
         instr->valu().opsel[0].swap(instr->valu().opsel[sgpr_idx]);
         /* swap bits using a 4-entry LUT */
         uint32_t swapped = (0x3120 >> (operand_mask & 0x3)) & 0xf;
         operand_mask = (operand_mask & ~0x3) | swapped;
      } else if (can_use_VOP3(ctx, instr) && !info.is_extract()) {
         instr->format = asVOP3(instr->format);
         instr->operands[sgpr_idx] = Operand(sgpr);
      } else {
         continue;
      }

      if (new_sgpr)
         sgpr_ids[num_sgprs++] = sgpr.id();
      ctx.uses[sgpr_info_id]--;
      ctx.uses[sgpr.id()]++;

      /* TODO: handle when it's a VGPR */
      if ((ctx.info[sgpr.id()].label & (label_extract | label_temp)) &&
          ctx.info[sgpr.id()].temp.type() == RegType::sgpr)
         operand_mask |= 1u << sgpr_idx;
   }
}

bool
interp_can_become_fma(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (instr->opcode != aco_opcode::v_interp_p2_f32_inreg)
      return false;

   instr->opcode = aco_opcode::v_fma_f32;
   instr->format = Format::VOP3;
   bool dpp_allowed = can_use_DPP(ctx.program->gfx_level, instr, false);
   instr->opcode = aco_opcode::v_interp_p2_f32_inreg;
   instr->format = Format::VINTERP_INREG;

   return dpp_allowed;
}

void
interp_p2_f32_inreg_to_fma_dpp(aco_ptr<Instruction>& instr)
{
   static_assert(sizeof(DPP16_instruction) == sizeof(VINTERP_inreg_instruction),
                 "Invalid instr cast.");
   instr->format = asVOP3(Format::DPP16);
   instr->opcode = aco_opcode::v_fma_f32;
   instr->dpp16().dpp_ctrl = dpp_quad_perm(2, 2, 2, 2);
   instr->dpp16().row_mask = 0xf;
   instr->dpp16().bank_mask = 0xf;
   instr->dpp16().bound_ctrl = 0;
   instr->dpp16().fetch_inactive = 1;
}

/* apply omod / clamp modifiers if the def is used only once and the instruction can have modifiers */
bool
apply_omod_clamp(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (instr->definitions.empty() || ctx.uses[instr->definitions[0].tempId()] != 1 ||
       !instr_info.alu_opcode_infos[(int)instr->opcode].output_modifiers)
      return false;

   bool can_vop3 = can_use_VOP3(ctx, instr);
   bool is_mad_mix =
      instr->opcode == aco_opcode::v_fma_mix_f32 || instr->opcode == aco_opcode::v_fma_mixlo_f16;
   bool needs_vop3 = !instr->isSDWA() && !instr->isVINTERP_INREG() && !is_mad_mix;
   if (needs_vop3 && !can_vop3)
      return false;

   if (instr_info.classes[(int)instr->opcode] == instr_class::valu_pseudo_scalar_trans)
      return false;

   /* SDWA omod is GFX9+. */
   bool can_use_omod = (can_vop3 || ctx.program->gfx_level >= GFX9) && !instr->isVOP3P() &&
                       (!instr->isVINTERP_INREG() || interp_can_become_fma(ctx, instr));

   ssa_info& def_info = ctx.info[instr->definitions[0].tempId()];

   uint64_t omod_labels = label_omod2 | label_omod4 | label_omod5;
   if (!def_info.is_clamp() && !(can_use_omod && (def_info.label & omod_labels)))
      return false;
   /* if the omod/clamp instruction is dead, then the single user of this
    * instruction is a different instruction */
   if (!ctx.uses[def_info.mod_instr->definitions[0].tempId()])
      return false;

   if (def_info.mod_instr->definitions[0].bytes() != instr->definitions[0].bytes())
      return false;

   /* MADs/FMAs are created later, so we don't have to update the original add */
   assert(!ctx.info[instr->definitions[0].tempId()].is_mad());

   if (!def_info.is_clamp() && (instr->valu().clamp || instr->valu().omod))
      return false;

   if (needs_vop3)
      instr->format = asVOP3(instr->format);

   if (!def_info.is_clamp() && instr->opcode == aco_opcode::v_interp_p2_f32_inreg)
      interp_p2_f32_inreg_to_fma_dpp(instr);

   if (def_info.is_omod2())
      instr->valu().omod = 1;
   else if (def_info.is_omod4())
      instr->valu().omod = 2;
   else if (def_info.is_omod5())
      instr->valu().omod = 3;
   else if (def_info.is_clamp())
      instr->valu().clamp = true;

   instr->definitions[0].swapTemp(def_info.mod_instr->definitions[0]);
   ctx.info[instr->definitions[0].tempId()].label &= label_clamp | label_insert | label_f2f16;
   ctx.uses[def_info.mod_instr->definitions[0].tempId()]--;
   ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
   ctx.info[def_info.mod_instr->definitions[0].tempId()].parent_instr = def_info.mod_instr;

   return true;
}

/* Combine an p_insert (or p_extract, in some cases) instruction with instr.
 * p_insert(instr(...)) -> instr_insert().
 */
bool
apply_insert(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (instr->definitions.empty() || ctx.uses[instr->definitions[0].tempId()] != 1)
      return false;

   ssa_info& def_info = ctx.info[instr->definitions[0].tempId()];
   if (!def_info.is_insert())
      return false;
   /* if the insert instruction is dead, then the single user of this
    * instruction is a different instruction */
   if (!ctx.uses[def_info.mod_instr->definitions[0].tempId()])
      return false;

   /* MADs/FMAs are created later, so we don't have to update the original add */
   assert(!ctx.info[instr->definitions[0].tempId()].is_mad());

   SubdwordSel sel = parse_insert(def_info.mod_instr);
   assert(sel);

   if (!can_use_SDWA(ctx.program->gfx_level, instr, true))
      return false;

   convert_to_SDWA(ctx.program->gfx_level, instr);
   if (instr->sdwa().dst_sel.size() != 4)
      return false;
   instr->sdwa().dst_sel = sel;

   instr->definitions[0].swapTemp(def_info.mod_instr->definitions[0]);
   ctx.info[instr->definitions[0].tempId()].label = 0;
   ctx.uses[def_info.mod_instr->definitions[0].tempId()]--;
   ctx.info[instr->definitions[0].tempId()].label = 0;
   ctx.info[def_info.mod_instr->definitions[0].tempId()].parent_instr = def_info.mod_instr;
   for (const Definition& def : instr->definitions)
      ctx.info[def.tempId()].parent_instr = instr.get();

   return true;
}

/* Remove superfluous extract after ds_read like so:
 * p_extract(ds_read_uN(), 0, N, 0) -> ds_read_uN()
 */
bool
apply_load_extract(opt_ctx& ctx, aco_ptr<Instruction>& extract)
{
   /* Check if p_extract has a usedef operand and is the only user. */
   if (ctx.uses[extract->operands[0].tempId()] > 1)
      return false;

   /* Check if the usedef is the right format. */
   Instruction* load = ctx.info[extract->operands[0].tempId()].parent_instr;
   if (!load->isDS() && !load->isSMEM() && !load->isMUBUF() && !load->isFlatLike())
      return false;

   unsigned extract_idx = extract->operands[1].constantValue();
   unsigned bits_extracted = extract->operands[2].constantValue();
   bool sign_ext = extract->operands[3].constantValue();
   unsigned dst_bitsize = extract->definitions[0].bytes() * 8u;

   unsigned bits_loaded = 0;
   bool can_shrink = false;
   switch (load->opcode) {
   case aco_opcode::ds_read_u8:
   case aco_opcode::ds_read_u8_d16:
   case aco_opcode::flat_load_ubyte:
   case aco_opcode::flat_load_ubyte_d16:
   case aco_opcode::global_load_ubyte:
   case aco_opcode::global_load_ubyte_d16:
   case aco_opcode::scratch_load_ubyte:
   case aco_opcode::scratch_load_ubyte_d16: can_shrink = true; FALLTHROUGH;
   case aco_opcode::s_load_ubyte:
   case aco_opcode::s_buffer_load_ubyte:
   case aco_opcode::buffer_load_ubyte:
   case aco_opcode::buffer_load_ubyte_d16: bits_loaded = 8; break;
   case aco_opcode::ds_read_u16:
   case aco_opcode::ds_read_u16_d16:
   case aco_opcode::flat_load_ushort:
   case aco_opcode::flat_load_short_d16:
   case aco_opcode::global_load_ushort:
   case aco_opcode::global_load_short_d16:
   case aco_opcode::scratch_load_ushort:
   case aco_opcode::scratch_load_short_d16: can_shrink = true; FALLTHROUGH;
   case aco_opcode::s_load_ushort:
   case aco_opcode::s_buffer_load_ushort:
   case aco_opcode::buffer_load_ushort:
   case aco_opcode::buffer_load_short_d16: bits_loaded = 16; break;
   default: return false;
   }

   /* TODO: These are doable, but probably don't occur too often. */
   if (extract_idx || bits_extracted > bits_loaded || dst_bitsize > 32 ||
       (load->definitions[0].regClass().type() != extract->definitions[0].regClass().type()))
      return false;

   /* We can't shrink some loads because that would remove zeroing of the offset/address LSBs. */
   if (!can_shrink && bits_extracted < bits_loaded)
      return false;

   /* Shrink the load if the extracted bit size is smaller. */
   bits_loaded = MIN2(bits_loaded, bits_extracted);

   /* Change the opcode so it writes the full register. */
   bool is_s_buffer = load->opcode == aco_opcode::s_buffer_load_ubyte ||
                      load->opcode == aco_opcode::s_buffer_load_ushort;
   if (bits_loaded == 8 && load->isDS())
      load->opcode = sign_ext ? aco_opcode::ds_read_i8 : aco_opcode::ds_read_u8;
   else if (bits_loaded == 16 && load->isDS())
      load->opcode = sign_ext ? aco_opcode::ds_read_i16 : aco_opcode::ds_read_u16;
   else if (bits_loaded == 8 && load->isMUBUF())
      load->opcode = sign_ext ? aco_opcode::buffer_load_sbyte : aco_opcode::buffer_load_ubyte;
   else if (bits_loaded == 16 && load->isMUBUF())
      load->opcode = sign_ext ? aco_opcode::buffer_load_sshort : aco_opcode::buffer_load_ushort;
   else if (bits_loaded == 8 && load->isFlat())
      load->opcode = sign_ext ? aco_opcode::flat_load_sbyte : aco_opcode::flat_load_ubyte;
   else if (bits_loaded == 16 && load->isFlat())
      load->opcode = sign_ext ? aco_opcode::flat_load_sshort : aco_opcode::flat_load_ushort;
   else if (bits_loaded == 8 && load->isGlobal())
      load->opcode = sign_ext ? aco_opcode::global_load_sbyte : aco_opcode::global_load_ubyte;
   else if (bits_loaded == 16 && load->isGlobal())
      load->opcode = sign_ext ? aco_opcode::global_load_sshort : aco_opcode::global_load_ushort;
   else if (bits_loaded == 8 && load->isScratch())
      load->opcode = sign_ext ? aco_opcode::scratch_load_sbyte : aco_opcode::scratch_load_ubyte;
   else if (bits_loaded == 16 && load->isScratch())
      load->opcode = sign_ext ? aco_opcode::scratch_load_sshort : aco_opcode::scratch_load_ushort;
   else if (bits_loaded == 8 && load->isSMEM() && is_s_buffer)
      load->opcode = sign_ext ? aco_opcode::s_buffer_load_sbyte : aco_opcode::s_buffer_load_ubyte;
   else if (bits_loaded == 8 && load->isSMEM() && !is_s_buffer)
      load->opcode = sign_ext ? aco_opcode::s_load_sbyte : aco_opcode::s_load_ubyte;
   else if (bits_loaded == 16 && load->isSMEM() && is_s_buffer)
      load->opcode = sign_ext ? aco_opcode::s_buffer_load_sshort : aco_opcode::s_buffer_load_ushort;
   else if (bits_loaded == 16 && load->isSMEM() && !is_s_buffer)
      load->opcode = sign_ext ? aco_opcode::s_load_sshort : aco_opcode::s_load_ushort;
   else
      UNREACHABLE("Forgot to add opcode above.");

   if (dst_bitsize <= 16 && ctx.program->gfx_level >= GFX9) {
      switch (load->opcode) {
      case aco_opcode::ds_read_i8: load->opcode = aco_opcode::ds_read_i8_d16; break;
      case aco_opcode::ds_read_u8: load->opcode = aco_opcode::ds_read_u8_d16; break;
      case aco_opcode::ds_read_i16: load->opcode = aco_opcode::ds_read_u16_d16; break;
      case aco_opcode::ds_read_u16: load->opcode = aco_opcode::ds_read_u16_d16; break;
      case aco_opcode::buffer_load_sbyte: load->opcode = aco_opcode::buffer_load_sbyte_d16; break;
      case aco_opcode::buffer_load_ubyte: load->opcode = aco_opcode::buffer_load_ubyte_d16; break;
      case aco_opcode::buffer_load_sshort: load->opcode = aco_opcode::buffer_load_short_d16; break;
      case aco_opcode::buffer_load_ushort: load->opcode = aco_opcode::buffer_load_short_d16; break;
      case aco_opcode::flat_load_sbyte: load->opcode = aco_opcode::flat_load_sbyte_d16; break;
      case aco_opcode::flat_load_ubyte: load->opcode = aco_opcode::flat_load_ubyte_d16; break;
      case aco_opcode::flat_load_sshort: load->opcode = aco_opcode::flat_load_short_d16; break;
      case aco_opcode::flat_load_ushort: load->opcode = aco_opcode::flat_load_short_d16; break;
      case aco_opcode::global_load_sbyte: load->opcode = aco_opcode::global_load_sbyte_d16; break;
      case aco_opcode::global_load_ubyte: load->opcode = aco_opcode::global_load_ubyte_d16; break;
      case aco_opcode::global_load_sshort: load->opcode = aco_opcode::global_load_short_d16; break;
      case aco_opcode::global_load_ushort: load->opcode = aco_opcode::global_load_short_d16; break;
      case aco_opcode::scratch_load_sbyte: load->opcode = aco_opcode::scratch_load_sbyte_d16; break;
      case aco_opcode::scratch_load_ubyte: load->opcode = aco_opcode::scratch_load_ubyte_d16; break;
      case aco_opcode::scratch_load_sshort: load->opcode = aco_opcode::scratch_load_short_d16; break;
      case aco_opcode::scratch_load_ushort: load->opcode = aco_opcode::scratch_load_short_d16; break;
      default: break;
      }
   }

   /* The load now produces the exact same thing as the extract, remove the extract. */
   std::swap(load->definitions[0], extract->definitions[0]);
   ctx.uses[extract->definitions[0].tempId()] = 0;
   ctx.info[load->definitions[0].tempId()].label = 0;
   ctx.info[extract->definitions[0].tempId()].parent_instr = extract.get();
   ctx.info[load->definitions[0].tempId()].parent_instr = load;
   return true;
}

/* v_and(a, not(b)) -> v_bfi_b32(b, 0, a)
 * v_or(a, not(b)) -> v_bfi_b32(b, a, -1)
 */
bool
combine_v_andor_not(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (instr->usesModifiers())
      return false;

   for (unsigned i = 0; i < 2; i++) {
      Instruction* op_instr = follow_operand(ctx, instr->operands[i], true);
      if (op_instr && !op_instr->usesModifiers() &&
          (op_instr->opcode == aco_opcode::v_not_b32 ||
           op_instr->opcode == aco_opcode::s_not_b32)) {

         Operand ops[3] = {
            op_instr->operands[0],
            Operand::zero(),
            instr->operands[!i],
         };
         if (instr->opcode == aco_opcode::v_or_b32) {
            ops[1] = instr->operands[!i];
            ops[2] = Operand::c32(-1);
         }
         if (!check_vop3_operands(ctx, 3, ops))
            continue;

         Instruction* new_instr = create_instruction(aco_opcode::v_bfi_b32, Format::VOP3, 3, 1);

         if (op_instr->operands[0].isTemp())
            ctx.uses[op_instr->operands[0].tempId()]++;
         for (unsigned j = 0; j < 3; j++)
            new_instr->operands[j] = ops[j];
         new_instr->definitions[0] = instr->definitions[0];
         new_instr->pass_flags = instr->pass_flags;
         instr.reset(new_instr);
         decrease_and_dce(ctx, op_instr->definitions[0].getTemp());
         ctx.info[instr->definitions[0].tempId()].label = 0;
         ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
         return true;
      }
   }

   return false;
}

/* v_add_co(c, s_lshl(a, b)) -> v_mad_u32_u24(a, 1<<b, c)
 * v_add_co(c, v_lshlrev(a, b)) -> v_mad_u32_u24(b, 1<<a, c)
 * v_sub(c, s_lshl(a, b)) -> v_mad_i32_i24(a, -(1<<b), c)
 * v_sub(c, v_lshlrev(a, b)) -> v_mad_i32_i24(b, -(1<<a), c)
 */
bool
combine_add_lshl(opt_ctx& ctx, aco_ptr<Instruction>& instr, bool is_sub)
{
   if (instr->usesModifiers())
      return false;

   /* Substractions: start at operand 1 to avoid mixup such as
    * turning v_sub(v_lshlrev(a, b), c) into v_mad_i32_i24(b, -(1<<a), c)
    */
   unsigned start_op_idx = is_sub ? 1 : 0;

   /* Don't allow 24-bit operands on subtraction because
    * v_mad_i32_i24 applies a sign extension.
    */
   bool allow_24bit = !is_sub;

   for (unsigned i = start_op_idx; i < 2; i++) {
      Instruction* op_instr = follow_operand(ctx, instr->operands[i]);
      if (!op_instr)
         continue;

      if (op_instr->opcode != aco_opcode::s_lshl_b32 &&
          op_instr->opcode != aco_opcode::v_lshlrev_b32)
         continue;

      int shift_op_idx = op_instr->opcode == aco_opcode::s_lshl_b32 ? 1 : 0;

      if (op_instr->operands[shift_op_idx].isConstant() &&
          ((allow_24bit && op_instr->operands[!shift_op_idx].is24bit()) ||
           op_instr->operands[!shift_op_idx].is16bit())) {
         uint32_t multiplier = 1 << (op_instr->operands[shift_op_idx].constantValue() % 32u);
         if (is_sub)
            multiplier = -multiplier;
         if (is_sub ? (multiplier < 0xff800000) : (multiplier > 0xffffff))
            continue;

         Operand ops[3] = {
            op_instr->operands[!shift_op_idx],
            Operand::c32(multiplier),
            instr->operands[!i],
         };
         if (!check_vop3_operands(ctx, 3, ops))
            return false;

         ctx.uses[instr->operands[i].tempId()]--;

         aco_opcode mad_op = is_sub ? aco_opcode::v_mad_i32_i24 : aco_opcode::v_mad_u32_u24;
         aco_ptr<Instruction> new_instr{create_instruction(mad_op, Format::VOP3, 3, 1)};
         for (unsigned op_idx = 0; op_idx < 3; ++op_idx)
            new_instr->operands[op_idx] = ops[op_idx];
         new_instr->definitions[0] = instr->definitions[0];
         new_instr->pass_flags = instr->pass_flags;
         instr = std::move(new_instr);
         ctx.info[instr->definitions[0].tempId()].label = 0;
         ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
         return true;
      }
   }

   return false;
}

void
propagate_swizzles(VALU_instruction* instr, bool opsel_lo, bool opsel_hi)
{
   /* propagate swizzles which apply to a result down to the instruction's operands:
    * result = a.xy + b.xx -> result.yx = a.yx + b.xx */
   uint8_t tmp_lo = instr->opsel_lo;
   uint8_t tmp_hi = instr->opsel_hi;
   uint8_t neg_lo = instr->neg_lo;
   uint8_t neg_hi = instr->neg_hi;
   if (opsel_lo == 1) {
      instr->opsel_lo = tmp_hi;
      instr->neg_lo = neg_hi;
   }
   if (opsel_hi == 0) {
      instr->opsel_hi = tmp_lo;
      instr->neg_hi = neg_lo;
   }
}

void
combine_vop3p(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   VALU_instruction* vop3p = &instr->valu();

   /* apply clamp */
   if (instr->opcode == aco_opcode::v_pk_mul_f16 && instr->operands[1].constantEquals(0x3C00) &&
       vop3p->clamp && instr->operands[0].isTemp() && ctx.uses[instr->operands[0].tempId()] == 1 &&
       !vop3p->opsel_lo[1] && !vop3p->opsel_hi[1]) {

      Instruction* op_instr = ctx.info[instr->operands[0].tempId()].parent_instr;
      const aco_alu_opcode_info& opcode_info = instr_info.alu_opcode_infos[(int)op_instr->opcode];
      aco_type op_type = opcode_info.def_types[0];
      if (op_instr->isVOP3P() && op_type.num_components == 2 &&
          op_type.base_type == aco_base_type_float && op_type.bit_size == 16 &&
          opcode_info.output_modifiers) {
         op_instr->valu().clamp = true;
         propagate_swizzles(&op_instr->valu(), vop3p->opsel_lo[0], vop3p->opsel_hi[0]);
         instr->definitions[0].swapTemp(op_instr->definitions[0]);
         ctx.info[op_instr->definitions[0].tempId()].parent_instr = op_instr;
         ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
         ctx.uses[instr->definitions[0].tempId()]--;
         return;
      }
   }

   if (instr->opcode == aco_opcode::v_pk_add_f16 || instr->opcode == aco_opcode::v_pk_add_u16) {
      bool fadd = instr->opcode == aco_opcode::v_pk_add_f16;
      if (fadd && instr->definitions[0].isPrecise())
         return;
      if (!fadd && instr->valu().clamp)
         return;

      Instruction* mul_instr = nullptr;
      unsigned add_op_idx = 0;
      bitarray8 mul_neg_lo = 0, mul_neg_hi = 0, mul_opsel_lo = 0, mul_opsel_hi = 0;
      uint32_t uses = UINT32_MAX;

      /* find the 'best' mul instruction to combine with the add */
      for (unsigned i = 0; i < 2; i++) {
         Instruction* op_instr = follow_operand(ctx, instr->operands[i], true);
         if (!op_instr)
            continue;

         if (op_instr->isVOP3P()) {
            if (fadd) {
               if (op_instr->opcode != aco_opcode::v_pk_mul_f16 ||
                   op_instr->definitions[0].isPrecise())
                  continue;
            } else {
               if (op_instr->opcode != aco_opcode::v_pk_mul_lo_u16)
                  continue;
            }

            /* no clamp allowed between mul and add */
            if (op_instr->valu().clamp)
               continue;

            Operand op[3] = {op_instr->operands[0], op_instr->operands[1], instr->operands[1 - i]};
            if (ctx.uses[instr->operands[i].tempId()] >= uses || !check_vop3_operands(ctx, 3, op))
               continue;

            mul_instr = op_instr;
            add_op_idx = 1 - i;
            uses = ctx.uses[instr->operands[i].tempId()];
            mul_neg_lo = mul_instr->valu().neg_lo;
            mul_neg_hi = mul_instr->valu().neg_hi;
            mul_opsel_lo = mul_instr->valu().opsel_lo;
            mul_opsel_hi = mul_instr->valu().opsel_hi;
         } else if (instr->operands[i].bytes() == 2) {
            if ((fadd && (op_instr->opcode != aco_opcode::v_mul_f16 ||
                          op_instr->definitions[0].isPrecise())) ||
                (!fadd && op_instr->opcode != aco_opcode::v_mul_lo_u16 &&
                 op_instr->opcode != aco_opcode::v_mul_lo_u16_e64))
               continue;

            if (op_instr->valu().clamp || op_instr->valu().omod || op_instr->valu().abs)
               continue;

            if (op_instr->isDPP() || (op_instr->isSDWA() && (op_instr->sdwa().sel[0].size() < 2 ||
                                                             op_instr->sdwa().sel[1].size() < 2)))
               continue;

            Operand op[3] = {op_instr->operands[0], op_instr->operands[1], instr->operands[1 - i]};
            if (ctx.uses[instr->operands[i].tempId()] >= uses || !check_vop3_operands(ctx, 3, op))
               continue;

            mul_instr = op_instr;
            add_op_idx = 1 - i;
            uses = ctx.uses[instr->operands[i].tempId()];
            mul_neg_lo = mul_instr->valu().neg;
            mul_neg_hi = mul_instr->valu().neg;
            if (mul_instr->isSDWA()) {
               for (unsigned j = 0; j < 2; j++)
                  mul_opsel_lo[j] = mul_instr->sdwa().sel[j].offset();
            } else {
               mul_opsel_lo = mul_instr->valu().opsel;
            }
            mul_opsel_hi = mul_opsel_lo;
         }
      }

      if (!mul_instr)
         return;

      /* turn mul + packed add into v_pk_fma_f16 */
      aco_opcode mad = fadd ? aco_opcode::v_pk_fma_f16 : aco_opcode::v_pk_mad_u16;
      aco_ptr<Instruction> fma{create_instruction(mad, Format::VOP3P, 3, 1)};
      fma->operands[0] = copy_operand(ctx, mul_instr->operands[0]);
      fma->operands[1] = copy_operand(ctx, mul_instr->operands[1]);
      fma->operands[2] = instr->operands[add_op_idx];
      fma->valu().clamp = vop3p->clamp;
      fma->valu().neg_lo = mul_neg_lo;
      fma->valu().neg_hi = mul_neg_hi;
      fma->valu().opsel_lo = mul_opsel_lo;
      fma->valu().opsel_hi = mul_opsel_hi;
      propagate_swizzles(&fma->valu(), vop3p->opsel_lo[1 - add_op_idx],
                         vop3p->opsel_hi[1 - add_op_idx]);
      fma->valu().opsel_lo[2] = vop3p->opsel_lo[add_op_idx];
      fma->valu().opsel_hi[2] = vop3p->opsel_hi[add_op_idx];
      fma->valu().neg_lo[2] = vop3p->neg_lo[add_op_idx];
      fma->valu().neg_hi[2] = vop3p->neg_hi[add_op_idx];
      fma->valu().neg_lo[1] = fma->valu().neg_lo[1] ^ vop3p->neg_lo[1 - add_op_idx];
      fma->valu().neg_hi[1] = fma->valu().neg_hi[1] ^ vop3p->neg_hi[1 - add_op_idx];
      fma->definitions[0] = instr->definitions[0];
      fma->pass_flags = instr->pass_flags;
      instr = std::move(fma);
      ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
      decrease_and_dce(ctx, mul_instr->definitions[0].getTemp());
      return;
   }
}

bool
can_use_mad_mix(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (ctx.program->gfx_level < GFX9)
      return false;

   /* v_mad_mix* on GFX9 always flushes denormals for 16-bit inputs/outputs */
   if (ctx.program->gfx_level == GFX9 && ctx.fp_mode.denorm16_64)
      return false;

   if (instr->valu().omod)
      return false;

   switch (instr->opcode) {
   case aco_opcode::v_add_f32:
   case aco_opcode::v_sub_f32:
   case aco_opcode::v_subrev_f32:
   case aco_opcode::v_mul_f32: return !instr->isSDWA() && !instr->isDPP();
   case aco_opcode::v_fma_f32:
      return ctx.program->dev.fused_mad_mix || !instr->definitions[0].isPrecise();
   case aco_opcode::v_fma_mix_f32:
   case aco_opcode::v_fma_mixlo_f16: return true;
   default: return false;
   }
}

void
to_mad_mix(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   ctx.info[instr->definitions[0].tempId()].label &= label_f2f16 | label_clamp;

   if (instr->opcode == aco_opcode::v_fma_f32) {
      instr->format = (Format)((uint32_t)withoutVOP3(instr->format) | (uint32_t)(Format::VOP3P));
      instr->opcode = aco_opcode::v_fma_mix_f32;
      return;
   }

   bool is_add = instr->opcode != aco_opcode::v_mul_f32;

   aco_ptr<Instruction> vop3p{create_instruction(aco_opcode::v_fma_mix_f32, Format::VOP3P, 3, 1)};

   for (unsigned i = 0; i < instr->operands.size(); i++) {
      vop3p->operands[is_add + i] = instr->operands[i];
      vop3p->valu().neg_lo[is_add + i] = instr->valu().neg[i];
      vop3p->valu().neg_hi[is_add + i] = instr->valu().abs[i];
   }
   if (instr->opcode == aco_opcode::v_mul_f32) {
      vop3p->operands[2] = Operand::zero();
      vop3p->valu().neg_lo[2] = true;
   } else if (is_add) {
      vop3p->operands[0] = Operand::c32(0x3f800000);
      if (instr->opcode == aco_opcode::v_sub_f32)
         vop3p->valu().neg_lo[2] ^= true;
      else if (instr->opcode == aco_opcode::v_subrev_f32)
         vop3p->valu().neg_lo[1] ^= true;
   }
   vop3p->definitions[0] = instr->definitions[0];
   vop3p->valu().clamp = instr->valu().clamp;
   vop3p->pass_flags = instr->pass_flags;
   instr = std::move(vop3p);
   ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
}

bool
combine_output_conversion(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   ssa_info& def_info = ctx.info[instr->definitions[0].tempId()];
   if (!def_info.is_f2f16())
      return false;
   Instruction* conv = def_info.mod_instr;

   if (!ctx.uses[conv->definitions[0].tempId()] || ctx.uses[instr->definitions[0].tempId()] != 1)
      return false;

   if (conv->usesModifiers())
      return false;

   if (interp_can_become_fma(ctx, instr))
      interp_p2_f32_inreg_to_fma_dpp(instr);

   if (!can_use_mad_mix(ctx, instr))
      return false;

   if (!instr->isVOP3P())
      to_mad_mix(ctx, instr);

   instr->opcode = aco_opcode::v_fma_mixlo_f16;
   instr->definitions[0].swapTemp(conv->definitions[0]);
   if (conv->definitions[0].isPrecise())
      instr->definitions[0].setPrecise(true);
   ctx.info[instr->definitions[0].tempId()].label &= label_clamp;
   ctx.uses[conv->definitions[0].tempId()]--;
   ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
   ctx.info[conv->definitions[0].tempId()].parent_instr = conv;

   return true;
}

void
combine_mad_mix(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (!can_use_mad_mix(ctx, instr))
      return;

   for (unsigned i = 0; i < instr->operands.size(); i++) {
      if (!instr->operands[i].isTemp())
         continue;
      Temp tmp = instr->operands[i].getTemp();

      Instruction* conv = ctx.info[tmp.id()].parent_instr;
      if (conv->opcode != aco_opcode::v_cvt_f32_f16 || !conv->operands[0].isTemp() ||
          conv->valu().clamp || conv->valu().omod) {
         continue;
      } else if (conv->isSDWA() &&
                 (conv->sdwa().dst_sel.size() != 4 || conv->sdwa().sel[0].size() != 2)) {
         continue;
      } else if (conv->isDPP()) {
         continue;
      }

      if (get_operand_type(instr, i).bit_size != 32)
         continue;

      /* Conversion to VOP3P will add inline constant operands, but that shouldn't affect
       * check_vop3_operands(). */
      Operand op[3];
      for (unsigned j = 0; j < instr->operands.size(); j++)
         op[j] = instr->operands[j];
      op[i] = conv->operands[0];
      if (!check_vop3_operands(ctx, instr->operands.size(), op))
         continue;
      if (!conv->operands[0].isOfType(RegType::vgpr) && instr->isDPP())
         continue;

      if (!instr->isVOP3P()) {
         bool is_add =
            instr->opcode != aco_opcode::v_mul_f32 && instr->opcode != aco_opcode::v_fma_f32;
         to_mad_mix(ctx, instr);
         i += is_add;
      }

      if (--ctx.uses[tmp.id()])
         ctx.uses[conv->operands[0].tempId()]++;
      instr->operands[i].setTemp(conv->operands[0].getTemp());
      if (conv->definitions[0].isPrecise())
         instr->definitions[0].setPrecise(true);
      instr->valu().opsel_hi[i] = true;
      if (conv->isSDWA() && conv->sdwa().sel[0].offset() == 2)
         instr->valu().opsel_lo[i] = true;
      else
         instr->valu().opsel_lo[i] = conv->valu().opsel[0];
      bool neg = conv->valu().neg[0];
      bool abs = conv->valu().abs[0];
      if (!instr->valu().abs[i]) {
         instr->valu().neg[i] ^= neg;
         instr->valu().abs[i] = abs;
      }
   }
}

// TODO: we could possibly move the whole label_instruction pass to combine_instruction:
// this would mean that we'd have to fix the instruction uses while value propagation

/* also returns true for inf */
bool
is_pow_of_two(opt_ctx& ctx, Operand op)
{
   if (op.isTemp() && ctx.info[op.tempId()].is_constant_or_literal(op.bytes() * 8))
      return is_pow_of_two(ctx, get_constant_op(ctx, ctx.info[op.tempId()], op.bytes() * 8));
   else if (!op.isConstant())
      return false;

   uint64_t val = op.constantValue64();

   if (op.bytes() == 4) {
      uint32_t exponent = (val & 0x7f800000) >> 23;
      uint32_t fraction = val & 0x007fffff;
      return (exponent >= 127) && (fraction == 0);
   } else if (op.bytes() == 2) {
      uint32_t exponent = (val & 0x7c00) >> 10;
      uint32_t fraction = val & 0x03ff;
      return (exponent >= 15) && (fraction == 0);
   } else {
      assert(op.bytes() == 8);
      uint64_t exponent = (val & UINT64_C(0x7ff0000000000000)) >> 52;
      uint64_t fraction = val & UINT64_C(0x000fffffffffffff);
      return (exponent >= 1023) && (fraction == 0);
   }
}

bool
is_mul(Instruction* instr)
{
   switch (instr->opcode) {
   case aco_opcode::v_mul_f64_e64:
   case aco_opcode::v_mul_f64:
   case aco_opcode::v_mul_f32:
   case aco_opcode::v_mul_legacy_f32:
   case aco_opcode::v_mul_f16: return true;
   case aco_opcode::v_fma_mix_f32:
      return instr->operands[2].constantEquals(0) && instr->valu().neg[2];
   default: return false;
   }
}

void
combine_instruction(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   if (instr->definitions.empty() || is_dead(ctx.uses, instr.get()))
      return;

   if (instr->isVALU() || instr->isSALU()) {
      /* Apply SDWA. Do this after label_instruction() so it can remove
       * label_extract if not all instructions can take SDWA. */
      for (unsigned i = 0; i < instr->operands.size(); i++) {
         Operand& op = instr->operands[i];
         if (!op.isTemp())
            continue;
         ssa_info& info = ctx.info[op.tempId()];
         if (!info.is_extract())
            continue;
         /* if there are that many uses, there are likely better combinations */
         // TODO: delay applying extract to a point where we know better
         if (ctx.uses[op.tempId()] > 4) {
            info.label &= ~label_extract;
            continue;
         }
         if (info.is_extract() &&
             (info.parent_instr->operands[0].getTemp().type() == RegType::vgpr ||
              instr->operands[i].getTemp().type() == RegType::sgpr) &&
             can_apply_extract(ctx, instr, i, info)) {
            /* Increase use count of the extract's operand if the extract still has uses. */
            apply_extract(ctx, instr, i, info);
            if (--ctx.uses[instr->operands[i].tempId()])
               ctx.uses[info.parent_instr->operands[0].tempId()]++;
            instr->operands[i].setTemp(info.parent_instr->operands[0].getTemp());
         }
      }
   }

   if (instr->isVALU()) {
      if (can_apply_sgprs(ctx, instr))
         apply_sgprs(ctx, instr);
      combine_mad_mix(ctx, instr);
      while (apply_omod_clamp(ctx, instr) || combine_output_conversion(ctx, instr))
         ;
      apply_insert(ctx, instr);
   }

   if (instr->isVOP3P() && instr->opcode != aco_opcode::v_fma_mix_f32 &&
       instr->opcode != aco_opcode::v_fma_mixlo_f16)
      return combine_vop3p(ctx, instr);

   if (instr->isSDWA() || instr->isDPP())
      return;

   if (instr->opcode == aco_opcode::p_extract || instr->opcode == aco_opcode::p_extract_vector) {
      ssa_info& info = ctx.info[instr->operands[0].tempId()];
      if (info.is_extract() && can_apply_extract(ctx, instr, 0, info)) {
         apply_extract(ctx, instr, 0, info);
         if (--ctx.uses[instr->operands[0].tempId()])
            ctx.uses[info.parent_instr->operands[0].tempId()]++;
         instr->operands[0].setTemp(info.parent_instr->operands[0].getTemp());
      }

      if (instr->opcode == aco_opcode::p_extract)
         apply_load_extract(ctx, instr);
   }

   /* TODO: There are still some peephole optimizations that could be done:
    * - abs(a - b) -> s_absdiff_i32
    * - various patterns for s_bitcmp{0,1}_b32 and s_bitset{0,1}_b32
    * - patterns for v_alignbit_b32 and v_alignbyte_b32
    * These aren't probably too interesting though.
    * There are also patterns for v_cmp_class_f{16,32,64}. This is difficult but
    * probably more useful than the previously mentioned optimizations.
    * The various comparison optimizations also currently only work with 32-bit
    * floats. */

   /* neg(mul(a, b)) -> mul(neg(a), b), abs(mul(a, b)) -> mul(abs(a), abs(b)) */
   if ((ctx.info[instr->definitions[0].tempId()].label & (label_neg | label_abs)) &&
       ctx.uses[ctx.info[instr->definitions[0].tempId()].temp.id()] == 1) {
      Temp val = ctx.info[instr->definitions[0].tempId()].temp;
      Instruction* mul_instr = ctx.info[val.id()].parent_instr;

      if (!is_mul(mul_instr))
         return;

      if (mul_instr->operands[0].isLiteral())
         return;
      if (mul_instr->valu().clamp)
         return;
      if (mul_instr->isSDWA() || mul_instr->isDPP())
         return;
      if (mul_instr->opcode == aco_opcode::v_mul_legacy_f32 &&
          mul_instr->definitions[0].isSZPreserve())
         return;
      if (mul_instr->definitions[0].bytes() != instr->definitions[0].bytes())
         return;

      /* convert to mul(neg(a), b), mul(abs(a), abs(b)) or mul(neg(abs(a)), abs(b)) */
      ctx.uses[mul_instr->definitions[0].tempId()]--;
      Definition def = instr->definitions[0];
      bool is_neg = ctx.info[instr->definitions[0].tempId()].is_neg();
      bool is_abs = ctx.info[instr->definitions[0].tempId()].is_abs();
      uint32_t pass_flags = instr->pass_flags;
      Format format = mul_instr->format == Format::VOP2 ? asVOP3(Format::VOP2) : mul_instr->format;
      instr.reset(create_instruction(mul_instr->opcode, format, mul_instr->operands.size(), 1));
      std::copy(mul_instr->operands.cbegin(), mul_instr->operands.cend(), instr->operands.begin());
      instr->pass_flags = pass_flags;
      instr->definitions[0] = def;
      VALU_instruction& new_mul = instr->valu();
      VALU_instruction& mul = mul_instr->valu();
      new_mul.neg = mul.neg;
      new_mul.abs = mul.abs;
      new_mul.omod = mul.omod;
      new_mul.opsel = mul.opsel;
      new_mul.opsel_lo = mul.opsel_lo;
      new_mul.opsel_hi = mul.opsel_hi;
      if (is_abs) {
         new_mul.neg[0] = new_mul.neg[1] = false;
         new_mul.abs[0] = new_mul.abs[1] = true;
      }
      new_mul.neg[0] ^= is_neg;
      new_mul.clamp = false;

      ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
      return;
   }

   /* combine mul+add -> mad */
   bool is_add_mix =
      (instr->opcode == aco_opcode::v_fma_mix_f32 ||
       instr->opcode == aco_opcode::v_fma_mixlo_f16) &&
      !instr->valu().neg_lo[0] &&
      ((instr->operands[0].constantEquals(0x3f800000) && !instr->valu().opsel_hi[0]) ||
       (instr->operands[0].constantEquals(0x3C00) && instr->valu().opsel_hi[0] &&
        !instr->valu().opsel_lo[0]));
   bool mad32 = instr->opcode == aco_opcode::v_add_f32 || instr->opcode == aco_opcode::v_sub_f32 ||
                instr->opcode == aco_opcode::v_subrev_f32;
   bool mad16 = instr->opcode == aco_opcode::v_add_f16 || instr->opcode == aco_opcode::v_sub_f16 ||
                instr->opcode == aco_opcode::v_subrev_f16;
   bool mad64 =
      instr->opcode == aco_opcode::v_add_f64_e64 || instr->opcode == aco_opcode::v_add_f64;
   if (is_add_mix || mad16 || mad32 || mad64) {
      Instruction* mul_instr = nullptr;
      unsigned add_op_idx = 0;
      uint32_t uses = UINT32_MAX;
      bool emit_fma = false;
      /* find the 'best' mul instruction to combine with the add */
      for (unsigned i = is_add_mix ? 1 : 0; i < instr->operands.size(); i++) {
         if (!instr->operands[i].isTemp())
            continue;
         ssa_info& info = ctx.info[instr->operands[i].tempId()];
         if (!is_mul(info.parent_instr))
            continue;

         /* no clamp/omod allowed between mul and add */
         if (info.parent_instr->isVOP3() &&
             (info.parent_instr->valu().clamp || info.parent_instr->valu().omod))
            continue;
         if (info.parent_instr->isVOP3P() && info.parent_instr->valu().clamp)
            continue;
         /* v_fma_mix_f32/etc can't do omod */
         if (info.parent_instr->isVOP3P() && instr->isVOP3() && instr->valu().omod)
            continue;
         /* don't promote fp16 to fp32 or remove fp32->fp16->fp32 conversions */
         if (is_add_mix && info.parent_instr->definitions[0].bytes() == 2)
            continue;

         if (get_operand_type(instr, i).bytes() != info.parent_instr->definitions[0].bytes())
            continue;

         bool legacy = info.parent_instr->opcode == aco_opcode::v_mul_legacy_f32;
         bool mad_mix = is_add_mix || info.parent_instr->isVOP3P();

         /* Multiplication by power-of-two should never need rounding. 1/power-of-two also works,
          * but using fma removes denormal flushing (0xfffffe * 0.5 + 0x810001a2).
          */
         bool is_fma_precise = is_pow_of_two(ctx, info.parent_instr->operands[0]) ||
                               is_pow_of_two(ctx, info.parent_instr->operands[1]);

         bool has_fma = mad16 || mad64 || (legacy && ctx.program->gfx_level >= GFX10_3) ||
                        (mad32 && !legacy && !mad_mix && ctx.program->dev.has_fast_fma32) ||
                        (mad_mix && ctx.program->dev.fused_mad_mix);
         bool has_mad = mad_mix ? !ctx.program->dev.fused_mad_mix
                                : ((mad32 && ctx.program->gfx_level < GFX10_3 &&
                                    ctx.program->family != CHIP_GFX940) ||
                                   (mad16 && ctx.program->gfx_level <= GFX9));
         bool can_use_fma = has_fma && (!(info.parent_instr->definitions[0].isPrecise() ||
                                          instr->definitions[0].isPrecise()) ||
                                        is_fma_precise);
         bool can_use_mad =
            has_mad && (mad_mix || mad32 ? ctx.fp_mode.denorm32 : ctx.fp_mode.denorm16_64) == 0;
         if (mad_mix && legacy)
            continue;
         if (!can_use_fma && !can_use_mad)
            continue;

         unsigned candidate_add_op_idx = is_add_mix ? (3 - i) : (1 - i);
         Operand op[3] = {info.parent_instr->operands[0], info.parent_instr->operands[1],
                          instr->operands[candidate_add_op_idx]};
         if (info.parent_instr->isSDWA() || info.parent_instr->isDPP() ||
             !check_vop3_operands(ctx, 3, op) || ctx.uses[instr->operands[i].tempId()] > uses)
            continue;

         if (ctx.uses[instr->operands[i].tempId()] == uses) {
            unsigned cur_idx = mul_instr->definitions[0].tempId();
            unsigned new_idx = info.parent_instr->definitions[0].tempId();
            if (cur_idx > new_idx)
               continue;
         }

         mul_instr = info.parent_instr;
         add_op_idx = candidate_add_op_idx;
         uses = ctx.uses[instr->operands[i].tempId()];
         emit_fma = !can_use_mad;
      }

      if (mul_instr) {
         /* turn mul+add into v_mad/v_fma */
         Operand op[3] = {mul_instr->operands[0], mul_instr->operands[1],
                          instr->operands[add_op_idx]};
         ctx.uses[mul_instr->definitions[0].tempId()]--;
         if (ctx.uses[mul_instr->definitions[0].tempId()]) {
            if (op[0].isTemp())
               ctx.uses[op[0].tempId()]++;
            if (op[1].isTemp())
               ctx.uses[op[1].tempId()]++;
         }

         bool neg[3] = {false, false, false};
         bool abs[3] = {false, false, false};
         unsigned omod = 0;
         bool clamp = false;
         bitarray8 opsel_lo = 0;
         bitarray8 opsel_hi = 0;
         bitarray8 opsel = 0;
         unsigned mul_op_idx = (instr->isVOP3P() ? 3 : 1) - add_op_idx;

         VALU_instruction& valu_mul = mul_instr->valu();
         neg[0] = valu_mul.neg[0];
         neg[1] = valu_mul.neg[1];
         abs[0] = valu_mul.abs[0];
         abs[1] = valu_mul.abs[1];
         opsel_lo = valu_mul.opsel_lo & 0x3;
         opsel_hi = valu_mul.opsel_hi & 0x3;
         opsel = valu_mul.opsel & 0x3;

         VALU_instruction& valu = instr->valu();
         neg[2] = valu.neg[add_op_idx];
         abs[2] = valu.abs[add_op_idx];
         opsel_lo[2] = valu.opsel_lo[add_op_idx];
         opsel_hi[2] = valu.opsel_hi[add_op_idx];
         opsel[2] = valu.opsel[add_op_idx];
         opsel[3] = valu.opsel[3];
         omod = valu.omod;
         clamp = valu.clamp;
         /* abs of the multiplication result */
         if (valu.abs[mul_op_idx]) {
            neg[0] = false;
            neg[1] = false;
            abs[0] = true;
            abs[1] = true;
         }
         /* neg of the multiplication result */
         neg[1] ^= valu.neg[mul_op_idx];

         if (instr->opcode == aco_opcode::v_sub_f32 || instr->opcode == aco_opcode::v_sub_f16)
            neg[1 + add_op_idx] = neg[1 + add_op_idx] ^ true;
         else if (instr->opcode == aco_opcode::v_subrev_f32 ||
                  instr->opcode == aco_opcode::v_subrev_f16)
            neg[2 - add_op_idx] = neg[2 - add_op_idx] ^ true;

         aco_ptr<Instruction> add_instr = std::move(instr);
         aco_ptr<Instruction> mad;
         if (add_instr->isVOP3P() || mul_instr->isVOP3P()) {
            assert(!omod);
            assert(!opsel);

            aco_opcode mad_op = add_instr->definitions[0].bytes() == 2 ? aco_opcode::v_fma_mixlo_f16
                                                                       : aco_opcode::v_fma_mix_f32;
            mad.reset(create_instruction(mad_op, Format::VOP3P, 3, 1));
         } else {
            assert(!opsel_lo);
            assert(!opsel_hi);

            aco_opcode mad_op = emit_fma ? aco_opcode::v_fma_f32 : aco_opcode::v_mad_f32;
            if (mul_instr->opcode == aco_opcode::v_mul_legacy_f32) {
               assert(emit_fma == (ctx.program->gfx_level >= GFX10_3));
               mad_op = emit_fma ? aco_opcode::v_fma_legacy_f32 : aco_opcode::v_mad_legacy_f32;
            } else if (mad16) {
               mad_op = emit_fma ? (ctx.program->gfx_level == GFX8 ? aco_opcode::v_fma_legacy_f16
                                                                   : aco_opcode::v_fma_f16)
                                 : (ctx.program->gfx_level == GFX8 ? aco_opcode::v_mad_legacy_f16
                                                                   : aco_opcode::v_mad_f16);
            } else if (mad64) {
               mad_op = aco_opcode::v_fma_f64;
            }

            mad.reset(create_instruction(mad_op, Format::VOP3, 3, 1));
         }

         for (unsigned i = 0; i < 3; i++) {
            mad->operands[i] = op[i];
            mad->valu().neg[i] = neg[i];
            mad->valu().abs[i] = abs[i];
         }
         mad->valu().omod = omod;
         mad->valu().clamp = clamp;
         mad->valu().opsel_lo = opsel_lo;
         mad->valu().opsel_hi = opsel_hi;
         mad->valu().opsel = opsel;
         mad->definitions[0] = add_instr->definitions[0];
         mad->definitions[0].setPrecise(add_instr->definitions[0].isPrecise() ||
                                        mul_instr->definitions[0].isPrecise());
         mad->pass_flags = add_instr->pass_flags;

         instr = std::move(mad);

         /* mark this ssa_def to be re-checked for profitability and literals */
         ctx.mad_infos.emplace_back(std::move(add_instr), mul_instr->definitions[0].tempId());
         ctx.info[instr->definitions[0].tempId()].set_mad(ctx.mad_infos.size() - 1);
         ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
         return;
      }
   }
   /* v_mul_f32(v_cndmask_b32(0, 1.0, cond), a) -> v_cndmask_b32(0, a, cond) */
   else if (((instr->opcode == aco_opcode::v_mul_f32 && !instr->definitions[0].isNaNPreserve() &&
              !instr->definitions[0].isInfPreserve()) ||
             (instr->opcode == aco_opcode::v_mul_legacy_f32 &&
              !instr->definitions[0].isSZPreserve())) &&
            !instr->usesModifiers() && !ctx.fp_mode.must_flush_denorms32) {
      for (unsigned i = 0; i < 2; i++) {
         if (instr->operands[i].isTemp() && ctx.info[instr->operands[i].tempId()].is_b2f() &&
             ctx.uses[instr->operands[i].tempId()] == 1 && instr->operands[!i].isTemp() &&
             instr->operands[!i].getTemp().type() == RegType::vgpr) {
            ctx.uses[instr->operands[i].tempId()]--;
            ctx.uses[ctx.info[instr->operands[i].tempId()].temp.id()]++;

            aco_ptr<Instruction> new_instr{
               create_instruction(aco_opcode::v_cndmask_b32, Format::VOP2, 3, 1)};
            new_instr->operands[0] = Operand::zero();
            new_instr->operands[1] = instr->operands[!i];
            new_instr->operands[2] = Operand(ctx.info[instr->operands[i].tempId()].temp);
            new_instr->definitions[0] = instr->definitions[0];
            new_instr->pass_flags = instr->pass_flags;
            instr = std::move(new_instr);
            ctx.info[instr->definitions[0].tempId()].label = 0;
            ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
            return;
         }
      }
   } else if (instr->opcode == aco_opcode::v_or_b32 && ctx.program->gfx_level >= GFX9) {
      if (combine_three_valu_op(ctx, instr, aco_opcode::s_or_b32, aco_opcode::v_or3_b32, "012",
                                1 | 2)) {
      } else if (combine_three_valu_op(ctx, instr, aco_opcode::v_or_b32, aco_opcode::v_or3_b32,
                                       "012", 1 | 2)) {
      } else if (combine_add_or_then_and_lshl(ctx, instr)) {
      } else if (combine_v_andor_not(ctx, instr)) {
      }
   } else if (instr->opcode == aco_opcode::v_xor_b32 && ctx.program->gfx_level >= GFX10) {
      if (combine_three_valu_op(ctx, instr, aco_opcode::v_xor_b32, aco_opcode::v_xor3_b32, "012",
                                1 | 2)) {
      } else if (combine_three_valu_op(ctx, instr, aco_opcode::s_xor_b32, aco_opcode::v_xor3_b32,
                                       "012", 1 | 2)) {
      } else if (combine_xor_not(ctx, instr)) {
      }
   } else if (instr->opcode == aco_opcode::v_not_b32 && ctx.program->gfx_level >= GFX10) {
      combine_not_xor(ctx, instr);
   } else if (instr->opcode == aco_opcode::v_add_u16 && !instr->valu().clamp) {
      combine_three_valu_op(
         ctx, instr, aco_opcode::v_mul_lo_u16,
         ctx.program->gfx_level == GFX8 ? aco_opcode::v_mad_legacy_u16 : aco_opcode::v_mad_u16,
         "120", 1 | 2);
   } else if (instr->opcode == aco_opcode::v_add_u16_e64 && !instr->valu().clamp) {
      combine_three_valu_op(ctx, instr, aco_opcode::v_mul_lo_u16_e64, aco_opcode::v_mad_u16, "120",
                            1 | 2);
   } else if (instr->opcode == aco_opcode::v_add_u32 && !instr->usesModifiers()) {
      if (combine_add_sub_b2i(ctx, instr, aco_opcode::v_addc_co_u32, 1 | 2)) {
      } else if (combine_add_bcnt(ctx, instr)) {
      } else if (combine_three_valu_op(ctx, instr, aco_opcode::v_mul_u32_u24,
                                       aco_opcode::v_mad_u32_u24, "120", 1 | 2)) {
      } else if (combine_three_valu_op(ctx, instr, aco_opcode::v_mul_i32_i24,
                                       aco_opcode::v_mad_i32_i24, "120", 1 | 2)) {
      } else if (ctx.program->gfx_level >= GFX9) {
         if (combine_three_valu_op(ctx, instr, aco_opcode::s_xor_b32, aco_opcode::v_xad_u32, "120",
                                   1 | 2)) {
         } else if (combine_three_valu_op(ctx, instr, aco_opcode::v_xor_b32, aco_opcode::v_xad_u32,
                                          "120", 1 | 2)) {
         } else if (combine_three_valu_op(ctx, instr, aco_opcode::s_add_i32, aco_opcode::v_add3_u32,
                                          "012", 1 | 2)) {
         } else if (combine_three_valu_op(ctx, instr, aco_opcode::s_add_u32, aco_opcode::v_add3_u32,
                                          "012", 1 | 2)) {
         } else if (combine_three_valu_op(ctx, instr, aco_opcode::v_add_u32, aco_opcode::v_add3_u32,
                                          "012", 1 | 2)) {
         } else if (combine_add_or_then_and_lshl(ctx, instr)) {
         }
      }
   } else if ((instr->opcode == aco_opcode::v_add_co_u32 ||
               instr->opcode == aco_opcode::v_add_co_u32_e64) &&
              !instr->usesModifiers()) {
      bool carry_out = ctx.uses[instr->definitions[1].tempId()] > 0;
      if (combine_add_sub_b2i(ctx, instr, aco_opcode::v_addc_co_u32, 1 | 2)) {
      } else if (!carry_out && combine_add_bcnt(ctx, instr)) {
      } else if (!carry_out && combine_three_valu_op(ctx, instr, aco_opcode::v_mul_u32_u24,
                                                     aco_opcode::v_mad_u32_u24, "120", 1 | 2)) {
      } else if (!carry_out && combine_three_valu_op(ctx, instr, aco_opcode::v_mul_i32_i24,
                                                     aco_opcode::v_mad_i32_i24, "120", 1 | 2)) {
      } else if (!carry_out && combine_add_lshl(ctx, instr, false)) {
      }
   } else if (instr->opcode == aco_opcode::v_sub_u32 || instr->opcode == aco_opcode::v_sub_co_u32 ||
              instr->opcode == aco_opcode::v_sub_co_u32_e64) {
      bool carry_out =
         instr->opcode != aco_opcode::v_sub_u32 && ctx.uses[instr->definitions[1].tempId()] > 0;
      if (combine_add_sub_b2i(ctx, instr, aco_opcode::v_subbrev_co_u32, 2)) {
      } else if (!carry_out && combine_add_lshl(ctx, instr, true)) {
      }
   } else if (instr->opcode == aco_opcode::v_subrev_u32 ||
              instr->opcode == aco_opcode::v_subrev_co_u32 ||
              instr->opcode == aco_opcode::v_subrev_co_u32_e64) {
      combine_add_sub_b2i(ctx, instr, aco_opcode::v_subbrev_co_u32, 1);
   } else if (instr->opcode == aco_opcode::v_lshlrev_b32 && ctx.program->gfx_level >= GFX9) {
      combine_three_valu_op(ctx, instr, aco_opcode::v_add_u32, aco_opcode::v_add_lshl_u32, "120",
                            2);
   } else if ((instr->opcode == aco_opcode::s_add_u32 || instr->opcode == aco_opcode::s_add_i32) &&
              ctx.program->gfx_level >= GFX9) {
      combine_salu_lshl_add(ctx, instr);
   } else if (instr->opcode == aco_opcode::s_not_b32 || instr->opcode == aco_opcode::s_not_b64) {
      if (!combine_salu_not_bitwise(ctx, instr))
         combine_inverse_comparison(ctx, instr);
   } else if (instr->opcode == aco_opcode::s_and_b32 || instr->opcode == aco_opcode::s_or_b32 ||
              instr->opcode == aco_opcode::s_and_b64 || instr->opcode == aco_opcode::s_or_b64) {
      combine_salu_n2(ctx, instr);
   } else if (instr->opcode == aco_opcode::s_abs_i32) {
      combine_sabsdiff(ctx, instr);
   } else if (instr->opcode == aco_opcode::v_and_b32) {
      combine_v_andor_not(ctx, instr);
   } else if (instr->opcode == aco_opcode::v_fma_f32 || instr->opcode == aco_opcode::v_fma_f16) {
      /* set existing v_fma_f32 with label_mad so we can create v_fmamk_f32/v_fmaak_f32.
       * since ctx.uses[mad_info::mul_temp_id] is always 0, we don't have to worry about
       * select_instruction() using mad_info::add_instr.
       */
      ctx.mad_infos.emplace_back(nullptr, 0);
      ctx.info[instr->definitions[0].tempId()].set_mad(ctx.mad_infos.size() - 1);
   } else if (instr->opcode == aco_opcode::v_med3_f32 || instr->opcode == aco_opcode::v_med3_f16) {
      /* Optimize v_med3 to v_add so that it can be dual issued on GFX11. We start with v_med3 in
       * case omod can be applied.
       */
      unsigned idx;
      if (detect_clamp(instr.get(), &idx)) {
         instr->format = asVOP3(Format::VOP2);
         instr->operands[0] = instr->operands[idx];
         instr->operands[1] = Operand::zero();
         instr->opcode =
            instr->opcode == aco_opcode::v_med3_f32 ? aco_opcode::v_add_f32 : aco_opcode::v_add_f16;
         instr->valu().clamp = true;
         instr->valu().abs = (uint8_t)instr->valu().abs[idx];
         instr->valu().neg = (uint8_t)instr->valu().neg[idx];
         instr->operands.pop_back();
      }
   } else {
      aco_opcode min, max, min3, max3, med3, minmax;
      bool some_gfx9_only;
      if (get_minmax_info(instr->opcode, &min, &max, &min3, &max3, &med3, &minmax,
                          &some_gfx9_only) &&
          (!some_gfx9_only || ctx.program->gfx_level >= GFX9)) {
         if (combine_minmax(ctx, instr, instr->opcode == min ? max : min,
                            instr->opcode == min ? min3 : max3, minmax)) {
         } else {
            combine_clamp(ctx, instr, min, max, med3);
         }
      }
   }
}

struct remat_entry {
   Instruction* instr;
   uint32_t block;
};

inline bool
is_constant(Instruction* instr)
{
   if (instr->opcode != aco_opcode::p_parallelcopy || instr->operands.size() != 1)
      return false;

   return instr->operands[0].isConstant() && instr->definitions[0].isTemp();
}

void
remat_constants_instr(opt_ctx& ctx, aco::map<Temp, remat_entry>& constants, Instruction* instr,
                      uint32_t block_idx)
{
   for (Operand& op : instr->operands) {
      if (!op.isTemp())
         continue;

      auto it = constants.find(op.getTemp());
      if (it == constants.end())
         continue;

      /* Check if we already emitted the same constant in this block. */
      if (it->second.block != block_idx) {
         /* Rematerialize the constant. */
         Builder bld(ctx.program, &ctx.instructions);
         Operand const_op = it->second.instr->operands[0];
         it->second.instr = bld.copy(bld.def(op.regClass()), const_op);
         it->second.block = block_idx;
         ctx.uses.push_back(0);
         ctx.info.push_back(ctx.info[op.tempId()]);
         ctx.info[it->second.instr->definitions[0].tempId()].parent_instr = it->second.instr;
      }

      /* Use the rematerialized constant and update information about latest use. */
      if (op.getTemp() != it->second.instr->definitions[0].getTemp()) {
         ctx.uses[op.tempId()]--;
         op.setTemp(it->second.instr->definitions[0].getTemp());
         ctx.uses[op.tempId()]++;
      }
   }
}

/**
 * This pass implements a simple constant rematerialization.
 * As common subexpression elimination (CSE) might increase the live-ranges
 * of loaded constants over large distances, this pass splits the live-ranges
 * again by re-emitting constants in every basic block.
 */
void
rematerialize_constants(opt_ctx& ctx)
{
   aco::monotonic_buffer_resource memory(1024);
   aco::map<Temp, remat_entry> constants(memory);

   for (Block& block : ctx.program->blocks) {
      if (block.logical_idom == -1)
         continue;

      if (block.logical_idom == (int)block.index)
         constants.clear();

      ctx.instructions.reserve(block.instructions.size());

      for (aco_ptr<Instruction>& instr : block.instructions) {
         if (is_dead(ctx.uses, instr.get()))
            continue;

         if (is_constant(instr.get())) {
            Temp tmp = instr->definitions[0].getTemp();
            constants[tmp] = {instr.get(), block.index};
         } else if (!is_phi(instr)) {
            remat_constants_instr(ctx, constants, instr.get(), block.index);
         }

         ctx.instructions.emplace_back(instr.release());
      }

      block.instructions = std::move(ctx.instructions);
   }
}

bool
to_uniform_bool_instr(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   /* Check every operand to make sure they are suitable. */
   for (Operand& op : instr->operands) {
      if (!op.isTemp())
         return false;
      if (!ctx.info[op.tempId()].is_uniform_bool() && !ctx.info[op.tempId()].is_uniform_bitwise())
         return false;
   }

   switch (instr->opcode) {
   case aco_opcode::s_and_b32:
   case aco_opcode::s_and_b64: instr->opcode = aco_opcode::s_and_b32; break;
   case aco_opcode::s_or_b32:
   case aco_opcode::s_or_b64: instr->opcode = aco_opcode::s_or_b32; break;
   case aco_opcode::s_xor_b32:
   case aco_opcode::s_xor_b64: instr->opcode = aco_opcode::s_absdiff_i32; break;
   case aco_opcode::s_not_b32:
   case aco_opcode::s_not_b64: {
      aco_ptr<Instruction> new_instr{
         create_instruction(aco_opcode::s_absdiff_i32, Format::SOP2, 2, 2)};
      new_instr->operands[0] = instr->operands[0];
      new_instr->operands[1] = Operand::c32(1);
      new_instr->definitions[0] = instr->definitions[0];
      new_instr->definitions[1] = instr->definitions[1];
      new_instr->pass_flags = instr->pass_flags;
      instr = std::move(new_instr);
      ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
      ctx.info[instr->definitions[1].tempId()].parent_instr = instr.get();
      break;
   }
   default:
      /* Don't transform other instructions. They are very unlikely to appear here. */
      return false;
   }

   for (Operand& op : instr->operands) {
      if (!op.isTemp())
         continue;

      ctx.uses[op.tempId()]--;

      if (ctx.info[op.tempId()].is_uniform_bool()) {
         /* Just use the uniform boolean temp. */
         op.setTemp(ctx.info[op.tempId()].temp);
      } else if (ctx.info[op.tempId()].is_uniform_bitwise()) {
         /* Use the SCC definition of the predecessor instruction.
          * This allows the predecessor to get picked up by the same optimization (if it has no
          * divergent users), and it also makes sure that the current instruction will keep working
          * even if the predecessor won't be transformed.
          */
         Instruction* pred_instr = ctx.info[op.tempId()].parent_instr;
         assert(pred_instr->definitions.size() >= 2);
         assert(pred_instr->definitions[1].isFixed() &&
                pred_instr->definitions[1].physReg() == scc);
         op.setTemp(pred_instr->definitions[1].getTemp());
      } else {
         UNREACHABLE("Invalid operand on uniform bitwise instruction.");
      }

      ctx.uses[op.tempId()]++;
   }

   instr->definitions[0].setTemp(Temp(instr->definitions[0].tempId(), s1));
   ctx.program->temp_rc[instr->definitions[0].tempId()] = s1;
   assert(!instr->operands[0].isTemp() || instr->operands[0].regClass() == s1);
   assert(!instr->operands[1].isTemp() || instr->operands[1].regClass() == s1);
   return true;
}

void
select_instruction(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   const uint32_t threshold = 4;

   if (is_dead(ctx.uses, instr.get())) {
      instr.reset();
      return;
   }

   /* convert split_vector into a copy or extract_vector if only one definition is ever used */
   if (instr->opcode == aco_opcode::p_split_vector) {
      unsigned num_used = 0;
      unsigned idx = 0;
      unsigned split_offset = 0;
      for (unsigned i = 0, offset = 0; i < instr->definitions.size();
           offset += instr->definitions[i++].bytes()) {
         if (ctx.uses[instr->definitions[i].tempId()]) {
            num_used++;
            idx = i;
            split_offset = offset;
         }
      }
      bool done = false;
      Instruction* vec = ctx.info[instr->operands[0].tempId()].parent_instr;
      if (num_used == 1 && vec->opcode == aco_opcode::p_create_vector &&
          ctx.uses[instr->operands[0].tempId()] == 1) {

         unsigned off = 0;
         Operand op;
         for (Operand& vec_op : vec->operands) {
            if (off == split_offset) {
               op = vec_op;
               break;
            }
            off += vec_op.bytes();
         }
         if (off != instr->operands[0].bytes() && op.bytes() == instr->definitions[idx].bytes()) {
            ctx.uses[instr->operands[0].tempId()]--;
            for (Operand& vec_op : vec->operands) {
               if (vec_op.isTemp())
                  ctx.uses[vec_op.tempId()]--;
            }
            if (op.isTemp())
               ctx.uses[op.tempId()]++;

            aco_ptr<Instruction> copy{
               create_instruction(aco_opcode::p_parallelcopy, Format::PSEUDO, 1, 1)};
            copy->operands[0] = op;
            copy->definitions[0] = instr->definitions[idx];
            instr = std::move(copy);
            ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();

            done = true;
         }
      }

      if (!done && num_used == 1 &&
          instr->operands[0].bytes() % instr->definitions[idx].bytes() == 0 &&
          split_offset % instr->definitions[idx].bytes() == 0) {
         aco_ptr<Instruction> extract{
            create_instruction(aco_opcode::p_extract_vector, Format::PSEUDO, 2, 1)};
         extract->operands[0] = instr->operands[0];
         extract->operands[1] =
            Operand::c32((uint32_t)split_offset / instr->definitions[idx].bytes());
         extract->definitions[0] = instr->definitions[idx];
         instr = std::move(extract);
         ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
      }
   }

   mad_info* mad_info = NULL;
   if (!instr->definitions.empty() && ctx.info[instr->definitions[0].tempId()].is_mad()) {
      mad_info = &ctx.mad_infos[ctx.info[instr->definitions[0].tempId()].val];
      /* re-check mad instructions */
      if (ctx.uses[mad_info->mul_temp_id] && mad_info->add_instr) {
         ctx.uses[mad_info->mul_temp_id]++;
         if (instr->operands[0].isTemp())
            ctx.uses[instr->operands[0].tempId()]--;
         if (instr->operands[1].isTemp())
            ctx.uses[instr->operands[1].tempId()]--;
         instr.swap(mad_info->add_instr);
         ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
         mad_info = NULL;
      }
   }

   /* Mark SCC needed, so the uniform boolean transformation won't swap the definitions
    * when it isn't beneficial */
   if (instr->isBranch() && instr->operands.size() && instr->operands[0].isTemp() &&
       instr->operands[0].isFixed() && instr->operands[0].physReg() == scc) {
      ctx.info[instr->operands[0].tempId()].set_scc_needed();
      return;
   } else if ((instr->opcode == aco_opcode::s_cselect_b64 ||
               instr->opcode == aco_opcode::s_cselect_b32) &&
              instr->operands[2].isTemp()) {
      ctx.info[instr->operands[2].tempId()].set_scc_needed();
   }

   /* check for literals */
   if (!instr->isSALU() && !instr->isVALU())
      return;

   /* Transform uniform bitwise boolean operations to 32-bit when there are no divergent uses. */
   if (instr->definitions.size() && ctx.uses[instr->definitions[0].tempId()] == 0 &&
       ctx.info[instr->definitions[0].tempId()].is_uniform_bitwise()) {
      bool transform_done = to_uniform_bool_instr(ctx, instr);

      if (transform_done && !ctx.info[instr->definitions[1].tempId()].is_scc_needed()) {
         /* Swap the two definition IDs in order to avoid overusing the SCC.
          * This reduces extra moves generated by RA. */
         uint32_t def0_id = instr->definitions[0].getTemp().id();
         uint32_t def1_id = instr->definitions[1].getTemp().id();
         instr->definitions[0].setTemp(Temp(def1_id, s1));
         instr->definitions[1].setTemp(Temp(def0_id, s1));
      }

      return;
   }

   /* This optimization is done late in order to be able to apply otherwise
    * unsafe optimizations such as the inverse comparison optimization.
    */
   if (instr->opcode == aco_opcode::s_and_b32 || instr->opcode == aco_opcode::s_and_b64) {
      if (instr->operands[0].isTemp() && fixed_to_exec(instr->operands[1]) &&
          ctx.uses[instr->operands[0].tempId()] == 1 &&
          ctx.uses[instr->definitions[1].tempId()] == 0 &&
          can_eliminate_and_exec(ctx, instr->operands[0].getTemp(), instr->pass_flags, true)) {
         ctx.uses[instr->operands[0].tempId()]--;
         Instruction* op_instr = ctx.info[instr->operands[0].tempId()].parent_instr;

         if (op_instr->opcode == aco_opcode::s_cselect_b32 ||
             op_instr->opcode == aco_opcode::s_cselect_b64) {
            for (unsigned i = 0; i < 2; i++) {
               if (op_instr->operands[i].constantEquals(-1))
                  op_instr->operands[i] = instr->operands[1];
            }
            ctx.info[op_instr->definitions[0].tempId()].label &= label_uniform_bool;
         }

         op_instr->definitions[0].setTemp(instr->definitions[0].getTemp());
         ctx.info[op_instr->definitions[0].tempId()].parent_instr = op_instr;
         instr.reset();
         return;
      }
   }

   /* Combine DPP copies into VALU. This should be done after creating MAD/FMA. */
   if (instr->isVALU() && !instr->isDPP()) {
      for (unsigned i = 0; i < instr->operands.size(); i++) {
         if (!instr->operands[i].isTemp())
            continue;
         ssa_info info = ctx.info[instr->operands[i].tempId()];

         if (!info.parent_instr->isDPP() || info.parent_instr->opcode != aco_opcode::v_mov_b32 ||
             info.parent_instr->pass_flags != instr->pass_flags)
            continue;

         /* We won't eliminate the DPP mov if the operand is used twice */
         bool op_used_twice = false;
         for (unsigned j = 0; j < instr->operands.size(); j++)
            op_used_twice |= i != j && instr->operands[i] == instr->operands[j];
         if (op_used_twice)
            continue;

         if (i != 0) {
            if (!can_swap_operands(instr, &instr->opcode, 0, i))
               continue;
            instr->valu().swapOperands(0, i);
         }

         bool dpp8 = info.parent_instr->isDPP8();
         if (!can_use_DPP(ctx.program->gfx_level, instr, dpp8))
            continue;

         bool input_mods = can_use_input_modifiers(ctx.program->gfx_level, instr->opcode, 0) &&
                           get_operand_type(instr, 0).bit_size == 32;
         bool mov_uses_mods = info.parent_instr->valu().neg[0] || info.parent_instr->valu().abs[0];
         if (((dpp8 && ctx.program->gfx_level < GFX11) || !input_mods) && mov_uses_mods)
            continue;

         convert_to_DPP(ctx.program->gfx_level, instr, dpp8);

         if (dpp8) {
            DPP8_instruction* dpp = &instr->dpp8();
            dpp->lane_sel = info.parent_instr->dpp8().lane_sel;
            dpp->fetch_inactive = info.parent_instr->dpp8().fetch_inactive;
            if (mov_uses_mods)
               instr->format = asVOP3(instr->format);
         } else {
            DPP16_instruction* dpp = &instr->dpp16();
            /* anything else doesn't make sense in SSA */
            assert(info.parent_instr->dpp16().row_mask == 0xf &&
                   info.parent_instr->dpp16().bank_mask == 0xf);
            dpp->dpp_ctrl = info.parent_instr->dpp16().dpp_ctrl;
            dpp->bound_ctrl = info.parent_instr->dpp16().bound_ctrl;
            dpp->fetch_inactive = info.parent_instr->dpp16().fetch_inactive;
         }

         instr->valu().neg[0] ^= info.parent_instr->valu().neg[0] && !instr->valu().abs[0];
         instr->valu().abs[0] |= info.parent_instr->valu().abs[0];

         if (--ctx.uses[info.parent_instr->definitions[0].tempId()])
            ctx.uses[info.parent_instr->operands[0].tempId()]++;
         instr->operands[0].setTemp(info.parent_instr->operands[0].getTemp());
         for (const Definition& def : instr->definitions)
            ctx.info[def.tempId()].parent_instr = instr.get();
         break;
      }
   }

   /* Use v_fma_mix for f2f32/f2f16 if it has higher throughput.
    * Do this late to not disturb other optimizations.
    */
   if ((instr->opcode == aco_opcode::v_cvt_f32_f16 || instr->opcode == aco_opcode::v_cvt_f16_f32) &&
       ctx.program->gfx_level >= GFX11 && ctx.program->wave_size == 64 && !instr->valu().omod &&
       !instr->isDPP()) {
      bool is_f2f16 = instr->opcode == aco_opcode::v_cvt_f16_f32;
      Instruction* fma = create_instruction(
         is_f2f16 ? aco_opcode::v_fma_mixlo_f16 : aco_opcode::v_fma_mix_f32, Format::VOP3P, 3, 1);
      fma->definitions[0] = instr->definitions[0];
      fma->operands[0] = instr->operands[0];
      fma->valu().opsel_hi[0] = !is_f2f16;
      fma->valu().opsel_lo[0] = instr->valu().opsel[0];
      fma->valu().clamp = instr->valu().clamp;
      fma->valu().abs[0] = instr->valu().abs[0];
      fma->valu().neg[0] = instr->valu().neg[0];
      fma->operands[1] = Operand::c32(fui(1.0f));
      fma->operands[2] = Operand::zero();
      fma->valu().neg[2] = true;
      instr.reset(fma);
      ctx.info[instr->definitions[0].tempId()].label = 0;
      ctx.info[instr->definitions[0].tempId()].parent_instr = instr.get();
   }

   /* Check operands for whether we can apply constants or literals. */
   if (std::none_of(instr->operands.begin(), instr->operands.end(),
                    [&](const Operand& op)
                    {
                       if (!op.isTemp() || op.isFixed())
                          return false;
                       auto& temp_info = ctx.info[op.tempId()];
                       return temp_info.is_constant_or_literal(op.size() * 32);
                    }))
      return;

   alu_opt_info input_info;
   if (!alu_opt_gather_info(ctx, instr.get(), input_info))
      return;

   unsigned literal_mask = 0;
   for (unsigned i = 0; i < input_info.operands.size(); i++) {
      Operand op = input_info.operands[i].op;
      if (!op.isTemp() || op.isFixed())
         continue;
      auto& temp_info = ctx.info[op.tempId()];
      if (temp_info.is_constant_or_literal(op.size() * 32))
         literal_mask |= BITFIELD_BIT(i);
   }

   alu_opt_info lit_info;
   bool force_create = false;
   unsigned lit_uses = threshold;
   for (unsigned sub_mask = (~literal_mask + 1) & literal_mask; sub_mask;
        sub_mask = ((sub_mask | ~literal_mask) + 1) & literal_mask) {
      alu_opt_info candidate = input_info;
      unsigned candidate_uses = UINT32_MAX;
      u_foreach_bit (i, sub_mask) {
         uint32_t tmpid = candidate.operands[i].op.tempId();
         candidate.operands[i].op = Operand::literal32(ctx.info[tmpid].val);
         candidate_uses = MIN2(candidate_uses, ctx.uses[tmpid]);
      }
      if (!alu_opt_info_is_valid(ctx, candidate))
         continue;

      switch (candidate.opcode) {
      case aco_opcode::v_fmaak_f32:
      case aco_opcode::v_fmaak_f16:
      case aco_opcode::v_madak_f32:
      case aco_opcode::v_madak_f16:
         /* This instruction won't be able to use fmac, so fmaak doesn't regress code size. */
         force_create = true;
         break;
      default: break;
      }

      if (!force_create && util_bitcount(sub_mask) <= 1 && candidate_uses >= lit_uses)
         continue;
      lit_info = candidate;
      lit_uses = candidate_uses;

      if (util_bitcount(sub_mask) > 1) {
         force_create = true;
         break;
      }
   }
   if (!lit_info.operands.size())
      return;

   for (const auto& op_info : lit_info.operands) {
      if (op_info.op.isTemp())
         ctx.uses[op_info.op.tempId()]++;
   }
   for (Operand op : instr->operands) {
      if (op.isTemp())
         decrease_and_dce(ctx, op.getTemp());
   }
   if (force_create || lit_uses == 1)
      instr.reset(alu_opt_info_to_instr(ctx, lit_info, instr.release()));
}

static aco_opcode
sopk_opcode_for_sopc(aco_opcode opcode)
{
#define CTOK(op)                                                                                   \
   case aco_opcode::s_cmp_##op##_i32: return aco_opcode::s_cmpk_##op##_i32;                        \
   case aco_opcode::s_cmp_##op##_u32: return aco_opcode::s_cmpk_##op##_u32;
   switch (opcode) {
      CTOK(eq)
      CTOK(lg)
      CTOK(gt)
      CTOK(ge)
      CTOK(lt)
      CTOK(le)
   default: return aco_opcode::num_opcodes;
   }
#undef CTOK
}

static bool
sopc_is_signed(aco_opcode opcode)
{
#define SOPC(op)                                                                                   \
   case aco_opcode::s_cmp_##op##_i32: return true;                                                 \
   case aco_opcode::s_cmp_##op##_u32: return false;
   switch (opcode) {
      SOPC(eq)
      SOPC(lg)
      SOPC(gt)
      SOPC(ge)
      SOPC(lt)
      SOPC(le)
   default: UNREACHABLE("Not a valid SOPC instruction.");
   }
#undef SOPC
}

static aco_opcode
sopc_32_swapped(aco_opcode opcode)
{
#define SOPC(op1, op2)                                                                             \
   case aco_opcode::s_cmp_##op1##_i32: return aco_opcode::s_cmp_##op2##_i32;                       \
   case aco_opcode::s_cmp_##op1##_u32: return aco_opcode::s_cmp_##op2##_u32;
   switch (opcode) {
      SOPC(eq, eq)
      SOPC(lg, lg)
      SOPC(gt, lt)
      SOPC(ge, le)
      SOPC(lt, gt)
      SOPC(le, ge)
   default: return aco_opcode::num_opcodes;
   }
#undef SOPC
}

static void
try_convert_sopc_to_sopk(aco_ptr<Instruction>& instr)
{
   if (sopk_opcode_for_sopc(instr->opcode) == aco_opcode::num_opcodes)
      return;

   if (instr->operands[0].isLiteral()) {
      std::swap(instr->operands[0], instr->operands[1]);
      instr->opcode = sopc_32_swapped(instr->opcode);
   }

   if (!instr->operands[1].isLiteral())
      return;

   if (instr->operands[0].isFixed() && instr->operands[0].physReg() >= 128)
      return;

   uint32_t value = instr->operands[1].constantValue();

   const uint32_t i16_mask = 0xffff8000u;

   bool value_is_i16 = (value & i16_mask) == 0 || (value & i16_mask) == i16_mask;
   bool value_is_u16 = !(value & 0xffff0000u);

   if (!value_is_i16 && !value_is_u16)
      return;

   if (!value_is_i16 && sopc_is_signed(instr->opcode)) {
      if (instr->opcode == aco_opcode::s_cmp_lg_i32)
         instr->opcode = aco_opcode::s_cmp_lg_u32;
      else if (instr->opcode == aco_opcode::s_cmp_eq_i32)
         instr->opcode = aco_opcode::s_cmp_eq_u32;
      else
         return;
   } else if (!value_is_u16 && !sopc_is_signed(instr->opcode)) {
      if (instr->opcode == aco_opcode::s_cmp_lg_u32)
         instr->opcode = aco_opcode::s_cmp_lg_i32;
      else if (instr->opcode == aco_opcode::s_cmp_eq_u32)
         instr->opcode = aco_opcode::s_cmp_eq_i32;
      else
         return;
   }

   instr->format = Format::SOPK;
   SALU_instruction* instr_sopk = &instr->salu();

   instr_sopk->imm = instr_sopk->operands[1].constantValue() & 0xffff;
   instr_sopk->opcode = sopk_opcode_for_sopc(instr_sopk->opcode);
   instr_sopk->operands.pop_back();
}

static void
opt_fma_mix_acc(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   /* fma_mix is only dual issued on gfx11 if dst and acc type match */
   bool f2f16 = instr->opcode == aco_opcode::v_fma_mixlo_f16;

   if (instr->valu().opsel_hi[2] == f2f16 || instr->isDPP())
      return;

   bool is_add = false;
   for (unsigned i = 0; i < 2; i++) {
      uint32_t one = instr->valu().opsel_hi[i] ? 0x3800 : 0x3f800000;
      is_add = instr->operands[i].constantEquals(one) && !instr->valu().neg[i] &&
               !instr->valu().opsel_lo[i];
      if (is_add) {
         instr->valu().swapOperands(0, i);
         break;
      }
   }

   if (is_add && instr->valu().opsel_hi[1] == f2f16) {
      instr->valu().swapOperands(1, 2);
      return;
   }

   unsigned literal_count = instr->operands[0].isLiteral() + instr->operands[1].isLiteral() +
                            instr->operands[2].isLiteral();

   if (!f2f16 || literal_count > 1)
      return;

   /* try to convert constant operand to fp16 */
   for (unsigned i = 2 - is_add; i < 3; i++) {
      if (!instr->operands[i].isConstant())
         continue;

      float value = uif(instr->operands[i].constantValue());
      uint16_t fp16_val = _mesa_float_to_half(value);
      bool is_denorm = (fp16_val & 0x7fff) != 0 && (fp16_val & 0x7fff) <= 0x3ff;

      if (_mesa_half_to_float(fp16_val) != value ||
          (is_denorm && !(ctx.fp_mode.denorm16_64 & fp_denorm_keep_in)))
         continue;

      instr->valu().swapOperands(i, 2);

      Operand op16 = Operand::c16(fp16_val);
      assert(!op16.isLiteral() || instr->operands[2].isLiteral());

      instr->operands[2] = op16;
      instr->valu().opsel_lo[2] = false;
      instr->valu().opsel_hi[2] = true;
      return;
   }
}

void
apply_literals(opt_ctx& ctx, aco_ptr<Instruction>& instr)
{
   /* Cleanup Dead Instructions */
   if (!instr)
      return;

   /* apply literals on SALU/VALU */
   if (instr->isSALU() || instr->isVALU()) {
      for (const Operand& op : instr->operands) {
         if (op.isTemp() && ctx.info[op.tempId()].is_literal(op.size() * 32) &&
             ctx.uses[op.tempId()] == 0) {
            alu_opt_info info;
            if (!alu_opt_gather_info(ctx, instr.get(), info))
               UNREACHABLE("We already check that we can apply lit");

            for (auto& op_info : info.operands) {
               if (op_info.op == op)
                  op_info.op = Operand::literal32(ctx.info[op.tempId()].val);
            }

            if (!alu_opt_info_is_valid(ctx, info))
               UNREACHABLE("We already check that we can apply lit");
            instr.reset(alu_opt_info_to_instr(ctx, info, instr.release()));
            break;
         }
      }
   }

   if (instr->isSOPC() && ctx.program->gfx_level < GFX12)
      try_convert_sopc_to_sopk(instr);

   if (instr->opcode == aco_opcode::v_fma_mixlo_f16 || instr->opcode == aco_opcode::v_fma_mix_f32)
      opt_fma_mix_acc(ctx, instr);

   ctx.instructions.emplace_back(std::move(instr));
}

void
validate_opt_ctx(opt_ctx& ctx)
{
   if (!(debug_flags & DEBUG_VALIDATE_OPT))
      return;

   Program* program = ctx.program;

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

         fprintf(memf, "Optimizer: %s: ", msg);
         aco_print_instr(program->gfx_level, instr, memf);
         u_memstream_close(&mem);

         aco_err(program, "%s", out);
         free(out);

         is_valid = false;
      }
   };

   for (Block& block : program->blocks) {
      for (aco_ptr<Instruction>& instr : block.instructions) {
         if (!instr)
            continue;
         for (const Definition& def : instr->definitions) {
            check(ctx.info[def.tempId()].parent_instr == instr.get(), "parent_instr incorrect",
                  instr.get());
         }
      }
   }
   if (!is_valid) {
      abort();
   }
}

void rename_loop_header_phis(opt_ctx& ctx) {
   for (Block& block : ctx.program->blocks) {
      if (!(block.kind & block_kind_loop_header))
         continue;

      for (auto& instr : block.instructions) {
         if (!is_phi(instr))
            break;

         for (unsigned i = 0; i < instr->operands.size(); i++) {
            if (!instr->operands[i].isTemp())
               continue;

            ssa_info info = ctx.info[instr->operands[i].tempId()];
            while (info.is_temp()) {
               pseudo_propagate_temp(ctx, instr, info.temp, i);
               info = ctx.info[info.temp.id()];
            }
         }
      }
   }
}

} /* end namespace */

void
optimize(Program* program)
{
   opt_ctx ctx;
   ctx.program = program;
   ctx.info = std::vector<ssa_info>(program->peekAllocationId());

   /* 1. Bottom-Up DAG pass (forward) to label all ssa-defs */
   for (Block& block : program->blocks) {
      ctx.fp_mode = block.fp_mode;
      for (aco_ptr<Instruction>& instr : block.instructions)
         label_instruction(ctx, instr);
   }

   validate_opt_ctx(ctx);

   rename_loop_header_phis(ctx);

   validate_opt_ctx(ctx);

   ctx.uses = dead_code_analysis(program);

   /* 2. Rematerialize constants in every block. */
   rematerialize_constants(ctx);

   validate_opt_ctx(ctx);

   /* 3. Combine v_mad, omod, clamp and propagate sgpr on VALU instructions */
   for (Block& block : program->blocks) {
      ctx.fp_mode = block.fp_mode;
      for (aco_ptr<Instruction>& instr : block.instructions)
         combine_instruction(ctx, instr);
   }

   validate_opt_ctx(ctx);

   /* 4. Top-Down DAG pass (backward) to select instructions (includes DCE) */
   for (auto block_rit = program->blocks.rbegin(); block_rit != program->blocks.rend();
        ++block_rit) {
      Block* block = &(*block_rit);
      ctx.fp_mode = block->fp_mode;
      for (auto instr_rit = block->instructions.rbegin(); instr_rit != block->instructions.rend();
           ++instr_rit)
         select_instruction(ctx, *instr_rit);
   }

   validate_opt_ctx(ctx);

   /* 5. Add literals to instructions */
   for (Block& block : program->blocks) {
      ctx.instructions.reserve(block.instructions.size());
      ctx.fp_mode = block.fp_mode;
      for (aco_ptr<Instruction>& instr : block.instructions)
         apply_literals(ctx, instr);
      block.instructions = std::move(ctx.instructions);
   }

   validate_opt_ctx(ctx);
}

} // namespace aco
