-- 3D CUBE TUTORIAL 8: Putting It All Together
--
-- This is the complete, polished version with:
-- - Rotation around TWO axes (Y and X)
-- - All constants at the top for easy tweaking
-- - Clean, organized code structure
--
-- You now understand every line of this code!

-- CONSTANTS - tweak these!
local WIDTH, HEIGHT = 32, 32
local CENTER_X, CENTER_Y = 16, 16
local SCALE = 28
local CAMERA_DIST = 1.8
local ROTATION_SPEED_Y = 0.5
local ROTATION_SPEED_X = 0.3
local ROTATION_SPEED_W = 1
local UNEVEN_TIME = false
local CUBE_SIZE = 0.7
local EDGE_BRIGHTNESS = 255
local GLOW_BRIGHTNESS = 60
local MIN_BRIGHTNESS = 0.2
local MAX_BRIGHTNESS = 1.0
local INVERT_BRIGHTNESS = false
local BG_R, BG_G, BG_B = 2, 2, 4

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
  {x= S, y= S, z= S, w=-S},  -- [1] front top-right
  {x=-S, y= S, z= S, w=-S},  -- [2] front top-left
  {x=-S, y=-S, z= S, w=-S},  -- [3] front bottom-left
  {x= S, y=-S, z= S, w=-S},  -- [4] front bottom-right
  {x= S, y= S, z=-S, w=-S},  -- [5] back top-right
  {x=-S, y= S, z=-S, w=-S},  -- [6] back top-left
  {x=-S, y=-S, z=-S, w=-S},  -- [7] back bottom-left
  {x= S, y=-S, z=-S, w=-S},  -- [8] back bottom-right
  --=================================================
  -- Cube 2
  -- Front face (Z = +S)
  {x= S, y= S, z= S, w=S},  -- [1] front top-right
  {x=-S, y= S, z= S, w=S},  -- [2] front top-left
  {x=-S, y=-S, z= S, w=S},  -- [3] front bottom-left
  {x= S, y=-S, z= S, w=S},  -- [4] front bottom-right
  -- Back face (Z = -S)
  {x= S, y= S, z=-S, w=S},  -- [5] back top-right
  {x=-S, y= S, z=-S, w=S},  -- [6] back top-left
  {x=-S, y=-S, z=-S, w=S},  -- [7] back bottom-left
  {x= S, y=-S, z=-S, w=S},  -- [8] back bottom-right

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
  {1,2},  -- [1] front top
  {2,3},  -- [2] front left
  {3,4},  -- [3] front bottom
  {4,1},  -- [4] front right
  -- Back face (Z-) edges
  {5,6},  -- [5] back top
  {6,7},  -- [6] back left
  {7,8},  -- [7] back bottom
  {8,5},  -- [8] back right
  -- Connecting edges (front to back)
  {1,5},  -- [9]  top-right connecting
  {2,6},  -- [10] top-left connecting
  {3,7},  -- [11] bottom-left connecting
  {4,8},  -- [12] bottom-right connecting
  --======================================
  -- Cube 2
  -- Front face (Z+) edges
  {9,10},  -- [1] front top
  {10,11},  -- [2] front left
  {11,12},  -- [3] front bottom
  {12,9},  -- [4] front right
  -- Back face (Z-) edges
  {13,14},  -- [5] back top
  {14,15},  -- [6] back left
  {15,16},  -- [7] back bottom
  {16,13},  -- [8] back right
  -- Connecting edges (front to back)
  {9,13},  -- [9]  top-right connecting
  {10,14},  -- [10] top-left connecting
  {11,15},  -- [11] bottom-left connecting
  {12,16},  -- [12] bottom-right connecting
  --======================================
  -- Hypercube edges
  {1,9},
  {2,10},
  {3,11},
  {4,12},
  {5,13},
  {6,14},
  {7,15},
  {8,16}
}
-- State
local fb_r, fb_g, fb_b = {}, {}, {}
local projected = {}
local last_frame = -1

-- Rotation functions
-- XY plane (roll in 3D)
local function rotate_xy(p, a)
  local c,s = math.cos(a), math.sin(a)
  return { x=p.x*c-p.y*s, y=p.x*s+p.y*c, z=p.z, w=p.w }
