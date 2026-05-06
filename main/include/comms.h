#ifndef COMMS_H
#define COMMS_H

// Create and bind the UDP socket used for telemetry, then spawn the task that
// transmits payloads on stream events.
void udp_socket_create(void* pv_params);

// Receive one command from the TCP console client
int rx(char* message, int sock);

// Send the full message, retrying until all bytes are written or the socket
// reports an error.
void tx(const char* message, int sock);


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