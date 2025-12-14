#!/usr/bin/env python3
import json
import sys
import shlex

def c_escape(s):
    if s is None:
        return "NULL"
    # Use raw strings or proper escaping
    escaped = s.replace('\\', '\\\\').replace('"', '\"').replace('\n', '\\n').replace('\r', '\\r').replace('\t', '\\t')
    return f'"{escaped}"'

def main():
    if len(sys.argv) < 3:
        print("Usage: convert_jsonl_to_c.py <output.c> <input.jsonl> [input2.jsonl ...]")
        sys.exit(1)

    output_file = sys.argv[1]
    input_files = sys.argv[2:]

    tests = []

    for input_file in input_files:
        with open(input_file, 'r') as f:
            for line in f:
                if not line.strip():
                    continue
                try:
                    data = json.loads(line)
                except json.JSONDecodeError:
                    continue

                name = data.get('name', 'unknown')
                commands = data.get('commands', [])

                for i, cmd in enumerate(commands):
                    argv = cmd['argv']
                    tool = argv[0]
                    
                    env_var = "ZIP_BIN"
                    if "unzip" in tool:
                        env_var = "UNZIP_BIN"
                    elif "zipinfo" in tool:
                        env_var = "ZIPINFO_BIN"
                    
                    safe_args = [shlex.quote(arg) for arg in argv[1:]]
                    cmd_str = " ".join(safe_args)
                    
                    test_case = {
                        "name": f"{name}_cmd{i}",
                        "tool_env": env_var,
                        "args": cmd_str,
                        "expected_rc": cmd['returncode'],
                        "expected_stdout": cmd.get('stdout', ''),
                        "expected_stderr": cmd.get('stderr', '')
                    }
                    tests.append(test_case)

    with open(output_file, 'w') as f:
        f.write('#include "common.h"\n')
        f.write('#include <stdio.h>\n')
        f.write('#include <string.h>\n')
        f.write('#include <stdlib.h>\n')
        f.write('\n')
        f.write('typedef struct {\n')
        f.write('    const char* name;\n')
        f.write('    const char* tool_env;\n')
        f.write('    const char* args;\n')
        f.write('    int expected_rc;\n')
        f.write('    const char* expected_stdout;\n')
        f.write('    const char* expected_stderr;\n')
        f.write('} TestCase;\n\n')

        f.write('static TestCase tests[] = {\n')
        for t in tests:
            f.write('    {\n')
            f.write(f'        .name = {c_escape(t["name"])},\n')
            f.write(f'        .tool_env = {c_escape(t["tool_env"])},\n')
            f.write(f'        .args = {c_escape(t["args"])},\n')
            f.write(f'        .expected_rc = {t["expected_rc"]},\n')
            f.write(f'        .expected_stdout = {c_escape(t["expected_stdout"])},\n')
            f.write(f'        .expected_stderr = {c_escape(t["expected_stderr"])}\n')
            f.write('    },\n')
        f.write('};\n\n')

        f.write('int main(int argc, char** argv) {\n')
        f.write('    int passed = 0;\n')
        f.write('    int failed = 0;\n')
        f.write('    char fixture_root[] = "/tmp/zu_parity_test_XXXXXX";\n')
        f.write('    if (!mkdtemp(fixture_root)) {\n')
        f.write('        perror("mkdtemp");\n')
        f.write('        return 1;\n')
        f.write('    }\n\n')
        
        f.write('    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {\n')
        f.write('        TestCase* t = &tests[i];\n')
        f.write('        printf("Running %s... ", t->name);\n')
        f.write('        fflush(stdout);\n\n')

        f.write('        // Setup fixture\n')
        f.write('        create_fixture(fixture_root);\n\n')

        f.write('        // Determine binary\n')
        f.write('        const char* bin = getenv(t->tool_env);\n')
        f.write('        if (!bin) bin = "./build/zip";\n')
        f.write('        if (strcmp(t->tool_env, "UNZIP_BIN") == 0 && !getenv("UNZIP_BIN")) bin = "./build/unzip";\n\n')

        f.write('        // Construct command\n')
        f.write('        char cmd[8192];\n')
        f.write('        snprintf(cmd, sizeof(cmd), "%s %s", bin, t->args);\n\n')

        f.write('        char* out = NULL;\n')
        f.write('        char* err = NULL;\n')
        f.write('        int rc = 0;\n')
        f.write('        int run_ret = run_command(fixture_root, cmd, &out, &err, &rc);\n\n')
        
        f.write('        // Check results\n')
        f.write('        bool ok = true;\n')
        f.write('        if (rc != t->expected_rc) {\n')
        f.write('            printf("\n  RC mismatch: expected %d, got %d\n", t->expected_rc, rc);\n')
        f.write('            ok = false;\n')
        f.write('        }\n')
        
        f.write('        if (strlen(t->expected_stdout) == 0 && strlen(out) > 0) {\n')
        f.write('            printf("\n  Stdout mismatch: expected empty, got %zu bytes\n", strlen(out));\n')
        f.write('            ok = false;\n')
        f.write('        } else if (strlen(t->expected_stdout) > 0 && strlen(out) == 0) {\n')
        f.write('            printf("\n  Stdout mismatch: expected content, got empty\n");\n')
        f.write('            ok = false;\n')
        f.write('        }\n')
        
        f.write('        if (strlen(t->expected_stderr) == 0 && strlen(err) > 0) {\n')
        f.write('            printf("\n  Stderr mismatch: expected empty, got %zu bytes\n", strlen(err));\n')
        f.write('            ok = false;\n')
        f.write('        } else if (strlen(t->expected_stderr) > 0 && strlen(err) == 0) {\n')
        f.write('            printf("\n  Stderr mismatch: expected content, got empty\n");\n')
        f.write('            ok = false;\n')
        f.write('        }\n\n')

        f.write('        free(out);\n')
        f.write('        free(err);\n')
        f.write('        cleanup_fixture(fixture_root);\n\n')

        f.write('        if (ok) {\n')
        f.write('            printf("PASS\n");\n')
        f.write('            passed++;\n')
        f.write('        } else {\n')
        f.write('            printf("FAIL\n");\n')
        f.write('            failed++;\n')
        f.write('        }\n')
        f.write('    }\n\n')

        f.write('    rmdir(fixture_root);\n')
        f.write('    printf("\nPassed: %d, Failed: %d\n", passed, failed);\n')
        f.write('    return failed > 0 ? 1 : 0;\n')
        f.write('}\n')

if __name__ == "__main__":
    main()