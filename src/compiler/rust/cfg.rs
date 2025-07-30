// Copyright © 2023 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use crate::bitset::BitSet;
use std::collections::HashMap;
use std::hash::{BuildHasher, Hash};
use std::ops::{Deref, DerefMut, Index, IndexMut};
use std::slice;

/// A [CFG] node
pub struct CFGNode<N> {
    node: N,
    dom: usize,
    dom_pre_idx: usize,
    dom_post_idx: usize,
    lph: usize,
    pred: Vec<usize>,
    succ: Vec<usize>,
}

impl<N> Deref for CFGNode<N> {
    type Target = N;

    fn deref(&self) -> &N {
        &self.node
    }
}

impl<N> DerefMut for CFGNode<N> {
    fn deref_mut(&mut self) -> &mut N {
        &mut self.node
    }
}

fn graph_post_dfs<N>(
    nodes: &Vec<CFGNode<N>>,
    id: usize,
    seen: &mut BitSet,
    post_idx: &mut Vec<usize>,
    count: &mut usize,
) {
    if seen.contains(id) {
        return;
    }
    seen.insert(id);

    // Reverse the order of the successors so that any successors which are
    // forward edges get descending indices.  This ensures that, in the reverse
    // post order, successors (and their dominated children) come in-order.
    // In particular, as long as fall-through edges are only ever used for
    // forward edges and the fall-through edge comes first, we guarantee that
    // the fallthrough block comes immediately after its predecessor.
    for s in nodes[id].succ.iter().rev() {
        graph_post_dfs(nodes, *s, seen, post_idx, count);
    }

    post_idx[id] = *count;
    *count += 1;
}

fn rev_post_order_sort<N>(nodes: &mut Vec<CFGNode<N>>) {
    let mut seen = BitSet::new();
    let mut post_idx = Vec::new();
    post_idx.resize(nodes.len(), usize::MAX);
    let mut count = 0;

    graph_post_dfs(nodes, 0, &mut seen, &mut post_idx, &mut count);

    assert!(count <= nodes.len());

    let remap_idx = |i: usize| {
        let pid = post_idx[i];
        if pid == usize::MAX {
            None
        } else {
            assert!(pid < count);
            Some((count - 1) - pid)
        }
    };
    assert!(remap_idx(0) == Some(0));

    // Re-map edges to use post-index numbering
    for n in nodes.iter_mut() {
        let remap_filter_idx = |i: &mut usize| {
            if let Some(r) = remap_idx(*i) {
                *i = r;
                true
            } else {
                false
            }
        };
        n.pred.retain_mut(remap_filter_idx);
        n.succ.retain_mut(remap_filter_idx);
    }

    // We know a priori that each non-MAX post_idx is unique so we can sort the
    // nodes by inserting them into a new array by index.
    let mut sorted: Vec<CFGNode<N>> = Vec::with_capacity(count);
    for (i, n) in nodes.drain(..).enumerate() {
        if let Some(r) = remap_idx(i) {
            unsafe { sorted.as_mut_ptr().add(r).write(n) };
        }
    }
    unsafe { sorted.set_len(count) };

    std::mem::swap(nodes, &mut sorted);
}

fn find_common_dom<N>(
    nodes: &Vec<CFGNode<N>>,
    mut a: usize,
    mut b: usize,
) -> usize {
    while a != b {
        while a > b {
            a = nodes[a].dom;
        }
        while b > a {
            b = nodes[b].dom;
        }
    }
    a
}

fn dom_idx_dfs<N>(
    nodes: &mut Vec<CFGNode<N>>,
    dom_children: &Vec<Vec<usize>>,
    id: usize,
    count: &mut usize,
) {
    nodes[id].dom_pre_idx = *count;
    *count += 1;

    for c in dom_children[id].iter() {
        dom_idx_dfs(nodes, dom_children, *c, count);
    }

    nodes[id].dom_post_idx = *count;
    *count += 1;
}

fn calc_dominance<N>(nodes: &mut Vec<CFGNode<N>>) {
    nodes[0].dom = 0;
    loop {
        let mut changed = false;
        for i in 1..nodes.len() {
            let mut dom = nodes[i].pred[0];
            for p in &nodes[i].pred[1..] {
                if nodes[*p].dom != usize::MAX {
                    dom = find_common_dom(nodes, dom, *p);
                }
            }
            assert!(dom != usize::MAX);
            if nodes[i].dom != dom {
                nodes[i].dom = dom;
                changed = true;
            }
        }

        if !changed {
            break;
        }
    }

    let mut dom_children = Vec::new();
    dom_children.resize(nodes.len(), Vec::new());

    for i in 1..nodes.len() {
        let p = nodes[i].dom;
        if p != i {
            dom_children[p].push(i);
        }
    }

    let mut count = 0_usize;
    dom_idx_dfs(nodes, &dom_children, 0, &mut count);
    debug_assert!(count == nodes.len() * 2);
}

