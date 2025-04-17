// Copyright © 2025 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use crate::ir::*;
use crate::legalize::{
    src_is_reg, swap_srcs_if_not_reg, LegalizeBuildHelpers, LegalizeBuilder,
};
use bitview::{
    BitMutView, BitMutViewable, BitView, BitViewable, SetBit, SetField,
    SetFieldU64,
};

use std::fmt;
use std::ops::Range;

pub struct ShaderModel20 {
    sm: u8,
}

impl ShaderModel20 {
    pub fn new(sm: u8) -> Self {
        assert!(sm >= 20 && sm < 32);
        Self { sm }
    }
}

impl ShaderModel for ShaderModel20 {
    fn sm(&self) -> u8 {
        self.sm
    }

    fn num_regs(&self, file: RegFile) -> u32 {
        match file {
            RegFile::GPR => 63,
            RegFile::UGPR => 0,
            RegFile::Pred => 7,
            RegFile::UPred => 0,
            RegFile::Carry => 1,
            RegFile::Bar => 0,
            RegFile::Mem => RegRef::MAX_IDX + 1,
        }
    }

    fn hw_reserved_gprs(&self) -> u32 {
        0
    }

    fn crs_size(&self, max_crs_depth: u32) -> u32 {
        if max_crs_depth <= 16 {
            0
        } else if max_crs_depth <= 32 {
            1024
        } else {
            ((max_crs_depth + 32) * 16).next_multiple_of(512)
        }
    }

    fn op_can_be_uniform(&self, _op: &Op) -> bool {
        false
    }

    fn exec_latency(&self, _op: &Op) -> u32 {
        1
    }

    fn raw_latency(
        &self,
        _write: &Op,
        _dst_idx: usize,
        _read: &Op,
        _src_idx: usize,
    ) -> u32 {
        // TODO
        13
    }

    fn war_latency(
        &self,
        _read: &Op,
        _src_idx: usize,
        _write: &Op,
        _dst_idx: usize,
    ) -> u32 {
        // TODO
        // We assume the source gets read in the first 4 cycles.  We don't know
        // how quickly the write will happen.  This is all a guess.
        4
    }

    fn waw_latency(
        &self,
        _a: &Op,
        _a_dst_idx: usize,
        _a_has_pred: bool,
        _b: &Op,
        _b_dst_idx: usize,
    ) -> u32 {
        // We know our latencies are wrong so assume the wrote could happen
        // anywhere between 0 and instr_latency(a) cycles

        // TODO
        13
    }

    fn paw_latency(&self, _write: &Op, _dst_idx: usize) -> u32 {
        // TODO
        13
    }

    fn worst_latency(&self, _write: &Op, _dst_idx: usize) -> u32 {
        // TODO
        15
    }

    fn legalize_op(&self, b: &mut LegalizeBuilder, op: &mut Op) {
        as_sm20_op_mut(op).legalize(b);
    }

    fn encode_shader(&self, s: &Shader<'_>) -> Vec<u32> {
        encode_sm20_shader(self, s)
    }
}

fn zero_reg() -> RegRef {
    RegRef::new(RegFile::GPR, 63, 1)
}

fn true_reg() -> RegRef {
    RegRef::new(RegFile::Pred, 7, 1)
}

enum AluSrc {
    None,
    Reg(RegRef),
    Imm(u32),
    CBuf(CBufRef),
}

impl AluSrc {
    fn from_src(src: Option<&Src>) -> AluSrc {
        if let Some(src) = src {
            assert!(src.src_swizzle.is_none());
            // do not assert src_mod, can be encoded by opcode.

            match src.src_ref {
                SrcRef::Zero => AluSrc::Reg(zero_reg()),
                SrcRef::Reg(r) => AluSrc::Reg(r),
                SrcRef::Imm32(x) => AluSrc::Imm(x),
                SrcRef::CBuf(x) => AluSrc::CBuf(x),
                _ => panic!("Unhandled ALU src type"),
            }
        } else {
            AluSrc::None
        }
    }
}

#[repr(u8)]
#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
enum SM20Unit {
    Float = 0,
    Double = 1,
    Imm32 = 2,
    Int = 3,
    Move = 4,
    Mem = 5,
    Tex = 6,
    Exec = 7,
}

impl fmt::Display for SM20Unit {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            SM20Unit::Float => write!(f, "float"),
            SM20Unit::Double => write!(f, "double"),
            SM20Unit::Imm32 => write!(f, "imm32"),
            SM20Unit::Int => write!(f, "int"),
            SM20Unit::Move => write!(f, "move"),
            SM20Unit::Mem => write!(f, "mem"),
            SM20Unit::Tex => write!(f, "tex"),
            SM20Unit::Exec => write!(f, "exec"),
        }
    }
}

trait SM20Op {
    fn legalize(&mut self, b: &mut LegalizeBuilder);
    fn encode(&self, e: &mut SM20Encoder<'_>);
}

struct SM20Encoder<'a> {
    _sm: &'a ShaderModel20,
    inst: [u32; 2],
}

impl BitViewable for SM20Encoder<'_> {
    fn bits(&self) -> usize {
        BitView::new(&self.inst).bits()
    }

    fn get_bit_range_u64(&self, range: Range<usize>) -> u64 {
        BitView::new(&self.inst).get_bit_range_u64(range)
    }
}

impl BitMutViewable for SM20Encoder<'_> {
    fn set_bit_range_u64(&mut self, range: Range<usize>, val: u64) {
        BitMutView::new(&mut self.inst).set_bit_range_u64(range, val);
    }
}

impl SetFieldU64 for SM20Encoder<'_> {
    fn set_field_u64(&mut self, range: Range<usize>, val: u64) {
        BitMutView::new(&mut self.inst).set_field_u64(range, val);
    }
}

