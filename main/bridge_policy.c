#include "bridge_policy.h"
#include <stddef.h>
#include <string.h>

bool bridge_password_valid(const char *password)
{
    if (!password) {
        return false;
    }
    size_t n = strlen(password);
    if (n < 8 || n > 63) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if ((unsigned char)password[i] < 32 || (unsigned char)password[i] > 126) {
            return false;
        }
    }
    return true;
}

bool bridge_frame_current(uint32_t packet_epoch, uint32_t usb_epoch,
                           uint32_t age_ms, uint16_t length)
{
    return packet_epoch == usb_epoch && age_ms <= 500 && length >= 14 && length <= 1514;
}

bool bridge_ssid_valid(const char *ssid)
{
    if (!ssid) return false;
    size_t length = strlen(ssid);
    if (!length || length > 32) return false;
    for (size_t i = 0; i < length; ++i) {
        if ((unsigned char)ssid[i] < 32 || (unsigned char)ssid[i] > 126) return false;
    }
    return strspn(ssid, " ") != length;
}
