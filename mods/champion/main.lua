-- champion (Wandering Champion): every so often, while you are outdoors, a champion appears 80-150 m away and roams.
-- The HUD shows how far it is and in which direction; slay it for a bonus. It leaves after a while, at dawn, or when
-- everyone is far away. A gameplay mod in Lua (an EMF example): change this file while the game runs, EMF reloads it.
-- Runs where the world runs (the host or a dedicated server). Commands: champion.where, champion.stats (every player,
-- also from the game's chat), champion.summon, champion.dismiss, champion.kill (host).

-- The champions. Edit the table and save: the mod reloads. Only templates seen to wake and fight when spawned away from
-- their camp belong here: some spawn asleep (the heavy wolf mini-boss did, Enemy_Wildbeast_Wolf_Heavy_BossHealthBar).
-- min_level / max_level: the player levels a champion comes for (the lowest band when none fits).
local CHAMPIONS = {
  {name = "Vukah Brawler", guid = "9c8e2008-7259-4114-b6fc-615f0f181c04", min_level = 1, max_level = 99},
}

-- The slayer's bonus on top of the champion's own loot (items by their debug names: `items rune` in the console). The
-- reward_item setting adds one more (gear at the slayer's level and the reward_rarity setting).
local REWARDS = {
  {item = "Material_T1_Token_WeaponUpgrade_Rune01", count = 10},
}

-- Which way is north on the map, as a direction in the world (x, z): a guess until checked in game; if the HUD says
-- north for what the map calls east, change these two lines and save.
local NORTH = {x = 0, z = 1}
local EAST = {x = 1, z = 0}

local MARKER = "0ce28db1-522f-4455-b358-3888fb5e089b"  -- what it guards while it patrols: a small barrel (the companion's)
local NEAR, FAR = 80, 150       -- metres from the player it appears at
local ENGAGE, LET_GO = 30, 60   -- a player this close is hunted; it goes back to its patrol past LET_GO
local GONE_FAR = 300            -- farther than this from every player: it leaves
local HOP = 60                  -- seconds at each patrol spot
local BASE_RADIUS = 40          -- "outdoors" = farther than this from every flame altar

emf.emits({"StartGuardingEvent", "AddThreatTargetEvent"})

local champ = nil       -- {id, marker, name, level, until_tick, spots, spot, hop_at, hunting, threat_at}
local crumbs = {}       -- spots players walked on outdoors {x, y, z}: ground at a known height
local altars = {}       -- flame altars {x, y, z}
local altars_at = -1e9
local slain = {}        -- account -> champions slain (this world)
local loaded = false    -- this world's data arrived
local session = emf.session()  -- the hosted session this file was loaded in (a reload keeps it: the champion stays)
local outdoors_s = 0    -- seconds a player has been outdoors (at night with night_only) since the last chance
local last_s = nil
local last_hitter = nil

local function now_ticks(world) return world:tick() end
local function dist(a, b) return math.sqrt((a.x - b.x) ^ 2 + (a.y - b.y) ^ 2 + (a.z - b.z) ^ 2) end
local function flat(a, b) return math.sqrt((a.x - b.x) ^ 2 + (a.z - b.z) ^ 2) end

local function pos(world, id)
  local x, y, z = world:position(id)
  if not x then return nil end
  return {x = x, y = y, z = z}
end

local function level_of(world, id)
  local l = world:get("Level", id)
  return l and l.dataStorage[1] or 1
end

-- A new hosted session (another world, a reload of the world): its champion went with the old one (spawns are not
-- saved) and the ids mean something else now. Called before anything uses an id.
local save
local function same_session()
  if session == emf.session() then return end
  session = emf.session()
  champ, crumbs, outdoors_s, last_s, altars_at, last_hitter = nil, {}, 0, nil, -1e9, nil
  emf.hud("track", "")
  save()  -- no stale "live" line stays for a later reload to misread
end

-- "N", "NE", ... from `from` towards `to`.
local function compass(from, to)
  local dx, dz = to.x - from.x, to.z - from.z
  local n = dx * NORTH.x + dz * NORTH.z
  local e = dx * EAST.x + dz * EAST.z
  local names = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"}
  local a = math.deg(math.atan(e, n)) % 360
  return names[math.floor((a + 22.5) / 45) % 8 + 1]
end

local function outdoors(p)
  for _, a in ipairs(altars) do
    if flat(p, a) <= BASE_RADIUS then return false end
  end
  return true
end

local function is_night()
  local h = emf.time_of_day()
  return h ~= nil and (h >= 18 or h < 6)
end

-- The players in this world (host and guests; a dedicated server has no local player): id, position, level, alive.
local function players(world)
  local out = {}
  for id = 1, 1023 do
    if world:account(id) and world:get("PlayerState", id) then
      local p = pos(world, id)
      local health = world:get("NetworkHealth", id)
      if p then out[#out + 1] = {id = id, pos = p, level = level_of(world, id), alive = not health or health.health > 0} end
    end
  end
  return out
end

local function nearest(list, p, within, living_only)
  local best, best_d = nil, within or math.huge
  for _, pl in ipairs(list) do
    local d = dist(pl.pos, p)
    if d <= best_d and (pl.alive or not living_only) then best, best_d = pl, d end
  end
  return best, best_d
end

save = function()
  local lines = {}
  local keys = {}
  for k in pairs(slain) do keys[#keys + 1] = k end
  table.sort(keys)
  for _, k in ipairs(keys) do lines[#lines + 1] = "slain " .. k .. " " .. slain[k] end
  if champ then  -- a reload of this file in the same run and session takes it over again
    local m = champ.marker_at or {x = 0, y = 0, z = 0}
    lines[#lines + 1] = string.format("live %s %d %d %d %d %s %.1f %.1f %.1f", emf.session_token(), champ.id,
      champ.marker or 0, champ.until_tick, champ.level, (champ.name:gsub(" ", "_")), m.x, m.y, m.z)
  end
  emf.save_world_data(table.concat(lines, "\n") .. "\n")
end

local function guard(who, whom)
  emf.emit("server", "StartGuardingEvent", {guardingEntityId = who, guardedEntityId = whom})
end

local hud_shown = false  -- the line is on screen (cleared when no hosted world runs any more)
local client_steps = 0   -- client world steps since the last server world step

local function show(world)
  if not champ or not emf.setting("hud") then
    emf.hud("track", "")
    hud_shown = false
    return
  end
  local me = pos(world, emf.local_player())
  local at = pos(world, champ.id)
  if not me or not at then return end
  hud_shown = true
  emf.hud("track", string.format("Champion: %s (level %d) - %d m %s", champ.name, champ.level, math.floor(flat(me, at) + 0.5),
    compass(me, at)), emf.setting("corner"), emf.setting("color"))
end

-- Removes the patrol marker if it is still the barrel we put down (a smashed one's id may belong to something else now).
local function drop_marker(world)
  if not champ or not champ.marker or champ.marker == 0 then return end
  local at = pos(world, champ.marker)
  if at and (not champ.marker_at or flat(at, champ.marker_at) < 3) then emf.destroy(world, champ.marker) end
  champ.marker = 0
end

local function forget(world, why)
  drop_marker(world)
  champ = nil
  last_hitter = nil
  emf.hud("track", "")
  if why then
    emf.notify(why)
    emf.log(why)
  end
  save()
end

-- Removes the champion (it leaves): the entity and its patrol marker.
local function despawn(world, why)
  if champ and pos(world, champ.id) then emf.destroy(world, champ.id) end
  forget(world, why)
end

local function pick_template(level)
  local fit = {}
  for _, c in ipairs(CHAMPIONS) do
    if level >= (c.min_level or 1) and level <= (c.max_level or 99) then fit[#fit + 1] = c end
  end
  if #fit > 0 then return fit[math.random(#fit)] end
  local low = CHAMPIONS[1]
  for _, c in ipairs(CHAMPIONS) do
    if (c.min_level or 1) < (low.min_level or 1) then low = c end
  end
  return low
end

-- Where it appears: a spot a player walked on outdoors, near..far metres from `p` (a metre above it, so it lands);
-- else that far in a random direction at the player's height + 3 m.
local function spawn_spot(p, near, far)
  local fits = {}
  for _, c in ipairs(crumbs) do
    local d = flat(c, p)
    if d >= near and d <= far and outdoors(c) then fits[#fits + 1] = c end
  end
  if #fits > 0 then
    local c = fits[math.random(#fits)]
    return {x = c.x, y = c.y + 1, z = c.z}, true
  end
  for _ = 1, 16 do  -- a few random directions; never inside a base
    local a = math.random() * 2 * math.pi
    local d = near + math.random() * (far - near)
    local at = {x = p.x + d * math.cos(a), y = p.y + 3, z = p.z + d * math.sin(a)}
    if outdoors(at) then return at, false end
  end
  return nil, false
end

-- Patrol spots: up to 4 walked spots within 60 m of where it appeared, nearest first.
local function patrol_spots(at)
  local list = {}
  for _, c in ipairs(crumbs) do
    local d = flat(c, at)
    if d > 5 and d <= 60 and outdoors(c) then list[#list + 1] = {spot = c, d = d} end
  end
  table.sort(list, function(a, b) return a.d < b.d end)
  local out = {}
  for i = 1, math.min(4, #list) do out[i] = list[i].spot end
  return out
end

local function set_level(world, id, level)
  local l = world:get("Level", id)
  if not l then return false end
  l.dataStorage[1] = level  -- the level, then a rescale at once (as the companion does: health 119 -> 417 at 10)
  local s = world:get("DynamicEnemyScaling", id)
  if s then s.nextUpdateTime = 0 end
  return true
end

local function hop(world)
  if #champ.spots < 2 then  -- after a takeover (or with too few walked spots): look for spots near it again
    local at = pos(world, champ.id)
    champ.spots = at and patrol_spots(at) or {}
    champ.spot = 1
    if #champ.spots < 2 then
      champ.hop_at = now_ticks(world) + HOP * 60
      return
    end
  end
  drop_marker(world)
  local s = champ.spots[champ.spot]
  champ.marker = emf.spawn(world, MARKER, s.x, s.y + 0.5, s.z)
  champ.marker_at = {x = s.x, y = s.y + 0.5, z = s.z}
  if champ.marker ~= 0 then guard(champ.id, champ.marker) end
  emf.log(string.format("patrol: spot %d of %d at (%.1f, %.1f, %.1f), marker %d", champ.spot, #champ.spots, s.x, s.y, s.z,
    champ.marker))
  champ.spot = champ.spot % #champ.spots + 1
  champ.hop_at = now_ticks(world) + HOP * 60
  save()  -- the marker's id, for a reload of this file
end

-- A champion for `pl` now; false + why when it could not appear.
local function summon(world, pl, near, far)
  local t = pick_template(pl.level)
  if not t then return false, "the CHAMPIONS table is empty" end
  local at, walked = spawn_spot(pl.pos, near, far)
  if not at then
    return false, string.format("no place outside the bases (every spot %d-%d m away is within %d m of a flame altar)",
      math.floor(near), math.floor(far), BASE_RADIUS)
  end
  local id = emf.spawn(world, t.guid, at.x, at.y, at.z)
  if id == 0 then return false, "the game refused the spawn (see the log)" end
  local level = math.max(1, math.min(99, pl.level + emf.setting("level_bonus")))
  champ = {id = id, marker = 0, name = t.name, level = level, spots = patrol_spots(at), spot = 1, hop_at = 0,
           until_tick = now_ticks(world) + emf.setting("lifetime_minutes") * 3600, level_set = set_level(world, id, level),
           seen_night = is_night()}
  if #champ.spots >= 2 then hop(world) end
  last_hitter = nil
  local text = string.format("A champion roams: %s (level %d), %d m %s of you", t.name, level,
    math.floor(flat(pl.pos, at) + 0.5), compass(pl.pos, at))
  emf.notify(text)
  emf.log(string.format("%s: entity %d at (%.1f, %.1f, %.1f) %s, %d patrol spot(s)", text, id, at.x, at.y, at.z,
    walked and "on a walked spot" or "at the player's height + 3 m (no walked spot that far)", #champ.spots))
  save()
  show(world)
  return true
end

-- Puts a bonus into the slayer's inventory; what does not fit, or a put the game refuses, is dropped at their feet
-- (the game's own pickup), so nothing is lost.
local function give_bonus(world, player, id, count, opts)
  local queued = emf.put(world, player, id, count, function(w, added)
    if added < count then
      emf.log(string.format("bonus: %d of %d x item %d fitted; the rest dropped at player %d's feet", added, count, id, player))
      emf.give(w, player, id, count - added)
    end
  end, opts)
  if not queued then
    emf.log(string.format("bonus: the put of %d x item %d was refused; dropped at player %d's feet", count, id, player))
    emf.give(world, player, id, count)
  end
end

local function reward(world, player)
  local level = level_of(world, player)
  for _, r in ipairs(REWARDS) do
    local id = emf.item(r.item)
    if id then give_bonus(world, player, id, r.count) else emf.log("no item named " .. r.item .. " (REWARDS)") end
  end
  local name = emf.setting("reward_item")
  if name ~= "" then
    local id = emf.item(name)
    if id then
      local rarity = emf.setting("reward_rarity")
      local opts = {level = level}
      if rarity ~= "default" then opts.rarity = rarity end
      give_bonus(world, player, id, emf.setting("reward_count"), opts)
    else
      emf.log("no item named " .. name .. " (the reward_item setting)")
    end
  end
end

-- This world's data: who slew how many, and a champion still out from before a reload of this file.
emf.on_world_data(function(world, text)
  slain = {}
  for account, n in text:gmatch("slain (%S+) (%d+)") do slain[account] = tonumber(n) end
  loaded = true
  local token, id, marker, until_tick, level, name, mx, my, mz =
    text:match("live (%S+) (%d+) (%d+) (%d+) (%d+) (%S+) (%S+) (%S+) (%S+)")
  if not token then
    if text:find("live ") then save() end  -- an older line: never taken over, rewritten away
    return
  end
  id, marker, level = tonumber(id), tonumber(marker), tonumber(level)
  -- after a restart sessions start at 1 again and ids are renumbered: the token names this run and session; then who it
  -- is: alive (not a corpse) at the level we gave it
  local health = world:get("NetworkHealth", id)
  local alive = health and health.health > 0
  if token ~= emf.session_token() or not pos(world, id) or not alive or level_of(world, id) ~= level then
    emf.log("a saved champion (entity " .. id .. ") is not this run's or not that creature any more: not taken over")
    save()
    return
  end
  local marker_at = {x = tonumber(mx) or 0, y = tonumber(my) or 0, z = tonumber(mz) or 0}
  local m = marker ~= 0 and pos(world, marker)
  if not m or dist(m, marker_at) > 1 then marker = 0 end  -- the marker only while it is where the barrel was put
  champ = {id = id, marker = marker, marker_at = marker ~= 0 and marker_at or nil, name = name:gsub("_", " "),
           level = level, until_tick = tonumber(until_tick), spots = {}, spot = 1, hop_at = now_ticks(world),
           level_set = true, seen_night = is_night()}
  emf.log("took over champion " .. champ.id .. " after a reload")
end)

emf.on_event("HitEvent", function(world, hit)
  same_session()
  if champ and hit.targetId == champ.id then last_hitter = hit.rootSourceId end
end)

emf.on_event("EntityDiedEvent", function(world, e)
  same_session()
  if not champ or e.deadEntityId ~= champ.id then return end
  local list = players(world)
  local slayer = nil
  for _, pl in ipairs(list) do
    if pl.id == last_hitter then slayer = pl end
  end
  local at = pos(world, champ.id)
  if not slayer and at then slayer = nearest(list, at, 60) end  -- no player's hit (emf.kill, a companion): the nearest
  local name = champ.name
  if slayer then
    reward(world, slayer.id)
    local account = world:account(slayer.id) or ("entity:" .. slayer.id)
    slain[account] = (slain[account] or 0) + 1
    forget(world, string.format("%s slain by player %d!", name, slayer.id))
  else
    forget(world, name .. " is dead")
  end
end)

emf.on_tick(function(world)
  if world:is_client() then  -- in someone else's game (no hosted world steps): the line goes
    client_steps = client_steps + 1
    if client_steps > 180 and hud_shown then
      emf.hud("track", "")
      hud_shown = false
    end
    return
  end
  if not world:is_server() then return end
  client_steps = 0
  same_session()
  local t = now_ticks(world)
  local s = math.floor(t / 60)
  if s == last_s then return end  -- once a second
  local step = last_s and s - last_s or 0
  last_s = s
  if t - altars_at >= 3600 then  -- the altars, once a minute
    altars_at = t
    altars = {}
    for _, id in ipairs(world:entities()) do
      if world:get("BaseIdComponent", id) then altars[#altars + 1] = pos(world, id) end
    end
  end
  local list = players(world)
  local out = {}
  for _, pl in ipairs(list) do
    if outdoors(pl.pos) then
      out[#out + 1] = pl
      local keep = true
      for _, c in ipairs(crumbs) do
        if flat(c, pl.pos) < 5 then keep = false break end
      end
      if keep then
        crumbs[#crumbs + 1] = pl.pos
        if #crumbs > 200 then table.remove(crumbs, 1) end
      end
    end
  end
  if champ then
    local at = pos(world, champ.id)
    if not at then
      forget(world, "The champion is gone")
      return
    end
    if not champ.level_set then champ.level_set = set_level(world, champ.id, champ.level) end
    if is_night() then champ.seen_night = true end
    local near_pl, d = nearest(list, at)
    local prey, prey_d = nearest(list, at, nil, true)  -- never a dead player (seen in game: a raider then wandered off)
    if t >= champ.until_tick then
      despawn(world, "The champion has left")
    elseif emf.setting("night_only") and champ.seen_night and not is_night() and emf.time_of_day() ~= nil then
      despawn(world, "The champion fades with the dawn")
    elseif not near_pl or d > GONE_FAR then
      despawn(world, "The champion wandered off")
    else
      if champ.hunting and (not prey or prey_d > LET_GO or champ.hunting ~= prey.id) then  -- back to its patrol
        champ.hunting = nil
        if champ.marker and champ.marker ~= 0 then guard(champ.id, champ.marker) end
      end
      if prey and prey_d <= ENGAGE then
        if champ.hunting ~= prey.id then
          champ.hunting = prey.id
          guard(champ.id, prey.id)  -- it comes for the nearest living player
        end
        if t >= (champ.threat_at or 0) then
          champ.threat_at = t + 180
          emf.emit("server", "AddThreatTargetEvent", {attackerId = champ.id, targetId = prey.id, threatValue = 1000})
        end
      elseif t >= champ.hop_at then
        hop(world)
      end
      show(world)
    end
    return
  end
  -- no champion: count the time players spend outdoors (at night, with night_only), then roll the chance
  if #out == 0 or not loaded or (emf.setting("night_only") and not is_night()) then return end
  outdoors_s = outdoors_s + step
  if outdoors_s < emf.setting("interval_minutes") * 60 then return end
  outdoors_s = 0
  if math.random(100) > emf.setting("chance") then
    emf.log("no champion this time (chance " .. emf.setting("chance") .. "%)")
    return
  end
  local ok, why = summon(world, out[math.random(#out)], NEAR, FAR)
  if not ok then emf.log("no champion: " .. why) end
end)

-- The player a command means: a player in the chat, else the host.
local function sender(world)
  local who = emf.sender()
  local id = (who and who.chat) and who.entity or emf.local_player()
  if not id or id == 0 then return nil end
  local p = pos(world, id)
  return p and {id = id, pos = p, level = level_of(world, id)} or nil
end

emf.command("where", ": where the champion is, from you", function(world, args)
  same_session()
  local me = sender(world)
  local at = champ and pos(world, champ.id)
  if not at then
    emf.reply("No champion roams now")
    return
  end
  if not me then
    emf.reply(string.format("The champion (%s, level %d) roams, but EMF could not tell where you are", champ.name, champ.level))
    return
  end
  emf.reply(string.format("The champion (%s, level %d) is %d m %s of you", champ.name, champ.level,
    math.floor(flat(me.pos, at) + 0.5), compass(me.pos, at)))
end, {players = true})

emf.command("stats", ": champions slain in this world", function(world, args)
  local me = sender(world)
  local mine = me and slain[world:account(me.id) or ("entity:" .. me.id)] or 0
  local all = 0
  for _, n in pairs(slain) do all = all + n end
  emf.reply(string.format("Champions slain in this world: %d by you, %d in all", mine, all))
end, {players = true})

emf.command("summon", "[metres]: a champion now, near you (host, for tests)", function(world, args)
  same_session()
  local me = sender(world)
  if not me then
    emf.reply("your position is not known")
    return
  end
  if champ then despawn(world, nil) end
  local d = tonumber(args[1] or "")
  local ok, why
  if d then ok, why = summon(world, me, math.max(5, d - 10), d + 10) else ok, why = summon(world, me, NEAR, FAR) end
  emf.reply(ok and ("summoned " .. champ.name .. " (entity " .. champ.id .. ")") or ("not summoned: " .. why))
end)

emf.command("dismiss", ": the champion leaves now (host)", function(world, args)
  same_session()
  if not champ then
    emf.reply("No champion roams now")
    return
  end
  despawn(world, "The champion was dismissed")
  emf.reply("dismissed")
end)

emf.command("kill", ": the champion dies now (host, for tests: no hit, the nearest player is rewarded)", function(world, args)
  same_session()
  if not champ then
    emf.reply("No champion roams now")
    return
  end
  emf.reply(emf.kill(world, champ.id) and "the champion dies" or "it could not be killed (see the log)")
end)

emf.on_setting_changed(function(key)
  if key == "night_only" or key == "interval_minutes" then outdoors_s = 0 end
end)
