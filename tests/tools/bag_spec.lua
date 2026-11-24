-- Unit tests for mods/beastmaster/bag.lua (the pack beast's bag), run by test_bag.py with a Lua 5.4 interpreter:
-- lua bag_spec.lua <path of bag.lua>. Stand-ins for EMF's Lua (emf.health, emf.offer, ...) and a server world.
local path = assert(arg[1], "usage: lua bag_spec.lua <bag.lua>")

local failures, ran = 0, 0
local function test(name, fn)
	ran = ran + 1
	local ok, err = pcall(fn)
	if not ok then
		failures = failures + 1
		io.stderr:write("FAIL ", name, ": ", tostring(err), "\n")
	end
end
local function eq(a, b, what)
	if a ~= b then error(string.format("%s: %s ~= %s", what, tostring(a), tostring(b)), 2) end
end

-- the world: one pack beast (entity 50) of account "a" beside its owner (player 1), its inventory set up
local function rig()
	local r = { health = {}, offers = {}, now = 0 }
	r.world = {
		get = function(_, comp, id)
			if comp == "InventorySetup" and id == 50 then return { isInitialized = true, linksEntities = {} } end
			if comp == "PlayerState" and id == 1 then return {} end
			return nil
		end,
		position = function(_, id) return 0, 0, (id == 1) and 0 or 3 end,
		entities = function() return { 1, 50 } end,
		account = function(_, id) return id == 1 and "a" or nil end,
	}
	emf = {
		health = function(_, id) return r.health[id] end,
		offer = function(_, id) r.offers[#r.offers + 1] = id return true end,
		offered = function() return true end,
		offer_remove = function() end,
		put = function() end,
		give = function() end,
		log = function() end,
	}
	local bag = dofile(path)
	r.bag = bag.new({
		now = function() return r.now end,
		tell = function() end,
		save = function() end,
		owns = function(id, account) return id == 50 and account == "a" end,
	})
	r.step = function(seconds)
		for _ = 1, (seconds or 1) * 4 do
			r.now = r.now + 0.25
			r.bag:step(r.world)
		end
	end
	return r
end

test("a new pack beast that reads health 0 for its first moments is kept, and its bag offered (seen live)", function()
	local r = rig()
	r.bag:spawned(r.world, "a", 50, 1)
	r.health[50] = 0  -- the game has not filled its health in yet
	r.step(1)
	eq(r.bag.live[50] ~= nil, true, "still watched")
	r.health[50] = 252
	r.step(2)
	eq(#r.offers >= 1, true, "offered")
	eq(r.offers[1], 50, "offered on the pack beast")
end)

test("health 0 after it was alive is a fall: the bag lets it go", function()
	local r = rig()
	r.bag:spawned(r.world, "a", 50, 1)
	r.health[50] = 252
	r.step(1)
	eq(r.bag.live[50] ~= nil, true, "watched while alive")
	r.health[50] = 0
	r.step(1)
	eq(r.bag.live[50], nil, "let go after the fall")
end)

test("an id that is no longer the account's pack beast is let go at once", function()
	local r = rig()
	r.bag:spawned(r.world, "b", 50, 1)  -- owns() says 50 is account a's
	r.health[50] = 252
	r.step(1)
	eq(r.bag.live[50], nil, "let go")
end)

if failures > 0 then
	io.stderr:write(string.format("%d of %d failed\n", failures, ran))
	os.exit(1)
end
print(string.format("%d passed", ran))
