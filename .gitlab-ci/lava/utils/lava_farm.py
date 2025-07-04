# When changing this file, you need to bump the following
# .gitlab-ci/image-tags.yml tags:
# ALPINE_X86_64_LAVA_TRIGGER_TAG

import logging
import os


def get_lava_farm() -> str:
    """
    Returns the LAVA farm based on the FARM environment variable.

    :return: The LAVA farm
    """
    farm: str = os.getenv("FARM", "unknown")

    if farm == "unknown":
        logging.warning("FARM environment variable is not set, using unknown")

    return farm.lower()

def get_lava_boot_method() -> str:
    """
    Returns the LAVA boot method based on the BOOT_METHOD environment variable.

    :return: The LAVA boot method
    """
    boot_method: str = os.getenv("BOOT_METHOD", "unknown")

    if boot_method == "unknown":
        logging.warning("BOOT_METHOD environment variable is not set, using unknown")

    return boot_method.lower()
