/* Build inside the pinned wpa_supplicant target to retain its private ABI. */
#include "utils/includes.h"
#include "utils/common.h"
#include "ap/wpa_auth.h"

struct wpa_authenticator *__real_wpa_init(const u8 *addr, struct wpa_auth_config *conf,
                                         struct wpa_auth_callbacks *callbacks);

struct wpa_authenticator *__wrap_wpa_init(const u8 *addr, struct wpa_auth_config *conf,
                                         struct wpa_auth_callbacks *callbacks)
{
    if (conf && conf->wpa == WPA_PROTO_WPA && conf->wpa_pairwise == WPA_CIPHER_TKIP) {
        /* IEEE 802.1X-2001 header for legacy WPA1 clients; PSK/MIC/TKIP unchanged. */
        conf->eapol_version = 1;
    }
    return __real_wpa_init(addr, conf, callbacks);
}
