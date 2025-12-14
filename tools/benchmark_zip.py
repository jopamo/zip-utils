#!/usr/bin/env python3
"""
Benchmark performance between system zip and build/zip.

This script runs a suite of performance benchmarks to compare
the speed and compression ratio of the system's Info-ZIP `zip` command
with the project's `build/zip` executable.

Usage:
    ./benchmark_zip.py [--output <file>]

Options:
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
import random
from pathlib import Path
from dataclasses import dataclass, asdict
from typing import List, Tuple, Optional

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

class ZipBenchmark:
    def __init__(self, system_zip: str, build_zip: str):
        self.system_zip = shutil.which(system_zip) or system_zip
        self.build_zip = str(Path(build_zip).resolve())

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
    parser.add_argument('--output', type=str)
    args = parser.parse_args()

    sys_zip = os.environ.get('SYSTEM_ZIP', 'zip')
    bld_zip = os.environ.get('BUILD_ZIP', 'build/zip')

    if not shutil.which(sys_zip):
        sys.exit(f"System zip '{sys_zip}' not found.")
    if not Path(bld_zip).exists():
        sys.exit(f"Build zip '{bld_zip}' not found.")

    bench = ZipBenchmark(sys_zip, bld_zip)
    
    res = bench.run_performance_suite()
    
    print(f"\n{'Dataset':<12} | {'Lvl':<3} | {'Sys Time':<8} | {'Bld Time':<8} | {'Speedup':<7} | {'Size Ratio':<10}", file=sys.stderr)
    print("-" * 75, file=sys.stderr)
    for r in res:
        print(f"{r.dataset:<12} | {r.compression_level:<3} | {r.system_time:.3f}s   | {r.build_time:.3f}s   | {r.speedup:.2f}x   | {r.size_ratio:.3f}", file=sys.stderr)

    if args.output:
        out_data = {'performance': [asdict(r) for r in res]}
        with open(args.output, 'w') as f:
            json.dump(out_data, f, indent=2)
        print(f"\nSaved results to {args.output}", file=sys.stderr)

if __name__ == '__main__':
    main()
