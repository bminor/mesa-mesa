// Copyright © 2022 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use crate::ir::*;
use crate::legalize::LegalizeBuilder;
use crate::sm70_encode::*;

pub struct ShaderModel70 {
    sm: u8,
}

impl ShaderModel70 {
    pub fn new(sm: u8) -> Self {
        assert!(sm >= 70);
        Self { sm }
    }

    fn has_uniform_alu(&self) -> bool {
        self.sm >= 73
    }
}

impl ShaderModel for ShaderModel70 {
    fn sm(&self) -> u8 {
        self.sm
    }

    fn num_regs(&self, file: RegFile) -> u32 {
        match file {
            RegFile::GPR => 255 - self.hw_reserved_gprs(),
            RegFile::UGPR => {
                if self.has_uniform_alu() {
                    63
                } else {
                    0
                }
            }
            RegFile::Pred => 7,
            RegFile::UPred => {
                if self.has_uniform_alu() {
                    7
                } else {
                    0
                }
            }
            RegFile::Carry => 0,
            RegFile::Bar => 16,
            RegFile::Mem => RegRef::MAX_IDX + 1,
        }
    }

    fn hw_reserved_gprs(&self) -> u32 {
        // On Volta+, 2 GPRs get burned for the program counter - see the
        // footnote on table 2 of the volta whitepaper
        // https://images.nvidia.com/content/volta-architecture/pdf/volta-architecture-whitepaper.pdf
        2
    }

    fn crs_size(&self, max_crs_depth: u32) -> u32 {
        assert!(max_crs_depth == 0);
        0
    }

    fn op_can_be_uniform(&self, op: &Op) -> bool {
        if !self.has_uniform_alu() {
            return false;
        }

        match op {
            Op::R2UR(_)
            | Op::S2R(_)
            | Op::BMsk(_)
            | Op::BRev(_)
            | Op::Flo(_)
            | Op::IAdd3(_)
            | Op::IAdd3X(_)
            | Op::IMad(_)
            | Op::IMad64(_)
            | Op::ISetP(_)
            | Op::Lea(_)
            | Op::LeaX(_)
            | Op::Lop3(_)
            | Op::Mov(_)
            | Op::PLop3(_)
            | Op::PopC(_)
            | Op::Prmt(_)
            | Op::PSetP(_)
            | Op::Sel(_)
            | Op::Shf(_)
            | Op::Shl(_)
            | Op::Shr(_)
            | Op::Vote(_)
            | Op::Copy(_)
            | Op::Pin(_)
            | Op::Unpin(_) => true,
            Op::Ldc(op) => op.offset.is_zero(),
            // UCLEA  USHL  USHR
            _ => false,
        }
    }

    fn legalize_op(&self, b: &mut LegalizeBuilder, op: &mut Op) {
        legalize_sm70_op(self, b, op);
    }

    fn encode_shader(&self, s: &Shader<'_>) -> Vec<u32> {
        encode_sm70_shader(self, s)
    }
}
