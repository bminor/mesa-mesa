#!/usr/bin/env python3
# Copyright © 2020 - 2023 Collabora Ltd.
# Authors:
#   David Heidelberg <david.heidelberg@collabora.com>
#
# SPDX-License-Identifier: MIT

"""
Monitors Marge-bot and return number of assigned MRs.
"""

import argparse
import time
import sys
from datetime import datetime, timezone
from dateutil import parser

import gitlab
from gitlab.v4.objects import Project
from gitlab_common import read_token, pretty_duration

REFRESH_WAIT = 30
MARGE_BOT_USER_ID = 9716


def parse_args() -> argparse.Namespace:
    """Parse args"""
    parse = argparse.ArgumentParser(
        description="Tool to show merge requests assigned to the marge-bot",
    )
    parse.add_argument(
        "--wait", action="store_true", help="wait until CI is free",
    )
    parse.add_argument(
        "--token",
        metavar="token",
        help="force GitLab token, otherwise it's read from ~/.config/gitlab-token",
    )
    return parse.parse_args()


def get_merge_queue(project: Project) -> int:
    mrs = project.mergerequests.list(
        assignee_id=MARGE_BOT_USER_ID,
        scope="all",
        state="opened",
        get_all=True
    )

    n_mrs = len(mrs)
    for mr in mrs:
        updated = parser.parse(mr.updated_at)
        now = datetime.now(timezone.utc)
        diff = (now - updated).total_seconds()
        print(
            f"⛭ \u001b]8;;{mr.web_url}\u001b\\"
            f"{mr.title}\u001b]8;;\u001b\\ ({pretty_duration(diff)})"
        )
    return n_mrs


def main():
    args = parse_args()
    token = read_token(args.token)
    gl = gitlab.Gitlab(url="https://gitlab.freedesktop.org", private_token=token)

    project = gl.projects.get("mesa/mesa")

    while True:
        n_mrs = get_merge_queue(project)

        print(f"Job waiting: {n_mrs}")

        if n_mrs == 0:
            sys.exit(0)
        if not args.wait:
            sys.exit(min(n_mrs, 127))

        try:
            time.sleep(REFRESH_WAIT)
        except KeyboardInterrupt:
            print("Sleep interrupted from keyboard, doing one last iteration before finish.")
            args.wait = False


if __name__ == "__main__":
    main()
