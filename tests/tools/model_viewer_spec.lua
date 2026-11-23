-- Unit tests for tools/models/model_viewer/main.lua's order commands, run by test_model_viewer.py with a Lua 5.4
-- interpreter: lua model_viewer_spec.lua <path of main.lua>. A stand-in for EMF's Lua API records what the mod asks.
local script = assert(arg[1], "usage: lua model_viewer_spec.lua <model_viewer/main.lua>")

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
	if a ~= b then error((what or "value") .. ": " .. tostring(a) .. ", expected " .. tostring(b), 2) end
end

-- a fresh mod on a fresh fake: returns the commands, the tick callback and what was recorded
local function load_mod()
	local rec = { replies = {}, orders = {}, cancelled = {}, now = 100.0, next_id = 500, next_order = 1, alive = {} }
	local world = {}
	function world:is_server() return true end
	function world:tick() return rec.now * 60 end
	function world:position(id)
		if id == 1 or rec.alive[id] then return 10, 20, 30 end
	end
	function world:get(name)
		if name == "Faction" then return { faction = 0 } end
	end
	local commands, tick
	emf = {
		emits = function() end,
		session = function() return 1 end,
		on_tick = function(fn) tick = fn end,
		command = function(name, _, fn) commands[name] = fn end,
		reply = function(text) rec.replies[#rec.replies + 1] = text end,
		local_player = function() return 1 end,
		emit = function() end,
		health = function() return 10, 10 end,
		on_event = function() end,
		spawn = function()
			rec.next_id = rec.next_id + 1
			rec.alive[rec.next_id] = true
			return rec.next_id
		end,
		destroy = function(_, id) rec.alive[id] = nil end,
		enemy_order = function(_, enemy, target, opts)
			if rec.refuse then return nil, rec.refuse end
			rec.orders[#rec.orders + 1] = { enemy = enemy, target = target, seconds = opts.seconds, allies = opts.allies,
				at = rec.now }
			rec.next_order = rec.next_order + 1
			return rec.next_order
		end,
		enemy_order_cancel = function(id) rec.cancelled[#rec.cancelled + 1] = id end,
	}
	commands = {}
	assert(loadfile(script))()
	return commands, function() tick(world) end, rec, world
end

local GUID = "2e377d42-ff1e-4e6e-b77a-1596e9404afd"

test("order sends every ally at the last foe as an ally's order", function()
	local c, _, rec, world = load_mod()
	c.show(world, { GUID }); c.show(world, { GUID }); c.foe(world, { GUID })
	c.order(world, {})
	eq(#rec.orders, 2, "orders")
	eq(rec.orders[1].enemy, 501); eq(rec.orders[2].enemy, 502)
	eq(rec.orders[1].target, 503, "target")
	eq(rec.orders[1].allies, true, "allies")
	eq(rec.replies[#rec.replies], "2 allies ordered at 503, renewed every 3 s (model_viewer.order off ends it)")
end)

test("the order is renewed every three seconds, not sooner", function()
	local c, tick, rec, world = load_mod()
	c.show(world, { GUID }); c.foe(world, { GUID }); c.order(world, {})
	rec.now = 102.9; tick()
	eq(#rec.orders, 1, "before three seconds")
	rec.now = 103.1; tick()
	eq(#rec.orders, 2, "after three seconds")
	eq(rec.orders[2].at, 103.1)
	rec.now = 104.0; tick()
	eq(#rec.orders, 2, "one second later")
end)

test("order off ends the orders and the renewing", function()
	local c, tick, rec, world = load_mod()
	c.show(world, { GUID }); c.foe(world, { GUID }); c.order(world, {})
	c.order(world, { "off" })
	eq(#rec.cancelled, 1, "cancelled")
	eq(rec.replies[#rec.replies], "orders ended")
	rec.now = 110; tick()
	eq(#rec.orders, 1, "no renewal")
end)

test("clear ends the renewing too", function()
	local c, tick, rec, world = load_mod()
	c.show(world, { GUID }); c.foe(world, { GUID }); c.order(world, {})
	c.clear(world, {})
	rec.now = 110; tick()
	eq(#rec.orders, 1, "no renewal")
end)

test("without a foe it says so", function()
	local c, _, rec, world = load_mod()
	c.show(world, { GUID })
	c.order(world, {})
	eq(#rec.orders, 0)
	eq(rec.replies[#rec.replies], "no foe (model_viewer.foe first)")
end)

test("a refused order is reported with the reason", function()
	local c, _, rec, world = load_mod()
	c.show(world, { GUID }); c.foe(world, { GUID })
	rec.refuse = "no enemy AI"
	c.order(world, {})
	eq(rec.replies[#rec.replies], "0 allies ordered at 502 (501: no enemy AI)")
end)

test("an EMF without enemy orders is named", function()
	local c, _, rec, world = load_mod()
	c.show(world, { GUID }); c.foe(world, { GUID })
	emf.enemy_order = nil
	c.order(world, {})
	eq(rec.replies[#rec.replies], "this EMF has no enemy orders (emf.enemy_order, Lua host 1.32)")
end)

io.write(string.format("%d tests, %d failed\n", ran, failures))
os.exit(failures == 0 and 0 or 1)
