/** esp_app_desc 的最小替身：ui_service 只读镜像版本串，其余字段不参与。 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t magic_word;
  uint32_t secure_version;
  uint32_t reserv1[2];
  char version[32];
  char project_name[16];
} esp_app_desc_t;

const esp_app_desc_t *esp_app_get_description(void);

#ifdef __cplusplus
}
#endif
