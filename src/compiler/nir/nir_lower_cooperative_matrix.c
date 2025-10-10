/*
 * Copyright © 2025 Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nir.h"
#include "nir_deref.h"
#include "nir_builder.h"

/*
 * Lower flexible size cooperative matrix operations down to operations at the supported granularity.
 */
struct split_mat {
   unsigned num_row_splits;
   unsigned num_col_splits;
   nir_variable **split_vars;
};

struct split_info {
   struct hash_table *split_mats;
   unsigned m_gran;
   unsigned n_gran;
   unsigned k_gran;
};

static struct split_mat *find_split(struct hash_table *split_mats,
                                    nir_intrinsic_instr *intr, int idx)
{
   nir_variable *var = nir_deref_instr_get_variable(nir_src_as_deref(intr->src[idx]));
   struct hash_entry *entry = _mesa_hash_table_search(split_mats, var);
   return entry ? entry->data : NULL;
}

static struct nir_deref_instr *recreate_derefs(nir_builder *b, nir_intrinsic_instr *intr, int idx,
                                               nir_variable *var)
{
   nir_deref_instr *deref = nir_src_as_deref(intr->src[idx]);

   nir_deref_path path;
   nir_deref_path_init(&path, deref, NULL);

   nir_deref_instr *old_head = path.path[0];
   b->cursor = nir_after_instr(&old_head->instr);
   nir_deref_instr *head = nir_build_deref_var(b, var);
   for (int i = 1; path.path[i]; i++) {
      nir_deref_instr *old = path.path[i];
      b->cursor = nir_after_instr(&old->instr);
      head = nir_build_deref_follower(b, head, old);
   }

   nir_deref_path_finish(&path);
   return head;
}

static void
get_rowcol_gran(struct glsl_cmat_description desc, unsigned m_gran,
                unsigned n_gran, unsigned k_gran,
                unsigned *row_gran, unsigned *col_gran)
{
   switch (desc.use) {
   case GLSL_CMAT_USE_A:
   default:
      *row_gran = m_gran;
      *col_gran = k_gran;
      break;
   case GLSL_CMAT_USE_B:
      *row_gran = k_gran;
      *col_gran = n_gran;
      break;
   case GLSL_CMAT_USE_ACCUMULATOR:
      *row_gran = m_gran;
      *col_gran = n_gran;
      break;
   }
}

static void
get_lower_sizes(struct glsl_cmat_description desc, unsigned m_gran,
                unsigned n_gran, unsigned k_gran,
                unsigned *split_rows_out, unsigned *split_cols_out)
{
   unsigned split_rows = 0, split_cols = 0;

   unsigned row_gran, col_gran;

   get_rowcol_gran(desc, m_gran, n_gran, k_gran, &row_gran, &col_gran);

   if (desc.rows && desc.rows != row_gran) {
      split_rows = row_gran;
      assert(desc.rows % split_rows == 0);
   }
   if (desc.cols && desc.cols != col_gran) {
      split_cols = col_gran;
      assert(desc.cols % split_cols == 0);
   }

   *split_rows_out = split_rows;
   *split_cols_out = split_cols;
}

static bool
split_cmat_construct(nir_builder *b,
                     nir_intrinsic_instr *intr,
                     struct split_info *info)
{
   nir_instr *instr = &intr->instr;
   struct split_mat *dst_split = find_split(info->split_mats, intr, 0);
   if (!dst_split)
      return false;

   unsigned splits = dst_split->num_col_splits * dst_split->num_row_splits;
   if (splits <= 1)
      return false;

   for (unsigned i = 0; i < splits; i++) {
      nir_deref_instr *dst_deref = recreate_derefs(b, intr, 0, dst_split->split_vars[i]);
      b->cursor = nir_before_instr(instr);
      nir_cmat_construct(b, &dst_deref->def, intr->src[1].ssa);
   }
   nir_instr_remove(instr);
   return true;
}

