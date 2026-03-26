-- BotPlay AutoStart - F6 to launch
-- Self-constructs LocalPlayFacilitator so you don't need to visit Local Play menu first.
-- Config: edit these values

-- Map pool - random map selected each match
local MapPool = {
    "Map_batcave",
    "map_classic_3_platform_1v1",
    "map_classic_3_platform_2v2",
    "map_m008",
    "map_m009",
    "map_m009_1v1",
    "map_M010",
    "map_m011",
    "map_m011_largenoplat",
    "map_m011_small",
    "map_m011_smallnoplat",
    "map_scooby_doo_2",
    "map_scooby_doo_2_noroof",
    "Map_TrainingRoomLarge",
    "Map_TrainingRoomSmall",
    "Map_tree_house_1v1",
    "map_tree_house_2v2_1_platform",
    "Map_TreeHouse_2v2",
    "map_trophy_room_2_platform",
    "map_trophy_room_large_platform",
}

-- Player setup - up to 6 players
-- Team: 0 or 1 (for Teams mode)
-- IsBot: true/false
-- BotDifficulty: 0=NoBots, 2=VeryEasy, 3=Easy, 4=Medium, 5=Hard (from EBotBehavior enum)
-- MinDifficulty/MaxDifficulty: 0.0-1.0 range for bot skill variance
local Players = {
    {Enabled = true,  Char = "character_batman",       Name = "Nibi",                      Team = 0, IsBot = false, BotDifficulty = 4, MinDifficulty = 0.5, MaxDifficulty = 0.8},
    {Enabled = true,  Char = "character_steven",       Name = "I BOT 1 MILLION TOASTS!!!", Team = 1, IsBot = true,  BotDifficulty = 4, MinDifficulty = 0.5, MaxDifficulty = 0.8},
    {Enabled = false, Char = "character_finn",         Name = "Botso",                     Team = 1, IsBot = true,  BotDifficulty = 4, MinDifficulty = 0.5, MaxDifficulty = 0.8},
    {Enabled = false, Char = "character_garnet",       Name = "I Bot This Account",        Team = 0, IsBot = true,  BotDifficulty = 4, MinDifficulty = 0.5, MaxDifficulty = 0.8},
    {Enabled = false, Char = "character_wonder_woman", Name = "Player 5",                  Team = 0, IsBot = false, BotDifficulty = 4, MinDifficulty = 0.5, MaxDifficulty = 0.8},
    {Enabled = false, Char = "character_harley_quinn", Name = "Player 6",                  Team = 1, IsBot = false, BotDifficulty = 4, MinDifficulty = 0.5, MaxDifficulty = 0.8},
}

-- Game mode: 0=Teams, 2=FreeForAll
-- TeamStyle: 0=Solos(1v1), 1=Duos(2v2)
local GameMode  = 0
local TeamStyle = 0
local Ringouts  = 3
local MatchTime = 420

local IsStarting = false
local MatchStarting = false

-- ---------------------------------------------------------------------------
-- Helpers
-- ---------------------------------------------------------------------------

local function BPCall(obj, name, ...)
    local ok, err = pcall(function(...)
        local fn = obj[name]
        if not fn then
            print("[BotPlay][WARN] Function not found: " .. name)
            return
        end
        fn(...)
    end, ...)
    if not ok then
        print("[BotPlay][WARN] " .. name .. " error: " .. tostring(err))
    end
    return ok
end

