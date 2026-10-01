-- mGBA multiplayer hardware-path probe.
-- Loaded into both link-test ROM instances. It drives A on hardware player 0,
-- then reads the ROM's real gba_link_t + application receive counters.

local function envnum(name, base)
    local s = os.getenv(name)
    if not s then error("missing env "..name) end
    local v = tonumber(s, base or 10)
    if not v then error("bad env "..name.."="..s) end
    return v
end

local link_base = envnum("LINK_BASE", 16)
local app_rx_addr = envnum("APP_RX_ADDR", 16)
local app_last_addr = envnum("APP_LAST_ADDR", 16)

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
    local sio = emu:read16(0x04000128)
    local player = math.floor(sio / 16) % 4

    -- Give both instances time to complete HELLO. Player 0 is the only one
    -- that presses A, which queues Elfin edge-count 10 in the actual ROM.
    if frame == 150 and player == 0 then
        emu:addKey(0) -- GBA_KEY_A
        pressed = true
    elseif frame == 156 and pressed then
        emu:clearKey(0)
        pressed = false
    end

    if frame == 300 then
        local ready = emu:read8(link_base + off_ready)
        local parent = emu:read8(link_base + off_parent)
        local peer = emu:read8(link_base + off_peer)
        local rx = emu:read32(link_base + off_frames_rx)
        local tx = emu:read32(link_base + off_frames_tx)
        local errs = emu:read32(link_base + off_errors)
        local app_rx = emu:read32(app_rx_addr)
        local app_last = emu:read8(app_last_addr)

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
        f:write(string.format("app_rx=%d\n", app_rx))
        f:write(string.format("app_last=%d\n", app_last))
        f:close()

        -- This is mGBA's internal framebuffer capture, not an X11 window grab.
        emu:screenshot(string.format("/tmp/mgba-probe-%d.png", player))
    end
end)