impl SM20Encoder<'_> {
    fn set_opcode(&mut self, unit: SM20Unit, opcode: u8) {
        self.set_field(0..3, unit as u8);
        self.set_field(58..64, opcode);
    }

    fn set_pred_reg(&mut self, range: Range<usize>, reg: RegRef) {
        assert!(range.len() == 3);
        assert!(reg.file() == RegFile::Pred);
        assert!(reg.base_idx() <= 7);
        assert!(reg.comps() == 1);
        self.set_field(range, reg.base_idx());
    }

    fn set_pred_src(&mut self, range: Range<usize>, src: Src) {
        let (not, reg) = match src.src_ref {
            SrcRef::True => (false, true_reg()),
            SrcRef::False => (true, true_reg()),
            SrcRef::Reg(reg) => (false, reg),
            _ => panic!("Not a register"),
        };
        self.set_pred_reg(range.start..(range.end - 1), reg);
        self.set_bit(range.end - 1, not ^ src.src_mod.is_bnot());
    }

    fn set_pred_dst(&mut self, range: Range<usize>, dst: Dst) {
        let reg = match dst {
            Dst::None => true_reg(),
            Dst::Reg(reg) => reg,
            _ => panic!("Dst is not pred {dst}"),
        };
        self.set_pred_reg(range, reg);
    }

    fn set_pred(&mut self, pred: &Pred) {
        // predicates are 4 bits starting at 18, last one denotes inversion
        assert!(!pred.is_false());
        self.set_pred_reg(
            10..13,
            match pred.pred_ref {
                PredRef::None => true_reg(),
                PredRef::Reg(reg) => reg,
                PredRef::SSA(_) => panic!("SSA values must be lowered"),
            },
        );
        self.set_bit(13, pred.pred_inv);
    }

    fn set_reg(&mut self, range: Range<usize>, reg: RegRef) {
        assert!(range.len() == 6);
        assert!(reg.file() == RegFile::GPR);
        self.set_field(range, reg.base_idx());
    }

    fn set_reg_src_ref(&mut self, range: Range<usize>, src_ref: &SrcRef) {
        match src_ref {
            SrcRef::Zero => self.set_reg(range, zero_reg()),
            SrcRef::Reg(reg) => self.set_reg(range, *reg),
            _ => panic!("Not a register"),
        }
    }

    fn set_reg_src(&mut self, range: Range<usize>, src: Src) {
        assert!(src.src_swizzle.is_none());
        self.set_reg_src_ref(range, &src.src_ref);
    }

    fn set_dst(&mut self, range: Range<usize>, dst: Dst) {
        let reg = match dst {
            Dst::None => zero_reg(),
            Dst::Reg(reg) => reg,
            _ => panic!("Invalid dst {dst}"),
        };
        self.set_reg(range, reg);
    }

    fn set_carry_in(&mut self, bit: usize, src: Src) {
        assert!(src.src_mod.is_none());
        match src.src_ref {
            SrcRef::Zero => self.set_bit(bit, false),
            SrcRef::Reg(reg) => {
                assert!(reg == RegRef::new(RegFile::Carry, 0, 1));
                self.set_bit(bit, true);
            }
            _ => panic!("Invalid carry in: {src}"),
        }
    }

    fn set_carry_out(&mut self, bit: usize, dst: Dst) {
        match dst {
            Dst::None => self.set_bit(bit, false),
            Dst::Reg(reg) => {
                assert!(reg == RegRef::new(RegFile::Carry, 0, 1));
                self.set_bit(bit, true);
            }
            _ => panic!("Invalid carry out: {dst}"),
        }
    }

    fn set_src_imm_i20(
        &mut self,
        range: Range<usize>,
        sign_bit: usize,
        i: u32,
    ) {
        assert!(range.len() == 19);
        assert!((i & 0xfff80000) == 0 || (i & 0xfff80000) == 0xfff80000);

        self.set_field(range, i & 0x7ffff);
        self.set_field(sign_bit..sign_bit + 1, (i & 0x80000) >> 19);
    }

    fn set_src_imm_f20(
        &mut self,
        range: Range<usize>,
        sign_bit: usize,
        f: u32,
    ) {
        assert!(range.len() == 19);
        assert!((f & 0x00000fff) == 0);

        self.set_field(range, (f >> 12) & 0x7ffff);
        self.set_field(sign_bit..sign_bit + 1, f >> 31);
    }

    fn encode_form_a(
        &mut self,
        unit: SM20Unit,
        opcode: u8,
        dst: Option<&Dst>,
        src0: Option<&Src>,
        src1: Option<&Src>,
        src2: Option<&Src>,
    ) {
        self.set_opcode(unit, opcode);
        if let Some(&dst) = dst {
            self.set_dst(14..20, dst);
        }

        if let AluSrc::Reg(reg0) = AluSrc::from_src(src0) {
            self.set_reg(20..26, reg0);
        } else {
            panic!("Unsupported src0");
        }

        match AluSrc::from_src(src1) {
            AluSrc::None => panic!("Unsupported src1"),
            AluSrc::Reg(reg1) => match AluSrc::from_src(src2) {
                AluSrc::None => {
                    self.set_reg(26..32, reg1);
                }
                AluSrc::Reg(reg2) => {
                    self.set_reg(26..32, reg1);
                    self.set_reg(49..55, reg2);
                }
                AluSrc::Imm(_) => {
                    panic!("Immediates are only allowed in src1");
                }
                AluSrc::CBuf(cb) => {
                    let CBuf::Binding(idx) = cb.buf else {
                        panic!("Must be a bound constant buffer");
                    };
                    self.set_field(26..42, cb.offset);
                    self.set_field(42..46, idx);
                    self.set_field(46..48, 2_u8);
                    self.set_reg(49..55, reg1);
                }
            },
            AluSrc::Imm(imm32) => {
                match unit {
                    SM20Unit::Float | SM20Unit::Double => {
                        self.set_src_imm_f20(26..45, 45, imm32);
                    }
                    SM20Unit::Int | SM20Unit::Move | SM20Unit::Tex => {
                        self.set_src_imm_i20(26..45, 45, imm32);
                    }
                    _ => panic!("Unknown unit for immediate: {unit}"),
                }
                self.set_field(46..48, 3_u8);
                if let Some(src2) = src2 {
                    self.set_reg_src_ref(49..55, &src2.src_ref);
                }
            }
            AluSrc::CBuf(cb) => {
                let CBuf::Binding(idx) = cb.buf else {
                    panic!("Must be a bound constant buffer");
                };
                self.set_field(26..42, cb.offset);
                self.set_field(42..46, idx);
                self.set_field(46..48, 1_u8);
                if let Some(src2) = src2 {
                    self.set_reg_src_ref(49..55, &src2.src_ref);
                }
            }
        }
    }

    fn encode_form_a_imm32(
        &mut self,
        opcode: u8,
        dst: Option<&Dst>,
        src0: Option<&Src>,
        imm_src1: u32,
    ) {
        self.set_opcode(SM20Unit::Imm32, opcode);
        if let Some(&dst) = dst {
            self.set_dst(14..20, dst);
        }

        if let AluSrc::Reg(reg0) = AluSrc::from_src(src0) {
            self.set_reg(20..26, reg0);
        } else {
            panic!("Unsupported src0");
        }

        self.set_field(26..58, imm_src1);
    }

    fn encode_form_b(
        &mut self,
        unit: SM20Unit,
        opcode: u8,
        dst: Dst,
        src: Src,
    ) {
        self.set_opcode(unit, opcode);
        self.set_dst(14..20, dst);

        match AluSrc::from_src(Some(&src)) {
            AluSrc::None => panic!("src is always Some"),
            AluSrc::Reg(reg) => {
                self.set_reg(26..32, reg);
            }
            AluSrc::Imm(imm32) => {
                match unit {
                    SM20Unit::Float | SM20Unit::Double => {
                        self.set_src_imm_f20(26..45, 45, imm32);
                    }
                    SM20Unit::Int | SM20Unit::Move | SM20Unit::Tex => {
                        self.set_src_imm_i20(26..45, 45, imm32);
                    }
                    _ => panic!("Unknown unit for immediate: {unit}"),
                }
                self.set_field(46..48, 3_u8);
            }
            AluSrc::CBuf(cb) => {
                let CBuf::Binding(idx) = cb.buf else {
                    panic!("Must be a bound constant buffer");
                };
                self.set_field(26..42, cb.offset);
                self.set_field(42..46, idx);
                self.set_field(46..48, 1_u8);
            }
        }
    }

    fn encode_form_b_imm32(&mut self, opcode: u8, dst: Dst, imm_src: u32) {
        self.set_opcode(SM20Unit::Imm32, opcode);
        self.set_dst(14..20, dst);
        self.set_field(26..58, imm_src);
    }

    fn set_rnd_mode(&mut self, range: Range<usize>, rnd_mode: FRndMode) {
        self.set_field(
            range,
            match rnd_mode {
                FRndMode::NearestEven => 0_u8,
                FRndMode::NegInf => 1_u8,
                FRndMode::PosInf => 2_u8,
                FRndMode::Zero => 3_u8,
            },
        );
    }

    fn set_float_cmp_op(&mut self, range: Range<usize>, op: FloatCmpOp) {
        assert!(range.len() == 4);
        self.set_field(
            range,
            match op {
                FloatCmpOp::OrdLt => 0x01_u8,
                FloatCmpOp::OrdEq => 0x02_u8,
                FloatCmpOp::OrdLe => 0x03_u8,
                FloatCmpOp::OrdGt => 0x04_u8,
                FloatCmpOp::OrdNe => 0x05_u8,
                FloatCmpOp::OrdGe => 0x06_u8,
                FloatCmpOp::UnordLt => 0x09_u8,
                FloatCmpOp::UnordEq => 0x0a_u8,
                FloatCmpOp::UnordLe => 0x0b_u8,
                FloatCmpOp::UnordGt => 0x0c_u8,
                FloatCmpOp::UnordNe => 0x0d_u8,
                FloatCmpOp::UnordGe => 0x0e_u8,
                FloatCmpOp::IsNum => 0x07_u8,
                FloatCmpOp::IsNan => 0x08_u8,
            },
        );
    }
}

