#!/usr/bin/env python3

import os
import subprocess
import tempfile
import zipfile
from pathlib import Path


def run(cmd, cwd=None, text=True, input_data=None):
    return subprocess.run(cmd, cwd=cwd, input=input_data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=text)


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
    if zip_bin is None:
        zip_bin = str(Path(__file__).resolve().parents[1] / 'build' / 'zip')

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        archive = work / 'entry-comments.zip'

        # Seed an archive with an entry comment using the stdlib writer.
        zi = zipfile.ZipInfo('entry.txt')
        zi.comment = b'hello entry comment'
        with zipfile.ZipFile(archive, 'w') as zf:
            zf.writestr(zi, 'initial payload')

        # Add a new file via the CLI to force a rewrite.
        extra = work / 'extra.txt'
        extra.write_text('extra payload')
        res_add = run([zip_bin, str(archive), extra.name], cwd=work)
        if res_add.returncode != 0:
            raise SystemExit(f"zip add failed: {res_add.stderr or res_add.stdout}")

        # Update the original entry and ensure its comment is preserved.
        entry_src = work / 'entry.txt'
        entry_src.write_text('updated payload')
        res_update = run([zip_bin, str(archive), entry_src.name], cwd=work)
        if res_update.returncode != 0:
            raise SystemExit(f"zip update failed: {res_update.stderr or res_update.stdout}")

        with zipfile.ZipFile(archive, 'r') as zf:
            info = zf.getinfo('entry.txt')
            if info.comment != b'hello entry comment':
                raise SystemExit(f"entry comment lost: {info.comment!r}")
            if zf.read('entry.txt') != b'updated payload':
                raise SystemExit("entry payload incorrect after update")
            if 'extra.txt' not in zf.namelist():
                raise SystemExit("extra.txt missing after rewrite")

        zipnote_bin = os.environ.get('ZIPNOTE_BIN')
        if not zipnote_bin or Path(zipnote_bin).name != 'zipnote':
            zipnote_bin = make_zipnote_alias(zip_bin, work)

        note_res = run([zipnote_bin, str(archive)], text=True)
        if note_res.returncode != 0:
            raise SystemExit(f"zipnote failed: {note_res.stderr or note_res.stdout}")
        if 'hello entry comment' not in note_res.stdout:
            raise SystemExit(f"zipnote output missing entry comment: {note_res.stdout}")


if __name__ == '__main__':
    main()
