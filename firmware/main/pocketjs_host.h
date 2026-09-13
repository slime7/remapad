#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start the product-owned PocketJS owner task: package, guest, UI core,
 * binding, renderer, and the UI turn loop all run on that one task. */
esp_err_t remapad_pocketjs_start(void);

#ifdef __cplusplus
}
#endif