impl SM20Op for OpFAdd {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1] = &mut self.srcs;
        swap_srcs_if_not_reg(src0, src1, GPR);
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::F32);
        if src1.as_imm_not_f20().is_some()
            && (self.saturate || self.rnd_mode == FRndMode::NearestEven)
        {
            b.copy_alu_src(src1, GPR, SrcType::F32);
        }
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        if let Some(imm32) = self.srcs[1].as_imm_not_f20() {
            // Technically the modifier bits for these do work but legalization
            // should fold any modifiers on immediates for us.
            assert!(self.srcs[1].src_mod.is_none());
            e.encode_form_a_imm32(
                0xa,
                Some(&self.dst),
                Some(&self.srcs[0]),
                imm32,
            );
            assert!(self.saturate);
            assert!(self.rnd_mode == FRndMode::NearestEven);
        } else {
            e.encode_form_a(
                SM20Unit::Float,
                0x14,
                Some(&self.dst),
                Some(&self.srcs[0]),
                Some(&self.srcs[1]),
                None,
            );
            e.set_bit(49, self.saturate);
            e.set_rnd_mode(55..57, self.rnd_mode);
        }
        e.set_bit(5, self.ftz);
        e.set_bit(6, self.srcs[1].src_mod.has_fabs());
        e.set_bit(7, self.srcs[0].src_mod.has_fabs());
        e.set_bit(8, self.srcs[1].src_mod.has_fneg());
        e.set_bit(9, self.srcs[0].src_mod.has_fneg());
    }
}

