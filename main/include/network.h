#ifndef NETWORK_H
#define NETWORK_H

#include "esp_err.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include <stdint.h>
// Bring up the ESP-IDF networking foundation: TCP/IP stack, default event
// loop, and an Ethernet netif object that the W5500 driver will attach to.
esp_err_t network_init(void);
// ESP32 S3 ETH uses a SPI ethernet controller not a MAC + external
// Initializing mac and phy, the ethernet driver components
esp_err_t mac_phy_init(void);
// driver initialization with mac and phy
// Install the ESP-IDF Ethernet driver instance backed by the initialized
// W5500 MAC/PHY objects.
esp_err_t driver_init(void);
// IP acquisition callback. Once the interface has a valid address we set an
// event bit so the rest of the application can safely start network services.
void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
// Replace DHCP with a known static IP. This simplifies multi-node deployment
// and makes it easier for the backend/controller to address nodes directly.
esp_err_t assign_static_ip(const char* ip_addr); 
// connecting the drivers and the network
esp_err_t attach_driver_to_network(const char* ip_addr, EventGroupHandle_t* main_group);

// Bring up the full Ethernet path in dependency order.
void network_start(void);


#endif