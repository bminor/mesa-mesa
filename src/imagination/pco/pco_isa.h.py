# Copyright © 2024 Imagination Technologies Ltd.
# SPDX-License-Identifier: MIT

from mako.template import Template
from pco_isa import *

template = """/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PCO_ISA_H
#define PCO_ISA_H

/**
 * \\file pco_isa.h
 *
 * \\brief PCO ISA definitions.
 */

#include "util/macros.h"

#include <stdbool.h>

#endif /* PCO_ISA_H */"""

def main():
   print(Template(template).render())

if __name__ == '__main__':
   main()
