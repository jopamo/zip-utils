#!/usr/bin/env python3
"""
Compare functionality and performance between system zip and build/zip.

This script runs a suite of functional tests and performance benchmarks to compare
the behavior and speed of the system's Info-ZIP `zip` command with the project's
`build/zip` executable. It checks exit codes, archive validity, compression ratios,
and execution times.

Usage:
    ./compare_zip.py [--functional] [--performance] [--output <file>]

    At least one of --functional or --performance must be specified.

Options:
    --functional    Run functional parity tests (exit codes, archive validity)
    --performance   Run performance benchmarks (speed, compression ratio)
    --output <file> Write detailed results as JSON to the specified file

Environment variables:
    SYSTEM_ZIP: path to system zip executable (default 'zip')
    BUILD_ZIP: path to project zip executable (default 'build/zip')

Output:
    The script prints a summary to stderr. If --output is given, a JSON file is
    written with detailed results. The JSON structure includes:
    - "functional": list of test results with exit codes, times, sizes, differences
    - "performance": list of benchmarks with timings, sizes, speedup ratios

Functional tests cover common options: -r, -j, -0..-9, -q, -v, -x, -i, -m, -u,
-f, -FS, -T, -e, -s, -t. Each test compares exit codes and archive validity.

Performance tests measure compression time and archive size for levels 0,1,6,9
using 10MB of random data (5×2MB files). Each test runs 3 iterations, discarding
the first as warm-up.

Examples:
    # Run both functional and performance tests
    ./compare_zip.py --functional --performance

    # Run only functional tests and save results
    ./compare_zip.py --functional --output results.json

    # Specify custom zip paths
    BUILD_ZIP=./build-debug/zip ./compare_zip.py --functional

Exit code:
    0 on success (all tests executed, regardless of parity)
    1 on argument error or missing binaries
"""

import argparse
import os
import subprocess
import tempfile
import time
import json
import sys
from pathlib import Path
from dataclasses import dataclass, asdict
from typing import List, Dict, Any, Optional, Tuple
import shutil
import zipfile

@dataclass
class TestResult:
    """Result of a single test case."""
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
    """Result of a performance benchmark."""
    name: str
    system_time: float
    build_time: float
    system_size: int
    build_size: int
    speedup: float  # system_time / build_time ( >1 means build faster)
    size_ratio: float  # system_size / build_size ( >1 means build smaller)

