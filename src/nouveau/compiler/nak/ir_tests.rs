// Copyright © 2025 Valve Corporation
// SPDX-License-Identifier: MIT
use crate::ir::*;

#[test]
fn test_ssa_ref_round_trip() {
    for len in 1..16 {
        let vec: Vec<_> = (0..len)
            .map(|i| SSAValue::new(RegFile::GPR, 1337 ^ i ^ len))
            .collect();

        let ssa_ref = SSARef::new(&vec);
        assert!(&ssa_ref[..] == &vec[..]);
    }
}
