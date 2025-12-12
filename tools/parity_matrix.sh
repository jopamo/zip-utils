#!/usr/bin/env bash
set -euo pipefail

SYSZIP=${SYSZIP:-/usr/bin/zip}
MYZIP=${MYZIP:-}
CMP=${CMP:-"./compare_zip.py"}

ROOT=/tmp/zu-parity
FIXT="$ROOT/fixt"
OUT="$ROOT/out"
LOG="$ROOT/log"
TMPDIR_CUSTOM="$ROOT/tmp"

mkfixt() {
  rm -rf "$FIXT"
  mkdir -p "$FIXT/d/sub" "$FIXT/emptydir"
  printf 'a\nb\nc\n' > "$FIXT/lf.txt"
  printf 'a\r\nb\r\nc\r\n' > "$FIXT/crlf.txt"
  head -c 4096 /dev/urandom > "$FIXT/blob.bin"
  printf '\x89PNG\r\n\x1a\n' > "$FIXT/fake.png"
  echo hi > "$FIXT/d/sub/hello.txt"
  ln -s lf.txt "$FIXT/link_lf"
  printf 'x\n' > "$FIXT/spaced name.txt"
  printf 'y\n' > "$FIXT/--looks-like-opt"
  printf 'z\n' > "$FIXT/-looks-like-opt"
  TZ=UTC touch -t 202401010101 "$FIXT/"{lf.txt,crlf.txt,blob.bin,fake.png,"spaced name.txt","--looks-like-opt","-looks-like-opt"} "$FIXT/d/sub/hello.txt"
  echo old > "$FIXT/old.txt"; echo new > "$FIXT/new.txt"
  TZ=UTC touch -t 202401010101 "$FIXT/old.txt"
  TZ=UTC touch -t 202512010101 "$FIXT/new.txt"
}

run_case() {
  local name="$1"
  local sys_cmd="$2"
  local my_cmd="$3"

  local sys_out="$OUT/$name.sys.zip"
  local my_out="$OUT/$name.my.zip"

  rm -f "$sys_out" "$my_out"

  ( cd "$FIXT" && eval "$sys_cmd" ) >"$LOG/$name.sys.log" 2>&1 || { echo "FAIL sys $name"; tail -n 80 "$LOG/$name.sys.log" >&2; return 1; }
  ( cd "$FIXT" && eval "$my_cmd" ) >"$LOG/$name.my.log" 2>&1 || { echo "FAIL my  $name"; tail -n 80 "$LOG/$name.my.log" >&2; return 1; }

  test -f "$sys_out" || { echo "no sys output $name" >&2; return 1; }
  test -f "$my_out" || { echo "no my output $name" >&2; return 1; }

  python3 - "$sys_out" "$my_out" >"$LOG/$name.cmp.log" 2>&1 <<'PY' || {
import sys, zipfile, hashlib
sys_zip, my_zip = sys.argv[1], sys.argv[2]
def read_zip(path):
    with zipfile.ZipFile(path, 'r') as z:
        names = sorted(z.namelist())
        data = {n: z.read(n) for n in names}
    return names, data
n1,d1 = read_zip(sys_zip)
n2,d2 = read_zip(my_zip)
if n1 != n2:
    print("names differ")
    print("sys:", n1)
    print("my: ", n2)
    sys.exit(1)
for n in n1:
    if d1[n] != d2[n]:
        print(f"content differs for {n}")
        sys.exit(1)
print("match ok")
PY
  echo "MISMATCH $name"
  tail -n 120 "$LOG/$name.cmp.log" >&2
  return 1
  }

  echo "OK $name"
}

check_mtime() {
  local name="$1"
  local sys_out="$OUT/$name.sys.zip"
  local my_out="$OUT/$name.my.zip"
  local sys_m=$(stat -c %Y "$sys_out")
  local my_m=$(stat -c %Y "$my_out")
  if [[ "$sys_m" != "$my_m" ]]; then
    echo "MISMATCH mtime $name sys=$sys_m my=$my_m" >&2
    return 1
  fi
  return 0
}