impl SM20Op for OpFFma {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1, src2] = &mut self.srcs;
        swap_srcs_if_not_reg(src0, src1, GPR);
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::F32);
        b.copy_alu_src_if_fabs(src0, GPR, SrcType::F32);
        b.copy_alu_src_if_fabs(src1, GPR, SrcType::F32);
        b.copy_alu_src_if_fabs(src2, GPR, SrcType::F32);
        if src1.as_imm_not_f20().is_some()
            && (self.saturate
                || self.rnd_mode != FRndMode::NearestEven
                || self.dst.as_reg().is_none()
                || self.dst.as_reg() != src2.src_ref.as_reg())
        {
            b.copy_alu_src(src1, GPR, SrcType::F32);
        }
        b.copy_alu_src_if_not_reg(src2, GPR, SrcType::F32);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        assert!(!self.srcs[0].src_mod.has_fabs());
        assert!(!self.srcs[1].src_mod.has_fabs());
        assert!(!self.srcs[2].src_mod.has_fabs());

        if let Some(imm32) = self.srcs[1].as_imm_not_f20() {
            // Long immediates are only allowed if src2 == dst.
            assert!(self.dst.as_reg().is_some());
            assert!(self.dst.as_reg() == self.srcs[2].src_ref.as_reg());

            // Technically the modifier bits for these do work but legalization
            // should fold any modifiers on immediates for us.
            assert!(self.srcs[1].src_mod.is_none());

            e.encode_form_a_imm32(
                0x8,
                Some(&self.dst),
                Some(&self.srcs[0]),
                imm32,
            );
            assert!(self.rnd_mode == FRndMode::NearestEven);
        } else {
            e.encode_form_a(
                SM20Unit::Float,
                0xc,
                Some(&self.dst),
                Some(&self.srcs[0]),
                Some(&self.srcs[1]),
                Some(&self.srcs[2]),
            );
            e.set_rnd_mode(55..57, self.rnd_mode);
        }

        e.set_bit(5, self.saturate);
        e.set_bit(6, self.ftz);
        e.set_bit(7, self.dnz);

        e.set_bit(8, self.srcs[2].src_mod.has_fneg());
        let neg0 = self.srcs[0].src_mod.has_fneg();
        let neg1 = self.srcs[1].src_mod.has_fneg();
        e.set_bit(9, neg0 ^ neg1);
    }
}

impl SM20Op for OpFMnMx {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1] = &mut self.srcs;
        swap_srcs_if_not_reg(src0, src1, GPR);
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::F32);
        b.copy_alu_src_if_f20_overflow(src1, GPR, SrcType::F32);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        e.encode_form_a(
            SM20Unit::Float,
            0x2,
            Some(&self.dst),
            Some(&self.srcs[0]),
            Some(&self.srcs[1]),
            None,
        );
        e.set_bit(5, self.ftz);
        e.set_bit(6, self.srcs[1].src_mod.has_fabs());
        e.set_bit(7, self.srcs[0].src_mod.has_fabs());
        e.set_bit(8, self.srcs[1].src_mod.has_fneg());
        e.set_bit(9, self.srcs[0].src_mod.has_fneg());
        e.set_pred_src(49..53, self.min);
    }
}

impl SM20Op for OpFMul {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1] = &mut self.srcs;
        swap_srcs_if_not_reg(src0, src1, GPR);
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::F32);
        b.copy_alu_src_if_fabs(src0, GPR, SrcType::F32);
        b.copy_alu_src_if_fabs(src1, GPR, SrcType::F32);
        if src1.as_imm_not_f20().is_some()
            && self.rnd_mode != FRndMode::NearestEven
        {
            b.copy_alu_src(src1, GPR, SrcType::F32);
        }
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        assert!(!self.srcs[0].src_mod.has_fabs());
        assert!(!self.srcs[1].src_mod.has_fabs());

        if let Some(mut imm32) = self.srcs[1].as_imm_not_f20() {
            // Technically the modifier bits for these do work but legalization
            // should fold any modifiers on immediates for us.
            assert!(self.srcs[1].src_mod.is_none());

            // We don't, however, have a modifier for src0.  Just flip the
            // immediate in that case.
            if self.srcs[0].src_mod.has_fneg() {
                imm32 ^= 0x80000000;
            }
            e.encode_form_a_imm32(
                0xc,
                Some(&self.dst),
                Some(&self.srcs[0]),
                imm32,
            );
            assert!(self.rnd_mode == FRndMode::NearestEven);
        } else {
            e.encode_form_a(
                SM20Unit::Float,
                0x16,
                Some(&self.dst),
                Some(&self.srcs[0]),
                Some(&self.srcs[1]),
                None,
            );
            e.set_rnd_mode(55..57, self.rnd_mode);
            let neg0 = self.srcs[0].src_mod.has_fneg();
            let neg1 = self.srcs[1].src_mod.has_fneg();
            e.set_bit(25, neg0 ^ neg1);
        }

        e.set_bit(5, self.saturate);
        e.set_bit(6, self.ftz);
        e.set_bit(7, self.dnz);
    }
}

