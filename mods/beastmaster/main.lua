-- Beastmaster: tame wild beasts, keep them in a collection, call them to fight beside you.
-- Tests: tests/test_beastmaster.cpp.
local species = require("species")
local roster = require("roster")
local tame_mod = require("tame")
local pack_mod = require("pack")
local aim = require("aim")
local health_mod = require("health")
local wild_mod = require("wild")
local rare_mod = require("rare")
local loyalty = require("loyalty")
local slots = require("slots")
local stable_mod = require("stable")
local home_mod = require("home")
local training_mod = require("training")
local grounds_mod = require("grounds")
local window_mod = require("beasts_window")
local talents = require("talents")
local wire = require("wire")
local remote_mod = require("remote")
local client_mod = require("client")
local beast_line = require("beast_line")
local markers_mod = require("markers")

emf.emits({ "StartGuardingEvent" })

local beasts = roster.new()  -- every player's collection, kept with the world
local loaded = false          -- the world's data has arrived (saving before that could overwrite it)
local last_hit = {}           -- player entity -> {id, at}: what hit them last (attack's choice without a crosshair target)
local taming                  -- the taming channels (made below; reset with each world)
local window                  -- the Beasts window (made below)
local on_first_stable         -- the world's first stable was found or marked (made below)
local on_stables_changed      -- the reported stables changed (made below: saved)
local at_stable_open          -- a player pressed E at a stable's prompt (made below)
local release_asked = {}      -- account -> {name, at}: a release by chat asked once (reset with each world)
local next_hud = 0            -- when the HUD line is written next (reset with each world: the clock may restart)
local remote                  -- the screens of players who joined, sent to their games (made below)
local client                  -- on a joined player's game: what the server's Beastmaster sends (made below)

local function setting(key)
	return emf.setting(key)
end

-- A note for a player: on screen for the host and for a player who joined with Beastmaster on their game (a message),
-- else a chat line.
local function tell(world, player, text)
	if player == emf.local_player() then
		emf.notify(text)
	elseif not (remote and remote:note(world, player, text)) then
		emf.tell(world, player, text)
	end
end

local function now(world)
	return world:tick() / 60
end

-- the stables in the world (built ones and the host's marks; the marks are kept with the world)
local stables = stable_mod.new({ now = now, log = emf.log, on_found = function(world) on_first_stable(world) end,
	on_open = function(world, player, entity) at_stable_open(world, player, entity) end,
	on_change = function(world) on_stables_changed(world) end })

-- The level of a player's character.
local function level_of(world, player)
	local l = player and world:get("Level", player)
	return l and l.dataStorage and l.dataStorage[1] or 0
end

-- How many beasts `player` may have out at once: their active slots; nil while their level cannot be read (0: a
-- player who just joined or loaded).
local function open_slots(world, player)
	local level = level_of(world, player)
	if level <= 0 then return nil end
	return slots.unlocked(level, setting)
end

local training  -- the beasts in training at a Training Grounds (made below; save() writes its lines)
local function save()
	if not loaded then return end
	local text = beasts:serialize()
	for _, more in ipairs({ stables:serialize(), training and training:serialize() or "" }) do
		if more ~= "" then text = text ~= "" and (text .. "\n" .. more) or more end
	end
	emf.save_world_data(text)
end
on_stables_changed = function() save() end

-- tougher beasts, watched from their spawn: a wild one by its species' health_mul (the wild bear: 2.5x health), a
-- tamed one by its role and loyalty (`mul`)
local health = health_mod.new()
local function spawned(world, id, sp, mul)
	health:watch(world, id, sp, mul)
end

-- the markers over the players' beasts and on the world's stables (1.16)
local markers = markers_mod.new({ setting = setting, now = now })

-- the beasts out in the world
local home  -- the beasts at home in a Large Stable (made below)
local trainees  -- the beasts in training shown at their Training Grounds (made below)
local pack = pack_mod.new({
	setting = setting,
	now = now,
	tell = tell,
	beasts = function() return beasts end,
	save = save,
	on_spawn = spawned,
	mode = function(account) return beasts:mode(account) end,
	slots = open_slots,
	stabled = function() return beasts:stabled() end,
	leave_home = function(world, b)
		home:leave(world, b)
		trainees:leave(world, b)
		training:back(b)  -- called: its training ends
	end,
	at_home = function(id) return home:has(id) or trainees:has(id) end,
})

home = home_mod.new({
	now = now,
	setting = setting,
	beasts = function() return beasts end,
	homes = function() return stables:homes() end,
	present = function() return pack.present end,
	log = emf.log,
	on_spawn = spawned,
	tell = tell,
	gain = function(world, account, b) pack:gain(world, { account = account, beast = b }) end,
})

-- the beasts in training at a Training Grounds (1.21, the beasts pack's)
training = training_mod.new({
	now = now,
	setting = setting,
	beasts = function() return beasts end,
	grounds = function() return stables:grounds() end,
	present = function() return pack.present end,
	gain = function(world, account, b) pack:gain(world, { account = account, beast = b }) end,
	tell = tell,
	save = save,
	log = emf.log,
})

-- the beasts in training shown on their Training Grounds (1.21): the place module home.lua with the grounds' scenes
trainees = home_mod.new({
	now = now,
	setting = setting,
	beasts = function() return beasts end,
	homes = function() return {} end,
	present = function() return pack.present end,
	log = emf.log,
	on_spawn = spawned,
	tell = tell,
	gain = function(world, account, b) pack:gain(world, { account = account, beast = b }) end,
}, grounds_mod.place(function() return stables:grounds() end, function() return training:room() end))

-- rare wild bears, aurochs and snakes (with their content packs)
local wild = wild_mod.new({ setting = setting, now = now, on_spawn = spawned })

-- the slayer's bonus for a wild rare snake
local rare = rare_mod.new()

emf.on_world_data(function(world, text)
	beasts = roster.parse(text)
	stables:load(text)
	training:load(text)
	window:reset()
	if remote then remote:reset() end
	release_asked = {}
	pack:reset()
	markers:reset()
	home:reset()
	trainees:reset()
	health:reset()
	wild:reset()
	rare:reset()
	taming:reset()
	last_hit = {}
	next_hud = 0
	loaded = true
end)

local as_player = nil  -- beastmaster.as: the player a word runs as (who answers them while it runs)

-- Who a command is for: the player beastmaster.as names, the player who typed it in the chat, else the host. Returns
-- entity, account text.
local function who(world)
	if as_player then return as_player, world:account(as_player) end
	local s = emf.sender()
	if s and s.chat then return s.entity, s.account end
	local p = emf.local_player()
	return p, p and world:account(p) or nil
end

-- The players' commands are also words of the bare /beastmaster (players who joined type "/beastmaster call Wolf").
local words, word_list = {}, {}
local function command(word, help, fn)
	emf.command(word, help, fn, { players = true })
	words[word] = fn
	word_list[#word_list + 1] = word
end

local function loyalty_of(b)
	return loyalty.level(b.points, setting("loyalty_pace"))
end

local function state_text(b, aside)
	local rest = b.rest > 0 and string.format("rests %d:%02d", math.floor(b.rest / 60), math.floor(b.rest % 60))
	if b.out and rest then return rest .. ", then comes back" end  -- it fell and comes back by itself (back_after_rest)
	if b.out then return aside and "aside" or "out" end
	if b.training then return rest and (rest .. ", training") or "training" end
	return rest or "at home"
end

local function active_count(account)
	local n = 0
	for _, x in ipairs(beasts:list(account)) do
		if x.active then n = n + 1 end
	end
	return n
end

-- The beasts in the stable: all but the active ones once the world has a stable (it holds collection_max of them);
-- before that the whole collection.
local function stored_count(account)
	return #beasts:list(account) - (beasts:stabled() and active_count(account) or 0)
end

-- How many beasts `player` may have in all: collection_max in the stable besides the active ones (before a stable,
-- collection_max). `joins_active`: a new beast takes a free active slot when there is one (a tame).
local function collection_cap(world, player, account, joins_active)
	local max = setting("collection_max")
	if not beasts:stabled() then return max end
	local active = active_count(account)
	local free = (joins_active and active < (open_slots(world, player) or math.huge)) and 1 or 0
	return max + active + free
end

-- ---- the stable's actions (the Beasts window, and the chat for players who joined); each returns what it did ----
local stable_do = {}

function stable_do.make_active(world, account, player, b)
	if b.active then return b.name .. " is active already." end
	if active_count(account) >= (open_slots(world, player) or 1) then return "No free active slot: store a beast first." end
	b.active = true
	save()
	return b.name .. " is active."
end

function stable_do.store(world, account, b)
	if not b.active then return b.name .. " is in the stable already." end
	local max = setting("collection_max")
	if stored_count(account) >= max then
		return string.format("Your stable is full (%d beasts): release one first.", max)
	end
	b.active = false
	if b.out then pack:dismiss(world, account, b) end
	save()
	return b.name .. " goes to the stable."
end

function stable_do.release(world, account, b)
	pack:release(world, account, b)
	return b.name .. " is gone: you let it go."
end

local function plural(n, word)
	return string.format("%d %s%s", n, word, n == 1 and "" or "s")
end

-- A talent point spent on `b` (talent `t`).
function stable_do.spend(world, account, b, t)
	local ok, said = talents.spend(b, t, setting("loyalty_pace"))
	if ok then
		pack:retalent(world, b)
		save()
	end
	return said
end

-- Every talent point of `b` back, free.
function stable_do.reset_talents(world, account, b)
	if talents.spent(b) == 0 then return b.name .. " has no talent points spent." end
	talents.reset(b)
	pack:retalent(world, b)
	save()
	return string.format("%s's talents are reset: %s to spend.", b.name,
		plural(talents.points(b, setting("loyalty_pace")), "point"))
end

-- the Beasts window (the host, at a stable)
local STABLE_REACH_M = 4  -- E opens the window with a stable under the crosshair this close
local actions = {}  -- each returns the text to show (a command's reply, a note after a wheel pick or a window click)
local window_env = {
	setting = setting,
	now = now,
	beasts = function() return beasts end,
	level = level_of,
	state = function(b, account) return state_text(b, pack:is_aside(account)) end,
	mode = function(account) return beasts:mode(account) end,
	wind_left = function(world, b) return pack.fight:wind_left(world, b) end,
	make_active = stable_do.make_active,
	store = stable_do.store,
	release = stable_do.release,
	spend = stable_do.spend,
	reset_talents = stable_do.reset_talents,
	can_train = function() return #training:grounds() > 0 end,
	train = function(world, account, player, b) return actions.train(world, player, account, b.name) end,
	untrain = function(world, account, b) return actions.untrain(world, nil, account, b.name) end,
}
window = window_mod.new(window_env)

-- What a player who joined (no window) can do at a stable, by chat.
local AT_STABLE_CHAT = "At a stable: /beastmaster active <name or number> (it can be called), /beastmaster store " ..
	"<name or number>, /beastmaster release <name or number> (twice), /beastmaster talents <name or number> [reset], " ..
	"/beastmaster talent <name or number> <talent>. /beastmaster list shows your beasts."

-- E at a stable's prompt (on `entity`): the host gets the Beasts window, and so does a player who joined with
-- Beastmaster on their own game (drawn there); anyone else, the stable's chat commands. An entity that is no stable's
-- now (its id given out again) does nothing.
at_stable_open = function(world, player, entity)
	if not world:is_server() or not loaded or not player or player == 0 then return end
	if not stables:stable_of(world, entity) then return end
	local host = emf.local_player()
	if host and host ~= 0 and player == host then
		window:show(world, player)
	elseif not (remote:is_remote(world, player) and remote:open_window(world, player)) then
		emf.tell(world, player, AT_STABLE_CHAT)
	end
end

-- The E key (stable_key): the Beasts window at a stable under the host's crosshair that has no prompt (the game refused
-- it there, or it has no entity for one); at a stable with the prompt the prompt opens it.
-- E on someone else's game: the game's own prompt at a stable may open the Beasts window there (client.lua shows a
-- window the server opens only after a press of E or the stable key).
emf.on_key("E", function(world)
	if not world:is_server() then client:on_open_key(world) end
end)

emf.on_key("stable_key", function(world)
	if not world:is_server() then  -- someone else's game: the server is asked
		client:on_stable_key(world)
		return
	end
	if not loaded then return end
	local player = emf.local_player()
	if not player or player == 0 then return end
	local b = stables:under_crosshair(world, player, STABLE_REACH_M)
	if not b or stables:prompted(world, b) then return end
	window:show(world, player)
end)

command("list", ": your beasts", function(world, args)
	local _, account = who(world)
	local list = account and beasts:list(account) or {}
	if #list == 0 then
		emf.reply("You have no beasts yet. Tame one: beat a wild wolf, boar, sabretooth, hound, bear, aurochs or snake down, or feed it its bait.")
		return
	end
	local head = string.format("Your beasts (%d/%d):", #list, setting("collection_max"))
	if beasts:stabled() then
		head = string.format("Your beasts (%d active, %d/%d in the stable):", active_count(account), stored_count(account),
			setting("collection_max"))
	end
	local lines = { head }
	local aside = pack:is_aside(account)
	for i, b in ipairs(list) do
		lines[#lines + 1] = string.format("%d. %s - %s, loyalty %d, %s%s", i, b.name, b.species, loyalty_of(b),
			state_text(b, aside), (beasts:stabled() and b.active) and ", active" or "")
	end
	if pack:aside_by_command(account) then
		lines[#lines + 1] = "They stepped aside: Come back on the wheel or /beastmaster aside brings them back."
	end
	emf.reply(table.concat(lines, "\n"))
end)

emf.command("species", "<animation graph guid> <faction number> [scale]: which beast a creature is (for modders)",
	function(world, args)
		local s = species.of(args[1], tonumber(args[2] or ""), tonumber(args[3] or ""))
		emf.reply(s and s.id or "unknown")
	end)

-- ---- taming ----
taming = tame_mod.new({
	setting = setting,
	calm = pack_mod.calm,
	now = now,
	tell = tell,
	on_tamed = function(world, player, id, sp)
		local account = world:account(player)
		local b = account and beasts:add(account, sp.id, sp.name, collection_cap(world, player, account, true))
		if not b then
			tell(world, player, (beasts:stabled() and "Your stable is full" or "Your collection is full") .. ": the " ..
				species.word(sp) .. " follows you until it falls.")
			return
		end
		pack:adopt(account, b, id, world)
		-- it takes a free slot; with none free it leaves (to the stable once the world has had one, else home)
		local said = string.format("You tamed %s: %s", species.a(sp), b.name)
		local open = open_slots(world, player) or math.huge  -- a level not read yet: it stays
		if beasts:stabled() then
			if active_count(account) < open then
				b.active = true
			else
				pack:dismiss(world, account, b)
				said = said .. ". It goes to your stable (no free active slot)."
			end
		elseif pack:out_count(account) > open then
			pack:dismiss(world, account, b)
			local out = pack:out_count(account)
			said = string.format("%s. It goes home (%d active slot%s, %d beast%s out).", said, open, open == 1 and "" or "s",
				out, out == 1 and "" or "s")
		end
		save()
		tell(world, player, said)
	end,
})

-- Nil, or what to say: no room for one more beast (a free active slot or room in the stable; before a stable, in the
-- collection).
local function collection_full(world, player)
	local account = world:account(player)
	if not account or #beasts:list(account) < collection_cap(world, player, account, true) then return nil end
	return beasts:stabled() and "Your stable is full." or "Your collection is full."
end

-- ---- actions (the table is made above the window's env): each returns the text to show ----

function actions.tame(world, player)
	local full = collection_full(world, player)
	if full then return full end
	return taming:tame(world, player)
end

function actions.feed(world, player)
	local full = collection_full(world, player)
	if full then return full end
	return taming:feed(world, player)
end

function actions.call(world, player, account, key)
	local b = account and beasts:find(account, key)
	if not b then return "No such beast (beastmaster.list shows yours)." end
	return pack:call(world, player, account, b)
end

function actions.dismiss(world, player, account, key)
	if not account then return "Your account is not known yet." end
	if key == "all" or key == "" then
		local said = {}
		for _, b in ipairs(beasts:list(account)) do
			if b.out then said[#said + 1] = pack:dismiss(world, account, b) end
		end
		return #said > 0 and table.concat(said, "\n") or "No beast is out."
	end
	local b = beasts:find(account, key)
	if not b then return "No such beast (beastmaster.list shows yours)." end
	return pack:dismiss(world, account, b)
end

-- A beast goes to train at the nearest Training Grounds with room (from the player; 1.21).
function actions.train(world, player, account, key)
	if not account then return "Your account is not known yet." end
	local b = beasts:find(account, key)
	if not b then return "No such beast (beastmaster.list shows yours)." end
	if b.training then return b.name .. " is training already (beastmaster.untrain brings it back)." end
	if b.out then return b.name .. " is out: send it home first (beastmaster.dismiss)." end
	if #training:grounds() == 0 then
		return "No Training Grounds in this world yet: build one in your base (the beasts pack's, crafted at the Workbench)."
	end
	local x, y, z = world:position(player)
	if not x then return "Your place is not known yet." end
	local free, nearest = training:nearest(x, y, z)
	if not free then
		return string.format("The Training Grounds is full (%d of %d): bring a beast back first, or build another.",
			training:count(nearest), training:room())
	end
	local ok, why = training:send(b, free)
	if not ok then return b.name .. " cannot train now (" .. tostring(why) .. ")." end
	home:leave(world, b)
	return b.name .. " goes to the Training Grounds: it trains there and earns loyalty by the game's clock while you are "
		.. "in the world."
end

-- A beast in training comes back to the stable (1.21).
function actions.untrain(world, player, account, key)
	if not account then return "Your account is not known yet." end
	local b = beasts:find(account, key)
	if not b then return "No such beast (beastmaster.list shows yours)." end
	if not training:back(b) then return b.name .. " is not training." end
	trainees:leave(world, b)
	return b.name .. " is back in the stable."
end

local ATTACK_RANGE_M, HIT_MEMORY_S = 40, 10
local ATTACK_CONE_DEG = 30  -- Attack with nothing under the crosshair: a monster at most this far off where you look

function actions.come_back(world, player, account)
	if not account then return "Your account is not known yet." end
	return pack:come_back(world, account, player)
end

-- The wheel's stable item (while the world has no stable): what to build.
function actions.stable_hint(world, player, account)
	local what = stable_mod.what_to_build()
	if beasts:stabled() then
		return "Your world's stables are gone: build " .. what .. " in your base, and press E at it to choose your " ..
			"active beasts."
	end
	return "Build " .. what .. " in your base: press E at it to choose your active beasts. Until your world has one, " ..
		"you can call any beast, as many at once as your active slots."
end

function actions.attack(world, player, account)
	if not account then return "Your account is not known yet." end
	local function foe(id) return pack_mod.fightable(world, id) end
	-- the creature under the crosshair (an animal too); else the nearest monster where you look; else what hit you
	local target = aim.pick(world, player, ATTACK_RANGE_M, foe)
	if not target then
		target = aim.cone(world, player, ATTACK_RANGE_M, ATTACK_CONE_DEG, function(id) return pack_mod.hostile(world, id) end)
	end
	local h = last_hit[player]
	if not target and h and now(world) - h.at <= HIT_MEMORY_S and foe(h.id) then
		local px, py, pz = world:position(player)
		local x, y, z = world:position(h.id)
		if px and x and math.sqrt((x - px) ^ 2 + (y - py) ^ 2 + (z - pz) ^ 2) <= ATTACK_RANGE_M then target = h.id end
	end
	if not target then return "No monster where you look (within 40 m): look at the creature to attack." end
	local n = pack:attack(world, account, target)
	return n > 0 and string.format("Your beasts attack (%d).", n) or "No beast of yours is out."
end

command("tame", ": tame the beast you look at (beaten under 25 % health)", function(world, args)
	emf.reply(actions.tame(world, who(world)))
end)

command("feed", ": feed the beast you look at its bait (3 times to tame it)", function(world, args)
	emf.reply(actions.feed(world, who(world)))
end)

command("train", "<name or number>: send a beast to train at the nearest Training Grounds (it earns loyalty by the "
	.. "game's clock); nothing: where your beasts train", function(world, args)
	local player, account = who(world)
	if #args == 0 then
		emf.reply(account and training:text(account) or "Your account is not known yet.")
		return
	end
	emf.reply(actions.train(world, player, account, table.concat(args, " ")))
end)

command("untrain", "<name or number>: bring a beast in training back to the stable", function(world, args)
	local player, account = who(world)
	emf.reply(actions.untrain(world, player, account, table.concat(args, " ")))
end)

command("call", "<name or number>: call a beast of your collection to your side", function(world, args)
	local player, account = who(world)
	emf.reply(actions.call(world, player, account, table.concat(args, " ")))
end)

command("dismiss", "[name, number or all]: send a beast home; nothing or all: every beast that is out", function(world, args)
	local player, account = who(world)
	emf.reply(actions.dismiss(world, player, account, table.concat(args, " ")))
end)

command("attack", ": your beasts out attack the creature you look at", function(world, args)
	local player, account = who(world)
	emf.reply(actions.attack(world, player, account))
end)

-- ---- modes: how all of a player's beasts fight ----
local MODE_SAYS = {
	passive = "Your beasts are passive: they ignore fights and follow you (Attack still sends them).",
	defending = "Your beasts are defending: they fight what attacks you or them.",
	aggressive = "Your beasts are aggressive: they also go after hostile creatures near you.",
}
local NEXT_MODE = { defending = "aggressive", aggressive = "passive", passive = "defending" }

function actions.mode(world, player, account)
	if not account then return "Your account is not known yet." end
	local m = NEXT_MODE[beasts:mode(account)]
	beasts:set_mode(account, m)
	pack:end_orders(world, account)
	save()
	return MODE_SAYS[m]
end

command("mode", "[passive, defending or aggressive]: how your beasts fight; nothing: the mode now", function(world, args)
	local _, account = who(world)
	if not account then
		emf.reply("Your account is not known yet.")
		return
	end
	local word = args[1] and args[1]:lower()
	if not word then
		emf.reply(MODE_SAYS[beasts:mode(account)])
	elseif not beasts:set_mode(account, word) then
		emf.reply("A mode is passive, defending or aggressive.")
	else
		pack:end_orders(world, account)
		save()
		emf.reply(MODE_SAYS[word])
	end
end)

command("markers", " on|off: a marker over each of your beasts out, for you (the world's setting otherwise)",
	function(world, args)
		local _, account = who(world)
		emf.reply(account and markers:choose(account, "markers", args[1] and args[1]:lower()) or "Your account is not known yet.")
	end)

command("stable_markers", " on|off: the stables on your map and nearby, for you (the world's setting otherwise)",
	function(world, args)
		local _, account = who(world)
		emf.reply(account and markers:choose(account, "stable_markers", args[1] and args[1]:lower()) or
			"Your account is not known yet.")
	end)

command("aside", ": your beasts out step aside (leave for now) or come back; while you build they do it on their own",
	function(world, args)
		local player, account = who(world)
		emf.reply(account and pack:toggle_aside(world, account, player) or "Your account is not known yet.")
	end)

command("slots", ": your active slots (how many beasts can be out) and when the next one opens", function(world, args)
	local player = who(world)
	local lvl = level_of(world, player)
	local n = slots.unlocked(lvl, setting)
	local next_slot, next_level = slots.next(lvl, setting)
	if next_slot then
		emf.reply(string.format("Active slots: %d (slot %d at level %d)", n, next_slot, next_level))
	else
		emf.reply(string.format("Active slots: %d", n))
	end
end)

command("rename", "<name or number> <new name>: rename one of your beasts (1-20 characters)", function(world, args)
	local _, account = who(world)
	local b = account and beasts:find(account, args[1])
	if not b then
		emf.reply("No such beast (beastmaster.list shows yours).")
		return
	end
	local old = b.name
	local ok, why = beasts:rename(account, b, table.concat(args, " ", 2))
	if not ok then
		emf.reply(why)
		return
	end
	save()
	emf.reply(string.format("%s is now called %s.", old, b.name))
end)

-- At a stable, by chat (players who joined have no window): active, store, release (asked twice).
local CHAT_STABLE_M = 10    -- within this of a stable
local CHAT_CONFIRM_S = 10   -- release: the same command again within this

-- Why `player` cannot act at a stable now (none within CHAT_STABLE_M), or nil; `bed`: the text says what a stable is.
local function away_from_stable(world, player, bed)
	if player and stables:near(world, player, CHAT_STABLE_M) then return nil end
	if bed then
		return string.format("Go to a stable %s within %d m to do that.", stable_mod.what_it_is(), CHAT_STABLE_M)
	end
	return string.format("Go to a stable (within %d m) to do that.", CHAT_STABLE_M)
end

local NO_BEAST = "No such beast (beastmaster.list shows yours)."

local function stable_command(word, help, fn)
	command(word, help, function(world, args)
		local player, account = who(world)
		if not account then
			emf.reply("Your account is not known yet.")
			return
		end
		local b = beasts:find(account, table.concat(args, " "))
		if not b then
			emf.reply(NO_BEAST)
			return
		end
		local away = away_from_stable(world, player)
		if away then
			emf.reply(away)
			return
		end
		emf.reply(fn(world, player, account, b))
	end)
end

stable_command("active", "<name or number>: make a beast active (at a stable): it can be called",
	function(world, player, account, b) return stable_do.make_active(world, account, player, b) end)

stable_command("store", "<name or number>: put a beast into the stable (at a stable)",
	function(world, player, account, b) return stable_do.store(world, account, b) end)

stable_command("release", "<name or number>: let a beast go for good (at a stable; type it twice)",
	function(world, player, account, b)
		local t = now(world)
		local asked = release_asked[account]
		if asked and asked.name == b.name and t - asked.at <= CHAT_CONFIRM_S then
			release_asked[account] = nil
			return stable_do.release(world, account, b)
		end
		release_asked[account] = { name = b.name, at = t }
		return string.format("Type the same command again within %d s to let %s go for good.", CHAT_CONFIRM_S, b.name)
	end)

-- Talents: listed anywhere; spent and reset at a stable (the Beasts window's beast tab does the same for the host).
-- The list is one reply a line (a reply holds at most 1023 bytes).
local function reply_talents(b)
	emf.reply(string.format("%s's talents: %s", b.name, talents.summary(b, setting("loyalty_pace"))))
	for _, t in ipairs(talents.LIST) do emf.reply(talents.row_text(b, t)) end
end

command("talents", "<name or number> [reset]: a beast's talents; reset (at a stable): every point back, free",
	function(world, args)
		local player, account = who(world)
		if not account then
			emf.reply("Your account is not known yet.")
			return
		end
		local reset = #args > 1 and args[#args]:lower() == "reset"
		local key = table.concat(args, " ", 1, reset and #args - 1 or #args)
		if key == "" then
			emf.reply("/beastmaster talents <name or number> [reset]")
			return
		end
		local b = beasts:find(account, key)
		if not b then
			emf.reply(NO_BEAST)
			return
		end
		if not reset then
			reply_talents(b)
			return
		end
		emf.reply(away_from_stable(world, player, true) or stable_do.reset_talents(world, account, b))
	end)

local TALENT_NAMES = {}
for _, t in ipairs(talents.LIST) do TALENT_NAMES[#TALENT_NAMES + 1] = t.name end
local WHICH_TALENT = string.format("A talent is %s or %s: /beastmaster talent <name or number> <talent>",
	table.concat(TALENT_NAMES, ", ", 1, #TALENT_NAMES - 1), TALENT_NAMES[#TALENT_NAMES])

command("talent", "<name or number> <talent>: spend a talent point (at a stable): strength, toughness, swiftness, second wind, ambush or guard",
	function(world, args)
		local player, account = who(world)
		if not account then
			emf.reply("Your account is not known yet.")
			return
		end
		local key, t = talents.split(table.concat(args, " "))
		if not t then
			emf.reply(WHICH_TALENT)
			return
		end
		local b = beasts:find(account, key)
		if not b then
			emf.reply(NO_BEAST)
			return
		end
		emf.reply(away_from_stable(world, player, true) or stable_do.spend(world, account, b, t))
	end)

local WORDS_HELP = " (call takes a name or number; dismiss a name, a number, or nothing for all)"
emf.command(emf.id, "<word>: " .. table.concat(word_list, ", ") .. WORDS_HELP,
	function(world, args)
		local word = args[1] and args[1]:lower()
		local fn = word and words[word]
		if not fn then
			emf.reply("/beastmaster " .. table.concat(word_list, " | ") .. WORDS_HELP)
			return
		end
		fn(world, { table.unpack(args, 2) })
	end, { players = true })

-- ---- the wheel (the host; players who join use the chat commands) ----
local WHEEL_CALLS = 6
local wheel_entries = {}  -- what each wheel item does: {action, key}
local pending = nil       -- the host's pick, run on the server world's next step

local recent = {}       -- label -> {entry, at}: what each label published in the last 2 minutes meant
local own_entries = {}  -- the own wheel's entries as they were when it opened

-- A pick on the shared wheel: by the label that was shown (the list may have moved since the wheel opened).
local function picked(world, choice, label)
	local r = label and recent[label]
	pending = r and r.entry or (choice and wheel_entries[choice]) or nil
end

-- A pick on Beastmaster's own wheel: by its place in the list as it was when the wheel opened.
local function picked_own(world, choice)
	pending = choice and own_entries[choice] or nil
end

-- The wheel's items for `player`'s account (the host's, or a joined player's on their own game): labels and what each
-- does.
local function wheel_items(world, player)
	local account = player and world:account(player)
	local labels, entries = {}, {}
	local function add(label, entry)
		labels[#labels + 1] = label
		entries[#entries + 1] = entry
	end
	-- short labels: a name of at most 20 characters, a detail in brackets of at most 13 (the wheel's slices are narrow)
	add("Tame (weakened)", { action = "tame" })
	add("Feed (bait)", { action = "feed" })
	add("Attack", { action = "attack" })
	if account then add("Mode: " .. beasts:mode(account):gsub("^%l", string.upper), { action = "mode" }) end
	if stables:count() == 0 then
		add(stable_mod.craftable(now(world)) and "Stable: Workbench" or "Stable: animal bed", { action = "stable_hint" })
	end
	if account and pack:aside_by_command(account) then add("Come back", { action = "come_back" }) end
	local list = account and beasts:list(account) or {}
	local out = 0
	for _, b in ipairs(list) do
		if b.out then
			out = out + 1
			local rest = b.rest > 0  -- it fell and comes back after its rest (back_after_rest): dismissing leaves it home
				and string.format(" (rests %d:%02d)", math.floor(b.rest / 60), math.floor(b.rest % 60)) or ""
			add("Dismiss " .. b.name .. rest, { action = "dismiss", key = b.name })
		end
	end
	if out > 1 then add("Dismiss all", { action = "dismiss", key = "all" }) end
	local shown = 0
	local stabled = beasts:stabled()
	for _, b in ipairs(list) do
		if not b.out and shown < WHEEL_CALLS and (b.active or not stabled) then
			shown = shown + 1
			local label
			if b.rest > 0 then
				label = string.format("Call %s (rests %d:%02d)", b.name, math.floor(b.rest / 60), math.floor(b.rest % 60))
			elseif b.name:lower():find(species.word(b.species), 1, true) == 1 then
				label = "Call " .. b.name  -- "Wolf 2" says it is a wolf
			else
				label = string.format("Call %s (%s)", b.name, species.word(b.species))
			end
			add(label, { action = "call", key = b.name })
		end
	end
	return labels, entries
end

-- The "summons" wheel group (lua 1.9): with shared_wheel on, Beastmaster's section is on the one wheel that every class
-- mod's key opens (Necromancer's too); off, it keeps its own wheel and is not on the shared one.
local GROUP = "summons"
local function publish(world)
	local labels, entries = wheel_items(world, emf.local_player())
	wheel_entries = entries
	local t = now(world)
	for i, l in ipairs(labels) do recent[l] = { entry = entries[i], at = t } end
	for l, r in pairs(recent) do
		if t - r.at > 120 or t < r.at then recent[l] = nil end  -- old, or the clock restarted
	end
	if setting("shared_wheel") then
		emf.wheel_section(GROUP, "Beasts", labels, picked)
	else
		emf.wheel_section(GROUP, "Beasts", nil)
	end
	return labels
end

emf.on_key("wheel_key", function(world)
	if not world:is_server() then  -- someone else's game: the server sends the items
		client:on_wheel_key(world)
		return
	end
	local labels = publish(world)
	if setting("shared_wheel") then
		emf.wheel_group(GROUP, "Summons", emf.setting("wheel_key"))
	else
		own_entries = wheel_entries
		emf.wheel("Beasts", labels, emf.setting("wheel_key"), picked_own)
	end
end)

-- ---- players who joined: Beastmaster's screens on their own game (mod messages, Lua 1.19) ----
remote = remote_mod.new({
	now = now,
	log = emf.log,
	present = function() return pack.present end,
	loaded = function() return loaded end,
	make_window = function(screen) return window_mod.new(window_env, screen) end,
	stable_key = function(world, player)
		if not loaded then return false end
		local b = stables:under_crosshair(world, player, STABLE_REACH_M)
		return b ~= nil and not stables:prompted(world, b)
	end,
	wheel_items = wheel_items,
	line = function(world, account)
		return beast_line.head(beasts:mode(account)), beast_line.parts(beasts:list(account), pack:is_aside(account))
	end,
	run = function(world, player, account, entry) return actions[entry.action](world, player, account, entry.key) end,
	tell = function(world, player, text) tell(world, player, text) end,
	report = function(world, player, list)
		local x, y, z = world:position(player)
		if not x then return end
		local changed, seen, added, gone = stables:report(world, x, y, z, list)
		if changed then
			emf.log(string.format("stables reported by %s: %d seen, %d added, %d gone", world:account(player) or "?", seen,
				added, gone))
		end
	end,
})
client = client_mod.new({ now = now, log = emf.log })
if emf.net and emf.net.channel then
	emf.net.channel(wire.CHANNEL, {
		version = wire.VERSION,
		to = "both",
		max = wire.MAX,
		on_server = function(world, from, data) remote:on_message(world, from, data) end,
		on_player = function(world, from, data) client:on_message(world, from, data) end,
	})
end

-- The host's own beasts on screen: "Beasts: Wolf (out), Boar (rests 1:20)".
local function update_hud(world)
	local now_s = now(world)
	if now_s < next_hud then return end
	next_hud = now_s + 1
	local account = world:account(emf.local_player())
	local parts = account and beast_line.parts(beasts:list(account), pack:is_aside(account)) or {}
	emf.hud("out", beast_line.text(beast_line.head(account and beasts:mode(account)), parts, 0), "bottom_left")
	window:refresh(world)
	remote:step(world)
	publish(world)  -- the shared wheel shows what the other class mod's key opens with
end

-- beast_health and beast_damage are live: the beasts out and at home follow at the next step
emf.on_setting_changed(function(key)
	if key == "beast_health" or key == "beast_damage" then
		pack:strength_changed()
		home:strength_changed()
		trainees:strength_changed()
	end
end)

emf.on_event("HitEvent", function(world, e)
	pack:on_hit(world, e.rootSourceId, e.targetId, e.healthChange)
	home:on_hit(world, e.rootSourceId, e.targetId)
	trainees:on_hit(world, e.rootSourceId, e.targetId)
	rare:on_hit(world, e.rootSourceId, e.targetId)
	if e.rootSourceId ~= 0 and e.rootSourceId ~= e.targetId and world:get("PlayerState", e.targetId) then
		last_hit[e.targetId] = { id = e.rootSourceId, at = now(world) }
	end
end)

emf.on_event("EntityDiedEvent", function(world, e)
	taming:on_died(e.deadEntityId)
	pack:on_died(e.deadEntityId, world)
	home:on_died(world, e.deadEntityId)
	trainees:on_died(world, e.deadEntityId)
	rare:on_died(world, e.deadEntityId)
end)

local client_steps = 0  -- client world steps since the server world's last (more than stable_mod.CLIENT_STEPS: joined)
emf.on_tick(function(world)
	if not world:is_server() then
		if world:is_client() then
			client_steps = client_steps + 1
			if client_steps > stable_mod.CLIENT_STEPS then
				client:step(world)  -- someone else's game
			else
				stables:client_step(world)
			end
		end
		return
	end
	client_steps = 0
	stables:step(world)
	taming:step(world)
	pack:step(world)
	markers:step(world, pack.present, pack.ids, stables.stables)
	home:step(world)
	trainees:step(world)
	training:step(world)
	wild:step(world)
	health:step(world)
	if pending then
		local p = pending
		pending = nil
		local player = emf.local_player()
		local account = world:account(player)
		emf.notify(actions[p.action](world, player, account, p.key))
	end
	update_hud(world)
end)

-- The world's first stable (found or marked; kept with the world): from now on only active beasts are called. Each
-- player's active slots fill with the beasts out, then the first in list order (a player not in the world: one slot).
on_first_stable = function(world)
	if beasts:stabled() then return end
	beasts:set_stabled()
	for _, account in ipairs(beasts:accounts()) do
		local open = open_slots(world, pack:owner(world, account)) or 1
		local n = 0
		for _, b in ipairs(beasts:list(account)) do
			if b.out and n < open then
				b.active, n = true, n + 1
			end
		end
		for _, b in ipairs(beasts:list(account)) do
			if not b.active and n < open then
				b.active, n = true, n + 1
			end
		end
	end
	save()
	emf.log("this world has its first stable: from now on only active beasts are called")
end

-- Host and admins: the stables (a prop marked as one, the mark taken off, the list).
emf.command("stable", "mark | unmark | list: the prop you look at is a stable; the marked prop nearest you is not; " ..
	"this world's stables (host and admins)", function(world, args)
	local word = args[1] and args[1]:lower() or "list"
	local player = who(world)
	if word == "mark" or word == "unmark" then
		if not loaded then
			emf.reply("This world's data is not loaded yet; try again in a moment.")
			return
		end
		local count = #stables.marks
		emf.reply(word == "mark" and stables:mark(world, player) or stables:unmark(world, player))
		if #stables.marks ~= count then save() end
	elseif word == "list" then
		emf.reply(stables:list_text(world, player, beasts:stabled()))
	else
		emf.reply("beastmaster.stable mark | unmark | list")
	end
end)

-- The monsters Beastmaster holds on you or your beasts now (for live checks).
emf.command("threat", ": the monsters held on you or your beasts now, and for how long", function(world, args)
	local _, account = who(world)
	emf.reply(account and pack.threat:held_text(world, account) or "Your account is not known yet.")
end)

-- Where your beasts at home are and what they do (for live checks).
emf.command("home", ": where your beasts at home are (in a Large Stable) and what they do", function(world, args)
	local _, account = who(world)
	emf.reply(account and home:text(world, account) or "Your account is not known yet.")
end)

-- What your beasts do while you stand still now (for live checks).
emf.command("idle", ": what your beasts do while you stand still now", function(world, args)
	local _, account = who(world)
	emf.reply(account and pack.idle:text(world, account) or "Your account is not known yet.")
end)

-- Each player's movement as this world sees it, and whether their beasts count them in the air (for live checks: on a
-- dedicated server, whether a joined player's glide reaches the server).
emf.command("air", ": each player's movement state as this world sees it, and whether they count as in the air",
	function(world, args)
		local lines = {}
		for _, id in ipairs(world:entities()) do
			if world:get("PlayerState", id) then
				local ok, loc = pcall(function() return world:get("DynamicLocomotion", id) end)
				local x, y = world:position(id)
				lines[#lines + 1] = string.format("player %d: movement state %s, near the ground %s, height %s, in the air %s",
					id, tostring(ok and loc and loc.state), tostring(ok and loc and loc.isNearGround),
					y and string.format("%.1f", y) or "?", tostring(pack.aside:in_air(id, now(world))))
			end
		end
		emf.reply(#lines > 0 and table.concat(lines, "\n") or "No player in the world.")
	end)

-- Admin: set a beast's loyalty (console only, not from the chat; for tests and live checks).
local function loyalty_command(world, args)
	local sender = emf.sender()
	if sender and sender.chat then
		emf.reply("beastmaster.loyalty is for the host's console")
		return
	end
	local _, account = who(world)
	local b = account and beasts:find(account, args[1])
	if not b then
		emf.reply("No such beast (beastmaster.list shows yours).")
		return
	end
	local n = tonumber(args[2] or "")
	if not n or n < 1 or n > loyalty.MAX or n ~= math.floor(n) then
		emf.reply("Loyalty is 1-10.")
		return
	end
	b.points = loyalty.points_for(n, setting("loyalty_pace"))
	save()
	emf.reply(string.format("%s's loyalty is now %d. Talents: %s.", b.name, n, talents.summary(b, setting("loyalty_pace"))))
end
emf.command("loyalty", "<name or number> <1-10>: set a beast's loyalty (admin)", loyalty_command)

-- Admin: add a beast to your collection without taming it (console only, not from the chat).
local function give_command(world, args)
	local sender = emf.sender()
	if sender and sender.chat then
		emf.reply("beastmaster.give is for the host's console")
		return
	end
	local s = species.by_id(args[1])
	if not s then
		local ids = {}
		for _, sp in ipairs(species.list) do ids[#ids + 1] = sp.id end
		emf.reply("no species " .. tostring(args[1]) .. " (" .. table.concat(ids, ", ") .. ")")
		return
	end
	local _, account = who(world)
	if not account then emf.reply("your account is not known yet") return end
	local b, why = beasts:add(account, s.id, s.name, collection_cap(world, nil, account, false))
	if not b then
		emf.reply(beasts:stabled() and string.format("your stable is full (%d beasts)", setting("collection_max")) or why)
		return
	end
	save()
	emf.reply(string.format("added %s (%s) to your collection", b.name, s.id))
end
emf.command("give", "<species>: add a beast to your collection (admin)", give_command)

-- Admin: a wild beast of a species 5 m in front of you, to try taming (console only, not from the chat).
emf.command("spawn", "<species>: a wild beast 5 m in front of you, to try taming (admin)", function(world, args)
	local sender = emf.sender()
	if sender and sender.chat then
		emf.reply("beastmaster.spawn is for the host's console")
		return
	end
	local s = species.by_id(args[1])
	if not s then
		local ids = {}
		for _, sp in ipairs(species.list) do ids[#ids + 1] = sp.id end
		emf.reply("no species " .. tostring(args[1]) .. " (" .. table.concat(ids, ", ") .. ")")
		return
	end
	local me = emf.local_player()
	local x, y, z = world:position(me or 0)
	if not x then emf.reply("your position is not known yet") return end
	local v = emf.player_view and emf.player_view()
	local yaw = math.rad((v and (v.camera_yaw or v.heading)) or 0)
	local id = emf.spawn(world, s.template, x + math.sin(yaw) * 5, y, z + math.cos(yaw) * 5)
	if not id or id == 0 then
		emf.reply("not spawned" .. (s.pack and (" (it needs " .. s.pack .. ")") or " (see the log)"))
		return
	end
	emf.reply(string.format("a wild %s (entity %d) in front of you: beat it under 25 %% health, then Tame (hold F3)",
		s.name, id))
end)

-- ---- beastmaster.as: a player's word run as that player from the host's or the server's console (tests on a
-- dedicated server, where the console has no player of its own) ----

-- The players in the world, lowest entity id first (#k).
local function players_by_id(world)
	local ids = {}
	for _, id in ipairs(world:entities()) do
		if world:get("PlayerState", id) and world:account(id) then ids[#ids + 1] = id end
	end
	table.sort(ids)
	return ids
end

-- The Beasts window for `player` as at a stable (on beast `name`'s tab when given): the host's own, or drawn on a
-- joined player's game. The reply.
local function open_as(world, player, name)
	local host = emf.local_player()
	if host and host ~= 0 and player == host then
		if not window:show(world, player, name) then return "The Beasts window did not open (another window is open)." end
		return "Opened the Beasts window."
	end
	if remote:is_remote(world, player) then
		if remote:open_window(world, player, name) then return "Opened the Beasts window on that player's game." end
		return "The Beasts window could not be sent to that player's game."
	end
	return "That player's game has no Beastmaster window (it needs EMF with Beastmaster 1.15 or newer switched on)."
end

local AS_WORDS = { give = give_command, loyalty = loyalty_command }
local AS_USAGE = "beastmaster.as <#k or player entity> <word> [...]: give, loyalty, open [name], or a /beastmaster word"

emf.command("as", "<#k or player entity> <word> [...]: run a player's word as that player: give, loyalty, open " ..
	"[name], or any /beastmaster word (console only; for tests on a dedicated server)", function(world, args)
	local sender = emf.sender()
	if sender and sender.chat then
		emf.reply("beastmaster.as is for the host's or the server's console")
		return
	end
	local target = args[1] or ""
	local player
	local k = target:match("^#(%d+)$")
	if k then
		player = players_by_id(world)[tonumber(k)]
		if not player then
			emf.reply(string.format("No player #%s in the world.", k))
			return
		end
	else
		local id = tonumber(target)
		if not (id and world:get("PlayerState", id) and world:account(id)) then
			emf.reply(target == "" and AS_USAGE or string.format("No player with entity %s.", target))
			return
		end
		player = id
	end
	local word = args[2] and args[2]:lower()
	local rest = { table.unpack(args, 3) }
	if word == "open" then
		local name = table.concat(rest, " ")
		emf.reply(open_as(world, player, name ~= "" and name or nil))
		return
	end
	local fn = word and (AS_WORDS[word] or words[word])
	if not fn then
		emf.reply(AS_USAGE)
		return
	end
	as_player = player
	local ok, err = pcall(fn, world, rest)
	as_player = nil
	if not ok then error(err, 0) end
end)
