#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "app_state.h"
#include "app_config.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/i2c_master.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

EventGroupHandle_t log_group;
EventGroupHandle_t main_group;
EventGroupHandle_t collect_group; 
char server_version_str[64];
char firmware_url_str[128];
char target_str[16];
char flash_size_MB_str[16];
char commit_version_str[128];
char response_buffer[MAX_HTTP_OUTPUT_BUFFER + 1] = {0};
int manifest_overflow = 0;
char ip_addr[16] = DEFAULT_IP_ADDR;
adc_oneshot_unit_handle_t adc_oneshot_handle; 
adc_cali_handle_t adc_cali_handle; 
i2c_master_dev_handle_t aht20_handle, hcsr505_handle;
SemaphoreHandle_t mutex; 
stream_data* sensor_data;
stream_payload* payload;

void groups_init(void) {
    main_group = xEventGroupCreate();
    collect_group = xEventGroupCreate();
    log_group = xEventGroupCreate();
}

void stream_init(void) {
    sensor_data = (stream_data*)calloc(1, sizeof(stream_data));
	payload = (stream_payload*)calloc(1, sizeof(stream_payload)); 
	payload->_stream_data = sensor_data;
}