impl SM20Op for OpRro {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        b.copy_alu_src_if_f20_overflow(&mut self.src, GPR, SrcType::F32);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        e.encode_form_b(SM20Unit::Float, 0x18, self.dst, self.src);
        e.set_field(
            5..6,
            match self.op {
                RroOp::SinCos => 0u8,
                RroOp::Exp2 => 1u8,
            },
        );
        e.set_bit(6, self.src.src_mod.has_fabs());
        e.set_bit(8, self.src.src_mod.has_fneg());
    }
}

impl SM20Op for OpMuFu {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        b.copy_alu_src_if_not_reg(&mut self.src, GPR, SrcType::I32);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        e.set_opcode(SM20Unit::Float, 0x32);

        e.set_dst(14..20, self.dst);
        e.set_reg_src_ref(20..26, &self.src.src_ref);

        e.set_bit(5, false); // .sat
        e.set_bit(6, self.src.src_mod.has_fabs());
        e.set_bit(8, self.src.src_mod.has_fneg());
        e.set_field(
            26..30,
            match self.op {
                MuFuOp::Cos => 0_u8,
                MuFuOp::Sin => 1_u8,
                MuFuOp::Exp2 => 2_u8,
                MuFuOp::Log2 => 3_u8,
                MuFuOp::Rcp => 4_u8,
                MuFuOp::Rsq => 5_u8,
                MuFuOp::Rcp64H => 6_u8,
                MuFuOp::Rsq64H => 7_u8,
                _ => panic!("mufu{} not supported on SM20", self.op),
            },
        );
    }
}

impl SM20Op for OpFSet {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1] = &mut self.srcs;
        if swap_srcs_if_not_reg(src0, src1, GPR) {
            self.cmp_op = self.cmp_op.flip();
        }
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::ALU);
        b.copy_alu_src_if_f20_overflow(src1, GPR, SrcType::ALU);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        e.encode_form_a(
            SM20Unit::Float,
            0x6,
            Some(&self.dst),
            Some(&self.srcs[0]),
            Some(&self.srcs[1]),
            None,
        );

        e.set_bit(5, self.ftz);
        e.set_bit(6, self.srcs[1].src_mod.has_fabs());
        e.set_bit(7, self.srcs[0].src_mod.has_fabs());
        e.set_bit(8, self.srcs[1].src_mod.has_fneg());
        e.set_bit(9, self.srcs[0].src_mod.has_fneg());
        e.set_float_cmp_op(55..59, self.cmp_op);
    }
}

impl SM20Op for OpFSetP {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1] = &mut self.srcs;
        if swap_srcs_if_not_reg(src0, src1, GPR) {
            self.cmp_op = self.cmp_op.flip();
        }
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::ALU);
        b.copy_alu_src_if_f20_overflow(src1, GPR, SrcType::ALU);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        e.encode_form_a(
            SM20Unit::Float,
            0x8,
            None,
            Some(&self.srcs[0]),
            Some(&self.srcs[1]),
            None,
        );

        e.set_bit(6, self.srcs[1].src_mod.has_fabs());
        e.set_bit(7, self.srcs[0].src_mod.has_fabs());
        e.set_bit(8, self.srcs[1].src_mod.has_fneg());
        e.set_bit(9, self.srcs[0].src_mod.has_fneg());
        e.set_pred_dst(14..17, Dst::None);
        e.set_pred_dst(17..20, self.dst);
        e.set_pred_src(49..53, self.accum);
        e.set_pred_set_op(53..55, self.set_op);
        e.set_float_cmp_op(55..59, self.cmp_op);
        e.set_bit(59, self.ftz);
    }
}

impl SM20Op for OpFlo {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        b.copy_alu_src_if_i20_overflow(&mut self.src, GPR, SrcType::ALU);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        e.encode_form_b(SM20Unit::Int, 0x1e, self.dst, self.src);
        e.set_bit(5, self.signed);
        e.set_bit(6, self.return_shift_amount);
        e.set_bit(8, self.src.src_mod.is_bnot());
    }
}

impl SM20Op for OpIAdd2 {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1] = &mut self.srcs;
        swap_srcs_if_not_reg(src0, src1, GPR);
        if src0.src_mod.is_ineg() && src1.src_mod.is_ineg() {
            assert!(self.carry_out.is_none());
            b.copy_alu_src_and_lower_ineg(src0, GPR, SrcType::I32);
        }
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::I32);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        assert!(
            self.srcs[0].src_mod.is_none() || self.srcs[1].src_mod.is_none()
        );

        if let Some(imm32) = self.srcs[1].as_imm_not_i20() {
            e.encode_form_a_imm32(
                0x2,
                Some(&self.dst),
                Some(&self.srcs[0]),
                imm32,
            );
            e.set_carry_out(58, self.carry_out);
        } else {
            e.encode_form_a(
                SM20Unit::Int,
                0x12,
                Some(&self.dst),
                Some(&self.srcs[0]),
                Some(&self.srcs[1]),
                None,
            );
            e.set_carry_out(48, self.carry_out);
        }

        e.set_bit(5, false); // saturate
        e.set_bit(8, self.srcs[1].src_mod.is_ineg());
        e.set_bit(9, self.srcs[0].src_mod.is_ineg());
    }
}

