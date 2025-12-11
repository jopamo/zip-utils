#!/usr/bin/env python3

import os
import subprocess
import tempfile
import zipfile
from pathlib import Path


def run(cmd, cwd=None, text=True, input_data=None):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=text, input=input_data)


def make_zipnote_alias(zip_bin: str, work: Path) -> str:
    link = work / 'zipnote'
    if not link.exists():
        try:
            link.symlink_to(Path(zip_bin))
        except OSError as exc:
            raise SystemExit(f"failed to create zipnote alias: {exc}") from exc
    return str(link)


def main():
    zip_bin = os.environ.get('WRITE_BIN')
    if not zip_bin:
        zip_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'zip')

    with tempfile.TemporaryDirectory() as tmpdir:
        work = Path(tmpdir)
        archive = work / 'notes.zip'

        with zipfile.ZipFile(archive, 'w') as zf:
            info1 = zipfile.ZipInfo('file1.txt')
            info1.comment = b'old comment'
            zf.writestr(info1, 'payload1')
            zf.writestr('file2.txt', 'payload2')

        zipnote_bin = os.environ.get('ZIPNOTE_BIN')
        if not zipnote_bin or Path(zipnote_bin).name != 'zipnote':
            zipnote_bin = make_zipnote_alias(zip_bin, work)

        note_text = """@ file1.txt
new comment 1
@@leading at
@
@ file2.txt
second comment
@
@ (zip file comment below this line)
archive note
@
"""
        apply_res = run([zipnote_bin, '-w', str(archive)], cwd=work, input_data=note_text, text=True)
        if apply_res.returncode != 0:
            raise SystemExit(f"zipnote -w failed: {apply_res.stderr or apply_res.stdout}")

        with zipfile.ZipFile(archive, 'r') as zf:
            info1 = zf.getinfo('file1.txt')
            info2 = zf.getinfo('file2.txt')
            if info1.comment != b'new comment 1\n@leading at\n':
                raise SystemExit(f"file1 comment mismatch: {info1.comment!r}")
            if info2.comment != b'second comment\n':
                raise SystemExit(f"file2 comment mismatch: {info2.comment!r}")
            if zf.comment != b'archive note\n':
                raise SystemExit(f"archive comment mismatch: {zf.comment!r}")

        list_res = run([zipnote_bin, str(archive)], cwd=work, text=True)
        if list_res.returncode != 0:
            raise SystemExit(f"zipnote list failed: {list_res.stderr or list_res.stdout}")
        if '@ file1.txt' not in list_res.stdout or '@ file2.txt' not in list_res.stdout:
            raise SystemExit(f"zipnote list missing entries: {list_res.stdout}")
        if 'archive note' not in list_res.stdout:
            raise SystemExit(f"zipnote list missing archive comment: {list_res.stdout}")


if __name__ == '__main__':
    main()
