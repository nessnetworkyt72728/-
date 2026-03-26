-- MultiVersus Offline Pre-Match Screen
-- UE4SS Lua Mod -- Fixed / Annotated Edition
-- v5: Skin selector crash fix + session timeout fix
--
-- FIXES FROM v4:
--   1. Skin selector crash on some characters:
--      - Staggered SetCharacter -> Populate -> FocusSelected with delays
--        so async skin assets finish loading before the grid is built.
--      - Added nil/validity guard on selector widget inside BndEvt hook.
--   2. Match kicked at ~6:10 (EOS session expiry):
--      - Heartbeat now also resets LocalPlayFacilitator_C and PandaGameState_C.
--      - Heartbeat interval reduced from 20s to 10s.

local TAG = "[OfflinePreMatch] "
local function log(msg) print(TAG .. tostring(msg)) end

-- ============================================================
-- Hardcoded default skin slug overrides
-- ============================================================
local SKIN_OVERRIDES = {
    c019          = "c019_s00",
    c020          = "c020_s00",
    c020b         = "c020b_s00",
    c021          = "c021_s00",
    c023a         = "c023a_s00",
    c023b         = "c023b_s00",
    c018          = "c018_s00",
    wonderwomanv2 = "skin_wonderwoman",
    harley        = "harley",
}

-- ============================================================
-- Character slug overrides
-- ============================================================
local CHAR_SLUG_OVERRIDES = {
    bugsbunnyv2   = "bugs_bunny",
    harleyquinn   = "harleyquinn",
    wonderwomanv2 = "wonder_woman",
    c016          = "c16",
    c015          = "taz",
    tomandjerry   = "tom_and_jerry",
}

-- ============================================================
-- Safe UFunction caller
-- ============================================================
local function safe_call(obj, func_name, ...)
    if not obj or not obj:IsValid() then
        log("WARN: object invalid when calling " .. func_name)
        return false
    end
    local ok, err = pcall(function(...) obj[func_name](obj, ...) end, ...)
    if not ok then
        log("ERROR calling " .. func_name .. ": " .. tostring(err))
    end
    return ok
end

-- ============================================================
-- Object finders
-- ============================================================
local function get_prematch_actor()
    local actors = FindAllOf("PreMatch_C") or {}
    for _, o in ipairs(actors) do
        if o:IsValid() then return o end
    end
    local lsas = FindAllOf("LevelScriptActor") or {}
    for _, o in ipairs(lsas) do
        if o:IsValid() and o:GetFullName():find("PreMatch") then return o end
    end
    return nil
end

local function get_lobby_pc()
    local pcs = FindAllOf("LobbyPlayerController_C") or {}
    for _, o in ipairs(pcs) do
        if o:IsValid() then return o end
    end
    pcs = FindAllOf("PFGPlayerController_C") or {}
    for _, o in ipairs(pcs) do
        if o:IsValid() then return o end
    end
    return nil
end

local function get_prematch_ui()
    local widgets = FindAllOf("UI_PreMatch_C") or {}
    for _, w in ipairs(widgets) do
        if w:IsValid() then return w end
    end
    return nil
end

local preview_prop_names = {
    [0] = "BP_CharacterPreview1_ExecuteUbergraph_PreMatch_RefProperty",
    [1] = "BP_CharacterPreview_2_ExecuteUbergraph_PreMatch_RefProperty",
    [2] = "BP_CharacterPreview_3_ExecuteUbergraph_PreMatch_RefProperty",
    [3] = "BP_CharacterPreview4_ExecuteUbergraph_PreMatch_RefProperty",
}

local function get_preview_actor(pm, slot)
    local prop = preview_prop_names[slot]
    if not prop then return nil end
    local actor = nil
    pcall(function() actor = pm[prop] end)
    if actor and actor:IsValid() then return actor end
    return nil
end

-- ============================================================
-- Slug / skin helpers
-- ============================================================
local function char_to_slug(char_data)
    local full = char_data:GetFullName()
    local folder = full:match("/Characters/([^/]+)/")
    if folder then return folder:lower() end
    local asset = full:match("/([^/]+)%.[^/]+$") or ""
    return asset:lower():gsub("^character_", "")
end

-- ============================================================
-- Skin cache
-- ============================================================
local skin_cache = {}

local function score_skin_asset(asset)
    local score = 0
    if asset:find("default")    then score = score + 10 end
    if asset:find("base")       then score = score + 8  end
    if asset:find("_s00")       then score = score + 10 end
    if asset:find("skin_1")     then score = score + 6  end
    if asset:find("_01")        then score = score + 5  end
    if asset:find("standard")   then score = score + 5  end
    if asset:find("original")   then score = score + 5  end
    if asset:find("classic")    then score = score + 4  end
    if asset:find("normal")     then score = score + 4  end
    if asset:find("skin_[2-9]") then score = score - 5  end
    if asset:find("_s0[1-9]")   then score = score - 3  end
    if asset:find("_0[2-9]")    then score = score - 4  end
    if asset:find("prestige")   then score = score - 6  end
    if asset:find("mastery")    then score = score - 6  end
    if asset:find("ranked")     then score = score - 4  end
    if asset:find("gold")       then score = score - 3  end
    if asset:find("diamond")    then score = score - 3  end
    if asset:find("platinum")   then score = score - 3  end
    if asset:find("server")     then score = score - 8  end
    if asset:find("twitch")     then score = score - 8  end
    if asset:find("prime")      then score = score - 5  end
    if asset:find("battle")     then score = score - 4  end
    if asset:find("season")     then score = score - 4  end
    if asset:find("event")      then score = score - 4  end
    if asset:find("exclusive")  then score = score - 6  end
    if asset:find("dlc")        then score = score - 5  end
    if asset:find("bundle")     then score = score - 5  end
    return score
end

local function find_skin_for_slug(slug)
    if slug == "" then return nil end

    local cached = skin_cache[slug]
    if cached ~= nil then
        if cached == false then return nil end
        local ok, valid = pcall(function() return cached:IsValid() end)
        if ok and valid then return cached end
        skin_cache[slug] = nil
        log("  Cache evict (stale): " .. slug)
    end

    local all_skins = FindAllOf("SkinData") or {}

    local override = SKIN_OVERRIDES[slug]
    if override then
        for _, skin in ipairs(all_skins) do
            local ok, valid = pcall(function() return skin:IsValid() end)
            if ok and valid then
                local nameok, name = pcall(function() return skin:GetFullName():lower() end)
                if nameok and name and name:find(override, 1, true) then
                    log("  Override match for '" .. slug .. "': " .. skin:GetFullName())
                    skin_cache[slug] = skin
                    return skin
                end
            end
        end
        log("  Override '" .. override .. "' not found, falling back to scoring")
    end

    local best_skin  = nil
    local best_score = -999
    local best_len   = 99999

    for _, skin in ipairs(all_skins) do
        local ok, valid = pcall(function() return skin:IsValid() end)
        if not (ok and valid) then goto continue end

        local nameok, name = pcall(function() return skin:GetFullName():lower() end)
        if not (nameok and name) then goto continue end

        if not name:find(slug, 1, true) then goto continue end

        local asset = name:match("/([^/]+)%.[^/]+$") or name
        local score = score_skin_asset(asset)
        local len   = #asset

        if score > best_score or (score == best_score and len < best_len) then
            best_score = score
            best_len   = len
            best_skin  = skin
        end
        ::continue::
    end

    skin_cache[slug] = best_skin or false
    if best_skin then
        log("  Skin cached for '" .. slug .. "': " .. best_skin:GetFullName())
    else
        log("  No skin found for '" .. slug .. "' (cached miss)")
    end
    return best_skin
end

-- ============================================================
-- Find CharacterData object from a skin's slug
-- ============================================================
local char_data_cache = {}

local function find_char_data_for_slug(slug)
    if slug == "" then return nil end
    if char_data_cache[slug] ~= nil then
        if char_data_cache[slug] == false then return nil end
        local ok, valid = pcall(function() return char_data_cache[slug]:IsValid() end)
        if ok and valid then return char_data_cache[slug] end
        char_data_cache[slug] = nil
    end

    local all_chars = FindAllOf("CharacterData") or {}
    for _, cd in ipairs(all_chars) do
        local ok, valid = pcall(function() return cd:IsValid() end)
        if not (ok and valid) then goto skip end
        local nameok, name = pcall(function() return cd:GetFullName():lower() end)
        if not (nameok and name) then goto skip end
        local folder = name:match("/characters/([^/]+)/")
        local asset  = name:match("/([^/]+)%.[^/]+$") or ""
        local cd_slug = (folder or asset):gsub("^character_", ""):gsub("^chardata_","")
        if cd_slug == slug or asset:find(slug, 1, true) then
            char_data_cache[slug] = cd
            log("  CharData cached for '" .. slug .. "': " .. cd:GetFullName())
            return cd
        end
        ::skip::
    end
    char_data_cache[slug] = false
    log("  No CharData found for slug '" .. slug .. "'")
    return nil
