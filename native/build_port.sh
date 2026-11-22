#!/bin/sh
# Build the co-op DLL for another game build: sh native/build_port.sh b2019-0223   ->  native/build/dinput8-b2019-0223.dll
# tools/portgen.py rewrites the game addresses of native/ with the table port/<build>.json into build/port-<build>/native/ (plus r1_build_port.h: the build's
# id, exe hash and network tag, which r1.h picks up), which build.sh then compiles. native/ itself is not touched.
set -e
cd "$(dirname "$0")"
ID="${1:?usage: build_port.sh <build id>  (a port/<id>.json)}"
python -I ../tools/portgen.py "../port/$ID.json" "../build/port-$ID"
SRCDIR="../build/port-$ID/native" OUT="${OUT:-$(pwd)/build/dinput8-$ID.dll}" sh "$(pwd)/build.sh"
