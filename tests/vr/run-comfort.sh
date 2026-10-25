#!/bin/sh
set -eu
SONICR_TEST_COMFORT_ONLY=1 exec "$(dirname "$0")/run-pose-render.sh"
