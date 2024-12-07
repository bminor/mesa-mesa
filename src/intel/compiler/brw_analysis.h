/*
 * Copyright © 2010-2020 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "brw_cfg.h"
#include "brw_inst.h"
#include "brw_ir_analysis.h"
#include "util/bitset.h"

struct fs_visitor;

namespace brw {
   /**
    * Immediate dominator tree analysis of a shader.
    */
   struct idom_tree {
      idom_tree(const fs_visitor *s);
      ~idom_tree();

      bool
      validate(const fs_visitor *) const
      {
         /* FINISHME */
         return true;
      }

      analysis_dependency_class
      dependency_class() const
      {
         return DEPENDENCY_BLOCKS;
      }

      const bblock_t *
      parent(const bblock_t *b) const
      {
         assert(unsigned(b->num) < num_parents);
         return parents[b->num];
      }

      bblock_t *
      parent(bblock_t *b) const
      {
         assert(unsigned(b->num) < num_parents);
         return parents[b->num];
      }

      bblock_t *
      intersect(bblock_t *b1, bblock_t *b2) const;

      /**
       * Returns true if block `a` dominates block `b`.
       */
      bool
      dominates(const bblock_t *a, const bblock_t *b) const
      {
         while (a != b) {
            if (b->num == 0)
               return false;

            b = parent(b);
         }
         return true;
      }

      void dump(FILE *file = stderr) const;

   private:
      unsigned num_parents;
      bblock_t **parents;
   };

   /**
    * Register pressure analysis of a shader.  Estimates how many registers
    * are live at any point of the program in GRF units.
    */
   struct register_pressure {
      register_pressure(const fs_visitor *v);
      ~register_pressure();

      analysis_dependency_class
      dependency_class() const
      {
         return (DEPENDENCY_INSTRUCTION_IDENTITY |
                 DEPENDENCY_INSTRUCTION_DATA_FLOW |
                 DEPENDENCY_VARIABLES);
      }

      bool
      validate(const fs_visitor *) const
      {
         /* FINISHME */
         return true;
      }

      unsigned *regs_live_at_ip;
   };

   class def_analysis {
   public:
      def_analysis(const fs_visitor *v);
      ~def_analysis();

      brw_inst *
      get(const brw_reg &reg) const
      {
         return reg.file == VGRF && reg.nr < def_count ?
                def_insts[reg.nr] : NULL;
      }

      bblock_t *
      get_block(const brw_reg &reg) const
      {
         return reg.file == VGRF && reg.nr < def_count ?
                def_blocks[reg.nr] : NULL;
      }

      uint32_t
      get_use_count(const brw_reg &reg) const
      {
         return reg.file == VGRF && reg.nr < def_count ?
                def_use_counts[reg.nr] : 0;
      }

      unsigned count() const { return def_count; }
      unsigned ssa_count() const;

      void print_stats(const fs_visitor *) const;

      analysis_dependency_class
      dependency_class() const
      {
         return DEPENDENCY_INSTRUCTION_IDENTITY |
                DEPENDENCY_INSTRUCTION_DATA_FLOW |
                DEPENDENCY_VARIABLES |
                DEPENDENCY_BLOCKS;
      }

      bool validate(const fs_visitor *) const;

   private:
      void mark_invalid(int);
      bool fully_defines(const fs_visitor *v, brw_inst *);
      void update_for_reads(const idom_tree &idom, bblock_t *block, brw_inst *);
      void update_for_write(const fs_visitor *v, bblock_t *block, brw_inst *);

      brw_inst **def_insts;
      bblock_t **def_blocks;
      uint32_t *def_use_counts;
      unsigned def_count;
   };

   class fs_live_variables {
   public:
      struct block_data {
         /**
          * Which variables are defined before being used in the block.
          *
          * Note that for our purposes, "defined" means unconditionally, completely
          * defined.
          */
         BITSET_WORD *def;

         /**
          * Which variables are used before being defined in the block.
          */
         BITSET_WORD *use;

         /** Which defs reach the entry point of the block. */
         BITSET_WORD *livein;

         /** Which defs reach the exit point of the block. */
         BITSET_WORD *liveout;

         /**
          * Variables such that the entry point of the block may be reached from any
          * of their definitions.
          */
         BITSET_WORD *defin;

         /**
          * Variables such that the exit point of the block may be reached from any
          * of their definitions.
          */
         BITSET_WORD *defout;

         BITSET_WORD flag_def[1];
         BITSET_WORD flag_use[1];
         BITSET_WORD flag_livein[1];
         BITSET_WORD flag_liveout[1];
      };

      fs_live_variables(const fs_visitor *s);
      ~fs_live_variables();

      bool validate(const fs_visitor *s) const;

      analysis_dependency_class
      dependency_class() const
      {
         return (DEPENDENCY_INSTRUCTION_IDENTITY |
                 DEPENDENCY_INSTRUCTION_DATA_FLOW |
                 DEPENDENCY_VARIABLES);
      }

      bool vars_interfere(int a, int b) const;
      bool vgrfs_interfere(int a, int b) const;
      int var_from_reg(const brw_reg &reg) const
      {
         return var_from_vgrf[reg.nr] + reg.offset / REG_SIZE;
      }

      /** Map from virtual GRF number to index in block_data arrays. */
      int *var_from_vgrf;

      /**
       * Map from any index in block_data to the virtual GRF containing it.
       *
       * For alloc.sizes of [1, 2, 3], vgrf_from_var would contain
       * [0, 1, 1, 2, 2, 2].
       */
      int *vgrf_from_var;

      int num_vars;
      int num_vgrfs;
      int bitset_words;

      /** @{
       * Final computed live ranges for each var (each component of each virtual
       * GRF).
       */
      int *start;
      int *end;
      /** @} */

      /** @{
       * Final computed live ranges for each VGRF.
       */
      int *vgrf_start;
      int *vgrf_end;
      /** @} */

      /** Per-basic-block information on live variables */
      struct block_data *block_data;

   protected:
      void setup_def_use();
      void setup_one_read(struct block_data *bd, int ip, const brw_reg &reg);
      void setup_one_write(struct block_data *bd, brw_inst *inst, int ip,
                           const brw_reg &reg);
      void compute_live_variables();
      void compute_start_end();

      const struct intel_device_info *devinfo;
      const cfg_t *cfg;
      void *mem_ctx;
   };

   /**
    * Various estimates of the performance of a shader based on static
    * analysis.
    */
   struct performance {
      performance(const fs_visitor *v);
      ~performance();

      analysis_dependency_class
      dependency_class() const
      {
         return (DEPENDENCY_INSTRUCTIONS |
                 DEPENDENCY_BLOCKS);
      }

      bool
      validate(const fs_visitor *) const
      {
         return true;
      }

      /**
       * Array containing estimates of the runtime of each basic block of the
       * program in cycle units.
       */
      unsigned *block_latency;

      /**
       * Estimate of the runtime of the whole program in cycle units assuming
       * uncontended execution.
       */
      unsigned latency;

      /**
       * Estimate of the throughput of the whole program in
       * invocations-per-cycle units.
       *
       * Note that this might be lower than the ratio between the dispatch
       * width of the program and its latency estimate in cases where
       * performance doesn't scale without limits as a function of its thread
       * parallelism, e.g. due to the existence of a bottleneck in a shared
       * function.
       */
      float throughput;

   private:
      performance(const performance &perf);
      performance &
      operator=(performance u);
   };
}
