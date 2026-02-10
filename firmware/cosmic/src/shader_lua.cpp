// Lua-based shader execution for Unicorn LED Stream
#include "shader_lua.hpp"

extern "C" {
#include "lua/lua.h"
#include "lua/lualib.h"
#include "lua/lauxlib.h"
}

#include <cstdio>
#include <cstring>
#include <cmath>
#include "pico/stdlib.h"

namespace shader_lua {

namespace {

lua_State* L = nullptr;
volatile bool shader_loaded = false;
volatile bool shader_loading = false;  // Flag to pause rendering during load
char error_msg[256] = {0};

// Custom math functions to add to Lua
int lua_clamp(lua_State* L) {
    double val = luaL_checknumber(L, 1);
    double lo = luaL_checknumber(L, 2);
    double hi = luaL_checknumber(L, 3);
    if (val < lo) val = lo;
    if (val > hi) val = hi;
    lua_pushnumber(L, val);
    return 1;
}

int lua_rgb(lua_State* L) {
    // rgb(r, g, b) -> returns r, g, b clamped to 0-255
    int r = (int)luaL_checknumber(L, 1);
    int g = (int)luaL_checknumber(L, 2);
    int b = (int)luaL_checknumber(L, 3);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    lua_pushinteger(L, r);
    lua_pushinteger(L, g);
    lua_pushinteger(L, b);
    return 3;
}

// Register custom functions
void register_shader_functions(lua_State* L) {
    // Add clamp to math table
    lua_getglobal(L, "math");
    lua_pushcfunction(L, lua_clamp);
    lua_setfield(L, -2, "clamp");
    lua_pop(L, 1);
    
    // Add rgb as global
    lua_pushcfunction(L, lua_rgb);
    lua_setglobal(L, "rgb");
}

} // anonymous namespace

bool init() {
    if (L) {
        shutdown();
    }
    
    L = luaL_newstate();
    if (!L) {
        snprintf(error_msg, sizeof(error_msg), "Failed to create Lua state");
        return false;
    }
    
    // Open standard libraries (math, string, table, base)
    luaL_openlibs(L);
    
    // Register our custom functions
    register_shader_functions(L);
    
    error_msg[0] = '\0';
    shader_loaded = false;
    
    printf("Lua VM initialized (version %s)\n", LUA_VERSION);
    return true;
}

bool load_shader(const char* source, size_t len) {
    if (!L) {
        snprintf(error_msg, sizeof(error_msg), "Lua not initialized");
        return false;
    }
    
    // Signal that we're loading - this pauses render_frame
    shader_loading = true;
    shader_loaded = false;
    
    // Brief delay to let any in-progress render complete
    sleep_ms(50);
    
    // Load and execute the shader source
    int status = luaL_loadbuffer(L, source, len, "shader");
    if (status != LUA_OK) {
        snprintf(error_msg, sizeof(error_msg), "Parse error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        shader_loading = false;
        return false;
    }
    
    // Execute the chunk (defines the shader function)
    status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
        snprintf(error_msg, sizeof(error_msg), "Load error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        shader_loading = false;
        return false;
    }
    
    // Verify that either 'shader' or 'render_frame' function exists
    lua_getglobal(L, "render_frame");
    bool has_render_frame = lua_isfunction(L, -1);
    lua_pop(L, 1);
    
    lua_getglobal(L, "shader");
    bool has_shader = lua_isfunction(L, -1);
    lua_pop(L, 1);
    
    if (!has_render_frame && !has_shader) {
        snprintf(error_msg, sizeof(error_msg), "No 'shader' or 'render_frame' function defined");
        shader_loading = false;
        return false;
    }
    
    shader_loaded = true;
    shader_loading = false;
    error_msg[0] = '\0';
    
    if (has_render_frame) {
        printf("Lua shader loaded (%zu bytes) - using render_frame API\n", len);
    } else {
        printf("Lua shader loaded (%zu bytes) - using per-pixel shader API\n", len);
    }
    return true;
}

bool is_loaded() {
    return L && shader_loaded && !shader_loading;
}

bool render_frame(uint8_t* buffer, int width, int height, float t, int frame, float dt) {
    if (!is_loaded() || !buffer) {
        return false;
    }
    
    // Check if render_frame Lua function exists (whole-frame API)
    lua_getglobal(L, "render_frame");
    if (lua_isfunction(L, -1)) {
        // Call render_frame(width, height, t, frame, dt) -> fb_r, fb_g, fb_b
        lua_pushinteger(L, width);
        lua_pushinteger(L, height);
        lua_pushnumber(L, t);
        lua_pushinteger(L, frame);
        lua_pushnumber(L, dt);
        
        int status = lua_pcall(L, 5, 3, 0);
        if (status != LUA_OK) {
            snprintf(error_msg, sizeof(error_msg), "render_frame error: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
            return false;
        }
        
        // Stack now has: fb_r, fb_g, fb_b (tables indexed by y * width + x)
        // Read pixel data from the three tables
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = y * width + x;
                size_t buf_idx = idx * 3;
                
                // Get r from fb_r[idx]
                lua_rawgeti(L, -3, idx);
                int r = lua_isnil(L, -1) ? 0 : (int)lua_tonumber(L, -1);
                lua_pop(L, 1);
                
                // Get g from fb_g[idx]
                lua_rawgeti(L, -2, idx);
                int g = lua_isnil(L, -1) ? 0 : (int)lua_tonumber(L, -1);
                lua_pop(L, 1);
                
                // Get b from fb_b[idx]
                lua_rawgeti(L, -1, idx);
                int b = lua_isnil(L, -1) ? 0 : (int)lua_tonumber(L, -1);
                lua_pop(L, 1);
                
                // Clamp and store
                buffer[buf_idx + 0] = r < 0 ? 0 : (r > 255 ? 255 : r);
                buffer[buf_idx + 1] = g < 0 ? 0 : (g > 255 ? 255 : g);
                buffer[buf_idx + 2] = b < 0 ? 0 : (b > 255 ? 255 : b);
            }
        }
        
        lua_pop(L, 3);  // Pop the three tables
        return true;
    }
    lua_pop(L, 1);  // Pop the nil/non-function
    
    // Fall back to per-pixel shader(x, y, t, frame, dt) API
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // Get the shader function
            lua_getglobal(L, "shader");
            
            // Push arguments: x, y, t, frame, dt
            lua_pushinteger(L, x);
            lua_pushinteger(L, y);
            lua_pushnumber(L, t);
            lua_pushinteger(L, frame);
            lua_pushnumber(L, dt);
            
            // Call shader(x, y, t, frame, dt) -> r, g, b
            int status = lua_pcall(L, 5, 3, 0);
            if (status != LUA_OK) {
                snprintf(error_msg, sizeof(error_msg), "Shader error at (%d,%d): %s", 
                    x, y, lua_tostring(L, -1));
                lua_pop(L, 1);
                return false;
            }
            
            // Get return values (r, g, b) - use tonumber since shaders often return floats
            int r = (int)lua_tonumber(L, -3);
            int g = (int)lua_tonumber(L, -2);
            int b = (int)lua_tonumber(L, -1);
            lua_pop(L, 3);
            
            // Clamp and store
            size_t idx = (y * width + x) * 3;
            buffer[idx + 0] = r < 0 ? 0 : (r > 255 ? 255 : r);
            buffer[idx + 1] = g < 0 ? 0 : (g > 255 ? 255 : g);
            buffer[idx + 2] = b < 0 ? 0 : (b > 255 ? 255 : b);
        }
    }
    
    return true;
}

void unload() {
    // Signal loading to pause render_frame
    shader_loading = true;
    shader_loaded = false;

    // Brief delay to let any in-progress render complete
    sleep_ms(50);

    // Clear the shader function
    if (L) {
        lua_pushnil(L);
        lua_setglobal(L, "shader");
    }

    shader_loading = false;
}

void set_global_int(const char* name, int value) {
    if (L) {
        lua_pushinteger(L, value);
        lua_setglobal(L, name);
    }
}

int get_global_int(const char* name, int default_value) {
    if (!L) return default_value;
    lua_getglobal(L, name);
    int result = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : default_value;
    lua_pop(L, 1);
    return result;
}

void shutdown() {
    if (L) {
        lua_close(L);
        L = nullptr;
    }
    shader_loaded = false;
}

const char* get_error() {
    return error_msg;
}

} // namespace shader_lua
