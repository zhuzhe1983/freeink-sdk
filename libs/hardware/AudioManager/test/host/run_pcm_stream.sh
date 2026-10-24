#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-pcm-stream.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror ${CXXFLAGS:-} -UNDEBUG \
  -DFREEINK_DEVICE_WAVESHARE_EPAPER_397=1 -DFREEINK_CAP_AUDIO=1 \
  -Istream_stubs -I../../../BoardConfig/include -I../../include \
  test_pcm_stream.cpp ../../src/AudioManager.cpp -o "$BUILD_DIR/test"
"$BUILD_DIR/test"
for flags in '-DFREEINK_DEVICE_WAVESHARE_EPAPER_397=1 -DFREEINK_CAP_AUDIO=0' '-DFREEINK_DEVICE_X4=1'; do
  ${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -Wno-unused-private-field ${CXXFLAGS:-} -UNDEBUG $flags \
    -Istream_stubs -I../../../BoardConfig/include -I../../include \
    test_pcm_stream_absent.cpp ../../src/AudioManager.cpp -o "$BUILD_DIR/absent"
  "$BUILD_DIR/absent"
done
