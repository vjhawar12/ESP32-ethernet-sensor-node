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
#include "rtos_objects.h"
#include "sensor_context.h"


// Main firmware control path. After initialization it waits for OTA request
// events while the rest of the system runs in FreeRTOS tasks
void s3_main(void) {   
	ESP_LOGW(S3_TAG, "s3_main started");
	main_group = xEventGroupCreate();
    collect_group = xEventGroupCreate();
    log_group = xEventGroupCreate();
	sensor_data = (stream_data*)calloc(1, sizeof(stream_data));
	payload = (stream_payload*)calloc(1, sizeof(stream_payload)); 
	payload->_stream_data = sensor_data;
	nvs_init();
	validate_ota(); 
	network_start();
	xEventGroupWaitBits(
		main_group,
		ETH_CONNECTED_BIT,
		pdFALSE,
		pdTRUE,
		portMAX_DELAY
	); 
	i2c_init();
	adc_init();
	gpio_init();
	if (http_get_request() == ESP_OK) {
		parse_manifest(false);
	}
	mutex = xSemaphoreCreateMutex();
	timer_setup();
	xTaskCreate(measure_sensor_values, "Measure sensor values", 4096, NULL, 3, NULL);
	xTaskCreate(heartbeat, "Heartbeat Monitor Task", 4096, NULL, 3, NULL); 
	xTaskCreate(udp_socket_create, "UDP Server Task", 4096, (void *)AF_INET, 5, NULL); 	
	xTaskCreate(udp_stream, "UDP Stream", 4096, payload, 2, NULL);
	xTaskCreate(tcp_server_create, "TCP Server Task", 4096, (void *)AF_INET, 5, NULL); 	
	while (1) {
		xEventGroupWaitBits(
			main_group,
			OTA_REQUESTED_BIT, 
			pdTRUE,
			pdTRUE,
			portMAX_DELAY
		);
		ESP_LOGW(S3_TAG, "OTA request bit received in main loop");
		if (http_get_request() != ESP_OK) {
			continue;
		}		
		parse_manifest(true);
	}
}  	 