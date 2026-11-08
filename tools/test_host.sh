#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_binary=$(mktemp /tmp/psp-bridge-policy.XXXXXX)
trap 'rm -f "$test_binary"' EXIT HUP INT TERM
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I main main/bridge_policy.c tests/test_bridge_policy.c -o "$test_binary"
"$test_binary"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I main main/router_policy.c main/proxy_protocol.c tests/test_router_proxy.c -o "$test_binary"
"$test_binary"
if [ ! -d managed_components/espressif__tinyusb/src ]; then
    printf '%s\n' 'Run sh tools/build.sh ecm first to download the pinned TinyUSB headers.' >&2
    exit 1
fi
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined \
    -I tests/usb_host_config -I managed_components/espressif__tinyusb/src -I main \
    main/usb_ecm_guard.c tests/test_ecm_guard.c -o "$test_binary"
"$test_binary"