impl SM20Op for OpIAdd2X {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1] = &mut self.srcs;
        swap_srcs_if_not_reg(src0, src1, GPR);
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::I32);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        assert!(
            self.srcs[0].src_mod.is_none() || self.srcs[1].src_mod.is_none()
        );

        if let Some(imm32) = self.srcs[1].as_imm_not_i20() {
            e.encode_form_a_imm32(
                0x2,
                Some(&self.dst),
                Some(&self.srcs[0]),
                imm32,
            );
            e.set_carry_out(58, self.carry_out);
        } else {
            e.encode_form_a(
                SM20Unit::Int,
                0x12,
                Some(&self.dst),
                Some(&self.srcs[0]),
                Some(&self.srcs[1]),
                None,
            );
            e.set_carry_out(48, self.carry_out);
        }

        e.set_bit(5, false); // saturate
        e.set_carry_in(6, self.carry_in);
        e.set_bit(8, self.srcs[1].src_mod.is_bnot());
        e.set_bit(9, self.srcs[0].src_mod.is_bnot());
    }
}

impl SM20Op for OpIMad {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1, src2] = &mut self.srcs;
        swap_srcs_if_not_reg(src0, src1, GPR);
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::I32);
        b.copy_alu_src_if_i20_overflow(src1, GPR, SrcType::I32);

        let neg_ab = src0.src_mod.is_ineg() ^ src1.src_mod.is_ineg();
        let neg_c = src2.src_mod.is_ineg();
        if neg_ab && neg_c {
            b.copy_alu_src_and_lower_ineg(src2, GPR, SrcType::I32);
        }
        if src_is_reg(src1, GPR) {
            b.copy_alu_src_if_imm(src2, GPR, SrcType::ALU);
        } else {
            b.copy_alu_src_if_not_reg(src2, GPR, SrcType::I32);
        }
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        e.encode_form_a(
            SM20Unit::Int,
            0x8,
            Some(&self.dst),
            Some(&self.srcs[0]),
            Some(&self.srcs[1]),
            Some(&self.srcs[2]),
        );

        e.set_bit(5, self.signed);
        e.set_bit(7, self.signed);

        let neg_ab =
            self.srcs[0].src_mod.is_ineg() ^ self.srcs[1].src_mod.is_ineg();
        let neg_c = self.srcs[2].src_mod.is_ineg();
        assert!(!neg_ab || !neg_c);
        e.set_bit(8, neg_c);
        e.set_bit(9, neg_ab);

        e.set_bit(24, false); // saturate
    }
}

impl SM20Op for OpIMul {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1] = &mut self.srcs;
        swap_srcs_if_not_reg(src0, src1, GPR);
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::I32);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        assert!(self.srcs[0].src_mod.is_none());
        assert!(self.srcs[1].src_mod.is_none());

        if let Some(imm32) = self.srcs[1].as_imm_not_i20() {
            e.encode_form_a_imm32(
                0x4,
                Some(&self.dst),
                Some(&self.srcs[0]),
                imm32,
            );
        } else {
            e.encode_form_a(
                SM20Unit::Int,
                0x14,
                Some(&self.dst),
                Some(&self.srcs[0]),
                Some(&self.srcs[1]),
                None,
            );
        }

        e.set_bit(5, self.signed[0]);
        e.set_bit(6, self.high);
        e.set_bit(7, self.signed[1]);
    }
}

impl SM20Encoder<'_> {
    fn set_pred_set_op(&mut self, range: Range<usize>, op: PredSetOp) {
        assert!(range.len() == 2);
        self.set_field(
            range,
            match op {
                PredSetOp::And => 0_u8,
                PredSetOp::Or => 1_u8,
                PredSetOp::Xor => 2_u8,
            },
        );
    }

    fn set_int_cmp_op(&mut self, range: Range<usize>, op: IntCmpOp) {
        assert!(range.len() == 3);
        self.set_field(
            range,
            match op {
                IntCmpOp::False => 0_u8,
                IntCmpOp::True => 7_u8,
                IntCmpOp::Eq => 2_u8,
                IntCmpOp::Ne => 5_u8,
                IntCmpOp::Lt => 1_u8,
                IntCmpOp::Le => 3_u8,
                IntCmpOp::Gt => 4_u8,
                IntCmpOp::Ge => 6_u8,
            },
        );
    }
}

impl SM20Op for OpISetP {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1] = &mut self.srcs;
        if swap_srcs_if_not_reg(src0, src1, GPR) {
            self.cmp_op = self.cmp_op.flip();
        }
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::ALU);
        b.copy_alu_src_if_i20_overflow(src1, GPR, SrcType::ALU);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        assert!(self.srcs[1].src_mod.is_none());
        assert!(self.srcs[0].src_mod.is_none());

        e.encode_form_a(
            SM20Unit::Int,
            0x6,
            None,
            Some(&self.srcs[0]),
            Some(&self.srcs[1]),
            None,
        );

        e.set_bit(5, self.cmp_type.is_signed());
        e.set_bit(6, self.ex);
        e.set_pred_dst(14..17, Dst::None);
        e.set_pred_dst(17..20, self.dst);
        e.set_pred_src(49..53, self.accum);
        e.set_pred_set_op(53..55, self.set_op);
        e.set_int_cmp_op(55..58, self.cmp_op);
    }
}

impl SM20Op for OpLop2 {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1] = &mut self.srcs;
        match self.op {
            LogicOp2::PassB => {
                *src0 = 0.into();
                b.copy_alu_src_if_i20_overflow(src1, GPR, SrcType::ALU);
            }
            LogicOp2::And | LogicOp2::Or | LogicOp2::Xor => {
                swap_srcs_if_not_reg(src0, src1, GPR);
                b.copy_alu_src_if_not_reg(src0, GPR, SrcType::ALU);
            }
        }
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        if let Some(imm32) = self.srcs[1].as_imm_not_i20() {
            e.encode_form_a_imm32(
                0xe,
                Some(&self.dst),
                Some(&self.srcs[0]),
                imm32,
            );
            assert!(self.op != LogicOp2::PassB);
        } else {
            e.encode_form_a(
                SM20Unit::Int,
                0x1a,
                Some(&self.dst),
                Some(&self.srcs[0]),
                Some(&self.srcs[1]),
                None,
            );
        }
        e.set_bit(5, false); // carry
        e.set_field(
            6..8,
            match self.op {
                LogicOp2::And => 0_u8,
                LogicOp2::Or => 1_u8,
                LogicOp2::Xor => 2_u8,
                LogicOp2::PassB => 3_u8,
            },
        );
        e.set_bit(8, self.srcs[1].src_mod.is_bnot());
        e.set_bit(9, self.srcs[0].src_mod.is_bnot());
    }
}

