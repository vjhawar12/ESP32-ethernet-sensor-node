#ifndef MANIFEST_H
#define MANIFEST_H

#include "app_config.h"

extern char server_version_str[64];
extern char firmware_url_str[128];
extern char target_str[16];
extern char flash_size_MB_str[16];
extern char commit_version_str[128];
extern char response_buffer[MAX_HTTP_OUTPUT_BUFFER + 1]; 
extern int manifest_overflow;
extern char ip_addr[16]; 

#endif