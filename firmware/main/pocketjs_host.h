#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the product-owned PocketJS package, guest, UI core, renderer, and runner. */
esp_err_t remapad_pocketjs_start(void);

#ifdef __cplusplus
}
#endif
