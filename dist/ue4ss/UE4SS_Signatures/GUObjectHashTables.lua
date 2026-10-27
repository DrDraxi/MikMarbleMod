-- FUObjectHashTables::Get -- PLACEHOLDER for Marbles on Stream, UE 5.8 (CL-58210709)
--
-- THIS DOES NOT FIND FUObjectHashTables::Get. Read before trusting the log line
-- "GUObjectHashTables_Get address: ... <- Lua Script".
--
-- Since the Oct-2026 game update none of patternsleuth's FUObjectHashTablesGet
-- patterns match ("Failed to find FUObjectHashTables::Get()"), and a failed
-- resolver fails the whole PS scan, so UE4SS retries until it times out and no
-- mod loads.
--
-- But UE4SS v3.0.1 never USES this address. Checked in source:
--   * deps/first/Unreal/src/UnrealInitializer.cpp copies every other PS result
--     out of PsScanResults; results.fuobject_hash_tables_get is never read.
--   * UE4SS/src/Signatures.cpp's Lua-override callback for this file only
--     prints the address and stores it nowhere.
-- So the only job of this override is to take the resolver out of the PS scan.
-- It returns the address of a known-unique code site (the GNatives
-- EX_LocalVariable registration stub, see GNatives.lua) purely so the override
-- reports success. Nothing dereferences it.
--
-- If a future UE4SS starts consuming this value, replace this with a real
-- signature -- do not ship this placeholder against such a build.

function Register()
    return "48 8D 05 ?? ?? ?? ?? 48 39 05 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 74 0A C7 05 ?? ?? ?? ?? 00 00 00 00 C3"
end

function OnMatchFound(MatchAddress)
    return MatchAddress
end