local function RandomMap()
    math.randomseed(os.time())
    return MapPool[math.random(#MapPool)]
end

-- ---------------------------------------------------------------------------
-- Construct or reuse the LocalPlayFacilitator
-- ---------------------------------------------------------------------------

local function GetOrCreateFacilitator(GI)
    -- 1) Already attached to the GameInstance
    local Fac = GI.LocalPlayFacilitator
    if Fac and Fac:IsValid() then
        print("[BotPlay] Using GI.LocalPlayFacilitator")
        return Fac
    end

    -- 2) One already floating in the world
    Fac = FindFirstOf("LocalPlayFacilitator_C")
    if Fac and Fac:IsValid() then
        print("[BotPlay] Using existing world LocalPlayFacilitator_C")
        return Fac
    end

    -- 3) Construct a new one via StaticConstructObject
    print("[BotPlay] Constructing LocalPlayFacilitator_C via StaticConstructObject...")

    local ClassObj = StaticFindObject("/Game/Panda_Main/Blueprints/LocalPlay/LocalPlayFacilitator.LocalPlayFacilitator_C")
    if not ClassObj or not ClassObj:IsValid() then
        ClassObj = FindFirstOf("LocalPlayFacilitator_C")
        if ClassObj and ClassObj:IsValid() then
            ClassObj = ClassObj:GetClass()
        end
    end

    if not ClassObj or not ClassObj:IsValid() then
        print("[BotPlay][Error] Could not find LocalPlayFacilitator_C class object.")
        print("[BotPlay]        Load into the Main Menu at least once so the Blueprint is in memory.")
        return nil
    end

    local ok, newFac = pcall(function()
        return StaticConstructObject(ClassObj, GI)
    end)

    if not ok or not newFac or not newFac:IsValid() then
        print("[BotPlay][Error] StaticConstructObject failed: " .. tostring(newFac))
        return nil
    end

    local ok2, err2 = pcall(function()
        newFac.GameInstance = GI
    end)
    if not ok2 then
        print("[BotPlay][WARN] Could not set GameInstance on facilitator: " .. tostring(err2))
    end

    local ok3, err3 = pcall(function()
        GI.LocalPlayFacilitator = newFac
    end)
    if not ok3 then
        print("[BotPlay][WARN] Could not pin Facilitator onto GI: " .. tostring(err3))
    end

    print("[BotPlay] LocalPlayFacilitator_C constructed successfully.")
    return newFac
end

-- ---------------------------------------------------------------------------
-- Post-match-start: apply teams / bot flags
-- ---------------------------------------------------------------------------

