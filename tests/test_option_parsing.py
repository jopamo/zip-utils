#!/usr/bin/env python3

import os
import subprocess
import tempfile
import zipfile
from pathlib import Path


def run(cmd, cwd=None, input_data: bytes | None = None, text: bool = False):
    return subprocess.run(
        cmd,
        cwd=cwd,
        input=input_data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=text,
    )


def main():
    zip_bin = os.environ.get("WRITE_BIN")
    if not zip_bin:
        zip_bin = str(Path(__file__).resolve().parents[1] / "build" / "zip")

    with tempfile.TemporaryDirectory() as tmpdir:
        work = Path(tmpdir)

        # Cluster pure flags and a compression digit.
        data = work / "data.txt"
        data.write_text("cluster me\n")
        archive = work / "cluster.zip"
        res = run([zip_bin, "-rq9", str(archive), data.name], cwd=work)
        if res.returncode != 0:
            raise SystemExit(f"-rq9 failed: {res.stderr.decode() or res.stdout.decode()}")
        with zipfile.ZipFile(archive, "r") as zf:
            if zf.namelist() != [data.name]:
                raise SystemExit("clustered flags archive contents mismatch")

        # -x list should stop at the next option marker (here --) and exclude skip.txt.
        keep = work / "keep.txt"
        skip = work / "skip.txt"
        keep.write_text("keep me")
        skip.write_text("skip me")
        archive2 = work / "exclude.zip"
        res2 = run(
            [zip_bin, "-x", "skip.txt", "--", str(archive2), keep.name, skip.name],
            cwd=work,
        )
        if res2.returncode != 0:
            raise SystemExit(f"-x list parse failed: {res2.stderr.decode() or res2.stdout.decode()}")
        with zipfile.ZipFile(archive2, "r") as zf:
            names = zf.namelist()
            if skip.name in names or keep.name not in names:
                raise SystemExit("exclude list was not honored")

        # Clustered -xpattern form should be accepted and skip matching names.
        archive3 = work / "badcluster.zip"
        res3 = run([zip_bin, str(archive3), keep.name, skip.name, "-xskip.txt"], cwd=work)
        if res3.returncode != 0:
            raise SystemExit(f"clustered -xparse failed: {res3.stderr.decode() or res3.stdout.decode()}")
        with zipfile.ZipFile(archive3, "r") as zf:
            names = zf.namelist()
            if skip.name in names or keep.name not in names:
                raise SystemExit("clustered -x did not apply exclusion")

        # Reject the undocumented -xi combined token.
        archive4 = work / "badxi.zip"
        res4 = run([zip_bin, "-xi", str(archive4), keep.name], cwd=work)
        if res4.returncode == 0:
            raise SystemExit("-xi should be rejected but command succeeded")

        # Filter mode: no archive or inputs -> read stdin, write archive to stdout.
        filter_out = run([zip_bin, "-q"], input_data=b"stdin-bytes\n")
        if filter_out.returncode != 0:
            raise SystemExit(f"filter mode failed: {filter_out.stderr.decode() if filter_out.stderr else ''}")
        if not filter_out.stdout:
            raise SystemExit("filter mode produced no output")
        out_path = work / "filter.zip"
        out_path.write_bytes(filter_out.stdout)
        with zipfile.ZipFile(out_path, "r") as zf:
            names = zf.namelist()
            if names != ["-"]:
                raise SystemExit(f"filter mode entry names mismatch: {names}")
            if zf.read("-") != b"stdin-bytes\n":
                raise SystemExit("filter mode content mismatch")


if __name__ == "__main__":
    main()
