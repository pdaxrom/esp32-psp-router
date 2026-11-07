#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bridge_policy.h"

int main(void)
{
    assert(!bridge_password_valid(NULL));
    assert(!bridge_password_valid(""));
    assert(!bridge_password_valid("1234567"));
    assert(bridge_password_valid("12345678"));
    assert(bridge_password_valid("psp3000wifi"));
    assert(!bridge_password_valid("1234567\n"));
    assert(!bridge_password_valid("1234567\x80"));
    char password[65];
    memset(password, 'x', 63);
    password[63] = 0;
    assert(bridge_password_valid(password));
    password[63] = 'x';
    password[64] = 0;
    assert(!bridge_password_valid(password));

    /* Queued frames from a previous enumeration must never reach a new host. */
    assert(bridge_frame_current(1, 1, 0, 14));
    assert(bridge_frame_current(UINT32_MAX, UINT32_MAX, 500, 1514));
    assert(!bridge_frame_current(1, 2, 0, 1514));
    assert(!bridge_frame_current(UINT32_MAX, 0, 0, 1514));
    assert(!bridge_frame_current(0, 0, 501, 1514));
    assert(!bridge_frame_current(0, 0, UINT32_MAX, 1514));
    assert(!bridge_frame_current(0, 0, 0, 13));
    assert(!bridge_frame_current(0, 0, 0, 1515));
    puts("PASS: 17 security/frame-lifetime boundary cases");
    return 0;
}
