#ifndef OTA_H
#define OTA_H

// Parse the manifest JSON, capture useful metadata, and trigger OTA when a
// newer firmware image is available.
void parse_manifest(bool can_trigger_ota); 

// If we successfully booted an OTA slot, mark it valid so ESP-IDF cancels any
// pending rollback to the previous image.
void validate_ota(void);

#endif