#!/usr/bin/env python3

import os
import subprocess
import tempfile
import zipfile
from pathlib import Path


def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def main():
    zip_bin = os.environ.get("WRITE_BIN")
    if not zip_bin:
        zip_bin = str(Path(__file__).resolve().parents[1] / "build" / "zip")

    with tempfile.TemporaryDirectory() as tmpdir:
        work = Path(tmpdir)
        (work / "store.txt").write_text("store me\n")
        (work / "photo.jpg").write_bytes(b"\xff\xd8fakejpeg")
        (work / "data.bin").write_bytes(b"\x00\x01\x02compressme")

        archive = work / "no-compress.zip"
        res = run(
            [zip_bin, "-n", ".txt:jpg", str(archive), "store.txt", "photo.jpg", "data.bin"],
            cwd=work,
        )
        if res.returncode != 0:
            raise SystemExit(f"-n suffix list failed: {res.stderr or res.stdout}")

        with zipfile.ZipFile(archive, "r") as zf:
            txt_info = zf.getinfo("store.txt")
            jpg_info = zf.getinfo("photo.jpg")
            bin_info = zf.getinfo("data.bin")

            if txt_info.compress_type != zipfile.ZIP_STORED:
                raise SystemExit("store.txt should be stored without compression")
            if jpg_info.compress_type != zipfile.ZIP_STORED:
                raise SystemExit("photo.jpg should be stored without compression")
            if bin_info.compress_type == zipfile.ZIP_STORED:
                raise SystemExit("data.bin should still be compressed by default")


if __name__ == "__main__":
    main()