end

-- ============================================================
-- Apply a SkinData directly to a preview actor slot.
-- ============================================================
local function apply_skin_to_preview(slot, skin, is_teammate)
    if not skin or not skin:IsValid() then return false end

    local ui = get_prematch_ui()
    if ui and ui:IsValid() then
        local team_index = is_teammate and 0 or 1
        local ok, err = pcall(function()
            ui:UpdateFighterPreview(skin, slot, team_index)
        end)
        if ok then
            log("  UpdateFighterPreview OK slot=" .. slot .. " team=" .. team_index .. ": " .. skin:GetFullName())
            return true
        else
            log("  UpdateFighterPreview failed: " .. tostring(err))
        end
    end

    local pm = get_prematch_actor()
    if not pm then return false end

    pcall(function()
        if is_teammate then
            pm.Teammates[slot + 1] = skin
        else
            local es = slot - 1
            if es < 1 then es = 1 end
            pm.Enemies[es] = skin
        end
    end)

    local preview = get_preview_actor(pm, slot)
    if preview and preview:IsValid() then
        local team_index = is_teammate and 0 or 1
        local ok2, err2 = pcall(function()
            preview:BPI_ChangeFighter(skin, team_index, true)
        end)
        if ok2 then
            log("  BPI_ChangeFighter OK slot " .. slot)
        else
            log("  BPI_ChangeFighter failed (" .. tostring(err2) .. "), writing DesiredSkin")
            pcall(function() preview.DesiredSkin = skin end)
            safe_call(pm, "BPI_ShowPlayer", slot, is_teammate)
        end
        return true
    else
        safe_call(pm, "BPI_ShowPlayer", slot, is_teammate)
        return false
    end
end

-- ============================================================
-- Set a preview actor slot to show a character (default skin).
-- ============================================================
local function set_preview_character(slot, char_data, is_teammate)
    local pm = get_prematch_actor()
    if not pm then log("ERROR: no PreMatch actor") return end

    local slug = char_to_slug(char_data)
    log("  Resolved slug: '" .. slug .. "'")

    local skin = find_skin_for_slug(slug)
    if skin then
        apply_skin_to_preview(slot, skin, is_teammate)
    else
        log("  Slot " .. slot .. ": no SkinData for '" .. slug .. "'")
        safe_call(pm, "BPI_ShowPlayer", slot, is_teammate)
    end
end

-- ============================================================
-- Extract SkinData from a UI_GridItemBase / UI_EquipperItem_V2
-- grid item widget.  Used by the skin-hover hook.
--
-- The UI_PM_SkinSelector_C stores the focused skin on itself in
-- "Focused Skin Data" immediately before broadcasting the focused
-- event, so we can read it straight from the selector widget.
-- If that fails we fall through to reading the item widget's own
-- ItemData (UI_EquipperItemData_C) which has a SkinData field.
-- ============================================================
local function extract_skin_from_grid_item(grid_item, selector_widget)
    -- Path 1: the selector widget already cached it in "Focused Skin Data"
    if selector_widget and selector_widget:IsValid() then
        local skin = nil
        pcall(function() skin = selector_widget["Focused Skin Data"] end)
        if skin then
            local ok, v = pcall(function() return skin:IsValid() end)
            if ok and v then
                log("  SkinHover: got skin from 'Focused Skin Data': " .. skin:GetFullName())
                return skin
            end
        end
    end

    if not grid_item or not grid_item:IsValid() then return nil end

    -- Path 2: cast grid item to UI_EquipperItem_V2_C and read ItemData.SkinData
    local skin = nil
    pcall(function()
        local item_data = grid_item.ItemData
        if item_data and item_data:IsValid() then
            local sd = item_data.SkinData
            if sd then
                local ok2, v2 = pcall(function() return sd:IsValid() end)
                if ok2 and v2 then skin = sd end
            end
        end
    end)
    if skin then
        log("  SkinHover: got skin from ItemData.SkinData: " .. skin:GetFullName())
        return skin
    end

    -- Path 3: generic Data property on the grid item base
    pcall(function()
        local d = grid_item.Data
        if d then
            local ok3, v3 = pcall(function() return d:IsValid() end)
            if ok3 and v3 then
                local nameok, nm = pcall(function() return d:GetFullName() end)
                if nameok and nm and nm:find("^SkinData ") then
                    skin = d
                end
            end
        end
    end)
    if skin then
        log("  SkinHover: got skin from generic Data property: " .. skin:GetFullName())
        return skin
    end

    log("  SkinHover: could not extract SkinData from grid item")
    return nil
end

-- ============================================================
-- Skin Selector system
-- ============================================================
local confirmed_skin       = nil
local skin_selector_active = false
local skin_sel_hooks       = {}

local function get_skin_selector_widget(ui)
    if not ui or not ui:IsValid() then return nil end
    local w = nil
    pcall(function() w = ui["UI_PM_SkinSelectorP0"] end)
    if w and w:IsValid() then return w end
    local all = FindAllOf("UI_PM_SkinSelector_C") or {}
    for _, s in ipairs(all) do
        local ok, v = pcall(function() return s:IsValid() end)
        if ok and v then return s end
    end
    return nil
end

-- FIX 1: Stagger SetCharacter -> Populate -> FocusSelected with delays
-- so async skin assets have time to load before the grid is built.
local function open_skin_selector(ui, char_data)
    if not ui or not ui:IsValid() then return end
    local sel = get_skin_selector_widget(ui)
    if not sel then
        log("SkinSelector: UI_PM_SkinSelectorP0 not found")
        return
    end

    local cos = nil
    pcall(function() cos = ui["CosmeticActionsP0"] end)
    if cos and cos:IsValid() then
        pcall(function() cos:SetVisibility(0) end)
    end

    pcall(function() sel:SetVisibility(0) end)

    -- Apply character first, then stagger Populate and FocusSelected
    -- so the game's async asset loader can finish before we build the grid.
    if char_data and char_data:IsValid() then
        safe_call(sel, "SetCharacter", char_data)
    end

    ExecuteWithDelay(150, function()
        if not sel:IsValid() then
            log("SkinSelector: sel became invalid before Populate")
            return
        end
        safe_call(sel, "Populate")

        ExecuteWithDelay(100, function()
            if not sel:IsValid() then
                log("SkinSelector: sel became invalid before FocusSelected")
                return
            end
            safe_call(sel, "FocusSelected")
            safe_call(sel, "UpdateAppearance")
        end)
    end)

    pcall(function()
        local btn = ui["ShowMoveListP0"]
        if btn and btn:IsValid() then
            btn:SetVisibility(0)
            log("SkinSelector: emote button ShowMoveListP0 shown")
        end
    end)

    skin_selector_active = true
    log("SkinSelector: opened for " ..
        (char_data and char_data:IsValid() and char_data:GetFullName() or "?"))
end

local function close_skin_selector(ui)
    if not ui or not ui:IsValid() then return end
    local sel = get_skin_selector_widget(ui)
    if sel and sel:IsValid() then
        pcall(function() sel:SetVisibility(1) end)
    end
    pcall(function()
        local btn = ui["ShowMoveListP0"]
        if btn and btn:IsValid() then btn:SetVisibility(1) end
    end)
    local cos = nil
    pcall(function() cos = ui["CosmeticActionsP0"] end)
    if cos and cos:IsValid() then
        pcall(function() cos:SetVisibility(1) end)
    end
    skin_selector_active = false
    log("SkinSelector: closed")
end

local function show_emotes(ui, open_picker)
    if not ui or not ui:IsValid() then return end
    pcall(function()
        local cos = ui["CosmeticActionsP0"]
        if cos and cos:IsValid() then cos:SetVisibility(0) end
    end)
    pcall(function()
        local btn = ui["ShowMoveListP0"]
        if btn and btn:IsValid() then
            btn:SetVisibility(0)
            log("Emotes: ShowMoveListP0 visible")
        end
    end)
    if open_picker then
        safe_call(ui, "OnEmotesButtonP0")
        log("Emotes: OnEmotesButtonP0 called")
    end
end

