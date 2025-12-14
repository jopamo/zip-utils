#!/usr/bin/env python3
"""
Compare functionality and performance between system zip and build/zip.

This script runs a suite of functional tests and performance benchmarks to compare
the behavior and speed of the system's Info-ZIP `zip` command with the project's
`build/zip` executable.

Usage:
    ./compare_zip.py [--functional] [--performance] [--output <file>]

Options:
    --functional    Run functional parity tests (exit codes, archive validity)
    --performance   Run performance benchmarks (speed, compression ratio)
    --output <file> Write detailed results as JSON to the specified file

Environment variables:
    SYSTEM_ZIP: path to system zip executable (default 'zip')
    BUILD_ZIP: path to project zip executable (default 'build/zip')
"""

import argparse
import os
import subprocess
import tempfile
import time
import json
import sys
import shutil
import zipfile
import random
from pathlib import Path
from dataclasses import dataclass, asdict
from typing import List, Tuple, Optional

@dataclass
class TestResult:
    name: str
    system_exit_code: int
    build_exit_code: int
    system_stdout: str
    build_stdout: str
    system_stderr: str
    build_stderr: str
    system_time: float
    build_time: float
    system_archive_size: int
    build_archive_size: int
    system_archive_valid: bool
    build_archive_valid: bool
    passed: bool
    differences: List[str]

@dataclass
class PerformanceResult:
    dataset: str
    compression_level: int
    system_time: float
    build_time: float
    system_size: int
    build_size: int
    speedup: float  # system_time / build_time
    size_ratio: float  # system_size / build_size

