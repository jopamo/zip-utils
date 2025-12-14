#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


def eprint(msg: str) -> None:
    print(msg, file=sys.stderr)


def c_escape_bytes(data: bytes) -> str:
    out: list[str] = []
    for b in data:
        if b == 0x5C:
            out.append("\\\\")
        elif b == 0x22:
            out.append('\\"')
        elif b == 0x0A:
            out.append("\\n")
        elif b == 0x0D:
            out.append("\\r")
        elif b == 0x09:
            out.append("\\t")
        elif 0x20 <= b <= 0x7E:
            out.append(chr(b))
        else:
            out.append(f"\\{b:03o}")
    return '"' + "".join(out) + '"'


def c_escape_str(s: str | None) -> str:
    if s is None:
        return "NULL"
    return c_escape_bytes(s.encode("utf-8", errors="replace"))


def split_c_literal(lit: str, max_chunk: int = 1400) -> str:
    if not (lit.startswith('"') and lit.endswith('"')):
        raise ValueError("expected quoted C string literal")
    inner = lit[1:-1]
    if len(inner) <= max_chunk:
        return lit
    parts: list[str] = []
    i = 0
    while i < len(inner):
        parts.append('"' + inner[i : i + max_chunk] + '"')
        i += max_chunk
    return "\n".join(parts)


def json_loads_relaxed(line: str) -> dict[str, Any] | None:
    s = line.strip()
    if not s:
        return None
    try:
        obj = json.loads(s)
    except json.JSONDecodeError:
        return None
    if not isinstance(obj, dict):
        return None
    return obj


def sha256_hex_utf8(s: str) -> str:
    return hashlib.sha256(s.encode("utf-8", errors="replace")).hexdigest()


def clamp_utf8_bytes(s: str, max_bytes: int) -> tuple[str, int]:
    raw = s.encode("utf-8", errors="replace")
    if len(raw) <= max_bytes:
        return s, 0
    clipped = raw[:max_bytes]
    return clipped.decode("utf-8", errors="replace"), 1


def tool_env_for(tool0: str, overrides: dict[str, str]) -> str:
    base = Path(tool0).name.lower()
    if base in overrides:
        return overrides[base]
    if "zipinfo" in base:
        return "ZIPINFO_BIN"
    if "unzip" in base:
        return "UNZIP_BIN"
    if "zip" in base:
        return "ZIP_BIN"
    return "TOOL_BIN"


def parse_overrides(items: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for it in items:
        if "=" not in it:
            raise ValueError(f"bad override {it!r}, expected tool=ENVVAR")
        k, v = it.split("=", 1)
        k = k.strip().lower()
        v = v.strip()
        if not k or not v:
            raise ValueError(f"bad override {it!r}, empty key/value")
        out[k] = v
    return out


def argv_to_args(argv: list[str]) -> str:
    if len(argv) <= 1:
        return ""
    return " ".join(shlex.quote(a) for a in argv[1:])


@dataclass(frozen=True)
class TestCase:
    name: str
    tool_env: str
    args: str
    expected_rc: int
    expected_stdout: str
    expected_stderr: str
    stdout_sha256: str
    stderr_sha256: str
    stdout_truncated: int
    stderr_truncated: int


class StringPool:
    def __init__(self) -> None:
        self._buf = bytearray()
        self._index: dict[bytes, tuple[int, int]] = {}

    def intern(self, s: str) -> tuple[int, int]:
        b = s.encode("utf-8", errors="replace")
        if b in self._index:
            return self._index[b]
        off = len(self._buf)
        self._buf += b
        self._buf.append(0)
        meta = (off, len(b))
        self._index[b] = meta
        return meta

    def data(self) -> bytes:
        return bytes(self._buf)

    def unique(self) -> int:
        return len(self._index)


def build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="convert_jsonl_to_c.py",
        description="Convert jsonl command traces to a C test runner with pooling and hashes",
    )
    p.add_argument("output_c", type=Path)
    p.add_argument("inputs", nargs="+", type=Path)

    p.add_argument("--strict", action="store_true", help="fail fast on malformed records")
    p.add_argument("--name-prefix", default="", help="prefix added to every generated test name")
    p.add_argument(
        "--max-output-bytes",
        type=int,
        default=128 * 1024,
        help="cap stdout/stderr bytes per test before embedding in C",
    )
    p.add_argument(
        "--require-exact",
        action="store_true",
        help="generated C compares stdout/stderr exact match (otherwise relaxed rules)",
    )
    p.add_argument(
        "--tool-env",
        action="append",
        default=[],
        metavar="tool=ENVVAR",
        help="override tool basename to env var mapping, can be repeated",
    )
    p.add_argument(
        "--default-bin-dir",
        default="./build",
        help="fallback directory for bin lookup when env var is not set",
    )
    return p