impl SM20Op for OpPopC {
    fn legalize(&mut self, _b: &mut LegalizeBuilder) {
        // Nothing to do
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        // popc on Fermi takes two sources and ANDs them and counts the
        // intersecting bits.  Pass it !rZ as the second source.
        let mask = Src::from(0).bnot();
        e.encode_form_a(
            SM20Unit::Move,
            0x15,
            Some(&self.dst),
            Some(&mask),
            Some(&self.src),
            None,
        );
        e.set_bit(8, self.src.src_mod.is_bnot());
        e.set_bit(9, mask.src_mod.is_bnot());
    }
}

impl SM20Op for OpMov {
    fn legalize(&mut self, _b: &mut LegalizeBuilder) {
        // Nothing to do
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        if let Some(imm32) = self.src.as_imm_not_i20() {
            e.encode_form_b_imm32(0x6, self.dst, imm32);
        } else {
            e.encode_form_b(SM20Unit::Move, 0xa, self.dst, self.src);
        }
        e.set_field(5..9, self.quad_lanes);
    }
}

impl SM20Op for OpPrmt {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1] = &mut self.srcs;
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::ALU);
        b.copy_alu_src_if_not_reg(src1, GPR, SrcType::ALU);
        if let SrcRef::Imm32(imm32) = &mut self.sel.src_ref {
            // Only the bottom 16 bits matter anyway
            *imm32 = *imm32 & 0xffff;
        }
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        e.encode_form_a(
            SM20Unit::Move,
            0x9,
            Some(&self.dst),
            Some(&self.srcs[0]),
            Some(&self.sel),
            Some(&self.srcs[1]),
        );
        e.set_field(
            5..8,
            match self.mode {
                PrmtMode::Index => 0_u8,
                PrmtMode::Forward4Extract => 1_u8,
                PrmtMode::Backward4Extract => 2_u8,
                PrmtMode::Replicate8 => 3_u8,
                PrmtMode::EdgeClampLeft => 4_u8,
                PrmtMode::EdgeClampRight => 5_u8,
                PrmtMode::Replicate16 => 6_u8,
            },
        );
    }
}

impl SM20Op for OpSel {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1] = &mut self.srcs;
        if swap_srcs_if_not_reg(src0, src1, GPR) {
            self.cond = self.cond.bnot();
        }
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::ALU);
        b.copy_alu_src_if_i20_overflow(src1, GPR, SrcType::ALU);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        e.encode_form_a(
            SM20Unit::Move,
            0x8,
            Some(&self.dst),
            Some(&self.srcs[0]),
            Some(&self.srcs[1]),
            None,
        );
        e.set_pred_src(49..53, self.cond);
    }
}

impl SM20Op for OpPSetP {
    fn legalize(&mut self, _b: &mut LegalizeBuilder) {
        // Nothing to do
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        e.set_opcode(SM20Unit::Move, 0x3);

        e.set_pred_dst(14..17, self.dsts[1]);
        e.set_pred_dst(17..20, self.dsts[0]);
        e.set_pred_src(20..24, self.srcs[0]);
        e.set_pred_src(26..30, self.srcs[1]);
        e.set_pred_set_op(30..32, self.ops[0]);
        e.set_pred_src(49..53, self.srcs[2]);
        e.set_pred_set_op(53..55, self.ops[1]);
    }
}

impl SM20Encoder<'_> {
    fn set_mem_type(&mut self, range: Range<usize>, mem_type: MemType) {
        assert!(range.len() == 3);
        self.set_field(
            range,
            match mem_type {
                MemType::U8 => 0_u8,
                MemType::I8 => 1_u8,
                MemType::U16 => 2_u8,
                MemType::I16 => 3_u8,
                MemType::B32 => 4_u8,
                MemType::B64 => 5_u8,
                MemType::B128 => 6_u8,
            },
        );
    }
}

/// Helper to legalize extended or external instructions
///
/// These are instructions which reach out external units such as load/store
/// and texture ops.  They typically can't take anything but GPRs and are the
/// only types of instructions that support vectors.
///
fn legalize_ext_instr(op: &mut impl SrcsAsSlice, _b: &mut LegalizeBuilder) {
    let src_types = op.src_types();
    for (i, src) in op.srcs_as_mut_slice().iter_mut().enumerate() {
        match src_types[i] {
            SrcType::SSA => {
                assert!(src.as_ssa().is_some());
            }
            SrcType::GPR => {
                assert!(src_is_reg(src, RegFile::GPR));
            }
            SrcType::ALU
            | SrcType::F16
            | SrcType::F16v2
            | SrcType::F32
            | SrcType::F64
            | SrcType::I32
            | SrcType::B32 => {
                panic!("ALU srcs must be legalized explicitly");
            }
            SrcType::Pred => {
                panic!("Predicates must be legalized explicitly");
            }
            SrcType::Carry => {
                panic!("Carry values must be legalized explicitly");
            }
            SrcType::Bar => panic!("Barrier regs are Volta+"),
        }
    }
}

