#!/usr/bin/env python3
import os, sys, tempfile, subprocess, shutil
from pathlib import Path

system_zip = shutil.which('zip')
build_zip = str(Path('build/zip').resolve())

with tempfile.TemporaryDirectory() as tmp:
    tmp_path = Path(tmp)
    # Create test files as in compare_zip.py
    (tmp_path / 'file1.txt').write_text('Hello, world!\n' * 100)
    (tmp_path / 'file2.txt').write_text('Another file with some content.\n' * 50)
    sub = tmp_path / 'subdir'
    sub.mkdir()
    (sub / 'nested.txt').write_text('Nested file content.\n')
    (tmp_path / 'empty_dir').mkdir()
    import random
    random_data = bytes(random.getrandbits(8) for _ in range(1024))
    (tmp_path / 'random.bin').write_bytes(random_data)

    input_files = ['file1.txt', 'file2.txt', 'subdir/nested.txt', 'random.bin']

    # Test exclude
    archive_system = tmp_path / 'system.zip'
    archive_build = tmp_path / 'build.zip'

    cmd_system = [system_zip, str(archive_system), '-x', '*.bin'] + input_files
    cmd_build = [build_zip, str(archive_build), '-x', '*.bin'] + input_files

    print('System command:', ' '.join(cmd_system))
    print('Build command:', ' '.join(cmd_build))
    print('Working dir:', tmp_path)

    # Run system zip
    result_system = subprocess.run(cmd_system, cwd=str(tmp_path), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    print('\n=== SYSTEM ZIP ===')
    print('Exit code:', result_system.returncode)
    print('Stdout:', result_system.stdout)
    print('Stderr:', result_system.stderr)
    if archive_system.exists():
        print('Archive size:', archive_system.stat().st_size)
        # Verify
        verify = subprocess.run(['unzip', '-t', str(archive_system)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        print('Verify exit:', verify.returncode)
    else:
        print('Archive not created')

    # Run build zip
    result_build = subprocess.run(cmd_build, cwd=str(tmp_path), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    print('\n=== BUILD ZIP ===')
    print('Exit code:', result_build.returncode)
    print('Stdout:', result_build.stdout)
    print('Stderr:', result_build.stderr)
    if archive_build.exists():
        print('Archive size:', archive_build.stat().st_size)
        verify = subprocess.run(['unzip', '-t', str(archive_build)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        print('Verify exit:', verify.returncode)
    else:
        print('Archive not created')

    # List contents
    if archive_system.exists():
        subprocess.run(['unzip', '-l', str(archive_system)])
    if archive_build.exists():
        subprocess.run(['unzip', '-l', str(archive_build)])