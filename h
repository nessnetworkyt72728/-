-- ============================================================
-- P2P Movement Sync — Two game instances on one PC
-- ============================================================
--
-- HOW TO USE:
--   1. Copy this file into BOTH game instance mod folders
--   2. In instance 1 (first window): set IS_HOST = true
--   3. In instance 2 (second window): set IS_HOST = false
--   4. Launch both instances
--   5. Press F6 in BOTH windows (host first, then client)
--   6. Both windows load into a 1v1 local match.
--      Each instance controls its own fighter.
--      Movement state is synced over UDP 127.0.0.1 between them.
--
-- KEYBINDS (same in both windows):
--   F6 = start match + begin sync
--   F7 = toggle sync on/off
--   F8 = print local fighter state to log
-- ============================================================

-- ---------------------------------------------------------------------------
-- CONFIG — the ONLY line that differs between the two instances
-- ---------------------------------------------------------------------------

local IS_HOST = true   -- true = instance 1 / P1.  false = instance 2 / P2.

-- UDP ports.  Both instances run on the same machine so we use loopback.
-- Host listens on HOST_PORT and sends to CLIENT_PORT, and vice versa.
local HOST_PORT   = 7770
local CLIENT_PORT = 7771

-- Characters (must match in both instances so the game loads correctly)
local P1_CHAR   = "character_c018"
local P2_CHAR   = "character_garnet"
local MATCH_MAP = "map_classic_3_platform_1v1"
local Ringouts  = 3
local MatchTime = 420

-- Rollback input delay in frames (2 = standard)
local ROLLBACK_DELAY = 2

-- ---------------------------------------------------------------------------
-- STATE
-- ---------------------------------------------------------------------------

local SyncEnabled   = false
local IsStarting    = false
local frameNum      = 0
local myFighter     = nil
local remoteFighter = nil
local sendSock      = nil
local recvSock      = nil
local localBuf      = {}
local remoteBuf     = {}
local timerRunning  = false

-- ---------------------------------------------------------------------------
-- HELPERS
-- ---------------------------------------------------------------------------

local function BPCall(obj, name, ...)
    local ok, err = pcall(function(...)
        local fn = obj[name]
        if not fn then return end
        fn(...)
    end, ...)
    if not ok then
        print("[P2P][WARN] " .. name .. ": " .. tostring(err))
    end
    return ok
end

local function GetOrCreateFacilitator(GI)
    local Fac = GI.LocalPlayFacilitator
    if Fac and Fac:IsValid() then return Fac end
    Fac = FindFirstOf("LocalPlayFacilitator_C")
    if Fac and Fac:IsValid() then return Fac end
    local ClassObj = StaticFindObject(
        "/Game/Panda_Main/Blueprints/LocalPlay/LocalPlayFacilitator.LocalPlayFacilitator_C")
    if not ClassObj or not ClassObj:IsValid() then
        ClassObj = FindFirstOf("LocalPlayFacilitator_C")
        if ClassObj and ClassObj:IsValid() then ClassObj = ClassObj:GetClass() end
    end
    if not ClassObj or not ClassObj:IsValid() then return nil end
    local ok, newFac = pcall(function() return StaticConstructObject(ClassObj, GI) end)
    if not ok or not newFac or not newFac:IsValid() then return nil end
    pcall(function() newFac.GameInstance = GI end)
    pcall(function() GI.LocalPlayFacilitator = newFac end)
    return newFac
end

-- ---------------------------------------------------------------------------
-- UDP SOCKET SETUP
-- ---------------------------------------------------------------------------

local function SetupSockets()
    local socket = require("socket")
    local myPort   = IS_HOST and HOST_PORT   or CLIENT_PORT
    local peerPort = IS_HOST and CLIENT_PORT or HOST_PORT

    local ok1, rs = pcall(function()
        local s = socket.udp()
        s:settimeout(0)
        s:setsockname("127.0.0.1", myPort)
        return s
    end)
    if not ok1 then
        print("[P2P][Error] Recv socket failed on :" .. myPort .. " -> " .. tostring(rs))
        return false
    end
    recvSock = rs

    local ok2, ss = pcall(function()
        local s = socket.udp()
        s:settimeout(0)
        s:setpeername("127.0.0.1", peerPort)
        return s
    end)
    if not ok2 then
        print("[P2P][Error] Send socket failed to :" .. peerPort .. " -> " .. tostring(ss))
        return false
    end
    sendSock = ss

    print("[P2P] Sockets OK — listening :" .. myPort .. "  sending :" .. peerPort)
    return true
