-- PSXjin 2.0.2, USA v1.0. Read-only telemetry from the original PJM.
-- Run in an isolated emulator directory with the user's discs/BIOS/cards.
-- Stop before the first disc-lid command: opening its modal dialog suspends Lua.
local prefix = os.getenv("MGS_TAS_TRACE_PREFIX") or "reference"
local stop = tonumber(os.getenv("MGS_TAS_TRACE_STOP")) or 191146
local slot = tonumber(os.getenv("MGS_TAS_TRACE_SLOT"))
-- The CLI starts Lua before the queued movie playback. Wait for playback
-- before resolving its slot basename or testing the main loop condition.
local startup = 0
while not movie.playing() do
    assert(not movie.recording() and startup < 1200, "PJM playback did not start")
    startup = startup + 1
    emu.frameadvance()
end
if slot then
    assert(slot >= 1 and slot <= 10 and slot == math.floor(slot), "invalid reference slot")
    savestate.load(savestate.create(slot))
end
local existing = io.open(prefix .. ".tsv", "r")
if existing then existing:close(); error("reference output already exists") end
assert(movie.framecount() <= stop, "reference checkpoint is after trace stop")
local out = assert(io.open(prefix .. ".tsv", "w"))
out:write("frame\tstage\tstory\tdifficulty\tx\ty\tz\tyaw\tpad_origin\tlife\tstance\tpause\tgame_status\tplayer_status\talert\tweapon\titem\tgame_tick\tpad0\tpad1\n")
local lastFrame = -1
local lastStage = ""
emu.speedmode("maximum")
while movie.playing() do
    local frame = movie.framecount()
    local stage = ""
    for i = 0, 7 do
        local c = memory.readbyte(0xb7500 + i)
        if c == 0 then break end
        if c >= 32 and c < 127 then stage = stage .. string.char(c) end
    end
    local control = memory.readdword(0xae16c) % 0x20000000
    local yaw = -1
    if control > 0 and control < 0x1ffff0 then yaw = memory.readword(control + 10) end
    if frame ~= lastFrame then
        -- GV_PAD uses PsyQ's byte order; PJM/native masks use wire order.
        -- USA Snake's pad pointer is work+0x9ac (Integral uses +0x9b0).
        local function pad(address)
            local status = memory.readword(address)
            return (status % 256) * 256 + math.floor(status / 256)
        end
        out:write(table.concat({frame, stage,
            memory.readwordsigned(0xb644a), memory.readwordsigned(0xb7512),
            memory.readwordsigned(0xae188), memory.readwordsigned(0xae18a),
            memory.readwordsigned(0xae18c), yaw, memory.readdword(0xadaa8),
            memory.readwordsigned(0xb7526), memory.readwordsigned(0xb7530),
            memory.readdword(0xae0a0), memory.readdword(0xadb3c),
            memory.readdword(0xae1c8), memory.readdword(0xae178),
            memory.readwordsigned(0xb752c), memory.readwordsigned(0xb752e),
            memory.readdword(0xada48), pad(0xb2d38), pad(0xb2d48)}, "\t"), "\n")
        if frame % 300 == 0 or stage ~= lastStage then out:flush() end
        lastFrame = frame
        lastStage = stage
    end
    if frame >= stop then break end
    emu.frameadvance()
end
out:close()
emu.exitemulator()
