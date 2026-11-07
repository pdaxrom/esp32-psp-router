/* Use the pinned TinyUSB's actual headers with its bare-metal host-test OSAL. */
#define CFG_TUSB_MCU OPT_MCU_NONE
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED 1
#define CFG_TUD_ECM_RNDIS 1
#define CFG_TUD_ENDPOINT0_SIZE 64
#define TUP_DCD_ENDPOINT_MAX 8
