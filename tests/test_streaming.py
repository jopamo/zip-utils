#!/usr/bin/env python3

import os
import subprocess
import tempfile
import sys
from pathlib import Path
import zipfile


def run_pipe(cmd, input_data, cwd=None):
    return subprocess.run(cmd, input=input_data, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=False)


def main():
    zip_bin = str(Path(__file__).resolve().parents[1] / 'build-meson-asan' / 'zip')
    zip_bin = os.path.abspath(zip_bin)
    unzip_bin = os.environ.get('UNZIP_BIN', 'unzip')

    if not os.path.exists(zip_bin):
        print(f"Zip binary not found at {zip_bin}")
        sys.exit(1)

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        archive = tmp_path / 'stream_in.zip'
        
        # Test 1: Stdin to File
        input_content = b"stream me content"
        
        cmd = [zip_bin, str(archive), '-']
        res = run_pipe(cmd, input_content, cwd=tmp_path)
        
        if res.stderr:
            print(f"Test 1 Zip Stderr:\n{res.stderr.decode()}")

        if res.returncode != 0:
            print(f"Test 1 failed: zip command returned non-zero exit code {res.returncode}")
            sys.exit(1)
            
        if not archive.exists() or archive.stat().st_size == 0:
            print(f"Test 1 failed: Archive file {archive} does not exist or is empty.")
            sys.exit(1)
            
        # Verify content
        import pdb; pdb.set_trace()
        try:
            with zipfile.ZipFile(archive, 'r') as zf:
                info = zf.getinfo('-')
                if not (info.flag_bits & 0x08):
                     print("Test 1 Warning: Bit 3 (Data Descriptor) not set for streaming input")
                     sys.exit(1)
                
                content = zf.read('-')
                if content != input_content:
                    print(f"Test 1 Content mismatch: {content} != {input_content}")
                    sys.exit(1)
        except Exception as e:
            print(f"Test 1 Verification failed: {e}")
            sys.exit(1)

        print("Test 1 (Stdin -> File) Passed")

        # Test 2: Stdin to Stdout
        archive_out = tmp_path / 'stream_out.zip'
        input_content_2 = b"stream me too content"
        
        cmd2 = [zip_bin, '-', '-']
        res2 = run_pipe(cmd2, input_content_2, cwd=tmp_path)
        
        if res2.stderr:
            print(f"Test 2 Zip Stderr:\n{res2.stderr.decode()}")
        if res2.returncode != 0:
            print(f"Test 2 failed: zip command returned non-zero exit code {res2.returncode}")
            sys.exit(1)
            
        with open(archive_out, 'wb') as f:
            f.write(res2.stdout)
        
        if not archive_out.exists() or archive_out.stat().st_size == 0:
            print(f"Test 2 failed: Archive file {archive_out} does not exist or is empty.")
            sys.exit(1)
            
        try:
            with zipfile.ZipFile(archive_out, 'r') as zf:
                info = zf.getinfo('-')
                if not (info.flag_bits & 0x08):
                     print("Test 2 Warning: Bit 3 not set")
                     sys.exit(1)
                
                content = zf.read('-')
                if content != input_content_2:
                    print(f"Test 2 Content mismatch: {content} != {input_content_2}")
                    sys.exit(1)
        except Exception as e:
            print(f"Test 2 Verification failed: {e}")
            sys.exit(1)

        print("Test 2 (Stdin -> Stdout) Passed")

if __name__ == '__main__':
    main()