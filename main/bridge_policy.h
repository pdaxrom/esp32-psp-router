#pragma once

#include <stdbool.h>
#include <stdint.h>

bool bridge_password_valid(const char *password);
bool bridge_frame_current(uint32_t packet_epoch, uint32_t usb_epoch,
                           uint32_t age_ms, uint16_t length);
