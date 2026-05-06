#ifndef NVS_H
#define NVS_H

// stores the ip address (found from nvs) of the node in ip_addr. If not found in NVS, it stores DEFAULT_IP_ADDR
// Load a persistent static IP from NVS. On first boot, write the default node
// IP so future boots reuse a stable address.
void load_ip(void);
void load_chip(void);
void nvs_init(void);

#endif