local function enable_skin_selector_hooks()
    if #skin_sel_hooks > 0 then return end

    local PM_PATH  = "/Game/Panda_Main/UI/PreMatch/UI_PreMatch.UI_PreMatch_C:"
    local SEL_PATH = "/Game/Panda_Main/UI/PreMatch/UI_PM_SkinSelector.UI_PM_SkinSelector_C:"

    -- Hook 1: OnSkinSelectorFocused on UI_PreMatch_C
    local ok1, pre1, post1 = pcall(RegisterHook, PM_PATH .. "OnSkinSelectorFocused",
        function(self, player_index_param, skin_data_param)
            local skin = nil
            pcall(function() skin = skin_data_param:get() end)
            if not skin or not skin:IsValid() then return end
            log("SkinFocused -> " .. skin:GetFullName())
            apply_skin_to_preview(0, skin, true)
        end,
        function() end
    )
    if ok1 and pre1 then
        table.insert(skin_sel_hooks, {path=PM_PATH.."OnSkinSelectorFocused", pre=pre1, post=post1})
        log("SkinSelector: OnSkinSelectorFocused hooked")
    else
        log("SkinSelector: OnSkinSelectorFocused hook FAILED: " .. tostring(pre1))
    end

    -- Hook 2: OnSkinSelectorSelected on UI_PreMatch_C
    local ok2, pre2, post2 = pcall(RegisterHook, PM_PATH .. "OnSkinSelectorSelected",
        function(self, player_index_param, skin_data_param, is_locked_param)
            local skin = nil
            pcall(function() skin = skin_data_param:get() end)
            if not skin or not skin:IsValid() then return end
            confirmed_skin = skin
            log("SkinSelected -> " .. skin:GetFullName())
            apply_skin_to_preview(0, skin, true)
        end,
        function() end
    )
    if ok2 and pre2 then
        table.insert(skin_sel_hooks, {path=PM_PATH.."OnSkinSelectorSelected", pre=pre2, post=post2})
        log("SkinSelector: OnSkinSelectorSelected hooked")
    else
        log("SkinSelector: OnSkinSelectorSelected hook FAILED: " .. tostring(pre2))
    end

    -- Hook 3: OnSkinFocused on UI_PreMatch_C
    local ok3, pre3, post3 = pcall(RegisterHook, PM_PATH .. "OnSkinFocused",
        function(self, skin_param, player_index_param)
            local skin = nil
            pcall(function() skin = skin_param:get() end)
            if not skin or not skin:IsValid() then return end
            log("OnSkinFocused -> " .. skin:GetFullName())
            apply_skin_to_preview(0, skin, true)
        end,
        function() end
    )
    if ok3 and pre3 then
        table.insert(skin_sel_hooks, {path=PM_PATH.."OnSkinFocused", pre=pre3, post=post3})
        log("SkinSelector: OnSkinFocused hooked")
    else
        log("SkinSelector: OnSkinFocused hook FAILED: " .. tostring(pre3))
    end

    -- Hook 4: OnPlayerPreviewedSkin on UI_PreMatch_C
    local ok4, pre4, post4 = pcall(RegisterHook, PM_PATH .. "OnPlayerPreviewedSkin",
        function(self, account_id_param, skin_param)
            local skin = nil
            pcall(function() skin = skin_param:get() end)
            if skin and skin:IsValid() then
                log("OnPlayerPreviewedSkin -> " .. skin:GetFullName())
            end
        end,
        function() end
    )
    if ok4 and pre4 then
        table.insert(skin_sel_hooks, {path=PM_PATH.."OnPlayerPreviewedSkin", pre=pre4, post=post4})
        log("SkinSelector: OnPlayerPreviewedSkin hooked")
    else
        log("SkinSelector: OnPlayerPreviewedSkin hook FAILED: " .. tostring(pre4))
    end

    -- FIX 1 continued: Added nil/validity guard on selector before reading skin.
    -- Hook 5: skinList FocusedEvent on UI_PM_SkinSelector_C (live hover preview)
    local ok5, pre5, post5 = pcall(RegisterHook,
        SEL_PATH .. "BndEvt__skinList_K2Node_ComponentBoundEvent_0_FocusedEvent__DelegateSignature",
        function(self, grid_item_param, player_index_param)
            -- Guard: selector widget must be valid before we do anything
            local selector = nil
            pcall(function() selector = self:get() end)
            if not selector or not selector:IsValid() then return end

            local grid_item = nil
            pcall(function() grid_item = grid_item_param:get() end)

            local skin = extract_skin_from_grid_item(grid_item, selector)
            if not skin then return end

            log("SkinHover (grid FocusedEvent) -> " .. skin:GetFullName())
            apply_skin_to_preview(0, skin, true)
        end,
        function() end
    )
    if ok5 and pre5 then
        table.insert(skin_sel_hooks, {
            path = SEL_PATH .. "BndEvt__skinList_K2Node_ComponentBoundEvent_0_FocusedEvent__DelegateSignature",
            pre  = pre5,
            post = post5,
        })
        log("SkinSelector: skinList FocusedEvent hooked (live hover preview ON)")
    else
        log("SkinSelector: skinList FocusedEvent hook FAILED: " .. tostring(pre5))
        log("  -> Falling back to OnSkinSelectorFocused / OnSkinFocused (hooks 1 & 3)")
    end
end

local function disable_skin_selector_hooks()
    for _, e in ipairs(skin_sel_hooks) do
        pcall(UnregisterHook, e.path, e.pre, e.post)
    end
    skin_sel_hooks = {}
    log("SkinSelector: hooks removed")
end

-- ============================================================
-- Play a named WidgetAnimation on the UI_PreMatch_C widget.
-- ============================================================
local function play_ui_anim(ui, anim_label)
    if not ui or not ui:IsValid() then return end
    local anim_obj = nil
    local prop_name = anim_label .. "_INST"
    pcall(function() anim_obj = ui[prop_name] end)
    if anim_obj and anim_obj:IsValid() then
        local ok, err = pcall(function()
            ui:PlayAnimation(anim_obj, 0.0, 1, 0, false)
        end)
        if ok then
            log("PlayAnimation: " .. anim_label .. " OK")
        else
            log("PlayAnimation: " .. anim_label .. " FAILED: " .. tostring(err))
        end
    else
        log("PlayAnimation: anim '" .. prop_name .. "' not found on UI_PreMatch_C")
    end
end

-- ============================================================
-- Character Info panel driver
-- ============================================================
local function get_all_char_info_widgets()
    local widgets = FindAllOf("UI_PM_CharacterInfo_V2_C") or {}
    local valid = {}
    for _, w in ipairs(widgets) do
        local ok, v = pcall(function() return w:IsValid() end)
        if ok and v then table.insert(valid, w) end
    end
    return valid
end

local function drive_char_info(widget, char_data, delay)
    if not widget or not widget:IsValid() then return end
    if not char_data or not char_data:IsValid() then return end
    delay = delay or 0.0
    local ok1 = safe_call(widget, "SetCharacter", char_data)
    local ok2 = safe_call(widget, "UpdateAppearance")
    safe_call(widget, "SetMinimal", false)
    safe_call(widget, "SetVisibility", 0)
    if ok1 then
        safe_call(widget, "Enter", delay)
        log("CharInfo: Enter(delay=" .. tostring(delay) .. ") for " .. char_data:GetFullName())
    else
        log("CharInfo: SetCharacter failed, skipping Enter")
    end
end

-- ============================================================
-- Perk Loadout visibility driver
-- ============================================================
local function show_perk_loadouts(ui)
    if not ui or not ui:IsValid() then return end

    local loadout_names = {
        "UI_Perks_LoadoutP0",
        "UI_Perks_LoadoutP1",
        "UI_Perks_LoadoutP2",
        "UI_Perks_LoadoutP3",
    }
    for _, name in ipairs(loadout_names) do
        local w = nil
        pcall(function() w = ui[name] end)
        if w and w:IsValid() then
            pcall(function() w:SetVisibility(0) end)
            log("Perk loadout visible: " .. name)
        end
    end

    local pc = nil
    pcall(function() pc = ui["PerksContainer"] end)
    if pc and pc:IsValid() then
        pcall(function() pc:SetVisibility(0) end)
        log("PerksContainer visible")
    end

    local phb = nil
    pcall(function() phb = ui["PerksHorizontalBox"] end)
    if phb and phb:IsValid() then
        pcall(function() phb:SetVisibility(0) end)
    end

    safe_call(ui, "ShowPerksNow")
    safe_call(ui, "LayoutCharacterInfos")
    safe_call(ui, "ApplyPerkLocations")
    safe_call(ui, "ApplyPlayerInfoLocations")
    log("Perk layout events fired.")