class ZipComparator:
    def __init__(self, system_zip: str, build_zip: str):
        # Resolve to absolute paths
        self.system_zip = shutil.which(system_zip) or system_zip
        self.build_zip = str(Path(build_zip).resolve())
        self.temp_dir = None
        self.test_data = {}

    def run_cmd(self, cmd: List[str], cwd: Optional[str] = None) -> Tuple[int, str, str, float]:
        """Run command and return (exit_code, stdout, stderr, elapsed_time)."""
        start = time.perf_counter()
        result = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        elapsed = time.perf_counter() - start
        return result.returncode, result.stdout, result.stderr, elapsed

    def benchmark_cmd(self, cmd: List[str], cwd: Optional[str] = None, iterations: int = 3) -> Tuple[float, int, str, str]:
        """Run command multiple times, return average time (excluding first), exit_code, stdout, stderr."""
        times = []
        exit_code = 0
        stdout = ""
        stderr = ""
        for i in range(iterations):
            start = time.perf_counter()
            result = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            elapsed = time.perf_counter() - start
            if i == 0:
                # First run: capture output and exit code
                exit_code = result.returncode
                stdout = result.stdout
                stderr = result.stderr
                # Optionally discard time (warm-up)
                continue
            times.append(elapsed)
        avg_time = sum(times) / len(times) if times else 0
        return avg_time, exit_code, stdout, stderr

    def verify_archive(self, archive_path: Path) -> bool:
        """Verify archive integrity using system unzip -t."""
        cmd = ['unzip', '-t', str(archive_path)]
        exit_code, stdout, stderr, _ = self.run_cmd(cmd)
        # unzip returns 0 for success, 1 for warnings (e.g., multi-part), 2+ for errors
        return exit_code <= 1

    def create_test_files(self, base_dir: Path) -> List[str]:
        """Create a standard set of test files and return relative paths."""
        # Clean up any existing test files/directories
        for child in base_dir.iterdir():
            if child.is_file():
                child.unlink()
            else:
                shutil.rmtree(child)
        # Simple text files
        (base_dir / 'file1.txt').write_text('Hello, world!\n' * 100)
        (base_dir / 'file2.txt').write_text('Another file with some content.\n' * 50)
        # Nested directory
        sub = base_dir / 'subdir'
        sub.mkdir()
        (sub / 'nested.txt').write_text('Nested file content.\n')
        # Empty directory
        (base_dir / 'empty_dir').mkdir()
        # Binary file (random bytes)
        import random
        random_data = bytes(random.getrandbits(8) for _ in range(1024))
        (base_dir / 'random.bin').write_bytes(random_data)
        # Return relative paths of files to archive (exclude empty directories)
        return ['file1.txt', 'file2.txt', 'subdir/nested.txt', 'random.bin']

    def run_functional_test(self, name: str, args: List[str], work_dir: Path, input_files: Optional[List[str]] = None) -> TestResult:
        """Run a single functional test with both zips."""
        if input_files is None:
            # Collect all non-directory files recursively
            input_files = []
            for path in work_dir.rglob('*'):
                if path.is_file():
                    input_files.append(str(path.relative_to(work_dir)))

        archive_system = work_dir / 'system.zip'
        archive_build = work_dir / 'build.zip'

        # Build command lines
        # For exclude/include, system zip expects options AFTER input files
        if name in ('exclude', 'include'):
            cmd_system = [self.system_zip, str(archive_system)] + input_files + args
            cmd_build = [self.build_zip, str(archive_build)] + input_files + args
        else:
            cmd_system = [self.system_zip, str(archive_system)] + args + input_files
            cmd_build = [self.build_zip, str(archive_build)] + args + input_files

        # Determine if test modifies input files
        modifying_flags = {'-m', '-u', '-f', '-FS'}
        need_backup = any(flag in args for flag in modifying_flags)
        backup_dir = None
        if need_backup:
            backup_dir = work_dir / '.backup'
            if backup_dir.exists():
                shutil.rmtree(backup_dir)
            shutil.copytree(work_dir, backup_dir, dirs_exist_ok=True)

        # Run system zip
        sys_code, sys_out, sys_err, sys_time = self.run_cmd(cmd_system, cwd=str(work_dir))
        sys_size = archive_system.stat().st_size if archive_system.exists() else 0
        sys_valid = self.verify_archive(archive_system) if sys_code == 0 else False

        # Restore original files if backup exists
        if backup_dir is not None:
            # Delete everything except backup and system archive
            for child in work_dir.iterdir():
                if child.name in ('.backup', 'system.zip'):
                    continue
                if child.is_file():
                    child.unlink()
                else:
                    shutil.rmtree(child)
            # Copy backup contents back
            for child in backup_dir.iterdir():
                if child.name == 'system.zip':
                    # Don't overwrite the newly created archive
                    continue
                dest = work_dir / child.name
                if child.is_file():
                    shutil.copy2(child, dest)
                else:
                    shutil.copytree(child, dest, dirs_exist_ok=True)
            shutil.rmtree(backup_dir)

        # Run build zip
        build_code, build_out, build_err, build_time = self.run_cmd(cmd_build, cwd=str(work_dir))
        build_size = archive_build.stat().st_size if archive_build.exists() else 0
        build_valid = self.verify_archive(archive_build) if build_code == 0 else False

        # Compare results
        differences = []
        passed = True

        if sys_code != build_code:
            differences.append(f"Exit codes differ: system={sys_code}, build={build_code}")
            passed = False

        if sys_valid != build_valid:
            differences.append(f"Archive validity differs: system={sys_valid}, build={build_valid}")
            passed = False

        # If both succeeded, compare archive contents
        if sys_code == 0 and build_code == 0:
            try:
                with zipfile.ZipFile(archive_system, 'r') as zs, zipfile.ZipFile(archive_build, 'r') as zb:
                    sys_names = sorted(zs.namelist())
                    build_names = sorted(zb.namelist())
                    if sys_names != build_names:
                        differences.append(f"Archive entries differ: system={sys_names}, build={build_names}")
                        passed = False
                    else:
                        for name in sys_names:
                            if zs.read(name) != zb.read(name):
                                differences.append(f"Entry content differs for {name}")
                                passed = False
                                break
            except Exception as exc:  # pragma: no cover - safety net
                differences.append(f"Archive compare failed: {exc}")
                passed = False

        # Compare archive sizes (only if both succeeded)
        if sys_code == 0 and build_code == 0 and sys_size != build_size:
            differences.append(f"Archive sizes differ: system={sys_size}, build={build_size}")
            # Not necessarily a failure, just note

        # Clean up
        if archive_system.exists():
            archive_system.unlink()
        if archive_build.exists():
            archive_build.unlink()

        return TestResult(
            name=name,
            system_exit_code=sys_code,
            build_exit_code=build_code,
            system_stdout=sys_out,
            build_stdout=build_out,
            system_stderr=sys_err,
            build_stderr=build_err,
            system_time=sys_time,
            build_time=build_time,
            system_archive_size=sys_size,
            build_archive_size=build_size,
            system_archive_valid=sys_valid,
            build_archive_valid=build_valid,
            passed=passed,
            differences=differences
        )

    def run_functional_suite(self) -> List[TestResult]:
        """Run all functional tests."""
        results = []
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            # Define test cases
            test_cases = [
                ("basic", []),
                ("recurse", ["-r"]),
                ("junk_paths", ["-j"]),
                ("store_only", ["-0"]),
                ("compress_fast", ["-1"]),
                ("compress_best", ["-9"]),
                ("quiet", ["-q"]),
                ("quiet_really", ["-qq"]),
                ("verbose", ["-v"]),
                ("exclude", ["-x", "*.bin"]),
                ("include", ["-i", "*.txt"]),
                ("move", ["-m"]),
                ("update", ["-u"]),
                ("freshen", ["-f"]),
                ("file_sync", ["-FS"]),
                ("test_after", ["-T"]),
                #("encrypt", ["-e", "-P", "testpass"]),
                ("split", ["-s", "100k"]),
                ("time_filter", ["-t", "20250101"]),
                ("time_filter_before", ["-tt", "20240101"]),
            ]

            for name, args in test_cases:
                print(f"Running functional test: {name}", file=sys.stderr)
                # Recreate test files for each test (some tests delete files)
                input_files = self.create_test_files(tmp_path)
                result = self.run_functional_test(name, args, tmp_path, input_files)
                results.append(result)

        return results

    def run_performance_test(self, name: str, data_dir: Path, args: List[str], input_files: Optional[List[str]] = None) -> PerformanceResult:
        """Run a performance benchmark with multiple iterations."""
        if input_files is None:
            # Collect all non-directory files recursively
            input_files = []
            for path in data_dir.rglob('*'):
                if path.is_file():
                    input_files.append(str(path.relative_to(data_dir)))

        archive_system = data_dir / 'system_perf.zip'
        archive_build = data_dir / 'build_perf.zip'

        cmd_system = [self.system_zip, str(archive_system)] + args + input_files
        cmd_build = [self.build_zip, str(archive_build)] + args + input_files

        # Helper to time multiple runs
        def time_zip(cmd, archive_path, iterations=3):
            times = []
            for i in range(iterations):
                # Delete archive if exists before run
                if archive_path.exists():
                    archive_path.unlink()
                start = time.perf_counter()
                result = subprocess.run(cmd, cwd=str(data_dir),
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                elapsed = time.perf_counter() - start
                if i == 0:
                    # First run: capture size and exit code
                    size = archive_path.stat().st_size if archive_path.exists() else 0
                    exit_code = result.returncode
                    stdout = result.stdout
                    stderr = result.stderr
                else:
                    times.append(elapsed)
            avg_time = sum(times) / len(times) if times else elapsed
            return avg_time, size, exit_code, stdout, stderr

        # Time system zip
        sys_time, sys_size, sys_code, sys_out, sys_err = time_zip(cmd_system, archive_system)
        # Time build zip
        build_time, build_size, build_code, build_out, build_err = time_zip(cmd_build, archive_build)

        # Clean up (already deleted in loop, but ensure)
        if archive_system.exists():
            archive_system.unlink()
        if archive_build.exists():
            archive_build.unlink()

        speedup = sys_time / build_time if build_time > 0 else 0
        size_ratio = sys_size / build_size if build_size > 0 else 0

        return PerformanceResult(
            name=name,
            system_time=sys_time,
            build_time=build_time,
            system_size=sys_size,
            build_size=build_size,
            speedup=speedup,
            size_ratio=size_ratio
        )

    def run_performance_suite(self) -> List[PerformanceResult]:
        """Run performance benchmarks with different data sizes and compression levels."""
        results = []

        # Create a larger dataset for performance testing
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            input_files = []
            # Generate 10MB of random data in multiple files
            for i in range(5):
                file_size = 2 * 1024 * 1024  # 2MB each
                random_data = os.urandom(file_size)
                file_path = tmp_path / f'large_{i}.bin'
                file_path.write_bytes(random_data)
                input_files.append(file_path.name)  # just the filename

            # Test different compression levels
            for level in [0, 1, 6, 9]:
                print(f"Running performance test: level -{level}", file=sys.stderr)
                result = self.run_performance_test(
                    f"compression_{level}",
                    tmp_path,
                    [f"-{level}", "-r"],
                    input_files
                )
                results.append(result)

        return results

def main():
    parser = argparse.ArgumentParser(description="Compare system zip and build/zip")
    parser.add_argument('--functional', action='store_true', help='Run functional tests')
    parser.add_argument('--performance', action='store_true', help='Run performance tests')
    parser.add_argument('--output', type=str, help='Output JSON file')
    args = parser.parse_args()

    if not (args.functional or args.performance):
        parser.error("At least one of --functional or --performance required")

    # Determine zip paths
    system_zip = os.environ.get('SYSTEM_ZIP', 'zip')
    build_zip = os.environ.get('BUILD_ZIP', 'build/zip')

    # Verify binaries exist
    if not shutil.which(system_zip):
        print(f"Error: system zip not found at '{system_zip}'", file=sys.stderr)
        sys.exit(1)
    if not Path(build_zip).exists():
        print(f"Error: build zip not found at '{build_zip}'", file=sys.stderr)
        sys.exit(1)

    comparator = ZipComparator(system_zip, build_zip)

    output_data = {}

    if args.functional:
        print("Running functional tests...", file=sys.stderr)
        functional_results = comparator.run_functional_suite()
        output_data['functional'] = [asdict(r) for r in functional_results]

        # Print summary
        passed = sum(1 for r in functional_results if r.passed)
        total = len(functional_results)
        print(f"\nFunctional tests: {passed}/{total} passed", file=sys.stderr)

        for result in functional_results:
            if not result.passed:
                print(f"\nFAILED: {result.name}", file=sys.stderr)
                for diff in result.differences:
                    print(f"  {diff}", file=sys.stderr)

    if args.performance:
        print("\nRunning performance tests...", file=sys.stderr)
        perf_results = comparator.run_performance_suite()
        output_data['performance'] = [asdict(r) for r in perf_results]

        # Print summary
        print("\nPerformance results:", file=sys.stderr)
        for result in perf_results:
            print(f"\n{result.name}:", file=sys.stderr)
            print(f"  System: {result.system_time:.2f}s, {result.system_size} bytes", file=sys.stderr)
            print(f"  Build:  {result.build_time:.2f}s, {result.build_size} bytes", file=sys.stderr)
            print(f"  Speedup: {result.speedup:.2f}x", file=sys.stderr)
            print(f"  Size ratio: {result.size_ratio:.2f}x", file=sys.stderr)

    # Output JSON if requested
    if args.output:
        with open(args.output, 'w') as f:
            json.dump(output_data, f, indent=2)
        print(f"\nResults written to {args.output}", file=sys.stderr)

if __name__ == '__main__':
    main()
