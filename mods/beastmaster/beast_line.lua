-- The beast line at the bottom left ("Beasts (defending): Wolf (out), Bear (rests 1:20)"): its parts, made on the
-- server, and its text, drawn by the host's game or by a joined player's game (which counts the rests down itself:
-- the server sends the line only when it changed, remote.lua).
local M = {}

local DRIFT_S = 2  -- s: a rest whose end moved more than this since the line was sent sends it again

-- The parts of a player's line: their beasts resting (with the seconds left; also one out that comes back after its
-- rest) and out ("out", or "aside" while they stepped aside), in list order.
function M.parts(list, aside)
	local parts = {}
	for _, b in ipairs(list) do
		if b.rest > 0 then
			parts[#parts + 1] = { name = b.name, state = "rest", rest = b.rest }
		elseif b.out then
			parts[#parts + 1] = { name = b.name, state = aside and "aside" or "out" }
		end
	end
	return parts
end

-- The line's start: with the player's mode, or without one (no account yet).
function M.head(mode)
	return mode and string.format("Beasts (%s): ", mode) or "Beasts: "
end

-- The line `elapsed` seconds after its parts were made; "" when no part shows (a rest that ran out goes).
function M.text(head, parts, elapsed)
	local shown = {}
	for _, p in ipairs(parts) do
		if p.state == "rest" then
			local left = p.rest - (elapsed or 0)
			if left > 0 then
				shown[#shown + 1] = string.format("%s (rests %d:%02d)", p.name, math.floor(left / 60), math.floor(left % 60))
			end
		else
			shown[#shown + 1] = p.name .. (p.state == "aside" and " (aside)" or " (out)")
		end
	end
	return #shown > 0 and (head .. table.concat(shown, ", ")) or ""
end

-- What was sent at `now`, to compare the next line with.
function M.snapshot(head, parts, now)
	local s = { head = head, parts = {} }
	for i, p in ipairs(parts) do s.parts[i] = { name = p.name, state = p.state, ends = p.rest and now + p.rest } end
	return s
end

-- Whether the line (head, parts at `now`) differs from the one sent (`sent`, nil: none): another head, other beasts
-- or states, or a rest whose end moved by more than DRIFT_S.
function M.changed(sent, head, parts, now)
	if not sent or sent.head ~= head or #sent.parts ~= #parts then return true end
	for i, p in ipairs(parts) do
		local q = sent.parts[i]
		if q.name ~= p.name or q.state ~= p.state then return true end
		if p.rest and (not q.ends or math.abs(now + p.rest - q.ends) > DRIFT_S) then return true end
	end
	return false
end

return M
