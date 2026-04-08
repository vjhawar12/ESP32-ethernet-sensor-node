#include "s3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
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

// Exceeding around 1024 causes stack overflow
#define MAX_HTTP_OUTPUT_BUFFER 1024
#define ETH_CONNECTED_BIT BIT0
#define OTA_REQUESTED_BIT BIT1
// NOTE: These constants are specific to the ESP32-S3-ETH boards. They use the following schematic: 
// https://files.waveshare.com/wiki/ESP32-S3-ETH/ESP32-S3-ETH-Schematic.pdf
#define S3_RESET_GPIO 9
#define S3_INT_GPIO 10
#define S3_MOSI_GPIO 11
#define S3_MISO_GPIO 12
#define S3_SCLK_GPIO 13
#define S3_CS_GPIO 14
// 36000000000ULL = 10 hours
#define UPDATE_CHECK_PERIOD_US 36000000000ULL
// not needed, so assigning it a default value
#define S3_ETH_PHY_ADDR 1
// conservative speed
#define S3_SPI_CLK_MHZ 20
#define S3_TAG "ESP_S3_ETH Client Instance"
#define TAG "HTTP_CLIENT_GET"
// ip adddresses for secondaries
#define IP_ADDR_0 "192.168.3.200"
#define IP_ADDR_1 "192.168.3.201"
#define IP_ADDR_2 "192.168.3.202"
#define IP_ADDR_3 "192.168.3.203"
#define IP_ADDR_4 "192.168.3.204"
#define IP_ADDR_5 "192.168.3.205"
#define IP_ADDR_6 "192.168.3.206"
// this is not a real node, its a test one
#define DEFAULT_IP_ADDR IP_ADDR_6
#define MANIFEST "https://192.168.3.125:4443/manifest.json"
#define SUBNET "255.255.255.0"
#define GATEWAY "192.168.3.1"
// port for udp server
#define PORT 5000

static char response_buffer[MAX_HTTP_OUTPUT_BUFFER + 1] = {0};
extern const char server_cert_pem_start[] asm("_binary_cert_pem_start");
char ip_addr[16] = DEFAULT_IP_ADDR;
esp_eth_mac_t *mac = NULL;
esp_eth_phy_t *phy = NULL;
esp_netif_t *eth_netif = NULL;
esp_eth_handle_t eth_handle = NULL;
spi_device_handle_t spi_handle = NULL;
static EventGroupHandle_t group; 

static esp_err_t network_init() {
	ESP_RETURN_ON_ERROR(esp_netif_init(), S3_TAG, "esp_netif failed"); // connects TCP/IP stack to the hardware
	ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), S3_TAG, "esp_event_loop_create_default failed"); // event loop that reports link status and network changes, not packet transmission	
	esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH(); // configuration structure for Ethernet
	eth_netif = esp_netif_new(&cfg); // create network interface for Ethernet driver

	if (!eth_netif) {
		ESP_LOGE(S3_TAG, "esp_netif_new failed");
		return ESP_FAIL;
	}

	return ESP_OK;
}


// ESP32 S3 ETH uses a SPI ethernet controller not a MAC + external
// Initializing mac and phy, the ethernet driver components
static esp_err_t mac_phy_init() {
	eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();      // apply default common MAC configuration
	eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();      // apply default PHY configuration
	phy_config.phy_addr = S3_ETH_PHY_ADDR;           // alter the PHY address according to your board design
	phy_config.reset_gpio_num = S3_RESET_GPIO; // alter the GPIO used for PHY reset
	// Install GPIO interrupt service (as the SPI-Ethernet module is interrupt-driven)
	ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), S3_TAG, "gpio_install_isr_service failed");
	// SPI bus configuration
	spi_bus_config_t buscfg = {
		.miso_io_num = S3_MISO_GPIO,
		.mosi_io_num = S3_MOSI_GPIO,
		.sclk_io_num = S3_SCLK_GPIO,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
	};
	ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO), S3_TAG, "spi_bus_initialize failed");
	// Configure SPI device
	spi_device_interface_config_t spi_devcfg = {
		.mode = 0,
		.clock_speed_hz = S3_SPI_CLK_MHZ * 1000 * 1000,
		.spics_io_num = S3_CS_GPIO,
		.queue_size = 20
	};
	eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &spi_devcfg);
	w5500_config.int_gpio_num = S3_INT_GPIO;
	mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
	phy = esp_eth_phy_new_w5500(&phy_config);

	if (!mac) {
		ESP_LOGE(S3_TAG, "esp_eth_mac_new_w5500 failed");
		return ESP_FAIL;
	}
	if (!phy) {
		ESP_LOGE(S3_TAG, "esp_eth_phy_new_w5500 failed");
		return ESP_FAIL;
	}

	return ESP_OK;
}  	


