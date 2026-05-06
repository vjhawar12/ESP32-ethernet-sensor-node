#ifndef COMMS_H
#define COMMS_H

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



// Remove CR/LF from a received console command so string comparison works
// reliably for commands typed from nc/telnet-style clients.
void strfilter(char* buffer, int length) ;
// Create and bind the UDP socket used for telemetry, then spawn the task that
// transmits payloads on stream events.
void udp_socket_create(void* pv_params);

// Receive one command from the TCP console client
int rx(char* message, int sock);

// Send the full message, retrying until all bytes are written or the socket
// reports an error.
void tx(const char* message, int sock);

void handle_exit(char* tx_buffer, int sock);

void handle_help(char* tx_buffer, int sock);

void handle_reboot(char* tx_buffer, int sock);

void handle_stream_on(char* tx_buffer, int sock);

void handle_stream_off(char* tx_buffer, int sock);

void handle_ota_check(char* tx_buffer, int sock, char* server_version_str);

void handle_ota_start(char* tx_buffer, int sock);

void handle_invalid(char* tx_buffer, int sock); 

// Simple line-oriented TCP console for remote interaction with the node. This
// is intentionally small but useful for remote bring-up and OTA control.
void communicate(int sock, char* server_version_str);

// Start the TCP console server. Each accepted connection is handled in-place
// and closed before the server returns to listen for the next client.
void tcp_server_create(void* pv_params); 


// Wait for either the periodic stream event or a manual stream-enable flag,
// then send the latest formatted payload to the backend over UDP.
void udp_stream(void* pvParams);

#endif