#pragma once

#include <cstdint>
#include <cstddef>

namespace shader_lua {

// Initialize the Lua VM
bool init();

// Load shader source code (Lua text)
// Returns true on success
bool load_shader(const char* source, size_t len);

// Check if a shader is loaded and ready
bool is_loaded();

// Render a single frame to the provided buffer
// buffer must be width * height * 3 bytes (RGB)
// t = time in seconds, frame = frame number, dt = delta time
bool render_frame(uint8_t* buffer, int width, int height, float t, int frame, float dt);

// Unload current shader
void unload();

// Set a global integer variable in Lua (for shader parameters)
void set_global_int(const char* name, int value);

// Get a global integer variable from Lua
int get_global_int(const char* name, int default_value = 0);

// Shutdown Lua VM
void shutdown();

// Get last error message (if any)
const char* get_error();

} // namespace shader_lua
