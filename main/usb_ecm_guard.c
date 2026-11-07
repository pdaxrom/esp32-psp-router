#include "sdkconfig.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "usb_ecm_guard.h"

/* In pinned TinyUSB 0.21.0~4 netd_reset() clears the interface, but preserves
 * its static can_xmit flag. Also SET_INTERFACE alt 0 does not clear that flag.
 * On re-enumeration can_xmit alone can therefore permit a transfer before ECM's
 * bulk endpoints have opened. Observe the actual alternate-setting request via
 * TinyUSB's supported application-class override, without patching the package.
 * Builtin netd still owns init/reset/deinit. This adapter owns only dispatch for
 * the ECM interfaces and the readiness flag; all packet handling is delegated.
 */
#if CONFIG_PSP_USB_ECM
static bool s_data_ready;

static void guard_init(void)
{
    s_data_ready = false;
}

static void guard_reset(uint8_t rhport)
{
    s_data_ready = false;
}

static bool guard_control(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request)
{
    bool ok = netd_control_xfer_cb(rhport, stage, request);
    if (ok && stage == CONTROL_STAGE_SETUP && request->bmRequestType == 0x01 &&
        request->bRequest == TUSB_REQ_SET_INTERFACE && request->wIndex == 1) {
        s_data_ready = request->wValue == 1;
    }
    return ok;
}

const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    static const usbd_class_driver_t guard = {
        .name = "ECM alternate-setting guard",
        .init = guard_init,
        .reset = guard_reset,
        .open = netd_open,
        .control_xfer_cb = guard_control,
        .xfer_cb = netd_xfer_cb,
    };
    *driver_count = 1;
    return &guard;
}
#endif

bool usb_ecm_data_ready(void)
{
#if CONFIG_PSP_USB_ECM
    return s_data_ready;
#else
    return true;
#endif
}
