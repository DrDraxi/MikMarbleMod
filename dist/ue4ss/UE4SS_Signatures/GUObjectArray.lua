-- GUObjectArray (FUObjectArray) for Marbles on Stream on Unreal Engine 5.8
--
-- WHY THIS IS NEEDED
-- UE 5.8 reordered FUObjectArray. ObjObjects (the TUObjectArray holding every
-- UObject) moved from offset 0x10 to offset 0x0, and everything after it
-- shifted:
--
--            member                          5.7      5.8
--            ObjObjects                      0x10     0x00
--            ObjFirstGCIndex                 0x00     0x20
--            UObjectDeleteListeners          0x80     0xA8
--            (total size)                    0xC0     0xE8
--
-- patternsleuth's built-in GUObjectArray resolver still applies the older
-- correction, so on this build it reports FUObjectArray + 0x68 instead of the
-- real base. Verified against live process memory:
--
--   reported base + 0x00 -> 0x000000AD          (not a pointer - garbage)
--   real base     + 0x00 -> Objects=0x7FEC43C29240
--                           NumElements=61712  MaxElements=2162688
--                           NumChunks=1        MaxChunks=33
--   (internally consistent: 2162688 / 65536 = 33 chunks, 61712 fits in 1)
--
-- The consequence is not a crash but a hang: UE4SS spins forever in
--     while (UObjectArray::GetNumElements() < 10000) {}
-- printing only "Waiting for object construction..." and burning a core, so no
-- mod ever loads.
--
-- Cross-check on the static image: the real base has 3455 `48 8B 05` code
-- references; the address patternsleuth returns has none (only misaligned
-- byte coincidences).
--
-- THE SIGNATURE
-- Matches the common idiom that loads the object array and indexes a chunk:
--
--     48 8B 05 <rel32>   mov rax, [rip+GUObjectArray]   ; ObjObjects.Objects
--     48 8B 0C C8        mov rcx, [rax+rcx*8]           ; chunk lookup
--     ...
--
-- Because ObjObjects now sits at offset 0x0, the resolved address IS the
-- FUObjectArray base. Note this deliberately does NOT subtract 0x10 the way
-- the bundled GUObjectArray.lua.example does -- that subtraction is exactly the
-- stale correction that breaks on 5.8.
--
-- Verified unique across the entire image (exactly 1 match).

function Register()
    return "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8D 04 D1 EB 03 49 8B C7 48 8B 00 48 C1 F8 20 C1 E8 19 A8 01 0F 85 C3 02 00 00 41 8B"
end

function OnMatchFound(MatchAddress)
    -- MatchAddress points at the `mov rax, [rip+disp32]`.
    local NextInstr = MatchAddress + 7
    return NextInstr + DerefToInt32(MatchAddress + 3)
end
