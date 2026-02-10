-- Localized math functions for performance
local floor = math.floor
local cos = math.cos
local sin = math.sin
local abs = math.abs

-- CONSTANTS - tweak these!
local WIDTH, HEIGHT = 32, 32
local CENTER_X, CENTER_Y = 16, 16
local SCALE = 28
local CAMERA_DIST = 1.8
local ROTATION_SPEED_Y = 0.5
local ROTATION_SPEED_X = 0.5
local ROTATION_SPEED_W = 1
local UNEVEN_TIME = true
local DRAW_VERTICES = true
local DRAW_EDGES = true
local CUBE_SIZE = 0.5
local EDGE_BRIGHTNESS = 255
local GLOW_BRIGHTNESS = 60
local BG_R, BG_G, BG_B = 8, 8, 16

-- Geometry
local S = CUBE_SIZE

-- CUBE VERTEX DIAGRAM (looking at the cube from the front-right)
--
--       2 -------- 1          Y+ (up)
--      /|         /|          |
--     / |        / |          |
--    6 -------- 5  |          +---- X+ (right)
--    |  3 ------|- 4         /
--    | /        | /         Z+ (toward you)
--    |/         |/
--    7 -------- 8
--
-- Front face (Z+): 1, 2, 3, 4 (clockwise from top-right)
-- Back face  (Z-): 5, 6, 7, 8 (clockwise from top-right)
--
local vertices = {
    -- Cube 1
    -- Front face (Z = -S)
    { x = S,  y = S,  z = S,  w = -S }, -- [1] front top-right
    { x = -S, y = S,  z = S,  w = -S }, -- [2] front top-left
    { x = -S, y = -S, z = S,  w = -S }, -- [3] front bottom-left
    { x = S,  y = -S, z = S,  w = -S }, -- [4] front bottom-right
    { x = S,  y = S,  z = -S, w = -S }, -- [5] back top-right
    { x = -S, y = S,  z = -S, w = -S }, -- [6] back top-left
    { x = -S, y = -S, z = -S, w = -S }, -- [7] back bottom-left
    { x = S,  y = -S, z = -S, w = -S }, -- [8] back bottom-right
    --=================================================
    -- Cube 2
    -- Front face (Z = +S)
    { x = S,  y = S,  z = S,  w = S }, -- [1] front top-right
    { x = -S, y = S,  z = S,  w = S }, -- [2] front top-left
    { x = -S, y = -S, z = S,  w = S }, -- [3] front bottom-left
    { x = S,  y = -S, z = S,  w = S }, -- [4] front bottom-right
    -- Back face (Z = -S)
    { x = S,  y = S,  z = -S, w = S }, -- [5] back top-right
    { x = -S, y = S,  z = -S, w = S }, -- [6] back top-left
    { x = -S, y = -S, z = -S, w = S }, -- [7] back bottom-left
    { x = S,  y = -S, z = -S, w = S }, -- [8] back bottom-right

}