static bool
split_cmat_copy(nir_builder *b,
                nir_intrinsic_instr *intr,
                struct split_info *info)
{
   nir_instr *instr = &intr->instr;
   struct split_mat *dst_split = find_split(info->split_mats, intr, 0);
   struct split_mat *src_split = find_split(info->split_mats, intr, 1);

   if (!dst_split)
      return false;

   unsigned splits = dst_split->num_col_splits * dst_split->num_row_splits;
   if (splits <= 1)
      return false;

   assert(src_split);

   for (unsigned i = 0; i < splits; i++) {
      nir_deref_instr *dst_deref = recreate_derefs(b, intr, 0, dst_split->split_vars[i]);
      nir_deref_instr *src_deref = recreate_derefs(b, intr, 1, src_split->split_vars[i]);
      b->cursor = nir_before_instr(instr);
      nir_cmat_copy(b, &dst_deref->def, &src_deref->def);
   }
   nir_instr_remove(instr);
   return true;
}

static bool
split_cmat_length(nir_builder *b,
                  nir_intrinsic_instr *intr,
                  struct split_info *info)
{
   nir_instr *instr = &intr->instr;
   struct glsl_cmat_description desc = nir_intrinsic_cmat_desc(intr);
   unsigned row_gran, col_gran;
   unsigned split_rows = 0, split_cols = 0;
   unsigned splits = 1;

   get_rowcol_gran(desc, info->m_gran, info->n_gran, info->k_gran, &row_gran, &col_gran);

   if (desc.rows == row_gran &&
       desc.cols == col_gran)
      return false;

   get_lower_sizes(desc, info->m_gran, info->n_gran, info->k_gran, &split_rows, &split_cols);

   if (split_rows) {
      splits = desc.rows / split_rows;
      desc.rows = split_rows;
   }

   if (split_cols) {
      splits *= desc.cols / split_cols;
      desc.cols = split_cols;
   }

   if (splits <= 1)
      return false;

   b->cursor = nir_before_instr(instr);
   nir_def *def = nir_cmat_length(b, .cmat_desc = desc);
   def = nir_imul_imm(b, def, splits);
   nir_def_replace(&intr->def, def);
   return true;
}

static bool
split_cmat_insert(nir_builder *b,
                  nir_intrinsic_instr *intr,
                  struct split_info *info)
{
   nir_instr *instr = &intr->instr;
   struct split_mat *dst_split = find_split(info->split_mats, intr, 0);
   struct split_mat *src_split = find_split(info->split_mats, intr, 2);

   if (!dst_split)
      return false;

   unsigned splits = dst_split->num_col_splits * dst_split->num_row_splits;
   if (splits <= 1)
      return false;

   assert(src_split);
   b->cursor = nir_before_instr(instr);

   nir_def *len = nir_cmat_length(b, .cmat_desc = *glsl_get_cmat_description(src_split->split_vars[0]->type));
   nir_def *arr_idx = nir_udiv(b, intr->src[3].ssa, len);
   nir_def *base_idx = nir_umod(b, intr->src[3].ssa, len);
   for (unsigned i = 0; i < splits; i++) {
      nir_deref_instr *dst_deref = recreate_derefs(b, intr, 0, dst_split->split_vars[i]);
      nir_deref_instr *src_deref = recreate_derefs(b, intr, 2, src_split->split_vars[i]);
      b->cursor = nir_before_instr(instr);

      nir_def *new_def = nir_cmat_extract(b, nir_src_bit_size(intr->src[1]), &src_deref->def, base_idx);
      nir_def *cond = nir_ieq_imm(b, arr_idx, i);

      new_def = nir_bcsel(b, cond, intr->src[1].ssa, new_def);
      nir_cmat_insert(b, &dst_deref->def, new_def, &src_deref->def, base_idx);
   }
   nir_instr_remove(instr);
   return true;
}

