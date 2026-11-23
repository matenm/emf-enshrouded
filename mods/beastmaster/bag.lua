-- The pack beast's bag (GitHub #48): E Open at the pack llama shows the game's chest screen of its own inventory (the
-- llama's template carries a 16-slot InventorySetup); what is in it is kept with the world as lines
-- "c <account> <item> <count> <level> <rarity>" and put back when the llama is called again (the creature itself is not
-- saved by the game). Gear (a stack with its own item entity) keeps its level and rarity; its wear is mended.
--
-- M.new(env) where env has: now(world), tell(world, player, text), owner_of(account) -> player entity or nil,
-- name_of(account) -> the owner's character name or nil, save().
local M = {}
local B = {}
B.__index = B

local OFFER = { verb = "Open", sequence = "chest", range = 6 }  -- sequence "chest": EMF's Lua 1.42
local OFFER_EVERY_S = 5   -- offered again this often while EMF no longer holds the offer (the game may drop it)
local PUT_TRIES = 30      -- seconds a new llama's inventory may take to be set up before its items wait for the next call

function M.new(env)
	return setmetatable({
		env = env,
		stacks = {},    -- account -> { {item, count, level, rarity}, ... } as kept with the world
		live = {},      -- llama entity -> { account, owner, restored, tries, asked, offered_at }
		no_chest = false,  -- an older EMF Lua: no bag (said once in the log)
	}, B)
end

function B:reset()
	self.stacks, self.live = {}, {}
end

-- The stacks in llama `id`'s inventory now, or nil while the game has not set it up.
function M.read(world, id)
	local setup = world:get("InventorySetup", id)
	if not setup or not setup.isInitialized then return nil end
	local out = {}
	local links = setup.linksEntities
	for i = 1, #links do
		local inv_id = links[i]
		local inv = inv_id ~= 0 and world:get("Inventory", inv_id)
		if inv then
			for j = 1, #inv.slots do
				local s = inv.slots[j]
				if s.id ~= 0 and s.data.count > 0 then
					local st = { item = s.id, count = s.data.count, level = 0, rarity = 0 }
					local pide = s.data.pide
					local state = pide ~= 0 and world:get("ItemState", pide)
					if state then st.level, st.rarity = state.itemLevelUi or 0, tonumber(state.itemRarityUi) or 0 end
					out[#out + 1] = st
				end
			end
		end
	end
	return out
end

local function same(a, b)
	if #a ~= #b then return false end
	for i = 1, #a do
		local x, y = a[i], b[i]
		if x.item ~= y.item or x.count ~= y.count or x.level ~= y.level or x.rarity ~= y.rarity then return false end
	end
	return true
end

-- The llama `id` of `account` came into the world beside `owner`: its bag is offered, and its kept items wait to be put
-- back once the game has set up its inventory.
function B:spawned(world, account, id, owner)
	self.live[id] = { account = account, owner = owner, restored = false, tries = 0 }
	self:offer(world, id)
end

function B:offer(world, id)
	local l = self.live[id]
	if not l or self.no_chest then return end
	local ok, res = pcall(emf.offer, world, id, OFFER, function(w, player, entity) self:opened(w, player, entity) end)
	if not ok then
		self.no_chest = true  -- an older Lua: no chest sequence
		emf.log("the pack llama's bag needs EMF's Lua 1.42 or newer (" .. tostring(res) .. ")")
		return
	end
	l.offered_at = self.env.now(world)
end

-- E at the llama: its owner's chest screen opens by the game's own sequence; anyone else is told whose bag it is.
function B:opened(world, player, entity)
	local l = self.live[entity]
	if not l then return end
	local acct = world:account(player)
	if acct and acct ~= l.account then
		local name = self.env.name_of(l.account)
		self.env.tell(world, player, "Only " .. (name or "its owner") .. " opens this bag.")
	end
end

-- Puts the kept items into a new llama once its inventory is set up; then follows what is in it.
function B:step(world)
	local now = self.env.now(world)
	for id, l in pairs(self.live) do
		if not l.restored then
			if not l.asked then
				local setup = world:get("InventorySetup", id)
				if setup and setup.isInitialized then
					l.asked = true
					local stacks = self.stacks[l.account] or {}
					l.waiting = #stacks
					if l.waiting == 0 then l.restored = true end
					for _, st in ipairs(stacks) do
						emf.put(world, id, st.item, st.count, function()
							l.waiting = l.waiting - 1
							if l.waiting <= 0 then l.restored = true end
						end, st.level > 0 and { level = st.level, rarity = st.rarity } or nil)
					end
				else
					l.tries = l.tries + 1
					if l.tries > PUT_TRIES then self.live[id] = nil end  -- its items wait for the next call
				end
			end
		else
			self:snapshot(world, id)
			if (now - (l.offered_at or -math.huge)) >= OFFER_EVERY_S and emf.offered and not emf.offered(world, id) then
				self:offer(world, id)
			end
		end
	end
end

-- What is in llama `id` now becomes what is kept (saved when it changed). Only once its kept items went back in: a new
-- llama's empty bag never replaces them.
function B:snapshot(world, id)
	local l = self.live[id]
	if not l or not l.restored then return end
	local now = M.read(world, id)
	if not now then return end
	if not same(now, self.stacks[l.account] or {}) then
		self.stacks[l.account] = now
		self.env.save()
	end
end

-- The llama `id` leaves the world (sent home, aside, its owner left): its bag as it is now is kept.
function B:gone(world, id)
	if not self.live[id] then return end
	self:snapshot(world, id)
	self.live[id] = nil
end

-- The llama was released for good: what it carried goes to its owner (what does not fit drops at their feet). Without
-- the owner's entity the items stay kept (a new llama brings them).
function B:release(world, account, owner)
	if not owner then return end
	local stacks = self.stacks[account] or {}
	self.stacks[account] = nil
	for _, st in ipairs(stacks) do
		emf.put(world, owner, st.item, st.count, function(w, added)
			if (added or 0) < st.count then emf.give(w, owner, st.item, st.count - (added or 0)) end
		end, st.level > 0 and { level = st.level, rarity = st.rarity } or nil)
	end
	self.env.save()
end

function B:serialize()
	local lines = {}
	for account, stacks in pairs(self.stacks) do
		for _, st in ipairs(stacks) do
			lines[#lines + 1] = string.format("c %s %d %d %d %d", account, st.item, st.count, st.level, st.rarity)
		end
	end
	table.sort(lines)
	return table.concat(lines, "\n")
end

function B:load(text)
	self:reset()
	for line in (text or ""):gmatch("[^\n]+") do
		local account, item, count, level, rarity = line:match("^c (0x%x+) (%d+) (%d+) (%d+) (%d+)$")
		if account then
			local list = self.stacks[account] or {}
			list[#list + 1] = { item = tonumber(item), count = tonumber(count), level = tonumber(level),
				rarity = tonumber(rarity) }
			self.stacks[account] = list
		end
	end
end

return M