end

-- ---------------------------------------------------------------------------
-- FIGHTER DISCOVERY
-- Finds all valid non-minion fighters in this instance, then splits them
-- into "mine" (human-controlled) and "remote" (bot placeholder).
-- ---------------------------------------------------------------------------

local function CollectFighters()
    local classNames = { "BaseFighter_C", "FighterCharacter", "ABP_FighterCharacter_C" }
    local seen, all = {}, {}
    for _, cls in ipairs(classNames) do
        local found = FindAllOf(cls)
        if found then
            for _, f in ipairs(found) do
                if f and f:IsValid() then
                    local addr = f:GetAddress()
                    if not seen[addr] then
                        seen[addr] = true
                        local isMinion = false
                        pcall(function() isMinion = f.IsMinion end)
                        if not isMinion then table.insert(all, f) end
                    end
                end
            end
        end
    end
    return all
end

local function FindFighters()
    myFighter     = nil
    remoteFighter = nil

    local all = CollectFighters()

    if #all < 2 then
        print("[P2P] Found " .. #all .. " fighter(s), need 2 — retrying in 1s...")
        ExecuteWithDelay(1000, FindFighters)
        return
    end

    -- Try to tell them apart by MatchPlayerData.PlayerIndex
    -- (safe field read — no method calls that could nullptr-crash)
    local byIndex = {}
    for _, f in ipairs(all) do
        local idx = nil
        pcall(function()
            local mpd = f.MatchPlayerData
            if mpd and mpd:IsValid() then
                local ok, pi = pcall(function() return mpd.PlayerIndex end)
                if ok and type(pi) == "number" then idx = pi end
            end
        end)
        if idx ~= nil then
            byIndex[idx] = f
        end
    end

    local p1 = byIndex[0] or all[1]
    local p2 = byIndex[1] or all[2]

    -- "Mine" is the slot matching our role; "remote" is the other
    if IS_HOST then
        myFighter     = p1
        remoteFighter = p2
    else
        myFighter     = p2
        remoteFighter = p1
    end

    print("[P2P] Fighters cached. mine=" ..
          tostring(myFighter:GetAddress()) ..
          " remote=" .. tostring(remoteFighter:GetAddress()))

    SyncEnabled = true
end

-- ---------------------------------------------------------------------------
-- SNAPSHOT  (read this instance's fighter state into a flat table)
-- ---------------------------------------------------------------------------

local function Snapshot(f)
    local s = { mx=0,my=0,sx=0,sy=0,btns=0,facing=false,
                px=0,py=0,pz=0, vx=0,vy=0,vz=0,
                grav=1, dashing=false,jumping=false,
                evading=false,ff=false }
    pcall(function()
        s.mx     = f.MoveXAxisValue or 0
        s.my     = f.MoveYAxisValue or 0
        s.sx     = f.StickAttackX  or 0
        s.sy     = f.StickAttackY  or 0
        s.facing = f.FacingLeft    or false
        s.dashing = f.IsDashing     or false
        s.jumping = f.Jumping       or false
        s.evading = f.IsEvading     or false
        s.ff      = f.IsFastFalling or false
    end)
    pcall(function()
        local bs = f.ButtonStates
        local b = 0
        if bs[0] then b=b|1 end
        if bs[1] then b=b|2 end
        if bs[2] then b=b|4 end
        if bs[3] then b=b|8 end
        s.btns = b
    end)
    pcall(function()
        local loc = f:K2_GetActorLocation()
        s.px,s.py,s.pz = loc.X,loc.Y,loc.Z
    end)
    pcall(function()
        local mc = f.FighterMovementComp
        if mc and mc:IsValid() then
            local v = mc.Velocity
            s.vx,s.vy,s.vz = v.X,v.Y,v.Z
            s.grav = mc.GravityScale or 1
        end
    end)
    return s
end

-- ---------------------------------------------------------------------------
-- SERIALIZE / DESERIALIZE
-- ---------------------------------------------------------------------------

local function Ser(frame, s)
    return string.format(
        "%d|%.3f|%.3f|%.3f|%.3f|%d|%d|%.1f|%.1f|%.1f|%.1f|%.1f|%.1f|%.3f|%d|%d|%d|%d",
        frame, s.mx,s.my,s.sx,s.sy, s.btns,
        s.facing and 1 or 0,
        s.px,s.py,s.pz, s.vx,s.vy,s.vz, s.grav,
        s.dashing and 1 or 0, s.jumping and 1 or 0,
        s.evading and 1 or 0, s.ff and 1 or 0)
end

local PAT =
    "(-?%d+)|(-?[%d%.]+)|(-?[%d%.]+)|(-?[%d%.]+)|(-?[%d%.]+)|"..
    "(%d+)|(%d+)|"..
    "(-?[%d%.]+)|(-?[%d%.]+)|(-?[%d%.]+)|"..
    "(-?[%d%.]+)|(-?[%d%.]+)|(-?[%d%.]+)|"..
    "(-?[%d%.]+)|(%d+)|(%d+)|(%d+)|(%d+)"

local function Des(data)
    local v = {data:match(PAT)}
    if #v < 18 then return nil end
    return {
        frame   = tonumber(v[1]),
        mx      = tonumber(v[2]),  my    = tonumber(v[3]),
        sx      = tonumber(v[4]),  sy    = tonumber(v[5]),
        btns    = tonumber(v[6]),
        facing  = tonumber(v[7]) == 1,
        px      = tonumber(v[8]),  py    = tonumber(v[9]),  pz = tonumber(v[10]),
        vx      = tonumber(v[11]), vy    = tonumber(v[12]), vz = tonumber(v[13]),
        grav    = tonumber(v[14]),
        dashing = tonumber(v[15]) == 1,
        jumping = tonumber(v[16]) == 1,
        evading = tonumber(v[17]) == 1,
        ff      = tonumber(v[18]) == 1,
    }
end

-- ---------------------------------------------------------------------------
-- APPLY REMOTE STATE onto the remote fighter in THIS instance
-- ---------------------------------------------------------------------------

local function axisTo8Way(x, y, fl)
    if fl then x = -x end
    if math.abs(x) < 0.25 and math.abs(y) < 0.25 then return 0 end
    local a = math.atan(y,x)*(180/math.pi)
    if a> 112.5 then return 8 elseif a> 67.5 then return 1
    elseif a> 22.5 then return 2 elseif a>-22.5 then return 3
    elseif a>-67.5 then return 4 elseif a>-112.5 then return 5
    elseif a>-157.5 then return 6 else return 7 end
end

local function ApplyRemote(s)
    local rf = remoteFighter
    if not rf or not rf:IsValid() then return end

    -- Axes + buttons
    pcall(function()
        rf.MoveXAxisValue = s.mx
        rf.MoveYAxisValue = s.my
        rf.StickAttackX   = s.sx
        rf.StickAttackY   = s.sy
        rf.FacingLeft     = s.facing
        rf.ButtonStates[0] = (s.btns&1)~=0
        rf.ButtonStates[1] = (s.btns&2)~=0
        rf.ButtonStates[2] = (s.btns&4)~=0
        rf.ButtonStates[3] = (s.btns&8)~=0
    end)

    local mc = nil
    pcall(function() mc = rf.FighterMovementComp end)
    if not mc or not mc:IsValid() then return end

    pcall(function() mc:NetGravity(s.grav) end)

    if s.jumping then
        local notJumping = true
        pcall(function() notJumping = not rf.Jumping end)
        if notJumping then
            local jv = 1000.0
            pcall(function() jv = rf.JumpVelocity end)
            pcall(function() mc:NetImpulse({X=0,Y=0,Z=jv}, false, true, 2) end)
        end
    end

    if s.evading then
        local notEvading = true
        pcall(function() notEvading = not rf.IsEvading end)
        if notEvading then
            local dx = s.mx*(s.facing and -1 or 1)
            pcall(function() mc:NetImpulse({X=0,Y=dx*800,Z=s.my*300}, true, false, 1) end)
            pcall(function() rf:EvadeButtonPressedMulticast(axisTo8Way(s.mx,s.my,s.facing)) end)
        end
    end

    if s.dashing then
        local notDashing = true
        pcall(function() notDashing = not rf.IsDashing end)
        if notDashing then
            pcall(function() rf:DashDanceLogic(false, true, math.abs(s.mx)>0.95) end)
        end
    end

    if s.ff then
        local notFF = true
        pcall(function() notFF = not rf.IsFastFalling end)
        if notFF then pcall(function() rf:TriggerFastFall() end) end
    end

    -- Position correction only when significantly desynced
    pcall(function()
        local loc = rf:K2_GetActorLocation()
        local d = math.sqrt((loc.X-s.px)^2+(loc.Y-s.py)^2+(loc.Z-s.pz)^2)
        if d > 80 then
            rf:K2_SetActorLocation({X=s.px,Y=s.py,Z=s.pz}, false, {}, false)
        end
    end)

    -- Velocity sync
    pcall(function()
        mc:NetImpulse({X=s.vx,Y=s.vy,Z=s.vz}, true, true, 7)
    end)

    pcall(function() rf:GenerateRollbackSaveState() end)
end

-- ---------------------------------------------------------------------------
-- SYNC TICK
-- ---------------------------------------------------------------------------

local function SyncTick()
    if not SyncEnabled then return end

    if not myFighter or not myFighter:IsValid() then
        SyncEnabled = false
        print("[P2P] Lost my fighter — re-searching...")
        ExecuteWithDelay(2000, FindFighters)
        return
    end

    frameNum = frameNum + 1
    local f  = frameNum

    -- Snapshot + buffer
    local snap = Snapshot(myFighter)
    localBuf[f] = snap

    -- Send delayed frame to peer
    local sf = f - ROLLBACK_DELAY
    if localBuf[sf] and sendSock then
        pcall(function() sendSock:send(Ser(sf, localBuf[sf])) end)
    end

    -- Receive all pending packets, keep newest
    if recvSock then
        while true do
            local data = recvSock:receive()
            if not data then break end
            local p = Des(data)
            if p then remoteBuf[p.frame] = p end
        end
    end

    -- Apply best available remote state
    local rs = remoteBuf[f - ROLLBACK_DELAY]
    if not rs then
        for i = f - ROLLBACK_DELAY - 1, f - ROLLBACK_DELAY - 8, -1 do
            if remoteBuf[i] then rs = remoteBuf[i]; break end
        end
    end
    if rs then ApplyRemote(rs) end

    pcall(function() myFighter:GenerateRollbackSaveState() end)

    -- Expire old entries
    localBuf[f-120]  = nil
    remoteBuf[f-120] = nil
end

local function StartSyncLoop()
    if timerRunning then return end
    timerRunning = true
    local function tick()
        SyncTick()
        ExecuteWithDelay(16, tick)
    end
    ExecuteWithDelay(16, tick)
    print("[P2P] Sync loop running.")
end

-- ---------------------------------------------------------------------------
-- MATCH LAUNCH  (F6)
-- ---------------------------------------------------------------------------

local function ClearBotSlot(playerIndex)
    ExecuteWithDelay(2500, function()
        local AllMPD = FindAllOf("MatchPlayerData_C")
        if not AllMPD then return end
        for _, MPD in ipairs(AllMPD) do
            if MPD and MPD:IsValid() then
                local ok, pi = pcall(function() return MPD.PlayerIndex end)
                if ok and pi == playerIndex then
                    pcall(function()
                        MPD.isBot       = false
                        MPD.BotBehavior = 0
                        MPD.BotFromDisconnection = false
                    end)
                    print("[P2P] Slot " .. playerIndex .. " cleared (no bot).")
                end
            end
        end
    end)
end

local function StartMatch()
    if IsStarting then return end
    IsStarting = true

    frameNum   = 0
    localBuf   = {}
    remoteBuf  = {}
    myFighter  = nil
    remoteFighter = nil
    SyncEnabled   = false

    print("[P2P] StartMatch as " .. (IS_HOST and "HOST/P1" or "CLIENT/P2"))

    if not SetupSockets() then
        print("[P2P][Error] Socket setup failed.")
        IsStarting = false
        return
    end

    local GI = FindFirstOf("PandaGameInstance_C")
    if not GI or not GI:IsValid() then
        print("[P2P][Error] No GameInstance.")
        IsStarting = false
        return
    end

    local PM = GI.PreferencesManager
    if not PM or not PM:IsValid() then
        print("[P2P][Error] No PreferencesManager.")
        IsStarting = false
        return
    end

    local Fac = GetOrCreateFacilitator(GI)
    if not Fac then
        print("[P2P][Error] No Facilitator.")
        IsStarting = false
        return
    end

    -- Identical match config in both instances
    BPCall(PM, "Set-LocalPlay-GameMode",  0)
    BPCall(PM, "Set-LocalPlay-TeamStyle", 0)
    BPCall(PM, "Set-LocalPlay-Time",      MatchTime)
    BPCall(PM, "Set-LocalPlay-Ringouts",  Ringouts)
    BPCall(PM, "Set-LocalPlay-Hazards",   false)
    BPCall(PM, "Set-LocalPlay-Maps",      { MATCH_MAP })
    BPCall(PM, "Set-LocalPlay-CharacterForPlayer", 0, P1_CHAR)
    BPCall(PM, "Set-LocalPlay-UsernameForPlayer",  0, "P1")
    BPCall(PM, "Set-LocalPlay-CharacterForPlayer", 1, P2_CHAR)
    BPCall(PM, "Set-LocalPlay-UsernameForPlayer",  1, "P2")

    Fac = GI.LocalPlayFacilitator or Fac
    local ok, err = pcall(function() Fac:BeginMatch() end)
    if not ok then
        print("[P2P][Error] BeginMatch: " .. tostring(err))
        IsStarting = false
        return
    end
    print("[P2P] BeginMatch OK.")

    -- Clear bot flag on this instance's own slot only
    ClearBotSlot(IS_HOST and 0 or 1)

    -- Wait for both fighters to spawn, then start syncing
    ExecuteWithDelay(6000, function()
        FindFighters()
        StartSyncLoop()
    end)

    IsStarting = false
end

-- ---------------------------------------------------------------------------
-- KEYBINDS
-- ---------------------------------------------------------------------------

local function ToggleSync()
    SyncEnabled = not SyncEnabled
    print("[P2P] Sync " .. (SyncEnabled and "ON" or "OFF"))
end

local function PrintState()
    local role = IS_HOST and "HOST/P1" or "CLIENT/P2"
    if not myFighter or not myFighter:IsValid() then
        print("[P2P] " .. role .. " — no fighter cached yet.")
        return
    end
    local pos,vel = "?","?"
    pcall(function()
        local l = myFighter:K2_GetActorLocation()
        pos = string.format("%.0f,%.0f,%.0f", l.X,l.Y,l.Z)
    end)
    pcall(function()
        local mc = myFighter.FighterMovementComp
        local v  = mc.Velocity
        vel = string.format("%.0f,%.0f,%.0f", v.X,v.Y,v.Z)
    end)
    local lc,rc = 0,0
    for _ in pairs(localBuf)  do lc=lc+1 end
    for _ in pairs(remoteBuf) do rc=rc+1 end
    print(string.format("[P2P] %s | f=%d | pos=(%s) | vel=(%s) | lbuf=%d rbuf=%d sync=%s",
        role, frameNum, pos, vel, lc, rc, tostring(SyncEnabled)))
end

RegisterKeyBind(Key.F6, StartMatch)
RegisterKeyBind(Key.F7, ToggleSync)
RegisterKeyBind(Key.F8, PrintState)

print("[P2P] Loaded as " .. (IS_HOST and "HOST (P1)" or "CLIENT (P2)"))
print("[P2P]   F6 = start match")
print("[P2P]   F7 = toggle sync")
print("[P2P]   F8 = print state")