static bool
split_cmat_extract(nir_builder *b,
                  nir_intrinsic_instr *intr,
                  struct split_info *info)
{
   nir_instr *instr = &intr->instr;
   struct split_mat *src_split = find_split(info->split_mats, intr, 0);

   if (!src_split)
      return false;

   unsigned splits = src_split->num_col_splits * src_split->num_row_splits;
   if (splits <= 1)
      return false;

   b->cursor = nir_before_instr(instr);

   nir_def *len = nir_cmat_length(b, .cmat_desc = *glsl_get_cmat_description(src_split->split_vars[0]->type));
   nir_def *arr_idx = nir_udiv(b, intr->src[1].ssa, len);
   nir_def *base_idx = nir_umod(b, intr->src[1].ssa, len);
   nir_def *last_def = nir_undef(b, 1, intr->def.bit_size);
   for (unsigned i = 0; i < splits; i++) {
      nir_deref_instr *src_deref = recreate_derefs(b, intr, 0, src_split->split_vars[i]);
      b->cursor = nir_before_instr(instr);
      nir_def *cond = nir_ieq_imm(b, arr_idx, i);
      nir_def *new_def = nir_cmat_extract(b, intr->def.bit_size, &src_deref->def, base_idx);

      last_def = nir_bcsel(b, cond, new_def, last_def);
   }
   nir_def_replace(&intr->def, last_def);
   return true;
}

static bool
split_cmat_convert(nir_builder *b,
                   nir_intrinsic_instr *intr,
                   struct split_info *info)
{
   nir_instr *instr = &intr->instr;
   struct split_mat *dst_split = find_split(info->split_mats, intr, 0);
   struct split_mat *src_split = find_split(info->split_mats, intr, 1);

   if (!dst_split && !src_split)
      return false;

   assert(dst_split && src_split);

   unsigned splits = src_split->num_col_splits * src_split->num_row_splits;
   for (unsigned i = 0; i < splits; i++) {
      nir_deref_instr *dst_deref = recreate_derefs(b, intr, 0, dst_split->split_vars[i]);
      nir_deref_instr *src_deref = recreate_derefs(b, intr, 1, src_split->split_vars[i]);
      b->cursor = nir_before_instr(instr);
      nir_cmat_convert(b, &dst_deref->def, &src_deref->def, .saturate = nir_intrinsic_saturate(intr),
                       .cmat_signed_mask = nir_intrinsic_cmat_signed_mask(intr));
   }
   nir_instr_remove(instr);
   return true;
}


static bool
split_cmat_transpose(nir_builder *b,
                     nir_intrinsic_instr *intr,
                     struct split_info *info)
{
   nir_instr *instr = &intr->instr;
   struct split_mat *dst_split = find_split(info->split_mats, intr, 0);
   struct split_mat *src_split = find_split(info->split_mats, intr, 1);

   if (!dst_split && !src_split)
      return false;

   assert(dst_split && src_split);

   for (unsigned r = 0; r < src_split->num_row_splits; r++) {
      for (unsigned c = 0; c < src_split->num_col_splits; c++) {
         int in_idx = r * src_split->num_col_splits + c;
         int out_idx = c * dst_split->num_col_splits + r;
         nir_deref_instr *dst_deref = recreate_derefs(b, intr, 0, dst_split->split_vars[out_idx]);
         nir_deref_instr *src_deref = recreate_derefs(b, intr, 1, src_split->split_vars[in_idx]);
         b->cursor = nir_before_instr(instr);
         nir_cmat_transpose(b, &dst_deref->def, &src_deref->def);
      }
   }
   nir_instr_remove(instr);
   return true;
}

static bool
split_cmat_bitcast(nir_builder *b,
                   nir_intrinsic_instr *intr,
                   struct split_info *info)
{
   nir_instr *instr = &intr->instr;
   struct split_mat *dst_split = find_split(info->split_mats, intr, 0);
   struct split_mat *src_split = find_split(info->split_mats, intr, 1);

   if (!dst_split)
      return false;

