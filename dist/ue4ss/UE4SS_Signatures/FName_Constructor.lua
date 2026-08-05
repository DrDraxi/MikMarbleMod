-- FName::FName(const WIDECHAR* Name, EFindName FindType)
--
-- Marbles on Stream moved to Unreal Engine 5.8 (Steam build 24556537,
-- ++UE5+Release-5.8-CL-55116800). On that build UE4SS's built-in pattern scan
-- can no longer resolve this constructor, and because FName::FName(wchar_t*)
-- is one of the REQUIRED symbols the entire scan aborts:
--
--     [PS] Failed to find FName::FName(wchar_t*): FNameCtorWchar
--     [PS] Scan failed                                (x83 attempts)
--     Fatal Error: PS scan timed out
--
-- UE4SS then never finishes initialising, so no mod loads at all -- MikMarbleMod
-- included. (GNatives and FUObjectHashTables::Get() also fail to resolve on this
-- build, but both are optional and do not abort the scan.)
--
-- WHY THE STOCK PATTERN BROKE
-- Upstream identifies the constructor from its CALL SITES, keying on the
-- EFindName argument being materialised by the caller:
--     41 B8 01 00 00 00      mov r8d, 1        ; EFindName::FNAME_Add
-- In this build FNAME_Add is baked INSIDE the constructor instead (it passes
-- r8d=1 to its own inner resolver), so callers no longer set r8 at all --
-- 5908 of the 5911 direct call sites don't touch it. Hence zero matches.
--
-- THE FIX
-- Match the constructor's own prologue, which does not depend on how callers
-- pass their arguments:
--
--     40 53                  push rbx
--     48 83 EC 30            sub  rsp, 0x30
--     48 8B D9               mov  rbx, rcx            ; this
--     48 89 54 24 20         mov  [rsp+0x20], rdx     ; Name
--     33 C9                  xor  ecx, ecx
--     4C 8B CA               mov  r9, rdx
--     44 8B C1               mov  r8d, ecx
--     48 85 D2               test rdx, rdx
--     74 ??                  je   <empty -> NAME_None>
--     0F B7 02               movzx eax, word ptr [rdx]   <-- 16-bit load
--     66 85 C0               test  ax, ax
--     74                     je   ...
--
-- The 0F B7 02 word load is the critical discriminator. The ANSICHAR overload
-- sits immediately before this one (at -0xA0) with a byte-for-byte identical
-- prologue up through `test rdx,rdx`, diverging only at its 8-bit load
-- (0F B6 02 / inc rdx). Matching that one instead would corrupt every name
-- lookup, so the signature deliberately extends past the divergence point.
--
-- Verified: exactly one match across the entire image.

function Register()
    return "40 53 48 83 EC 30 48 8B D9 48 89 54 24 20 33 C9 4C 8B CA 44 8B C1 48 85 D2 74 ?? 0F B7 02 66 85 C0 74"
end

function OnMatchFound(MatchAddress)
    -- The AOB matches the function's first byte, so the match is the address.
    return MatchAddress
end
