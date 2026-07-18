#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t provisioning_portal_start(void);
void provisioning_portal_stop(void);
bool provisioning_portal_is_active(void);
int64_t provisioning_portal_last_activity_us(void);

#ifdef __cplusplus
}
#endif
