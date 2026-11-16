-- Necromancer: raise skeleton warriors (and, with the undead pack, a lich) that stay and follow you; cast raging spirits
-- that rush your foe.
-- Tests: tests/test_necromancer.cpp.
local minions_mod = require("minions")
local spirits_mod = require("spirits")
local aim = require("aim")

emf.emits({ "StartGuardingEvent", "AddThreatTargetEvent" })

local SKELETON = "b3e66f3b-7b13-4fa0-a68c-d2a41d8c84d9"  -- Enemy_Skeleton_Melee01, made an ally
local LICH = "c4e1a7d2-6b3f-4d8e-9a10-5f2b7c3d9e02"      -- Necromancer_Lich (emf_necromancer_undead: a skeleton mage copy)
local GOLEM = "e7b3c9d1-2a4f-4e6b-8c15-9d2f4a6b8e02"     -- Necromancer_BoneGolem (the undead pack: a heavy skeleton copy)
local SPIRIT = "442ac9a3-ea47-4129-878d-73659f94f9d4"    -- Player_Companion_FlyingSkull
local SPIRIT_AIM_M, SPIRIT_NEAR_M = 30, 12
local ATTACK_RANGE_M, HIT_MEMORY_S = 40, 10

local loaded = false   -- the world's data has arrived (saving before that could overwrite it)
local last_hit = {}    -- player entity -> {id, at}: what hit them last (attack's choice without a crosshair target)
local minions          -- the skeletons (made below)

local function setting(key)
	return emf.setting(key)
end

local function now(world)
	return world:tick() / 60
end

-- A note for a player: on screen for the host, a chat line for a player who joined.
local function tell(world, player, text)
	if player == emf.local_player() then emf.notify(text) else emf.tell(world, player, text) end
end

local function save()
	if loaded then emf.save_world_data(minions:serialize()) end
end

-- The kinds of undead, in wheel order. Their ids are saved: never rename one.
local KINDS = {
	{ id = "skeleton", name = "skeleton", title = "Skeleton", plural = "Skeletons", template = SKELETON,
	  cap = "skeleton_cap", mana = "raise_mana_pct" },
	{ id = "lich", name = "lich", title = "Lich", plural = "Liches", template = LICH, cap = "lich_cap",
	  mana = "lich_mana_pct", health_mul = 1.5, pack = "the Necromancer undead pack" },
	{ id = "golem", name = "bone golem", title = "Bone Golem", plural = "Bone Golems", template = GOLEM,
	  cap = "golem_cap", mana = "golem_mana_pct", health_mul = 2, pack = "the Necromancer undead pack" },
}

minions = minions_mod.new({ setting = setting, now = now, tell = tell, save = save, kinds = KINDS })
local spirits = spirits_mod.new({ setting = setting, now = now, template = SPIRIT })

emf.on_world_data(function(world, text)
	minions:parse(text)
	minions:reset()
	spirits:reset()
	last_hit = {}
	loaded = true
end)

-- Who a command is for: the player who typed it in the chat, else the host. Returns entity, account text.
local function who(world)
	local s = emf.sender()
	if s and s.chat then return s.entity, s.account end
	local p = emf.local_player()
	return p, p and world:account(p) or nil
end

-- A foe for attack orders and spirits: a living creature of a monster faction (Fogger 2, Vukah 3, Scavenger 4, Wildlife 5,
-- WildBeast 6, Skeleton 10, Cyclops 13, Drak 14, Ancients 15). Never a player, the player side, a farm animal, or a base
-- object with health (Environment 0: training dummies, HitAll 1: the player's own mines, HitOthers 9).
local MONSTERS = { [2] = true, [3] = true, [4] = true, [5] = true, [6] = true, [10] = true, [13] = true, [14] = true, [15] = true }
local function foe(world, id)
	if world:get("PlayerState", id) then return false end
	local f = world:get("Faction", id)
	local cur = emf.health(world, id)
	return f ~= nil and MONSTERS[f.faction] == true and cur ~= nil and cur > 0
end

-- What the player looks at within 40 m, else what hit them in the last 10 s.
local function target_of(world, player)
	local t = aim.pick(world, player, ATTACK_RANGE_M, function(id) return foe(world, id) end)
	local h = last_hit[player]
	if not t and h and now(world) - h.at <= HIT_MEMORY_S and foe(world, h.id) then
		local px, py, pz = world:position(player)
		local x, y, z = world:position(h.id)
		if px and x and math.sqrt((x - px) ^ 2 + (y - py) ^ 2 + (z - pz) ^ 2) <= ATTACK_RANGE_M then t = h.id end
	end
	return t
end

-- The spirits' target: what the player looks at within 30 m, else the nearest foe within 12 m of them, else nil.
local function spirit_target(world, player)
	local t = aim.pick(world, player, SPIRIT_AIM_M, function(id) return foe(world, id) end)
	if t then return t end
	local px, py, pz = world:position(player)
	if not px then return nil end
	local best, best_d
	for _, id in ipairs(world:entities()) do
		if id ~= player then
			local x, y, z = world:position(id)
			if x then
				local d = math.sqrt((x - px) ^ 2 + (y - py) ^ 2 + (z - pz) ^ 2)
				if d <= SPIRIT_NEAR_M and (not best or d < best_d) and foe(world, id) then best, best_d = id, d end
			end
		end
	end
	return best
end

-- ---- actions: each returns the text to show (a command's reply, or a note after a wheel pick) ----
local actions = {}

function actions.spirits(world, player, account)
	if not account then return "Your account is not known yet." end
	return spirits:cast(world, player, account, spirit_target(world, player))
end

function actions.raise(world, player, account, args)
	if not account then return "Your account is not known yet." end
	local word = args and #args > 0 and table.concat(args, " ") or nil  -- "bone golem" is two words
	return minions:raise(world, player, account, word)
end

function actions.list(world, player, account)
	if not account then return "Your account is not known yet." end
	local text = "Your undead: " .. minions:summary(account) .. "."
	if minions.aside:by_command(account) then
		text = text .. " They stepped aside: Come back on the wheel, Gather or /necromancer aside brings them back."
	elseif minions.aside:why(account) == "air" then
		text = text .. " They step aside while you are in the air and come back once you land."
	elseif minions.aside:is_aside(account) then
		text = text .. " They step aside while someone builds next to them."
	end
	return text
end

-- No word or "all": every minion; a kind ("skeleton", "lich", "bone golem"): the newest of that kind.
function actions.dismiss(world, player, account, args)
	if not account then return "Your account is not known yet." end
	local word = args and #args > 0 and table.concat(args, " ") or nil
	if not word or word:lower() == "all" then return minions:dismiss_all(world, account) end
	local kind = minions:kind(word)
	if not kind then
		local names = {}
		for _, k in ipairs(KINDS) do names[#names + 1] = k.id end
		return "Dismiss what? " .. table.concat(names, " | ") .. " | all"
	end
	return minions:dismiss_one(world, account, kind)
end

function actions.gather(world, player, account)
	if not account then return "Your account is not known yet." end
	local busy, why = minions:building(world, account, player)
	if busy then return minions:waits(why, "they come back") end
	if minions.aside:release(account) then return minions:coming_back(world, account, player) end
	local n = minions:gather(world, player, account)
	return n > 0 and string.format("Your undead gather to you (%d).", n) or "You have no undead."
end

function actions.attack(world, player, account)
	if not account then return "Your account is not known yet." end
	local target = target_of(world, player)
	if not target then return "Look at a creature to attack (within 40 m)." end
	local n = minions:attack(world, account, target)
	return n > 0 and string.format("Your undead attack (%d).", n) or "You have no undead."
end

-- The players' commands are also words of the bare /necromancer (players who joined type "/necromancer raise").
local words, word_list = {}, {}
local function command(word, help, action)
	local fn = function(world, args)
		local player, account = who(world)
		emf.reply(action(world, player, account, args))
	end
	emf.command(word, help, fn, { players = true })
	words[word] = fn
	word_list[#word_list + 1] = word
end

command("raise", "[skeleton|lich|golem]: raise a skeleton warrior (or, with the undead pack, a lich or a bone golem) "
	.. "beside you (mana)", actions.raise)
command("spirits", ": raging spirits rush the creature you look at, or the nearest foe (mana)", actions.spirits)
command("list", ": your undead", actions.list)
command("gather", ": your undead come to you (also after aside)", actions.gather)
command("attack", ": your undead attack the creature you look at (else what hit you)", actions.attack)
command("aside", ": your undead step aside (leave for now) or come back; while you build they do it on their own",
	function(world, player, account)
		if not account then return "Your account is not known yet." end
		return minions:toggle_aside(world, account, player)
	end)
command("dismiss", "[skeleton|lich|golem|all]: one undead of that kind returns to the ground (the newest); nothing or "
	.. "all: every one", actions.dismiss)

emf.command(emf.id, "<word>: " .. table.concat(word_list, ", "), function(world, args)
	local word = args[1] and args[1]:lower()
	local fn = word and words[word]
	if not fn then
		emf.reply("/necromancer " .. table.concat(word_list, " | "))
		return
	end
	fn(world, { table.unpack(args, 2) })
end, { players = true })

-- ---- the wheel (the host; players who join use the chat commands) ----
-- The "summons" wheel group (lua 1.9): with shared_wheel on, the Undead section is on the one wheel that every class
-- mod's key opens (Beastmaster's too); off, Necromancer keeps its own wheel and is not on the shared one.
local GROUP = "summons"
local wheel_entries = {}  -- what each wheel item does: {action name, args}
local pending = nil       -- the host's pick, run on the server world's next step

local recent = {}       -- label -> {entry, at}: what each label published in the last 2 minutes meant
local own_entries = {}  -- the own wheel's entries as they were when it opened

-- A pick on the shared wheel: by the label that was shown (the list may have moved since the wheel opened: a raise or a
-- fall adds or takes away a Dismiss item).
local function picked(world, choice, label)
	local r = label and recent[label]
	pending = r and r.entry or (choice and wheel_entries[choice]) or nil
end

-- A pick on Necromancer's own wheel: by its place in the list as it was when the wheel opened.
local function picked_own(world, choice)
	pending = choice and own_entries[choice] or nil
end

-- Short labels: a name of at most 20 characters, a detail in brackets of at most 13 (the wheel's slices are narrow).
local function wheel_items(world)
	local account = world:account(emf.local_player())
	local labels, entries = {}, {}
	for _, k in ipairs(KINDS) do
		local cap = setting(k.cap)
		if cap > 0 then
			local n = account and minions:count(account, k.id) or 0
			labels[#labels + 1] = string.format("Raise %s (%d/%d)", k.title, n, cap)
			entries[#entries + 1] = { "raise", { k.id } }
		end
	end
	local rest = {
		{ string.format("Raging Spirits (%d %% mana)", setting("spirit_mana_pct")), "spirits" },
		{ account and minions.aside:by_command(account) and "Come back" or "Gather", "gather" },  -- Gather ends an aside
		{ "Attack", "attack" },
	}
	for _, r in ipairs(rest) do
		labels[#labels + 1] = r[1]
		entries[#entries + 1] = { r[2], {} }
	end
	local total = 0
	for _, k in ipairs(KINDS) do
		local n = account and minions:count(account, k.id) or 0
		if n > 0 then
			total = total + n
			labels[#labels + 1] = "Dismiss " .. k.title
			entries[#entries + 1] = { "dismiss", { k.id } }
		end
	end
	if total > 1 then
		labels[#labels + 1] = "Dismiss all"
		entries[#entries + 1] = { "dismiss", { "all" } }
	end
	return labels, entries
end

local function publish(world)
	local labels, entries = wheel_items(world)
	wheel_entries = entries
	local t = now(world)
	for i, l in ipairs(labels) do recent[l] = { entry = entries[i], at = t } end
	for l, r in pairs(recent) do
		if t - r.at > 120 or t < r.at then recent[l] = nil end  -- old, or the clock restarted
	end
	if setting("shared_wheel") then
		emf.wheel_section(GROUP, "Undead", labels, picked)
	else
		emf.wheel_section(GROUP, "Undead", nil)
	end
	return labels
end

emf.on_key("wheel_key", function(world)
	if not world:is_server() then
		emf.notify("The summoning wheel works in a world you host; in someone else's game type /necromancer in the chat.")
		return
	end
	local labels = publish(world)
	if setting("shared_wheel") then
		emf.wheel_group(GROUP, "Summons", emf.setting("wheel_key"))
	else
		own_entries = wheel_entries
		emf.wheel("Undead", labels, emf.setting("wheel_key"), picked_own)
	end
end)

-- The host's own undead on screen: "Undead: Skeletons 2/3, Liches 1/1, Spirits 2"; the wheel section kept current.
local next_hud = 0
local function update_hud(world)
	local t = now(world)
	if t < next_hud and next_hud - t <= 1 then return end  -- once a second (a clock that restarted updates at once)
	next_hud = t + 1
	local account = world:account(emf.local_player())
	local parts = {}
	if account then
		local had = minions:summary(account, true)
		if had ~= "" then parts[#parts + 1] = had end
		local sp = spirits:count(account)
		if sp > 0 then parts[#parts + 1] = string.format("Spirits %d", sp) end
	end
	local title = account and minions.aside:is_aside(account) and "Undead (aside): " or "Undead: "
	local text = #parts > 0 and (title .. table.concat(parts, ", ")) or ""
	emf.hud("out", text, "bottom_left")
	publish(world)
end

emf.on_event("HitEvent", function(world, e)
	minions:on_hit(world, e.rootSourceId, e.targetId)
	local caster = spirits:account_of(e.rootSourceId)
	if caster then minions:credit(world, caster, e.targetId) end
	if e.rootSourceId ~= 0 and e.rootSourceId ~= e.targetId and world:get("PlayerState", e.targetId) then
		last_hit[e.targetId] = { id = e.rootSourceId, at = now(world) }
	end
end)

emf.on_event("EntityDiedEvent", function(world, e)
	minions:on_died(e.deadEntityId)
	spirits:on_died(e.deadEntityId)
end)

emf.on_tick(function(world)
	if not world:is_server() then return end
	minions:step(world)
	spirits:step(world)
	if pending then
		local action, args = pending[1], pending[2]
		pending = nil
		local player = emf.local_player()
		emf.notify(actions[action](world, player, world:account(player), args))
	end
	update_hud(world)
end)
