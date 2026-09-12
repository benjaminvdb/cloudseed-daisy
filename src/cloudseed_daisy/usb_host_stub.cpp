// Keeps libDaisy's USB host stack out of a firmware that does not use it
// (CLOUDSEED_STRIP_USB_HOST=1 in cloudseed.mk, some 11 KB of flash).
//
// libDaisy's USB interrupt handlers (src/sys/system.cpp) reference the USB
// host's handle, which links the host stack (usbh_conf.c and what it calls,
// some 8 KB of flash). The handle defined here has no instance, so the
// handlers leave it alone, and the stack stays out. The handlers' call into
// the HAL's host driver (HAL_HCD_IRQHandler, 2.5 KB with what it calls) is
// dead for the same reason; cloudseed.mk wraps that symbol to the empty
// function below, so the driver stays out as well. A firmware that does use
// the USB host must not enable this: the handle would be defined twice,
// which the linker reports.
#ifdef STM32H750xx
#include "stm32h7xx_hal.h"

extern "C" {
HCD_HandleTypeDef hhcd_USB_OTG_HS;
void __wrap_HAL_HCD_IRQHandler(HCD_HandleTypeDef*) {}
}
#endif
