-- The Training Grounds as a place home.lua shows beasts at (home_mod.new(env, grounds.place(...))): the beasts in
-- training (training.lua) show up on the platform of their own Training Grounds while their owner is within 50 m of
-- it, on spots round the dummy (SPOTS, the platform's frame: x across, z forward, turned with the prop), and pass the
-- time there in scenes, one after another:
--   attack: the beast goes for the dummy and swings at it: an enemy order (allies) on an invisible target at the dummy
--     (the game's Base_Door_Collision, DoNotSave, no health: nothing breaks), renewed every ORDER_EVERY_S, for ATTACK_S;
--     the beast's swings come at its kind's own pace (a wolf about every 2.8 s, seen in game). Its fight state is left
--     alone meanwhile (home.lua's calm skips a beast with an order). The first scene after a beast shows up is an attack.
--   rest: it turns to face the dummy and is held in the game's Sleep state (lies down, or stands) for REST_S.
--   sniff: the game's own roaming near the grounds for SNIFF_S.
--   things: by the nearest of the things on the platform (THINGS: the hay, the bowl, the rope, the bones) when it is
--     within NEAR_THING_M: put beside it (a step, the game's teleport with a ground search) facing it, and held for
--     THINGS_S; farther from every one, a rest instead.
--   After an attack comes no attack; at night (18 to 6 by the game's clock) only rests and things.
-- The invisible target of a Training Grounds is spawned at its first attack and taken away when none of its beasts is
-- shown there any more (only where it was spawned: the game gives ids out again); a world load forgets it (it is not
-- saved).
local training_mod = require("training")
local home_mod = require("home")

local M = {}

M.TARGET = "217bfc46-d4c4-4732-b097-0e8564cb57bc"  -- Base_Door_Collision: what Night Raids' raiders swing at in a wall
local TOP = 0.32               -- m: the platform's planks above its place
local SPOTS = { { x = 0.95, z = 0.95 }, { x = -0.95, z = -0.95 }, { x = -0.95, z = 0.95 }, { x = 0.95, z = -0.95 } }
local FLOOR_R = 2.25           -- m: the platform's planks inside its rim
local THINGS = {               -- frame metres: where each thing lies, and the spot beside it a beast lies at
	{ name = "hay", x = 1.67, z = 0.51 },
	{ name = "bowl", x = -1.84, z = 0.0 },
	{ name = "rope", x = 0.15, z = -1.81 },
	{ name = "bones", x = -0.2, z = 1.83 },
}
local BESIDE_M = 0.65          -- a beast lies this much nearer the middle than the thing
local NEAR_THING_M = 3         -- a beast this near a thing may go and lie by it
local ATTACK_S = { 8, 15 }
local REST_S = { 10, 25 }
local SNIFF_S = { 10, 20 }
local THINGS_S = { 15, 30 }
local ORDER_S, ORDER_EVERY_S = 4, 3  -- the order lasts ORDER_S and is renewed every ORDER_EVERY_S
local WEIGHTS = { attack = 45, rest = 20, sniff = 20, things = 15 }
local NIGHT_FROM, DAWN = 18, 6

local function yaw_to(x, z, tx, tz)
	return math.deg(math.atan(tx - x, tz - z))
end

local function night(world)
	local h = emf.time_of_day and emf.time_of_day(world)
	return h ~= nil and (h >= NIGHT_FROM or h < DAWN)
end

local function pick(after_attack, at_night)
	local w = {}
	for k, v in pairs(WEIGHTS) do
		if not (k == "attack" and after_attack) and not (at_night and (k == "attack" or k == "sniff")) then w[k] = v end
	end
	local total = 0
	for _, v in pairs(w) do total = total + v end
	local r = math.random() * total
	for _, k in ipairs({ "attack", "rest", "sniff", "things" }) do
		if w[k] then
			r = r - w[k]
			if r <= 0 then return k end
		end
	end
	return "rest"
end

-- The invisible target at Training Grounds `h` (spawned when needed), or nil when the game refused it.
local function target(home, world, h)
	local st = home.place_state
	st.targets = st.targets or {}
	local key = training_mod.key(h)
	local t = st.targets[key]
	if t then
		local x, y, z = world:position(t.id)
		if x and math.abs(x - t.x) < 0.5 and math.abs(y - t.y) < 0.5 and math.abs(z - t.z) < 0.5 then return t.id end
		st.targets[key] = nil
	end
	if st.refused then return nil end
	local x, y, z = home_mod.at(h, 0, 0, TOP)
	local id = emf.spawn(world, M.TARGET, x, y, z)
	if not id or id == 0 then
		st.refused = true
		home.env.log("training: the game refused the dummy's invisible target: beasts in training do not attack it")
		return nil
	end
	st.targets[key] = { id = id, x = x, y = y, z = z }
	return id
end

local function end_order(e)
	if e.ordered and emf.enemy_order_cancel then emf.enemy_order_cancel(e.ordered) end
	e.ordered = nil
end

local function start(home, world, e, h, now, x, z, scene)
	if scene == "attack" then
		local t = target(home, world, h)
		local id = t and emf.enemy_order and emf.enemy_order(world, e.id, t, { seconds = ORDER_S, allies = true })
		if id then
			home:wake(world, e)
			e.ordered, e.order_at = id, now + ORDER_EVERY_S
			e.scene, e.until_s = "attack", now + home_mod.between(ATTACK_S)
			return
		end
		scene = "sniff"
	end
	if scene == "things" then
		local best, best_d
		for _, th in ipairs(THINGS) do
			local tx, _, tz = home_mod.at(h, th.x, th.z)
			local d = math.sqrt((tx - x) ^ 2 + (tz - z) ^ 2)
			if d <= NEAR_THING_M and (not best_d or d < best_d) then best, best_d = th, d end
		end
		if best then
			local r = math.sqrt(best.x ^ 2 + best.z ^ 2)
			local k = (r - BESIDE_M) / r
			local sx, sy, sz = home_mod.at(h, best.x * k, best.z * k, TOP + 0.2)
			local tx, _, tz = home_mod.at(h, best.x, best.z)
			home:wake(world, e)
			home:turn(world, e, yaw_to(sx, sz, tx, tz), sx, sy, sz)
			if home:hold(world, e, now) then
				e.scene, e.thing, e.until_s = "things", best.name, now + home_mod.between(THINGS_S)
				return
			end
		end
		scene = "rest"
	end
	if scene == "rest" then
		local cx, _, cz = home_mod.at(h, 0, 0)
		home:wake(world, e)
		home:turn(world, e, yaw_to(x, z, cx, cz))
		if home:hold(world, e, now) then
			e.scene, e.until_s = "rest", now + home_mod.between(REST_S)
			return
		end
	end
	home:wake(world, e)
	home:guard_place(world, e, h)
	e.scene, e.until_s = "sniff", now + home_mod.between(SNIFF_S)
end

-- place.scene: the beast's scenes at its Training Grounds (every second, from home.lua's upkeep).
local function scene(home, world, e, h, now, x, z)
	if e.ordered then
		if now >= e.until_s then
			end_order(e)
			e.attacked = true
			start(home, world, e, h, now, x, z, pick(true, night(world)))
		elseif now >= e.order_at then
			local t = target(home, world, h)
			local id = t and emf.enemy_order(world, e.id, t, { seconds = ORDER_S, allies = true })
			if id then
				e.ordered, e.order_at = id, now + ORDER_EVERY_S
			else
				end_order(e)
				start(home, world, e, h, now, x, z, "sniff")
			end
		end
		return
	end
	if now < e.until_s then return end
	local first = not e.started
	e.started = true
	if first and not night(world) then
		start(home, world, e, h, now, x, z, "attack")
	else
		start(home, world, e, h, now, x, z, pick(e.scene == "attack", night(world)))
	end
end

-- place.after_check: the invisible target of a Training Grounds none of whose beasts is shown goes.
local function after_check(home, world)
	local st = home.place_state
	if not st.targets then return end
	local shown = {}
	for _, e in pairs(home.at) do shown[e.key] = true end
	for key, t in pairs(st.targets) do
		if not shown[key] then
			local x, y, z = world:position(t.id)
			if x and math.abs(x - t.x) < 0.5 and math.abs(y - t.y) < 0.5 and math.abs(z - t.z) < 0.5 then
				emf.destroy(world, t.id)
			end
			st.targets[key] = nil
		end
	end
end

local WORDS = { attack = "attacking the dummy", rest = "resting", sniff = "sniffing about", stroll = "sniffing about" }

-- The place for home_mod.new: `grounds()` the Training Grounds found now (stable.lua S:grounds()), `room()` the beasts
-- each holds (training.lua T:room()).
function M.place(grounds, room)
	return {
		name = "Training Grounds",
		log = "training",
		room = function() return room() end,
		spots = SPOTS,
		floor = function(lx, lz) return lx * lx + lz * lz <= FLOOR_R * FLOOR_R end,
		spawn_y = TOP + 0.2,
		roam_m = 4,
		places = function() return grounds() end,
		on = function() return true end,
		belongs = function(b) return b.training ~= nil and not b.out end,
		fits = function(b, key) return b.training == key end,
		key = training_mod.key,
		scene = scene,
		after_check = after_check,
		yaw = function(h, e)
			local sx, _, sz = home_mod.at(h, SPOTS[e.spot].x, SPOTS[e.spot].z)
			local cx, _, cz = home_mod.at(h, 0, 0)
			return yaw_to(sx, sz, cx, cz)
		end,
		words = function(e)
			if e.scene == "things" then return "lying by the " .. (e.thing or "things") end
			return WORDS[e.scene] or e.scene
		end,
	}
end

return M
