#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path
import zipfile
import sys

def run(cmd, cwd=None, stdin=None):
    return subprocess.run(cmd, cwd=cwd, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=False if stdin else True)

def main():
    zip_bin = os.environ.get('WRITE_BIN')
    if zip_bin is None:
        zip_bin = str(Path(__file__).resolve().parents[2] / 'build' / 'zip')
    unzip_bin = os.environ.get('UNZIP_REWRITE_BIN')
    if unzip_bin is None:
        unzip_bin = str(Path(__file__).resolve().parents[2] / 'build' / 'unzip')

    if not os.path.exists(zip_bin):
        print(f"Skipping: {zip_bin} not found")
        sys.exit(0)

    print(f"Testing recovery with {zip_bin} and {unzip_bin}")

    # Test 1: Simple recovery (-F) of truncated EOCD
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        
        payload_dir = tmp_path / 'payload'
        payload_dir.mkdir()
        (payload_dir / 'a.txt').write_text('content A' * 100)
        (payload_dir / 'b.txt').write_text('content B' * 100)
        
        archive = tmp_path / 'broken.zip'
        fixed = tmp_path / 'fixed.zip'
        
        run([zip_bin, str(archive), 'payload/a.txt', 'payload/b.txt'], cwd=tmp_path)
        
        # Corrupt the archive by chopping off the last 100 bytes (likely CD and EOCD)
        data = archive.read_bytes()
        if len(data) > 100:
            truncated_data = data[:-100]
            archive.write_bytes(truncated_data)
        
        # Run -F
        res = run([zip_bin, '-F', str(archive), '--out', str(fixed)], cwd=tmp_path)
        if res.returncode != 0:
            print("STDOUT:", res.stdout)
            print("STDERR:", res.stderr)
            sys.exit(f"-F failed with {res.returncode}")
            
        # Verify fixed.zip
        res = run([unzip_bin, '-t', str(fixed)])
        if res.returncode != 0:
             print("STDOUT:", res.stdout)
             print("STDERR:", res.stderr)
             sys.exit(f"-F produced invalid zip")
             
        print("PASS: -F recovery", flush=True)

    # Test 2: Full scan (-FF) with stripped CD
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        payload_dir = tmp_path / 'payload'
        payload_dir.mkdir()
        (payload_dir / 'a.txt').write_text('content A' * 100)
        
        archive = tmp_path / 'messy.zip'
        fixed = tmp_path / 'fixed_messy.zip'
        
        run([zip_bin, str(archive), 'payload/a.txt'], cwd=tmp_path)
        
        data = archive.read_bytes()
        cd_sig = b'PK\x01\x02'
        cd_pos = data.find(cd_sig)
        if cd_pos != -1:
             corrupted = data[:cd_pos] # Keep locals, drop CD and EOCD
             archive.write_bytes(corrupted)
        
        res = run([zip_bin, '-FF', str(archive), '--out', str(fixed)], cwd=tmp_path)
        if res.returncode != 0:
            print("STDOUT:", res.stdout)
            print("STDERR:", res.stderr)
            sys.exit(f"-FF failed with {res.returncode}")

        res = run([unzip_bin, '-t', str(fixed)])
        if res.returncode != 0:
             sys.exit(f"-FF produced invalid zip: {res.stderr}")

        print("PASS: -FF recovery", flush=True)

    # Test 3: Data Descriptor recovery
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        
        archive = tmp_path / 'streamed.zip'
        fixed = tmp_path / 'fixed_streamed.zip'
        
        input_data = b'streamed content ' * 500
        
        # create archive with data descriptor by streaming stdin
        # zip streamed.zip -
        res = run([zip_bin, str(archive), '-'], cwd=tmp_path, stdin=input_data)
        if res.returncode != 0:
            sys.exit("Failed to create streamed zip")

        # Now corrupt it. Remove CD/EOCD.
        data = archive.read_bytes()
        cd_pos = data.find(b'PK\x01\x02')
        if cd_pos != -1:
            corrupted = data[:cd_pos]
            archive.write_bytes(corrupted)
        else:
            # Maybe it's small enough or different?
            # If we can't find CD, assume it's already weird, but let's just chop end
            if len(data) > 100:
                archive.write_bytes(data[:-50])
            
        # Recover.
        print("Running zip -FF on streamed.zip...", flush=True)
        res = run([zip_bin, '-FF', str(archive), '--out', str(fixed)], cwd=tmp_path)
        if res.returncode != 0:
            print("STDOUT:", res.stdout)
            print("STDERR:", res.stderr)
            sys.exit(f"-FF data descriptor recovery failed with {res.returncode}")
            
        print("Running unzip -t on fixed_streamed.zip...", flush=True)
        res = run([unzip_bin, '-t', str(fixed)])
        if res.returncode != 0:
             sys.exit(f"-FF (dd) produced invalid zip: {res.stderr}")
             
        # Check content
        with zipfile.ZipFile(fixed, 'r') as zf:
            # The name used for stdin input is "-"
            if zf.read('-') != input_data:
                 sys.exit("-FF (dd) content mismatch")
                 
        print("PASS: Data Descriptor recovery")

if __name__ == '__main__':
    main()
