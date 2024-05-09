# Copyright © 2024 Imagination Technologies Ltd.
# SPDX-License-Identifier: MIT

from mako.template import Template
from pco_ops import *

template = """/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PCO_OPS_H
#define PCO_OPS_H

/**
 * \\file pco_ops.h
 *
 * \\brief PCO op definitions and functions.
 */

#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#endif /* PCO_OPS_H */"""

def main():
   print(Template(template).render())

if __name__ == '__main__':
   main()
