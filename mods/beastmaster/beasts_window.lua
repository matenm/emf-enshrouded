-- The Beasts window (the host presses E at a stable), with two tabs: Stable (the active slots and every beast of the
-- collection) and the selected beast's own (its stats and talents). Selecting a beast (its row or its slot) opens its
-- tab; Stable goes back. Both have Make active, Store and Release for the selected beast; Release asks for a second click
-- within CONFIRM_S. The window is drawn again after every click, and once a second when what it shows changed (a beast
-- fell, rests, came).
-- M.new(env) where env has: setting(key), now(world), beasts() (the roster), level(world, player), state(beast, account)
-- ("out", "at home", "rests 1:20"), mode(account) (how the beasts fight), wind_left(world, beast) (seconds before Second
-- Wind works again), and the stable's
-- actions, each returning what it did: make_active(world, account, player, beast), store(world, account, beast),
-- release(world, account, beast), spend(world, account, beast, talent), reset_talents(world, account, beast), and
-- optionally (1.21) can_train() (a Training Grounds is known: Send to training and Back to the stable show),
-- train(world, account, player, beast) and untrain(world, account, beast).
-- M.new(env, screen): where the window is drawn (nil: M.host_screen(), EMF's overlay in this game); a screen has
-- open(world, def, on_event) -> bool, update(world, def) -> bool (false: it is closed) and picture(sp) -> the fields a
-- picture row of kind `sp` gets, or nil (none). def = {title, width, place, rows}. A screen with counts_down = true
-- counts the rows' countdowns down itself: a change of them alone does not draw the window again.
local species = require("species")
local loyalty = require("loyalty")
local slots = require("slots")
local talents = require("talents")
local threat = require("threat")
local pack_mod = require("pack")

local M = {}
M.ID = "stable"
local CONFIRM_S = 5

-- The countdowns a row's text can hold ("rests 1:20", "again in 0:45", "rests for 1:20"): a joined player's game counts
-- them down itself, so a window drawn there is sent again only when something else changed.
local COUNTDOWNS = { "([Rr]ests )(%d+):(%d%d)", "(again in )(%d+):(%d%d)", "([Rr]ests for )(%d+):(%d%d)" }
local COUNTDOWN_RESEND_S = 15  -- s: countdowns alone draw a joined player's window again this often (a game that does
                               -- not count them itself, Beastmaster 1.15.0, still sees them move)

-- Whether `text` holds a countdown.
function M.has_countdown(text)
	for _, p in ipairs(COUNTDOWNS) do
		if text:find(p) then return true end
	end
	return false
end

-- `text` with its countdowns `elapsed` seconds further on (not below 0:00).
function M.count_down(text, elapsed)
	for _, p in ipairs(COUNTDOWNS) do
		text = text:gsub(p, function(lead, m, s)
			if #m > 4 then return nil end  -- no real countdown (left as it is)
			local left = math.max(0, tonumber(m) * 60 + tonumber(s) - elapsed)
			return string.format("%s%d:%02d", lead, math.floor(left / 60), math.floor(left % 60))
		end)
	end
	return text
end

-- `text` with its countdowns' numbers left out (what a window shows apart from them).
local function masked(text)
	for _, p in ipairs(COUNTDOWNS) do text = text:gsub(p, "%1#") end
	return text
end
local PORTRAIT_LINES = 6  -- the picture's height in text lines: the last row (the buttons) still fits on a 1080p screen
local PORTRAIT_FPS = 8    -- its 24 frames (mod.json "portraits"): one turn in 3 s

local W = {}
W.__index = W

function M.new(env, screen)
	-- opening: counts the windows opened (kept over resets)
	local w = setmetatable({ env = env, opening = 0, screen = screen or M.host_screen() }, W)
	w:reset()
	return w
end

-- The host's screen: EMF's overlay in this game (emf.window, its events on the server world), the pictures EMF drew
-- from this game at the start (mod.json "portraits", key beastmaster.<id>), asked once per kind (the cache only changes
-- at a start; no emf.image (Lua before 1.15, whose windows refuse a picture row): none).
function M.host_screen()
	local portraits = {}  -- species id -> picture handle (0: none)
	local s = {}
	local function full(def)
		def.close_key = emf.setting("stable_key") or ""
		def.world = "server"
		return def
	end
	function s.open(world, def, on_event)
		return emf.window(M.ID, full(def), on_event)
	end
	function s.update(world, def)
		return emf.window_update(M.ID, full(def))
	end
	function s.picture(sp)
		if not (sp and emf.image) then return nil end
		local h = portraits[sp.id]
		if h == nil then
			h = emf.image("portrait:beastmaster." .. sp.id) or 0
			portraits[sp.id] = h
		end
		return h ~= 0 and { image = h } or nil
	end
	return s
