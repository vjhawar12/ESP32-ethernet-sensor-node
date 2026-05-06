#include "s3.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "driver/spi_master.h"
#include "esp_event.h"
#include "esp_eth_phy_w5500.h"
#include "esp_eth_mac_w5500.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include "sdkconfig.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <sys/param.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "nvs_flash.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "driver/gptimer.h"
#include "stdbool.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "cJSON.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "driver/i2c_slave.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "app_config.h"
#include "network.h"
#include "sensors.h"
#include "comms.h"
#include "ota.h"
#include "http.h"
#include "nvs.h"
#include "periodic.h"
#include "peripherals.h"
#include "sensor_context.h"

void gpio_init(void) {
	gpio_config_t io_intr_conf = {
		.pin_bit_mask = (1ULL << HCSR505_INTR_PIN),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_HIGH_LEVEL,
	}; 
	gpio_config(&io_intr_conf);
	// mac_phy_init() calls gpio_install_isr_service(0) which is needed for gpio_isr_handler_add
	// so ethernet_init must happen before gpio_init
	gpio_isr_handler_add(HCSR505_INTR_PIN, motion_detected_handler, NULL);
}; 


void i2c_init(void) {
    uint8_t success;
	// master
	// hardware controller, pins, timing, etc
	i2c_master_bus_config_t i2c_mst_config = {
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.i2c_port = I2C_NUM_0,
		.scl_io_num = MASTER_SCL_GPIO,
		.sda_io_num = MASTER_SDA_GPIO,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = false, // going to rely on external pullup ~10 kohms instead 
	};
	i2c_master_bus_handle_t i2c_bus_handle;
	ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &i2c_bus_handle)); 
	// slave 1: aht20
	i2c_device_config_t aht20_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = AHT20_ADDR,
		.scl_speed_hz = SCL_FREQUENCY_HZ,	
	};
	// slave 1: pcf8575
	i2c_device_config_t pcf8575_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = PCF8585_ADDR,
		.scl_speed_hz = SCL_FREQUENCY_HZ,	
	};
	ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &aht20_cfg, &aht20_handle)); 
	ESP_ERROR_CHECK(i2c_master_probe(i2c_bus_handle, AHT20_ADDR, -1));
	ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &pcf8575_cfg, &hcsr505_handle)); 
	ESP_ERROR_CHECK(i2c_master_probe(i2c_bus_handle, PCF8585_ADDR, -1));
	uint8_t init = 0x71;
	esp_err_t err; 
	vTaskDelay(pdMS_TO_TICKS(40));
	err = i2c_master_transmit_receive(aht20_handle, &init, 1, &success, 1, -1);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "Error on calibration!");
		return;
	}
	if (!(success & 0x08)) {
		// init required
		uint8_t calibrate[3] = {0xBE, 0x08, 0x00};
		success = 0;
		err = i2c_master_transmit(aht20_handle, calibrate, 3, -1);
		vTaskDelay(pdMS_TO_TICKS(10));
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "Error on calibration!");
			return;
		}
	}
}	

void adc_init() {
	adc_oneshot_unit_init_cfg_t oneshot_config = {
		.unit_id = ADC_UNIT_1, 
		.ulp_mode = ADC_ULP_MODE_DISABLE,
	};
	adc_oneshot_chan_cfg_t chan_config = {
		.bitwidth = ADC_BITWIDTH_13, 
		.atten = ADC_ATTEN_DB_12,
	};
	int adc_raw, adc_cali;
	ESP_ERROR_CHECK(adc_oneshot_new_unit(&oneshot_config, &adc_oneshot_handle));
	adc_oneshot_config_channel(adc_oneshot_handle, ADC_CHANNEL_1, &chan_config); 
	adc_cali_curve_fitting_config_t cali_config = {
		.unit_id = ADC_UNIT_1,
		.atten = ADC_ATTEN_DB_12,
		.bitwidth = ADC_BITWIDTH_13,
	};
	ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle)); 
}