fn loop_detect_dfs<N>(
    nodes: &Vec<CFGNode<N>>,
    id: usize,
    pre: &mut BitSet,
    post: &mut BitSet,
    loops: &mut BitSet,
) {
    if pre.contains(id) {
        if !post.contains(id) {
            loops.insert(id);
        }
        return;
    }

    pre.insert(id);

    for s in nodes[id].succ.iter() {
        loop_detect_dfs(nodes, *s, pre, post, loops);
    }

    post.insert(id);
}

fn detect_loops<N>(nodes: &mut Vec<CFGNode<N>>) -> bool {
    let mut dfs_pre = BitSet::new();
    let mut dfs_post = BitSet::new();
    let mut loops = BitSet::new();
    loop_detect_dfs(nodes, 0, &mut dfs_pre, &mut dfs_post, &mut loops);

    let mut has_loop = false;
    nodes[0].lph = usize::MAX;
    for i in 1..nodes.len() {
        if loops.contains(i) {
            // This is a loop header
            nodes[i].lph = i;
            has_loop = true;
        } else {
            // Otherwise, we have the same loop header as our dominator
            let dom = nodes[i].dom;
            let dom_lph = nodes[dom].lph;
            nodes[i].lph = dom_lph;
        }
    }

    has_loop
}

/// A container structure which represents a control-flow graph.  Nodes are
/// automatically sorted and stored in reverse post-DFS order.  This means that
/// iterating over the nodes guarantees that dominators are visited before the
/// nodes they dominate.
pub struct CFG<N> {
    has_loop: bool,
    nodes: Vec<CFGNode<N>>,
}

impl<N> CFG<N> {
    /// Creates a new CFG from nodes and edges.
    pub fn from_blocks_edges(
        nodes: impl IntoIterator<Item = N>,
        edges: impl IntoIterator<Item = (usize, usize)>,
    ) -> Self {
        let mut nodes = Vec::from_iter(nodes.into_iter().map(|n| CFGNode {
            node: n,
            dom: usize::MAX,
            dom_pre_idx: usize::MAX,
            dom_post_idx: 0,
            lph: usize::MAX,
            pred: Vec::new(),
            succ: Vec::new(),
        }));

        for (p, s) in edges {
            nodes[s].pred.push(p);
            nodes[p].succ.push(s);
        }

        rev_post_order_sort(&mut nodes);
        calc_dominance(&mut nodes);
        let has_loop = detect_loops(&mut nodes);

        CFG {
            has_loop: has_loop,
            nodes: nodes,
        }
    }

    /// Returns a reference to the node at the given index.
    pub fn get(&self, idx: usize) -> Option<&N> {
        self.nodes.get(idx).map(|n| &n.node)
    }

    /// Returns a mutable reference to the node at the given index.
    pub fn get_mut(&mut self, idx: usize) -> Option<&mut N> {
        self.nodes.get_mut(idx).map(|n| &mut n.node)
    }

    /// Returns an iterator over the nodes.
    pub fn iter(&self) -> slice::Iter<CFGNode<N>> {
        self.nodes.iter()
    }

    /// Returns a mutable iterator over the nodes.
    pub fn iter_mut(&mut self) -> slice::IterMut<CFGNode<N>> {
        self.nodes.iter_mut()
    }

    /// Returns the number of nodes.
    pub fn len(&self) -> usize {
        self.nodes.len()
    }

    /// Returns the pre-index of the given node in a DFS of the dominance tree.
    pub fn dom_dfs_pre_index(&self, idx: usize) -> usize {
        self.nodes[idx].dom_pre_idx
    }

    /// Returns the post-index of the given node in a DFS of the dominance tree.
    pub fn dom_dfs_post_index(&self, idx: usize) -> usize {
        self.nodes[idx].dom_post_idx
    }

    /// Returns the index to the dominator parent of this node, if any.  If
    /// this is the entry node, `None` is returned.
    pub fn dom_parent_index(&self, idx: usize) -> Option<usize> {
        if idx == 0 {
            None
        } else {
            Some(self.nodes[idx].dom)
        }
    }

    /// Returns true if `parent` dominates `child`.
    pub fn dominates(&self, parent: usize, child: usize) -> bool {
        // If a block is unreachable, then dom_pre_idx == usize::MAX and
        // dom_post_idx == 0.  This allows us to trivially handle unreachable
        // blocks here with zero extra work.
        self.dom_dfs_pre_index(child) >= self.dom_dfs_pre_index(parent)
            && self.dom_dfs_post_index(child) <= self.dom_dfs_post_index(parent)
    }

