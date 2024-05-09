# Copyright © 2024 Imagination Technologies Ltd.
# SPDX-License-Identifier: MIT

from mako.template import Template
from pco_pygen_common import *

template = """/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PCO_COMMON_H
#define PCO_COMMON_H

/**
 * \\file pco_common.h
 *
 * \\brief PCO common definitions.
 */

#include "util/macros.h"

#include <stdbool.h>

#endif /* PCO_COMMON_H */"""

def main():
   print(Template(template).render())

if __name__ == '__main__':
   main()
