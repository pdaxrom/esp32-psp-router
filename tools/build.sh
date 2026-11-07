#!/bin/sh
# Run after sourcing ESP-IDF export.sh. Separate sdkconfig per profile avoids
# accidentally carrying OPEN or RNDIS choices between builds.
set -eu
cd "$(dirname "$0")/.."
profile=${1:-rndis}
security=${2:-wpa1}
case "$profile" in rndis|ecm) ;; *) printf '%s\n' 'Usage: sh tools/build.sh [rndis|ecm] [wpa1|open]' >&2; exit 2;; esac
case "$security" in wpa1|open) ;; *) printf '%s\n' 'Usage: sh tools/build.sh [rndis|ecm] [wpa1|open]' >&2; exit 2;; esac
defaults=sdkconfig.defaults
if [ "$profile" = ecm ]; then defaults="$defaults;sdkconfig.ecm"; fi
if [ "$security" = open ]; then defaults="$defaults;sdkconfig.open"; fi
build_dir="build-$profile"
if [ "$security" = open ]; then build_dir="$build_dir-open"; fi
idf.py -B "$build_dir" -DIDF_TARGET=esp32s3 \
    -DSDKCONFIG="$build_dir/sdkconfig" -DSDKCONFIG_DEFAULTS="$defaults" build
python tools/check_build.py "$build_dir" --profile "$profile" --security "$security"
