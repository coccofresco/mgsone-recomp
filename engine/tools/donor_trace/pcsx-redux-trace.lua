-- PCSX-Redux producer for the ps1trace/v1 comparison schema.
-- Requires pcsx-redux-state-api.patch. Load while paused, then hard-reset:
--   loadfile("/absolute/path/pcsx-redux-trace.lua")()

local OUT = os.getenv("PS1_DONOR_TRACE") or "/tmp/crash3-redux.jsonl"
local CPU_HZ = 33868800
local frame = 0
local seq = 0
local out = assert(io.open(OUT, "w"))

if _ps1trace_callbacks then
    PCSX.Trace.setGPUControlCallback(nil)
    PCSX.Trace.setGPUDrawCallback(nil)
    PCSX.Trace.setCDCallback(nil)
    PCSX.Trace.setSPUCallback(nil)
    for _, callback in pairs(_ps1trace_callbacks) do callback:free() end
end
_ps1trace_callbacks = {}

local function signed11(value)
    value = bit.band(value, 0x7ff)
    if value >= 0x400 then return value - 0x800 end
    return value
end

local function emit(event, fields, cycles)
    seq = seq + 1
    local host_us = math.floor(tonumber(cycles) * 1000000 / CPU_HZ)
    local parts = {
        '{"schema":"ps1trace/v1","source":"pcsx-redux","seq":',
        tostring(seq), ',"frame":', tostring(frame), ',"host_us":',
        tostring(host_us), ',"event":"', event, '"'
    }
    for key, value in pairs(fields or {}) do
        parts[#parts + 1] = ',"' .. key .. '":' .. tostring(value)
    end
    parts[#parts + 1] = '}\n'
    out:write(table.concat(parts))
    out:flush()
end

_ps1trace_callbacks.gpuControl = ffi.cast(
    "void (*)(uint32_t, uint64_t)", function(value, cycles)
        value = tonumber(value)
        local command = bit.band(bit.rshift(value, 24), 0xff)
        local word = bit.band(value, 0xffffff)
        if command == 3 then
            emit("gpu.display_enable", {enabled=bit.band(word, 1) == 0 and 1 or 0}, cycles)
        elseif command == 5 then
            emit("gpu.display_start", {
                x=bit.band(word, 0x3ff),
                y=bit.band(bit.rshift(word, 10), 0x1ff),
                word=word
            }, cycles)
        elseif command == 6 then
            emit("gpu.display_range_h", {
                x0=bit.band(word, 0xfff),
                x1=bit.band(bit.rshift(word, 12), 0xfff),
                word=word
            }, cycles)
        elseif command == 7 then
            emit("gpu.display_range_v", {
                y0=bit.band(word, 0x3ff),
                y1=bit.band(bit.rshift(word, 10), 0x3ff),
                word=word
            }, cycles)
        elseif command == 8 then
            emit("gpu.display_mode", {mode=bit.band(word, 0xff), word=word}, cycles)
        end
    end)

_ps1trace_callbacks.gpuDraw = ffi.cast(
    "void (*)(uint32_t, uint32_t, uint64_t)", function(command, value, cycles)
        command = tonumber(command)
        value = tonumber(value)
        if command == 3 then
            emit("gpu.draw_area_start", {
                x=bit.band(value, 0x3ff), y=bit.band(bit.rshift(value, 10), 0x3ff), word=value
            }, cycles)
        elseif command == 4 then
            emit("gpu.draw_area_end", {
                x=bit.band(value, 0x3ff), y=bit.band(bit.rshift(value, 10), 0x3ff), word=value
            }, cycles)
        elseif command == 5 then
            emit("gpu.draw_offset", {
                x=signed11(value), y=signed11(bit.rshift(value, 11)), word=value
            }, cycles)
        end
    end)

_ps1trace_callbacks.cd = ffi.cast(
    "void (*)(uint32_t, uint32_t, uint32_t, uint64_t)",
    function(command, parameterCount, lba, cycles)
        emit("cdrom.command", {
            command=tonumber(command), parameters=tonumber(parameterCount), lba=tonumber(lba)
        }, cycles)
    end)

_ps1trace_callbacks.spu = ffi.cast(
    "void (*)(bool, uint32_t, uint64_t)", function(keyOn, voices, cycles)
        voices = tonumber(voices)
        if voices ~= 0 then
            emit(keyOn and "spu.key_on" or "spu.key_off", {voices=voices}, cycles)
        end
    end)

PCSX.Trace.setGPUControlCallback(_ps1trace_callbacks.gpuControl)
PCSX.Trace.setGPUDrawCallback(_ps1trace_callbacks.gpuDraw)
PCSX.Trace.setCDCallback(_ps1trace_callbacks.cd)
PCSX.Trace.setSPUCallback(_ps1trace_callbacks.spu)

if _ps1trace_listener then _ps1trace_listener:remove() end
_ps1trace_listener = PCSX.Events.createEventListener("GPU::Vsync", function()
    frame = frame + 1
    local cycles = PCSX.getCPUCycles()
    local s = PCSX.GPU.getTraceState()
    local ds = tonumber(s.displayStart)
    local da0 = tonumber(s.drawingStart)
    local da1 = tonumber(s.drawingEnd)
    local off = tonumber(s.drawingOffset)
    local mode = bit.band(tonumber(s.displayMode), 0xff)
    local x = bit.band(ds, 0x3ff)
    local y = bit.band(bit.rshift(ds, 10), 0x1ff)
    local w
    if bit.band(mode, 0x40) ~= 0 then
        w = 368
    else
        local hres = bit.band(mode, 3)
        w = ({[0] = 256, [1] = 320, [2] = 512, [3] = 640})[hres]
    end
    local h = bit.band(mode, 4) ~= 0 and 480 or 240
    local draw_x0 = bit.band(da0, 0x3ff)
    local draw_y0 = bit.band(bit.rshift(da0, 10), 0x1ff)
    local draw_x1 = bit.band(da1, 0x3ff)
    local draw_y1 = bit.band(bit.rshift(da1, 10), 0x1ff)
    local overlap = x <= draw_x1 and x + w - 1 >= draw_x0 and
                    y <= draw_y1 and y + h - 1 >= draw_y0

    emit("gpu.vblank_latch", {
        display_x=x, display_y=y, display_w=w, display_h=h,
        draw_x0=draw_x0, draw_y0=draw_y0,
        draw_x1=draw_x1, draw_y1=draw_y1,
        draw_offset_x=signed11(off),
        draw_offset_y=signed11(bit.rshift(off, 11)),
        display_draw_overlap=overlap and 1 or 0
    }, cycles)
    emit("timing.vblank", {index=frame}, cycles)
end)

PCSX.log("[ps1trace] writing " .. OUT)
print("[ps1trace] PCSX-Redux capture active: " .. OUT)
