#ifndef APP_STATE_H
#define APP_STATE_H
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "app_config.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/i2c_master.h"
#include "freertos/semphr.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

extern EventGroupHandle_t log_group;
extern EventGroupHandle_t main_group;
extern EventGroupHandle_t collect_group; 
extern char server_version_str[64];
extern char firmware_url_str[128];
extern char target_str[16];
extern char flash_size_MB_str[16];
extern char commit_version_str[128];
extern char response_buffer[MAX_HTTP_OUTPUT_BUFFER + 1];
extern int manifest_overflow;
extern char ip_addr[16];
extern adc_oneshot_unit_handle_t adc_oneshot_handle; 
extern adc_cali_handle_t adc_cali_handle; 
extern i2c_master_dev_handle_t aht20_handle, hcsr505_handle;
extern SemaphoreHandle_t mutex;

typedef struct stream_data {
	char chip[16];
	char firmvers[16];
	char ip[16];
	char temperature[16];
	char humidity[16];
	char air_quality[16];
	bool motion_detected;
} stream_data; 

// Transport-side context for the UDP streaming task. Bundles the latest
// payload string, destination address, and socket handle.
typedef struct stream_payload {
	stream_data* _stream_data;
	int sock;
	struct sockaddr_in dest_addr;
	char msg[512];
} stream_payload;


void groups_init(void);
void stream_init(void);

#endif