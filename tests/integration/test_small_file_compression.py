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

        tiny = work / "tiny.txt"
        tiny.write_text("int main() { return 0; }\n" * 10)
        archive = work / "tiny.zip"

        res = run([zip_bin, str(archive), tiny.name], cwd=work)
        if res.returncode != 0:
            raise SystemExit(f"zip failed: {res.stderr or res.stdout}")

        with zipfile.ZipFile(archive, "r") as zf:
            info = zf.getinfo("tiny.txt")
            if info.compress_type != zipfile.ZIP_DEFLATED:
                raise SystemExit("tiny.txt should be deflated even when small")
            if info.compress_size >= info.file_size:
                raise SystemExit("deflate should reduce tiny.txt size")

        noise = work / "noise.bin"
        noise.write_bytes(os.urandom(512))
        archive2 = work / "noise.zip"

        res = run([zip_bin, str(archive2), noise.name], cwd=work)
        if res.returncode != 0:
            raise SystemExit(f"zip failed: {res.stderr or res.stdout}")

        with zipfile.ZipFile(archive2, "r") as zf:
            info = zf.getinfo("noise.bin")
            if info.compress_type != zipfile.ZIP_STORED and info.compress_size >= info.file_size:
                raise SystemExit("noise.bin should be stored when compression is not effective")


if __name__ == "__main__":
    main()