   unsigned splits = dst_split->num_col_splits * dst_split->num_row_splits;
   if (splits <= 1)
      return false;

   assert(src_split);

   for (unsigned i = 0; i < splits; i++) {
      nir_deref_instr *dst_deref = recreate_derefs(b, intr, 0, dst_split->split_vars[i]);
      nir_deref_instr *src_deref = recreate_derefs(b, intr, 1, src_split->split_vars[i]);
      b->cursor = nir_before_instr(instr);
      nir_cmat_bitcast(b, &dst_deref->def, &src_deref->def);
   }
   nir_instr_remove(instr);
   return true;
}

static bool
split_cmat_binary_op(nir_builder *b,
                     nir_intrinsic_instr *intr,
                     struct split_info *info)
{
   nir_instr *instr = &intr->instr;
   struct split_mat *dst_split = find_split(info->split_mats, intr, 0);
   struct split_mat *src0_split = find_split(info->split_mats, intr, 1);
   struct split_mat *src1_split = find_split(info->split_mats, intr, 2);

   if (!dst_split)
      return false;

   unsigned splits = dst_split->num_col_splits * dst_split->num_row_splits;
   if (splits <= 1)
      return false;

   assert(src0_split);
   assert(src1_split);

   for (unsigned i = 0; i < splits; i++) {
      nir_deref_instr *dst_deref = recreate_derefs(b, intr, 0, dst_split->split_vars[i]);
      nir_deref_instr *src0_deref = recreate_derefs(b, intr, 1, src0_split->split_vars[i]);
      nir_deref_instr *src1_deref = recreate_derefs(b, intr, 2, src1_split->split_vars[i]);
      b->cursor = nir_before_instr(instr);
      nir_cmat_binary_op(b, &dst_deref->def, &src0_deref->def, &src1_deref->def,
                         .alu_op = nir_intrinsic_alu_op(intr));
   }
   nir_instr_remove(instr);
   return true;
}

static bool
split_cmat_unary_op(nir_builder *b,
                    nir_intrinsic_instr *intr,
                    struct split_info *info)
{
   nir_instr *instr = &intr->instr;
   struct split_mat *dst_split = find_split(info->split_mats, intr, 0);
   struct split_mat *src_split = find_split(info->split_mats, intr, 1);

   if (!dst_split)
      return false;

   unsigned splits = dst_split->num_col_splits * dst_split->num_row_splits;
   if (splits <= 1)
      return false;

   assert(src_split);

   for (unsigned i = 0; i < splits; i++) {
      nir_deref_instr *dst_deref = recreate_derefs(b, intr, 0, dst_split->split_vars[i]);
      nir_deref_instr *src_deref = recreate_derefs(b, intr, 1, src_split->split_vars[i]);
      b->cursor = nir_before_instr(instr);
      nir_cmat_unary_op(b, &dst_deref->def, &src_deref->def, .alu_op = nir_intrinsic_alu_op(intr));
   }
   nir_instr_remove(instr);
   return true;
}

static bool
split_cmat_scalar_op(nir_builder *b,
                     nir_intrinsic_instr *intr,
                     struct split_info *info)
{
   nir_instr *instr = &intr->instr;
   struct split_mat *dst_split = find_split(info->split_mats, intr, 0);
   struct split_mat *src_split = find_split(info->split_mats, intr, 1);

   if (!dst_split)
      return false;

   unsigned splits = dst_split->num_col_splits * dst_split->num_row_splits;
   if (splits <= 1)
      return false;

   assert(src_split);

   for (unsigned i = 0; i < splits; i++) {
      nir_deref_instr *dst_deref = recreate_derefs(b, intr, 0, dst_split->split_vars[i]);
      nir_deref_instr *src_deref = recreate_derefs(b, intr, 1, src_split->split_vars[i]);
      b->cursor = nir_before_instr(instr);
      nir_cmat_scalar_op(b, &dst_deref->def, &src_deref->def, intr->src[2].ssa,
                         .alu_op = nir_intrinsic_alu_op(intr));
   }
   nir_instr_remove(instr);
   return true;
}

