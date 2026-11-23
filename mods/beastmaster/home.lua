-- Home: the beasts a player left at home (in their collection, not out) live in the Large Stable nearest them (the
-- beasts pack's; stable.lua HOMES) while the player is within HOME_M of it; they leave when the player is farther than
-- LEAVE_M. A Large Stable has room for ROOM beasts, all players together: the beasts shown stay, then the others by
-- account and list order. Server world only.
--   Each is spawned on a spot of its own on the stable's floor (SPOTS, in the stable's frame: x across, z out of its
--   open side, turned with the stable's CurrentTransform) as a player-side creature at its owner's level and its
--   loyalty's size, told to guard the stable (the game's own roaming near it), and turned to face out of the open side.
--   Scenes, in turns: a stroll (the game's roaming; the first FIRST_S, then STROLL_S) and a rest (REST_S, held in the
--   game's Sleep state as idle.lua holds a beast: one whose graph has sleep states lies down, the others stand still),
--   facing out. A stroll that ends off the floor puts the beast back on its spot before it rests; a beast ROAM_M or more
--   from the stable's middle for AWAY_S is put back at once (the game's teleport, with a ground search).
--   A beast the Sleep state cannot hold (no enemy AI state) strolls on.
--   A beast another mod walks somewhere (EMF move orders) is left alone while it walks: it gets up and is not put back,
--   turned or told to guard the stable; after the move, one left ROAM_M or more away is put back AWAY_S later.
--   At night (NIGHT_FROM to DAWN by the game's clock) they rest the whole night: a stroll ends at once, a rest goes on;
--   in the morning each rest ends when it was due, so they get up one by one.
--   Each carries the game's "E Pet" prompt (emf.offer; offered again every OFFER_EVERY_S while EMF no longer holds it).
--   A pet turns the beast to the player and settles it for PET_S (a rest); its owner's pet earns it a loyalty point
--   (env.gain) once a game day (the days are counted at each dawn the game's clock passes, while the world runs).
--   It never fights (its aggro and chosen targets are cleared every step). A hit on one of them, or a creature's hit on
--   a player within BUILD_M of their stable, sends that stable's beasts inside (out of the world) until SHELTER_S after
--   the last such hit. It has the health it has when called (its role, loyalty and talents: env.on_spawn) and is healed
--   to full every second; one that dies anyway is removed and shows up again after RESPAWN_S. Its owner loses nothing:
--   the collection is not touched.
--   They leave the world when they are called (leave, before the call's spawn), sent away or released, their owner is
--   away or left, the setting home_beasts is off, the stable is gone, or (with the setting make_room) a player builds
--   within BUILD_M of it (the game blocks building next to creatures); back GRACE_S after the building stops.
--   A creature is ours while it is there with the SpawnTime first read from it (the game gives ids out again: another
--   creature on the id is left alone); one of ours that is no longer player-side is taken out.
--   A kind the game cannot spawn (its pack is not installed), or whose creature does not become player-side, is tried
--   again after RETRY_S, logged once per kind.
--   The game's time (for the Sleep lease) is kept as an offset from env.now, taken again from each new home beast's
--   SpawnTime.
--   After the script's start (a reload) the player-side creatures running a beast's animation graph within ROAM_M of a
--   Large Stable's middle that are none of ours and guard no player (a beast out guards its owner) are the beasts the
--   script had there before: taken out of the world at the first check that has a Large Stable.
-- M.new(env [, place]) where env has: now(world), setting(key), beasts() (the roster), homes() (the Large Stables:
-- stable.lua S:homes()), present() (account -> player entity, nil before the first check), log(text), on_spawn(world,
-- id, sp, health_mul) (optional: the health watch), tell(world, player, text) and gain(world, account, beast) (a
-- loyalty point; both optional: a pet then only settles the beast).
-- `place` (optional; M.LARGE_STABLE by default) makes it serve another kind of place the same way (training.lua's
-- Training Grounds): name, log, room(env), spots (frame metres {x, z}), floor(lx, lz), spawn_y, roam_m, places(env)
-- (the places now), on(env) (shown at all), belongs(b) (a beast that may be shown at such a place), fits(b, key) (at
-- that place), key(h), scene(home, world, e, h, now, x, z) (optional: the place's own scenes instead of the stroll and
-- the rest), yaw(h, e) (optional: where a beast faces when put back), words(e) (optional: what it does, for text).
local species = require("species")
local loyalty = require("loyalty")
local idle_mod = require("idle")
local aside_mod = require("aside")
local pack_mod = require("pack")

local M = {}
local H = {}
H.__index = H

local PLAYER_ALLY = 11
local HOME_M, LEAVE_M = 50, 70  -- m from the player: their beasts show up in a Large Stable within HOME_M, leave beyond
local ROOM = 4
-- the spots on the Large Stable's floor (its frame, metres from its middle; the open side +z), side by side
local SPOTS = { { x = -1.75, z = 0.45 }, { x = -0.6, z = 0.45 }, { x = 0.6, z = 0.45 }, { x = 1.75, z = 0.45 } }
local FLOOR = { x0 = -2.3, x1 = 2.45, z0 = -1.4, z1 = 2.25 }  -- its plank floor between the walls (frame metres)
local SPAWN_Y = 0.5          -- m above the stable's place: a beast is spawned just above the floor (0.31 m high)
local ROAM_M, AWAY_S = 6, 3  -- a beast this far from the stable's middle for this long is put back on its spot
local FIRST_S = { 5, 12 }    -- s: the first stroll
local STROLL_S = { 15, 30 }  -- s: a stroll
local REST_S = { 40, 90 }    -- s: a rest
local JITTER = 20            -- degrees: a beast faces out of the open side give or take this
local FACE_TRIES = 3         -- a turn to face out that the game did not carry out is asked this often in all
local RESPAWN_S = 60
local RETRY_S = 300
local BUILD_M, GRACE_S = 20, 5
local SHELTER_S = 30  -- s after the last hit at a stable its beasts stay inside
local NIGHT_FROM, DAWN = 18, 6  -- the game's night: its clock's night start and day start (hours)
local PET_S = 20         -- s a pet settles a beast
local OFFER_EVERY_S = 10 -- s between two looks at a beast's Pet prompt (offered again when EMF no longer holds it)
local PET = { verb = "Pet" }
local LEASE_S, RENEW_S = 120, 5  -- the Sleep lease: the game's time plus LEASE_S, renewed every RENEW_S
local CHECK_AHEAD_NS = 1000000000000000000
local NS = 1000000000

function M.new(env, place)
	local h = setmetatable({ env = env, p = place or M.LARGE_STABLE }, H)
	h:reset()
	return h
end

-- A new world (or a reload): entity ids from before mean nothing now.
function H:reset()
	self.at = {}         -- beast (its roster entry) -> {id, beast, account, key, spot, scene, until_s, sc, faced,
	                     -- spawned, far, moved}
	self.ids = {}        -- entity id -> the same entry
	self.bodies = {}     -- a home beast's body -> when it is removed
	self.back_at = {}    -- beast -> when it may show again (it died)
	self.refused = {}    -- kind id -> until when it is not tried (the game refused its spawn)
	self.told = {}       -- kind id -> logged
	self.busy = {}       -- stable key -> until when building there keeps it empty
	self.clock = nil     -- the game's time (ns) minus env.now (s) x 10^9, once known
	self.next_check = 0
	self.swept = false
	self.day = 0         -- game days begun while the world runs (a dawn passed)
	self.last_hour = nil -- the game's clock at the last check
	self.night = false
	self.place_state = {}  -- what the place's own scenes keep (place.scene, place.after_check)
end

local function key_of(h)
	return string.format("%.1f %.1f %.1f", h.x, h.y, h.z)
end

-- The Large Stable: the place the beasts at home live in.
M.LARGE_STABLE = {
	name = "Large Stable",
	log = "home",
	room = function() return ROOM end,
	spots = SPOTS,
	floor = function(lx, lz) return lx >= FLOOR.x0 and lx <= FLOOR.x1 and lz >= FLOOR.z0 and lz <= FLOOR.z1 end,
	spawn_y = SPAWN_Y,
	roam_m = ROAM_M,
	places = function(env) return env.homes() end,
	on = function(env) return env.setting("home_beasts") end,
	belongs = function(b) return not b.out and not b.training end,  -- one in training is at its Training Grounds
	fits = function() return true end,
	key = key_of,
}

local function dist(h, x, y, z)
	return math.sqrt((h.x - x) ^ 2 + (h.y - y) ^ 2 + (h.z - z) ^ 2)
end

local function get(world, component, id)
	local ok, v = pcall(world.get, world, component, id)
	return ok and v or nil
end

-- A stable's turn as a unit quaternion (none, or not a turn: none).
local function turn_of(h)
	local q = h.q
	if not q then return 0, 0, 0, 1 end
	local n = math.sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w)
	if n < 0.5 then return 0, 0, 0, 1 end
	return q.x / n, q.y / n, q.z / n, q.w / n
end

-- (vx, vy, vz) turned by the quaternion (qx, qy, qz, qw).
local function rotate(qx, qy, qz, qw, vx, vy, vz)
	local tx, ty, tz = 2 * (qy * vz - qz * vy), 2 * (qz * vx - qx * vz), 2 * (qx * vy - qy * vx)
	return vx + qw * tx + (qy * tz - qz * ty), vy + qw * ty + (qz * tx - qx * tz), vz + qw * tz + (qx * ty - qy * tx)
end

-- The world place of the point (lx, lz) in place `h`'s frame, at `up` metres above it.
function M.at(h, lx, lz, up)
	local qx, qy, qz, qw = turn_of(h)
	local dx, _, dz = rotate(qx, qy, qz, qw, lx, 0, lz)
	return h.x + dx, h.y + (up or 0), h.z + dz
end

-- (x, z) in place `h`'s frame.
function M.local_of(h, x, z)
	local qx, qy, qz, qw = turn_of(h)
	local lx, _, lz = rotate(-qx, -qy, -qz, qw, x - h.x, 0, z - h.z)
	return lx, lz
end

-- The world place of spot `i` of place `h` (place kind `p`).
local function spot_at(p, h, i)
	return M.at(h, p.spots[i].x, p.spots[i].z, p.spawn_y)
end

-- Whether (x, z) is on place `h`'s floor.
local function on_floor(p, h, x, z)
	return p.floor(M.local_of(h, x, z))
end

-- The yaw (degrees; 0 = +z, 90 = +x) out of stable `h`'s open side, give or take JITTER.
local function yaw_out(h)
	local qx, qy, qz, qw = turn_of(h)
	local dx, _, dz = rotate(qx, qy, qz, qw, 0, 0, 1)
	return math.deg(math.atan(dx, dz)) + (math.random() - 0.5) * 2 * JITTER
end

local function between(span)
	return math.random(span[1], span[2])
end

-- A player-side creature (alive or a body) is there on the id.
local function ours(world, id)
	if not world:position(id) then return false end
	local f = get(world, "Faction", id)
	return f ~= nil and f.faction == PLAYER_ALLY
end

-- Entry `e`'s creature is still the one spawned: there, with the SpawnTime first read from it.
local function still(world, e)
	if not world:position(e.id) then return false end
	local s = get(world, "SpawnTime", e.id)
	local t = s and s.spawnTime or 0
	return not (e.stamp and t > 0 and t ~= e.stamp)
end

-- The animation graphs the beasts run (species.lua): a creature running none of them is no beast.
local function beast_graph(world, id)
	local ok, g = pcall(world.template, world, "AnimationGraphResourceComponent2", id)
	local graph = ok and g and g.animationGraph and tostring(g.animationGraph):lower()
	if not graph then return false end
	for _, sp in ipairs(species.list) do
		if sp.graph == graph or sp.also == graph then return true end
	end
	return false
end
local SPAWN_GRACE_S = 5  -- a creature spawned a moment ago reads as dead this long at most

-- Another mod walks creature `id` somewhere (EMF move orders, Lua 1.33).
local function walking(world, id)
	return emf.enemy_moving ~= nil and emf.enemy_moving(world, id) ~= nil
end

-- The game's time now in ns plus `plus_ns`, or nil while it is not known.
function H:game_now(world, plus_ns)
	if not self.clock then return nil end
	return math.floor(self.env.now(world) * NS + self.clock + plus_ns + 0.5)
end

local function guard(world, id, h)
	if h.entity then emf.emit("server", "StartGuardingEvent", { guardingEntityId = id, guardedEntityId = h.entity }) end
end

-- Turns entry `e`'s beast where it stands to face out of stable `h`'s open side: true when the game took it. One the
-- game took but did not carry out (its answer: failed) counts as not faced: the upkeep asks again, FACE_TRIES in all.
local function face(world, e, h, p)
	local x, y, z = world:position(e.id)
	if not (x and emf.teleport) then return false end
	e.tries = (e.tries or 0) + 1
	local yaw = p and p.yaw and p.yaw(h, e) or yaw_out(h)
	return emf.teleport(world, e.id, x, y, z, { yaw = yaw }, function(moved)
		if not moved then e.faced = false end
	end) == true
end

-- Puts entry `e`'s beast back on its spot of stable `h`, facing out.
function H:put_back(world, e, h, now)
	local x, y, z = spot_at(self.p, h, e.spot)
	local yaw = self.p.yaw and self.p.yaw(h, e) or yaw_out(h)
	if emf.teleport then emf.teleport(world, e.id, x, y, z, { ground = true, yaw = yaw }) end
	e.moved, e.far = now, nil
	e.faced = true
end

-- Entry `e`'s beast leaves the world (when it is still ours; an id the game gave out again is left alone).
function H:remove(world, e)
	if still(world, e) and ours(world, e.id) then emf.destroy(world, e.id) end
	self.at[e.beast], self.ids[e.id] = nil, nil
end

-- Kind `sp` is not tried again for RETRY_S (logged once per kind).
function H:refuse(sp, why, now)
	self.refused[sp.id] = now + RETRY_S
	if not self.told[sp.id] then
		self.told[sp.id] = true
		self.env.log(string.format("%s: %s for a %s (%s); tried again in %d s", self.p.log, why, self.p.name,
			species.word(sp), RETRY_S))
	end
end

-- A beast leaves its Large Stable now (it is called).
function H:leave(world, beast)
	local e = self.at[beast]
	if e then self:remove(world, e) end
end

-- Whether entity `id` is a beast at home.
function H:has(id)
	return self.ids[id] ~= nil
end

-- A HitEvent: a hit on a beast at home (from anything but itself), or a creature's hit on a player within BUILD_M of a
-- Large Stable, sends that stable's beasts inside until SHELTER_S after it (a fall has no source: nothing).
function H:on_hit(world, source, target)
	if not source or source == 0 or source == target then return end
	local now = self.env.now(world)
	local e = self.ids[target]
	if e then
		self.busy[e.key] = now + SHELTER_S
		self.next_check = 0
		return
	end
	if not get(world, "PlayerState", target) or get(world, "PlayerState", source) then return end
	local px, py, pz = world:position(target)
	if not px then return end
	for _, h in ipairs(self.p.places(self.env)) do
		if dist(h, px, py, pz) <= BUILD_M then
			self.busy[self.p.key(h)] = now + SHELTER_S
			self.next_check = 0
		end
	end
end

-- `player` pressed E at the Pet prompt on `entity`: the beast turns to them and settles for PET_S; its owner's first pet
-- of the game day earns it a loyalty point.
function H:pet(world, player, entity)
	local e = self.ids[entity]
	if not (e and still(world, e)) then return end
	local now = self.env.now(world)
	local b = e.beast
	local px, _, pz = world:position(player)
	local x, y, z = world:position(e.id)
	local walks = walking(world, e.id)  -- on another mod's move: not turned or settled
	if not walks and px and x and emf.teleport and (px - x) ^ 2 + (pz - z) ^ 2 > 0.01 then
		emf.teleport(world, e.id, x, y, z, { yaw = math.deg(math.atan(px - x, pz - z)) })
	end
	e.faced = true
	e.sc.turned = true
	local was_rest = e.scene == "rest"
	if not walks and idle_mod.hold(world, e.id, e.sc, now, self:game_now(world, LEASE_S * NS), self:game_now(world, CHECK_AHEAD_NS)) then
		e.until_s = was_rest and math.max(e.until_s, now + PET_S) or now + PET_S
		e.scene = "rest"
	end
	local tell = self.env.tell or function() end
	if world:account(player) ~= e.account then
		tell(world, player, string.format("%s enjoys it.", b.name))
	elseif b.petted ~= self.day and self.env.gain then
		b.petted = self.day
		self.env.gain(world, e.account, b)
		tell(world, player, string.format("%s enjoys it: a loyalty point.", b.name))
	else
		tell(world, player, string.format("%s enjoys it (a pet earns a loyalty point once a game day: again tomorrow).",
			b.name))
	end
end

-- A home beast died (the game's event): its body goes, the beast shows up again after RESPAWN_S.
function H:on_died(world, id)
	local e = self.ids[id]
	if not e then return end
	self.ids[id], self.at[e.beast] = nil, nil
	local now = self.env.now(world)
	self.back_at[e.beast] = now + RESPAWN_S
	self.bodies[id] = now + 1
end

-- Puts beast `b` of `account` (`owner` their player) on spot `spot` of stable `h`; the entry, or nil.
function H:show(world, b, account, owner, h, spot, now)
	local sp = species.by_id(b.species)
	if not sp then return nil end
	local x, y, z = spot_at(self.p, h, spot)
	local id = emf.spawn(world, sp.template, x, y, z, loyalty.spawn_size(sp, b.points, self.env.setting("loyalty_pace")))
	if not id or id == 0 then
		self:refuse(sp, "the game refused the spawn" .. (sp.pack and (" (its pack: " .. sp.pack .. ")") or ""), now)
		return nil
	end
	local e = { id = id, beast = b, account = account, key = self.p.key(h), spot = spot, scene = "stroll",
		until_s = now + between(FIRST_S), sc = {}, faced = false, spawned = now }
	self.at[b], self.ids[id] = e, e  -- kept before anything else: an error below leaves no creature nobody tracks
	local f = get(world, "Faction", id)
	if f then f.faction = PLAYER_ALLY end
	if not ours(world, id) then
		emf.destroy(world, id)
		self.at[b], self.ids[id] = nil, nil
		self:refuse(sp, "the creature spawned did not become player-side", now)
		return nil
	end
	local lvl, mine = get(world, "Level", id), get(world, "Level", owner)
	if lvl and lvl.dataStorage and mine and mine.dataStorage then lvl.dataStorage[1] = mine.dataStorage[1] end
	local scaling = get(world, "DynamicEnemyScaling", id)
	if scaling then scaling.nextUpdateTime = 0 end
	self:health(world, e)
	guard(world, id, h)
	return e
end

-- Home beast `e`'s health as when called: its role, loyalty and talents, times the beast_health setting (the health
-- watch, env.on_spawn).
function H:health(world, e)
	if not self.env.on_spawn then return end
	local health = pack_mod.strength(self.env.setting)
	self.env.on_spawn(world, e.id, species.by_id(e.beast.species),
		pack_mod.health_mul(e.beast, self.env.setting("loyalty_pace"), health))
end

-- beast_health changed: every beast at home follows at the next step.
function H:strength_changed()
	self.restrength = true
end

-- Whether beast `b` may be shown now.
function H:allowed(b, now)
	if not self.p.belongs(b) then return false end
	local sp = species.by_id(b.species)
	if not sp or now < (self.refused[sp.id] or 0) then return false end
	return now >= (self.back_at[b] or 0)
end

-- The player-side beasts near a Large Stable's middle that are none of ours and guard no player: the script's beasts
-- there before its reload.
function H:sweep(world, homes)
	local n = 0
	for _, id in ipairs(world:entities()) do
		if not self.ids[id] and ours(world, id) and not get(world, "PlayerState", id) then
			local x, y, z = world:position(id)
			local near = false
			for _, h in ipairs(homes) do
				if dist(h, x, y, z) < self.p.roam_m then near = true end
			end
			local ai = near and get(world, "DynamicEnemy01BehaviorState", id)
			local g = ai and ai.guardEntityId or 0
			if near and not (g ~= 0 and get(world, "PlayerState", g)) and beast_graph(world, id) then
				emf.destroy(world, id)
				n = n + 1
			end
		end
	end
	if n > 0 then self.env.log(string.format("%s: %d beast%s left in a %s before a reload taken out", self.p.log, n,
		n == 1 and "" or "s", self.p.name)) end
end

-- Who is shown where: the beasts shown stay while they may, then the others go to the nearest Large Stable with room.
-- Returns the list {beast, account, owner, key, spot} in order.
function H:assign(world, homes, present, now)
	local by_key = {}
	for _, h in ipairs(homes) do by_key[self.p.key(h)] = h end
	local room = self.p.room(self.env)
	local taken, want, placed = {}, {}, {}
	local function take(k, i)
		taken[k] = taken[k] or {}
		taken[k][i] = true
	end
	local function free(k)
		for i = 1, room do
			if not (taken[k] and taken[k][i]) then return i end
		end
		return nil
	end
	local accounts = {}
	for account in pairs(present) do accounts[#accounts + 1] = account end
	table.sort(accounts)
	local beasts = self.env.beasts()
	for _, account in ipairs(accounts) do
		local owner = present[account]
		local ox, oy, oz = world:position(owner)
		for _, b in ipairs(beasts:list(account)) do
			local e = self.at[b]
			local h = e and by_key[e.key]
			if h and ox and self:allowed(b, now) and self.p.fits(b, e.key) and now >= (self.busy[e.key] or 0)
				and dist(h, ox, oy, oz) <= LEAVE_M and e.spot <= room and not (taken[e.key] and taken[e.key][e.spot]) then
				take(e.key, e.spot)
				placed[b] = { beast = b, account = account, owner = owner, key = e.key, spot = e.spot }
			end
		end
	end
	for _, account in ipairs(accounts) do
		local owner = present[account]
		local ox, oy, oz = world:position(owner)
		for _, b in ipairs(beasts:list(account)) do
			if not placed[b] and ox and self:allowed(b, now) then
				local best, best_d
				for _, h in ipairs(homes) do
					local k = self.p.key(h)
					local d = dist(h, ox, oy, oz)
					if d <= HOME_M and self.p.fits(b, k) and now >= (self.busy[k] or 0) and free(k)
						and (not best_d or d < best_d) then
						best, best_d = k, d
					end
				end
				if best then
					local i = free(best)
					take(best, i)
					placed[b] = { beast = b, account = account, owner = owner, key = best, spot = i }
				end
			end
			if placed[b] then want[#want + 1] = placed[b] end
		end
	end
	return want, by_key
end

-- The upkeep of entry `e` in stable `h` (every second): gone, the clock, health, facing, roaming off, the scenes.
function H:upkeep(world, e, h, now)
	local cur, max = emf.health(world, e.id)
	if not cur or cur <= 0 then
		if now - e.spawned >= SPAWN_GRACE_S then self:on_died(world, e.id) end  -- dead without the game's event
		return
	end
	if not e.stamp then
		local s = get(world, "SpawnTime", e.id)
		if s and s.spawnTime > 0 then
			e.stamp = s.spawnTime
			self.clock = s.spawnTime - e.spawned * NS  -- the game's time again, from each new beast
		end
	end
	if max and cur < max then emf.attribute_add(world, e.id, "health", max - cur) end
	if walking(world, e.id) then
		-- another mod walks it somewhere (as pack.lua leaves a beast out alone): it gets up and is not put back, turned or
		-- told to guard the stable meanwhile; EMF gives it back to the stable after, and one left far is put back then
		if e.sc.held then idle_mod.wake(world, e.id, e.sc) end
		e.far = nil
		return
	end
	if not e.faced and (e.tries or 0) < FACE_TRIES then e.faced = face(world, e, h, self.p) end
	if emf.offer and now >= (e.offer_at or 0) then
		e.offer_at = now + OFFER_EVERY_S
		local held = e.offered and (not emf.offered or emf.offered(world, e.id))
		if not held then
			e.offered = emf.offer(world, e.id, PET, function(w, player, entity) self:pet(w, player, entity) end) == true
		end
	end
	local x, _, z = world:position(e.id)
	if dist(h, x, h.y, z) >= self.p.roam_m then
		e.far = e.far or now
		if now - e.far >= AWAY_S and now - (e.moved or -math.huge) >= AWAY_S then self:put_back(world, e, h, now) end
	else
		e.far = nil
	end
	if self.p.scene then return self.p.scene(self, world, e, h, now, x, z) end
	if self.night and e.scene == "stroll" and not e.no_hold then e.until_s = math.min(e.until_s, now) end
	if now < e.until_s then return end
	if e.scene == "stroll" then
		e.sc.turned = true
		local lease, check = self:game_now(world, LEASE_S * NS), self:game_now(world, CHECK_AHEAD_NS)
		if not (lease and check) then return end  -- the game's time is not known yet: it strolls on
		if not on_floor(self.p, h, x, z) then self:put_back(world, e, h, now) else face(world, e, h) end
		if idle_mod.hold(world, e.id, e.sc, now, lease, check) then
			e.scene, e.until_s = "rest", now + between(REST_S)
		else
			e.no_hold = true
			e.until_s = now + between(STROLL_S)  -- the Sleep state cannot hold it: it strolls on
		end
	elseif self.night then
		e.until_s = now + between(REST_S)  -- it sleeps on until the morning
	else
		idle_mod.wake(world, e.id, e.sc)
		guard(world, e.id, h)
		e.scene, e.until_s = "stroll", now + between(STROLL_S)
	end
end

-- The game's clock: a dawn passed (from before DAWN to after it, or a jump over midnight to after it: a night slept
-- through) begins a game day; night or not.
function H:clock_step(world)
	local h = emf.time_of_day and emf.time_of_day(world)
	if not h then
		self.night = false
		return
	end
	local last = self.last_hour
	if last and h >= DAWN and (last < DAWN or h < last) then self.day = self.day + 1 end
	self.last_hour = h
	self.night = h >= NIGHT_FROM or h < DAWN
end

-- Every second: the stables being built at, who is shown where, then each one's upkeep.
function H:check(world, now)
	self:clock_step(world)
	for id, due in pairs(self.bodies) do
		if now >= due then
			self.bodies[id] = nil
			local cur = emf.health(world, id)
			if world:position(id) and (not cur or cur <= 0) then emf.destroy(world, id) end
		end
	end
	local homes = self.p.on(self.env) and self.p.places(self.env) or {}
	local present = self.env.present() or {}
	if not self.swept and #homes > 0 then
		self.swept = true
		self:sweep(world, homes)
	end
	for b, e in pairs(self.at) do  -- before anything is spawned: an id the game gave out again is not ours
		if not still(world, e) then
			self.at[b], self.ids[e.id] = nil, nil
		elseif not ours(world, e.id) then
			emf.destroy(world, e.id)
			self.at[b], self.ids[e.id] = nil, nil
		end
	end
	for _, player in pairs(self.env.setting("make_room") and present or {}) do
		if aside_mod.building(world, player) then
			local px, py, pz = world:position(player)
			if px then
				for _, h in ipairs(homes) do
					if dist(h, px, py, pz) <= BUILD_M then self.busy[self.p.key(h)] = now + GRACE_S end
				end
			end
		end
	end
	local want, by_key = self:assign(world, homes, present, now)
	local wanted = {}
	for _, w in ipairs(want) do wanted[w.beast] = w end
	for b, e in pairs(self.at) do
		local w = wanted[b]
		if not w or w.key ~= e.key or w.spot ~= e.spot then self:remove(world, e) end
	end
	for _, w in ipairs(want) do
		if not self.at[w.beast] then self:show(world, w.beast, w.account, w.owner, by_key[w.key], w.spot, now) end
	end
	for _, e in pairs(self.at) do
		if e.spawned < now then self:upkeep(world, e, by_key[e.key], now) end
	end
	if self.p.after_check then self.p.after_check(self, world, now) end
end

-- Every step of the server world: the beasts at home are calmed and their leases renewed; once a second, the check.
function H:step(world)
	local now = self.env.now(world)
	if self.restrength then
		self.restrength = nil
		for id, e in pairs(self.ids) do
			if still(world, e) and ours(world, id) then self:health(world, e) end
		end
	end
	for id, e in pairs(self.ids) do
		if not e.ordered then pack_mod.calm(world, id) end  -- one on its scene's enemy order (the dummy) is left alone
		if e.sc.held and (now < e.sc.leased or now - e.sc.leased >= RENEW_S) then
			local ai = get(world, "DynamicEnemy01BehaviorState", id)
			local lease = self:game_now(world, LEASE_S * NS)
			if ai and lease then
				ai.sleepEndTime = lease
				e.sc.leased = now
			end
		end
	end
	if now < self.next_check then return end
	self.next_check = now + 1
	self:check(world, now)
end

-- beastmaster.home: where `account`'s beasts at home are, what they do and how far from the stable's middle they are.
function H:text(world, account)
	local lines = {}
	for _, b in ipairs(self.env.beasts():list(account)) do
		local e = self.at[b]
		if e then
			local x, y, z = e.key:match("^([^%s,]+)[%s,]([^%s,]+)[%s,]([^%s,]+)$")
			x, y, z = tonumber(x), tonumber(y), tonumber(z)
			local bx, by, bz = world:position(e.id)
			local far = bx and string.format(", %.1f m from its middle", dist({ x = x, y = y, z = z }, bx, by, bz)) or ""
			local words = self.p.words and self.p.words(e)
				or (e.scene == "rest" and (self.night and "sleeping" or "resting") or "strolling")
			lines[#lines + 1] = string.format("%s: in the %s at %d, %d, %d, %s%s", b.name, self.p.name,
				math.floor(x + 0.5), math.floor(y + 0.5), math.floor(z + 0.5), words, far)
		end
	end
	if #lines == 0 then
		return string.format("None of your beasts is in a Large Stable now: the beasts you left at home show up in one "
			.. "within %d m of you (the beasts pack's Large Stable, crafted at the Workbench).", HOME_M)
	end
	return table.concat(lines, "\n")
end


-- ---- for a place's own scenes (place.scene) ----
M.between = between

-- Entry `e`'s beast guards place `h` (the game's roaming near it).
function H:guard_place(world, e, h)
	guard(world, e.id, h)
end

-- Entry `e`'s beast is held in the game's Sleep state (lies down, or stands still): true when held; false while the
-- game's time is not known yet or the state cannot hold it.
function H:hold(world, e, now)
	local lease, check = self:game_now(world, LEASE_S * NS), self:game_now(world, CHECK_AHEAD_NS)
	if not (lease and check) then return false end
	return idle_mod.hold(world, e.id, e.sc, now, lease, check) == true
end

-- Entry `e`'s beast gets up (held or not).
function H:wake(world, e)
	idle_mod.wake(world, e.id, e.sc)
end

-- Entry `e`'s beast turns where it stands to face `yaw` (degrees; 0 = +z, 90 = +x), or is put at (x, y, z) with a ground
-- search when given.
function H:turn(world, e, yaw, x, y, z)
	if not emf.teleport then return false end
	local px, py, pz = world:position(e.id)
	if not px then return false end
	local opts = { yaw = yaw }
	if x then opts.ground = true end
	return emf.teleport(world, e.id, x or px, y or py, z or pz, opts) == true
end

return M