// driver initialization with mac and phy
static esp_err_t driver_init() {
	esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
	ESP_RETURN_ON_ERROR(esp_eth_driver_install(&config, &eth_handle), S3_TAG, "ETH_DEFAULT_CONFIG failed");
	return ESP_OK;
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address\n");
	ESP_LOGI(TAG, "ETH IP: " IPSTR "\n", IP2STR(&ip_info->ip));
	ESP_LOGI(TAG, "ETH MASK: " IPSTR "\n", IP2STR(&ip_info->netmask));
	ESP_LOGI(TAG, "ETH GW: " IPSTR "\n", IP2STR(&ip_info->gw));
	fflush(stdout);

	xEventGroupSetBits(group, ETH_CONNECTED_BIT);
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_START:
            ESP_LOGI(S3_TAG, "Ethernet Started");
            break;
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(S3_TAG, "Ethernet Link Up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(S3_TAG, "Ethernet Link Down");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(S3_TAG, "Ethernet Stopped");
            break;
        default:
            break;
    }
}

static esp_err_t assign_static_ip(const char* ip_addr) {
	// switching from dhcp to static ip
	ESP_RETURN_ON_ERROR(esp_netif_dhcpc_stop(eth_netif), S3_TAG, "esp_netif_dhcpc_stop failed");

	esp_netif_ip_info_t ip;
	// clearing struct before setting
    memset(&ip, 0 , sizeof(esp_netif_ip_info_t));
	// assigning IP
	ip.ip.addr = ipaddr_addr(ip_addr);
	// assigning subnet (devices that are considered local)
    ip.netmask.addr = ipaddr_addr(SUBNET);
	// assigning gateway (home router)
    ip.gw.addr = ipaddr_addr(GATEWAY);

	ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(eth_netif, &ip), S3_TAG, "esp_netif_set_ip_info failed");

	ESP_LOGD(S3_TAG, "Success to set static ip: %s, netmask: %s, gw: %s", ip_addr, SUBNET, GATEWAY);

    return ESP_OK;
}


// connecting the drivers and the network
static esp_err_t attach_driver_to_network(const char* ip_addr) {
	// starting handler to catch errors
	ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL), S3_TAG, "eth_event_handler failed");
	ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL), S3_TAG, "got_ip_event_handler failed");
	// attach Ethernet driver to TCP/IP stack
	ESP_RETURN_ON_ERROR(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)), S3_TAG, "esp_netif_attach failed");
	// assigning static IPs
	ESP_ERROR_CHECK(assign_static_ip(ip_addr));
	// starting ethernet
	ESP_RETURN_ON_ERROR(esp_eth_start(eth_handle), S3_TAG, "esp_eth_start failed");

	return ESP_OK;
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static int output_len;       // Stores number of bytes read
    switch(evt->event_id) {
		// error on GET request
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
		// TCP connection formed 
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
			// must reset output_len to accept new data later
			output_len = 0;
            break;
		// HTTP request sent
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
		// receiving response headers
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
		// receiving response data
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            // Clean the buffer in case of a new request
            if (output_len == 0) {
                memset(response_buffer, 0, MAX_HTTP_OUTPUT_BUFFER + 1);
            }
			
			// if new data
			int copy_len = MIN(evt->data_len, (MAX_HTTP_OUTPUT_BUFFER - output_len));
			if (copy_len) {
				// copy output data into buffer
				memcpy((char *)response_buffer + output_len, evt->data, copy_len);
				output_len += copy_len;
				// dont forget to null-terminate text
            	((char *)response_buffer)[output_len] = '\0';
			}
            break;
		// response finished
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            output_len = 0;
            break;
        default:
            break;
    }
    return ESP_OK;
}


void http_get_request() {
    esp_http_client_config_t config = {
        .url = MANIFEST,	
        .method = HTTP_METHOD_GET,
		.event_handler = _http_event_handler,
		.cert_pem = server_cert_pem_start,
        .user_data = response_buffer, // Pass buffer to the event handler
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %" PRId64,
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
        // Response data will be in response_buffer if handled via user_data
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    } 	
	ESP_LOGI(TAG, "Response body: \n %s", response_buffer); 

    esp_http_client_cleanup(client);
}