static bool
split_cmat_muladd(nir_builder *b,
                  nir_function_impl *impl,
                  nir_intrinsic_instr *intr,
                  struct split_info *info)
{
   nir_instr *instr = &intr->instr;
   struct split_mat *result_split = find_split(info->split_mats, intr, 0);
   struct split_mat *a_split = find_split(info->split_mats, intr, 1);
   struct split_mat *b_split = find_split(info->split_mats, intr, 2);
   struct split_mat *c_split = find_split(info->split_mats, intr, 3);

   unsigned m_splits = 1;
   unsigned n_splits = 1;
   unsigned k_splits = 1;

   if (!result_split && !a_split && !b_split && !c_split)
      return false;

   if (result_split) {
      assert(c_split);
      m_splits = result_split->num_row_splits;
      n_splits = result_split->num_col_splits;

      assert(c_split->num_row_splits == m_splits);
      assert(c_split->num_col_splits == n_splits);
      if (a_split)
         assert(a_split->num_row_splits == m_splits);

      if (b_split)
         assert(b_split->num_col_splits == n_splits);
   }

   if (a_split && a_split->num_col_splits > 1) {
      assert(b_split);
      assert(b_split->num_row_splits == a_split->num_col_splits);
      k_splits = a_split->num_col_splits;
   }

   for (unsigned m = 0; m < m_splits; m++) {
      for (unsigned n = 0; n < n_splits; n++) {
         unsigned idx = m * n_splits + n;
         nir_deref_instr *dst_deref = result_split ? recreate_derefs(b, intr, 0, result_split->split_vars[idx]) : nir_src_as_deref(intr->src[0]);
         nir_deref_instr *c_deref = c_split ? recreate_derefs(b, intr, 3, c_split->split_vars[idx]) : nir_src_as_deref(intr->src[3]);

         for (unsigned k = 0; k < k_splits; k++) {
            unsigned a_idx = m * k_splits + k;
            unsigned b_idx = k * n_splits + n;
            nir_deref_instr *a_deref = a_split ? recreate_derefs(b, intr, 1, a_split->split_vars[a_idx]) : nir_src_as_deref(intr->src[1]);
            nir_deref_instr *b_deref = b_split ? recreate_derefs(b, intr, 2, b_split->split_vars[b_idx]) : nir_src_as_deref(intr->src[2]);
            nir_deref_instr *k_dst_deref = k == k_splits - 1 ? dst_deref : c_deref;
            b->cursor = nir_before_instr(instr);

            nir_cmat_muladd(b, &k_dst_deref->def, &a_deref->def, &b_deref->def, &c_deref->def,
                            .saturate = nir_intrinsic_saturate(intr),
                            .cmat_signed_mask = nir_intrinsic_cmat_signed_mask(intr));
         }
      }
   }

   nir_instr_remove(instr);
   return true;
}

static bool
split_cmat_load_store(nir_builder *b,
                      nir_intrinsic_instr *intr,
                      struct split_info *info)
{
   nir_instr *instr = &intr->instr;
   const bool is_load = intr->intrinsic == nir_intrinsic_cmat_load;
   enum glsl_matrix_layout layout = nir_intrinsic_matrix_layout(intr);
   nir_variable *var = nir_deref_instr_get_variable(nir_src_as_deref(intr->src[!is_load]));
   struct hash_entry *entry = _mesa_hash_table_search(info->split_mats, var);
   if (!entry)
      return false;

