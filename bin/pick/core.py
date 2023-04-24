# Copyright © 2019-2020 Intel Corporation

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""Core data structures and routines for pick."""

import asyncio
import contextlib
import enum
import json
import pathlib
import re
import subprocess
import typing

import attr

if typing.TYPE_CHECKING:
    from .ui import UI

    import typing_extensions

    class CommitDict(typing_extensions.TypedDict):

        sha: str
        description: str
        nominated: bool
        nomination_type: int
        resolution: typing.Optional[int]
        main_sha: typing.Optional[str]
        because_sha: typing.Optional[str]
        notes: typing.Optional[str]

IS_FIX = re.compile(r'^\s*fixes:\s*([a-f0-9]{6,40})', flags=re.MULTILINE | re.IGNORECASE)
# FIXME: I dislike the duplication in this regex, but I couldn't get it to work otherwise
IS_CC = re.compile(r'^\s*cc:\s*["\']?([0-9]{2}\.[0-9])?["\']?\s*["\']?([0-9]{2}\.[0-9])?["\']?\s*\<?mesa-stable',
                   flags=re.MULTILINE | re.IGNORECASE)
IS_REVERT = re.compile(r'This reverts commit ([0-9a-f]{40})')

git_toplevel = subprocess.check_output(['git', 'rev-parse', '--show-toplevel'],
                                       stderr=subprocess.DEVNULL).decode("ascii").strip()
pick_status_json = pathlib.Path(git_toplevel) / '.pick_status.json'


@attr.s(slots=True, cmp=False)
class AsyncRWLock:

    """An asynchronous Read/Write lock.
    
    This is a very simple read/write lock that prioritizes reads.

    As an implementation detail, this relies on python's global locking to drop
    the need for a lock to protect the `readers` attribute.
    """

    readers: int = attr.ib(0, init=False)
    global_lock: asyncio.Lock = attr.ib(factory=asyncio.Lock, init=False)
    read_lock: asyncio.Lock = attr.ib(factory=asyncio.Lock, init=False)

    @contextlib.asynccontextmanager
    async def read(self) -> typing.AsyncIterator[None]:
        async with self.read_lock:
            self.readers += 1
            if self.readers == 1:
                await self.global_lock.acquire()
        yield
        async with self.read_lock:
            self.readers -= 1
            if self.readers == 0:
                self.global_lock.release()

    @contextlib.asynccontextmanager
    async def write(self) -> typing.AsyncIterator[None]:
        async with self.global_lock:
            yield


GIT_LOCK = AsyncRWLock()
STATE_LOCK = AsyncRWLock()


class PickUIException(Exception):
    pass


@enum.unique
class NominationType(enum.Enum):

    CC = 0
    FIXES = 1
    REVERT = 2
    NONE = 3


@enum.unique
class Resolution(enum.Enum):

    UNRESOLVED = 0
    MERGED = 1
    DENOMINATED = 2
    BACKPORTED = 3
    NOTNEEDED = 4


async def commit_state(*, amend: bool = False, message: str = 'Update') -> bool:
    """Commit the .pick_status.json file."""
    async with STATE_LOCK.write():
        p = await asyncio.create_subprocess_exec(
            'git', 'add', pick_status_json.as_posix(),
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL,
        )
        v = await p.wait()
        if v != 0:
            return False

        if amend:
            cmd = ['--amend', '--no-edit']
        else:
            cmd = ['--message', f'.pick_status.json: {message}']
        p = await asyncio.create_subprocess_exec(
            'git', 'commit', *cmd,
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL,
        )
        v = await p.wait()
        if v != 0:
            return False
    return True


@attr.s(slots=True)
class Commit:

    sha: str = attr.ib()
    description: str = attr.ib()
    nominated: bool = attr.ib(False)
    nomination_type: NominationType = attr.ib(NominationType.NONE)
    resolution: Resolution = attr.ib(Resolution.UNRESOLVED)
    main_sha: typing.Optional[str] = attr.ib(None)
    because_sha: typing.Optional[str] = attr.ib(None)
    notes: typing.Optional[str] = attr.ib(None)

    def to_json(self) -> 'CommitDict':
        d: typing.Dict[str, typing.Any] = attr.asdict(self)
        d['nomination_type'] = self.nomination_type.value
        if self.resolution is not None:
            d['resolution'] = self.resolution.value
        return typing.cast('CommitDict', d)

    @classmethod
    def from_json(cls, data: 'CommitDict') -> 'Commit':
        c = cls(data['sha'], data['description'], data['nominated'], main_sha=data['main_sha'],
                because_sha=data['because_sha'], notes=data['notes'])
        if (d := data['nomination_type']) is None:
            d = NominationType.NONE.value
        c.nomination_type = NominationType(d)
        if data['resolution'] is not None:
            c.resolution = Resolution(data['resolution'])
        return c

    def date(self) -> str:
        # Show commit date, ie. when the commit actually landed
        # (as opposed to when it was first written)
        return subprocess.check_output(
            ['git', 'show', '--no-patch', '--format=%cs', self.sha],
            stderr=subprocess.DEVNULL
        ).decode("ascii").strip()

    async def apply(self) -> typing.Tuple[bool, str]:
        # FIXME: This isn't really enough if we fail to cherry-pick because the
        # git tree will still be dirty
        # We'll end up with a recursive locking situation here if we take the git lock
        p = await asyncio.create_subprocess_exec(
            'git', 'cherry-pick', '-x', self.sha,
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.PIPE,
        )
        _, err = await p.communicate()

        ret = p.returncode == 0
        if ret:
            self.resolution = Resolution.MERGED
        return ret, err.decode()

    async def abort_cherry(self, ui: 'UI', err: str) -> None:
        await ui.feedback(f'{self.sha} ({self.description}) failed to apply\n{err}')
        async with GIT_LOCK.write():
            p = await asyncio.create_subprocess_exec(
                'git', 'cherry-pick', '--abort',
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.DEVNULL,
            )
            r = await p.wait()
        await ui.feedback(f'{"Successfully" if r == 0 else "Failed to"} abort cherry-pick.')

    async def denominate(self) -> None:
        self.resolution = Resolution.DENOMINATED

    async def backport(self) -> None:
        self.resolution = Resolution.BACKPORTED

    async def resolve(self, ui: 'UI') -> None:
        self.resolution = Resolution.MERGED
        await ui.save()
        v = await commit_state(amend=True)
        assert v
        await ui.feedback(f'{self.sha} ({self.description}) committed successfully')

    async def update_notes(self, ui: 'UI', notes: typing.Optional[str]) -> None:
        self.notes = notes
        await ui.save()
        v = await commit_state(message=f'Updates notes for {self.sha}')
        assert v
        await ui.feedback(f'{self.sha} ({self.description}) notes updated successfully')


async def get_new_commits(sha: str) -> typing.List[typing.Tuple[str, str]]:
    # Try to get the authoritative upstream main
    async with GIT_LOCK.read():
        p = await asyncio.create_subprocess_exec(
            'git', 'for-each-ref', '--format=%(upstream)', 'refs/heads/main',
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL)
        out, _ = await p.communicate()
    upstream = out.decode().strip()

    async with GIT_LOCK.read():
        p = await asyncio.create_subprocess_exec(
            'git', 'log', '--pretty=oneline', f'{sha}..{upstream}',
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL)
        out, _ = await p.communicate()
    assert p.returncode == 0, f"git log didn't work: {sha}"
    return list(split_commit_list(out.decode().strip()))


def split_commit_list(commits: str) -> typing.Generator[typing.Tuple[str, str], None, None]:
    if not commits:
        return
    for line in commits.split('\n'):
        v = tuple(line.split(' ', 1))
        assert len(v) == 2, 'this is really just for mypy'
        yield typing.cast(typing.Tuple[str, str], v)


async def is_commit_in_branch(sha: str) -> bool:
    async with GIT_LOCK.read():
        p = await asyncio.create_subprocess_exec(
            'git', 'merge-base', '--is-ancestor', sha, 'HEAD',
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL,
        )
        await p.wait()
    return p.returncode == 0


async def full_sha(sha: str) -> str:
    async with GIT_LOCK.read():
        p = await asyncio.create_subprocess_exec(
            'git', 'rev-parse', sha,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        out, _ = await p.communicate()
    if p.returncode:
        raise PickUIException(f'Invalid Sha {sha}')
    return out.decode().strip()


async def resolve_nomination(commit: 'Commit', version: str) -> 'Commit':
    async with GIT_LOCK.read():
        p = await asyncio.create_subprocess_exec(
            'git', 'log', '--format=%B', '-1', commit.sha,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        _out, _ = await p.communicate()
        assert p.returncode == 0, f'git log for {commit.sha} failed'
    out = _out.decode()

    # We give precedence to fixes and cc tags over revert tags.
    # XXX: not having the walrus operator available makes me sad :=
    m = IS_FIX.search(out)
    if m:
        # We set the nomination_type and because_sha here so that we can later
        # check to see if this fixes another staged commit.
        try:
            commit.because_sha = fixed = await full_sha(m.group(1))
        except PickUIException:
            pass
        else:
            commit.nomination_type = NominationType.FIXES
            if await is_commit_in_branch(fixed):
                commit.nominated = True
                return commit

    m = IS_CC.search(out)
    if m:
        if m.groups() == (None, None) or version in m.groups():
            commit.nominated = True
            commit.nomination_type = NominationType.CC
            return commit

    m = IS_REVERT.search(out)
    if m:
        # See comment for IS_FIX path
        try:
            commit.because_sha = reverted = await full_sha(m.group(1))
        except PickUIException:
            pass
        else:
            commit.nomination_type = NominationType.REVERT
            if await is_commit_in_branch(reverted):
                commit.nominated = True
                return commit

    return commit


async def changes_commit(commit: Commit, commits: typing.List['Commit']) -> typing.List[Commit]:
    """Find all reverts and fixes for a given commit.

    """
    new_commits: typing.List[Commit] = []
    for c in reversed(commits):
        if c is commit:
            break
        new_commits.append(c)

    ret: typing.List[Commit] = []

    for c in reversed(new_commits):
        if (c.nomination_type in {NominationType.REVERT, NominationType.FIXES} and
                c.because_sha == commit.sha):
            c.nominated = True
            ret.append(c)
    
    return ret


async def gather_commits(version: str, previous: typing.List['Commit'],
                         new: typing.List[typing.Tuple[str, str]], cb) -> typing.List['Commit']:
    # We create an array of the final size up front, then we pass that array
    # to the "inner" co-routine, which is turned into a list of tasks and
    # collected by asyncio.gather. We do this to allow the tasks to be
    # asynchronously gathered, but to also ensure that the commits list remains
    # in order.
    m_commits: typing.List[typing.Optional['Commit']] = [None] * len(new)
    tasks = []

    async def inner(commit: 'Commit', version: str,
                    commits: typing.List[typing.Optional['Commit']],
                    index: int, cb) -> None:
        commits[index] = await resolve_nomination(commit, version)
        cb()

    for i, (sha, desc) in enumerate(new):
        tasks.append(asyncio.ensure_future(
            inner(Commit(sha, desc), version, m_commits, i, cb)))

    await asyncio.gather(*tasks)
    assert None not in m_commits
    commits = typing.cast(typing.List[Commit], m_commits)

    for commit in commits:
        if commit.resolution is Resolution.UNRESOLVED and not commit.nominated:
            commit.resolution = Resolution.NOTNEEDED

    return commits


def load() -> typing.List['Commit']:
    if not pick_status_json.exists():
        return []
    with pick_status_json.open('r') as f:
        raw = json.load(f)
        return [Commit.from_json(c) for c in raw]


def save(commits: typing.Iterable['Commit']) -> None:
    commits = list(commits)
    with pick_status_json.open('wt') as f:
        json.dump([c.to_json() for c in commits], f, indent=4)

    asyncio.ensure_future(commit_state(message=f'Update to {commits[0].sha}'))