def extract_tests(
    inputs: list[Path],
    *,
    strict: bool,
    name_prefix: str,
    max_output_bytes: int,
    env_overrides: dict[str, str],
) -> list[TestCase]:
    tests: list[TestCase] = []

    for p in inputs:
        with p.open("r", encoding="utf-8", errors="replace") as f:
            for lineno, line in enumerate(f, start=1):
                obj = json_loads_relaxed(line)
                if obj is None:
                    continue

                base_name = obj.get("name")
                if not isinstance(base_name, str) or not base_name.strip():
                    base_name = "unknown"

                commands = obj.get("commands", [])
                if not isinstance(commands, list):
                    if strict:
                        raise ValueError(f"{p}:{lineno} commands is not a list")
                    continue

                for i, cmd in enumerate(commands):
                    if not isinstance(cmd, dict):
                        if strict:
                            raise ValueError(f"{p}:{lineno} command[{i}] is not an object")
                        continue

                    argv = cmd.get("argv")
                    if not (isinstance(argv, list) and all(isinstance(x, str) for x in argv) and argv):
                        if strict:
                            raise ValueError(f"{p}:{lineno} command[{i}] missing argv")
                        continue

                    rc = cmd.get("returncode")
                    if not isinstance(rc, int):
                        if strict:
                            raise ValueError(f"{p}:{lineno} command[{i}] missing returncode")
                        continue

                    stdout = cmd.get("stdout", "")
                    stderr = cmd.get("stderr", "")
                    if not isinstance(stdout, str):
                        stdout = ""
                    if not isinstance(stderr, str):
                        stderr = ""

                    stdout_c, so_trunc = clamp_utf8_bytes(stdout, max_output_bytes)
                    stderr_c, se_trunc = clamp_utf8_bytes(stderr, max_output_bytes)

                    tool_env = tool_env_for(argv[0], env_overrides)
                    args = argv_to_args(argv)
                    tname = f"{name_prefix}{base_name}_cmd{i}"

                    tests.append(
                        TestCase(
                            name=tname,
                            tool_env=tool_env,
                            args=args,
                            expected_rc=rc,
                            expected_stdout=stdout_c,
                            expected_stderr=stderr_c,
                            stdout_sha256=sha256_hex_utf8(stdout_c),
                            stderr_sha256=sha256_hex_utf8(stderr_c),
                            stdout_truncated=so_trunc,
                            stderr_truncated=se_trunc,
                        )
                    )

    return tests