end

-- XZ plane (yaw in 3D) - your rotate_y
local function rotate_xz(p, a)
  local c,s = math.cos(a), math.sin(a)
  return { x=p.x*c-p.z*s, y=p.y, z=p.x*s+p.z*c, w=p.w }
end

-- YZ plane (pitch in 3D) - your rotate_x
local function rotate_yz(p, a)
  local c,s = math.cos(a), math.sin(a)
  return { x=p.x, y=p.y*c-p.z*s, z=p.y*s+p.z*c, w=p.w }
end

-- XW plane (4D only)
local function rotate_xw(p, a)
  local c,s = math.cos(a), math.sin(a)
  return { x=p.x*c-p.w*s, y=p.y, z=p.z, w=p.x*s+p.w*c }
end

-- YW plane (4D only)
local function rotate_yw(p, a)
  local c,s = math.cos(a), math.sin(a)
  return { x=p.x, y=p.y*c-p.w*s, z=p.z, w=p.y*s+p.w*c }
end

-- ZW plane (4D only) - your rotate_zw
local function rotate_zw(p, a)
  local c,s = math.cos(a), math.sin(a)
  return { x=p.x, y=p.y, z=p.z*c-p.w*s, w=p.z*s+p.w*c }
end

local rotate_x = rotate_yz
local rotate_y = rotate_xz
local rotate_w = rotate_zw

-- Depth to brightness: closer = brighter (maps z range to brightness range)
local function depth_to_brightness(z)
  local near = CAMERA_DIST - CUBE_SIZE
  local far = CAMERA_DIST + CUBE_SIZE
  local t = (z - near) / (far - near)  -- 0 at near, 1 at far
  t = math.max(0, math.min(1, t))
  t = t * t  -- power curve for more dramatic falloff
  if INVERT_BRIGHTNESS then t = 1 - t end
  return MAX_BRIGHTNESS - (MAX_BRIGHTNESS - MIN_BRIGHTNESS) * t
end

-- 4D -> 2D Perspective projection
local function project(p)
  -- First: 4D -> 3D perspective (scale x, y, z by w-distance)
  local w = p.w + CAMERA_DIST
  if w < 0.1 then w = 0.1 end
  local x3 = p.x / w
  local y3 = p.y / w
  local z3 = p.z / w

  -- Then: 3D -> 2D perspective
  local z = z3 + CAMERA_DIST
  if z < 0.1 then z = 0.1 end
  return {
    x = CENTER_X + x3/z*SCALE,
    y = CENTER_Y - y3/z*SCALE,
    brightness = depth_to_brightness(z)
  }
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
  while true do
    set_pixel(x0, y0, r, g, b)
    set_pixel(x0-1, y0, glow_r, glow_g, glow_b)
    set_pixel(x0+1, y0, glow_r, glow_g, glow_b)
    set_pixel(x0, y0-1, glow_r, glow_g, glow_b)
    set_pixel(x0, y0+1, glow_r, glow_g, glow_b)
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

    -- time dimension
    local aw = t * ROTATION_SPEED_W
    local aw_oscillating = ROTATION_SPEED_W * (t - 0.5 * math.cos(t))
    if UNEVEN_TIME then aw = aw_oscillating end

    for i, v in ipairs(vertices) do
      --local r = rotate_w(rotate_x(rotate_y(v, ay), ax), aw)  -- 1: W-X-Y
      --local r = rotate_w(rotate_y(rotate_x(v, ax), ay), aw)  -- 2: W-Y-X
      local r = rotate_x(rotate_w(rotate_y(v, ay), aw), ax)  -- 3: X-W-Y
      --local r = rotate_x(rotate_y(rotate_w(v, aw), ay), ax)  -- 4: X-Y-W
      --local r = rotate_y(rotate_w(rotate_x(v, ax), aw), ay)  -- 5: Y-W-X
      --local r = rotate_y(rotate_x(rotate_w(v, aw), ax), ay)  -- 6: Y-X-W
      projected[i] = project(r)
    end
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

-- Congratulations! You understand 3D graphics!
-- Try: ROTATION_SPEED_Y = 0 to see only X rotation
-- Try: CAMERA_DIST = 1.2 for extreme perspective
