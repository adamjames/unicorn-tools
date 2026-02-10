-- Benchmark harness for hyprcube.lua
-- Run with: lua hyprcube_bench.lua

-- Load the shader
dofile("hyprcube.lua")

local WIDTH, HEIGHT = 32, 32
local NUM_FRAMES = 1000
local WARMUP_FRAMES = 100

-- Simulate the render loop
local function render_frame(frame, t, dt)
    for y = 0, HEIGHT - 1 do
        for x = 0, WIDTH - 1 do
            shader(x, y, t, frame, dt)
        end
    end
end

-- Warmup (let JIT compile, if using LuaJIT)
print("Warming up...")
for frame = 1, WARMUP_FRAMES do
    local t = frame / 60
    render_frame(frame, t, 1/60)
end

-- Reset frame counter for actual test
last_frame = -1

-- Benchmark
print("Running " .. NUM_FRAMES .. " frames...")
local start_time = os.clock()

for frame = 1, NUM_FRAMES do
    local t = frame / 60
    render_frame(frame, t, 1/60)
end

local end_time = os.clock()
local elapsed = end_time - start_time
local fps = NUM_FRAMES / elapsed

print(string.format("Elapsed: %.3f seconds", elapsed))
print(string.format("FPS: %.1f", fps))
print(string.format("Frame time: %.3f ms", (elapsed / NUM_FRAMES) * 1000))
