-- model_viewer: look at new creature models in the game (a developer tool for tools/models).
--   model_viewer.show <template guid> [metres]   spawns it that far in front of the camera (default 4 m), makes it an
--                                                ally (Faction 11) and asks it to guard you, so it stays in view
--   model_viewer.clear                           removes everything model_viewer spawned
-- Host only. Animal-AI templates (wildlife) ignore guarding and may still wander off; enemy-AI ones stay.
local PLAYER_ALLY = 11

--   model_viewer.foe <template guid> [ahead] [right]  spawns a creature as it is (hostile ones stay hostile)
--   model_viewer.attack                               every ally model_viewer spawned attacks the last foe
--   model_viewer.status                               health of every ally and foe it spawned
--   model_viewer.god [on|off]                         test god mode: your health is filled up every step (a single
--                                                     blow bigger than your health can still kill you)
--   model_viewer.hurt <percent>                       the last foe's health down to that percent (not a hit)
--   model_viewer.near [metres]                        the 12 nearest creatures: faction, health, level, scale
--   model_viewer.stats <id>                           Health, DamageMod, Movement, Level storage and scale of one
--   model_viewer.poke <id> <Component> <field> <v>    writes one field (experiments)
--   model_viewer.push <id> <metres>                   teleports a creature that far along +x (leash tests)
emf.emits({"StartGuardingEvent", "AddThreatTargetEvent"})

local spawned = {}
local foes = {}
local god = false
local session = emf.session()

emf.on_tick(function(world)
	if not god or not world:is_server() then return end
	local player = emf.local_player()
	if not player or player == 0 then return end
	local cur, max = emf.health(world, player)
	if cur and max and cur > 0 and cur < max then emf.attribute_add(world, player, "health", max - cur) end
end)

emf.command("god", "[on|off]: test god mode, your health is filled up every step", function(world, args)
	if args[1] == "on" then god = true elseif args[1] == "off" then god = false else god = not god end
	emf.reply("god mode " .. (god and "on" or "off"))
end)

local function same_session()
	if session ~= emf.session() then
		session = emf.session()
		spawned = {}
		foes = {}
	end
end

-- the camera's view direction on the ground plane: +z of the camera orientation (PlayerInput.fromClient.camera)
local function view_dir(world, player)
	local input = world:get("PlayerInput", player)
	if not input or not input.fromClient or not input.fromClient.camera then return nil end
	local q = input.fromClient.camera.orientation
	local fx = 2 * (q.x * q.z + q.w * q.y)
	local fz = 1 - 2 * (q.x * q.x + q.y * q.y)
	local h = math.sqrt(fx * fx + fz * fz)
	if h < 1e-6 then return nil end
	return fx / h, fz / h
end

-- spawn a template ahead of the camera (and `side` metres to the right); returns the id or nil
local function spawn_ahead(world, args)
	local guid = args[1]
	local dist = math.max(1, math.min(30, tonumber(args[2] or "4") or 4))
	local side = tonumber(args[3] or "0") or 0  -- metres to the right of the view line (game space: +x is right)
	if not guid then return nil, "usage: <template guid> [metres ahead] [metres right]" end
	local player = emf.local_player()
	local px, py, pz = world:position(player)
	if not px then return nil, "your position is not known" end
	local dx, dz = view_dir(world, player)
	if not dx then dx, dz = 1, 0 end
	local x, y, z = px + dx * dist + dz * side, py + 0.3, pz + dz * dist - dx * side
	local id = emf.spawn(world, guid, x, y, z)
	if not id or id == 0 then return nil, "the game refused the spawn (see the log)" end
	return id, string.format("%.1f %.1f %.1f (%.1f m ahead, view %.2f %.2f)", x, y, z, dist, dx, dz)
end

