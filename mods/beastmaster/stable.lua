-- Stables: the props Beastmaster counts as a stable.
--   - a built prop whose render model is in M.MODELS, within BASE_M (horizontally) of a flame altar that is not
--     abandoned (players build only in a base; villages and ruins may have the same model);
--   - a prop the host marked (beastmaster.stable mark): kept with the world by its place and model, as lines
--     "k <x> <y> <z> <model or ->" in the world's data, until the host takes the mark off;
--   - a stable a joined player's game saw in its client world and reported (S:report; Beastmaster on their game, the
--     beastmaster/ui channel): counted when a flame altar of the last search is within BASE_M, kept with the world as
--     lines "r <x> <y> <z> <model> <qx> <qy> <qz> <qw>" (or "-" for no turn) until a game within CLIENT_SEE_M of it
--     (a joined player's, or the host's own) no longer shows it there, after that game's first SETTLE_S. Any EMF
--     player's game reports; a player could make any prop count (accepted: a stable opens the window and holds the
--     beasts at home, nothing more). A joined player's game searches its client world with M.new_reporter().
-- The server world is searched a slice at a time (SCAN_PER_STEP entities per step, a new search every RESCAN_S
-- seconds) for flame altars and props with a stable's model. Render models are client-side: on the host the client
-- world is searched for the models too, every CLIENT_RESCAN_S (a bed just built counts within seconds), and its beds
-- count by their place. A bed the client world no longer shows is kept while the server world has an entity at its
-- place (the host may just be too far away to see it), unless the host is within CLIENT_SEE_M of it (then it was taken
-- down, and stops counting as fast). A dedicated server has no client world, so there only marked props count unless
-- its server world has the model.
-- Each stable has the game's own "E Open" prompt (emf.offer, Lua 1.14) on its server-world entity: a bed the server
-- world shows, its own; a bed only the client world shows, or a marked prop, the entity with a StaticCollider nearest its
-- place (matched in the server world's search); only an entity with a StaticCollider that is still at the stable's
-- place gets one (the game gives an id out again when its entity streams out). The prompts are offered again after
-- every search (the game drops one when its entity streams out) and taken off a stable that stops counting.
local aim = require("aim")

local M = {}

-- The render models of the props players build as a stable, and the names the list shows: the game's animal beds (the
-- straw beds for farm animals; items Animal_Farm_Housing_Sleeping_Bed and ..._YAK_Sanddigger), which have no E use of
-- their own, and the beasts pack's Stable and Large Stable (crafted at the Workbench; render models of their own).
M.MODELS = {
	["daeb81bb-8398-4128-9107-c75b2e3e7d5b"] = "Animal bed",        -- animalprops_farm_animal_sleep_01
	["7dd9139a-fba7-4e60-8b5a-fa853c250ef7"] = "Large animal bed",  -- animalprops_farm_animal_sleep_large_01
	["1a49170a-b3d2-498d-9062-8946f6469613"] = "Stable",            -- beastmaster_stable (the beasts pack)
	["5879f304-0050-4ecb-96f4-5cc0e66155d9"] = "Large Stable",      -- beastmaster_large_stable (the beasts pack)
	["bb3549a0-5088-4c3b-8057-3a7e48a59ad6"] = "Training Grounds",  -- beastmaster_training_grounds (the beasts pack)
}
-- The stables the beasts at home live in (home.lua): the Large Stable.
M.HOMES = { ["5879f304-0050-4ecb-96f4-5cc0e66155d9"] = true }
-- Where beasts train (training.lua): the Training Grounds.
M.GROUNDS = { ["bb3549a0-5088-4c3b-8057-3a7e48a59ad6"] = true }
M.STABLE_ITEM = "Prop_Beastmaster_Stable"  -- the beasts pack's Stable: its item's debug name (the pack has both)

local ASK_AGAIN_S = 60  -- s: craftable(t) asks again this long after an item lookup that found nothing
local craftable = false  -- the Stable's item was found (it stays: the game's items do not change while it runs)
local ask_after = nil    -- craftable(t): no lookup before this time

-- Whether the game has the beasts pack's Stable to craft. Inside callbacks only (emf.item needs a world; a lookup reads
-- the game's whole item list). Without `t` it looks the item up until found (the commands and hints that ask are rare);
-- with the world's time `t` (the wheel, built every second) at most once every ASK_AGAIN_S.
function M.craftable(t)
	if craftable then return true end
	if t and ask_after and t < ask_after then return false end
	local ok, id = pcall(function() return emf.item and emf.item(M.STABLE_ITEM) end)
	craftable = ok and id ~= nil and id ~= false
	if t then ask_after = t + ASK_AGAIN_S end
	return craftable
