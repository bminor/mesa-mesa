/*
 * Copyright © 2025 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "intel_tools.h"

#include "compiler/brw_disasm.h"
#include "compiler/brw_isa_info.h"
#include "compiler/elk/elk_disasm.h"
#include "compiler/elk/elk_isa_info.h"
#include "dev/intel_device_info.h"

void
intel_disassemble(const struct intel_device_info *devinfo,
                  const void *assembly, int start, FILE *out)
{
      if (devinfo->ver >= 9) {
         struct brw_isa_info isa;
         brw_init_isa_info(&isa, devinfo);
         brw_disassemble_with_errors(&isa, assembly, start, out);
      } else {
         struct elk_isa_info isa;
         elk_init_isa_info(&isa, devinfo);
         elk_disassemble_with_errors(&isa, assembly, start, out);
      }
}

void
intel_decoder_init(struct intel_batch_decode_ctx *ctx,
                   const struct intel_device_info *devinfo,
                   FILE *fp, enum intel_batch_decode_flags flags,
                   const char *xml_path,
                   struct intel_batch_decode_bo (*get_bo)(void *, bool, uint64_t),
                   unsigned (*get_state_size)(void *, uint64_t, uint64_t),
                   void *user_data)
{
   if (devinfo->ver >= 9) {
      struct brw_isa_info isa;
      brw_init_isa_info(&isa, devinfo);
      intel_batch_decode_ctx_init_brw(ctx, &isa, devinfo, fp,
                                      flags, xml_path, get_bo, get_state_size, user_data);
   } else {
      struct elk_isa_info isa;
      elk_init_isa_info(&isa, devinfo);
      intel_batch_decode_ctx_init_elk(ctx, &isa, devinfo, fp,
                                      flags, xml_path, get_bo, get_state_size, user_data);
   }
}
