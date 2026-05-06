#ifndef HTTP_H
#define HTTP_H

#include "esp_err.h"
// Fetch the OTA manifest over HTTPS. The event handler fills response_buffer,
// which is then parsed by parse_manifest().
esp_err_t http_get_request(void);

#endif