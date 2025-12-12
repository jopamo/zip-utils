#!/usr/bin/env python3

import os
import shutil
import struct
import subprocess
import tempfile
import zipfile
from pathlib import Path

ATTR_TAGS = {0x5455, 0x5855, 0x7875}


def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def parse_local_extra(zip_path: Path, name: str) -> bytes:
    with open(zip_path, "rb") as f:
        data = f.read()
    with zipfile.ZipFile(zip_path) as zf:
        info = zf.getinfo(name)
        offset = info.header_offset
    sig, ver, flags, method, mtime, mdate, crc, csize, usize, name_len, extra_len = struct.unpack_from(
        "<IHHHHHIIIHH", data, offset
    )
    if sig != 0x04034B50:
        raise SystemExit("Invalid local header signature")
    start = offset + 30 + name_len
    return data[start : start + extra_len]


def extra_tags(extra: bytes):
    tags = []
    pos = 0
    while pos + 4 <= len(extra):
        tag = int.from_bytes(extra[pos : pos + 2], "little")
        size = int.from_bytes(extra[pos + 2 : pos + 4], "little")
        end = pos + 4 + size
        if end > len(extra):
            break
        tags.append(tag)
        pos = end
    return tags


def main():
    write_bin = os.environ.get("WRITE_BIN")
    if write_bin is None:
        write_bin = str(Path(__file__).resolve().parents[1] / "build" / "zip")
    else:
        write_bin = str(Path(write_bin).resolve())

    sys_zip = os.environ.get("SYS_ZIP_BIN")
    if sys_zip:
        sys_zip = shutil.which(sys_zip) or str(Path(sys_zip).resolve())
    else:
        sys_zip = shutil.which("zip") or "zip"

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)

        (tmp_path / "keep.txt").write_text("keep")
        base = tmp_path / "base.zip"

        res = run([sys_zip, str(base), "keep.txt"], cwd=tmp_path)
        if res.returncode != 0:
            raise SystemExit(f"system zip failed: {res.stderr}")

        orig_extra = parse_local_extra(base, "keep.txt")
        orig_tags = extra_tags(orig_extra)
        if not any(tag in ATTR_TAGS for tag in orig_tags):
            raise SystemExit("system zip did not add expected attribute extras")

        (tmp_path / "add.txt").write_text("add")
        res = run([write_bin, "-X", str(base), "add.txt"], cwd=tmp_path)
        if res.returncode != 0:
            raise SystemExit(f"zip -X failed: {res.stderr}")

        new_extra = parse_local_extra(base, "keep.txt")
        new_tags = extra_tags(new_extra)
        if any(tag in ATTR_TAGS for tag in new_tags):
            raise SystemExit("attribute extras were not stripped from existing entry")


if __name__ == "__main__":
    main()
