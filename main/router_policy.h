#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Addresses are host byte order, independent of lwIP. */
bool router_choose_lan(uint32_t wan, uint32_t mask, uint32_t *gateway);
bool router_client_allowed(uint32_t peer, uint32_t lan, uint32_t mask, uint32_t usb_host);
