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
#include "rtos_objects.h"
#include "sensor_context.h"
#include "manifest.h"


// Remove CR/LF from a received console command so string comparison works
// reliably for commands typed from nc/telnet-style clients.
void strfilter(char* buffer, int length) {
	int i = 0;
	while (i < length && buffer[i] != 0) {
		if (buffer[i] == '\r' || buffer[i] == '\n') {
			buffer[i] = 0;
			break;
		}
		i++;
	}
}

// Create and bind the UDP socket used for telemetry, then spawn the task that
// transmits payloads on stream events.
void udp_socket_create(void* pv_params) {
	int addr_family = (int)pv_params; // ipv4
	struct sockaddr_in dest_addr_ip4 = {0};
	struct sockaddr_in dest_addr = {0};
	int ip_protocol = IPPROTO_IP;
	int sock; 
	sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
	if (sock < 0) {
		ESP_LOGI(S3_TAG, "Failed to create socket");  
		return;
	}
	dest_addr_ip4.sin_addr.s_addr = htonl(INADDR_ANY);
	dest_addr_ip4.sin_family = AF_INET;
	dest_addr_ip4.sin_port = htons(PORT_UDP);
	int res = inet_pton(AF_INET, STATION3_IP, &dest_addr.sin_addr);
	if (res != 1) {
		ESP_LOGI(S3_TAG, "Invalid Station3 IP");
		close(sock);
		return;  
	}
	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(PORT_UDP);
	payload->sock = sock;
	payload->dest_addr = dest_addr;
	ESP_LOGI(S3_TAG, "Created socket"); 
	int err = bind(sock,  (struct sockaddr *)&dest_addr_ip4, sizeof(dest_addr_ip4));
	if (err < 0) {
		ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
		close(sock);
		return;
	}
	ESP_LOGI(TAG, "UDP Socket bound, port %d", PORT_UDP);
}

// Receive one command from the TCP console client
int rx(char* message, int sock) {
	int len = recv(sock, message, 127, 0);
	if (len < 0) {
		ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
		return -1;
	} else if (len > 0) {
		message[len] = 0; 
		ESP_LOGI(TAG, "Received %d bytes: %s", len, message);
		return 1;
	} else {
		ESP_LOGI(TAG, "Client closed connection.");
		return 0;
	}
}

// Send the full message, retrying until all bytes are written or the socket
// reports an error.
void tx(const char* message, int sock) {
	int to_write = strlen(message);
	int len = strlen(message);
	while (to_write > 0) {
		int written = send(sock, message + (len - to_write), to_write, 0);
		if (written < 0) {
			ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
			return;
		}
		to_write -= written;
	}
}

void handle_exit(char* tx_buffer, int sock) {
	tx("Session terminated\n", sock);
}

void handle_help(char* tx_buffer, int sock) {
	tx("Options:\nreboot\nstream on\nstream off\nota flash\n ota status\n", sock);
}

void handle_reboot(char* tx_buffer, int sock) {
	tx("Restarting...\n", sock);
	esp_restart();
}

void handle_stream_on(char* tx_buffer, int sock) {
	tx("Resuming live streaming of sensor data...\n", sock);
	xEventGroupSetBits(log_group, STREAM_BIT_MANUAL); 
}

void handle_stream_off(char* tx_buffer, int sock) {
	tx("Pausing live streaming of sensor data...\n", sock);
	xEventGroupClearBits(log_group, STREAM_BIT_MANUAL); 
}

void handle_ota_check(char* tx_buffer, int sock, char* server_version_str) {
	snprintf(tx_buffer, 256, "Latest Version: %s\nRunning Version: %s\nLast time ota triggered: %s\n", server_version_str, esp_app_get_description()->version, "NULL");
	tx(tx_buffer, sock);
}

void handle_ota_start(char* tx_buffer, int sock) {
	tx("Initiating OTA...\n", sock);
	xEventGroupSetBits(main_group, OTA_REQUESTED_BIT);
}

void handle_invalid(char* tx_buffer, int sock) {
	tx("Invalid command. Use 'help' to see valid commands\n", sock);
}