def emit_c(
    out_path: Path,
    tests: list[TestCase],
    *,
    require_exact: bool,
    default_bin_dir: str,
) -> None:
    pool = StringPool()
    pool.intern("")

    packed: list[dict[str, int]] = []

    for t in tests:
        name_off, name_len = pool.intern(t.name)
        tool_off, tool_len = pool.intern(t.tool_env)
        args_off, args_len = pool.intern(t.args)
        so_off, so_len = pool.intern(t.expected_stdout)
        se_off, se_len = pool.intern(t.expected_stderr)
        soh_off, soh_len = pool.intern(t.stdout_sha256)
        seh_off, seh_len = pool.intern(t.stderr_sha256)

        packed.append(
            {
                "name_off": name_off,
                "name_len": name_len,
                "tool_off": tool_off,
                "tool_len": tool_len,
                "args_off": args_off,
                "args_len": args_len,
                "rc": t.expected_rc,
                "so_off": so_off,
                "so_len": so_len,
                "se_off": se_off,
                "se_len": se_len,
                "soh_off": soh_off,
                "soh_len": soh_len,
                "seh_off": seh_off,
                "seh_len": seh_len,
                "so_trunc": t.stdout_truncated,
                "se_trunc": t.stderr_truncated,
            }
        )

    pool_lit = split_c_literal(c_escape_bytes(pool.data()))
    default_bin_dir_lit = c_escape_str(default_bin_dir)

    with out_path.open("w", encoding="utf-8", errors="strict") as f:
        f.write("#define _POSIX_C_SOURCE 200809L\n")
        f.write('#include "common.h"\n')
        f.write("#include <stdbool.h>\n")
        f.write("#include <stdint.h>\n")
        f.write("#include <stdio.h>\n")
        f.write("#include <stdlib.h>\n")
        f.write("#include <string.h>\n")
        f.write("#include <unistd.h>\n")
        f.write("\n")

        f.write("typedef struct {\n")
        f.write("    uint32_t name_off;\n")
        f.write("    uint32_t name_len;\n")
        f.write("    uint32_t tool_env_off;\n")
        f.write("    uint32_t tool_env_len;\n")
        f.write("    uint32_t args_off;\n")
        f.write("    uint32_t args_len;\n")
        f.write("    int expected_rc;\n")
        f.write("    uint32_t stdout_off;\n")
        f.write("    uint32_t stdout_len;\n")
        f.write("    uint32_t stderr_off;\n")
        f.write("    uint32_t stderr_len;\n")
        f.write("    uint32_t stdout_sha256_off;\n")
        f.write("    uint32_t stdout_sha256_len;\n")
        f.write("    uint32_t stderr_sha256_off;\n")
        f.write("    uint32_t stderr_sha256_len;\n")
        f.write("    uint8_t stdout_truncated;\n")
        f.write("    uint8_t stderr_truncated;\n")
        f.write("} TestCase;\n\n")

        f.write("static const unsigned char test_strpool[] =\n")
        f.write(pool_lit)
        f.write(";\n\n")

        f.write("static inline const char *pool_ptr(uint32_t off) {\n")
        f.write("    return (const char *)(test_strpool + off);\n")
        f.write("}\n\n")

        f.write("static TestCase tests[] = {\n")
        for e in packed:
            f.write("    {\n")
            f.write(f"        .name_off = {e['name_off']},\n")
            f.write(f"        .name_len = {e['name_len']},\n")
            f.write(f"        .tool_env_off = {e['tool_off']},\n")
            f.write(f"        .tool_env_len = {e['tool_len']},\n")
            f.write(f"        .args_off = {e['args_off']},\n")
            f.write(f"        .args_len = {e['args_len']},\n")
            f.write(f"        .expected_rc = {e['rc']},\n")
            f.write(f"        .stdout_off = {e['so_off']},\n")
            f.write(f"        .stdout_len = {e['so_len']},\n")
            f.write(f"        .stderr_off = {e['se_off']},\n")
            f.write(f"        .stderr_len = {e['se_len']},\n")
            f.write(f"        .stdout_sha256_off = {e['soh_off']},\n")
            f.write(f"        .stdout_sha256_len = {e['soh_len']},\n")
            f.write(f"        .stderr_sha256_off = {e['seh_off']},\n")
            f.write(f"        .stderr_sha256_len = {e['seh_len']},\n")
            f.write(f"        .stdout_truncated = {e['so_trunc']},\n")
            f.write(f"        .stderr_truncated = {e['se_trunc']},\n")
            f.write("    },\n")
        f.write("};\n\n")

        f.write("static const char *default_bin_dir = ")
        f.write(default_bin_dir_lit)
        f.write(";\n\n")

        f.write("static const char *guess_fallback_bin(const char *tool_env) {\n")
        f.write("    if (strcmp(tool_env, \"UNZIP_BIN\") == 0) return \"unzip\";\n")
        f.write("    if (strcmp(tool_env, \"ZIPINFO_BIN\") == 0) return \"zipinfo\";\n")
        f.write("    if (strcmp(tool_env, \"ZIP_BIN\") == 0) return \"zip\";\n")
        f.write("    return \"tool\";\n")
        f.write("}\n\n")

        f.write("static void build_cmd(char *dst, size_t dstsz, const char *bin, const char *args) {\n")
        f.write("    if (!args || args[0] == '\\0') {\n")
        f.write("        snprintf(dst, dstsz, \"%s\", bin);\n")
        f.write("        return;\n")
        f.write("    }\n")
        f.write("    snprintf(dst, dstsz, \"%s %s\", bin, args);\n")
        f.write("}\n\n")

        f.write("static bool match_output(const char *got, const char *expected, bool require_exact_match) {\n")
        f.write("    if (!expected) expected = \"\";\n")
        f.write("    if (!got) got = \"\";\n")
        f.write("    if (require_exact_match) {\n")
        f.write("        return strcmp(got, expected) == 0;\n")
        f.write("    }\n")
        f.write("    if (expected[0] == '\\0') {\n")
        f.write("        return got[0] == '\\0';\n")
        f.write("    }\n")
        f.write("    if (got[0] == '\\0') {\n")
        f.write("        return false;\n")
        f.write("    }\n")
        f.write("    return strncmp(got, expected, strlen(expected)) == 0;\n")
        f.write("}\n\n")

        f.write("int main(void) {\n")
        f.write("    int passed = 0;\n")
        f.write("    int failed = 0;\n")
        f.write("    const bool require_exact_match = ")
        f.write("true" if require_exact else "false")
        f.write(";\n")
        f.write("    char fixture_root[] = \"/tmp/zu_parity_test_XXXXXX\";\n")
        f.write("    if (!mkdtemp(fixture_root)) {\n")
        f.write("        perror(\"mkdtemp\");\n")
        f.write("        return 1;\n")
        f.write("    }\n\n")

        f.write("    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {\n")
        f.write("        TestCase *t = &tests[i];\n")
        f.write("        const char *name = pool_ptr(t->name_off);\n")
        f.write("        const char *tool_env = pool_ptr(t->tool_env_off);\n")
        f.write("        const char *args = pool_ptr(t->args_off);\n")
        f.write("        const char *exp_out = pool_ptr(t->stdout_off);\n")
        f.write("        const char *exp_err = pool_ptr(t->stderr_off);\n")
        f.write("        const char *exp_out_h = pool_ptr(t->stdout_sha256_off);\n")
        f.write("        const char *exp_err_h = pool_ptr(t->stderr_sha256_off);\n\n")

        f.write("        printf(\"Running %s... \", name);\n")
        f.write("        fflush(stdout);\n\n")

        f.write("        create_fixture(fixture_root);\n\n")

        f.write("        const char *bin = getenv(tool_env);\n")
        f.write("        char fallback[4096];\n")
        f.write("        if (!bin || bin[0] == '\\0') {\n")
        f.write("            const char *leaf = guess_fallback_bin(tool_env);\n")
        f.write("            snprintf(fallback, sizeof(fallback), \"%s/%s\", default_bin_dir, leaf);\n")
        f.write("            bin = fallback;\n")
        f.write("        }\n\n")

        f.write("        char cmd[8192];\n")
        f.write("        build_cmd(cmd, sizeof(cmd), bin, args);\n\n")

        f.write("        char *out = NULL;\n")
        f.write("        char *err = NULL;\n")
        f.write("        int rc = 0;\n")
        f.write("        run_command(fixture_root, cmd, &out, &err, &rc);\n\n")

        f.write("        bool ok = true;\n")
        f.write("        if (rc != t->expected_rc) {\n")
        f.write("            printf(\"\\n  RC mismatch: expected %d, got %d\\n\", t->expected_rc, rc);\n")
        f.write("            ok = false;\n")
        f.write("        }\n\n")

        f.write("        if (!match_output(out, exp_out, require_exact_match)) {\n")
        f.write("            printf(\"\\n  Stdout mismatch\\n\");\n")
        f.write("            printf(\"  expected_sha256=%s truncated=%u\\n\", exp_out_h, (unsigned)t->stdout_truncated);\n")
        f.write("            ok = false;\n")
        f.write("        }\n\n")

        f.write("        if (!match_output(err, exp_err, require_exact_match)) {\n")
        f.write("            printf(\"\\n  Stderr mismatch\\n\");\n")
        f.write("            printf(\"  expected_sha256=%s truncated=%u\\n\", exp_err_h, (unsigned)t->stderr_truncated);\n")
        f.write("            ok = false;\n")
        f.write("        }\n\n")

        f.write("        free(out);\n")
        f.write("        free(err);\n")
        f.write("        cleanup_fixture(fixture_root);\n\n")

        f.write("        if (ok) {\n")
        f.write("            printf(\"PASS\\n\");\n")
        f.write("            passed++;\n")
        f.write("        } else {\n")
        f.write("            printf(\"FAIL\\n\");\n")
        f.write("            failed++;\n")
        f.write("        }\n")
        f.write("    }\n\n")

        f.write("    rmdir(fixture_root);\n")
        f.write("    printf(\"\\nPassed: %d, Failed: %d\\n\", passed, failed);\n")
        f.write("    return failed > 0 ? 1 : 0;\n")
        f.write("}\n")


def main(argv: list[str]) -> int:
    ap = build_argparser()
    ns = ap.parse_args(argv)

    if ns.max_output_bytes < 0:
        eprint("--max-output-bytes must be >= 0")
        return 2

    env_overrides = parse_overrides(ns.tool_env)

    inputs: list[Path] = []
    for p in ns.inputs:
        if not p.exists():
            eprint(f"missing input {p}")
            return 2
        inputs.append(p)

    tests = extract_tests(
        inputs,
        strict=ns.strict,
        name_prefix=ns.name_prefix,
        max_output_bytes=ns.max_output_bytes,
        env_overrides=env_overrides,
    )

    emit_c(
        ns.output_c,
        tests,
        require_exact=ns.require_exact,
        default_bin_dir=ns.default_bin_dir,
    )

    eprint(f"wrote {ns.output_c} with {len(tests)} tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