    /// Returns true if this CFG contains a loop.
    pub fn has_loop(&self) -> bool {
        self.has_loop
    }

    /// Returns true if the given node is a loop header.
    pub fn is_loop_header(&self, idx: usize) -> bool {
        self.nodes[idx].lph == idx
    }

    /// Returns the index of the loop header of the innermost loop containing
    /// this node, if any.  If this node is not contained in any loops, `None`
    /// is returned.
    pub fn loop_header_index(&self, idx: usize) -> Option<usize> {
        let lph = self.nodes[idx].lph;
        if lph == usize::MAX {
            None
        } else {
            debug_assert!(self.is_loop_header(lph));
            Some(lph)
        }
    }

    /// Returns the loop depth of the given node.  Nodes not in any loops have
    /// a loop depth of zero.  For nodes inside a loop, this is the count of
    /// number of loop headers above them in the dominance tree.
    pub fn loop_depth(&self, idx: usize) -> usize {
        let mut idx = idx;
        let mut depth = 0;
        loop {
            let lph = self.nodes[idx].lph;
            if lph == usize::MAX {
                return depth;
            }

            depth += 1;

            // Loop headers have themselves as the lph so we need to skip to
            // the dominator of the loop header for the next iteration.
            idx = self.nodes[lph].dom;
        }
    }

    /// Returns the indices of the successors of this node in the CFG.
    pub fn succ_indices(&self, idx: usize) -> &[usize] {
        &self.nodes[idx].succ[..]
    }

    /// Returns the indices of the predecessors of this node in the CFG.
    pub fn pred_indices(&self, idx: usize) -> &[usize] {
        &self.nodes[idx].pred[..]
    }

    /// Drains the CFG and returns an iterator over the node data.
    pub fn drain(&mut self) -> impl Iterator<Item = N> + '_ {
        self.has_loop = false;
        self.nodes.drain(..).map(|n| n.node)
    }
}

impl<N> Index<usize> for CFG<N> {
    type Output = N;

    fn index(&self, idx: usize) -> &N {
        &self.nodes[idx].node
    }
}

impl<N> IndexMut<usize> for CFG<N> {
    fn index_mut(&mut self, idx: usize) -> &mut N {
        &mut self.nodes[idx].node
    }
}

impl<'a, N> IntoIterator for &'a CFG<N> {
    type Item = &'a CFGNode<N>;
    type IntoIter = slice::Iter<'a, CFGNode<N>>;

    fn into_iter(self) -> slice::Iter<'a, CFGNode<N>> {
        self.iter()
    }
}

impl<'a, N> IntoIterator for &'a mut CFG<N> {
    type Item = &'a mut CFGNode<N>;
    type IntoIter = slice::IterMut<'a, CFGNode<N>>;

    fn into_iter(self) -> slice::IterMut<'a, CFGNode<N>> {
        self.iter_mut()
    }
}

/// A structure for building a [CFG].
///
/// Building a control-flow graph often involves mapping some preexisting data
/// structure (such as block indices another CFG) onto nodes in the new CFG.
/// `CFGBuilder` makes all that automatic by letting you add nodes and edges
/// using any key type desired.  You then call `as_cfg()` to get the final
/// control-flow graph.
pub struct CFGBuilder<K, N, H: BuildHasher + Default> {
    nodes: Vec<N>,
    edges: Vec<(K, K)>,
    key_map: HashMap<K, usize, H>,
}

impl<K, N, H: BuildHasher + Default> CFGBuilder<K, N, H> {
    /// Creates a new CFG builder.
    pub fn new() -> Self {
        CFGBuilder {
            nodes: Vec::new(),
            edges: Vec::new(),
            key_map: Default::default(),
        }
    }
}

impl<K: Eq + Hash, N, H: BuildHasher + Default> CFGBuilder<K, N, H> {
    /// Adds a node to the CFG.
    pub fn add_node(&mut self, k: K, n: N) {
        self.key_map.insert(k, self.nodes.len());
        self.nodes.push(n);
    }

    /// Adds an edge to the CFG.
    pub fn add_edge(&mut self, s: K, p: K) {
        self.edges.push((s, p));
    }

    /// Destroys this builder and returns a CFG.
    pub fn as_cfg(mut self) -> CFG<N> {
        let edges = self.edges.drain(..).map(|(s, p)| {
            let s = *self.key_map.get(&s).unwrap();
            let p = *self.key_map.get(&p).unwrap();
            (s, p)
        });
        CFG::from_blocks_edges(self.nodes, edges)
    }
}

impl<K, N, H: BuildHasher + Default> Default for CFGBuilder<K, N, H> {
    fn default() -> Self {
        CFGBuilder::new()
    }
}