// Simple line-oriented TCP console for remote interaction with the node. This
// is intentionally small but useful for remote bring-up and OTA control.
void communicate(int sock, char* server_version_str) {
	char rx_buffer[256];
	char tx_buffer[256];
	int status;
	int exit = 0;
	strncpy(tx_buffer, "ESP32 Node Console\nType 'help' for commands\nType 'exit' to terminate the session\n", sizeof(tx_buffer) - 1);
	tx_buffer[sizeof(tx_buffer) - 1] = 0;
	tx(tx_buffer, sock);
	while (!exit) {
		strncpy(tx_buffer, "esp32$\t", sizeof(tx_buffer) - 1);
		tx_buffer[sizeof(tx_buffer) - 1] = 0;
		tx(tx_buffer, sock);
		status = rx(rx_buffer, sock);	
		if (status < 1) {
			return;
		} 
		strfilter(rx_buffer, 128);
		// returns 0 if equal
		if (!strcmp(rx_buffer, "exit")) {
			handle_exit(tx_buffer, sock);
			exit = 1;
		} else if (!strcmp(rx_buffer, "help")) {
			handle_help(tx_buffer, sock);
		} else if (!strcmp(rx_buffer, "reboot")) {
			handle_reboot(tx_buffer, sock);
		} else if (!strcmp(rx_buffer, "stream on")) {
			handle_stream_on(tx_buffer, sock);
		} else if (!strcmp(rx_buffer, "stream off")) {
			handle_stream_off(tx_buffer, sock);
		} else if (!strcmp(rx_buffer, "ota status")) {
			handle_ota_check(tx_buffer, sock, server_version_str);
		} else if (!strcmp(rx_buffer, "ota flash")) {
			handle_ota_start(tx_buffer, sock);
		} else {
			handle_invalid(tx_buffer, sock);
		}
	}
}

// Start the TCP console server. Each accepted connection is handled in-place
// and closed before the server returns to listen for the next client.
void tcp_server_create(void* pv_params) {
	const int buffer_length = 128;
	char addr_str[buffer_length]; // client ip address 
	int ipv4 = (int)pv_params; // using ipv4 strictly
	int ip_protocol = IPPROTO_IP; // default protocol
	struct sockaddr_storage dest_addr; // address the server will bind to
	struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in*) &dest_addr;
	dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY); // bind to all possible IPs on the ESP
	dest_addr_ip4->sin_family = AF_INET; // using ipv4
	dest_addr_ip4->sin_port = htons(PORT_TCP); // assign port PORT_TCP
	ip_protocol = IPPROTO_IP;
	// socket used to wait for clients
	int listen_socket = socket(ipv4, SOCK_STREAM, ip_protocol);
	if (listen_socket < 0) {
		ESP_LOGE(TAG, "Unable to create TCP socket");
		return; 
	}
	int keep_alive = 1;
	int disable_delay = 1;
	int idle_seconds = 30;
	int keepalive_intvl = 5;
	int probe_count = 3;
	int reuse = 1;
	// letting server bind to same address/port after reset
	setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int));
	// attaching socket to ip address at a specific port
	int err = bind(listen_socket, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
	if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during bind: errno %d", errno);
		return;
    }
	// wait for connection requests
	err = listen(listen_socket, 1); 
	if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
		return;
    }
	while (1) {
		ESP_LOGI(TAG, "TCP Socket bound and listening on port %d", PORT_TCP);
		struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
		// accept incoming connection on a new socket used for communication
        int sock = accept(listen_socket, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }
		// enabling keep alive probes on socket
		setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keep_alive, sizeof(int));
		// disable delay during transmission 
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &disable_delay, sizeof(int));
		// how long to idle for before first keep alive probe 
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle_seconds, sizeof(int));	
		// time between keepalive probes 
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepalive_intvl, sizeof(int));	
		// number of keepalive probes 
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &probe_count, sizeof(int));	
		if (source_addr.ss_family == PF_INET) {
			// converting client ip to stringan
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
			ESP_LOGI(TAG, "Listening to client at ip %s", addr_str);
			communicate(sock, server_version_str);
        }
		shutdown(sock, 0);
		close(sock);
	}
	shutdown(listen_socket, 0);
	close(listen_socket);
}


// Wait for either the periodic stream event or a manual stream-enable flag,
// then send the latest formatted payload to the backend over UDP.
void udp_stream(void* pvParams) {
	stream_payload* payload = (stream_payload*) pvParams;
	while (1) {
		xEventGroupWaitBits(
			log_group,
			STREAM_BIT | STREAM_BIT_MANUAL,
			pdFALSE, 
			pdTRUE,
			portMAX_DELAY
		);
		if (mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
			snprintf(payload->msg, sizeof(payload->msg), "{\n\t\"chip\"\t:\t\"%s\",\n\t\"firmvers\"\t:\t\"%s\",\n\t\"ip\"\t:\t\"%s\",\n\t\"temperature\"\t:\t\"%s\",\n\t\"humidity\"\t:\t\"%s\",\n\t\"air_quality\"\t:\t\"%s\",\n\t\"motion_detected\"\t:\t%s\n}", 
				sensor_data->chip, sensor_data->firmvers, sensor_data->ip, sensor_data->temperature, sensor_data->humidity, sensor_data->air_quality, sensor_data->motion_detected? "true" : "false" 
			);
			int err = sendto(payload->sock, payload->msg, strlen(payload->msg), 0, (struct sockaddr *)&payload->dest_addr, sizeof(payload->dest_addr));
			if (err < 0) {
				ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
			} else {
				ESP_LOGI(TAG, "Message sent");
			}
			xSemaphoreGive(mutex);
		}
		xEventGroupClearBits(log_group, STREAM_BIT);
	}
}