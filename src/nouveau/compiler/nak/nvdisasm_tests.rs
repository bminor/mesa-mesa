// Copyright © 2025 Valve Corporation
// SPDX-License-Identifier: MIT

use crate::ir::*;
use crate::sm70::ShaderModel70;
use compiler::cfg::CFGBuilder;

use std::io::Write;
use std::mem;
use std::process;
use std::process::Command;
use std::slice;
use std::sync::atomic::AtomicUsize;

static FILE_NUM: AtomicUsize = AtomicUsize::new(0);

fn run_nvdisasm(s: &Shader) -> String {
    let code = s.sm.encode_shader(s);
    // println!("{code:x?}");

    let slice_u8: &[u8] = unsafe {
        slice::from_raw_parts(
            code.as_ptr() as *const u8,
            code.len() * mem::size_of::<u32>(),
        )
    };

    let tmp_file = format!(
        "/tmp/nak_dis_{}_{}",
        process::id(),
        FILE_NUM.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
    );
    std::fs::write(&tmp_file, slice_u8).expect("Failed to write file");

    let out = Command::new("nvdisasm")
        .arg("-b")
        .arg(format!("SM{}", s.sm.sm()))
        .arg("--print-raw")
        .arg(&tmp_file)
        .output()
        .expect("failed to execute process");

    std::io::stderr().write_all(&out.stderr).expect("IO error");
    assert!(out.status.success());
    let stdout = std::str::from_utf8(&out.stdout).unwrap();
    std::fs::remove_file(tmp_file).unwrap();
    stdout.into()
}

fn disassemble_instrs(instrs: Vec<Box<Instr>>, sm: u8) -> Vec<String> {
    let mut label_alloc = LabelAllocator::new();
    let block = BasicBlock {
        label: label_alloc.alloc(),
        uniform: true,
        instrs,
    };

    let mut cfg = CFGBuilder::new();
    cfg.add_node(0, block);

    let f = Function {
        ssa_alloc: SSAValueAllocator::new(),
        phi_alloc: PhiAllocator::new(),
        blocks: cfg.as_cfg(),
    };

    let cs_info = ComputeShaderInfo {
        local_size: [32, 1, 1],
        smem_size: 0,
    };
    let info = ShaderInfo {
        max_warps_per_sm: 0,
        num_gprs: 0,
        num_control_barriers: 0,
        num_instrs: 0,
        num_static_cycles: 0,
        num_spills_to_mem: 0,
        num_fills_from_mem: 0,
        num_spills_to_reg: 0,
        num_fills_from_reg: 0,
        slm_size: 0,
        max_crs_depth: 0,
        uses_global_mem: true,
        writes_global_mem: true,
        uses_fp64: false,
        stage: ShaderStageInfo::Compute(cs_info),
        io: ShaderIoInfo::None,
    };

    let sm: Box<dyn ShaderModel> = Box::new(ShaderModel70::new(sm));
    let s = Shader {
        sm: &*sm,
        info: info,
        functions: vec![f],
    };
    let out = run_nvdisasm(&s);
    let out: Vec<String> = out
        .lines()
        .map(|line| {
            let mut line: String = line
                .trim_start_matches(|c| -> bool {
                    match c {
                        '/' | '*' => true,
                        'a'..='f' => true, // Actual instructions are uppercase
                        _ => c.is_numeric() || c.is_whitespace(),
                    }
                })
                .trim()
                .into();
            line.make_ascii_lowercase();
            line
        })
        .collect();

    out
}

struct DisasmCheck {
    instrs: Vec<Box<Instr>>,
    expected: Vec<String>,
}

impl DisasmCheck {
    fn new() -> Self {
        DisasmCheck {
            instrs: Vec::new(),
            expected: Vec::new(),
        }
    }

    fn push(&mut self, instr: impl Into<Instr>, expected: impl Into<String>) {
        self.instrs.push(Box::new(instr.into()));
        self.expected.push(expected.into());
    }

    fn check(mut self, sm: u8) {
        assert!(self.expected.len() > 0);
        let actual = disassemble_instrs(std::mem::take(&mut self.instrs), sm);
        assert_eq!(actual.len(), self.expected.len());

        let mut any_different = false;
        for (a, e) in actual
            .into_iter()
            .zip(std::mem::take(&mut self.expected).into_iter())
        {
            if a != e {
                if !any_different {
                    eprintln!("Error: Difference on SM{sm}");
                    any_different = true;
                }
                eprintln!("actual: {a}");
                eprintln!("expect: {e}\n");
            }
        }
        if any_different {
            panic!("Differences found");
        }
    }
}

const SM_LIST: [u8; 8] = [70, 75, 80, 86, 89, 90, 100, 120];

#[test]
pub fn test_nop() {
    for sm in SM_LIST {
        let mut c = DisasmCheck::new();
        c.push(OpNop { label: None }, "nop;");
        c.check(sm);
    }
}
