-- Beastmaster: tame wild beasts, keep them in a collection, call them to fight beside you.
-- Tests: tests/test_beastmaster.cpp.
local species = require("species")
local roster = require("roster")
local tame_mod = require("tame")
local pack_mod = require("pack")
local aim = require("aim")
local health_mod = require("health")
local wild_mod = require("wild")

emf.emits({ "StartGuardingEvent", "AddThreatTargetEvent" })

local beasts = roster.new()  -- every player's collection, kept with the world
local loaded = false          -- the world's data has arrived (saving before that could overwrite it)
local last_hit = {}           -- player entity -> {id, at}: what hit them last (attack's choice without a crosshair target)
local taming                  -- the taming channels (made below; reset with each world)
local next_hud = 0            -- when the HUD line is written next (reset with each world: the clock may restart)

local function setting(key)
	return emf.setting(key)
end

-- A note for a player: on screen for the host, a chat line for a player who joined.
local function tell(world, player, text)
	if player == emf.local_player() then emf.notify(text) else emf.tell(world, player, text) end
end

local function now(world)
	return world:tick() / 60
end

local function save()
	if loaded then emf.save_world_data(beasts:serialize()) end
end

-- tougher beasts (the bear: 2.5x health), watched from their spawn
local health = health_mod.new()
local function spawned(world, id, sp)
	health:watch(world, id, sp)
end

-- the beasts out in the world
local pack = pack_mod.new({
	setting = setting,
	now = now,
	tell = tell,
	beasts = function() return beasts end,
	save = save,
	on_spawn = spawned,
})

-- rare wild bears and aurochs (with their content packs)
local wild = wild_mod.new({ setting = setting, now = now, on_spawn = spawned })

emf.on_world_data(function(world, text)
	beasts = roster.parse(text)
	pack:reset()
	health:reset()
	wild:reset()
	taming:reset()
	last_hit = {}
	next_hud = 0
	loaded = true
end)

-- Who a command is for: the player who typed it in the chat, else the host. Returns entity, account text.
local function who(world)
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

local function state_text(b)
	if b.out then return "out" end
	if b.rest > 0 then return string.format("rests %d:%02d", math.floor(b.rest / 60), math.floor(b.rest % 60)) end
	return "at home"
end

command("list", ": your beasts", function(world, args)
	local _, account = who(world)
	local list = account and beasts:list(account) or {}
	if #list == 0 then
		emf.reply("You have no beasts yet. Tame one: beat a wild wolf, boar, sabretooth, hound, bear or aurochs down, or feed it its bait.")
		return
	end
	local lines = { string.format("Your beasts (%d/%d):", #list, setting("collection_max")) }
	for i, b in ipairs(list) do
		lines[#lines + 1] = string.format("%d. %s - %s, %s", i, b.name, b.species, state_text(b))
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
	now = now,
	tell = tell,
	on_tamed = function(world, player, id, sp)
		local account = world:account(player)
		local b = account and beasts:add(account, sp.id, sp.name, setting("collection_max"))
		if not b then
			tell(world, player, "Your collection is full: the " .. sp.id .. " follows you until it falls.")
			return
		end
		pack:adopt(account, b, id)
		save()
		tell(world, player, string.format("You tamed a %s: %s", sp.id, b.name))
	end,
})

local function collection_full(world, player)
	local account = world:account(player)
	return account and #beasts:list(account) >= setting("collection_max")
end

-- ---- actions: each returns the text to show (a command's reply, or a note after a wheel pick) ----
local actions = {}

function actions.tame(world, player)
	if collection_full(world, player) then return "Your collection is full." end
	return taming:tame(world, player)
end

function actions.feed(world, player)
	if collection_full(world, player) then return "Your collection is full." end
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

local ATTACK_RANGE_M, HIT_MEMORY_S = 40, 10

function actions.attack(world, player, account)
	if not account then return "Your account is not known yet." end
	local function foe(id)
		if world:get("PlayerState", id) then return false end
		local f = world:get("Faction", id)
		local cur = emf.health(world, id)
		return f ~= nil and f.faction ~= 11 and cur ~= nil and cur > 0
	end
	local target = aim.pick(world, player, ATTACK_RANGE_M, foe)
	local h = last_hit[player]
	if not target and h and now(world) - h.at <= HIT_MEMORY_S and foe(h.id) then
		local px, py, pz = world:position(player)
		local x, y, z = world:position(h.id)
		if px and x and math.sqrt((x - px) ^ 2 + (y - py) ^ 2 + (z - pz) ^ 2) <= ATTACK_RANGE_M then target = h.id end
	end
	if not target then return "Look at a creature to attack (within 40 m)." end
	local n = pack:attack(world, account, target)
	return n > 0 and string.format("Your beasts attack (%d).", n) or "No beast of yours is out."
end

command("tame", ": tame the beast you look at (beaten under 25 % health)", function(world, args)
	emf.reply(actions.tame(world, who(world)))
end)

command("feed", ": feed the beast you look at its bait (3 times to tame it)", function(world, args)
	emf.reply(actions.feed(world, who(world)))
end)

command("call", "<name or number>: call a beast of your collection to your side", function(world, args)
	local player, account = who(world)
	emf.reply(actions.call(world, player, account, table.concat(args, " ")))
end)

command("dismiss", "<name or number | all>: send a beast home", function(world, args)
	local player, account = who(world)
	emf.reply(actions.dismiss(world, player, account, table.concat(args, " ")))
end)

command("attack", ": your beasts out attack the creature you look at", function(world, args)
	local player, account = who(world)
	emf.reply(actions.attack(world, player, account))
end)

emf.command(emf.id, "<word>: " .. table.concat(word_list, ", ") .. " (call and dismiss take a name or number)",
	function(world, args)
		local word = args[1] and args[1]:lower()
		local fn = word and words[word]
		if not fn then
			emf.reply("/beastmaster " .. table.concat(word_list, " | ") .. " (call and dismiss take a name or number)")
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

-- The wheel's items for the host's account: labels and what each does.
local function wheel_items(world)
	local account = world:account(emf.local_player())
	local labels, entries = {}, {}
	local function add(label, entry)
		labels[#labels + 1] = label
		entries[#entries + 1] = entry
	end
	add("Tame (look at a weakened beast)", { action = "tame" })
	add("Feed (its bait, 3 times)", { action = "feed" })
	add("Attack (what you look at)", { action = "attack" })
	add("Dismiss all", { action = "dismiss", key = "all" })
	local shown = 0
	for _, b in ipairs(account and beasts:list(account) or {}) do
		if not b.out and shown < WHEEL_CALLS then
			shown = shown + 1
			add(b.rest > 0 and string.format("Call %s (rests %d:%02d)", b.name, math.floor(b.rest / 60), math.floor(b.rest % 60))
				or string.format("Call %s (%s)", b.name, b.species), { action = "call", key = b.name })
		end
	end
	return labels, entries
end

-- The "summons" wheel group (lua 1.9): with shared_wheel on, Beastmaster's section is on the one wheel that every class
-- mod's key opens (Necromancer's too); off, it keeps its own wheel and is not on the shared one.
local GROUP = "summons"
local function publish(world)
	local labels, entries = wheel_items(world)
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
	if not world:is_server() then
		emf.notify("The beast wheel works in a world you host; in someone else's game type /beastmaster in the chat.")
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

-- The host's own beasts on screen: "Beasts: Wolf (out), Boar (rests 1:20)".
local function update_hud(world)
	local now_s = now(world)
	if now_s < next_hud then return end
	next_hud = now_s + 1
	local account = world:account(emf.local_player())
	local parts = {}
	for _, b in ipairs(account and beasts:list(account) or {}) do
		if b.out then
			parts[#parts + 1] = b.name .. " (out)"
		elseif b.rest > 0 then
			parts[#parts + 1] = string.format("%s (rests %d:%02d)", b.name, math.floor(b.rest / 60), math.floor(b.rest % 60))
		end
	end
	emf.hud("out", #parts > 0 and ("Beasts: " .. table.concat(parts, ", ")) or "", "bottom_left")
	publish(world)  -- the shared wheel shows what the other class mod's key opens with
end

emf.on_event("HitEvent", function(world, e)
	pack:on_hit(world, e.rootSourceId, e.targetId)
	if e.rootSourceId ~= 0 and e.rootSourceId ~= e.targetId and world:get("PlayerState", e.targetId) then
		last_hit[e.targetId] = { id = e.rootSourceId, at = now(world) }
	end
end)

emf.on_event("EntityDiedEvent", function(world, e)
	taming:on_died(e.deadEntityId)
	pack:on_died(e.deadEntityId)
end)

emf.on_tick(function(world)
	if not world:is_server() then return end
	taming:step(world)
	pack:step(world)
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

-- Admin: add a beast to your collection without taming it (console only, not from the chat).
emf.command("give", "<species>: add a beast to your collection (admin)", function(world, args)
	local sender = emf.sender()
	if sender and sender.chat then
		emf.reply("beastmaster.give is for the host's console")
		return
	end
	local s = species.by_id(args[1])
	if not s then
		emf.reply("no species " .. tostring(args[1]) .. " (wolf, boar, sabretooth, hound, bear)")
		return
	end
	local _, account = who(world)
	if not account then emf.reply("your account is not known yet") return end
	local b, why = beasts:add(account, s.id, s.name, setting("collection_max"))
	if not b then emf.reply(why) return end
	save()
	emf.reply(string.format("added %s (%s) to your collection", b.name, s.id))
end)
