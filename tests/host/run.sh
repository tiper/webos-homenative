#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The monkey test: the real src/home.c on a fake TV (fake.c), under AddressSanitizer and
# UndefinedBehaviorSanitizer, with random remote input and hostile system replies.
#   tests/host/run.sh [seeds] [frames]
# Needs a host compiler and GLib (Debian: libglib2.0-dev); everything else is fake.
set -e
cd "$(dirname "$0")/../.."
SEEDS=${1:-20}; FRAMES=${2:-30000}; OUT=${TMPDIR:-/tmp}/homenative-monkey
FLAGS="-std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Itests/host/include -Ivendor $(pkg-config --cflags glib-2.0)"
${CC:-cc} $FLAGS -w -include tests/host/shim.h -Dmain=homenative_main -c src/home.c -o "$OUT.home.o"
${CC:-cc} $FLAGS -w -c tests/host/fake.c -o "$OUT.fake.o"
${CC:-cc} $FLAGS -w -c vendor/cJSON.c -o "$OUT.cjson.o"
${CC:-cc} -fsanitize=address,undefined "$OUT.home.o" "$OUT.fake.o" "$OUT.cjson.o" $(pkg-config --libs glib-2.0) -lm -lpthread -o "$OUT"
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:allow_user_segv_handler=0:log_path=$OUT.report UBSAN_OPTIONS=print_stacktrace=1:log_path=$OUT.report
fail=0
for s in $(seq 1 "$SEEDS"); do
  rm -f "$OUT".report.* /tmp/homenative-starts
  if ! r=$("$OUT" "$s" "$FRAMES" 2>/dev/null); then echo "seed $s: $r"; cat "$OUT".report.* 2>/dev/null | head -40; fail=1; break; fi
  echo "seed $s: $r"
done
exit $fail
