// Copyright © 2022 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use crate::ir::*;

use std::ops::{Index, IndexMut, Range};

pub fn exec_latency(sm: u8, op: &Op) -> u32 {
    if sm >= 70 {
        match op {
            Op::Bar(_) | Op::MemBar(_) => {
                if sm >= 80 {
                    6
                } else {
                    5
                }
            }
            Op::CCtl(_op) => {
                // CCTL.C needs 8, CCTL.I needs 11
                11
            }
            // Op::DepBar(_) => 4,
            _ => 1, // TODO: co-issue
        }
    } else {
        match op {
            Op::CCtl(_)
            | Op::MemBar(_)
            | Op::Bra(_)
            | Op::SSy(_)
            | Op::Sync(_)
            | Op::Brk(_)
            | Op::PBk(_)
            | Op::Cont(_)
            | Op::PCnt(_)
            | Op::Exit(_)
            | Op::Bar(_)
            | Op::Kill(_)
            | Op::OutFinal(_) => 13,
            _ => 1,
        }
    }
}

pub fn instr_latency(sm: u8, op: &Op, dst_idx: usize) -> u32 {
    let file = match op.dsts_as_slice()[dst_idx] {
        Dst::None => return 0,
        Dst::SSA(vec) => vec.file().unwrap(),
        Dst::Reg(reg) => reg.file(),
    };

    let (gpr_latency, pred_latency) = if sm < 80 {
        match op {
            // Double-precision float ALU
            Op::DAdd(_)
            | Op::DFma(_)
            | Op::DMnMx(_)
            | Op::DMul(_)
            | Op::DSetP(_)
            // Half-precision float ALU
            | Op::HAdd2(_)
            | Op::HFma2(_)
            | Op::HMul2(_)
            | Op::HSet2(_)
            | Op::HSetP2(_)
            | Op::HMnMx2(_) => if sm == 70 {
                // Volta is even slower
                (13, 15)
            } else {
                (13, 14)
            }
            _ => (6, 13)
        }
    } else {
        (6, 13)
    };

    // This is BS and we know it
    match file {
        RegFile::GPR => gpr_latency,
        RegFile::UGPR => 12,
        RegFile::Pred => pred_latency,
        RegFile::UPred => 11,
        RegFile::Bar => 0, // Barriers have a HW scoreboard
        RegFile::Carry => 6,
        RegFile::Mem => panic!("Not a register"),
    }
}

/// Read-after-write latency
pub fn raw_latency(
    sm: u8,
    write: &Op,
    dst_idx: usize,
    _read: &Op,
    _src_idx: usize,
) -> u32 {
    instr_latency(sm, write, dst_idx)
}

/// Write-after-read latency
pub fn war_latency(
    _sm: u8,
    _read: &Op,
    _src_idx: usize,
    _write: &Op,
    _dst_idx: usize,
) -> u32 {
    // We assume the source gets read in the first 4 cycles.  We don't know how
    // quickly the write will happen.  This is all a guess.
    4
}

/// Write-after-write latency
pub fn waw_latency(
    sm: u8,
    a: &Op,
    a_dst_idx: usize,
    _b: &Op,
    _b_dst_idx: usize,
) -> u32 {
    // We know our latencies are wrong so assume the wrote could happen anywhere
    // between 0 and instr_latency(a) cycles
    instr_latency(sm, a, a_dst_idx)
}

/// Predicate read-after-write latency
pub fn paw_latency(sm: u8, write: &Op, _dst_idx: usize) -> u32 {
    if sm == 70 {
        match write {
            Op::DSetP(_) | Op::HSetP2(_) => 15,
            _ => 13,
        }
    } else {
        13
    }
}

pub struct RegTracker<T> {
    reg: [T; 255],
    ureg: [T; 63],
    pred: [T; 7],
    upred: [T; 7],
    carry: [T; 1],
}

fn new_array_with<T, const N: usize>(f: &impl Fn() -> T) -> [T; N] {
    let mut v = Vec::new();
    for _ in 0..N {
        v.push(f());
    }
    v.try_into()
        .unwrap_or_else(|_| panic!("Array size mismatch"))
}

impl<T> RegTracker<T> {
    pub fn new_with(f: &impl Fn() -> T) -> Self {
        Self {
            reg: new_array_with(f),
            ureg: new_array_with(f),
            pred: new_array_with(f),
            upred: new_array_with(f),
            carry: new_array_with(f),
        }
    }

    pub fn for_each_instr_pred_mut(
        &mut self,
        instr: &Instr,
        mut f: impl FnMut(&mut T),
    ) {
        if let PredRef::Reg(reg) = &instr.pred.pred_ref {
            for i in &mut self[*reg] {
                f(i);
            }
        }
    }

    pub fn for_each_instr_src_mut(
        &mut self,
        instr: &Instr,
        mut f: impl FnMut(usize, &mut T),
    ) {
        for (i, src) in instr.srcs().iter().enumerate() {
            match &src.src_ref {
                SrcRef::Reg(reg) => {
                    for t in &mut self[*reg] {
                        f(i, t);
                    }
                }
                SrcRef::CBuf(CBufRef {
                    buf: CBuf::BindlessUGPR(reg),
                    ..
                }) => {
                    for t in &mut self[*reg] {
                        f(i, t);
                    }
                }
                _ => (),
            }
        }
    }

    pub fn for_each_instr_dst_mut(
        &mut self,
        instr: &Instr,
        mut f: impl FnMut(usize, &mut T),
    ) {
        for (i, dst) in instr.dsts().iter().enumerate() {
            if let Dst::Reg(reg) = dst {
                for t in &mut self[*reg] {
                    f(i, t);
                }
            }
        }
    }
}

impl<T> Index<RegRef> for RegTracker<T> {
    type Output = [T];

    fn index(&self, reg: RegRef) -> &[T] {
        let range = reg.idx_range();
        let range = Range {
            start: usize::try_from(range.start).unwrap(),
            end: usize::try_from(range.end).unwrap(),
        };

        match reg.file() {
            RegFile::GPR => &self.reg[range],
            RegFile::UGPR => &self.ureg[range],
            RegFile::Pred => &self.pred[range],
            RegFile::UPred => &self.upred[range],
            RegFile::Carry => &self.carry[range],
            RegFile::Bar => &[], // Barriers have a HW scoreboard
            RegFile::Mem => panic!("Not a register"),
        }
    }
}

impl<T> IndexMut<RegRef> for RegTracker<T> {
    fn index_mut(&mut self, reg: RegRef) -> &mut [T] {
        let range = reg.idx_range();
        let range = Range {
            start: usize::try_from(range.start).unwrap(),
            end: usize::try_from(range.end).unwrap(),
        };

        match reg.file() {
            RegFile::GPR => &mut self.reg[range],
            RegFile::UGPR => &mut self.ureg[range],
            RegFile::Pred => &mut self.pred[range],
            RegFile::UPred => &mut self.upred[range],
            RegFile::Carry => &mut self.carry[range],
            RegFile::Bar => &mut [], // Barriers have a HW scoreboard
            RegFile::Mem => panic!("Not a register"),
        }
    }
}
