-- FTSE user script (FTSE's init.lua loads core/FTSE/ftse.lua).
-- Loads the co-op mod. netlog.dll goes in the game folder, next to BOS.exe.
-- If you already have an ftse.lua from another mod, add these two lines to it instead.
local ok, err = package.loadlib("netlog.dll", "*")
if not ok then logger:log("netlog load failed: " .. tostring(err)) end