   struct split_mat *split = entry->data;
   unsigned splits = split->num_row_splits * split->num_col_splits;
   for (unsigned i = 0; i < splits; i++) {
      nir_deref_instr *new_deref = recreate_derefs(b, intr, !is_load, split->split_vars[i]);
      nir_deref_instr *ptr_deref;
      nir_def *stride = intr->src[2].ssa;
      nir_def *ptr = intr->src[is_load].ssa;

      b->cursor = nir_before_instr(instr);
      if (i > 0) {
         nir_deref_instr *addr_deref = nir_src_as_deref(intr->src[is_load]);
         unsigned dst_bit_size = addr_deref->def.bit_size;
         nir_def *this_index = nir_imm_zero(b, 1, dst_bit_size);
         unsigned deref_bytes_size = glsl_get_explicit_size(addr_deref->type, false);
         const struct glsl_type *scalar_type = glsl_get_scalar_type(glsl_get_cmat_element(var->type));
         unsigned elem_size = glsl_get_explicit_size(scalar_type, false);
         struct glsl_cmat_description desc = *glsl_get_cmat_description(split->split_vars[i]->type);
         unsigned row_offset, col_offset;

         row_offset = (i % split->num_col_splits) * desc.cols;
         col_offset = (i / split->num_col_splits) * desc.rows;

         if (layout == GLSL_MATRIX_LAYOUT_ROW_MAJOR)
            SWAP(row_offset, col_offset);

         ptr_deref = nir_build_deref_cast(b, &addr_deref->def, addr_deref->modes, scalar_type, elem_size);
         stride = nir_udiv_imm(b, nir_imul_imm(b, stride, deref_bytes_size), elem_size);

         if (col_offset)
            this_index = nir_imm_intN_t(b, col_offset, dst_bit_size);
         if (row_offset)
            this_index = nir_iadd(b, this_index, nir_u2uN(b, nir_imul_imm(b, stride, row_offset), dst_bit_size));
         ptr_deref = nir_build_deref_ptr_as_array(b, ptr_deref, this_index);
         ptr = &ptr_deref->def;
      }
      if (is_load)
         nir_cmat_load(b, &new_deref->def, ptr, stride,
                       .matrix_layout = layout);
      else
         nir_cmat_store(b, ptr, &new_deref->def, stride,
                        .matrix_layout = layout);
   }
   nir_instr_remove(instr);
   return true;
}

static bool
split_matrix_impl(nir_function_impl *impl, struct split_info *info)
{
   bool progress = false;
   nir_builder b = nir_builder_create(impl);
   nir_foreach_block_reverse (block, impl) {
      nir_foreach_instr_reverse_safe (instr, block) {
         b.cursor = nir_before_instr(instr);
         switch (instr->type) {
         case nir_instr_type_intrinsic: {
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_cmat_construct:
               progress |= split_cmat_construct(&b, intr, info);
               break;
            case nir_intrinsic_cmat_copy:
               progress |= split_cmat_copy(&b, intr, info);
               break;
            case nir_intrinsic_cmat_length:
               progress |= split_cmat_length(&b, intr, info);
               break;
            case nir_intrinsic_cmat_insert:
               progress |= split_cmat_insert(&b, intr, info);
               break;
            case nir_intrinsic_cmat_extract:
               progress |= split_cmat_extract(&b, intr, info);
               break;
            case nir_intrinsic_cmat_convert:
               progress |= split_cmat_convert(&b, intr, info);
               break;
            case nir_intrinsic_cmat_transpose:
               progress |= split_cmat_transpose(&b, intr, info);
               break;
            case nir_intrinsic_cmat_bitcast:
               progress |= split_cmat_bitcast(&b, intr, info);
               break;
            case nir_intrinsic_cmat_binary_op:
               progress |= split_cmat_binary_op(&b, intr, info);
               break;
            case nir_intrinsic_cmat_unary_op:
               progress |= split_cmat_unary_op(&b, intr, info);
               break;
            case nir_intrinsic_cmat_scalar_op:
               progress |= split_cmat_scalar_op(&b, intr, info);
               break;
            case nir_intrinsic_cmat_muladd:
               progress |= split_cmat_muladd(&b, impl, intr, info);
               break;
            case nir_intrinsic_cmat_load:
            case nir_intrinsic_cmat_store:
               progress |= split_cmat_load_store(&b, intr, info);
               break;
            default:
               break;
            }
            break;
         }
         default:
            break;
         }
      }
   }
   return progress;
}

