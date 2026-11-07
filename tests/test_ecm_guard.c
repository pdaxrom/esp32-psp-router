#include <assert.h>
#include <stdio.h>
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "usb_ecm_guard.h"

static bool control_result = true;
static unsigned calls;

/* Endpoint operations remain the responsibility of the real TinyUSB driver.
 * These stubs isolate the application's readiness gate from device hardware.
 */
bool netd_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request)
{
    ++calls;
    return control_result;
}

uint16_t netd_open(uint8_t rhport, const tusb_desc_interface_t *descriptor, uint16_t length)
{
    return length;
}

bool netd_xfer_cb(uint8_t rhport, uint8_t endpoint, xfer_result_t result, uint32_t length)
{
    return true;
}

int main(void)
{
    uint8_t count = 0;
    const usbd_class_driver_t *driver = usbd_app_driver_get_cb(&count);
    assert(count == 1 && driver && driver->open == netd_open && driver->xfer_cb == netd_xfer_cb);
    driver->init();
    assert(!usb_ecm_data_ready());
    tusb_control_request_t request = {
        .bmRequestType = 0x01,
        .bRequest = TUSB_REQ_SET_INTERFACE,
        .wIndex = 1,
        .wValue = 1,
    };
    assert(driver->control_xfer_cb(0, CONTROL_STAGE_SETUP, &request));
    assert(usb_ecm_data_ready());
    driver->reset(0);
    assert(!usb_ecm_data_ready());

    /* A rejected SET_INTERFACE or unrelated request must not enable bulk TX. */
    control_result = false;
    assert(!driver->control_xfer_cb(0, CONTROL_STAGE_SETUP, &request));
    assert(!usb_ecm_data_ready());
    control_result = true;
    assert(driver->control_xfer_cb(0, CONTROL_STAGE_ACK, &request));
    assert(!usb_ecm_data_ready());
    request.wIndex = 3; /* CDC console data interface. */
    assert(driver->control_xfer_cb(0, CONTROL_STAGE_SETUP, &request));
    assert(!usb_ecm_data_ready());
    request.wIndex = 1;
    request.bmRequestType = 0x21; /* Class request, not standard SET_INTERFACE. */
    assert(driver->control_xfer_cb(0, CONTROL_STAGE_SETUP, &request));
    assert(!usb_ecm_data_ready());
    request.bmRequestType = 0x01;
    assert(driver->control_xfer_cb(0, CONTROL_STAGE_SETUP, &request));
    assert(usb_ecm_data_ready());
    request.wValue = 0;
    assert(driver->control_xfer_cb(0, CONTROL_STAGE_SETUP, &request));
    assert(!usb_ecm_data_ready());
    request.wValue = 1;
    assert(driver->control_xfer_cb(0, CONTROL_STAGE_SETUP, &request));
    assert(usb_ecm_data_ready());
    assert(calls == 8); /* All control stages are delegated to TinyUSB. */
    puts("PASS: ECM init/reset, alt 0/1, rejected/unrelated control requests, delegation");
    return 0;
}
