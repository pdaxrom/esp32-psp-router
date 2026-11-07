#pragma once
#include <stdbool.h>

/* Call only in the TinyUSB task. */
bool usb_ecm_data_ready(void);
