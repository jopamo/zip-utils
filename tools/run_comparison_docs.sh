#!/usr/bin/env bash
# Run all documentation comparison captures (zip, unzip, zipinfo) against
# system binaries and the local build. Outputs land under the provided base dir.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

out_base="${1:-comparison-docs}"
zip_system="${ZIP_BIN:-zip}"
unzip_system="${UNZIP_BIN:-unzip}"
zipinfo_system="${ZIPINFO_BIN:-zipinfo}"
zip_build="${OUR_ZIP:-$here/build/zip}"
unzip_build="${OUR_UNZIP:-$here/build/unzip}"

abs() {
  # Prefer realpath, fallback to python if missing
  if command -v realpath >/dev/null 2>&1; then
    realpath "$1"
  else
    python3 - <<PY
import os,sys
print(os.path.abspath(sys.argv[1]))
PY
  fi
}

need_exec() {
  local label="$1" path="$2"
  if ! command -v "$path" >/dev/null 2>&1 && [ ! -x "$path" ]; then
    echo "missing executable for $label: $path" >&2
    exit 1
  fi
}

need_exec "zip (system)" "$zip_system"
need_exec "unzip (system)" "$unzip_system"
need_exec "zipinfo (system)" "$zipinfo_system"
need_exec "zip (build)" "$zip_build"
need_exec "unzip (build)" "$unzip_build"

zip_build_abs="$(abs "$zip_build")"
unzip_build_abs="$(abs "$unzip_build")"

mkdir -p "$out_base"

run_doc() {
  local label="$1"
  shift
  echo "== $label =="
  "$@"
}

run_doc "zip baseline" \
  python3 "$here/tools/document_zip_output.py" --zip "$zip_system" --outdir "$out_base/zip-doc-system"

run_doc "zip build" \
  python3 "$here/tools/document_zip_output.py" --zip "$zip_build_abs" --outdir "$out_base/zip-doc-build"

run_doc "unzip baseline" \
  python3 "$here/tools/document_unzip_output.py" --unzip "$unzip_system" --outdir "$out_base/unzip-doc-system"

run_doc "unzip build" \
  python3 "$here/tools/document_unzip_output.py" --unzip "$unzip_build_abs" --outdir "$out_base/unzip-doc-build"

run_doc "zipinfo baseline" \
  python3 "$here/tools/document_zipinfo_output.py" --zipinfo "$zipinfo_system" --outdir "$out_base/zipinfo-doc-system"

# zipinfo via unzip requires -Z
run_doc "zipinfo build" \
  python3 "$here/tools/document_zipinfo_output.py" --zipinfo "$unzip_build_abs" --zipinfo-arg=-Z --outdir "$out_base/zipinfo-doc-build"

echo "All comparison captures complete under $out_base/"
