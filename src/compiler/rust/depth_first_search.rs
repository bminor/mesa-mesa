// Copyright © 2025 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use crate::bitset::BitSet;

/// A trait implementing a depth-first search over a graph
pub trait DepthFirstSearch {
    type ChildIter;

    /// Pre-visit a node.  This is called before any children or edges are
    /// visited.  Returns an iterator to this node's children in the graph.
    fn pre(&mut self, id: usize) -> Self::ChildIter;

    /// Visit an edge.  An edge is visited before the child at the end of that
    /// edge is pre-visited.  Every edge is visited, even if if the node this
    /// edge points to has already been visited.
    fn edge(&mut self, _parent: usize, _child: usize) {
        // Does nothing by default
    }

    /// Post-visit a node.  This is called after all the children have been
    /// visited.
    fn post(&mut self, _id: usize) {
        // Does nothing by default
    }
}

fn dfs_impl<I, D>(dfs: &mut D, seen: &mut BitSet, id: usize)
where
    I: Iterator<Item = usize>,
    D: DepthFirstSearch<ChildIter = I>,
{
    if seen.contains(id) {
        return;
    }

    seen.insert(id);

    let children = dfs.pre(id);
    for child in children {
        dfs.edge(id, child);
        dfs_impl(dfs, seen, child);
    }
    dfs.post(id);
}

pub fn dfs<I, D>(dfs: &mut D, start: usize)
where
    I: Iterator<Item = usize>,
    D: DepthFirstSearch<ChildIter = I>,
{
    let mut seen = BitSet::new();
    dfs_impl(dfs, &mut seen, start);
}
