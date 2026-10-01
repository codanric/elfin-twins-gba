-- mGBA multiplayer hardware-path probe.
-- Loaded into both mGBA windows. Reads the actual link-test ROM's gba_link_t
-- from IWRAM, injects A on the parent, and writes one report per player.

do
    local f = assert(io.open("/tmp/mgba-script-trace.txt", "a"))
    f:write("loaded\n")
    f:close()
    print("ELFIN_PROBE_LOADED")
end

local function envnum(name, base)
    local s = os.getenv(name)
    if not s then error("missing env "..name) end
    return tonumber(s, base or 10)
end

local link_base = envnum("LINK_BASE", 16)
local off_ready = envnum("OFF_READY")
local off_parent = envnum("OFF_PARENT")
local off_peer = envnum("OFF_PEER")
local off_frames_rx = envnum("OFF_FRAMES_RX")
local off_frames_tx = envnum("OFF_FRAMES_TX")
local off_errors = envnum("OFF_ERRORS")

local frame = 0
local pressed = false

callbacks:add("frame", function()
    frame = frame + 1
    if frame == 1 or frame == 60 or frame == 120 or frame == 240 or frame == 420 then
        local f = assert(io.open("/tmp/mgba-script-trace.txt", "a"))
        local sio = emu:read16(0x04000128)
        f:write(string.format("frame=%d sio=%04X parent=%d ready=%d peer=%d\n",
            frame, sio,
            emu:read8(link_base + off_parent),
            emu:read8(link_base + off_ready),
            emu:read8(link_base + off_peer)))
        f:close()
    end

    local parent = emu:read8(link_base + off_parent)

    if frame == 240 and parent ~= 0 then
        emu:addKey(C.GBA_KEY.A)
        pressed = true
    elseif frame == 246 and pressed then
        emu:clearKey(C.GBA_KEY.A)
        pressed = false
    end

    if frame == 420 then
        local sio = emu:read16(0x04000128)
        local player = math.floor(sio / 16) % 4
        local ready = emu:read8(link_base + off_ready)
        local peer = emu:read8(link_base + off_peer)
        local rx = emu:read32(link_base + off_frames_rx)
        local tx = emu:read32(link_base + off_frames_tx)
        local errs = emu:read32(link_base + off_errors)
        local path = string.format("/tmp/mgba-probe-%d.txt", player)
        local f = assert(io.open(path, "w"))
        f:write(string.format("player=%d\n", player))
        f:write(string.format("sio=%04X\n", sio))
        f:write(string.format("ready=%d\n", ready))
        f:write(string.format("parent=%d\n", parent))
        f:write(string.format("peer=%d\n", peer))
        f:write(string.format("frames_rx=%d\n", rx))
        f:write(string.format("frames_tx=%d\n", tx))
        f:write(string.format("sio_errors=%d\n", errs))
        f:close()
        emu:screenshot(string.format("/tmp/mgba-probe-%d.png", player))
    end
end)