main() {
  if [[ -z "$MYZIP" ]]; then
    if [[ -x "./build/zip" ]]; then
      MYZIP="./build/zip"
    elif [[ -x "./build-meson-release/zip" ]]; then
      MYZIP="./build-meson-release/zip"
    elif [[ -x "./build-meson-asan/zip" ]]; then
      MYZIP="./build-meson-asan/zip"
    else
      echo "Could not find zip binary; set MYZIP=..." >&2
      exit 1
    fi
  fi

  SYSZIP=$(realpath "$SYSZIP")
  MYZIP=$(realpath "$MYZIP")
  CMP=$(realpath "$CMP")

  test -x "$SYSZIP"
  test -x "$MYZIP"
  test -f "$CMP"

  rm -rf "$OUT" "$LOG" "$TMPDIR_CUSTOM"
  mkdir -p "$OUT" "$LOG" "$TMPDIR_CUSTOM"
  mkfixt

  fails=0

  run_case basic_1 \
    "$SYSZIP $OUT/basic_1.sys.zip lf.txt" \
    "$MYZIP $OUT/basic_1.my.zip lf.txt" || fails=$((fails+1))

  run_case basic_multi \
    "$SYSZIP $OUT/basic_multi.sys.zip lf.txt crlf.txt blob.bin fake.png" \
    "$MYZIP $OUT/basic_multi.my.zip lf.txt crlf.txt blob.bin fake.png" || fails=$((fails+1))

  run_case end_of_opts \
    "$SYSZIP $OUT/end_of_opts.sys.zip -- -looks-like-opt --looks-like-opt" \
    "$MYZIP $OUT/end_of_opts.my.zip -- -looks-like-opt --looks-like-opt" || fails=$((fails+1))

  run_case filter_stdout \
    "printf 'stdin\n' | $SYSZIP > $OUT/filter_stdout.sys.zip" \
    "printf 'stdin\n' | $MYZIP > $OUT/filter_stdout.my.zip" || fails=$((fails+1))

  run_case names_stdin \
    "printf 'd/sub/hello.txt\nlf.txt\n' | $SYSZIP $OUT/names_stdin.sys.zip -@" \
    "printf 'd/sub/hello.txt\nlf.txt\n' | $MYZIP $OUT/names_stdin.my.zip -@" || fails=$((fails+1))

  run_case r_dir \
    "$SYSZIP -r $OUT/r_dir.sys.zip d" \
    "$MYZIP -r $OUT/r_dir.my.zip d" || fails=$((fails+1))

  run_case rj_dir \
    "$SYSZIP -r -j $OUT/rj_dir.sys.zip d" \
    "$MYZIP -r -j $OUT/rj_dir.my.zip d" || fails=$((fails+1))

  run_case lvl_0 \
    "$SYSZIP -0 $OUT/lvl_0.sys.zip lf.txt" \
    "$MYZIP -0 $OUT/lvl_0.my.zip lf.txt" || fails=$((fails+1))

  run_case lvl_9 \
    "$SYSZIP -9 $OUT/lvl_9.sys.zip lf.txt" \
    "$MYZIP -9 $OUT/lvl_9.my.zip lf.txt" || fails=$((fails+1))

  run_case n_suffixes \
    "$SYSZIP -9 -n .png:.bin $OUT/n_suffixes.sys.zip fake.png blob.bin lf.txt" \
    "$MYZIP -9 -n .png:.bin $OUT/n_suffixes.my.zip fake.png blob.bin lf.txt" || fails=$((fails+1))

  run_case b_tmp \
    "$SYSZIP -b $TMPDIR_CUSTOM $OUT/b_tmp.sys.zip blob.bin" \
    "$MYZIP -b $TMPDIR_CUSTOM $OUT/b_tmp.my.zip blob.bin" || fails=$((fails+1))

  run_case l_lf \
    "$SYSZIP -l $OUT/l_lf.sys.zip lf.txt" \
    "$MYZIP -l $OUT/l_lf.my.zip lf.txt" || fails=$((fails+1))

  run_case ll_crlf \
    "$SYSZIP -ll $OUT/ll_crlf.sys.zip crlf.txt" \
    "$MYZIP -ll $OUT/ll_crlf.my.zip crlf.txt" || fails=$((fails+1))

  run_case x_inline \
    "$SYSZIP $OUT/x_inline.sys.zip -x'*.bin' lf.txt blob.bin fake.png" \
    "$MYZIP $OUT/x_inline.my.zip -x'*.bin' lf.txt blob.bin fake.png" || fails=$((fails+1))

  run_case i_inline \
    "$SYSZIP $OUT/i_inline.sys.zip -i'*.txt' lf.txt crlf.txt blob.bin fake.png" \
    "$MYZIP $OUT/i_inline.my.zip -i'*.txt' lf.txt crlf.txt blob.bin fake.png" || fails=$((fails+1))

  run_case t_after \
    "$SYSZIP $OUT/t_after.sys.zip -t 01012025 old.txt new.txt" \
    "$MYZIP $OUT/t_after.my.zip -t 01012025 old.txt new.txt" || fails=$((fails+1))

  run_case tt_before \
    "$SYSZIP $OUT/tt_before.sys.zip -tt 01012025 old.txt new.txt" \
    "$MYZIP $OUT/tt_before.my.zip -tt 01012025 old.txt new.txt" || fails=$((fails+1))

  run_case qq \
    "$SYSZIP -qq $OUT/qq.sys.zip lf.txt" \
    "$MYZIP -qq $OUT/qq.my.zip lf.txt" || fails=$((fails+1))

  run_case T_basic \
    "$SYSZIP -T $OUT/T_basic.sys.zip lf.txt" \
    "$MYZIP -T $OUT/T_basic.my.zip lf.txt" || fails=$((fails+1))

  run_case z_comment \
    "printf 'comment\n' | $SYSZIP -z $OUT/z_comment.sys.zip lf.txt" \
    "printf 'comment\n' | $MYZIP -z $OUT/z_comment.my.zip lf.txt" || fails=$((fails+1))

  run_case no_dirs \
    "$SYSZIP -D -r $OUT/no_dirs.sys.zip d" \
    "$MYZIP -D -r $OUT/no_dirs.my.zip d" || fails=$((fails+1))

  run_case strip_attrs \
    "$SYSZIP -X $OUT/strip_attrs.sys.zip lf.txt" \
    "$MYZIP -X $OUT/strip_attrs.my.zip lf.txt" || fails=$((fails+1))

  run_case symlink_store \
    "$SYSZIP -y $OUT/symlink_store.sys.zip link_lf" \
    "$MYZIP -y $OUT/symlink_store.my.zip link_lf" || fails=$((fails+1))

  if run_case o_mtime \
    "$SYSZIP -o $OUT/o_mtime.sys.zip old.txt new.txt" \
    "$MYZIP -o $OUT/o_mtime.my.zip old.txt new.txt"; then
    check_mtime o_mtime || fails=$((fails+1))
  else
    fails=$((fails+1))
  fi

  echo
  echo "outputs: $OUT"
  echo "logs:    $LOG"

  test "$fails" -eq 0 || { echo "failures: $fails" >&2; exit 1; }
}

main "$@"
