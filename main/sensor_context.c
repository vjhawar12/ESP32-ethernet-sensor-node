#include "sensor_context.h"
#include "lwip/sockets.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/i2c_master.h"
#include "freertos/semphr.h"

stream_data* sensor_data;
stream_payload* payload;
adc_oneshot_unit_handle_t adc_oneshot_handle; 
adc_cali_handle_t adc_cali_handle; 
i2c_master_dev_handle_t aht20_handle, hcsr505_handle;

