#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path
import zipfile


def zip_bin_path() -> str:
    override = os.environ.get('WRITE_BIN')
    if override:
        return override
    return str(Path(__file__).resolve().parents[1] / 'build' / 'zip')


def run_zip(cmd, input_data: bytes, cwd: Path):
    return subprocess.run(cmd, input=input_data, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=False)


def assert_stream_archive(path: Path, expected: bytes):
    with zipfile.ZipFile(path, 'r') as zf:
        info = zf.getinfo('-')
        if (info.flag_bits & 0x08) == 0:
            raise SystemExit("data descriptor flag missing on streamed entry")
        content = zf.read('-')
        if content != expected:
            raise SystemExit(f"content mismatch: {content!r} != {expected!r}")
        if info.compress_type != zipfile.ZIP_DEFLATED:
            raise SystemExit(f"unexpected compression method: {info.compress_type}")


def main():
    zip_path = Path(zip_bin_path())
    if not zip_path.exists():
        raise SystemExit(f"zip binary not found at {zip_path}")

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)

        data1 = b"stream me content\n"
        archive = tmp_path / 'stream_in.zip'
        res1 = run_zip([str(zip_path), str(archive), '-'], data1, tmp_path)
        if res1.returncode != 0:
            raise SystemExit(f"zip stdin->file failed: {res1.stderr.decode()}")
        if not archive.exists() or archive.stat().st_size == 0:
            raise SystemExit("archive file missing after stdin stream write")
        assert_stream_archive(archive, data1)

        data2 = b"stream me too content\n"
        res2 = run_zip([str(zip_path), '-', '-'], data2, tmp_path)
        if res2.returncode != 0:
            raise SystemExit(f"zip stdin->stdout failed: {res2.stderr.decode()}")
        out_path = tmp_path / 'stream_out.zip'
        out_path.write_bytes(res2.stdout)
        assert_stream_archive(out_path, data2)


if __name__ == '__main__':
    main()
