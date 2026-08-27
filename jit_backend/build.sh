#!/bin/sh
# Build jit_backend as three .bmir files (HTTP core + API + JSON builder)
# for jitrunner runtime-link:
#   jitrunner jit_backend/app.bmir jit_backend/api.bmir jit_backend/jsonbuild.bmir --mode gen
#
# Run from the nanoproxy repo root: sh jit_backend/build.sh

set -e

CLASSYC="${CLASSYC:-classyc}"
if command -v "$CLASSYC" >/dev/null 2>&1; then
    CLASSYC="$(command -v "$CLASSYC")"
elif [ -x "$HOME/.classyc/bin/classyc" ]; then
    CLASSYC="$HOME/.classyc/bin/classyc"
elif [ -x /usr/local/bin/classyc ]; then
    CLASSYC=/usr/local/bin/classyc
else
    echo "build.sh: cannot find 'classyc' on PATH, ~/.classyc/bin, or /usr/local/bin" >&2
    exit 1
fi

cd "$(dirname "$0")/.."

# An installed classyc auto-discovers its own <prefix>/include/classyc, so
# no -I is needed for that. cchan.h (worker-pool support) lives under
# <prefix>/share/classyc/ccchan instead, which isn't auto-discovered -- find
# it relative to the classyc binary the same way classyc finds its own
# include dir. If you're pointing CLASSYC at a plain repo build instead of
# an install, pass -I yourself via CLASSYC_EXTRA_ARGS.
CLASSYC_PREFIX=$(dirname "$(dirname "$CLASSYC")")
CCCHAN_INC=""
if [ -d "$CLASSYC_PREFIX/share/classyc/ccchan" ]; then
    CCCHAN_INC="-I $CLASSYC_PREFIX/share/classyc/ccchan"
elif [ -d "$CLASSYC_PREFIX/ext/ccchan" ]; then
    CCCHAN_INC="-I $CLASSYC_PREFIX/ext/ccchan"
fi

# Each file is its own MIR module. jitrunner loads all three and links
# them at run time (app.cy has main(); api.cy has [[HttpGet]] +
# app_handle; jsonbuild.cy is the cejson-backed JSON builder api.cy calls
# for /api/posts). -c is required for api.cy/jsonbuild.cy (no main of
# their own).
CYFLAGS="$CLASSYC_EXTRA_ARGS $CCCHAN_INC -I jit_backend -w -l sqlite3 -l crypto"

"$CLASSYC" $CYFLAGS -c -o jit_backend/app.bmir jit_backend/app.cy
"$CLASSYC" $CYFLAGS -c -o jit_backend/api.bmir jit_backend/api.cy

# jsonbuild.cy vendors jit_backend/cejson.h (upstream: ~/src/GUI/cejson,
# a separate repo -- header-only, so a plain copy is fine) and is built
# separately, WITHOUT $CYFLAGS's normal ownership checking: cejson.h's
# builder functions (json_create_int/float/string) trip classyc's
# ownership checker with false-positive "double-free risk" errors on
# their malloc-then-maybe-free-on-capacity-failure pattern (same category
# as classyc's own dict.h). -fno-ownership is a whole-compile-unit flag,
# so this file is kept separate specifically so app.cy/api.cy keep full
# ownership checking -- see jsonbuild.h's header comment.
"$CLASSYC" $CLASSYC_EXTRA_ARGS $CCCHAN_INC -I jit_backend -fno-ownership -w \
    -c -o jit_backend/jsonbuild.bmir jit_backend/jsonbuild.cy

echo "build.sh: wrote jit_backend/app.bmir jit_backend/api.bmir jit_backend/jsonbuild.bmir"
echo "build.sh: run with: jitrunner jit_backend/app.bmir jit_backend/api.bmir jit_backend/jsonbuild.bmir --mode gen"