local function ApplyTeamsAndBots()
    ExecuteWithDelay(2000, function()
        print("[BotPlay] ApplyTeamsAndBots: Starting...")

        local GI = FindFirstOf("PandaGameInstance_C")
        if not GI or not GI:IsValid() then
            print("[BotPlay][Error] No GameInstance")
            return
        end

        local PDM = GI.PlayerDataManager
        if not PDM or not PDM:IsValid() then
            print("[BotPlay][Error] No PlayerDataManager")
            return
        end

        local AllMPD = FindAllOf("MatchPlayerData_C")
        if not AllMPD then
            print("[BotPlay][Error] FindAllOf returned nil")
            return
        end
        print("[BotPlay] Found " .. #AllMPD .. " total MatchPlayerData instances")

        -- Filter to only MPDs that belong to the current match
        -- by checking if their PlayerState is valid and alive
        local matchMPDs = {}
        for _, MPD in ipairs(AllMPD) do
            if MPD and MPD:IsValid() then
                local ok, ps = pcall(function() return MPD.PlayerState end)
                if ok and ps and ps:IsValid() then
                    table.insert(matchMPDs, MPD)
                    print("[BotPlay] Active MPD: PlayerIndex=" .. tostring(MPD.PlayerIndex) .. " isBot=" .. tostring(MPD.isBot))
                end
            end
        end

        print("[BotPlay] Found " .. #matchMPDs .. " active match MPDs")

        for _, MPD in ipairs(matchMPDs) do
            local idx = MPD.PlayerIndex
            print("[BotPlay] Processing player " .. idx)

            -- Find matching player config by enabled-slot index
            local playerConfig = nil
            local configIdx = 1
            for j = 1, #Players do
                if Players[j].Enabled then
                    if configIdx - 1 == idx then
                        playerConfig = Players[j]
                        break
                    end
                    configIdx = configIdx + 1
                end
            end

            if playerConfig then
                print("[BotPlay] Config found: Team=" .. playerConfig.Team .. " IsBot=" .. tostring(playerConfig.IsBot))

                -- Only assign team index for human players
                if not playerConfig.IsBot then
                    local ok, err = pcall(function()
                        MPD:AssignPlayerAndTeamIndex(idx, playerConfig.Team)
                    end)
                    if ok then
                        print("[BotPlay] ✓ Player " .. idx .. " -> Team " .. playerConfig.Team)
                    else
                        print("[BotPlay][WARN] Team assignment failed: " .. tostring(err))
                    end
                end

                -- Write bot properties directly (Set Is Bot function crashes the game)
                if playerConfig.IsBot then
                    local ok, err = pcall(function()
                        MPD.isBot = true
                        MPD.BotBehavior = playerConfig.BotDifficulty
                        MPD.MinBotDifficulty = playerConfig.MinDifficulty
                        MPD.MaxBotDifficulty = playerConfig.MaxDifficulty
                        MPD.BotFromDisconnection = false
                    end)
                    if ok then
                        print("[BotPlay] ✓ Player " .. idx .. " -> Bot written. isBot=" .. tostring(MPD.isBot) .. " BotBehavior=" .. tostring(MPD.BotBehavior))
                    else
                        print("[BotPlay][WARN] Bot write failed: " .. tostring(err))
                    end
                end

            else
                print("[BotPlay][WARN] No config found for player " .. idx)
            end
        end

        MatchStarting = false
        print("[BotPlay] ApplyTeamsAndBots: Done.")
    end)
end

-- ---------------------------------------------------------------------------
-- Main entry point
-- ---------------------------------------------------------------------------

local function StartMatch()
    if IsStarting then return end
    IsStarting = true
    MatchStarting = true
    print("[BotPlay] StartMatch triggered.")

    local GI = FindFirstOf("PandaGameInstance_C")
    if not GI or not GI:IsValid() then
        print("[BotPlay][Error] No PandaGameInstance_C found. Are you in-game?")
        IsStarting = false
        return
    end

    local PM = GI.PreferencesManager
    if not PM or not PM:IsValid() then
        print("[BotPlay][Error] No PreferencesManager found.")
        IsStarting = false
        return
    end

    -- Attempt to get / construct the facilitator BEFORE touching preferences
    local Fac = GetOrCreateFacilitator(GI)
    if not Fac then
        print("[BotPlay][Error] Could not obtain LocalPlayFacilitator_C. Aborting.")
        IsStarting = false
        return
    end

    -- Count enabled players
    local enabledCount = 0
    for i = 1, #Players do
        if Players[i].Enabled then enabledCount = enabledCount + 1 end
    end
    print("[BotPlay] " .. enabledCount .. " players enabled")

    -- Pick random map
    local SelectedMap = RandomMap()
    print("[BotPlay] Selected map: " .. SelectedMap)

    -- Apply settings to PreferencesManager
    BPCall(PM, "Set-LocalPlay-GameMode",  GameMode)
    BPCall(PM, "Set-LocalPlay-TeamStyle", TeamStyle)
    BPCall(PM, "Set-LocalPlay-Time",      MatchTime)
    BPCall(PM, "Set-LocalPlay-Ringouts",  Ringouts)
    BPCall(PM, "Set-LocalPlay-Hazards",   true)
    BPCall(PM, "Set-LocalPlay-Maps",      { SelectedMap })

    local playerIndex = 0
    for i = 1, #Players do
        if Players[i].Enabled then
            BPCall(PM, "Set-LocalPlay-CharacterForPlayer", playerIndex, Players[i].Char)
            BPCall(PM, "Set-LocalPlay-UsernameForPlayer",  playerIndex, Players[i].Name)
            playerIndex = playerIndex + 1
        end
    end

    print("[BotPlay] Settings applied. Calling BeginMatch...")

    -- Re-fetch in case GI.LocalPlayFacilitator was updated by GetOrCreateFacilitator
    Fac = GI.LocalPlayFacilitator or Fac

    local ok, err = pcall(function() Fac:BeginMatch() end)
    if ok then
        print("[BotPlay] BeginMatch called successfully.")
        ApplyTeamsAndBots()
    else
        print("[BotPlay][Error] BeginMatch: " .. tostring(err))
        print("[BotPlay]        If this keeps failing, open Local Play in the menu once to")
        print("[BotPlay]        fully initialise the facilitator, then press F6 again.")
    end

    IsStarting = false
end

RegisterKeyBind(Key.F6, StartMatch)

print("[BotPlay] Loaded. Press F6 anywhere to launch a match.")
print("[BotPlay] If BeginMatch errors, visit the Local Play screen once first.")
