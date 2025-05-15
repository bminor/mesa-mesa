// Copyright © 2025 Valve Corporation
// SPDX-License-Identifier: MIT

//! Dataflow analysis
//!
//! This module contains helpers for solving dataflow problems. A dataflow
//! problem is characterized by a "transfer" function, which updates information
//! based on a single block, and a "join" function, which updates information
//! along a control flow edge. See the wikipedia article for more information on
//! this terminology.
//! https://en.wikipedia.org/wiki/Data-flow_analysis#Basic_principles

use compiler::cfg::CFG;

pub struct BackwardDataflow<'a, Block, BlockIn, BlockOut, Transfer, Join>
where
    Transfer: FnMut(usize, &Block, &mut BlockIn, &BlockOut) -> bool,
    Join: FnMut(&mut BlockOut, &BlockIn),
{
    pub cfg: &'a CFG<Block>,
    pub block_in: &'a mut [BlockIn],
    pub block_out: &'a mut [BlockOut],

    /// Generate the block input from the block's output
    ///
    /// Returns true if block_in has changed, false otherwise
    pub transfer: Transfer,

    /// Update the block output based on a successor's input
    pub join: Join,
}

impl<'a, Block, BlockIn, BlockOut, Transfer, Join>
    BackwardDataflow<'a, Block, BlockIn, BlockOut, Transfer, Join>
where
    Transfer: FnMut(usize, &Block, &mut BlockIn, &BlockOut) -> bool,
    Join: FnMut(&mut BlockOut, &BlockIn),
{
    /// Solve the dataflow problem and generate output for each block
    pub fn solve(mut self) {
        assert_eq!(self.cfg.len(), self.block_in.len());
        assert_eq!(self.cfg.len(), self.block_out.len());

        let mut to_do = true;
        while to_do {
            to_do = false;
            for (block_idx, (block_out, block)) in self
                .block_out
                .iter_mut()
                .zip(self.cfg.iter())
                .enumerate()
                .rev()
            {
                for succ_idx in self.cfg.succ_indices(block_idx) {
                    (self.join)(block_out, &self.block_in[*succ_idx])
                }

                to_do |= (self.transfer)(
                    block_idx,
                    block,
                    &mut self.block_in[block_idx],
                    block_out,
                );
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use compiler::bitset::BitSet;
    use compiler::cfg::CFGBuilder;
    use std::hash::RandomState;

    fn check_graph_reachability(
        edges: &[(usize, usize)],
        expected: &[&[usize]],
    ) {
        let mut builder = CFGBuilder::<_, _, RandomState>::new();
        for (i, expected) in expected.iter().enumerate() {
            builder.add_node(i, (i, expected));
        }
        for &(a, b) in edges {
            builder.add_edge(a, b);
        }
        let cfg = builder.as_cfg();

        let mut reachable_in: Vec<BitSet> =
            (0..cfg.len()).map(|_| Default::default()).collect();
        let mut reachable_out: Vec<BitSet> =
            (0..cfg.len()).map(|_| Default::default()).collect();
        BackwardDataflow {
            cfg: &cfg,
            block_in: &mut reachable_in[..],
            block_out: &mut reachable_out[..],
            transfer: |_block_idx, block, live_in, live_out| {
                // The dataflow framework guarantees that each block and edge is
                // processed at least once, so we don't need to record whether
                // this first insert changed anything or not
                live_in.insert(block.0);

                live_in.union_with(live_out.s(..))
            },
            join: |live_out, succ_live_in| {
                *live_out |= succ_live_in.s(..);
            },
        }
        .solve();

        for (node, reachable_out) in cfg.iter().zip(reachable_out.iter()) {
            let (_i, expected) = **node;
            let r: Vec<usize> = reachable_out.iter().collect();
            assert_eq!(*expected, &r[..]);
        }
    }

    #[test]
    fn test_if_else() {
        check_graph_reachability(
            &[(0, 1), (0, 2), (1, 3), (2, 3)],
            &[&[1, 2, 3], &[3], &[3], &[]],
        );
    }

    #[test]
    fn test_loop() {
        check_graph_reachability(
            &[(0, 1), (1, 2), (2, 3), (2, 1)],
            &[&[1, 2, 3], &[1, 2, 3], &[1, 2, 3], &[]],
        );
    }

    #[test]
    fn test_irreducible() {
        check_graph_reachability(
            &[(0, 1), (0, 2), (1, 2), (2, 1), (1, 3), (2, 3)],
            &[&[1, 2, 3], &[1, 2, 3], &[1, 2, 3], &[]],
        );
    }
}
