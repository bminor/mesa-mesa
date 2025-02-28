#!/usr/bin/env python3

# Copyright © 2022 Valve Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

from jinja2 import Environment, FileSystemLoader
from os import environ, path


# Pass through all the B2C environment variables
values = {
    key: environ[key]
    for key in environ if key.startswith("B2C_")
}

env = Environment(loader=FileSystemLoader(path.dirname(environ['B2C_JOB_TEMPLATE'])),
                  trim_blocks=True, lstrip_blocks=True)

template = env.get_template(path.basename(environ['B2C_JOB_TEMPLATE']))

values['ci_job_id'] = environ['CI_JOB_ID']
values['ci_runner_description'] = environ['CI_RUNNER_DESCRIPTION']
values['working_dir'] = environ['CI_PROJECT_DIR']

# Pull all our images through our proxy registry
for image in ['B2C_IMAGE_UNDER_TEST', 'B2C_MACHINE_REGISTRATION_IMAGE', 'B2C_TELEGRAF_IMAGE']:
    values[image] = values[image].replace(
        'registry.freedesktop.org',
        '{{ fdo_proxy_registry }}'
    )

with open(path.splitext(path.basename(environ['B2C_JOB_TEMPLATE']))[0], "w") as f:
    f.write(template.render(values))