int less_than(const char *current, const char *latest)
{
    int c_major, c_minor, c_patch;
    int l_major, l_minor, l_patch;

    sscanf(current, "%d.%d.%d", &c_major, &c_minor, &c_patch);
    sscanf(latest,  "%d.%d.%d", &l_major, &l_minor, &l_patch);

    if (c_major != l_major)
        return c_major < l_major;

    if (c_minor != l_minor)
        return c_minor < l_minor;

    return c_patch < l_patch;
}

void parse_manifest() {
	ESP_LOGI(TAG, "Raw manifest response: %s", response_buffer);
	cJSON *json = cJSON_Parse(response_buffer);

	if (json == NULL) {
		ESP_LOGI(TAG, "NULL json\n");
		return;
	}

	cJSON* version = cJSON_GetObjectItemCaseSensitive(json, "version");

    if (cJSON_IsString(version) && (version->valuestring != NULL)) {
        ESP_LOGI(TAG, "Version: %s\n", version->valuestring);

		cJSON *firmware_url = cJSON_GetObjectItemCaseSensitive(json, "firmware_url");
		if (cJSON_IsString(firmware_url) && (firmware_url->valuestring != NULL)) {
			ESP_LOGI(TAG, "Firmware URL: %s\n", firmware_url->valuestring);
		} else {
			ESP_LOGI(TAG, "Missing firmware URL\n");
			cJSON_Delete(json);
			return;
		}

		const esp_app_desc_t *app_desc = esp_app_get_description();

		ESP_LOGI(TAG, "Current app version: %s", app_desc->version);
		ESP_LOGI(TAG, "Manifest version: %s", version->valuestring);

		// comparing current version (app_desc->version) with latest version (version->valuestring)
		if (less_than(app_desc->version, version->valuestring)) {
			// trigger OTA
				 esp_http_client_config_t http_config = {
					.url = firmware_url->valuestring,
					.cert_pem = server_cert_pem_start,
					.keep_alive_enable = true,
				};

				esp_https_ota_config_t ota_config = {
					.http_config = &http_config,
				};
				
				ESP_LOGI(TAG, "Newer firmware detected, starting OTA...");
				ESP_LOGI(TAG, "OTA URL: %s", firmware_url->valuestring);
				esp_err_t err = esp_https_ota(&ota_config);

				if (err == ESP_OK) {
					ESP_LOGI(TAG, "OTA triggered succesfully! Flashing new code now ...");
					esp_restart();
				} else {
					ESP_LOGI(TAG, "Raw manifest response: %s", response_buffer);
					ESP_LOGI(TAG, "Current app version: %s", app_desc->version);
					ESP_LOGI(TAG, "Manifest version: %s", version->valuestring);
					ESP_LOGI(TAG, "OTA URL: %s", firmware_url->valuestring);
					ESP_LOGE(TAG, "OTA Failed! %s (0x%x)", esp_err_to_name(err), err);
				}
		} else {
			ESP_LOGI(TAG, "Already up-to-date with firmware"); 
		}	
    } else {
		ESP_LOGI(TAG, "Invalid JSON \n");
	}
	cJSON_Delete(json);
}

static bool IRAM_ATTR timer_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t* edata, void* usr_ctx) {
	BaseType_t xHigherPriorityTaskWoken, xResult;
    xHigherPriorityTaskWoken = pdFALSE; // ensures the scheduler does not preempt this ISR
	xEventGroupSetBitsFromISR(group, OTA_REQUESTED_BIT, &xHigherPriorityTaskWoken);

	if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR(); // yields only after the ISR is finished
    }

	return false;
}


void timer_setup() {
	gptimer_handle_t timer; 
	gptimer_config_t timer_config = {
		.clk_src = GPTIMER_CLK_SRC_DEFAULT,
		.direction = GPTIMER_COUNT_UP, 
		.resolution_hz = 1000000, // one tick = 1 us	
	}; 

	gptimer_alarm_config_t alarm_config = {
		.reload_count = 0,
		.alarm_count = UPDATE_CHECK_PERIOD_US,
		.flags.auto_reload_on_alarm = true
	};

	gptimer_event_callbacks_t cbs = {
		.on_alarm = timer_callback,
	}; 

	ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &timer));
	ESP_ERROR_CHECK(gptimer_set_alarm_action(timer, &alarm_config));
	ESP_ERROR_CHECK(gptimer_register_event_callbacks(timer, &cbs, NULL));
	ESP_ERROR_CHECK(gptimer_enable(timer));
	ESP_ERROR_CHECK(gptimer_start(timer));
}