end

-- What players build as a stable, for the hints.
function M.what_to_build()
	if M.craftable() then return "a Stable or a Large Stable (crafted at the Workbench) or an animal bed" end
	return "an animal bed (the straw bed for farm animals)"
end

-- What a stable is, in brackets after "a stable": "(an animal bed in your base)".
function M.what_it_is()
	return "(" .. (M.craftable() and "a Stable, a Large Stable or an animal bed" or "an animal bed") .. " in your base)"
end

local SCAN_PER_STEP = 250  -- entities looked at per step while a world is searched
local RESCAN_S = 30        -- s between two searches of the server world (flame altars)
local CLIENT_RESCAN_S = 5  -- s between two searches of the host's client world (the beds' render models)
local BASE_M = 80          -- m: a built stable is within this of a flame altar (horizontally)
local SAME_M = 1           -- m: two places within this are one
local MARK_REACH_M = 8     -- m: mark and unmark reach this far
local MARK_NEAR_M = 3      -- m: mark without camera data takes the nearest prop this close
local PROP_HEIGHT = 1      -- m: the point of a prop the crosshair aims at, above its position
local KEY_RAY_M = 1        -- under_crosshair: the crosshair passes within this of a stable's middle
local CLIENT_STEPS = 180   -- the client world stepping this often without a server step: someone else's game
local CLIENT_SEE_M = 40    -- m: a bed this close to the host that the client world does not show is gone
local OFFER = { verb = "Open" }  -- the prompt: "E Open" (the game's word; a text of our own shows no word)
local WAIT_S = 300         -- s: a reported stable no flame altar is known near yet is tried again after searches this long
local REPORT_NEAR_M = 150  -- m: a reported stable farther than this from the player whose game reports it is not taken
local REPORTED_MAX = 64    -- reported stables a world keeps (a game that reports more: they are not taken, logged once)
local WAITING_MAX = 32     -- reported stables waiting for a flame altar
M.CLIENT_STEPS = CLIENT_STEPS
M.REPORT_MAX = 32          -- stables a report holds (the nearest to its player first); more are not looked at
M.SETTLE_S = 30            -- s: a game's first this long in a world (still streaming in) removes no reported stable

local function dist(a, x, y, z) return math.sqrt((a.x - x) ^ 2 + (a.y - y) ^ 2 + (a.z - z) ^ 2) end
local function round(v) return math.floor(v + 0.5) end
local function round2(v) return math.floor(v * 100 + 0.5) / 100 end

local function has(world, component, id)
	local ok, value = pcall(world.get, world, component, id)
	return ok and value ~= nil
end

local function is_player(world, id)
	return id >= 1 and id <= 1023 and has(world, "PlayerState", id)
end

-- An entity's turn in the world: its CurrentTransform orientation {x, y, z, w}, or nil.
local function turn_of(world, id)
	local ok, t = pcall(world.get, world, "CurrentTransform", id)
	local q = ok and t and t.transform and t.transform.orientation
	if not q then return nil end
	return { x = q.x, y = q.y, z = q.z, w = q.w }
end

-- The camera ray of `player`, or nil when the server has no camera for them.
local function camera(world, player)
	local ok, input = pcall(world.get, world, "PlayerInput", player)
	if not ok or not input then return nil end
	local q = input.fromClient.camera.orientation
	if q.x == 0 and q.y == 0 and q.z == 0 and q.w == 0 then return nil end
	return aim.ray(world, player)
end

-- One slice of a search of a client world for the stables' render models: entities w.i .. of w.ids into w.models
-- (entity -> {model, x, y, z, q}). True when the search is done.
local function scan_slice(world, w)
	local last = math.min(#w.ids, w.i + SCAN_PER_STEP - 1)
	for k = w.i, last do
		local id = w.ids[k]
		local ok, t = pcall(world.template, world, "ModelResource", id)
		if ok and t and M.MODELS[t.model] then
			local x, y, z = world:position(id)
			if x then w.models[id] = { model = t.model, x = x, y = y, z = z, q = turn_of(world, id) } end
		end
	end
	w.i = last + 1
	return w.i > #w.ids
end

-- Whether `list` (entries {x, y, z, model}) has one with `e`'s model within SAME_M of it.
local function has_entry(list, e)
	for _, f in ipairs(list) do
		if f.model == e.model and dist(f, e.x, e.y, e.z) <= SAME_M then return true end
	end
	return false
end

-- ---- a joined player's game: the stables its client world shows, for the server (client.lua) ----
local RP = {}
RP.__index = RP

function M.new_reporter()
	local rp = setmetatable({}, RP)
	rp:reset()
	return rp
end

-- A new session (another server, or the same one again): search at once, nothing seen yet.
function RP:reset()
	self.walk, self.after, self.last = nil, 0, nil  -- last: the last finished search's list
end

-- A step of the client world (`t` its time, `me` this game's player): searched every CLIENT_RESCAN_S, SCAN_PER_STEP
-- entities a step. A finished search answers its list {m (model), x, y, z, q}, the nearest to `me` first, at most
-- M.REPORT_MAX; nil while it searches.
function RP:step(world, t, me)
	if not self.walk then
		if t + CLIENT_RESCAN_S < self.after then self.after = t end  -- the clock went back (another server's)
		if t < self.after then return nil end
		self.walk = { ids = world:entities(), i = 1, models = {} }
	end
	if not scan_slice(world, self.walk) then return nil end
	local models = self.walk.models
	self.walk, self.after = nil, t + CLIENT_RESCAN_S
	local px, py, pz
	if me and me ~= 0 then px, py, pz = world:position(me) end
	local list = {}
	for _, c in pairs(models) do
		local q = c.q and { x = c.q.x, y = c.q.y, z = c.q.z, w = c.q.w } or nil
		list[#list + 1] = { m = c.model, x = round2(c.x), y = round2(c.y), z = round2(c.z), q = q,
			d = px and dist(c, px, py, pz) or 0 }
	end
	table.sort(list, function(a, b)
		if a.d ~= b.d then return a.d < b.d end
		if a.x ~= b.x then return a.x < b.x end
		return a.z < b.z
	end)
	while #list > M.REPORT_MAX do list[#list] = nil end
	for _, e in ipairs(list) do e.d = nil end
	self.last = list
	return list
end

-- Whether two reported lists hold the same stables (each of `a` has one of the same model within SAME_M in `b`).
function M.same_list(a, b)
	if not a or not b or #a ~= #b then return false end
	for _, e in ipairs(a) do
		local found = false
		for _, f in ipairs(b) do
			if f.m == e.m and dist(f, e.x, e.y, e.z) <= SAME_M then
				found = true
				break
			end
		end
		if not found then return false end
	end
	return true
end

local S = {}
S.__index = S

-- env: now(world) -> seconds; log(text); on_found(world) (optional): called after a search or a mark when the world
-- has a stable; on_open(world, player, entity) (optional): a player pressed E at a stable's prompt; on_change(world)
-- (optional): the reported stables changed (to be saved).
function M.new(env)
	local s = setmetatable({ env = env, marks = {}, reported = {} }, S)
	s:reset()
	return s
end

-- A world (re)loaded: what was found is searched again.
function S:reset()
	self.walk, self.walk_after = nil, 0
	self.cwalk, self.cwalk_after = nil, 0
	self.client_models = {}  -- entity -> {model, x, y, z, q}: the beds the host's client world showed in its last search
	self.client_steps = 0
	self.altars = {}         -- the flame altars the last search of the server world found: {x, z}
	self.server_found = {}   -- the props with a stable's model the server world itself showed: {id, x, y, z, name,
	                         -- model, q}
	self.found = {}          -- the built stables (in a base): {id, x, y, z, name, model, q (its turn), entity}
	self.t = 0               -- the server world's last time
	self.stables = {}
	self.client_server = {}  -- a client world bed's id -> the server world's entity at its place (for its prompt)
	self.offered = {}        -- server entity -> true: a stable's prompt the game took
	self.waiting = {}        -- reported stables no flame altar was known near: key -> {x, y, z, model, q, until_t}
	self.client_since = nil  -- the server world's time at the host's client world's first step with the host in it
	self.told = {}           -- what was logged once (a full list of reported stables)
	self.host_view = nil     -- the host's last client search after its first SETTLE_S: {x, y, z, models}
end

-- The marks and the reported stables from the world's data.
function S:load(text)
	self.marks, self.reported = {}, {}
	for line in (text or ""):gmatch("[^\r\n]+") do
		local x, y, z, model = line:match("^k (%S+) (%S+) (%S+) (%S+)$")
		if x and tonumber(x) and tonumber(y) and tonumber(z) then
			self.marks[#self.marks + 1] = { x = tonumber(x), y = tonumber(y), z = tonumber(z), model = model }
		end
		local rx, ry, rz, rmodel, rest = line:match("^r (%S+) (%S+) (%S+) (%S+)%s*(.*)$")
		if rx and tonumber(rx) and tonumber(ry) and tonumber(rz) and M.MODELS[rmodel] then
			local qx, qy, qz, qw = rest:match("^(%S+) (%S+) (%S+) (%S+)$")
			local q
			if qx and tonumber(qx) and tonumber(qy) and tonumber(qz) and tonumber(qw) then
				q = { x = tonumber(qx), y = tonumber(qy), z = tonumber(qz), w = tonumber(qw) }
			end
			self.reported[#self.reported + 1] = { x = tonumber(rx), y = tonumber(ry), z = tonumber(rz), model = rmodel,
				q = q }
		end
	end
	self:reset()
	self:rebuild()
end

function S:serialize()
	local lines = {}
	for _, m in ipairs(self.marks) do lines[#lines + 1] = string.format("k %.2f %.2f %.2f %s", m.x, m.y, m.z, m.model) end
	for _, r in ipairs(self.reported) do
		local q = r.q and string.format("%.4f %.4f %.4f %.4f", r.q.x, r.q.y, r.q.z, r.q.w) or "-"
		lines[#lines + 1] = string.format("r %.2f %.2f %.2f %s %s", r.x, r.y, r.z, r.model, q)
	end
	return table.concat(lines, "\n")
end

-- The model of an entity's template: on this world, else what the host's client world showed for it at its place
-- (for the log of a mark).
function S:model_of(world, id)
	local ok, t = pcall(world.template, world, "ModelResource", id)
	if ok and t then return t.model end
	local c = self.client_models[id]
	if c then
		local x, y, z = world:position(id)
		if x and dist(c, x, y, z) <= SAME_M then return c.model end
	end
	return nil
end

-- The stables: the built ones found, the reported ones, then the marks (one place counts once).
function S:rebuild(world)
	local list = {}
	for _, f in ipairs(self.found) do
		list[#list + 1] = { id = f.id, x = f.x, y = f.y, z = f.z, kind = "built", name = f.name, entity = f.entity,
			model = f.model, q = f.q }
	end
	for _, r in ipairs(self.reported) do
		local twice = false
		for _, b in ipairs(list) do
			if dist(b, r.x, r.y, r.z) <= SAME_M then twice = true end
		end
		if not twice then
			list[#list + 1] = { x = r.x, y = r.y, z = r.z, kind = "built", reported = true, name = M.MODELS[r.model],
				model = r.model, q = r.q, entity = r.entity }
		end
	end
	for _, m in ipairs(self.marks) do
		local twice = false
		for _, b in ipairs(list) do
			if dist(b, m.x, m.y, m.z) <= SAME_M then twice = true end
		end
		if not twice then list[#list + 1] = { x = m.x, y = m.y, z = m.z, kind = "mark", entity = m.entity } end
	end
	self.stables = list
	if world then self:sync_offers(world) end
	if world and #list > 0 and self.env.on_found then self.env.on_found(world) end
end

-- Whether stable `b`'s entity is at its place now (an id the game gave out again is not).
local function at_place(world, b)
	if not b.entity then return false end
	local x, y, z = world:position(b.entity)
	return x ~= nil and dist(b, x, y, z) <= SAME_M
end

-- Every stable's prompt offered (again: an identical offer only replaces its function) on its entity when that is at
-- its place and has a StaticCollider (the game shows no prompt without one), and taken off the entities that are no
-- stable's now. Server world only.
function S:sync_offers(world)
	if not (emf.offer and world:is_server()) then return end
	local want = {}
	for _, b in ipairs(self.stables) do
		if at_place(world, b) and has(world, "StaticCollider", b.entity) then want[b.entity] = true end
	end
	for id in pairs(want) do
		local ok = emf.offer(world, id, OFFER, function(w, player, entity)
			if self.env.on_open then self.env.on_open(w, player, entity) end
		end)
		self.offered[id] = ok or nil
	end
	for id in pairs(self.offered) do
		if not want[id] then
			emf.offer_remove(world, id)
			self.offered[id] = nil
		end
	end
end

-- Whether stable `b` (from the list) has the game's prompt now: offered, still held by EMF (emf.offered, Lua 1.15), its
-- entity still at its place, and the game's prompt there (InteractionOfferRuntime, where it can be read). A prompt the
-- game or EMF dropped since (the entity streamed out and back) starts a new search, which offers it again.
function S:prompted(world, b)
	if not (b and b.entity and self.offered[b.entity]) then return false end
	local held = not emf.offered or emf.offered(world, b.entity)
	local ok, prompt = pcall(world.get, world, "InteractionOfferRuntime", b.entity)
	if held and at_place(world, b) and not (ok and prompt == nil) then return true end
	if not self.walk then self.walk_after = 0 end
	return false
end

-- The stable whose prompt is on entity `id`, at its place now; nil when none.
function S:stable_of(world, id)
	for _, b in ipairs(self.stables) do
		if b.entity == id and at_place(world, b) then return b end
	end
	return nil
end

function S:count()
	return #self.stables
end

-- The built stables the beasts at home live in (M.HOMES): {x, y, z, q (its turn, or nil), entity (its server-world
-- entity, or nil), name}.
function S:homes()
	local out = {}
	for _, b in ipairs(self.stables) do
		if b.kind == "built" and b.model and M.HOMES[b.model] then out[#out + 1] = b end
	end
	return out
end

-- The built Training Grounds (M.GROUNDS), as homes() gives the Large Stables.
function S:grounds()
	local out = {}
	for _, b in ipairs(self.stables) do
		if b.kind == "built" and b.model and M.GROUNDS[b.model] then out[#out + 1] = b end
	end
	return out
end

-- A prompt's entity at a place: an entity with a StaticCollider, the nearest (`best` {id, d} so far, or nil).
local function nearer(world, best, id, d)
	if (best and d >= best.d) or is_player(world, id) or not has(world, "StaticCollider", id) then return best end
	return { id = id, d = d }
end

function S:walk_step(world)
	if not self.walk then
		local beds = {}  -- the client world's beds when this search began: which have an entity at their place here
		for id in pairs(self.client_models) do beds[id] = false end
		self.walk = { ids = world:entities(), i = 1, found = {}, altars = {}, beds = beds, bed_at = {}, mark_at = {},
			rep_at = {} }
	end
	local w = self.walk
	local last = math.min(#w.ids, w.i + SCAN_PER_STEP - 1)
	for k = w.i, last do
		local id = w.ids[k]
		if next(w.beds) or #self.marks > 0 or #self.reported > 0 then
			local x, y, z = world:position(id)
			if x then
				for bed in pairs(w.beds) do
					local c = self.client_models[bed]
					local d = c and dist(c, x, y, z)
					if d and d <= SAME_M then
						w.beds[bed] = true
						w.bed_at[bed] = nearer(world, w.bed_at[bed], id, d)
					end
				end
				for _, m in ipairs(self.marks) do
					local d = dist(m, x, y, z)
					if d <= SAME_M then w.mark_at[m] = nearer(world, w.mark_at[m], id, d) end
				end
				for _, r in ipairs(self.reported) do
					local d = dist(r, x, y, z)
					if d <= SAME_M then w.rep_at[r] = nearer(world, w.rep_at[r], id, d) end
				end
			end
		end
		local ok, base = pcall(world.get, world, "BaseIdComponent", id)
		if ok and base and not base.wasAbandoned then  -- an abandoned flame altar makes no base
			local x, _, z = world:position(id)
			if x then w.altars[#w.altars + 1] = { x = x, z = z } end
		end
		local okm, t = pcall(world.template, world, "ModelResource", id)
		local model = okm and t and t.model
		if model and M.MODELS[model] then
			local x, y, z = world:position(id)
			if x then
				w.found[#w.found + 1] = { id = id, x = x, y = y, z = z, name = M.MODELS[model], model = model,
					q = turn_of(world, id) }
			end
		end
	end
	w.i = last + 1
	if w.i <= #w.ids then return end
	self.altars, self.server_found = w.altars, w.found
	for bed, seen in pairs(w.beds) do  -- no entity at its place on the server world: it is gone
		if not seen then self.client_models[bed] = nil end
	end
	self.client_server = {}
	for bed, at in pairs(w.bed_at) do self.client_server[bed] = at.id end
	for _, m in ipairs(self.marks) do
		local at = w.mark_at[m]
		m.entity = at and at.id or nil
	end
	for _, r in ipairs(self.reported) do
		local at = w.rep_at[r]
		r.entity = at and at.id or nil
	end
	self.walk = nil
	self.walk_after = self.t + RESCAN_S
	self:refound(world)
end

-- The built stables from the server world's props and the client world's beds (one place counts once), those in a
-- base; then the list.
function S:refound(world)
	local all = {}
	for _, f in ipairs(self.server_found) do
		all[#all + 1] = { id = f.id, x = f.x, y = f.y, z = f.z, name = f.name, entity = f.id, model = f.model, q = f.q }
	end
	local ids = {}
	for id in pairs(self.client_models) do ids[#ids + 1] = id end
	table.sort(ids)
	for _, id in ipairs(ids) do
		local c = self.client_models[id]
		local twice = false
		for _, f in ipairs(all) do
			if dist(f, c.x, c.y, c.z) <= SAME_M then twice = true end
		end
		if not twice then
			all[#all + 1] = { id = id, x = c.x, y = c.y, z = c.z, name = M.MODELS[c.model], entity = self.client_server[id],
				model = c.model, q = c.q }
		end
	end
	self.found = {}
	for _, f in ipairs(all) do
		if self:in_base(f) then self.found[#self.found + 1] = f end
	end
	-- the reported stables that waited for a flame altar
	local accepted = false
	for key, wt in pairs(self.waiting) do
		if self.t > wt.until_t then
			self.waiting[key] = nil
		elseif self:in_base(wt) then
			self.waiting[key] = nil
			if not has_entry(self.reported, wt) and self:room(wt) then
				self.reported[#self.reported + 1] = { x = wt.x, y = wt.y, z = wt.z, model = wt.model, q = wt.q }
				accepted = true
			end
		end
	end
	self:rebuild(world)
	if accepted and self.env.on_change then self.env.on_change(world) end
end

-- Whether a flame altar of the last search is within BASE_M (horizontally) of `p`.
function S:in_base(p)
	for _, a in ipairs(self.altars) do
		if (a.x - p.x) ^ 2 + (a.z - p.z) ^ 2 <= BASE_M * BASE_M then return true end
	end
	return false
end

-- A reported entry checked: {x, y, z, model, q} or nil (not a table, not a stable's model, a bad number).
local function num(v) return type(v) == "number" and v == v and math.abs(v) < 1e6 end
local function valid_entry(e)
	if type(e) ~= "table" or type(e.m) ~= "string" or not M.MODELS[e.m] then return nil end
	if not (num(e.x) and num(e.y) and num(e.z)) then return nil end
	local q
	if type(e.q) == "table" and num(e.q.x) and num(e.q.y) and num(e.q.z) and num(e.q.w) then
		q = { x = e.q.x, y = e.q.y, z = e.q.z, w = e.q.w }
	end
	return { x = e.x, y = e.y, z = e.z, model = e.m, q = q }
end

-- Whether a reported stable may be taken: the world keeps at most REPORTED_MAX (more are logged once).
function S:room(e)
	if #self.reported < REPORTED_MAX then return true end
	if not self.told.full then
		self.told.full = true
		self.env.log(string.format("the reported stables are full (%d): more are not taken until some are gone",
			REPORTED_MAX))
	end
	return false
end

-- The reported stables within CLIENT_SEE_M of (x, y, z) (and closer than `limit` when given) that `seen` (entries
-- {x, y, z, model}) does not have are gone (and those waiting there). How many went.
function S:prune(x, y, z, seen, limit)
	local function near(p)
		local d = dist(p, x, y, z)
		return d <= CLIENT_SEE_M and (not limit or d < limit)
	end
	local gone = 0
	for i = #self.reported, 1, -1 do
		local r = self.reported[i]
		if near(r) and not has_entry(seen, r) then
			table.remove(self.reported, i)
			gone = gone + 1
		end
	end
	for key, wt in pairs(self.waiting) do
		if near(wt) and not has_entry(seen, wt) then self.waiting[key] = nil end
	end
	return gone
end

-- A joined player's game at (px, py, pz) (the server world's place of that player) reports the stables its client world
-- shows (`list`: entries {m, x, y, z, q}, the nearest first; at most M.REPORT_MAX looked at, bad ones and ones farther
-- than REPORT_NEAR_M from the player skipped). New ones count when a flame altar of the last search is near (else they
-- wait for one) and the world keeps fewer than REPORTED_MAX; reported ones near the player that the list does not have
-- are gone (a full list holds only its nearest: then only those closer than its farthest). Answers changed, seen,
-- added, gone.
function S:report(world, px, py, pz, list)
	if type(list) ~= "table" then return false, 0, 0, 0 end
	local seen = {}
	local farthest = 0
	for i = 1, math.min(#list, M.REPORT_MAX) do
		local e = valid_entry(list[i])
		if e and dist(e, px, py, pz) <= REPORT_NEAR_M then
			seen[#seen + 1] = e
			farthest = math.max(farthest, dist(e, px, py, pz))
		end
	end
	local gone = self:prune(px, py, pz, seen, #list >= M.REPORT_MAX and farthest or nil)
	local added = 0
	local waiting = 0
	for _ in pairs(self.waiting) do waiting = waiting + 1 end
	for _, e in ipairs(seen) do
		if not has_entry(self.reported, e) then
			if self:in_base(e) then
				if self:room(e) then
					self.reported[#self.reported + 1] = e
					added = added + 1
				end
			elseif waiting < WAITING_MAX then
				local key = string.format("%s %.0f %.0f %.0f", e.model, e.x, e.y, e.z)
				if not self.waiting[key] then waiting = waiting + 1 end
				self.waiting[key] = { x = e.x, y = e.y, z = e.z, model = e.model, q = e.q, until_t = self.t + WAIT_S }
			end
		end
	end
	local changed = added > 0 or gone > 0
	if changed then
		self:rebuild(world)
		if self.env.on_change then self.env.on_change(world) end
	end
	return changed, #seen, added, gone
end

-- Every step of the server world.
function S:step(world)
	self.t = self.env.now(world)
	self.client_steps = 0
	local v = self.host_view
	if v then  -- the host's own game is one that sees the reported stables near it
		self.host_view = nil
		local seen = {}
		for _, c in pairs(v.models) do seen[#seen + 1] = { x = c.x, y = c.y, z = c.z, model = c.model } end
		if self:prune(v.x, v.y, v.z, seen) > 0 then
			self:rebuild(world)
			if self.env.on_change then self.env.on_change(world) end
		end
	end
	if self.walk or self.t >= self.walk_after then self:walk_step(world) end
end

-- Every step of a client world: on the host (its server world steps too) the stables' models are searched here.
function S:client_step(world)
	self.client_steps = self.client_steps + 1
	if self.client_steps > CLIENT_STEPS then return end
	if not self.client_since and self.t > 0 then  -- the settle starts with the host in its world (not on the loading screen)
		local me = emf.local_player()
		if me and me ~= 0 and world:position(me) then self.client_since = self.t end
	end
	if not self.cwalk then
		if self.t < self.cwalk_after then return end
		self.cwalk = { ids = world:entities(), i = 1, models = {} }
	end
	local w = self.cwalk
	if scan_slice(world, w) then
		-- a bed built, taken down (not shown with the host near it) or moved: the server world is searched now (its end
		-- decides the stables again, with this list)
		local changed = false
		for id, c in pairs(w.models) do
			local old = self.client_models[id]
			if not old or old.model ~= c.model or dist(old, c.x, c.y, c.z) > SAME_M then changed = true end
		end
		local me = emf.local_player()
		local hx, hy, hz
		if me and me ~= 0 then hx, hy, hz = world:position(me) end
		for id, old in pairs(self.client_models) do
			if not w.models[id] then
				if hx and dist(old, hx, hy, hz) <= CLIENT_SEE_M then
					changed = true
				else
					w.models[id] = old  -- out of the host's sight: the server world's search decides
				end
			end
		end
		self.client_models = w.models
		if hx and self.client_since and self.t - self.client_since >= M.SETTLE_S then
			self.host_view = { x = hx, y = hy, z = hz, models = w.models }
		end
		if changed and not self.walk then self.walk_after = 0 end
		self.cwalk = nil
		self.cwalk_after = self.t + CLIENT_RESCAN_S
	end
end

-- The stable nearest to `player` within `range` metres, or nil.
function S:near(world, player, range)
	local x, y, z = world:position(player)
	if not x then return nil end
	local best, best_d = nil, range
	for _, b in ipairs(self.stables) do
		local d = dist(b, x, y, z)
		if d <= best_d then best, best_d = b, d end
	end
	return best
end

-- The stable under `player`'s crosshair within `range` metres: the camera ray passes within KEY_RAY_M of its middle
-- (PROP_HEIGHT above its place), the nearest to the ray. No camera: the nearest stable within range. nil when none.
function S:under_crosshair(world, player, range)
	local px, py, pz = world:position(player)
	if not px then return nil end
	local ray = camera(world, player)
	if not ray then return self:near(world, player, range) end
	local best, best_perp
	for _, b in ipairs(self.stables) do
		if dist(b, px, py, pz) <= range then
			local vx, vy, vz = b.x - ray.ox, b.y + PROP_HEIGHT - ray.oy, b.z - ray.oz
			local t = vx * ray.dx + vy * ray.dy + vz * ray.dz
			if t > 0 then
				local ex, ey, ez = vx - ray.dx * t, vy - ray.dy * t, vz - ray.dz * t
				local perp = math.sqrt(ex * ex + ey * ey + ez * ez)
				if perp <= KEY_RAY_M and (not best or perp < best_perp) then best, best_perp = b, perp end
			end
		end
	end
	return best
end

-- The prop with a collider nearest to `player` within `range` metres (not a player), or nil.
local function prop_nearest(world, player, range)
	local px, py, pz = world:position(player)
	if not px then return nil end
	local best, best_d = nil, range
	for _, id in ipairs(world:entities()) do
		if id ~= player then
			local x, y, z = world:position(id)
			if x then
				local d = math.sqrt((x - px) ^ 2 + (y - py) ^ 2 + (z - pz) ^ 2)
				if d <= best_d and not is_player(world, id) and has(world, "StaticCollider", id) then best, best_d = id, d end
			end
		end
	end
	return best
end

-- beastmaster.stable mark: the prop `player` looks at (no camera data: the one beside them) is a stable. The reply.
function S:mark(world, player)
	if not player or player == 0 then return "Mark a stable from the chat in the game (no player here)." end
	local id, nearest = nil, false
	if camera(world, player) then
		id = aim.pick(world, player, MARK_REACH_M, function(e)
			return not is_player(world, e) and (has(world, "StaticCollider", e) or self:model_of(world, e) ~= nil)
		end)
	else
		id = prop_nearest(world, player, MARK_NEAR_M)
		nearest = id ~= nil
	end
	if not id then return "Look at a prop within 8 m (a post, a trough, a fence), or stand next to one, and try again." end
	local x, y, z = world:position(id)
	for _, b in ipairs(self.stables) do
		if dist(b, x, y, z) <= SAME_M then return "That is a stable already." end
	end
	local model = self:model_of(world, id) or "-"
	self.marks[#self.marks + 1] = { x = x, y = y, z = z, model = model, entity = id }
	self:rebuild(world)
	self.env.log(string.format("prop %d at %d, %d, %d (model %s) marked as a stable", id, round(x), round(y), round(z), model))
	if nearest then
		return string.format("Marked: the prop nearest to you (at %d, %d, %d) is a stable now.", round(x), round(y), round(z))
	end
	return string.format("Marked: this prop is a stable now (at %d, %d, %d).", round(x), round(y), round(z))
end

-- beastmaster.stable unmark: the marked prop nearest to `player` is no stable. The reply.
function S:unmark(world, player)
	local x, y, z
	if player and player ~= 0 then x, y, z = world:position(player) end
	if not x then return "Unmark a stable from the chat in the game (no player here)." end
	local best, best_d = nil, MARK_REACH_M
	for i, m in ipairs(self.marks) do
		local d = dist(m, x, y, z)
		if d <= best_d then best, best_d = i, d end
	end
	if not best then return "No marked stable within 8 m." end
	local m = table.remove(self.marks, best)
	self:rebuild(world)
	return string.format("Unmarked: the prop at %d, %d, %d is no stable now.", round(m.x), round(m.y), round(m.z))
end

-- beastmaster.stable list: this world's stables (the distance from `player` when there is one); `had`: the world has
-- had a stable. The reply.
function S:list_text(world, player, had)
	if #self.stables == 0 then
		local how = "build " .. M.what_to_build() .. " in your base, or mark a prop with " ..
			"beastmaster.stable mark."
		if had then return "No stable in this world now (there was one: only active beasts can be called): " .. how end
		return "No stable in this world yet: " .. how
	end
	local x, y, z
	if player and player ~= 0 then x, y, z = world:position(player) end
	local lines = {}
	for i, b in ipairs(self.stables) do
		lines[#lines + 1] = string.format("%d. %s at %d, %d, %d%s%s", i, b.kind == "built" and b.name or "marked prop",
			round(b.x), round(b.y), round(b.z), b.reported and ", seen by a player's game" or "",
			x and string.format(" (%d m from you)", round(dist(b, x, y, z))) or "")
	end
	return table.concat(lines, "\n")
end

return M
