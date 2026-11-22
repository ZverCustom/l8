"""Build identity for both Git checkouts and exported source archives."""
from pathlib import Path
import subprocess


def build_identity(root):
    root = Path(root).resolve()
    revision = 'unknown'
    try:
        def git(*args):
            return subprocess.check_output(['git', *args], cwd=root, text=True,
                                           stderr=subprocess.DEVNULL).strip()
        # An archive unpacked inside another checkout must not borrow its identity.
        if Path(git('rev-parse', '--show-toplevel')).resolve() == root:
            revision = git('describe', '--tags', '--always', '--dirty') or 'unknown'
    except (OSError, subprocess.CalledProcessError):
        pass  # Git missing, .git absent, or inaccessible metadata: valid archive build.
    return revision, root.joinpath('VERSION').read_text().strip() + '+' + revision
