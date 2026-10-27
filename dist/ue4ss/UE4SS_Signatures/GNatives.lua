-- GNatives for Marbles on Stream, UE 5.8 (CL-58210709, Oct-2026 game update)
--
-- WHY THIS IS NEEDED
-- patternsleuth finds GNatives two ways and both stopped matching on this build:
-- the inlined-GRegisterNative init pattern (its bInitialized guard is gone) and
-- the UObject::SkipFunction prologue. UE4SS then logs
--     [PS] Failed to find GNatives: GNatives: expected at least one value
-- and the scan retries until "PS scan timed out", so no mod loads.
--
-- GNatives matters to this mod: UE4SS's UFunction hook path re-steps script
-- frames through GNatives_Internal, so it must be the real table, not a guess.
--
-- HOW THE ADDRESS WAS ESTABLISHED (two independent confirmations)
--   1. Live memory of the unmodded game: a 2 KB array in .data at RVA 0xB4E6610
--      of code pointers, ~164 slots all = &UObject::execUndefined, the rest
--      distinct exec* functions; slot 0x0B (EX_Nothing) is the tiny folded stub.
--   2. The script VM's inlined FFrame::Step dispatch indexes exactly that RVA:
--          movzx ecx, byte ptr [rdx]                    ; opcode
--          mov   r9, qword ptr [r15 + rcx*8 + 0xB4E6610] ; r15 = image base
--          call  r9
--   All 91 static GRegisterNative stubs (below) resolve to the same base.
--
-- THE SIGNATURE
-- Each IMPLEMENT_VM_FUNCTION now compiles to a stand-alone static-init stub:
--
--     48 8D 05 <rel32>   lea rax, [&UObject::execUndefined]
--     48 39 05 <rel32>   cmp [GNatives + idx*8], rax      ; slot still free?
--     48 8D 05 <rel32>   lea rax, [&UObject::execXxx]
--     48 89 05 <rel32>   mov [GNatives + idx*8], rax
--     74 0A              je  ok
--     C7 05 <rel32> idx  mov [GNativeDuplicate], idx
--     C3                 ret
--
-- We match the idx = 0 (EX_LocalVariable) stub, so the cmp's target IS the
-- table base. Verified unique across the image (exactly 1 match).

function Register()
    return "48 8D 05 ?? ?? ?? ?? 48 39 05 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 74 0A C7 05 ?? ?? ?? ?? 00 00 00 00 C3"
end

function OnMatchFound(MatchAddress)
    -- cmp qword ptr [rip+disp32], rax starts at +7; disp32 at +10; next instr at +14.
    return MatchAddress + 14 + DerefToInt32(MatchAddress + 10)
end