emf.command("show", "<template guid> [metres ahead 1-30] [metres right]: spawn it in front of the camera", function(world, args)
	same_session()
	if not world:is_server() then emf.reply("host only") return end
	local id, where = spawn_ahead(world, args)
	if not id then emf.reply(where) return end
	spawned[#spawned + 1] = id
	local f = world:get("Faction", id)
	if f then f.faction = PLAYER_ALLY end
	emf.emit("server", "StartGuardingEvent", {guardingEntityId = id, guardedEntityId = emf.local_player()})
	emf.reply("spawned " .. id .. " at " .. where)
end)

emf.command("foe", "<template guid> [metres ahead] [metres right]: spawn a creature as it is (a foe)", function(world, args)
	same_session()
	if not world:is_server() then emf.reply("host only") return end
	local id, where = spawn_ahead(world, args)
	if not id then emf.reply(where) return end
	foes[#foes + 1] = id
	emf.reply("foe " .. id .. " at " .. where)
end)

emf.command("attack", ": every ally model_viewer spawned attacks the last foe", function(world, args)
	same_session()
	local target = foes[#foes]
	if not target or not world:position(target) then emf.reply("no foe (model_viewer.foe first)") return end
	local n = 0
	for _, id in ipairs(spawned) do
		if world:position(id) then
			emf.emit("server", "AddThreatTargetEvent", {attackerId = id, targetId = target, threatValue = 1000, threatPoolId = 0})
			n = n + 1
		end
	end
	emf.reply(n .. " allies attack " .. target)
end)

emf.command("status", ": health of the allies and foes model_viewer spawned", function(world, args)
	same_session()
	local parts = {}
	local function add(kind, id)
		local cur, max = emf.health(world, id)
		local x, y, z = world:position(id)
		parts[#parts + 1] = string.format("%s %d %s/%s %s", kind, id, tostring(cur), tostring(max),
			x and string.format("(%.1f %.1f %.1f)", x, y, z) or "(gone)")
	end
	for _, id in ipairs(spawned) do add("ally", id) end
	for _, id in ipairs(foes) do add("foe", id) end
	emf.reply(#parts > 0 and table.concat(parts, " | ") or "nothing spawned")
end)

emf.command("hurt", "<percent>: the last foe's health down to that percent (not a hit: no aggro, no credit)", function(world, args)
	same_session()
	local target = foes[#foes]
	local pct = tonumber(args[1] or "")
	if not target or not world:position(target) then emf.reply("no foe (model_viewer.foe first)") return end
	if not pct then emf.reply("usage: model_viewer.hurt <percent>") return end
	local cur, max = emf.health(world, target)
	if not cur or not max then emf.reply("its health is not known") return end
	local want = math.floor(max * pct / 100)
	if cur > want then emf.attribute_add(world, target, "health", want - cur) end
	emf.reply(string.format("foe %d: %d -> %d of %d", target, cur, math.min(cur, want), max))
end)

emf.command("near", "[metres]: the 12 nearest creatures: id, faction, distance, health, level, scale", function(world, args)
	local r = tonumber(args[1] or "") or 15
	local player = emf.local_player()
	local px, py, pz = world:position(player)
	if not px then emf.reply("your position is not known") return end
	local found = {}
	for _, id in ipairs(world:entities()) do
		local f = world:get("Faction", id)
		local x, y, z = world:position(id)
		local cur, max = emf.health(world, id)
		if f and x and cur and id ~= player and not world:get("PlayerState", id) then  -- creatures (they have health)
			local d = math.sqrt((x - px) ^ 2 + (y - py) ^ 2 + (z - pz) ^ 2)
			if d <= r then
				local s = world:get("ScaleComponent", id)
				local lvl = world:get("Level", id)
				found[#found + 1] = { d = d, text = string.format("%d f%s %.0fm hp %s/%s lvl %s scale %s", id, tostring(f.faction),
					d, tostring(cur), tostring(max), lvl and tostring(lvl.dataStorage[1]) or "-",
					s and string.format("%.2f", s.uniformScale) or "-") }
			end
		end
	end
	table.sort(found, function(a, b) return a.d < b.d end)
	local lines = {}
	for i = 1, math.min(#found, 12) do lines[i] = found[i].text end  -- the 12 nearest: a console line has a limit
	emf.reply(#lines > 0 and table.concat(lines, " | ") or "nothing within " .. r .. " m")
end)

emf.command("stats", "<id>: a creature's Health, DynamicEnemyScaling, DamageMod, Movement and Level storage", function(world, args)
	local id = tonumber(args[1] or "")
	if not id then emf.reply("usage: model_viewer.stats <id>") return end
	local function storage(name, n)
		local c = world:get(name, id)
		if not c then return name .. " -" end
		local v = {}
		for i = 1, n do
			local x = c.dataStorage[i]
			if x == nil then break end
			v[#v + 1] = string.format("%.4g", x)
		end
		return name .. " " .. table.concat(v, " ")
	end
	local parts = { storage("Health", 8), storage("DamageMod", 4), storage("Movement", 10), storage("Level", 2) }
	local s = world:get("DynamicEnemyScaling", id)
	if s then
		parts[#parts + 1] = string.format("Scaling next %s lastLevel %d lastDmg %.3g baseHp %d str %d dex %d int %d",
			tostring(s.nextUpdateTime), s.lastLevel, s.lastDamageMultiplier, s.initialBaseMaxHealth, s.initialBaseStrength,
			s.initialBaseDexterity, s.initialBaseIntelligence)
	end
	local sc = world:get("ScaleComponent", id)
	parts[#parts + 1] = "Scale " .. (sc and string.format("%.2f", sc.uniformScale) or "-")
	emf.reply(table.concat(parts, " | "))
end)

emf.command("poke", "<id> <Component> <field or dataStorage.N> <value>: write one field (experiments)", function(world, args)
	local id, comp, field, value = tonumber(args[1] or ""), args[2], args[3], tonumber(args[4] or "")
	if not id or not comp or not field or not value then emf.reply("usage: model_viewer.poke <id> <Component> <field> <value>") return end
	local c = world:get(comp, id)
	if not c then emf.reply("no " .. comp .. " on " .. id) return end
	local name, index = field:match("^(%w+)%.(%d+)$")
	local ok, err = pcall(function()
		if name then c[name][tonumber(index)] = value else c[field] = value end
	end)
	emf.reply(ok and ("set " .. comp .. "." .. field .. " = " .. value) or tostring(err))
end)

emf.command("push", "<id> <metres>: move a creature that far along +x (the game's teleport), e.g. to test a leash", function(world, args)
	local id, m = tonumber(args[1] or ""), tonumber(args[2] or "")
	if not id or not m then emf.reply("usage: model_viewer.push <id> <metres>") return end
	local x, y, z = world:position(id)
	if not x then emf.reply("no entity " .. tostring(id)) return end
	emf.reply(emf.teleport(world, id, x + m, y + 1, z, { ground = true }) and "pushed" or "refused (see the log)")
end)

emf.command("clear", ": remove every ally and foe model_viewer spawned", function(world, args)
	same_session()
	local n = 0
	for _, list in ipairs({spawned, foes}) do
		for _, id in ipairs(list) do
			if world:position(id) then
				emf.destroy(world, id)
				n = n + 1
			end
		end
	end
	spawned, foes = {}, {}
	emf.reply("removed " .. n)
end)

-- Every hit to or from one creature, and its death: what hurt it, and who got the kill.
local watched, hits = nil, {}
emf.command("watch", "<id>: record every hit to or from this creature and its death (model_viewer.hits shows them)", function(world, args)
	watched = tonumber(args[1] or "")
	hits = {}
	emf.reply(watched and ("watching " .. watched) or "usage: model_viewer.watch <id>")
end)

emf.on_event("HitEvent", function(world, h)
	if not watched or (h.targetId ~= watched and h.rootSourceId ~= watched and h.sourceId ~= watched) then return end
	local cur = emf.health(world, watched)
	hits[#hits + 1] = string.format("%.1fs hit root %d src %d -> %d health %d flags 0x%x program %s (now %s)",
		world:tick() / 60, h.rootSourceId, h.sourceId, h.targetId, h.healthChange, h.flags, tostring(h.programId),
		tostring(cur))
end, "server")

emf.on_event("EntityDiedEvent", function(world, d)
	if watched and d.deadEntityId == watched then
		hits[#hits + 1] = string.format("%.1fs died", world:tick() / 60)
	end
end, "server")

emf.command("hits", ": the hits recorded by model_viewer.watch", function(world, args)
	emf.reply(#hits > 0 and table.concat(hits, " | ") or "no hits recorded")
end)