void udp_server_create(void* pvParams) {
	char rx_buffer[128];
    char addr_str[128];
	int addr_family = (int)pvParams; // ipv4
	struct sockaddr_in dest_addr_ip4;
	int ip_protocol = 0;

	while (1) {
		dest_addr_ip4.sin_addr.s_addr = htonl(INADDR_ANY);
		dest_addr_ip4.sin_family = AF_INET;
		dest_addr_ip4.sin_port = htons(PORT);
		ip_protocol = IPPROTO_IP;

		int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);

		if (sock < 0) {
			ESP_LOGI(S3_TAG, "Failed to create socket");  
			continue;
		}

		ESP_LOGI(S3_TAG, "Created socket"); 

		int err = bind(sock,  (struct sockaddr *)&dest_addr_ip4, sizeof(dest_addr_ip4));
        if (err < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
			close(sock);
			continue;
        }
        ESP_LOGI(TAG, "Socket bound, port %d", PORT);

		struct sockaddr_storage source_addr; 
		socklen_t socklen = sizeof(source_addr);

		while (1) {
            ESP_LOGI(TAG, "Waiting for data");
			int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
			
			if (len < 0) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                break;
            } else {
				rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string...

				if (source_addr.ss_family == PF_INET) {
					inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
				}

                ESP_LOGI(TAG, "Received %d bytes from %s:", len, addr_str);
                ESP_LOGI(TAG, "%s", rx_buffer);
			}
			
			// returns 0 = match
			if (!strcmp(rx_buffer, "OTA_REQUESTED")) {
				xEventGroupSetBits(group, OTA_REQUESTED_BIT); 
			}

		}

		shutdown(sock, 0); 
		close(sock); 
	}
}

// stores the ip address (found from nvs) of the node in ip_addr. If not found in NVS, it stores DEFAULT_IP_ADDR
void load_ip() {
	esp_err_t err = nvs_flash_init();
	nvs_handle_t nvs_handle; 

	if (err != ESP_OK) {
		ESP_LOGE(S3_TAG, "Could not initialize NVS partition");
	} else {
		ESP_LOGI(S3_TAG, "Opening NVS partition ...");
		err = nvs_open("netcfg", NVS_READWRITE, &nvs_handle); 
		
		if (err != ESP_OK) {
			ESP_LOGE(S3_TAG, "Could not open NVS partition");
		} else {
			size_t ip_addr_len = sizeof(ip_addr); 
			err = nvs_get_str(nvs_handle, "ip_addr", ip_addr, &ip_addr_len); 

			if (err == ESP_OK) {
				ESP_LOGI(S3_TAG, "Retrieved ip address %s", ip_addr); 
				nvs_close(nvs_handle);
			} else {
				ESP_LOGI(S3_TAG, "IP Address not found, writing %s", ip_addr);
				err = nvs_set_str(nvs_handle, "ip_addr", ip_addr);
				ESP_ERROR_CHECK((nvs_handle));
				ESP_LOGI(S3_TAG, "Successfully wrote %s to ip_addr", ip_addr); 
				nvs_close(nvs_handle);
			}
		}
	}
}

void validate_ota() {
	const esp_partition_t *running = esp_ota_get_running_partition();

	if (running != NULL) {
		if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 || running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
			esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
			if (err == ESP_OK) {
				ESP_LOGI(S3_TAG, "Marked OTA app valid");
			} else {
				ESP_LOGW(S3_TAG, "Failed to mark OTA app valid: %s", esp_err_to_name(err));
			}
		} else {
			ESP_LOGI(S3_TAG, "Running from factory partition, not marking OTA valid");
		}
	}	
}

void network_start() {
	ESP_ERROR_CHECK(network_init());
	ESP_ERROR_CHECK(mac_phy_init());
	ESP_ERROR_CHECK(driver_init());
	ESP_ERROR_CHECK(attach_driver_to_network(ip_addr));
}

void s3_main() {   
	ESP_LOGI(S3_TAG, "s3_main started");
	group = xEventGroupCreate();

	load_ip(); 
	validate_ota(); 
	network_start();
	
	xEventGroupWaitBits(
		group,
		ETH_CONNECTED_BIT,
		pdFALSE,
		pdTRUE,
		portMAX_DELAY
	); 

	timer_setup();
	xTaskCreate(udp_server_create, "UDP Server Task", 4096, (void *)AF_INET, 5, NULL); 	

	while (1) {
		xEventGroupWaitBits(
			group,
			OTA_REQUESTED_BIT, 
			pdTRUE,
			pdTRUE,
			portMAX_DELAY
		);

		http_get_request();
		parse_manifest();
	}
}  	 