-- EDGE DIAGRAM
--
-- Front face edges:        Back face edges:         Connecting edges:
--     [1]                      [5]                   (front -> back)
--   2-----1                  6-----5
--   |     |                  |     |                 1--5  [9]
--[2]|     |[4]            [6]|     |[8]              2--6  [10]
--   |     |                  |     |                 3--7  [11]
--   3-----4                  7-----8                 4--8  [12]
--     [3]                      [7]
--
local edges = {
    -- Cube 1
    -- Front face (Z+) edges
    { 1,  2 }, -- [1] front top
    { 2,  3 }, -- [2] front left
    { 3,  4 }, -- [3] front bottom
    { 4,  1 }, -- [4] front right
    -- Back face (Z-) edges
    { 5,  6 }, -- [5] back top
    { 6,  7 }, -- [6] back left
    { 7,  8 }, -- [7] back bottom
    { 8,  5 }, -- [8] back right
    -- Connecting edges (front to back)
    { 1,  5 }, -- [9]  top-right connecting
    { 2,  6 }, -- [10] top-left connecting
    { 3,  7 }, -- [11] bottom-left connecting
    { 4,  8 }, -- [12] bottom-right connecting
    --======================================
    -- Cube 2
    -- Front face (Z+) edges
    { 9,  10 }, -- [1] front top
    { 10, 11 }, -- [2] front left
    { 11, 12 }, -- [3] front bottom
    { 12, 9 },  -- [4] front right
    -- Back face (Z-) edges
    { 13, 14 }, -- [5] back top
    { 14, 15 }, -- [6] back left
    { 15, 16 }, -- [7] back bottom
    { 16, 13 }, -- [8] back right
    -- Connecting edges (front to back)
    { 9,  13 }, -- [9]  top-right connecting
    { 10, 14 }, -- [10] top-left connecting
    { 11, 15 }, -- [11] bottom-left connecting
    { 12, 16 }, -- [12] bottom-right connecting
    --======================================
    -- Hypercube edges
    { 1,  9 },
    { 2,  10 },
    { 3,  11 },
    { 4,  12 },
    { 5,  13 },
    { 6,  14 },
    { 7,  15 },
    { 8,  16 }
}
-- State
local fb_r, fb_g, fb_b = {}, {}, {}
local projected = {}
local last_frame = -1

-- Reusable temp tables for rotation (avoid allocations)
local temp1 = { x = 0, y = 0, z = 0, w = 0 }
local temp2 = { x = 0, y = 0, z = 0, w = 0 }

-- Rotation functions (take pre-computed cos/sin, write to output table)
-- XY plane (roll in 3D)
local function rotate_xy(p, c, s, out)
    out.x = p.x * c - p.y * s
    out.y = p.x * s + p.y * c
    out.z = p.z
    out.w = p.w
end

-- XZ plane (yaw in 3D) - rotate_y
local function rotate_xz(p, c, s, out)
    out.x = p.x * c - p.z * s
    out.y = p.y
    out.z = p.x * s + p.z * c
    out.w = p.w
end

-- YZ plane (pitch in 3D) - rotate_x
local function rotate_yz(p, c, s, out)
    out.x = p.x
    out.y = p.y * c - p.z * s
    out.z = p.y * s + p.z * c
    out.w = p.w
end

-- XW plane (4D only)
local function rotate_xw(p, c, s, out)
    out.x = p.x * c - p.w * s
    out.y = p.y
    out.z = p.z
    out.w = p.x * s + p.w * c
end

-- YW plane (4D only)
local function rotate_yw(p, c, s, out)
    out.x = p.x
    out.y = p.y * c - p.w * s
    out.z = p.z
    out.w = p.y * s + p.w * c
end

-- ZW plane (4D only) - rotate_w
local function rotate_zw(p, c, s, out)
    out.x = p.x
    out.y = p.y
    out.z = p.z * c - p.w * s
    out.w = p.z * s + p.w * c
end

local rotate_x = rotate_yz
local rotate_y = rotate_xz
local rotate_w = rotate_zw

-- Perspective projection (reuses projected[i] table)
local function project(p, out)
    local z = p.z + CAMERA_DIST
    if z < 0.1 then z = 0.1 end
    out.x = CENTER_X + p.x / z * SCALE
    out.y = CENTER_Y - p.y / z * SCALE
end

-- Pixel setting with max brightness (per channel)
local function set_pixel(px, py, r, g, b)
    if px >= 0 and px < WIDTH and py >= 0 and py < HEIGHT then
        local idx = py * WIDTH + px
        if r > (fb_r[idx] or 0) then fb_r[idx] = r end
        if g > (fb_g[idx] or 0) then fb_g[idx] = g end
        if b > (fb_b[idx] or 0) then fb_b[idx] = b end
    end
end

