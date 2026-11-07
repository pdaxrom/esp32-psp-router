#pragma once
#include "tinyusb.h"

void usb_descriptors_configure(tinyusb_config_t *config, const uint8_t device_mac[6]);
