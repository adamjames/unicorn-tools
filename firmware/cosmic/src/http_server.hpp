#pragma once

#include <cstdint>
#include <cstddef>

namespace http_server {

bool init(uint16_t port = 80);
void stop();
void poll();
void resolve_allowed_hosts();
int get_active_connections();
bool reboot_requested();
bool reboot_to_bootloader();
void warmup(void (*animate_callback)() = nullptr);

// Pending display operations (to be processed by core 0)
bool has_pending_brightness();
float get_pending_brightness();
bool has_pending_frame();
uint8_t* get_pending_frame_buffer();
void clear_pending_frame();
uint32_t get_frame_sequence();

// Delta frame support
uint16_t get_delta_count();      // 0 = full frame, >0 = number of changed pixels
uint16_t* get_delta_indices();   // Array of pixel indices that changed

// Frame buffer locking for safe cross-core access
void acquire_frame_lock();
void release_frame_lock();

} // namespace http_server
