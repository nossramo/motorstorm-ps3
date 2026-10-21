#!/bin/sh
# The Xcode app and Terminal use the same validated native launch defaults.
set -eu
cd "$(dirname "$0")/.."
profile=$(python3 tools/launch_profile.py)
eval "$profile"
mkdir -p "$PS3RECOMP_MSL_CACHE"
exec sh tools/play.command
