#!/bin/sh
set -eu
SONICR_TEST_WATER_ONLY=1 exec "$(dirname "$0")/run-adjacent-batch.sh"