-- Bresenham line with glow and color
local function draw_line(x0, y0, x1, y1, r, g, b)
    x0, y0 = floor(x0 + 0.5), floor(y0 + 0.5)
    x1, y1 = floor(x1 + 0.5), floor(y1 + 0.5)
    local dx, dy = abs(x1 - x0), -abs(y1 - y0)
    local sx, sy = x0 < x1 and 1 or -1, y0 < y1 and 1 or -1
    local err = dx + dy
    local glow_r, glow_g, glow_b = r * 0.25, g * 0.25, b * 0.25
    while true do
        set_pixel(x0, y0, r, g, b)
        set_pixel(x0 - 1, y0, glow_r, glow_g, glow_b)
        set_pixel(x0 + 1, y0, glow_r, glow_g, glow_b)
        set_pixel(x0, y0 - 1, glow_r, glow_g, glow_b)
        set_pixel(x0, y0 + 1, glow_r, glow_g, glow_b)
        if x0 == x1 and y0 == y1 then break end
        local e2 = 2 * err
        if e2 >= dy then
            err = err + dy; x0 = x0 + sx
        end
        if e2 <= dx then
            err = err + dx; y0 = y0 + sy
        end
    end
end

-- Pre-allocate projected tables
for i = 1, #vertices do
    projected[i] = { x = 0, y = 0 }
end

function shader(x, y, t, frame, dt)
    if frame ~= last_frame then
        last_frame = frame
        -- Clear framebuffers by replacing with new tables
        fb_r, fb_g, fb_b = {}, {}, {}

        local ay = t * ROTATION_SPEED_Y
        local ax = t * ROTATION_SPEED_X

        -- time dimension
        local aw = t * ROTATION_SPEED_W
        local aw_oscillating = t * ROTATION_SPEED_W + sin(t) * CUBE_SIZE
        if UNEVEN_TIME then aw = aw_oscillating end

        -- Pre-compute trig values once per frame
        local cos_ay, sin_ay = cos(ay), sin(ay)
        local cos_ax, sin_ax = cos(ax), sin(ax)
        local cos_aw, sin_aw = cos(aw), sin(aw)

        for i, v in ipairs(vertices) do
            -- Rotation order 3: X-W-Y (rotate_y first, then rotate_w, then rotate_x)
            --rotate_w(rotate_x(rotate_y(v, ay), ax), aw)  -- 1: W-X-Y
            --rotate_w(rotate_y(rotate_x(v, ax), ay), aw)  -- 2: W-Y-X
            rotate_y(v, cos_ay, sin_ay, temp1) -- 3: X-W-Y
            rotate_w(temp1, cos_aw, sin_aw, temp2)
            rotate_x(temp2, cos_ax, sin_ax, temp1)
            --rotate_x(rotate_y(rotate_w(v, aw), ay), ax)  -- 4: X-Y-W
            --rotate_y(rotate_w(rotate_x(v, ax), aw), ay)  -- 5: Y-W-X
            --rotate_y(rotate_x(rotate_w(v, aw), ax), ay)  -- 6: Y-X-W
            project(temp1, projected[i])
        end
        if DRAW_EDGES then
            for i, e in ipairs(edges) do
                local r, g, b
                if e[1] <= 8 and e[2] <= 8 then
                    -- Cube 1: green
                    r, g, b = 60, 255, 60
                elseif e[1] >= 9 and e[2] >= 9 then
                    -- Cube 2: red
                    r, g, b = 255, 60, 60
                else
                    -- Connecting edges: blue
                    r, g, b = 60, 60, 255
                end
                draw_line(projected[e[1]].x, projected[e[1]].y,
                    projected[e[2]].x, projected[e[2]].y, r, g, b)
            end
        end
        if DRAW_VERTICES then
            for i, p in ipairs(projected) do
                local r, g, b
                if i <= 8 then
                    r, g, b = 100, 255, 100 -- Cube 1: bright green
                else
                    r, g, b = 255, 100, 100 -- Cube 2: bright red
                end
                local px, py = floor(p.x + 0.5), floor(p.y + 0.5)
                set_pixel(px, py, r, g, b)
            end
        end
    end

    local idx = y * WIDTH + x
    return fb_r[idx] or BG_R, fb_g[idx] or BG_G, fb_b[idx] or BG_B
end

-- Congratulations! You understand 3D graphics!
-- Try: ROTATION_SPEED_Y = 0 to see only X rotation
-- Try: CAMERA_DIST = 1.2 for extreme perspective