impl SM20Op for OpLd {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        legalize_ext_instr(self, b);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        match self.access.space {
            MemSpace::Global(addr_type) => {
                e.set_opcode(SM20Unit::Mem, 0x20);
                e.set_field(26..58, self.offset);
                e.set_bit(58, addr_type == MemAddrType::A64);
            }
            MemSpace::Local => {
                e.set_opcode(SM20Unit::Mem, 0x30);
                e.set_bit(56, false); // shared
                e.set_field(26..50, self.offset);
            }
            MemSpace::Shared => {
                e.set_opcode(SM20Unit::Mem, 0x30);
                e.set_bit(56, true); // shared
                e.set_field(26..50, self.offset);
            }
        }
        e.set_mem_type(5..8, self.access.mem_type);
        // 8..9: cache hints (.ca, .cg, .lu, .cv)
        e.set_dst(14..20, self.dst);
        e.set_reg_src(20..26, self.addr);
    }
}

impl SM20Op for OpLdc {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        b.copy_alu_src_if_not_reg(&mut self.offset, GPR, SrcType::GPR);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        assert!(self.cb.src_mod.is_none());
        let SrcRef::CBuf(cb) = &self.cb.src_ref else {
            panic!("Not a CBuf source");
        };
        let CBuf::Binding(cb_idx) = cb.buf else {
            panic!("Must be a bound constant buffer");
        };

        e.set_opcode(SM20Unit::Tex, 0x5);

        e.set_mem_type(5..8, self.mem_type);
        e.set_field(
            8..10,
            match self.mode {
                LdcMode::Indexed => 0_u8,
                LdcMode::IndexedLinear => 1_u8,
                LdcMode::IndexedSegmented => 2_u8,
                LdcMode::IndexedSegmentedLinear => 3_u8,
            },
        );
        e.set_dst(14..20, self.dst);
        e.set_reg_src(20..26, self.offset);
        e.set_field(26..42, cb.offset);
        e.set_field(42..47, cb_idx);
    }
}

impl SM20Op for OpSt {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        legalize_ext_instr(self, b);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        match self.access.space {
            MemSpace::Global(addr_type) => {
                e.set_opcode(SM20Unit::Mem, 0x24);
                e.set_field(26..58, self.offset);
                e.set_bit(58, addr_type == MemAddrType::A64);
            }
            MemSpace::Local => {
                e.set_opcode(SM20Unit::Mem, 0x32);
                e.set_bit(56, false); // shared
                e.set_field(26..50, self.offset);
            }
            MemSpace::Shared => {
                e.set_opcode(SM20Unit::Mem, 0x32);
                e.set_bit(56, true); // shared
                e.set_field(26..50, self.offset);
            }
        }
        e.set_mem_type(5..8, self.access.mem_type);
        // 8..9: cache hints (.ca, .cg, .lu, .cv)
        e.set_reg_src(14..20, self.data);
        e.set_reg_src(20..26, self.addr);
    }
}

impl SM20Op for OpExit {
    fn legalize(&mut self, _b: &mut LegalizeBuilder) {
        // Nothing to do
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        e.set_opcode(SM20Unit::Exec, 0x20);
        e.set_field(5..9, 0xf_u8); // flags
    }
}

impl SM20Op for OpNop {
    fn legalize(&mut self, _b: &mut LegalizeBuilder) {
        // Nothing to do
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        e.set_opcode(SM20Unit::Move, 0x10);
        e.set_field(5..9, 0xf_u8); // flags
    }
}

impl SM20Op for OpS2R {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        legalize_ext_instr(self, b);
    }

    fn encode(&self, e: &mut SM20Encoder<'_>) {
        e.set_opcode(SM20Unit::Move, 0xb);
        e.set_dst(14..20, self.dst);
        e.set_field(26..32, self.idx);
    }
}

macro_rules! as_sm20_op_match {
    ($op: expr) => {
        match $op {
            Op::FAdd(op) => op,
            Op::FFma(op) => op,
            Op::FMnMx(op) => op,
            Op::FMul(op) => op,
            Op::Rro(op) => op,
            Op::MuFu(op) => op,
            Op::FSet(op) => op,
            Op::FSetP(op) => op,
            Op::Flo(op) => op,
            Op::IAdd2(op) => op,
            Op::IAdd2X(op) => op,
            Op::IMad(op) => op,
            Op::IMul(op) => op,
            Op::ISetP(op) => op,
            Op::Lop2(op) => op,
            Op::PopC(op) => op,
            Op::Mov(op) => op,
            Op::Prmt(op) => op,
            Op::Sel(op) => op,
            Op::PSetP(op) => op,
            Op::Ld(op) => op,
            Op::Ldc(op) => op,
            Op::St(op) => op,
            Op::Exit(op) => op,
            Op::Nop(op) => op,
            Op::S2R(op) => op,
            _ => panic!("Unhandled instruction {}", $op),
        }
    };
}

fn as_sm20_op(op: &Op) -> &dyn SM20Op {
    as_sm20_op_match!(op)
}

fn as_sm20_op_mut(op: &mut Op) -> &mut dyn SM20Op {
    as_sm20_op_match!(op)
}

fn encode_sm20_shader(sm: &ShaderModel20, s: &Shader<'_>) -> Vec<u32> {
    assert!(s.functions.len() == 1);
    let func = &s.functions[0];

    let mut encoded = Vec::new();
    for b in &func.blocks {
        for instr in &b.instrs {
            let mut e = SM20Encoder {
                _sm: sm,
                inst: [0_u32; 2],
            };
            as_sm20_op(&instr.op).encode(&mut e);
            e.set_pred(&instr.pred);
            encoded.extend(&e.inst[..]);
        }
    }

    encoded
}