end

-- ============================================================
-- Full "match found" reveal sequence
-- ============================================================
local confirmed_char_data = {}

local function run_match_reveal_sequence()
    local ui = get_prematch_ui()
    if not ui or not ui:IsValid() then
        log("Reveal: no UI_PreMatch_C, aborting")
        return
    end

    log("Reveal: starting match-found animation sequence")

    safe_call(ui, "MatchFoundState")

    ExecuteWithDelay(100, function()
        play_ui_anim(ui, "TeamHeaderEnterAnim")
    end)

    ExecuteWithDelay(750, function()
        local char_widgets = get_all_char_info_widgets()
        log("Reveal: found " .. #char_widgets .. " UI_PM_CharacterInfo_V2_C widget(s)")

        for idx, cw in ipairs(char_widgets) do
            local slot = idx - 1
            local cd = confirmed_char_data[slot]
            if not cd then
                if slot == 0 and last_focused_item and last_focused_item:IsValid() then
                    pcall(function()
                        local c = last_focused_item.Character
                        if c and c:IsValid() then cd = c end
                    end)
                end
            end
            if cd and cd:IsValid() then
                local enter_delay = slot * 0.15
                drive_char_info(cw, cd, enter_delay)
            else
                safe_call(cw, "Enter", slot * 0.15)
                log("Reveal: no CharData for slot " .. slot .. ", calling Enter only")
            end
        end

        local pm = get_prematch_actor()
        if pm then
            safe_call(pm, "BPI_SequenceEvent", "AnimIn")
        end
        safe_call(ui, "BPI_SequenceEvent", "AnimIn")
    end)

    ExecuteWithDelay(1500, function()
        show_perk_loadouts(ui)
        play_ui_anim(ui, "NewAnimation")
    end)

    ExecuteWithDelay(3000, function()
        local pm = get_prematch_actor()
        if pm then
            safe_call(pm, "OnAnimInComplete")
            safe_call(pm, "BPI_TransitionComplete", "AnimIn")
        end
        log("Reveal: animation sequence complete")
    end)
end

-- ============================================================
-- Cursor visibility helpers
-- ============================================================
local cursor_visible = false

local function set_cursor(visible)
    local pc = get_lobby_pc()
    if pc and pc:IsValid() then
        pcall(function() pc.bShowMouseCursor       = visible end)
        pcall(function() pc.bEnableClickEvents     = visible end)
        pcall(function() pc.bEnableMouseOverEvents = visible end)
        log("Cursor " .. (visible and "ON" or "OFF"))
    end
end

local function toggle_cursor()
    cursor_visible = not cursor_visible
    set_cursor(cursor_visible)
end

-- ============================================================
-- Transition / match-start helpers
-- ============================================================
local function inject_skin_data(pc)
    safe_call(pc, "BPI_PreMatchSkinData", {}, {}, {}, {}, {}, {})
end

local function show_players_2v2()
    local pm = get_prematch_actor()
    if not pm then log("ERROR: PreMatch actor not found.") return end
    safe_call(pm, "BPI_ShowPlayer", 0, true)
    safe_call(pm, "BPI_ShowPlayer", 1, true)
    safe_call(pm, "BPI_ShowPlayer", 2, false)
    safe_call(pm, "BPI_ShowPlayer", 3, false)
    log("ShowPlayer called for all 4 slots (2v2).")
end

local function fake_anim_complete()
    local pm = get_prematch_actor()
    if not pm then return end
    safe_call(pm, "OnAnimInComplete")
    safe_call(pm, "BPI_TransitionComplete", "AnimIn")
    log("OnAnimInComplete + BPI_TransitionComplete fired.")
end

local function trigger_transition(mode)
    local pc = get_lobby_pc()
    if not pc then log("ERROR: No LobbyPlayerController found.") return end
    inject_skin_data(pc)
    ExecuteWithDelay(200, function()
        safe_call(pc, "BPI_PreMatchTransition", mode)
        local pm = get_prematch_actor()
        if pm then
            safe_call(pm, "PlayTransition", mode)
            if mode == "FFA" then safe_call(pm, "FFASeamless") end
        end
        log(mode .. " transition fired.")
    end)
end

-- ============================================================
-- OFFLINE CHARACTER SELECTION INTERCEPT
-- ============================================================
local char_select_hook_active = false
local registered_hooks        = {}

local pending_char_data  = nil
local pending_player_idx = 0

local function try_hook(path, pre_fn, post_fn)
    local ok, pre_id, post_id = pcall(RegisterHook, path,
        pre_fn  or function() end,
        post_fn or function() end
    )
    if ok and pre_id then
        table.insert(registered_hooks, {path=path, pre=pre_id, post=post_id})
        log("Hooked: " .. path)
        return true
    end
    return false
end

local function find_start_transition_path()
    local pc = get_lobby_pc()
    if not pc or not pc:IsValid() then return nil end
    local class_full = nil
    pcall(function()
        local cls = pc:GetClass()
        if cls and cls:IsValid() then class_full = cls:GetFullName() end
    end)
    if class_full then
        local asset_path = class_full:match("%S+ (/[^%s]+)$")
        if asset_path then return asset_path .. ":StartTransition" end
    end
    return nil
end

local function enable_char_select_intercept()
    registered_hooks = {}

    try_hook(
        "/Game/Panda_Main/UI/PreMatch/UI_PreMatch.UI_PreMatch_C:OnCharacterGridSelected",
        function(self, grid_item_param, player_index_param)
            local ui = self:get()
            if not ui or not ui:IsValid() then return end
            pcall(function() pending_player_idx = player_index_param:get() end)
            local item = nil
            pcall(function() item = grid_item_param:get() end)
            if item and item:IsValid() then
                pcall(function()
                    local c = item.Character
                    if c and c:IsValid() then
                        pending_char_data = c
                        log("Grid PRE char: " .. c:GetFullName())
                    end
                end)
            end
        end,
        function()
            log("Grid POST: character selected (lock simulation disabled)")
        end
    )

    local BASE = "/Game/Panda_Main/UI/PreMatch/UI_PreMatch.UI_PreMatch_C:"
    local diag_fns = {
        "OnCharacterSelectedBase",
        "OnPlayerLockedCharacter",
        "OnCharacterFocused",
        "OnCharacterConfirmed",
    }
    local diag_ok = 0
    for _, fn in ipairs(diag_fns) do
        local ok = try_hook(BASE .. fn,
            function() log("PRE  >> " .. fn) end,
            function() log("POST >> " .. fn) end
        )
        if ok then diag_ok = diag_ok + 1 end
    end
    log("Diagnostic hooks registered: " .. diag_ok .. "/" .. #diag_fns)

    local st_candidates = {
        find_start_transition_path(),
        "/Game/Panda_Main/Lobby/LobbyPlayerController.LobbyPlayerController_C:StartTransition",
        "/Game/Panda_Main/UI/PreMatch/LobbyPlayerController.LobbyPlayerController_C:StartTransition",
        "/Game/Panda_Main/LobbyPlayerController.LobbyPlayerController_C:StartTransition",
    }
    for _, path in ipairs(st_candidates) do
        if path then
            local ok = try_hook(path,
                function() end,
                function()
                    log("StartTransition POST detected (no lock sim)")
                end
            )
            if ok then break end
        end
    end

    char_select_hook_active = true
    log("Char-select intercept ENABLED (" .. #registered_hooks .. " hooks)")
end

local function disable_char_select_intercept()
    for _, e in ipairs(registered_hooks) do
        pcall(UnregisterHook, e.path, e.pre, e.post)
    end
    registered_hooks = {}
    char_select_hook_active = false
    log("Char-select intercept DISABLED")
end

-- ============================================================
-- Character preview hook
-- ============================================================
local preview_hook_active = false
local preview_pre_id      = nil
local preview_post_id     = nil
local last_focused_char   = nil
local last_focused_item   = nil

local function on_char_item_graph(self)
    local item = self:get()
    if not item or not item:IsValid() then return end

    local char_data = nil
    pcall(function()
        local c = item.Character
        if c and c:IsValid() then char_data = c end
    end)
    if not char_data then return end

    local full_name = char_data:GetFullName()
    if not full_name:find("^CharacterData ") then return end

    if full_name == last_focused_char then return end

    last_focused_char = full_name
    last_focused_item = item
    log("Character focused: " .. full_name)
    set_preview_character(0, char_data, true)

    ExecuteWithDelay(100, function()
        local char_widgets = get_all_char_info_widgets()
        for _, cw in ipairs(char_widgets) do
            if cw:IsValid() then
                safe_call(cw, "SetCharacter", char_data)
                safe_call(cw, "UpdateAppearance")
                safe_call(cw, "SetMinimal", false)
                safe_call(cw, "SetVisibility", 0)
            end
        end
    end)
end

local function enable_preview_hook()
    if preview_hook_active then return end
    local ok, pre, post = pcall(RegisterHook,
        "/Game/Panda_Main/UI/PreMatch/UI_PM_CharacterItem.UI_PM_CharacterItem_C:ExecuteUbergraph_UI_PM_CharacterItem",
        on_char_item_graph,
        function() end
    )
    if ok and pre then
        preview_pre_id      = pre
        preview_post_id     = post
        preview_hook_active = true
        log("Preview hook ON")
    else
        log("Preview hook registration failed: " .. tostring(pre))
    end
end

local function disable_preview_hook()
    if preview_pre_id then
        pcall(UnregisterHook,
            "/Game/Panda_Main/UI/PreMatch/UI_PM_CharacterItem.UI_PM_CharacterItem_C:ExecuteUbergraph_UI_PM_CharacterItem",
            preview_pre_id, preview_post_id
        )
        preview_pre_id      = nil
        preview_post_id     = nil
        preview_hook_active = false
        log("Preview hook OFF")
    end
end

-- ============================================================
-- Anti-flicker hooks
-- ============================================================
local VIS_VISIBLE   = 0
local VIS_COLLAPSED = 1

local flicker_hooks  = {}
local flicker_active = false

local flicker_targets = {
    "/Game/Panda_Main/UI/Prototype/Common/UI_Waiting.UI_Waiting_C:ExecuteUbergraph_UI_Waiting",
    "/Game/Panda_Main/UI/Prototype/Common/UI_Waiting.UI_Waiting_C:Construct",
}

local function enable_flicker_hooks()
    for _, path in ipairs(flicker_targets) do
        local ok, pre, post = pcall(RegisterHook, path,
            function(self)
                local obj = self:get()
                if obj and obj:IsValid() then
                    pcall(function() obj:SetVisibility(VIS_COLLAPSED) end)
                end
            end,
            function() end
        )
        if ok and pre then
            table.insert(flicker_hooks, {path=path, pre=pre, post=post})
        end
    end
    local ws = FindAllOf("UI_Waiting_C") or {}
    for _, w in ipairs(ws) do
        if w:IsValid() then pcall(function() w:SetVisibility(VIS_COLLAPSED) end) end
    end
    flicker_active = true
    log("Anti-flicker ON (" .. #flicker_hooks .. " hooks)")
end

local function disable_flicker_hooks()
    for _, e in ipairs(flicker_hooks) do
        pcall(UnregisterHook, e.path, e.pre, e.post)
    end
    flicker_hooks  = {}
    flicker_active = false
    log("Anti-flicker OFF")
end

-- ============================================================
-- Kill spinner (F5)
-- ============================================================
local function kill_spinner()
    log("F5: Reset to state 0...")
    local ws = FindAllOf("UI_Waiting_C") or {}
    for _, w in ipairs(ws) do
        if w:IsValid() then pcall(function() w:SetVisibility(VIS_COLLAPSED) end) end
    end
    local ss = FindAllOf("UI_Spinner_C") or {}
    for _, s in ipairs(ss) do
        if s:IsValid() then pcall(function() s:SetVisibility(VIS_COLLAPSED) end) end
    end
    local pms = FindAllOf("UI_PreMatch_C") or {}
    for _, w in ipairs(pms) do
        if w:IsValid() then
            pcall(function() w:SetState(0, "") end)
            pcall(function() w.MatchFound            = true end)
            pcall(function() w.AreNamesReady         = true end)
            pcall(function() w.HasPlayedLoadingState = true end)
        end
    end
    local ui = get_prematch_ui()
    if ui then close_skin_selector(ui) end
    disable_skin_selector_hooks()

    f8_flow_active  = false
    f8_flow_phase   = 0
    confirmed_char_data = {}
    confirmed_skin  = nil
    disable_preview_hook()
    last_focused_char = nil
    last_focused_item = nil
    log("F5: State 0 -- ready for F8.")
end

-- ============================================================
-- Debug dump (F6)
-- ============================================================
local function debug_dump()
    log("=== DEBUG DUMP ===")
    local pm = get_prematch_actor()
    if pm then
        log("  PreMatch_C: " .. pm:GetFullName())
        for slot = 0, 3 do
            local actor = get_preview_actor(pm, slot)
            if actor then
                log("  PreviewActor[" .. slot .. "]: " .. actor:GetFullName())
                local ds = nil
                local sm = nil
                pcall(function() ds = actor.DesiredSkin end)
                pcall(function() sm = actor.Comp_Pawn_SkinManager end)
                log("    DesiredSkin:       " ..
                    (ds and ds:IsValid() and ds:GetFullName() or "nil"))
                log("    Comp_Pawn_SkinMgr: " ..
                    (sm and sm:IsValid() and "valid" or "NIL  <-- crash risk!"))
            else
                log("  PreviewActor[" .. slot .. "]: NOT FOUND")
            end
        end
    else
        log("  PreMatch_C: NOT FOUND")
    end

    local pc = get_lobby_pc()
    log("  LobbyPC: " .. (pc and pc:GetFullName() or "NOT FOUND"))

    local ui = get_prematch_ui()
    if ui and ui:IsValid() then
        local st   = 0
        local acct = ""
        pcall(function() st   = ui.State end)
        pcall(function() acct = tostring(ui["Account ID"] or "") end)
        log("  UI_PreMatch state:  " .. tostring(st))
        log("  Account ID:        '" .. acct .. "'")
        local sel_len = 0
        pcall(function() sel_len = #ui.Selections end)
        log("  Selections length: " .. tostring(sel_len))

        local sel = get_skin_selector_widget(ui)
        if sel and sel:IsValid() then
            local focused_skin = nil
            pcall(function() focused_skin = sel["Focused Skin Data"] end)
            log("  SkinSelector 'Focused Skin Data': " ..
                (focused_skin and focused_skin:IsValid() and focused_skin:GetFullName() or "nil"))
        end
    else
        log("  UI_PreMatch_C: NOT FOUND")
    end

    local char_widgets = get_all_char_info_widgets()
    log("  UI_PM_CharacterInfo_V2_C count: " .. #char_widgets)
    for i, cw in ipairs(char_widgets) do
        if cw:IsValid() then
            log("    [" .. i .. "] " .. cw:GetFullName())
        end
    end

    log("  CharSelect intercept: " .. tostring(char_select_hook_active))
    log("  Registered hooks:     " .. #registered_hooks)
    log("  Preview hook active:  " .. tostring(preview_hook_active))
    log("  SkinSelector active:  " .. tostring(skin_selector_active))
    log("  SkinSelector hooks:   " .. #skin_sel_hooks)
    log("  F8 flow phase:        " .. tostring(f8_flow_phase))
    log("  Confirmed skin:       " ..
        (confirmed_skin and confirmed_skin:IsValid() and confirmed_skin:GetFullName() or "none"))
    log("  Pending char: " ..
        (pending_char_data and pending_char_data:IsValid()
         and pending_char_data:GetFullName() or "none"))
    log("  Confirmed chars:")
    for slot, cd in pairs(confirmed_char_data) do
        log("    slot " .. slot .. ": " ..
            (cd and cd:IsValid() and cd:GetFullName() or "nil"))
    end

    local all_skins = FindAllOf("SkinData") or {}
    log("  Loaded SkinData count: " .. #all_skins)
    local cache_hits, cache_misses = 0, 0
    for _, v in pairs(skin_cache) do
        if v then cache_hits = cache_hits + 1
        else cache_misses = cache_misses + 1 end
    end
    log("  Skin cache: " .. cache_hits .. " resolved, " .. cache_misses .. " misses")

    if last_focused_item and last_focused_item:IsValid() then
        local cd = nil
        pcall(function() cd = last_focused_item.Character end)
        if cd and cd:IsValid() then
            local slug = char_to_slug(cd)
            log("  Last hovered: " .. cd:GetFullName() ..
                " (slug='" .. slug .. "')")
            local skin = find_skin_for_slug(slug)
            log("  Best SkinData: " ..
                (skin and skin:GetFullName() or "NONE"))
        end
    else
        log("  Last hovered: none")
    end
    log("=== END DUMP ===")
end

-- ============================================================
-- BotPlay launch
-- ============================================================
local BOT_MapPool = {
    "Map_batcave","map_classic_3_platform_1v1","map_classic_3_platform_2v2",
    "map_m008","map_m009","map_m009_1v1","map_M010","map_m011",
    "map_m011_largenoplat","map_m011_small","map_m011_smallnoplat",
    "Map_tree_house_1v1","map_tree_house_2v2_1_platform",
    "map_trophy_room_2_platform","map_trophy_room_large_platform",
}
local BOT_CharPool = {
    "character_bugs_bunny","character_superman","character_shaggy",
    "character_batman","character_wonder_woman","character_harleyquinn",
    "character_finn","character_jake","character_garnet","character_steven",
    "character_arya","character_taz","character_c16","character_c020",
    "character_c019","character_c023b","character_c018","character_c021",
    "character_creature","character_velma","character_c017","character_c023a",
    "character_c020b","character_tom_and_jerry",
}
local BOT_NamePool = {
    "I BOT 1 MILLION TOASTS!!!","Professional Loser","Definitely Not A Bot",
    "Touch Grass","GG EZ","Your Worst Nightmare","I Woke Up Like This",
    "Skill Issue","Free Real Estate","Uninstall Please","Average Enjoyer",
    "No Thoughts Head Empty","Living Rent Free","404 Skill Not Found",
    "Certified Menace","Just A Visitor","Error 404","Speedrun Any%",
    "Capn Botticus","Sir Bottsalot","Madam Botz","Billy Bob Bot Jr",
    "Billy Bob Bot Sr","Botso","I Bot This Account","Capn Obvius",
    "MoustachedWar","OxfordComma","JDzX","BereBery","Dotso","Laney03",
    "CentBox","Gabler","Golemri","Linget","Prattin","SoccerLyfe",
    "TrackerRoz","Boltex","Crawlerildr","Gemmagy","Haymisab","LummoAbove",
    "Revini","Stegoty","weeddi","Briconia","Dinged","Godatro","LawnExtra",
    "MessagesWitch","Scannoyer","TenPrecise","GoodPlayer223","Everma",
    "Lindebasi","Plotiona","Shardis","TigerBoosh","BanditFix","Inextsoft55",
    "Medtershe","RollXan","Washton","Amesiani","BoostMura","Insides",
    "NearlyPool97","ScoobyWow","Truestem","Bracess","LessChronos",
    "NeoRadiant","UnderWa","BristleKemp","TheSnail2","PapaHeadline",
    "RealMore","SpunkyBroadway","xVengeans","Forumenti","Nanosakim",
    "Studison","Bentlor","CowPow1","BeefyCheesy","Landerne","Percsha",
    "Solidgene","TacticAngles","Wizewiz5","Callarts","Hondatash",
    "Lightshma","Pinkin","Spydersans","Talenta","UntamedLlama",
    "QuotePerson87","Stonefire","Biocalo","Currica","Borgizen","Florrekko",
    "Lentiva","MrWar","TigerNees","DragonQ","Godanque","Pongle557",
    "UpforceSign","McNephew","Pandee","Venuest9","SweetieDown",
    "TwinkleStarSprite","TickleMeBatman","Casualte42","JoshXoo","Postic",
    "DatingSimEsports101","NaanViolence09","MotoHead","InAMeeting",
    "AlexUhPlayDespotSeeToe","BloodbathAndBeyond","1v1 Me","CapnTanktop",
    "8BitPoultry","UnstoppableMenbun","GoatOnaMission","SheepOfFury",
    "UntamedSheep","GoatAteMyHomework","AlarminglyPeppy","GooglyEyeballs",
    "BruhBruh","ReinDoge","SaladCat","WhichButtonIsTaunt","HardcoreCasual",
    "NoJons","ItsMonday",
}

local BOT_Players = {
    {Enabled=true,  Char="character_bugs_bunny",  Name="MVSB+ Tester",  Team=0, IsBot=false, BotDifficulty=4, MinDifficulty=0.5, MaxDifficulty=0.8},
    {Enabled=true,  Char="character_superman",    Name="Player 2",      Team=1, IsBot=true,  BotDifficulty=4, MinDifficulty=3.0, MaxDifficulty=3.0},
    {Enabled=false, Char="character_shaggy",      Name="Bot 1",         Team=1, IsBot=true,  BotDifficulty=4, MinDifficulty=0.5, MaxDifficulty=0.8},
    {Enabled=false, Char="character_batman",      Name="Bot 2",         Team=1, IsBot=true,  BotDifficulty=4, MinDifficulty=0.5, MaxDifficulty=0.8},
    {Enabled=false, Char="character_wonder_woman",Name="Player 5",      Team=0, IsBot=false, BotDifficulty=4, MinDifficulty=0.5, MaxDifficulty=0.8},
    {Enabled=false, Char="character_harley_quinn",Name="Player 6",      Team=1, IsBot=false, BotDifficulty=4, MinDifficulty=0.5, MaxDifficulty=0.8},
}

local function BOT_RandomChar()
    return BOT_CharPool[math.random(#BOT_CharPool)]
end
local function BOT_RandomName()
    return BOT_NamePool[math.random(#BOT_NamePool)]
end
local BOT_GameMode  = 0
local BOT_TeamStyle = 0
local BOT_Ringouts  = 3
local BOT_MatchTime = 420

local function BOT_BPCall(obj, name, ...)
    local ok, err = pcall(function(...) obj[name](...) end, ...)
    if not ok then log("BotPlay " .. name .. ": " .. tostring(err)) end
    return ok
end

local function BOT_RandomMap()
    math.randomseed(os.time())
    return BOT_MapPool[math.random(#BOT_MapPool)]
end

local function BOT_GetOrCreateFacilitator(GI)
    local Fac = GI.LocalPlayFacilitator
    if Fac and Fac:IsValid() then return Fac end
    Fac = FindFirstOf("LocalPlayFacilitator_C")
    if Fac and Fac:IsValid() then return Fac end
    local ClassObj = StaticFindObject("/Game/Panda_Main/Blueprints/LocalPlay/LocalPlayFacilitator.LocalPlayFacilitator_C")
    if not ClassObj or not ClassObj:IsValid() then
        ClassObj = FindFirstOf("LocalPlayFacilitator_C")
        if ClassObj and ClassObj:IsValid() then ClassObj = ClassObj:GetClass() end
    end
    if not ClassObj or not ClassObj:IsValid() then
        log("BotPlay: Could not find LocalPlayFacilitator_C class")
        return nil
    end
    local ok, newFac = pcall(function() return StaticConstructObject(ClassObj, GI) end)
    if not ok or not newFac or not newFac:IsValid() then
        log("BotPlay: StaticConstructObject failed: " .. tostring(newFac))
        return nil
    end
    pcall(function() newFac.GameInstance = GI end)
    pcall(function() GI.LocalPlayFacilitator = newFac end)
    log("BotPlay: LocalPlayFacilitator_C constructed")
    return newFac
end

local function BOT_ApplyTeamsAndBots()
    ExecuteWithDelay(2000, function()
        local GI = FindFirstOf("PandaGameInstance_C")
        if not GI or not GI:IsValid() then log("BotPlay: No GameInstance"); return end

        local PDM = GI.PlayerDataManager
        if not PDM or not PDM:IsValid() then log("BotPlay: No PlayerDataManager"); return end

        local AllMPD = FindAllOf("MatchPlayerData_C") or {}
        log("BotPlay: Found " .. #AllMPD .. " total MatchPlayerData instances")

        local enabledCount = 0
        for i = 1, #BOT_Players do
            if BOT_Players[i].Enabled then enabledCount = enabledCount + 1 end
        end

        local recentMPDs = {}
        local startIdx = math.max(1, #AllMPD - enabledCount + 1)
        for i = startIdx, #AllMPD do
            local MPD = AllMPD[i]
            if MPD and MPD:IsValid() then
                table.insert(recentMPDs, MPD)
            end
        end
        log("BotPlay: Processing " .. #recentMPDs .. " most recent MatchPlayerData instances")

        for i = 1, #recentMPDs do
            local MPD = recentMPDs[i]
            local idx = MPD.PlayerIndex
            local cfg = nil
            local ci = 1
            for j = 1, #BOT_Players do
                if BOT_Players[j].Enabled then
                    if ci - 1 == idx then cfg = BOT_Players[j]; break end
                    ci = ci + 1
                end
            end
            if cfg then
                local ok, err = pcall(function()
                    MPD:AssignPlayerAndTeamIndex(idx, cfg.Team)
                end)
                if ok then
                    log("BotPlay: Player " .. idx .. " -> Team " .. cfg.Team)
                else
                    log("BotPlay: Team assignment failed: " .. tostring(err))
                end
                if cfg.IsBot then
                    local bok, berr = pcall(function()
                        MPD.isBot            = true
                        MPD.BotBehavior      = cfg.BotDifficulty
                        MPD.MinBotDifficulty = cfg.MinDifficulty
                        MPD.MaxBotDifficulty = cfg.MaxDifficulty
                        MPD.BotFromDisconnection = false
                    end)
                    if bok then
                        log("BotPlay: Player " .. idx .. " -> bot written")
                    else
                        log("BotPlay: Bot write failed: " .. tostring(berr))
                    end
                end
            else
                log("BotPlay: No config for player " .. idx)
            end
        end
        log("BotPlay: ApplyTeamsAndBots done")
    end)
end

local BOT_IsStarting = false

local function BOT_StartMatch(hoveredChar)
    if BOT_IsStarting then return end
    BOT_IsStarting = true

    local GI = FindFirstOf("PandaGameInstance_C")
    if not GI or not GI:IsValid() then
        log("BotPlay: No PandaGameInstance_C")
        BOT_IsStarting = false; return
    end
    local PM = GI.PreferencesManager
    if not PM or not PM:IsValid() then
        log("BotPlay: No PreferencesManager")
        BOT_IsStarting = false; return
    end
    local Fac = BOT_GetOrCreateFacilitator(GI)
    if not Fac then
        log("BotPlay: No facilitator")
        BOT_IsStarting = false; return
    end

    local SelectedMap = BOT_RandomMap()
    log("BotPlay: map=" .. SelectedMap .. " char=" .. tostring(hoveredChar))

    BOT_BPCall(PM, "Set-LocalPlay-GameMode",  BOT_GameMode)
    BOT_BPCall(PM, "Set-LocalPlay-TeamStyle", BOT_TeamStyle)
    BOT_BPCall(PM, "Set-LocalPlay-Time",      BOT_MatchTime)
    BOT_BPCall(PM, "Set-LocalPlay-Ringouts",  BOT_Ringouts)
    BOT_BPCall(PM, "Set-LocalPlay-Hazards",   true)
    BOT_BPCall(PM, "Set-LocalPlay-Maps",      { SelectedMap })

    math.randomseed(os.time())
    local playerIndex = 0
    local humanDone = false
    for i = 1, #BOT_Players do
        if BOT_Players[i].Enabled then
            local ch   = BOT_Players[i].Char
            local name = BOT_Players[i].Name
            if BOT_Players[i].IsBot then
                ch   = BOT_RandomChar()
                name = BOT_RandomName()
            elseif not humanDone and hoveredChar then
                ch = hoveredChar
                humanDone = true
            end
            BOT_BPCall(PM, "Set-LocalPlay-CharacterForPlayer", playerIndex, ch)
            BOT_BPCall(PM, "Set-LocalPlay-UsernameForPlayer",  playerIndex, name)
            log("BotPlay: slot " .. playerIndex .. " -> " .. ch .. " / " .. name)
            playerIndex = playerIndex + 1
        end
    end

    Fac = GI.LocalPlayFacilitator or Fac
    local ok, err = pcall(function() Fac:BeginMatch() end)
    if ok then
        log("BotPlay: BeginMatch OK")
        BOT_ApplyTeamsAndBots()
    else
        log("BotPlay: BeginMatch failed: " .. tostring(err))
    end

    BOT_IsStarting = false
    f8_flow_active = false
end

local function bot_get_last_hovered_char_name()
    if last_focused_item and last_focused_item:IsValid() then
        local cd = nil
        pcall(function() cd = last_focused_item.Character end)
        if cd and cd:IsValid() then
            local full = cd:GetFullName()
            local folder = full:match("/Characters/([^/]+)/")
            if folder then
                local slug = folder:lower()
                slug = CHAR_SLUG_OVERRIDES[slug] or slug
                return "character_" .. slug
            end
            local asset = full:match("/([^/]+)%.[^/]+$")
            if asset then return asset:lower() end
        end
    end
    return nil
end

-- ============================================================
-- State 9 watchdog
-- ============================================================
local state9_watchdog_active = false
local state9_hooks           = {}

local function state9_watchdog()
    if not state9_watchdog_active then return end

    local pms = FindAllOf("UI_PreMatch_C") or {}
    for _, w in ipairs(pms) do
        if w:IsValid() then
            pcall(function() w.RequestedMatchmakingCancel = true end)
            pcall(function() w.IsPreppingForBots          = true end)
            pcall(function() w.MatchFound                 = true end)
            pcall(function() w.AreNamesReady              = true end)
            pcall(function() w.HasPlayedLoadingState      = true end)
            pcall(function() w:StopAllActivities() end)
        end
    end

    ExecuteWithDelay(1000, state9_watchdog)
end

local function enable_state9_hooks()
    local BASE = "/Game/Panda_Main/UI/PreMatch/UI_PreMatch.UI_PreMatch_C:"
    local block_fns = {
        "OnMatchmakingTimeout",
        "CancelMatchmakingNoResponse",
        "OnInvalidMatchDetected",
        "OnPreMatchError",
        "AbandonMatch",
        "OnMatchmakingCancel",
        "OnStartMatchmakingError",
    }
    for _, fn in ipairs(block_fns) do
        local ok, pre, post = pcall(RegisterHook, BASE .. fn,
            function()
                log("State9: BLOCKED " .. fn)
                return false
            end,
            function() end
        )
        if ok and pre then
            table.insert(state9_hooks, {path=BASE..fn, pre=pre, post=post})
            log("State9: hooked " .. fn)
        end
    end
end

local function disable_state9_hooks()
    for _, e in ipairs(state9_hooks) do
        pcall(UnregisterHook, e.path, e.pre, e.post)
    end
    state9_hooks = {}
    log("State9: hooks removed")
end

local function start_state9_watchdog()
    if state9_watchdog_active then return end
    state9_watchdog_active = true
    enable_state9_hooks()
    log("State9 watchdog: STARTED")
    state9_watchdog()
end

local function stop_state9_watchdog()
    state9_watchdog_active = false
    disable_state9_hooks()
    log("State9 watchdog: STOPPED")
end

-- ============================================================
-- Guided state flow (F8)
-- ============================================================
local f8_flow_active  = false
local f8_flow_phase   = 0

local function do_set_state(state)
    local pms = FindAllOf("UI_PreMatch_C") or {}
    for _, w in ipairs(pms) do
        if w:IsValid() then
            pcall(function() w:SetState(state, "") end)
        end
    end
    log("F8: SetState(" .. state .. ")")
end

local function cycle_state()
    if f8_flow_phase == 3 then
        log("F8: Launch already in progress, ignoring")
        return
    end

    local pms = FindAllOf("UI_PreMatch_C") or {}
    if #pms == 0 then log("F8: UI_PreMatch_C not found") return end

    if f8_flow_phase == 0 and not bot_get_last_hovered_char_name() then
        log("F8: No character hovered -- setting state 1 (character select)")
        do_set_state(1)
        ExecuteWithDelay(1000, function()
            last_focused_char = nil
            last_focused_item = nil
            enable_preview_hook()
            f8_flow_phase = 1
            log("F8 [phase 1]: Hover a character, then press F8 to confirm it.")
        end)
        return
    end

    if f8_flow_phase <= 1 then
        local hoveredChar = bot_get_last_hovered_char_name()
        if not hoveredChar then
            log("F8: Still no character hovered, waiting...")
            return
        end

        local cd = nil
        if last_focused_item and last_focused_item:IsValid() then
            pcall(function()
                local c = last_focused_item.Character
                if c and c:IsValid() then cd = c end
            end)
        end
        if cd then
            confirmed_char_data[0] = cd
            log("F8 [phase 2]: Char confirmed: " .. cd:GetFullName())
        end

        disable_preview_hook()
        confirmed_skin = nil

        do_set_state(2)
        enable_skin_selector_hooks()

        local ui = get_prematch_ui()
        open_skin_selector(ui, cd)

        f8_flow_phase = 2
        log("F8 [phase 2]: Skin selector open. Hover skins for live preview. Press F8 to lock in.")
        return
    end

    if f8_flow_phase == 2 then
        local ui = get_prematch_ui()
        close_skin_selector(ui)
        disable_skin_selector_hooks()

        if not confirmed_skin then
            local cd = confirmed_char_data[0]
            if cd and cd:IsValid() then
                local slug = char_to_slug(cd)
                confirmed_skin = find_skin_for_slug(slug)
                log("F8 [phase 3]: No skin selected, using default for '" .. slug .. "'")
            end
        end

        if confirmed_skin and confirmed_skin:IsValid() then
            apply_skin_to_preview(0, confirmed_skin, true)
            log("F8 [phase 3]: Confirmed skin: " .. confirmed_skin:GetFullName())
        end

        f8_flow_phase = 3
        f8_flow_active = true

        log("F8 [phase 3]: confirm(3s) -> ready+animations(6s) -> state9 -> launch")

        ExecuteWithDelay(3000, function()
            do_set_state(5)
            log("F8: State 5 (ready) -- running reveal animation sequence...")
            run_match_reveal_sequence()

            ExecuteWithDelay(6000, function()
                do_set_state(9)
                start_state9_watchdog()
                f8_flow_active = false
                local ch = bot_get_last_hovered_char_name()
                log("F8: State 9 -- launching with char: " .. tostring(ch))
                ExecuteWithDelay(4000, function()
                    stop_state9_watchdog()
                    BOT_StartMatch(ch)
                    f8_flow_phase = 0
                end)
            end)
        end)
        return
    end
end

-- ============================================================
-- Auto SetState(0) on PreMatch map load
-- ============================================================
local prematch_state_reset_fired = false
local was_in_prematch_map        = false

local function apply_state0()
    local pms = FindAllOf("UI_PreMatch_C") or {}
    local done = false
    for _, w in ipairs(pms) do
        if w:IsValid() then
            pcall(function() w:SetState(0, "") end)
            pcall(function() w.MatchFound            = true end)
            pcall(function() w.AreNamesReady         = true end)
            pcall(function() w.HasPlayedLoadingState = true end)
            done = true
        end
    end
    local ws = FindAllOf("UI_Waiting_C") or {}
    for _, w in ipairs(ws) do
        if w:IsValid() then pcall(function() w:SetVisibility(VIS_COLLAPSED) end) end
    end
    if done then log("Auto: SetState(0) applied") end
    return done
end

local function prematch_map_poll()
    local pm = get_prematch_actor()
    local ui = get_prematch_ui()
    local in_prematch = (pm ~= nil) or (ui ~= nil)

    if in_prematch and not was_in_prematch_map then
        was_in_prematch_map        = true
        prematch_state_reset_fired = false
        f8_flow_active             = false
        f8_flow_phase              = 0
        confirmed_char_data        = {}
        confirmed_skin             = nil
        log("Auto: PreMatch map entered -- applying state 0. Press F8 to begin.")
        local attempts = 0
        local function try()
            attempts = attempts + 1
            if apply_state0() then return end
            if attempts < 30 then ExecuteWithDelay(300, try) end
        end
        try()
    elseif not in_prematch and was_in_prematch_map then
        was_in_prematch_map        = false
        prematch_state_reset_fired = false
        f8_flow_active             = false
        f8_flow_phase              = 0
        confirmed_char_data        = {}
        confirmed_skin             = nil
        stop_state9_watchdog()
        disable_skin_selector_hooks()
        disable_preview_hook()
        last_focused_char = nil
        last_focused_item = nil
        log("Auto: Left PreMatch map -- all state reset.")
    end

    ExecuteWithDelay(300, prematch_map_poll)
end
ExecuteWithDelay(600, prematch_map_poll)

-- ============================================================
-- Keybinds
-- ============================================================
RegisterKeyBind(Key.F1,  function() toggle_cursor() end)
RegisterKeyBind(Key.F5,  function() kill_spinner() end)
RegisterKeyBind(Key.F6,  function() debug_dump() end)
RegisterKeyBind(Key.F7,  function()
    log("F7: Triggering 2v2...")
    trigger_transition("2v2")
    ExecuteWithDelay(800,  show_players_2v2)
    ExecuteWithDelay(4000, fake_anim_complete)
end)
RegisterKeyBind(Key.F8,  function() cycle_state() end)
RegisterKeyBind(Key.F9,  function()
    if preview_hook_active then
        disable_preview_hook()
        last_focused_char = nil
    else
        enable_preview_hook()
    end
end)
RegisterKeyBind(Key.F10, function()
    if flicker_active then disable_flicker_hooks()
    else enable_flicker_hooks() end
end)
RegisterKeyBind(Key.F11, function()
    local ui = get_prematch_ui()
    if not ui then log("F11: no PreMatch UI found") return end
    show_emotes(ui, true)
    log("F11: emote picker triggered")
end)

-- ============================================================
-- Session heartbeat (every 10s)
-- ONLY resets known-safe properties. All speculative method calls
-- (KeepAlive, Heartbeat, RefreshSession, etc.) have been removed
-- because they don't exist on these objects and crash via a C++
-- nullptr before Lua's pcall can catch it.
-- The actual timeout fix is: zero out every timer property we can
-- find, plus the LocalPlayFacilitator's MatchElapsedTime.
-- ============================================================
local function session_heartbeat()
    -- GameInstance timer properties
    local GI = FindFirstOf("PandaGameInstance_C")
    if GI and GI:IsValid() then
        pcall(function() GI.IdleTime          = 0 end)
        pcall(function() GI.InactiveTime      = 0 end)
        pcall(function() GI.TimeSinceActivity = 0 end)
        pcall(function() GI.IdleKickTimer     = 0 end)
        pcall(function() GI.DisconnectTimer   = 0 end)
        pcall(function() GI.SessionTimeout    = 0 end)
        pcall(function() GI.TimeoutTimer      = 0 end)

        -- SessionManager timer properties (no method calls)
        local SM = nil
        pcall(function() SM = GI.SessionManager end)
        if SM and SM:IsValid() then
            pcall(function() SM.IdleTime          = 0 end)
            pcall(function() SM.InactiveTime      = 0 end)
            pcall(function() SM.TimeSinceActivity = 0 end)
            pcall(function() SM.TimeoutTimer      = 0 end)
            pcall(function() SM.DisconnectTimer   = 0 end)
        end

        -- NetworkManager timer properties (no method calls)
        local NM = nil
        pcall(function() NM = GI.NetworkManager end)
        if NM and NM:IsValid() then
            pcall(function() NM.IdleTime     = 0 end)
            pcall(function() NM.TimeoutTimer = 0 end)
        end

        -- LocalPlayFacilitator: zero MatchElapsedTime so the
        -- local match session doesn't think it has been running too long.
        local Fac = nil
        pcall(function() Fac = GI.LocalPlayFacilitator end)
        if not Fac or not Fac:IsValid() then
            pcall(function() Fac = FindFirstOf("LocalPlayFacilitator_C") end)
        end
        if Fac and Fac:IsValid() then
            pcall(function() Fac.MatchElapsedTime    = 0 end)
            pcall(function() Fac.SessionTimeoutTimer = 0 end)
            pcall(function() Fac.DisconnectTimer     = 0 end)
            pcall(function() Fac.IdleKickTimer       = 0 end)
        end
    end

    -- GameState timer properties
    local GS = FindFirstOf("PandaGameState_C")
    if GS and GS:IsValid() then
        pcall(function() GS.SessionTimeoutTimer = 0 end)
        pcall(function() GS.DisconnectTimer     = 0 end)
        pcall(function() GS.IdleKickTimer       = 0 end)
    end

    -- Keep PreMatch UI flags healthy
    local pms = FindAllOf("UI_PreMatch_C") or {}
    for _, w in ipairs(pms) do
        if w:IsValid() then
            pcall(function() w.MatchFound            = true end)
            pcall(function() w.AreNamesReady         = true end)
            pcall(function() w.HasPlayedLoadingState = true end)
        end
    end

    ExecuteWithDelay(10000, session_heartbeat)
end
ExecuteWithDelay(3000, session_heartbeat)

-- ============================================================
-- Startup
-- ============================================================
ExecuteWithDelay(500, function()
    cursor_visible = true
    set_cursor(true)
    log("Cursor shown on startup.")
    enable_flicker_hooks()
    enable_char_select_intercept()
end)

log("Mod loaded. v5 -- skin selector crash fix + session timeout fix.")
log("F1=Cursor  F5=KillSpinner/Reset  F6=Dump  F7=2v2")
log("F8 flow: [1] state1+charSelect -> [hover char, 2] state2+skinSelector")
log("         -> [hover skins for LIVE preview, 3] confirm -> state9 -> launch")
log("F9=PreviewHook manual toggle  F10=AntiFlicker  F11=EmotePicker")