static struct split_mat *
split_var(nir_shader *shader,
          nir_function_impl *impl,
          void *mem_ctx,
          nir_variable *var,
          unsigned m_gran,
          unsigned n_gran,
          unsigned k_gran)
{
   if (!glsl_type_is_cmat(glsl_without_array(var->type)))
      return NULL;

   const struct glsl_type *type = var->type;
   if (glsl_type_is_array(type)) {
      type = glsl_without_array(var->type);
   }

   struct glsl_cmat_description desc = *glsl_get_cmat_description(type);
   unsigned split_rows = 0, split_cols = 0;

   get_lower_sizes(desc, m_gran, n_gran, k_gran, &split_rows, &split_cols);

   unsigned num_row_split = 1, num_col_split = 1;

   if (split_rows) {
      num_row_split = desc.rows / split_rows;
      desc.rows = split_rows;
   }
   if (split_cols) {
      num_col_split = desc.cols / split_cols;
      desc.cols = split_cols;
   }

   if (num_row_split == 1 && num_col_split == 1)
      return NULL;

   const struct glsl_type *new_type = glsl_type_wrap_in_arrays(glsl_cmat_type(&desc), var->type);

   struct split_mat *split_mat = ralloc(mem_ctx, struct split_mat);
   if (!split_mat)
      return NULL;

   unsigned num_split = num_row_split * num_col_split;
   split_mat->num_row_splits = num_row_split;
   split_mat->num_col_splits = num_col_split;
   split_mat->split_vars = ralloc_array(split_mat, struct nir_variable *, num_split);
   for (unsigned i = 0; i < num_split; i++) {
      if (!nir_variable_is_global(var)) {
         split_mat->split_vars[i] = nir_local_variable_create(impl,
                                                              new_type, var->name);
      } else {
         split_mat->split_vars[i] = nir_variable_create(shader, var->data.mode,
                                                        new_type, var->name);
      }
   }
   return split_mat;
}

static bool
lower_dimensions(nir_shader *shader, nir_function_impl *impl,
                 unsigned m_gran, unsigned n_gran, unsigned k_gran)
{
   struct hash_table *split_mats = _mesa_pointer_hash_table_create(NULL);
   void *mem_ctx = ralloc_context(NULL);
   bool progress = false;

   nir_foreach_variable_in_shader(var, shader) {
      struct split_mat *split_mat = split_var(shader, NULL, mem_ctx, var, m_gran, n_gran, k_gran);
      if (split_mat)
         _mesa_hash_table_insert(split_mats, var, split_mat);
   }
   nir_foreach_function_temp_variable (var, impl) {
      struct split_mat *split_mat = split_var(shader, impl, mem_ctx, var, m_gran, n_gran, k_gran);
      if (split_mat)
         _mesa_hash_table_insert(split_mats, var, split_mat);
   }

   struct split_info split_info = {
      .split_mats = split_mats,
      .m_gran = m_gran,
      .n_gran = n_gran,
      .k_gran = k_gran,
   };
   progress = split_matrix_impl(impl, &split_info);
   _mesa_hash_table_destroy(split_mats, NULL);
   ralloc_free(mem_ctx);
   return progress;
}

bool
nir_lower_cooperative_matrix_flexible_dimensions(nir_shader *shader, unsigned m_gran, unsigned n_gran, unsigned k_gran)
{
   bool progress = false;

   if (!shader->info.cs.has_cooperative_matrix)
      return false;

   struct nir_function *func = (struct nir_function *)exec_list_get_head_const(&shader->functions);

   progress |= lower_dimensions(shader, func->impl, m_gran, n_gran, k_gran);

   nir_foreach_function_impl(fnim, shader)
      nir_progress(progress, fnim, 0);
   return progress;
}