class ZipComparator:
    def __init__(self, system_zip: str, build_zip: str):
        self.system_zip = shutil.which(system_zip) or system_zip
        self.build_zip = str(Path(build_zip).resolve())

    def run_cmd(self, cmd: List[str], cwd: Optional[str] = None) -> Tuple[int, str, str, float]:
        start = time.perf_counter()
        result = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        elapsed = time.perf_counter() - start
        return result.returncode, result.stdout, result.stderr, elapsed

    def verify_archive(self, archive_path: Path) -> bool:
        cmd = ['unzip', '-t', str(archive_path)]
        exit_code, _, _, _ = self.run_cmd(cmd)
        return exit_code <= 1

    # --- Dataset Generators ---

    def create_source_code_dataset(self, root: Path):
        """Simulates a source code tree: many small text files, nested dirs."""
        src_root = root / "src"
        src_root.mkdir()
        code_snippets = [
            "import sys\nimport os\n\ndef main():\n    print('hello')\n",
            "#include <stdio.h>\nint main() { return 0; }\n",
            "const x = 1;\nfunction test() { return true; }\n"
        ]
        for i in range(50):
            d = src_root / f"dir_{i % 5}"
            d.mkdir(exist_ok=True)
            f = d / f"module_{i}.py"
            content = (random.choice(code_snippets) * random.randint(1, 10))
            f.write_text(content)

    def create_log_dataset(self, root: Path):
        """Simulates logs: large files, highly repetitive/compressible text."""
        log_root = root / "logs"
        log_root.mkdir()
        line = "2025-01-01 12:00:00 [INFO] Request ID: 12345 received from IP 192.168.1.1\n"
        content = line * 50000  # ~4MB per file
        for i in range(3):
            (log_root / f"server_{i}.log").write_text(content)

    def create_binary_dataset(self, root: Path):
        """Simulates binary data: random bytes, incompressible."""
        bin_root = root / "bin"
        bin_root.mkdir()
        # 3 files, 2MB each
        for i in range(3):
            (bin_root / f"data_{i}.dat").write_bytes(os.urandom(2 * 1024 * 1024))

    def create_media_dataset(self, root: Path):
        """Simulates media: moderate size files, mostly incompressible."""
        media_root = root / "media"
        media_root.mkdir()
        # 10 files, 500KB each
        for i in range(10):
            (media_root / f"image_{i}.jpg").write_bytes(os.urandom(500 * 1024))

    # --- Functional Tests ---

    def create_functional_files(self, base_dir: Path) -> List[str]:
        # Simple setup for functional logic checks
        for child in base_dir.iterdir():
            if child.is_file(): child.unlink()
            else: shutil.rmtree(child)

        (base_dir / 'file1.txt').write_text('Hello\n' * 50)
        (base_dir / 'file2.txt').write_text('World\n' * 50)
        sub = base_dir / 'subdir'
        sub.mkdir()
        (sub / 'nested.txt').write_text('Nested\n')
        (base_dir / 'random.bin').write_bytes(os.urandom(1024))

        return ['file1.txt', 'file2.txt', 'subdir/nested.txt', 'random.bin']

    def run_functional_test(self, name: str, args: List[str], work_dir: Path, input_files: List[str]) -> TestResult:
        archive_sys = work_dir / 'sys.zip'
        archive_bld = work_dir / 'bld.zip'

        # Special argument ordering for exclude/include
        if name in ('exclude', 'include'):
            cmd_sys = [self.system_zip, str(archive_sys)] + input_files + args
            cmd_bld = [self.build_zip, str(archive_bld)] + input_files + args
        else:
            cmd_sys = [self.system_zip, str(archive_sys)] + args + input_files
            cmd_bld = [self.build_zip, str(archive_bld)] + args + input_files

        # Handle tests that modify existing files
        modifying = any(x in args for x in ['-m', '-u', '-f', '-FS'])
        backup = None
        if modifying:
            backup = work_dir / '.backup'
            shutil.copytree(work_dir, backup, dirs_exist_ok=True)

        # Run System
        sys_c, sys_o, sys_e, sys_t = self.run_cmd(cmd_sys, cwd=str(work_dir))
        sys_sz = archive_sys.stat().st_size if archive_sys.exists() else 0
        sys_ok = self.verify_archive(archive_sys) if sys_c == 0 else False

        # Reset for Build
        if backup:
            # Wipe and restore
            for item in work_dir.iterdir():
                if item.name not in ('.backup', 'sys.zip'):
                    if item.is_file(): item.unlink()
                    else: shutil.rmtree(item)
            for item in backup.iterdir():
                if item.name == 'sys.zip': continue
                dest = work_dir / item.name
                if item.is_file(): shutil.copy2(item, dest)
                else: shutil.copytree(item, dest, dirs_exist_ok=True)
            shutil.rmtree(backup)

        # Run Build
        bld_c, bld_o, bld_e, bld_t = self.run_cmd(cmd_bld, cwd=str(work_dir))
        bld_sz = archive_bld.stat().st_size if archive_bld.exists() else 0
        bld_ok = self.verify_archive(archive_bld) if bld_c == 0 else False

        diffs = []
        passed = True
        if sys_c != bld_c:
            diffs.append(f"Exit code: sys={sys_c} build={bld_c}")
            passed = False
        if sys_ok != bld_ok:
            diffs.append(f"Validity: sys={sys_ok} build={bld_ok}")
            passed = False

        # Cleanup
        if archive_sys.exists(): archive_sys.unlink()
        if archive_bld.exists(): archive_bld.unlink()

        return TestResult(name, sys_c, bld_c, sys_o, bld_o, sys_e, bld_e, sys_t, bld_t,
                          sys_sz, bld_sz, sys_ok, bld_ok, passed, diffs)

    def run_functional_suite(self) -> List[TestResult]:
        results = []
        with tempfile.TemporaryDirectory() as tmp:
            tpath = Path(tmp)
            cases = [
                ("basic", []),
                ("recurse", ["-r"]),
                ("junk_paths", ["-j"]),
                ("store", ["-0"]),
                ("fast", ["-1"]),
                ("best", ["-9"]),
                ("exclude", ["-x", "*.bin"]),
                ("include", ["-i", "*.txt"]),
                ("update", ["-u"]),
                ("freshen", ["-f"]),
                ("filesync", ["-FS"]),
                ("test_opt", ["-T"]),
                ("time_filter", ["-t", "20250101"]),
            ]
            for name, args in cases:
                print(f"Functional: {name}", file=sys.stderr)
                files = self.create_functional_files(tpath)
                results.append(self.run_functional_test(name, args, tpath, files))
        return results

    # --- Performance Benchmarks ---

    def run_benchmark(self, dataset_name: str, level: int, work_dir: Path) -> PerformanceResult:
        sys_zip = work_dir / "sys.zip"
        bld_zip = work_dir / "bld.zip"
        args = [f"-{level}", "-r", "."]

        def time_it(cmd):
            times = []
            for _ in range(3):
                if sys_zip.exists(): sys_zip.unlink()
                if bld_zip.exists(): bld_zip.unlink()
                start = time.perf_counter()
                subprocess.run(cmd, cwd=str(work_dir), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                times.append(time.perf_counter() - start)
            return min(times) # Best time

        sys_cmd = [self.system_zip, str(sys_zip)] + args
        bld_cmd = [self.build_zip, str(bld_zip)] + args

        sys_t = time_it(sys_cmd)
        sys_sz = sys_zip.stat().st_size

        bld_t = time_it(bld_cmd)
        bld_sz = bld_zip.stat().st_size

        return PerformanceResult(
            dataset=dataset_name,
            compression_level=level,
            system_time=sys_t,
            build_time=bld_t,
            system_size=sys_sz,
            build_size=bld_sz,
            speedup=(sys_t / bld_t) if bld_t > 0 else 0,
            size_ratio=(sys_sz / bld_sz) if bld_sz > 0 else 0
        )

    def run_performance_suite(self) -> List[PerformanceResult]:
        results = []
        datasets = [
            ("Source Code", self.create_source_code_dataset),
            ("Logs", self.create_log_dataset),
            ("Binary", self.create_binary_dataset),
            ("Media", self.create_media_dataset),
        ]

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)

            for name, generator in datasets:
                print(f"Generating dataset: {name}...", file=sys.stderr)
                # Clear and generate
                for item in root.iterdir():
                    if item.is_file(): item.unlink()
                    else: shutil.rmtree(item)
                generator(root)

                for level in [1, 6, 9]:
                    print(f"  Benchmark {name} level {level}...", file=sys.stderr)
                    res = self.run_benchmark(name, level, root)
                    results.append(res)

        return results

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--functional', action='store_true')
    parser.add_argument('--performance', action='store_true')
    parser.add_argument('--output', type=str)
    args = parser.parse_args()

    if not (args.functional or args.performance):
        parser.error("Specify --functional or --performance")

    sys_zip = os.environ.get('SYSTEM_ZIP', 'zip')
    bld_zip = os.environ.get('BUILD_ZIP', 'build/zip')

    if not shutil.which(sys_zip):
        sys.exit(f"System zip '{sys_zip}' not found.")
    if not Path(bld_zip).exists():
        sys.exit(f"Build zip '{bld_zip}' not found.")

    comp = ZipComparator(sys_zip, bld_zip)
    out_data = {}

    if args.functional:
        res = comp.run_functional_suite()
        out_data['functional'] = [asdict(r) for r in res]
        passed = sum(1 for r in res if r.passed)
        print(f"\nFunctional: {passed}/{len(res)} passed", file=sys.stderr)
        for r in res:
            if not r.passed:
                print(f"FAILED: {r.name} -> {r.differences}", file=sys.stderr)

    if args.performance:
        res = comp.run_performance_suite()
        out_data['performance'] = [asdict(r) for r in res]
        print(f"\n{'Dataset':<12} | {'Lvl':<3} | {'Sys Time':<8} | {'Bld Time':<8} | {'Speedup':<7} | {'Size Ratio':<10}", file=sys.stderr)
        print("-" * 75, file=sys.stderr)
        for r in res:
            print(f"{r.dataset:<12} | {r.compression_level:<3} | {r.system_time:.3f}s   | {r.build_time:.3f}s   | {r.speedup:.2f}x   | {r.size_ratio:.3f}", file=sys.stderr)

    if args.output:
        with open(args.output, 'w') as f:
            json.dump(out_data, f, indent=2)
        print(f"\nSaved results to {args.output}", file=sys.stderr)

if __name__ == '__main__':
    main()