end

-- A new world: no window.
function W:reset()
	self.open = false
	self.account, self.player = nil, nil
	self.selected = nil  -- the selected beast's name
	self.tab = "stable"  -- "stable" or "beast" (the selected beast's)
	self.confirm = nil   -- {name, at}: Release clicked once
	self.note = nil      -- the line under the buttons: what the last click did
	self.drawn = nil     -- what the window shows now (to draw it again only when that changed)
	self.drawn_full = nil  -- the same with its countdowns (a screen that counts them down)
	self.drawn_at = nil    -- when it was drawn last
end

function W:loyalty_of(b)
	return loyalty.level(b.points, self.env.setting("loyalty_pace"))
end

-- The active beasts of the window's account, in list order.
function W:active_list()
	local out = {}
	for _, b in ipairs(self.env.beasts():list(self.account)) do
		if b.active then out[#out + 1] = b end
	end
	return out
end

function W:selection()
	return self.selected and self.env.beasts():find(self.account, self.selected) or nil
end

function W:open_slots(world)
	return slots.unlocked(self.env.level(world, self.player), self.env.setting)
end

-- The tab buttons: Stable, and the selected beast's name; the open one lit.
local function tabs(rows, sel, on_beast)
	rows[#rows + 1] = { button = "tab.stable", text = "Stable", selected = not on_beast }
	if sel then rows[#rows + 1] = { button = "tab.beast", text = sel.name, same_line = true, selected = on_beast } end
	rows[#rows + 1] = { separator = true }
end

-- Make active, Store and Release for the selection, and the line saying what the last click did.
function W:action_rows(rows, sel)
	rows[#rows + 1] = { button = "make_active", text = "Make active", disabled = not (sel and not sel.active) }
	rows[#rows + 1] = { button = "store", text = "Store", same_line = true, disabled = not (sel and sel.active) }
	rows[#rows + 1] = { button = "release", text = "Release", same_line = true, disabled = not sel,
		tooltip = "Let the beast go for good (click twice)" }
	if self.env.can_train and self.env.can_train() then
		if sel and sel.training then
			rows[#rows + 1] = { button = "untrain", text = "Back to the stable", same_line = true }
		else
			rows[#rows + 1] = { button = "train", text = "Send to training", same_line = true,
				disabled = not sel or sel.out == true,
				tooltip = "It trains at the nearest Training Grounds and earns loyalty by the game's clock" }
		end
	end
	if self.note then rows[#rows + 1] = { text = self.note } end
end

local function mmss(s)
	return string.format("%d:%02d", math.floor(s / 60), math.floor(s % 60))
end

-- The selected beast's tab: its picture (turning), its stats (kind, level, loyalty, what its role, loyalty and talents
-- make of its health, damage and speed, where it is), its talents with a + each and Reset talents, and its actions.
function W:beast_rows(world, rows, b)
	local env = self.env
	local pace = env.setting("loyalty_pace")
	local sp = species.by_id(b.species)
	local role_health, role_damage = species.role(sp)
	local lvl = self:loyalty_of(b)
	local f = loyalty.factor(lvl)
	rows[#rows + 1] = { heading = b.name }
	local pic = self.screen.picture(sp)
	if pic then
		local row = { height = PORTRAIT_LINES, fps = PORTRAIT_FPS }
		for k, v in pairs(pic) do row[k] = v end
		rows[#rows + 1] = row
	end
	local level = env.level(world, self.player)
	local kind = sp and sp.name or tostring(b.species)
	rows[#rows + 1] = { text = level > 0 and string.format("%s, level %d", kind, level) or kind }
	if lvl >= loyalty.MAX then
		rows[#rows + 1] = { text = string.format("Loyalty %d (the highest)", lvl) }
	else
		local kills = loyalty.points_for(lvl + 1, pace) - (b.points or 0)
		rows[#rows + 1] = { text = string.format("Loyalty %d: %d kill%s to loyalty %d", lvl, kills, kills == 1 and "" or "s",
			lvl + 1), tooltip = "A kill is a hostile creature it hit that dies within 30 s" }
	end
	local health, damage = pack_mod.strength(env.setting)
	rows[#rows + 1] = { text = string.format("Health x%.2f, damage x%.2f, speed x%.2f", role_health * f * talents.health(b) * health,
		role_damage * f * talents.damage(b) * damage, talents.speed(b)),
		tooltip = "Its role, loyalty and talents together, with the Beast health and Beast damage settings" }
	if not env.setting("threat") then
		rows[#rows + 1] = { text = "Draws no monsters (Beasts draw monsters is off)" }
	elseif env.mode(self.account) == "passive" then
		rows[#rows + 1] = { text = "Draws no monsters (passive mode)" }
	else
		rows[#rows + 1] = { text = string.format("Draws %d %% of the monsters in a fight",
			math.floor(threat.share(b, env.setting) * 100 + 0.5)),
			tooltip = "All your beasts together at most " .. env.setting("beasts_share_max_pct") .. " %, of a boss at most " ..
				env.setting("boss_share_max_pct") .. " %" }
	end
	local state = env.state(b, self.account)
	rows[#rows + 1] = { text = state:sub(1, 1):upper() .. state:sub(2) .. (b.active and ", active" or ", in the stable") }
	rows[#rows + 1] = { separator = true }
	rows[#rows + 1] = { heading = "Talents: " .. talents.summary(b, pace) }
	local left = talents.left(b, pace)
	for _, t in ipairs(talents.LIST) do
		local n = talents.rank(b, t.id)
		rows[#rows + 1] = { button = "talent." .. t.id, text = "+", disabled = n >= t.max or left <= 0,
			tooltip = "Spend a point on " .. t.name }
		local text = talents.row_text(b, t)
		if t.heal and n > 0 and b.out then
			local wait = env.wind_left(world, b)
			text = text .. (wait > 0 and string.format(" (again in %s)", mmss(wait)) or " (ready)")
		end
		rows[#rows + 1] = { text = text, same_line = true,
			tooltip = string.format("%s: %s (at most %d)", t.name, talents.effect(t, 0), t.max) }
	end
	rows[#rows + 1] = { button = "reset_talents", text = "Reset talents", disabled = talents.spent(b) == 0,
		tooltip = "Every point back, free" }
	rows[#rows + 1] = { separator = true }
	self:action_rows(rows, b)
	return rows
end

-- The window's rows for its account: the open tab.
function W:rows(world)
	local env = self.env
	local list = env.beasts():list(self.account)
	local active = self:active_list()
	local open = self:open_slots(world)
	local sel = self:selection()
	local rows = {}
	tabs(rows, sel, sel ~= nil and self.tab == "beast")
	if sel and self.tab == "beast" then return self:beast_rows(world, rows, sel) end
	rows[#rows + 1] = { heading = string.format("Active %d/%d", #active, open) }
	local shown = 0
	for i = 1, slots.MAX do
		local unlock = slots.level_for(i, env.setting)
		local row
		if i <= open then
			local b = active[i]
			if b then
				row = { slot = "active." .. i, text = b.name, count = self:loyalty_of(b), selected = b == sel,
					tooltip = string.format("%s, loyalty %d, %s", species.word(b.species), self:loyalty_of(b),
						env.state(b, self.account)) }
			else
				row = { slot = "active." .. i, text = "Empty", empty = true, tooltip = "Select a beast and Make active" }
			end
		elseif unlock then
			row = { slot = "active." .. i, text = "Level " .. unlock, locked = true,
				tooltip = string.format("Opens at character level %d", unlock) }
		end
		if row then
			shown = shown + 1
			if shown > 1 then row.same_line = true end
			rows[#rows + 1] = row
		end
	end
	for _, b in ipairs(active) do  -- an active beast's rest, counted down (it is not in the stable list)
		if (b.rest or 0) > 0 then
			rows[#rows + 1] = { text = b.name .. " " .. env.state(b, self.account), dim = true }
		end
	end
	rows[#rows + 1] = { separator = true }
	-- the stable: the beasts not in a slot (it holds collection_max of them besides the active ones)
	local stored = {}
	for _, b in ipairs(list) do
		if not b.active then stored[#stored + 1] = b end
	end
	rows[#rows + 1] = { heading = string.format("Stable %d/%d", #stored, env.setting("collection_max")) }
	if #list == 0 then
		rows[#rows + 1] = { text = "No beasts yet: tame one (beat it down, or feed it its bait).", dim = true }
	end
	for _, b in ipairs(stored) do
		rows[#rows + 1] = { row = "beast." .. b.name, selected = b == sel,
			text = string.format("%s (%s, loyalty %d, %s)", b.name, species.word(b.species), self:loyalty_of(b),
				env.state(b, self.account)) }
	end
	rows[#rows + 1] = { separator = true }
	self:action_rows(rows, sel)
	return rows
end

function W:def(world)
	return { title = "Beasts", width = 30, place = "centre", rows = self:rows(world) }
end

-- The rows as one text: the window is drawn again only when this changed (`counts_down`: apart from the countdowns).
local function signature(rows, counts_down)
	local parts = {}
	for _, r in ipairs(rows) do
		local keys = {}
		for k in pairs(r) do keys[#keys + 1] = k end
		table.sort(keys)
		for _, k in ipairs(keys) do
			local v = r[k]
			if counts_down and type(v) == "string" then v = masked(v) end
			parts[#parts + 1] = k .. "=" .. tostring(v)
		end
		parts[#parts + 1] = "|"
	end
	return table.concat(parts, ";")
end

-- Opens the window for `player` (the screen's player), on beast `name`'s tab when given; not while it is open (one E
-- press can reach both the stable's prompt and the E key). Each opening answers only its own events: a window from
-- before (a world loaded again under it) that closes late leaves the one opened since alone. True when it opened.
function W:show(world, player, name)
	if self.open then return false end
	local account = world:account(player)
	if not account then return false end
	self:reset()
	self.account, self.player = account, player
	local b = name and self.env.beasts():find(account, name)
	if b then self:select(b) end
	local def = self:def(world)
	self.opening = self.opening + 1
	local opening = self.opening
	if not self.screen.open(world, def, function(w, ev)
		if opening == self.opening then self:on_event(w, ev) end
	end) then
		self:reset()
		return false
	end
	self.open = true
	self.drawn = signature(def.rows, self.screen.counts_down)
	self.drawn_full = signature(def.rows)
	self.drawn_at = self.env.now(world)
	return true
end

-- Draws the window again when what it shows changed (`force`: always); on a screen that counts the countdowns down,
-- when only they moved, every COUNTDOWN_RESEND_S.
function W:refresh(world, force)
	if not self.open then return end
	local def = self:def(world)
	local counts = self.screen.counts_down
	local sig = signature(def.rows, counts)
	local full = signature(def.rows)
	local now = self.env.now(world)
	if not force and sig == self.drawn then
		if not counts or full == self.drawn_full then return end
		local at = self.drawn_at or now
		if now >= at and now - at < COUNTDOWN_RESEND_S then return end
	end
	if self.screen.update(world, def) then
		self.drawn, self.drawn_full, self.drawn_at = sig, full, now
	else
		self.open = false  -- the player closed it meanwhile
	end
end

-- ---- clicks ----
-- Selects `b` and opens its tab (nil: none, the Stable tab).
function W:select(b)
	self.selected = b and b.name or nil
	self.tab = b and "beast" or "stable"
	self.confirm = nil
	self.note = nil
end

function W:release(world, b)
	local now = self.env.now(world)
	local c = self.confirm
	if c and c.name == b.name and now - c.at <= CONFIRM_S then
		self.selected, self.confirm, self.tab = nil, nil, "stable"
		self.note = self.env.release(world, self.account, b)
		return
	end
	self.confirm = { name = b.name, at = now }
	self.note = string.format("Click Release again within %d s to let %s go for good.", CONFIRM_S, b.name)
end

function W:on_event(world, ev)
	if ev.kind == "closed" then
		self.open = false
		return
	end
	if ev.kind ~= "click" or not self.open then return end
	local key = ev.key or ""
	local slot = key:match("^active%.(%d+)$")
	local name = key:match("^beast%.(.+)$")
	local talent = talents.find(key:match("^talent%.(.+)$"))
	local sel = self:selection()
	if key == "tab.stable" then
		self.tab, self.confirm = "stable", nil
	elseif key == "tab.beast" then
		if sel then self.tab, self.confirm = "beast", nil end
	elseif slot then
		local b = self:active_list()[tonumber(slot)]
		if b then self:select(b) end
	elseif name then
		local b = self.env.beasts():find(self.account, name)
		if b then self:select(b) end
	elseif sel and key == "make_active" then
		self.confirm = nil
		self.note = self.env.make_active(world, self.account, self.player, sel)
	elseif sel and key == "store" then
		self.confirm = nil
		self.note = self.env.store(world, self.account, sel)
	elseif sel and key == "release" then
		self:release(world, sel)
	elseif sel and key == "train" and self.env.train then
		self.confirm = nil
		self.note = self.env.train(world, self.account, self.player, sel)
	elseif sel and key == "untrain" and self.env.untrain then
		self.confirm = nil
		self.note = self.env.untrain(world, self.account, sel)
	elseif sel and talent then
		self.confirm = nil
		self.note = self.env.spend(world, self.account, sel, talent)
	elseif sel and key == "reset_talents" then
		self.confirm = nil
		self.note = self.env.reset_talents(world, self.account, sel)
	end
	self:refresh(world, true)
end

return M
