-- FTSE user script (FTSE's init.lua loads core/FTSE/ftse.lua).
-- Loads the co-op mod. netlog.dll goes in the game folder, next to BOS.exe.
-- If you already have an ftse.lua from another mod, add the lines below to it instead.
local ok, err = package.loadlib("netlog.dll", "*")
if not ok then logger:log("netlog load failed: " .. tostring(err)) end

-- Co-op: burst rolls the same on every copy. FTSE's burst fix (OnBurstAttack in init.lua) rolls each bullet with
-- math.random(), seeded from os.time(): every copy of a co-op game rolled its own numbers, so an NPC's burst hit a
-- character on one copy and missed on the others, and the copies drifted apart from there. The same rules as
-- FTSE's fix (CalculateChance / CalculateShare in init.lua), with the rolls from a generator seeded by what every
-- copy has alike: the game time, the attacker, the shots and every target's id, chance and angle.
local M = 2147483647  -- Park-Miller: state * 48271 % M stays exact in a Lua number

local function mix(h, v)
  v = math.floor((tonumber(v) or 0) * 1000 + 0.5) % M
  return (h * 31 + v + 7) % M
end

local function time_seed(h)
  local okt, t = pcall(function() return world:GetTime() end)
  if not okt then return h end
  if type(t) == "table" then
    local keys = {}
    for k in pairs(t) do keys[#keys + 1] = tostring(k) end
    table.sort(keys)
    for _, k in ipairs(keys) do h = mix(h, tonumber(t[k]) or 0) end
    return h
  end
  return mix(h, tonumber(t) or 0)
end

local function id_of(e)
  if type(e) ~= "table" and type(e) ~= "userdata" then return 0 end
  local okid, id = pcall(function() return e["id"] end)
  return okid and tonumber(id) or 0
end

function OnBurstAttack(attacker, shots, target_table)
  local h = time_seed(12345)
  h = mix(h, id_of(attacker))
  h = mix(h, shots)
  for _, tgt in ipairs(target_table) do
    h = mix(h, id_of(tgt["actor"]))
    h = mix(h, tgt["hit_chance"])
    h = mix(h, tgt["angle"])
    h = mix(h, tgt["intended"] and 1 or 0)
  end
  local state = h % (M - 1) + 1
  local function roll()
    state = (state * 48271) % M
    return math.floor(state / M * 100)
  end

  local results = {}
  local totalshare = 0
  for _, tgt in ipairs(target_table) do
    totalshare = totalshare + CalculateShare(CalculateChance(tgt["hit_chance"], tgt["intended"], tgt["angle"]))
  end
  totalshare = math.max(totalshare, 100)
  local shotsleft = shots
  for _, tgt in ipairs(target_table) do
    local chance = CalculateChance(tgt["hit_chance"], tgt["intended"], tgt["angle"])
    local tgtshots = math.min(shotsleft, math.max(1, math.floor(shots * CalculateShare(chance) / totalshare)))
    shotsleft = shotsleft - tgtshots
    local myhits = 0
    for i = 1, tgtshots do
      if roll() < chance then myhits = myhits + 1 end
    end
    table.insert(results, {id = tgt["actor"]["id"], hits = myhits})
  end
  return results
end
