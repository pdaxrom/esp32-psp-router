#include "router_policy.h"

bool router_client_allowed(uint32_t peer, uint32_t lan, uint32_t mask, uint32_t usb_host)
{
    return peer && (((peer & mask) == (lan & mask)) || (usb_host && peer == usb_host));
}

bool router_choose_lan(uint32_t wan, uint32_t mask, uint32_t *gateway)
{
    static const uint32_t candidates[] = {0xc0a83201, 0xc0a83301, 0xac1f3201, 0x0a4d0001};
    uint32_t inverse = ~mask;
    if (!gateway || (wan && (inverse & (inverse + 1)))) {
        return false; /* Not a contiguous IPv4 netmask. */
    }
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        uint32_t first = candidates[i] & 0xffffff00, last = first | 255;
        if (!wan || last < (wan & mask) || first > (wan | inverse)) {
            *gateway = candidates[i];
            return true;
        }
    }
    return false;
}
