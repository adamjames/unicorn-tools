// Built-in shaders for button cycling
#pragma once

struct BuiltinShader {
    const char* name;
    const char* code;
};

static const BuiltinShader BUILTIN_SHADERS[] = {
    {
        "Plasma",
        R"LUA(
local sin, cos = math.sin, math.cos
function shader(x, y, t, frame, dt)
    local u = x / 32 - 0.5
    local v = y / 32 - 0.5
    local val = sin(u * 10 + t) + sin(v * 10 + t) + sin((u + v) * 10 + t) + sin((u * u + v * v) * 5 + t)
    val = (val + 4) / 8
    local r = sin(val * 3.14159 * 2) * 127 + 128
    local g = sin(val * 3.14159 * 2 + 2.094) * 127 + 128
    local b = sin(val * 3.14159 * 2 + 4.188) * 127 + 128
    return r, g, b
end
)LUA"
    },
    {
        "Fire",
        R"LUA(
local random, floor, min = math.random, math.floor, math.min
local WIDTH, HEIGHT = 32, 32
local heat = {}
local last_frame = -1

for i = 0, WIDTH * HEIGHT - 1 do heat[i] = 0 end

function shader(x, y, t, frame, dt)
    if frame ~= last_frame then
        last_frame = frame
        for i = 0, WIDTH - 1 do
            heat[(HEIGHT - 1) * WIDTH + i] = random(100, 255)
        end
        for row = 0, HEIGHT - 2 do
            for col = 0, WIDTH - 1 do
                local idx = row * WIDTH + col
                local below = (row + 1) * WIDTH + col
                local left = (row + 1) * WIDTH + ((col - 1) % WIDTH)
                local right = (row + 1) * WIDTH + ((col + 1) % WIDTH)
                local avg = (heat[below] + heat[left] + heat[right] + heat[below]) / 4
                heat[idx] = floor(avg * 0.97)
            end
        end
    end
    local h = heat[y * WIDTH + x]
    local r = min(255, h * 1.5)
    local g = min(255, h * 0.5)
    local b = min(255, h * 0.1)
    return r, g, b
end
)LUA"
    },
    {
        "Matrix",
        R"LUA(
local random, floor = math.random, math.floor
local WIDTH, HEIGHT = 32, 32
local drops = {}
local last_frame = -1

for i = 0, WIDTH - 1 do
    drops[i] = { y = random(0, HEIGHT - 1), speed = random(5, 15) / 10, bright = random(150, 255) }
end

local fb = {}

function shader(x, y, t, frame, dt)
    if frame ~= last_frame then
        last_frame = frame
        for i = 0, WIDTH * HEIGHT - 1 do
            local prev = fb[i] or 0
            fb[i] = floor(prev * 0.85)
        end
        for col = 0, WIDTH - 1 do
            local d = drops[col]
            d.y = d.y + d.speed
            if d.y >= HEIGHT then
                d.y = 0
                d.speed = random(5, 15) / 10
                d.bright = random(150, 255)
            end
            local row = floor(d.y)
            if row >= 0 and row < HEIGHT then
                fb[row * WIDTH + col] = d.bright
            end
        end
    end
    local val = fb[y * WIDTH + x] or 0
    return val * 0.3, val, val * 0.3
end
)LUA"
    },
    {
        "Sparkle",
        R"LUA(
local random = math.random
function shader(x, y, t, frame, dt)
    if random() < 0.02 then
        local bright = random(200, 255)
        return bright, bright, bright
    end
    return 0, 0, 0
end
)LUA"
    },
    {
        "Gradient",
        R"LUA(
local sin = math.sin
function shader(x, y, t, frame, dt)
    local hue = (x + y + t * 20) % 64 / 64
    local r = sin(hue * 6.28) * 127 + 128
    local g = sin(hue * 6.28 + 2.09) * 127 + 128
    local b = sin(hue * 6.28 + 4.19) * 127 + 128
    return r, g, b
end
)LUA"
    },
    {
        "Ocean",
        R"LUA(
local sin, cos = math.sin, math.cos
function shader(x, y, t, frame, dt)
    local wave = sin(x * 0.3 + t * 2) * 4 + sin(x * 0.7 - t) * 2
    local depth = (y + wave) / 32
    local r = 10 + depth * 20
    local g = 50 + depth * 100 + sin(t + x * 0.2) * 20
    local b = 150 + depth * 100 + cos(t * 0.7 + y * 0.1) * 30
    return r, g, b
end
)LUA"
    },
    {
        "Stars",
        R"LUA(
local random, floor = math.random, math.floor
local stars = {}
for i = 1, 50 do
    stars[i] = { x = random(0, 31), y = random(0, 31), b = random(100, 255), s = random(10, 30) / 10 }
end

function shader(x, y, t, frame, dt)
    for _, star in ipairs(stars) do
        if floor(star.x) == x and floor(star.y) == y then
            local twinkle = (math.sin(t * star.s) + 1) / 2
            local b = star.b * (0.5 + twinkle * 0.5)
            return b, b, b * 0.9
        end
    end
    return 2, 2, 8
end
)LUA"
    },
    {
        "Spiral",
        R"LUA(
local sin, cos, atan2, sqrt = math.sin, math.cos, math.atan2, math.sqrt
function shader(x, y, t, frame, dt)
    local cx, cy = x - 16, y - 16
    local angle = atan2(cy, cx)
    local dist = sqrt(cx * cx + cy * cy)
    local spiral = (angle + dist * 0.3 - t * 2) % 6.28
    local val = sin(spiral * 3) * 0.5 + 0.5
    local hue = (dist * 0.1 + t * 0.5) % 1
    local r = sin(hue * 6.28) * 127 + 128
    local g = sin(hue * 6.28 + 2.09) * 127 + 128
    local b = sin(hue * 6.28 + 4.19) * 127 + 128
    return r * val, g * val, b * val
end
)LUA"
    },
    {
        "Hypercube",
        R"LUA(
local floor, cos, sin, abs = math.floor, math.cos, math.sin, math.abs
local WIDTH, HEIGHT = 32, 32
local CENTER_X, CENTER_Y = 16, 16
local SCALE, CAMERA_DIST = 28, 1.8
local S = 0.5
local GLOW = 0.1  -- Glow intensity (0 = off, 0.25 = default, 1 = full)
local GLOW_THRESHOLD = 1.6  -- Z distance threshold for 4-sided glow (closer = more glow)

local vertices = {
    {x=S,y=S,z=S,w=-S},{x=-S,y=S,z=S,w=-S},{x=-S,y=-S,z=S,w=-S},{x=S,y=-S,z=S,w=-S},
    {x=S,y=S,z=-S,w=-S},{x=-S,y=S,z=-S,w=-S},{x=-S,y=-S,z=-S,w=-S},{x=S,y=-S,z=-S,w=-S},
    {x=S,y=S,z=S,w=S},{x=-S,y=S,z=S,w=S},{x=-S,y=-S,z=S,w=S},{x=S,y=-S,z=S,w=S},
    {x=S,y=S,z=-S,w=S},{x=-S,y=S,z=-S,w=S},{x=-S,y=-S,z=-S,w=S},{x=S,y=-S,z=-S,w=S},
}
local edges = {
    {1,2},{2,3},{3,4},{4,1},{5,6},{6,7},{7,8},{8,5},{1,5},{2,6},{3,7},{4,8},
    {9,10},{10,11},{11,12},{12,9},{13,14},{14,15},{15,16},{16,13},{9,13},{10,14},{11,15},{12,16},
    {1,9},{2,10},{3,11},{4,12},{5,13},{6,14},{7,15},{8,16}
}
local fb_r, fb_g, fb_b = {}, {}, {}
local projected = {}
local last_frame = -1
-- Reusable points: result holds final rotated vertex, scratch is for chained rotations
local result, scratch = {x=0,y=0,z=0,w=0}, {x=0,y=0,z=0,w=0}

-- 4D rotation functions: rotate point p by angle (cos c, sin s) into output o
local function rotate_xy(p,c,s,o) o.x=p.x*c-p.y*s; o.y=p.x*s+p.y*c; o.z=p.z; o.w=p.w end
local function rotate_xz(p,c,s,o) o.x=p.x*c-p.z*s; o.y=p.y; o.z=p.x*s+p.z*c; o.w=p.w end
local function rotate_xw(p,c,s,o) o.x=p.x*c-p.w*s; o.y=p.y; o.z=p.z; o.w=p.x*s+p.w*c end
local function rotate_yz(p,c,s,o) o.x=p.x; o.y=p.y*c-p.z*s; o.z=p.y*s+p.z*c; o.w=p.w end
local function rotate_yw(p,c,s,o) o.x=p.x; o.y=p.y*c-p.w*s; o.z=p.z; o.w=p.y*s+p.w*c end
local function rotate_zw(p,c,s,o) o.x=p.x; o.y=p.y; o.z=p.z*c-p.w*s; o.w=p.z*s+p.w*c end
local function copy_point(src,dst) dst.x=src.x; dst.y=src.y; dst.z=src.z; dst.w=src.w end

local function project(p,o)
    local w=p.w+CAMERA_DIST; if w<0.1 then w=0.1 end
    local x3,y3,z3=p.x/w,p.y/w,p.z/w
    local z=z3+CAMERA_DIST; if z<0.1 then z=0.1 end
    o.x=CENTER_X+x3/z*SCALE; o.y=CENTER_Y-y3/z*SCALE; o.z=z
end

local function set_pixel(px,py,r,g,b)
    if px>=0 and px<WIDTH and py>=0 and py<HEIGHT then
        local idx=py*WIDTH+px
        if r>(fb_r[idx] or 0) then fb_r[idx]=r end
        if g>(fb_g[idx] or 0) then fb_g[idx]=g end
        if b>(fb_b[idx] or 0) then fb_b[idx]=b end
    end
end

local function draw_line(x0,y0,x1,y1,z0,z1,r,g,b)
    x0,y0=floor(x0+0.5),floor(y0+0.5); x1,y1=floor(x1+0.5),floor(y1+0.5)
    local dx,dy=abs(x1-x0),-abs(y1-y0)
    local sx,sy=x0<x1 and 1 or -1,y0<y1 and 1 or -1
    local err=dx+dy
    local gr,gg,gb=r*GLOW,g*GLOW,b*GLOW
    local full_glow = z0 < GLOW_THRESHOLD and z1 < GLOW_THRESHOLD
    while true do
        set_pixel(x0,y0,r,g,b)
        if GLOW > 0 then
            set_pixel(x0-1,y0,gr,gg,gb); set_pixel(x0,y0-1,gr,gg,gb)
            if full_glow then
                set_pixel(x0+1,y0,gr,gg,gb); set_pixel(x0,y0+1,gr,gg,gb)
            end
        end
        if x0==x1 and y0==y1 then break end
        local e2=2*err
        if e2>=dy then err=err+dy; x0=x0+sx end
        if e2<=dx then err=err+dx; y0=y0+sy end
    end
end

for i=1,#vertices do projected[i]={x=0,y=0,z=0} end

function shader(x, y, t, frame, dt)
    -- Render scene once per frame (not per pixel)
    if frame ~= last_frame then
        last_frame = frame
        fb_r, fb_g, fb_b = {}, {}, {}

        -- rotation_mode: set by D button to change rotation planes
        -- 0: XZ+ZW+YZ (full 4D tumble)
        -- 1: XZ+YZ (3D tumble, no 4D)
        -- 2: ZW only (pure 4D, cubes pass through each other)
        -- 3: XW+YW (4D tumble via W axis)
        -- 4: XY+ZW (spin + 4D)
        local mode = rotation_mode or 0

        -- Two rotation angles for varied motion
        local a1 = t * 0.5
        local a2 = t * 0.5 + sin(t) * 0.5
        local c1, s1 = cos(a1), sin(a1)
        local c2, s2 = cos(a2), sin(a2)

        -- Transform each vertex based on rotation mode
        for i, v in ipairs(vertices) do
            if mode == 0 then
                -- Full 4D tumble
                rotate_xz(v, c1, s1, result)
                rotate_zw(result, c2, s2, scratch)
                rotate_yz(scratch, c1, s1, result)
            elseif mode == 1 then
                -- 3D tumble only
                rotate_xz(v, c1, s1, result)
                rotate_yz(result, c2, s2, scratch)
                copy_point(scratch, result)
            elseif mode == 2 then
                -- Pure 4D rotation
                rotate_zw(v, c1, s1, result)
            elseif mode == 3 then
                -- 4D tumble via W
                rotate_xw(v, c1, s1, result)
                rotate_yw(result, c2, s2, scratch)
                copy_point(scratch, result)
            elseif mode == 4 then
                -- Spin + 4D
                rotate_xy(v, c1, s1, result)
                rotate_zw(result, c2, s2, scratch)
                copy_point(scratch, result)
            else
                -- Default: same as mode 0
                rotate_xz(v, c1, s1, result)
                rotate_zw(result, c2, s2, scratch)
                rotate_yz(scratch, c1, s1, result)
            end
            project(result, projected[i])
        end

        -- Draw edges: green=inner cube, red=outer cube, blue=connecting
        for _, e in ipairs(edges) do
            local r, g, b
            if e[1] <= 8 and e[2] <= 8 then
                r, g, b = 60, 255, 60
            elseif e[1] >= 9 and e[2] >= 9 then
                r, g, b = 255, 60, 60
            else
                r, g, b = 60, 60, 255
            end
            local p1, p2 = projected[e[1]], projected[e[2]]
            draw_line(p1.x, p1.y, p2.x, p2.y, p1.z, p2.z, r, g, b)
        end

        -- Draw vertices
        for i, p in ipairs(projected) do
            local r = i <= 8 and 100 or 255
            local g = i <= 8 and 255 or 100
            local b = i <= 8 and 100 or 100
            set_pixel(floor(p.x + 0.5), floor(p.y + 0.5), r, g, b)
        end
    end

    -- Return pixel color from frame buffer
    local idx = y * WIDTH + x
    return fb_r[idx] or 8, fb_g[idx] or 8, fb_b[idx] or 16
end
)LUA"
    },
    {
        "Hypercube Uneven",
        R"LUA(
-- Consts
local WIDTH, HEIGHT = 32, 32
local CENTER_X, CENTER_Y = 16, 16
local SCALE = 28
local CAMERA_DIST = 1.8
local ROTATION_SPEED_Y = 0.5
local ROTATION_SPEED_X = 0.3
local ROTATION_SPEED_W = 1
local UNEVEN_TIME = true
local CUBE_SIZE = 0.7
local EDGE_BRIGHTNESS = 255
local GLOW_BRIGHTNESS = 64
local GLOW_THRESHOLD = 0.7  -- Brightness threshold for 4-sided glow (higher = closer)
local MIN_BRIGHTNESS = 0.4
local MAX_BRIGHTNESS = 1.0
local INVERT_BRIGHTNESS = true
local BG_R, BG_G, BG_B = 2, 2, 4

-- Geometry
local S = CUBE_SIZE

local vertices = {
  {x= S, y= S, z= S, w=-S}, {x=-S, y= S, z= S, w=-S}, {x=-S, y=-S, z= S, w=-S}, {x= S, y=-S, z= S, w=-S},
  {x= S, y= S, z=-S, w=-S}, {x=-S, y= S, z=-S, w=-S}, {x=-S, y=-S, z=-S, w=-S}, {x= S, y=-S, z=-S, w=-S},
  {x= S, y= S, z= S, w=S}, {x=-S, y= S, z= S, w=S}, {x=-S, y=-S, z= S, w=S}, {x= S, y=-S, z= S, w=S},
  {x= S, y= S, z=-S, w=S}, {x=-S, y= S, z=-S, w=S}, {x=-S, y=-S, z=-S, w=S}, {x= S, y=-S, z=-S, w=S},
}

local edges = {
  {1,2},{2,3},{3,4},{4,1},{5,6},{6,7},{7,8},{8,5},{1,5},{2,6},{3,7},{4,8},
  {9,10},{10,11},{11,12},{12,9},{13,14},{14,15},{15,16},{16,13},{9,13},{10,14},{11,15},{12,16},
  {1,9},{2,10},{3,11},{4,12},{5,13},{6,14},{7,15},{8,16}
}

local fb_r, fb_g, fb_b = {}, {}, {}
local projected = {}
local last_frame = -1

local function rotate_yz(p, a)
  local c,s = math.cos(a), math.sin(a)
  return { x=p.x, y=p.y*c-p.z*s, z=p.y*s+p.z*c, w=p.w }
end

local function rotate_xz(p, a)
  local c,s = math.cos(a), math.sin(a)
  return { x=p.x*c-p.z*s, y=p.y, z=p.x*s+p.z*c, w=p.w }
end

local function rotate_zw(p, a)
  local c,s = math.cos(a), math.sin(a)
  return { x=p.x, y=p.y, z=p.z*c-p.w*s, w=p.z*s+p.w*c }
end

local rotate_x = rotate_yz
local rotate_y = rotate_xz
local rotate_w = rotate_zw

local function depth_to_brightness(z)
  local near = CAMERA_DIST - CUBE_SIZE
  local far = CAMERA_DIST + CUBE_SIZE
  local t = (z - near) / (far - near)
  t = math.max(0, math.min(1, t))
  t = t * t
  if INVERT_BRIGHTNESS then t = 1 - t end
  return MAX_BRIGHTNESS - (MAX_BRIGHTNESS - MIN_BRIGHTNESS) * t
end

local function project(p)
  local w = p.w + CAMERA_DIST
  if w < 0.1 then w = 0.1 end
  local x3 = p.x / w
  local y3 = p.y / w
  local z3 = p.z / w
  local z = z3 + CAMERA_DIST
  if z < 0.1 then z = 0.1 end
  return {
    x = CENTER_X + x3/z*SCALE,
    y = CENTER_Y - y3/z*SCALE,
    brightness = depth_to_brightness(z)
  }
end

local function set_pixel(px, py, r, g, b)
  if px >= 0 and px < WIDTH and py >= 0 and py < HEIGHT then
    local idx = py * WIDTH + px
    if r > (fb_r[idx] or 0) then fb_r[idx] = r end
    if g > (fb_g[idx] or 0) then fb_g[idx] = g end
    if b > (fb_b[idx] or 0) then fb_b[idx] = b end
  end
end

local function draw_line(x0, y0, x1, y1, b0, b1, r, g, b)
  x0,y0 = math.floor(x0+0.5), math.floor(y0+0.5)
  x1,y1 = math.floor(x1+0.5), math.floor(y1+0.5)
  local dx,dy = math.abs(x1-x0), -math.abs(y1-y0)
  local sx,sy = x0<x1 and 1 or -1, y0<y1 and 1 or -1
  local err = dx + dy
  local brightness = (b0 + b1) / 2
  local edge_scale = (EDGE_BRIGHTNESS / 255) * brightness
  local glow_scale = (GLOW_BRIGHTNESS / 255) * brightness
  local glow_r, glow_g, glow_b = r * glow_scale, g * glow_scale, b * glow_scale
  r, g, b = r * edge_scale, g * edge_scale, b * edge_scale
  local full_glow = b0 > GLOW_THRESHOLD and b1 > GLOW_THRESHOLD
  while true do
    set_pixel(x0, y0, r, g, b)
    set_pixel(x0-1, y0, glow_r, glow_g, glow_b)
    set_pixel(x0, y0-1, glow_r, glow_g, glow_b)
    if full_glow then
      set_pixel(x0+1, y0, glow_r, glow_g, glow_b)
      set_pixel(x0, y0+1, glow_r, glow_g, glow_b)
    end
    if x0==x1 and y0==y1 then break end
    local e2 = 2*err
    if e2 >= dy then err=err+dy; x0=x0+sx end
    if e2 <= dx then err=err+dx; y0=y0+sy end
  end
end

function shader(x, y, t, frame, dt)
  if frame ~= last_frame then
    last_frame = frame
    for i = 0, WIDTH*HEIGHT-1 do
      fb_r[i] = 0
      fb_g[i] = 0
      fb_b[i] = 0
    end

    local ay = t * ROTATION_SPEED_Y
    local ax = t * ROTATION_SPEED_X
    local aw = t * ROTATION_SPEED_W
    if UNEVEN_TIME then aw = ROTATION_SPEED_W * (t - 0.5 * math.cos(t)) end

    for i, v in ipairs(vertices) do
      local r = rotate_x(rotate_w(rotate_y(v, ay), aw), ax)
      projected[i] = project(r)
    end
    for i, e in ipairs(edges) do
      local r, g, b
      if e[1] <= 8 and e[2] <= 8 then
        r, g, b = 60, 255, 60
      elseif e[1] >= 9 and e[2] >= 9 then
        r, g, b = 255, 60, 60
      else
        r, g, b = 60, 60, 255
      end
      local p1, p2 = projected[e[1]], projected[e[2]]
      draw_line(p1.x, p1.y, p2.x, p2.y, p1.brightness, p2.brightness, r, g, b)
    end
  end

  local idx = y * WIDTH + x
  local r, g, b = fb_r[idx] or 0, fb_g[idx] or 0, fb_b[idx] or 0
  if r > 0 or g > 0 or b > 0 then
    return r, g, b
  end
  return BG_R, BG_G, BG_B
end
)LUA"
    },
    {
        "Hypercube Mono",
        R"LUA(
local floor, cos, sin, abs = math.floor, math.cos, math.sin, math.abs
local WIDTH, HEIGHT = 32, 32
local CENTER_X, CENTER_Y = 16, 16
local SCALE, CAMERA_DIST = 28, 1.8
local S = 0.5
local GLOW = 0.1
local GLOW_THRESHOLD = 1.6

local vertices = {
    {x=S,y=S,z=S,w=-S},{x=-S,y=S,z=S,w=-S},{x=-S,y=-S,z=S,w=-S},{x=S,y=-S,z=S,w=-S},
    {x=S,y=S,z=-S,w=-S},{x=-S,y=S,z=-S,w=-S},{x=-S,y=-S,z=-S,w=-S},{x=S,y=-S,z=-S,w=-S},
    {x=S,y=S,z=S,w=S},{x=-S,y=S,z=S,w=S},{x=-S,y=-S,z=S,w=S},{x=S,y=-S,z=S,w=S},
    {x=S,y=S,z=-S,w=S},{x=-S,y=S,z=-S,w=S},{x=-S,y=-S,z=-S,w=S},{x=S,y=-S,z=-S,w=S},
}
local edges = {
    {1,2},{2,3},{3,4},{4,1},{5,6},{6,7},{7,8},{8,5},{1,5},{2,6},{3,7},{4,8},
    {9,10},{10,11},{11,12},{12,9},{13,14},{14,15},{15,16},{16,13},{9,13},{10,14},{11,15},{12,16},
    {1,9},{2,10},{3,11},{4,12},{5,13},{6,14},{7,15},{8,16}
}
local fb_r, fb_g, fb_b = {}, {}, {}
local projected = {}
local last_frame = -1
local result, scratch = {x=0,y=0,z=0,w=0}, {x=0,y=0,z=0,w=0}

local function rotate_xz(p,c,s,o) o.x=p.x*c-p.z*s; o.y=p.y; o.z=p.x*s+p.z*c; o.w=p.w end
local function rotate_zw(p,c,s,o) o.x=p.x; o.y=p.y; o.z=p.z*c-p.w*s; o.w=p.z*s+p.w*c end
local function rotate_yz(p,c,s,o) o.x=p.x; o.y=p.y*c-p.z*s; o.z=p.y*s+p.z*c; o.w=p.w end
local function project(p,o)
    local w=p.w+CAMERA_DIST; if w<0.1 then w=0.1 end
    local x3,y3,z3=p.x/w,p.y/w,p.z/w
    local z=z3+CAMERA_DIST; if z<0.1 then z=0.1 end
    o.x=CENTER_X+x3/z*SCALE; o.y=CENTER_Y-y3/z*SCALE; o.z=z
end

local function set_pixel(px,py,r,g,b)
    if px>=0 and px<WIDTH and py>=0 and py<HEIGHT then
        local idx=py*WIDTH+px
        if r>(fb_r[idx] or 0) then fb_r[idx]=r end
        if g>(fb_g[idx] or 0) then fb_g[idx]=g end
        if b>(fb_b[idx] or 0) then fb_b[idx]=b end
    end
end

local function draw_line(x0,y0,x1,y1,z0,z1,r,g,b)
    x0,y0=floor(x0+0.5),floor(y0+0.5); x1,y1=floor(x1+0.5),floor(y1+0.5)
    local dx,dy=abs(x1-x0),-abs(y1-y0)
    local sx,sy=x0<x1 and 1 or -1,y0<y1 and 1 or -1
    local err=dx+dy
    local gr,gg,gb=r*GLOW,g*GLOW,b*GLOW
    local full_glow = z0 < GLOW_THRESHOLD and z1 < GLOW_THRESHOLD
    while true do
        set_pixel(x0,y0,r,g,b)
        if GLOW > 0 then
            set_pixel(x0-1,y0,gr,gg,gb); set_pixel(x0,y0-1,gr,gg,gb)
            if full_glow then
                set_pixel(x0+1,y0,gr,gg,gb); set_pixel(x0,y0+1,gr,gg,gb)
            end
        end
        if x0==x1 and y0==y1 then break end
        local e2=2*err
        if e2>=dy then err=err+dy; x0=x0+sx end
        if e2<=dx then err=err+dx; y0=y0+sy end
    end
end

for i=1,#vertices do projected[i]={x=0,y=0,z=0} end

function shader(x, y, t, frame, dt)
    if frame ~= last_frame then
        last_frame = frame
        fb_r, fb_g, fb_b = {}, {}, {}

        local mode = rotation_mode or 0
        local a1 = t * 0.5
        local a2 = t * 0.5 + sin(t) * 0.5
        local c1, s1 = cos(a1), sin(a1)
        local c2, s2 = cos(a2), sin(a2)

        for i, v in ipairs(vertices) do
            if mode == 0 then
                rotate_xz(v, c1, s1, result)
                rotate_zw(result, c2, s2, scratch)
                rotate_yz(scratch, c1, s1, result)
            elseif mode == 1 then
                rotate_xz(v, c1, s1, result)
                rotate_yz(result, c2, s2, scratch)
                result.x, result.y, result.z, result.w = scratch.x, scratch.y, scratch.z, scratch.w
            elseif mode == 2 then
                rotate_zw(v, c1, s1, result)
            else
                rotate_xz(v, c1, s1, result)
                rotate_zw(result, c2, s2, scratch)
                rotate_yz(scratch, c1, s1, result)
            end
            project(result, projected[i])
        end

        -- All edges same color (cyan/white)
        for _, e in ipairs(edges) do
            local p1, p2 = projected[e[1]], projected[e[2]]
            draw_line(p1.x, p1.y, p2.x, p2.y, p1.z, p2.z, 100, 200, 255)
        end

        -- All vertices same color
        for i, p in ipairs(projected) do
            set_pixel(floor(p.x + 0.5), floor(p.y + 0.5), 200, 255, 255)
        end
    end

    local idx = y * WIDTH + x
    return fb_r[idx] or 4, fb_g[idx] or 4, fb_b[idx] or 8
end
)LUA"
    },
    {
        "Rez",
        R"LUA(
local floor, cos, sin, abs = math.floor, math.cos, math.sin, math.abs
local W, H = 32, 32
local GLOW = 0.12

-- Rez-style humanoid: quad slices with 3D rotation
-- 2D vertices in grid space (0-31), converted to model space
-- x -> model X (centered at 16), y -> model Y-up (flipped, centered at 16)
-- Each quad gets Z depth for 3D
local v2d = {
  14,1, 18,1, 14,3, 18,3,       -- 1-4: head top      (TL TR BL BR)
  13,3, 19,3, 13,5, 19,5,       -- 5-8: head bottom   (TL TR BL BR)
  11,6, 21,6, 11,8, 21,8,       -- 9-12: shoulders    (TL TR BL BR)
  12,9, 20,9, 12,11, 20,11,     -- 13-16: chest       (TL TR BL BR)
  13,12, 19,12, 13,14, 19,14,   -- 17-20: waist       (TL TR BL BR)
  14,15, 18,15, 14,17, 18,17,   -- 21-24: hips        (TL TR BL BR)
  8,7, 11,7, 7,9, 10,9,         -- 25-28: left arm upper
  6,10, 9,10, 5,12, 8,12,       -- 29-32: left arm lower
  21,7, 24,7, 22,9, 25,9,       -- 33-36: right arm upper
  23,10, 26,10, 24,12, 27,12,   -- 37-40: right arm lower
  12,18, 15,18, 11,20, 14,20,   -- 41-44: left thigh top
  10,21, 13,21, 9,24, 12,24,    -- 45-48: left thigh mid
  9,25, 11,25, 8,28, 10,28,     -- 49-52: left shin
  8,29, 10,29, 7,31, 9,31,      -- 53-56: left foot
  17,18, 20,18, 18,20, 21,20,   -- 57-60: right thigh top
  19,21, 22,21, 20,24, 23,24,   -- 61-64: right thigh mid
  21,25, 23,25, 22,28, 24,28,   -- 65-68: right shin
  22,29, 24,29, 23,31, 25,31    -- 69-72: right foot
}
local NV = 72

-- Build 3D vertices: x centered, y=up (flipped), z=depth per quad
local verts = {}  -- {x, y, z} for each vertex
local DEPTH = 2.5  -- quad depth in grid units

for i = 1, NV do
  local gx = v2d[(i-1)*2+1]
  local gy = v2d[(i-1)*2+2]
  -- Center and scale to roughly -1..1
  local x = (gx - 16) / 16
  local y = (16 - gy) / 16  -- flip Y to up
  verts[i] = {x, y, 0}
end

-- Each quad is 4 consecutive verts; give front pair z=+d, back pair z=-d
local NQ = NV / 4
local d = DEPTH / 16
for q = 0, NQ - 1 do
  local base = q * 4
  -- verts 1,2 = front face, 3,4 = back face (same quad, just depth)
  verts[base+1][3] = d
  verts[base+2][3] = d
  verts[base+3][3] = -d
  verts[base+4][3] = -d
end

-- Edges: 4 edges per quad (outline rectangle)
-- Vertex layout per quad: 1=TL, 2=TR, 3=BL, 4=BR
-- Connect as: TL-TR, TR-BR, BR-BL, BL-TL
local edges = {}
for q = 0, NQ - 1 do
  local b = q * 4
  edges[#edges+1] = {b+1, b+2}  -- top: TL -> TR
  edges[#edges+1] = {b+2, b+4}  -- right: TR -> BR
  edges[#edges+1] = {b+4, b+3}  -- bottom: BR -> BL
  edges[#edges+1] = {b+3, b+1}  -- left: BL -> TL
end

local fb_r, fb_g, fb_b = {}, {}, {}
local proj = {}
local last_frame = -1
for i = 1, NV do proj[i] = {0, 0, 0} end

local function set_px(px, py, r, g, b)
  if px >= 0 and px < W and py >= 0 and py < H then
    local idx = py * W + px
    if r > (fb_r[idx] or 0) then fb_r[idx] = r end
    if g > (fb_g[idx] or 0) then fb_g[idx] = g end
    if b > (fb_b[idx] or 0) then fb_b[idx] = b end
  end
end

-- Line with brightness fading toward center (brighter at endpoints)
local function line(x0, y0, x1, y1, r, g, b, rm, gm, bm)
  x0, y0 = floor(x0+0.5), floor(y0+0.5)
  x1, y1 = floor(x1+0.5), floor(y1+0.5)
  local dx, dy = abs(x1-x0), -abs(y1-y0)
  local sx, sy = x0<x1 and 1 or -1, y0<y1 and 1 or -1
  local err = dx + dy
  local total = dx - dy  -- total steps (manhattan-ish)
  if total < 1 then total = 1 end
  local step = 0
  while true do
    -- t goes 0..1..0 (bright at ends, dim at center)
    local t2 = step / total
    local fade = 1.0 - (1.0 - abs(2*t2 - 1))  -- 1 at ends, 0 at center
    fade = fade * fade  -- sharpen the falloff
    local cr = rm + (r - rm) * fade
    local cg = gm + (g - gm) * fade
    local cb = bm + (b - bm) * fade
    set_px(x0, y0, cr, cg, cb)
    if x0==x1 and y0==y1 then break end
    local e2 = 2*err
    if e2 >= dy then err=err+dy; x0=x0+sx end
    if e2 <= dx then err=err+dx; y0=y0+sy end
    step = step + 1
  end
end

-- Smooth continuous camera: orbit + breathe + drift
-- All sine-based at different frequencies so it never looks repetitive

function shader(x, y, t, frame, dt)
  if frame ~= last_frame then
    last_frame = frame
    fb_r, fb_g, fb_b = {}, {}, {}

    -- Variable-speed orbit: slow at front/back, fast through side views
    -- Use a base angle that advances linearly, then warp it
    local base = t * 0.3
    -- Warp: subtract a sine term so we linger at 0/pi and rush through pi/2
    local rot_y = base - sin(2 * base) * 0.35

    -- Breathe in and out: distance oscillates 0.7..1.1
    local cam_dist = 0.9 + sin(t * 0.15) * 0.2

    -- Drift up and down the body slightly
    local cam_y = 0.1 + sin(t * 0.11) * 0.25

    -- Gentle tilt wobble
    local cam_tilt = sin(t * 0.08) * 0.15

    -- Scale tied to distance: closer = bigger projection
    local sc = 20 + (1.1 - cam_dist) * 14

    local ca, sa = cos(rot_y), sin(rot_y)
    local cb, sb = cos(cam_tilt), sin(cam_tilt)

    -- Transform all vertices
    for i = 1, NV do
      local vx, vy, vz = verts[i][1], verts[i][2], verts[i][3]
      -- Rotate around Y axis
      local rx = vx * ca + vz * sa
      local rz = -vx * sa + vz * ca
      -- X tilt (camera pitch)
      local ry = vy * cb - rz * sb
      local rz2 = vy * sb + rz * cb
      -- Apply camera offset
      ry = ry - cam_y
      -- Project
      local z = rz2 + cam_dist
      if z < 0.1 then z = 0.1 end
      proj[i][1] = 16 + rx / z * sc
      proj[i][2] = 16 - ry / z * sc
      proj[i][3] = z
    end

    -- Draw only front two edges of each quad (the ones closer to camera)
    -- Each quad has 4 verts: TL(1) TR(2) BL(3) BR(4)
    -- Front pair = verts with smaller z (closer), back pair = larger z
    for q = 0, NQ - 1 do
      local b = q * 4
      local tl, tr, bl, br = proj[b+1], proj[b+2], proj[b+3], proj[b+4]
      local fz = (tl[3] + tr[3]) * 0.5  -- front face avg z
      local bz = (bl[3] + br[3]) * 0.5  -- back face avg z

      -- Depth-based brightness (min ~80/255, never disappears)
      local az = (fz + bz) * 0.5
      local bright = 1.3 - (az - cam_dist + 1) * 0.4
      if bright < 0.3 then bright = 0.3 end
      if bright > 1.0 then bright = 1.0 end

      -- Subtle interior fill via scanline
      local fill = floor(25 * bright)
      local y_top = tl[2]
      if tr[2] < y_top then y_top = tr[2] end
      local y_bot = bl[2]
      if br[2] > y_bot then y_bot = br[2] end
      local ymin = floor(y_top + 0.5)
      local ymax = floor(y_bot + 0.5)
      if ymin < 0 then ymin = 0 end
      if ymax >= H then ymax = H - 1 end
      local dy_l = bl[2] - tl[2]
      local dy_r = br[2] - tr[2]
      for py = ymin, ymax do
        local fl = 0.5
        if dy_l > 0.5 or dy_l < -0.5 then fl = (py - tl[2]) / dy_l end
        if fl < 0 then fl = 0 elseif fl > 1 then fl = 1 end
        local xl = tl[1] + (bl[1] - tl[1]) * fl
        local fr = 0.5
        if dy_r > 0.5 or dy_r < -0.5 then fr = (py - tr[2]) / dy_r end
        if fr < 0 then fr = 0 elseif fr > 1 then fr = 1 end
        local xr = tr[1] + (br[1] - tr[1]) * fr
        local x0 = floor(xl + 0.5)
        local x1 = floor(xr + 0.5)
        if x0 > x1 then x0, x1 = x1, x0 end
        if x0 < 0 then x0 = 0 end
        if x1 >= W then x1 = W - 1 end
        for px = x0, x1 do
          set_px(px, py, fill, fill, fill)
        end
      end

      -- White edges, bright at corners, dim at midpoints
      local hi = floor(255 * bright)
      local lo = floor(80 * bright)

      line(tl[1], tl[2], tr[1], tr[2], hi, hi, hi, lo, lo, lo)
      line(tr[1], tr[2], br[1], br[2], hi, hi, hi, lo, lo, lo)
      line(br[1], br[2], bl[1], bl[2], hi, hi, hi, lo, lo, lo)
      line(bl[1], bl[2], tl[1], tl[2], hi, hi, hi, lo, lo, lo)
    end
  end

  local idx = y * W + x
  return fb_r[idx] or 0, fb_g[idx] or 0, fb_b[idx] or 0
end
)LUA"
    },
};

static const int BUILTIN_SHADER_COUNT = sizeof(BUILTIN_SHADERS) / sizeof(BUILTIN_SHADERS[0]);
