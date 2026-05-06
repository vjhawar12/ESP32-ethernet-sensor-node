#include "app_config.h"
#include "manifest.h"

char server_version_str[64];
char firmware_url_str[128];
char target_str[16];
char flash_size_MB_str[16];
char commit_version_str[128];
char response_buffer[MAX_HTTP_OUTPUT_BUFFER + 1] = {0};
int manifest_overflow = 0;
char ip_addr[16] = DEFAULT_IP_ADDR;
