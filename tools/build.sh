#!/bin/sh
# Run after sourcing ESP-IDF export.sh. Separate sdkconfig per profile avoids
# accidentally carrying OPEN or RNDIS choices between builds.
set -eu
cd "$(dirname "$0")/.."
profile=${1:-rndis}
security=${2:-wpa2}
mode=${3:-router}
case "$profile" in rndis|ecm) ;; *) printf '%s\n' 'Usage: sh tools/build.sh [rndis|ecm] [wpa2|wpa1|open] [router|bridge|bridge-proxy]' >&2; exit 2;; esac
case "$security" in wpa2|wpa1|open) ;; *) printf '%s\n' 'Usage: sh tools/build.sh [rndis|ecm] [wpa2|wpa1|open] [router|bridge|bridge-proxy]' >&2; exit 2;; esac
case "$mode" in router|bridge|bridge-proxy) ;; *) printf '%s\n' 'Mode must be router, bridge or bridge-proxy' >&2; exit 2;; esac
defaults=sdkconfig.defaults
if [ "$profile" = ecm ]; then defaults="$defaults;sdkconfig.ecm"; fi
if [ "$security" = wpa1 ]; then defaults="$defaults;sdkconfig.wpa1"; fi
if [ "$security" = open ]; then defaults="$defaults;sdkconfig.open"; fi
if [ "$mode" != router ]; then defaults="$defaults;sdkconfig.bridge"; fi
if [ "$mode" = bridge-proxy ]; then defaults="$defaults;sdkconfig.bridge_proxy"; fi
build_dir="build-$profile-$security-$mode"
idf.py -B "$build_dir" -DIDF_TARGET=esp32s3 \
    -DSDKCONFIG="$build_dir/sdkconfig" -DSDKCONFIG_DEFAULTS="$defaults" build
python tools/check_build.py "$build_dir" --profile "$profile" --security "$security" --mode "$mode"
