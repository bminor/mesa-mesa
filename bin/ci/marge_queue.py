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
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from dateutil import parser
from typing import Optional

import gitlab
from gitlab.v4.objects import Project, ProjectMergeRequest
from gitlab_common import read_token, pretty_duration

REFRESH_WAIT = 30
MARGE_BOT_USER_ID = 9716
ASSIGNED_TO_MARGE = "assigned to @marge-bot"


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


@dataclass
class MargeMergeRequest:
    """Represent a Merge Request assigned to Marge"""
    id: int = field(init=False)
    mr: ProjectMergeRequest = field(repr=False)
    updated_at: datetime | None = field(init=False, repr=False)
    assigned_at: datetime | None = field(init=False, repr=False)

    def __post_init__(self):
        self.id = self.mr.iid
        self.updated_at = parser.parse(self.mr.updated_at)
        self.assigned_at = self.__find_last_assign_to_marge()

    def __find_last_assign_to_marge(self) -> Optional[datetime]:
        for note in self.mr.notes.list(
            iterator=True,
            order_by="updated_at",
            sort="desc"
        ):  # start with the most recent
            if note.body.startswith(ASSIGNED_TO_MARGE):
                return parser.parse(note.created_at)

    def __eq__(self, other: "MargeMergeRequest") -> bool:
        return self.id == other.id

    @property
    def time_enqueued(self) -> timedelta:
        if self.assigned_at is None:
            raise ValueError("Assign to marge timestamp not defined")
        return datetime.now(timezone.utc) - self.assigned_at

    @property
    def web_url(self) -> str:
        return self.mr.web_url

    @property
    def title(self) -> str:
        return self.mr.title


@dataclass
class MargeQueue:
    """
    Collect and sort the merge requests assigned to marge
    """
    elements_sorted: dict[datetime, MargeMergeRequest] = field(
        init=False, repr=False, default_factory=dict
    )
    undetermined: list[MargeMergeRequest] = field(
        init=False, repr=False, default_factory=list
    )

    def append(self, mr: MargeMergeRequest) -> None:
        if mr.assigned_at is not None:
            self.elements_sorted[mr.assigned_at] = mr
        else:
            self.undetermined.append(mr)

    @property
    def n_merge_requests_enqueued(self) -> int:
        return len(self.elements_sorted) + len(self.undetermined)

    @property
    def sorted_queue(self) -> list[MargeMergeRequest]:
        """
        Provide a list of the elements that can be sorted based on the
        assignment to Marge.
        """
        return list(dict(sorted(self.elements_sorted.items())).values())

    @property
    def all_assigned(
        self
    ) -> list[MargeMergeRequest]:
        """
        Provide a single list, but in sorted, of all the elements found
        assigned to Marge. This include the elements in elements_sorted
        and the ones undetermined (expected to be empty).
        """
        return self.sorted_queue + self.undetermined


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
