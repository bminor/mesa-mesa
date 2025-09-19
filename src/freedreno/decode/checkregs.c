/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its susidiaries.
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdio.h>

#include "rnn.h"
#include "rnndec.h"
#include "rnnutil.h"

/**
 * A simple utility to check for overlapping/conflicting reg definitions
 * for any given generation.
 */

int
main(int argc, char **argv)
{
   struct rnn *rnn;
   int ret = 0;

   if (argc != 2) {
      fprintf(stderr, "usage: dumpregs GEN\n");
      return -1;
   }

   rnn = rnn_new(true);

   rnn_load(rnn, argv[1]);

   for (unsigned i = 0; i < rnn->dom[0]->subelemsnum; i++) {
      struct rnndelem *ei = rnn->dom[0]->subelems[i];

      if (!rnndec_varmatch(rnn->vc, &ei->varinfo))
         continue;

      uint32_t mini = ei->offset;
      uint32_t maxi = mini;

      if (ei->type == RNN_ETYPE_ARRAY)
         maxi = mini + ei->length - 1;

      for (unsigned j = i + 1; j < rnn->dom[0]->subelemsnum; j++) {
         struct rnndelem *ej = rnn->dom[0]->subelems[j];

         if (!rnndec_varmatch(rnn->vc, &ej->varinfo))
            continue;

         uint32_t minj = ej->offset;
         uint32_t maxj = minj;

         if (ej->type == RNN_ETYPE_ARRAY)
            maxj = minj + ej->length - 1;

         if ((maxi >= minj) && (maxj >= mini)) {
            fprintf(stderr, "Conflict: %s (0x%04x->0x%04x) vs %s (0x%04x->0x%04x)\n",
                    ei->name, mini, maxi, ej->name, minj, maxj);
            ret = -1;
         }
      }
   }

   return ret;
}
