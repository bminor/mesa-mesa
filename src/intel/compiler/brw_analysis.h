/*
 * Copyright © 2010 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "brw_cfg.h"
#include "brw_inst.h"
#include "brw_ir_analysis.h"
#include "brw_fs_live_variables.h"
#include "brw_ir_performance.h"

struct fs_visitor;

namespace brw {
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
}
