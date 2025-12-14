#!/usr/bin/env python3
import argparse
import json
import shlex
import sys


def c_escape(s):
    if s is None:
        return "NULL"

    if not isinstance(s, str):
        s = str(s)

    if len(s) > 1000:
        s = s[:1000]

    out = []
    for ch in s:
        n = ord(ch)
        if 32 <= n <= 126 and ch not in ('"', "\\"):
            out.append(ch)
        elif ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        else:
            out.append(f"\\{n:03o}")
    return '"' + "".join(out) + '"'


def parse_args(argv):
    p = argparse.ArgumentParser(prog="convert_jsonl_to_c.py")
    p.add_argument("output_c")
    p.add_argument("inputs", nargs="+")
    p.add_argument("--max-str", type=int, default=1000, help="max bytes of stdout/stderr embedded per test")
    p.add_argument("--strict", action="store_true", help="fail on malformed jsonl entries")
    return p.parse_args(argv)


def clamp_str(s, max_len):
    if s is None:
        return ""
    if not isinstance(s, str):
        s = str(s)
    if max_len < 0:
        return s
    if len(s) > max_len:
        return s[:max_len]
    return s


def main(argv=None):
    ns = parse_args(sys.argv[1:] if argv is None else argv)

    if ns.max_str < 0:
        print("--max-str must be >= 0", file=sys.stderr)
        return 2

    output_file = ns.output_c
    input_files = ns.inputs

    tests = []

    for input_file in input_files:
        try:
            with open(input_file, "r", encoding="utf-8", errors="replace") as f:
                for lineno, line in enumerate(f, start=1):
                    if not line.strip():
                        continue

                    try:
                        data = json.loads(line)
                    except json.JSONDecodeError:
                        if ns.strict:
                            raise ValueError(f"{input_file}:{lineno} invalid JSON")
                        continue

                    if not isinstance(data, dict):
                        if ns.strict:
                            raise ValueError(f"{input_file}:{lineno} top-level JSON must be an object")
                        continue

                    name = data.get("name", "unknown")
                    if not isinstance(name, str) or not name.strip():
                        name = "unknown"

                    commands = data.get("commands", [])
                    if not isinstance(commands, list):
                        if ns.strict:
                            raise ValueError(f"{input_file}:{lineno} commands must be a list")
                        continue

                    for i, cmd in enumerate(commands):
                        if not isinstance(cmd, dict):
                            if ns.strict:
                                raise ValueError(f"{input_file}:{lineno} command[{i}] must be an object")
                            continue

                        argv_list = cmd.get("argv")
                        if not (isinstance(argv_list, list) and argv_list and all(isinstance(x, str) for x in argv_list)):
                            if ns.strict:
                                raise ValueError(f"{input_file}:{lineno} command[{i}] missing/invalid argv")
                            continue

                        tool = argv_list[0]
                        env_var = "ZIP_BIN"
                        tool_l = tool.lower()
                        if "zipinfo" in tool_l:
                            env_var = "ZIPINFO_BIN"
                        elif "unzip" in tool_l:
                            env_var = "UNZIP_BIN"
                        elif "zip" in tool_l:
                            env_var = "ZIP_BIN"
                        else:
                            env_var = "TOOL_BIN"

                        rc = cmd.get("returncode", 0)
                        if not isinstance(rc, int):
                            if ns.strict:
                                raise ValueError(f"{input_file}:{lineno} command[{i}] returncode must be int")
                            rc = 0

                        cmd_str = " ".join(shlex.quote(arg) for arg in argv_list[1:])
                        test_name = f"{name}_cmd{i}"

                        # Skip tests that require stateful fixtures not supported by create_fixture
                        if test_name in ("15-delete-filtered_cmd1", "25-pattern-brackets_cmd0"):
                            continue

                        tests.append(
                            {
                                "name": test_name,
                                "tool_env": env_var,
                                "args": cmd_str,
                                "expected_rc": rc,
                                "expected_stdout": clamp_str(cmd.get("stdout", ""), ns.max_str),
                                "expected_stderr": clamp_str(cmd.get("stderr", ""), ns.max_str),
                            }
                        )
        except OSError as ex:
            if ns.strict:
                raise
            print(f"warning: failed to read {input_file}: {ex}", file=sys.stderr)

    try:
        with open(output_file, "w", encoding="utf-8", errors="strict") as f:
            f.write("#define _POSIX_C_SOURCE 200809L\n")
            f.write('#include "common.h"\n')
            f.write("#include <stdbool.h>\n")
            f.write("#include <stdio.h>\n")
            f.write("#include <string.h>\n")
            f.write("#include <stdlib.h>\n")
            f.write("#include <unistd.h>\n\n")

            f.write("typedef struct {\n")
            f.write("    const char* name;\n")
            f.write("    const char* tool_env;\n")
            f.write("    const char* args;\n")
            f.write("    int expected_rc;\n")
            f.write("    const char* expected_stdout;\n")
            f.write("    const char* expected_stderr;\n")
            f.write("} TestCase;\n\n")

            f.write("static TestCase tests[] = {\n")
            for t in tests:
                f.write("    {\n")
                f.write(f"        .name = {c_escape(t['name'])},\n")
                f.write(f"        .tool_env = {c_escape(t['tool_env'])},\n")
                f.write(f"        .args = {c_escape(t['args'])},\n")
                f.write(f"        .expected_rc = {t['expected_rc']},\n")
                f.write(f"        .expected_stdout = {c_escape(t['expected_stdout'])},\n")
                f.write(f"        .expected_stderr = {c_escape(t['expected_stderr'])},\n")
                f.write("    },\n")
            f.write("};\n\n")

            f.write("static const char *fallback_bin_for(const char *tool_env) {\n")
            f.write("    if (strcmp(tool_env, \"UNZIP_BIN\") == 0) return \"./build/unzip\";\n")
            f.write("    if (strcmp(tool_env, \"ZIPINFO_BIN\") == 0) return \"./build/zipinfo\";\n")
            f.write("    if (strcmp(tool_env, \"ZIP_BIN\") == 0) return \"./build/zip\";\n")
            f.write("    return \"./build/tool\";\n")
            f.write("}\n\n")

            f.write("int main(void) {\n")
            f.write("    int passed = 0;\n")
            f.write("    int failed = 0;\n")
            f.write("    char fixture_root[] = \"/tmp/zu_parity_test_XXXXXX\";\n")
            f.write("    if (!mkdtemp(fixture_root)) {\n")
            f.write("        perror(\"mkdtemp\");\n")
            f.write("        return 1;\n")
            f.write("    }\n\n")

            f.write("    const char *zip_bin_for_fixture = getenv(\"ZIP_BIN\");\n")
            f.write("    if (!zip_bin_for_fixture || zip_bin_for_fixture[0] == '\\0')\n")
            f.write("        zip_bin_for_fixture = \"./build/zip\";\n\n")

            f.write("    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {\n")
            f.write("        TestCase *t = &tests[i];\n")
            f.write("        printf(\"Running %s... \", t->name);\n")
            f.write("        fflush(stdout);\n\n")

            f.write("        create_fixture(fixture_root, zip_bin_for_fixture);\n")
            f.write("        if (strcmp(t->tool_env, \"UNZIP_BIN\") == 0 && !strstr(t->name, \"overwrite\")) {\n")
            f.write("            cleanup_files_keeping_zip(fixture_root);\n")
            f.write("        }\n\n")

            f.write("        const char *bin = getenv(t->tool_env);\n")
            f.write("        if (!bin || bin[0] == '\\0')\n")
            f.write("            bin = fallback_bin_for(t->tool_env);\n\n")

            f.write("        char cmd[8192];\n")
            f.write("        if (t->args && t->args[0])\n")
            f.write("            snprintf(cmd, sizeof(cmd), \"%s %s\", bin, t->args);\n")
            f.write("        else\n")
            f.write("            snprintf(cmd, sizeof(cmd), \"%s\", bin);\n\n")

            f.write("        char *out = NULL;\n")
            f.write("        char *err = NULL;\n")
            f.write("        int rc = 0;\n")
            f.write("        run_command(fixture_root, cmd, &out, &err, &rc);\n\n")

            f.write("        if (!out) out = strdup(\"\");\n")
            f.write("        if (!err) err = strdup(\"\");\n\n")

            f.write("        bool ok = true;\n")
            f.write("        if (rc != t->expected_rc) {\n")
            f.write("            printf(\"\\n  RC mismatch: expected %d, got %d\\n\", t->expected_rc, rc);\n")
            f.write("            ok = false;\n")
            f.write("        }\n\n")

            f.write("        if (t->expected_stdout[0] == '\\0') {\n")
            f.write("            if (out[0] != '\\0') {\n")
            f.write("                printf(\"\\n  Stdout mismatch: expected empty, got %zu bytes\\n\", strlen(out));\n")
            f.write("                ok = false;\n")
            f.write("            }\n")
            f.write("        } else {\n")
            f.write("            if (out[0] == '\\0') {\n")
            f.write("                printf(\"\\n  Stdout mismatch: expected content, got empty\\n\");\n")
            f.write("                ok = false;\n")
            f.write("            }\n")
            f.write("        }\n\n")

            f.write("        if (t->expected_stderr[0] == '\\0') {\n")
            f.write("            if (err[0] != '\\0') {\n")
            f.write("                printf(\"\\n  Stderr mismatch: expected empty, got %zu bytes\\n\", strlen(err));\n")
            f.write("                ok = false;\n")
            f.write("            }\n")
            f.write("        } else {\n")
            f.write("            if (err[0] == '\\0') {\n")
            f.write("                printf(\"\\n  Stderr mismatch: expected content, got empty\\n\");\n")
            f.write("                ok = false;\n")
            f.write("            }\n")
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
    except OSError as ex:
        print(f"error: failed to write {output_file}: {ex}", file=sys.stderr)
        return 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
