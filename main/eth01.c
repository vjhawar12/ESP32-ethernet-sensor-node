#include "eth01.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "esp_eth_phy.h"
#include "esp_check.h"
#include "lwip/ip4_addr.h"


// NOTE: These constants are specific to the ESP32-ETH-01 boards. They use the following schematic: 
// https://github.com/ldijkman/WT32-ETH01-LAN-8720-RJ45-/blob/main/WT32_ETH01_V2.schematic.pdf

#define ETH_01_MDC_GPIO 23
#define ETH_01_MDIO_GPIO 18

// schematic shows that nRST is pulled up  and grounded, not controlled by the esp
#define ETH_01_PHY_RST_GPIO -1
// schematic shows 1 for this board. Try 0 if commms fail
#define ETH_01_PHY_ADDR 1

#define ETH_01_TAG "ESP_ETH_01 Client Instance"

// ip adddresses for secondaries
#define IP_ADDR_14 "192.168.3.200"
#define IP_ADDR_6 "192.168.3.210"
#define IP_ADDR_7 "192.168.3.211"
#define IP_ADDR_8 "192.168.3.212"
#define IP_ADDR_9 "192.168.3.213"
#define IP_ADDR_10 "192.168.3.214"
#define IP_ADDR_11 "192.168.3.215"
#define IP_ADDR_12 "192.168.3.216"
#define IP_ADDR_13 "192.168.3.221"
#define IP_ADDR_13 "192.168.3.222"

#define SUBNET "255.255.255.0"
#define GATEWAY "192.168.3.1"


esp_eth_mac_t *eth_mac = NULL;
esp_eth_phy_t *eth_phy = NULL;
esp_netif_t *eth01_netif = NULL;
esp_eth_handle_t eth01_handle = NULL;


static esp_err_t eth01_network_init() {
	ESP_RETURN_ON_ERROR(esp_netif_init(), ETH_01_TAG, "esp_netif failed"); // connects TCP/IP stack to the hardware
	ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), ETH_01_TAG, "esp_event_loop_create_default failed"); // event loop that reports link status and network changes, not packet transmission	
	esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH(); // configuration structure for Ethernet
	eth01_netif = esp_netif_new(&cfg); // create network interface for Ethernet driver

	if (!eth01_netif) {
		ESP_LOGE(ETH_01_TAG, "esp_netif_new failed");
		return ESP_FAIL;
	}
	printf("Network successfully initialized\n");
	return ESP_OK;
}

static void eth01_enable_oscillator(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << 16,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_16, 1));
    vTaskDelay(pdMS_TO_TICKS(10));   // give oscillator a moment to start
}


// ESP32 ETH 01 uses a onboard EMAC + PHY chip. We're assuming PHY is lan8720 but we dont have the link to the actual board
// Initializing eth_mac and eth_phy, the ethernet driver components
static esp_err_t eth01_mac_phy_init() {
	eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();                      // apply default common MAC configuration
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG(); // apply default vendor-specific MAC configuration
    esp32_emac_config.smi_gpio.mdc_num = ETH_01_MDC_GPIO;            // alter the GPIO used for MDC signal
    esp32_emac_config.smi_gpio.mdio_num = ETH_01_MDIO_GPIO;          // alter the GPIO used for MDIO signal
	esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
	esp32_emac_config.clock_config.rmii.clock_gpio = EMAC_CLK_IN_GPIO; 
    eth_mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config); // create MAC instance

	if (eth_mac == NULL) {
		printf("MAC could not be created\n");
		return ESP_FAIL;
	}

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();      // apply default PHY configuration
    phy_config.phy_addr = ETH_01_PHY_ADDR; // alter the PHY address according to your board design
    phy_config.reset_gpio_num = ETH_01_PHY_RST_GPIO; // alter the GPIO used for PHY
	eth_phy = esp_eth_phy_new_lan87xx(&phy_config);

	if (eth_phy == NULL) {
		printf("PHY could not be created\n");
		return ESP_FAIL;
	}

	printf("MAC and PHY succesfully initialized\n");

	return ESP_OK;
}  	


// driver initialization with eth_mac and eth_phy
static esp_err_t eth01_driver_init() {
	esp_eth_config_t config = ETH_DEFAULT_CONFIG(eth_mac, eth_phy);
	esp_err_t err = esp_eth_driver_install(&config, &eth01_handle);

	if (err != ESP_OK) {
    	printf("esp_eth_driver_install failed: %s (%d)\n", esp_err_to_name(err), err);
        return err;
    }


	printf("Ethernet drivers succesfully initialized\n");
	return ESP_OK;
}

static void eth01_got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    printf("Ethernet Got IP Address\n");
	printf("ETH IP: " IPSTR "\n", IP2STR(&ip_info->ip));
	printf("ETH MASK: " IPSTR "\n", IP2STR(&ip_info->netmask));
	printf("ETH GW: " IPSTR "\n", IP2STR(&ip_info->gw));
	fflush(stdout);
}

static void eth01_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_START:
            printf("Ethernet Started");
            break;
        case ETHERNET_EVENT_CONNECTED:
            printf("Ethernet Link Up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            printf("Ethernet Link Down");
            break;
        case ETHERNET_EVENT_STOP:
            printf("Ethernet Stopped");
            break;
        default:
            break;
    }
}

static esp_err_t eth01_assign_static_ip(const char* ip_addr) {
	// switching from dhcp to static ip
	ESP_RETURN_ON_ERROR(esp_netif_dhcpc_stop(eth01_netif), ETH_01_TAG, "esp_netif_dhcpc_stop failed");

	esp_netif_ip_info_t ip;
	// clearing struct before setting
    memset(&ip, 0 , sizeof(esp_netif_ip_info_t));
	// assigning IP
	ip.ip.addr = ipaddr_addr(ip_addr);
	// assigning subnet (devices that are considered local)
    ip.netmask.addr = ipaddr_addr(SUBNET);
	// assigning gateway (home router)
    ip.gw.addr = ipaddr_addr(GATEWAY);

	ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(eth01_netif, &ip), ETH_01_TAG, "esp_netif_set_ip_info failed");

	printf("Success to set static ip: %s, netmask: %s, gw: %s", ip_addr, SUBNET, GATEWAY);

#define IP_ADDR_12 "192.168.3.216"
    return ESP_OK;
}


// connecting the drivers and the network
static esp_err_t eth01_attach_driver_to_network(const char* ip_addr) {
	// starting handler to catch errors
	ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth01_event_handler, NULL), ETH_01_TAG, "eth_event_handler failed");
	ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth01_got_ip_event_handler, NULL), ETH_01_TAG, "got_ip_event_handler failed");
	// attach Ethernet driver to TCP/IP stack
	ESP_RETURN_ON_ERROR(esp_netif_attach(eth01_netif, esp_eth_new_netif_glue(eth01_handle)), ETH_01_TAG, "esp_netif_attach failed");
	// assigning static IPs
	ESP_ERROR_CHECK(eth01_assign_static_ip(ip_addr));
	// starting ethernet
	ESP_RETURN_ON_ERROR(esp_eth_start(eth01_handle), ETH_01_TAG, "esp_eth_start failed");

	return ESP_OK;
}


void eth_01_run() {  
	eth01_enable_oscillator();
	ESP_ERROR_CHECK(eth01_network_init());
	ESP_ERROR_CHECK(eth01_mac_phy_init());
	ESP_ERROR_CHECK(eth01_driver_init());
	ESP_ERROR_CHECK(eth01_attach_driver_to_network(IP_ADDR_14));
}  	