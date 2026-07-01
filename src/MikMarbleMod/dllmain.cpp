// MikMarbleMod — Marbles on Stream result tracker (UE4SS C++ mod)
//
// Reads the final standings of each race / royale and POSTs them as JSON to an
// HTTP endpoint so an external service can react to who won and who lost.
//
// ── How it reads results (event-driven) ─────────────────────────────────────
// Earlier versions polled the result UMG widgets once per second and recovered
// each field by *discovering byte offsets* inside an opaque result data object,
// cross-referencing the animating on-screen table. That was the source of the
// "sometimes wrong winner/loser" flakiness: it latched on a half-rendered frame,
// the offset discovery needed a finisher+DNF mix the visible ~24-row window
// rarely showed, and off-screen rows fell back to alphabetical ordering.
//
// A reflection capture of the game (see git history / the round-N dumps) showed
// the result data object is genuinely opaque to UE4SS reflection, but the gameplay
// EVENTS are not. So the mod hooks the game's own UFunctions on AMarbleRaceHUDY2:
//   • OnMarbleFinishedRace(Marble)  → finish order (race mode)
//   • OnMarbleEliminated(Marble, DamageInstigator, DamageEvent)
//       → elimination order (loser first); DamageInstigator carries the killer's
//         Username/DisplayName, so per-player elimination counts are event-derived
//   • OnMatchEnded()                → the one authoritative "results are final" trigger
// plus the royale resurrection path (a powerup pickup can revive an eliminated
// marble): AMarbleRoyaleGameMode::RespawnMarble(PlayerState) — the function that
// actually spawns the new pawn, verified by forcing respawns in live matches —
// and URoyaleGameRespawnComponent::BP_RespawnPlayer as a second net. On respawn
// the player's earlier elimination is erased so only the FINAL death (or
// survival) determines their placement. A hook-free safety net backs this up at
// match end: a standings leader who is "eliminated" but demonstrably alive was
// resurrected, and is un-eliminated before the winner is chosen — so a
// resurrected marble that goes on to win is crowned even if no hook saw the
// respawn, and the loser slot isn't polluted by someone who died first but
// came back.
// Each Marble exposes its player name (_Username) and PlayerState by reflection,
// so standings are reconstructed from authoritative, virtualization-proof events
// with zero guessed offsets and no dependence on the UI having rendered.
//
// Payload: {"type":"race"|"royale","players":[{"name","finished","eliminations"}]}
// ordered finishers-first then DNFs (last entry = first eliminated = the loser).

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>      // UClass, UFunction, RegisterPostHook
#include <Unreal/CoreUObject/UObject/UnrealType.hpp> // TFieldRange, EFieldIterationFlags
#include <Unreal/UFunctionStructs.hpp>               // UnrealScriptFunctionCallableContext
#include <UE4SSProgram.hpp>
#include <imgui.h>

#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <set>
#include <map>
#include <memory>
#include <deque>
#include <cwctype>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <excpt.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

using namespace RC;
using namespace RC::Unreal;

// ── Config ──────────────────────────────────────────────────────────────────
//
// The mod POSTs to {path}/ready and {path}/results. The default and the
// auto-created config.txt both use /mod, matching the bundled dummy server.

static constexpr const char* kDefaultPath = "/mod";

struct ModConfig
{
    std::string host = "127.0.0.1";
    int         port = 5000;
    std::string path = kDefaultPath;
};

static std::filesystem::path GetModDirectory()
{
    HMODULE hMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GetModDirectory), &hMod);
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(hMod, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path().parent_path(); // dlls/main.dll -> MikMarbleMod/
}

static std::string Trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static ModConfig LoadConfig()
{
    ModConfig cfg;
    try
    {
        auto configPath = GetModDirectory() / "config.txt";

        std::ifstream file(configPath);
        if (!file.is_open())
        {
            std::ofstream out(configPath);
            out << "host=127.0.0.1\n";
            out << "port=5000\n";
            out << "path=" << kDefaultPath << "\n";
            return cfg;
        }

        std::string line;
        while (std::getline(file, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Trim(line.substr(0, eq));
            std::string val = Trim(line.substr(eq + 1));
            if (key == "host") cfg.host = val;
            else if (key == "port") { try { cfg.port = std::stoi(val); } catch (...) {} }
            else if (key == "path") cfg.path = val;
        }
    }
    catch (...) { /* fall back to defaults */ }
    return cfg;
}

// ── Result payload type ─────────────────────────────────────────────────────

struct PlayerResult
{
    std::wstring name;
    bool         finished = false;
    int          eliminations = 0;   // kills credited to this player (event-derived)
};

// ── SEH-protected raw-memory readers ────────────────────────────────────────
//
// FindAllOf / hooked params can hand back stale or in-flux UObjects. Every raw
// dereference goes through SafeReadBytes so an access violation can't crash the
// game thread; it's swallowed and treated as "couldn't read".

__declspec(noinline) static bool SafeReadBytes(const void* src, void* dst, size_t bytes) noexcept
{
    __try { memcpy(dst, src, bytes); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static UClass* SafeGetClassPrivate(UObject* obj) noexcept
{
    UClass* cls = nullptr;
    __try { cls = obj->GetClassPrivate(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { cls = nullptr; }
    return cls;
}

struct FStringRaw { wchar_t* Data; int32_t Num; int32_t Max; };  // Num includes the null terminator
struct TArrayRaw  { void* Data; int32_t Num; int32_t Max; };
struct FVectorD   { double X, Y, Z; };  // UE5 FVector (double precision, size 0x18)

// Reads + validates an FString at `src`. Accepts 1..127 chars; allows non-ASCII
// (Unicode stream names) but rejects control chars and obviously-bad memory.
static bool SafeReadFString(const void* src, std::wstring& out)
{
    FStringRaw fs{};
    if (!SafeReadBytes(src, &fs, sizeof(fs))) return false;
    if (!fs.Data || fs.Num <= 1 || fs.Num > 128 || fs.Max < fs.Num) return false;

    wchar_t buf[128]{};
    int32_t bytes = fs.Num * static_cast<int32_t>(sizeof(wchar_t));
    if (!SafeReadBytes(fs.Data, buf, bytes)) return false;
    if (buf[fs.Num - 1] != 0) return false;
    for (int i = 0; i < fs.Num - 1; ++i)
        if (buf[i] != 0 && buf[i] < 0x20) return false;   // reject control chars
    try { out.assign(buf, buf + (fs.Num - 1)); }
    catch (...) { return false; }
    return true;
}

// ── Reflection helpers (by property name) ───────────────────────────────────

// Read an object-pointer property (e.g. Marble.PlayerState, Marble.LastHitBy).
static UObject* ReadObjProp(UObject* obj, const TCHAR* name)
{
    if (!obj) return nullptr;
    UObject** pp = nullptr;
    try { pp = obj->GetValuePtrByPropertyNameInChain<UObject*>(name); } catch (...) { return nullptr; }
    if (!pp) return nullptr;
    UObject* o = nullptr;
    if (!SafeReadBytes(pp, &o, sizeof(o))) return nullptr;
    return o;
}

// Read a bool/bitfield property by name. Engine bitfields (bIsSpectator etc.)
// share one byte; the FBoolProperty carries the exact byte offset and mask.
static bool ReadBoolProp(UObject* obj, const TCHAR* name, bool& out)
{
    if (!obj) return false;
    UClass* cls = SafeGetClassPrivate(obj);
    if (!cls) return false;
    for (FProperty* prop : TFieldRange<FProperty>(cls, EFieldIterationFlags::IncludeSuper))
    {
        if (!prop) continue;
        std::wstring n;
        try { n = prop->GetName(); } catch (...) { continue; }
        if (n != name) continue;
        try
        {
            auto* bp = static_cast<FBoolProperty*>(prop);
            uint8 mask = bp->GetByteMask();
            if (mask == 0 || (mask & (mask - 1)) != 0) return false;   // not a plausible bool prop
            uint8 byteVal = 0;
            const char* addr = reinterpret_cast<const char*>(obj) + prop->GetOffset_Internal() + bp->GetByteOffset();
            if (!SafeReadBytes(addr, &byteVal, 1)) return false;
            out = (byteVal & mask) != 0;
            return true;
        }
        catch (...) { return false; }
    }
    return false;
}

// Read a string property (FString) by name.
static bool ReadStrProp(UObject* obj, const TCHAR* name, std::wstring& out)
{
    if (!obj) return false;
    void* fp = nullptr;
    try { fp = obj->GetValuePtrByPropertyNameInChain<FStringRaw>(name); } catch (...) { return false; }
    if (!fp) return false;
    return SafeReadFString(fp, out);
}

// Replay/ghost marbles (GhostMarble_BP_C) derive from AMarble and sit near the
// track — they must never enter the standings.
static bool IsGhostMarble(UObject* obj)
{
    UClass* cls = SafeGetClassPrivate(obj);
    if (!cls) return false;
    std::wstring cn;
    try { cn = cls->GetName(); } catch (...) { return false; }
    return cn.find(L"Ghost") != std::wstring::npos;
}

// Resolve a display username from a Marble pawn OR a PlayerState. Marbles carry
// _Username directly; PlayerStates carry Username; a marble also links its
// PlayerState. Try all so the same helper works for marbles and Top10 entries.
static std::wstring ResolveUsername(UObject* obj)
{
    std::wstring s;
    if (!obj) return s;
    if (ReadStrProp(obj, STR("_Username"), s) && !s.empty()) return s;
    if (ReadStrProp(obj, STR("Username"),  s) && !s.empty()) return s;
    if (UObject* ps = ReadObjProp(obj, STR("PlayerState")))
        if (ReadStrProp(ps, STR("Username"), s) && !s.empty()) return s;
    return s;
}

// Read element 0 of a UObject*-array property (e.g. GameState.Top10[0]).
static UObject* ReadFirstArrayElem(UObject* obj, const TCHAR* arrayName)
{
    if (!obj) return nullptr;
    void* arrField = nullptr;
    try { arrField = obj->GetValuePtrByPropertyNameInChain<TArrayRaw>(arrayName); } catch (...) { return nullptr; }
    if (!arrField) return nullptr;
    TArrayRaw a{};
    if (!SafeReadBytes(arrField, &a, sizeof(a))) return nullptr;
    if (a.Num <= 0 || a.Num > 100000 || !a.Data) return nullptr;
    UObject* o = nullptr;
    if (!SafeReadBytes(a.Data, &o, sizeof(o))) return nullptr;   // first element
    return o;
}

// Read up to maxN elements of a UObject*-array property (e.g. GameState.Top10).
// Skips null slots; returns them in array order.
static std::vector<UObject*> ReadObjectArray(UObject* obj, const TCHAR* arrayName, int maxN = 256)
{
    std::vector<UObject*> out;
    if (!obj) return out;
    void* arrField = nullptr;
    try { arrField = obj->GetValuePtrByPropertyNameInChain<TArrayRaw>(arrayName); } catch (...) { return out; }
    if (!arrField) return out;
    TArrayRaw a{};
    if (!SafeReadBytes(arrField, &a, sizeof(a))) return out;
    if (a.Num <= 0 || a.Num > 100000 || !a.Data) return out;
    int n = a.Num < maxN ? a.Num : maxN;
    for (int i = 0; i < n; ++i)
    {
        UObject* o = nullptr;
        if (!SafeReadBytes(static_cast<const char*>(a.Data) + i * sizeof(UObject*), &o, sizeof(o))) break;
        if (o) out.push_back(o);
    }
    return out;
}

// Count of players in the match (GameState.PlayerArray), for a sanity log.
static int ReadPlayerArrayNum(UObject* gs)
{
    if (!gs) return -1;
    void* arrField = nullptr;
    try { arrField = gs->GetValuePtrByPropertyNameInChain<TArrayRaw>(STR("PlayerArray")); } catch (...) { return -1; }
    if (!arrField) return -1;
    TArrayRaw a{};
    if (!SafeReadBytes(arrField, &a, sizeof(a))) return -1;
    if (a.Num < 0 || a.Num > 100000) return -1;
    return a.Num;
}

// ── HTTP POST (runs on a background thread) ─────────────────────────────────

struct HttpResult
{
    bool        ok = false;
    int         statusCode = 0;
    DWORD       lastError = 0;
    std::string label;
};

static HttpResult HttpPostJson(const std::string& host, int port,
                               const std::string& path, const std::string& jsonBody) noexcept
{
    HttpResult r;
    r.label = path;
    HINTERNET hSession = nullptr, hConnect = nullptr, hRequest = nullptr;
    try
    {
        hSession = WinHttpOpen(L"MikMarbleMod/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
        if (!hSession) { r.lastError = GetLastError(); goto done; }
        WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

        {
            std::wstring wHost(host.begin(), host.end());
            hConnect = WinHttpConnect(hSession, wHost.c_str(), static_cast<INTERNET_PORT>(port), 0);
        }
        if (!hConnect) { r.lastError = GetLastError(); goto done; }

        {
            std::wstring wPath(path.begin(), path.end());
            hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        }
        if (!hRequest) { r.lastError = GetLastError(); goto done; }

        {
            const wchar_t* headers = L"Content-Type: application/json\r\n";
            BOOL ok = WinHttpSendRequest(hRequest, headers, static_cast<DWORD>(-1),
                                         const_cast<char*>(jsonBody.c_str()), static_cast<DWORD>(jsonBody.size()),
                                         static_cast<DWORD>(jsonBody.size()), 0);
            if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);
            if (ok)
            {
                DWORD statusCode = 0, statusSize = sizeof(statusCode);
                WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
                r.ok = true;
                r.statusCode = static_cast<int>(statusCode);
            }
            else { r.lastError = GetLastError(); }
        }
    }
    catch (...) { /* swallow */ }
done:
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return r;
}

// ── JSON helpers ────────────────────────────────────────────────────────────

static std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
    return out;
}

static std::string EscapeJsonString(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c; break;
        }
    }
    return out;
}

static std::string BuildResultsJson(const std::vector<PlayerResult>& players, const std::string& gameType)
{
    std::string json = R"({"type": ")" + EscapeJsonString(gameType) + R"(", "players": [)";
    for (size_t i = 0; i < players.size(); ++i)
    {
        if (i > 0) json += ", ";
        json += R"({"name": ")" + EscapeJsonString(WideToUtf8(players[i].name)) +
                R"(", "finished": )" + (players[i].finished ? "true" : "false") +
                R"(, "eliminations": )" + std::to_string(players[i].eliminations) + "}";
    }
    json += "]}";
    return json;
}

// Shared state held by HTTP workers via shared_ptr so a worker outlives the mod
// instance if UE4SS unloads mid-request.
struct HttpSharedState
{
    std::mutex                mtx;
    std::vector<PlayerResult> lastSentPlayers;
    std::string               lastGameType;
    std::deque<HttpResult>    pendingLogs;     // drained on the game thread
    std::atomic<bool>         httpBusy{false};
    std::atomic<int>          inflight{0};
};

// ── Per-match event accumulator ─────────────────────────────────────────────
//
// Populated by the UFunction post-hooks, consumed when OnMatchEnded fires. The
// hooks and on_update both run on the game thread; the mutex is belt-and-braces
// in case a hook ever fires off-thread.
// Killer names arrive as FDamageInstigator.Username/DisplayName, which differ
// only in casing from the marble's _Username (Twitch login vs display name), so
// elimination counts are keyed case-insensitively.
static std::wstring ToLowerW(std::wstring s)
{
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

namespace mik
{
    static std::mutex                  g_mtx;
    static std::vector<std::wstring>   g_finishOrder;   // race: finishers, in order
    static std::vector<std::wstring>   g_elimOrder;     // eliminated players, in order (first = loser)
    static std::set<std::wstring>      g_finishSet;
    static std::set<std::wstring>      g_elimSet;
    static std::map<std::wstring, int> g_elimsBy;       // kills per player, key = lowercase killer DISPLAY name
    // lower(_Username) -> lower(_Displayname), recorded off each marble seen in a
    // hook. Standings use _Username ("Wuudetda12") but the DamageInstigator names
    // the killer by display name ("Wuudetda"), so kill lookups join through this.
    // (For real Twitch players the two differ only by case; bots differ by suffix.)
    static std::map<std::wstring, std::wstring> g_dispOf;
    static std::atomic<bool>           g_endPending{false};

    // Killer-name offsets inside the OnMarbleEliminated param block, resolved
    // from the UFunction's own param reflection at hook-registration time
    // (-1 = unresolved). Set once on the game thread before any hook can fire.
    static std::atomic<int32_t>        g_offKillerUser{-1};
    static std::atomic<int32_t>        g_offKillerDisp{-1};
    // BP_RespawnPlayer param-block offsets (PlayerState / return pawn).
    static std::atomic<int32_t>        g_offRespawnPS{-1};
    static std::atomic<int32_t>        g_offRespawnPawn{-1};
    // RespawnMarble(APlayerState*) -> AMarbleRoyaleMarble* param-block offsets.
    static std::atomic<int32_t>        g_offRespawn2PS{-1};
    static std::atomic<int32_t>        g_offRespawn2Pawn{-1};
    static int                         g_elimDebugLeft = 0;   // per-match raw-read log budget

    static void ResetMatch()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_finishOrder.clear();
        g_elimOrder.clear();
        g_finishSet.clear();
        g_elimSet.clear();
        g_elimsBy.clear();
        g_dispOf.clear();
        g_elimDebugLeft = 3;
        g_endPending.store(false);
    }

    // Remember a marble's username→displayname pair (for the kill-count join).
    static void RecordNamePair(const std::wstring& username, const std::wstring& display)
    {
        if (username.empty() || display.empty()) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        g_dispOf[ToLowerW(username)] = ToLowerW(display);
    }

    static void RecordFinish(const std::wstring& name)
    {
        if (name.empty()) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_finishSet.insert(name).second) g_finishOrder.push_back(name);
    }

    static void RecordElimination(const std::wstring& victim, const std::wstring& killer)
    {
        if (victim.empty()) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_elimSet.insert(victim).second) g_elimOrder.push_back(victim);
        if (!killer.empty()) ++g_elimsBy[ToLowerW(killer)];
    }

    // A royale resurrection: erase the player's earlier elimination so only their
    // FINAL death (or survival) counts. If they die again they re-enter the
    // elimination order at the new, later position. Kills already credited to
    // their eliminator are kept — the kill still happened.
    static void RecordRespawn(const std::wstring& name)
    {
        if (name.empty()) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_elimSet.erase(name) > 0)
        {
            g_elimOrder.erase(std::remove(g_elimOrder.begin(), g_elimOrder.end(), name),
                              g_elimOrder.end());
            return;
        }
        // Name-source mismatch fallback (respawn pawn vs PlayerState casing).
        std::wstring low = ToLowerW(name);
        for (auto it = g_elimOrder.begin(); it != g_elimOrder.end(); ++it)
            if (ToLowerW(*it) == low) { g_elimSet.erase(*it); g_elimOrder.erase(it); break; }
    }
}

// ── Mod class ───────────────────────────────────────────────────────────────

class MikMarbleMod : public RC::CppUserModBase
{
    std::shared_ptr<HttpSharedState> m_state = std::make_shared<HttpSharedState>();

    ModConfig  m_config;
    std::mutex m_configMutex;

    std::atomic<bool> m_unrealReady{false};
    std::atomic<bool> m_readySent{false};

    // Match lifecycle (game-thread only).
    UObject* m_curGameState   = nullptr;   // identity of the current match's GameState
    bool     m_hooksRegistered = false;    // UFunction hooks are session-global, register once
    bool     m_respawnHookRegistered = false; // royale respawn hook (component is royale-only)
    bool     m_loggedResolve   = false;    // log the HUD/function resolution once
    bool     m_sentThisMatch   = false;

    float m_pollTimer = 0.0f;
    static constexpr float POLL_INTERVAL = 0.5f; // seconds

    // ── GameState / mode ────────────────────────────────────────────────────

    static UObject* FindGameState()
    {
        if (UObject* gs = UObjectGlobals::FindFirstOf(STR("MarbleRoyaleGameState"))) return gs;
        if (UObject* gs = UObjectGlobals::FindFirstOf(STR("MarbleRaceGameState")))   return gs;
        if (UObject* gs = UObjectGlobals::FindFirstOf(STR("BullseyeGameState")))     return gs;
        if (UObject* gs = UObjectGlobals::FindFirstOf(STR("TiltedGameState")))       return gs;
        return nullptr;
    }

    static bool IsRoyale()
    {
        return UObjectGlobals::FindFirstOf(STR("MarbleRoyaleGameState")) != nullptr;
    }

    // Bullseye / "target" race: marbles settle near a target and are ranked by
    // proximity (score), not finish position. It derives from MarbleRaceGameState
    // but its scoring goes through ABullseyeFinish, NOT the OnMarbleFinishedRace /
    // OnMarbleEliminated delegates — so the event accumulator stays empty and this
    // mode needs its own standings read (see BuildBullseyeStandings).
    static bool IsBullseye()
    {
        return UObjectGlobals::FindFirstOf(STR("BullseyeGameState")) != nullptr;
    }

    // ── UFunction hooks ──────────────────────────────────────────────────────
    //
    // Hook the game's own events on AMarbleRaceHUDY2 (the royale HUD derives from
    // it, so one set of hooks covers both modes). Resolved once the HUD exists.

    // Read the first object parameter (the Marble) from a hooked call.
    __declspec(noinline) static UObject* FirstObjParam(UnrealScriptFunctionCallableContext& ctx) noexcept
    {
        UObject* marble = nullptr;
        __try
        {
            struct P { UObject* obj; };
            marble = ctx.GetParams<P>().obj;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { marble = nullptr; }
        return marble;
    }

    // Base address of a hooked call's parameter block, for handlers that need
    // more than the first pointer. All reads from it go through SafeReadBytes.
    __declspec(noinline) static void* ParamsBase(UnrealScriptFunctionCallableContext& ctx) noexcept
    {
        void* base = nullptr;
        __try
        {
            struct P { UObject* obj; };
            base = &ctx.GetParams<P>();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { base = nullptr; }
        return base;
    }

    // SEH-guarded handlers — reading a stale marble's fields must never crash.
    // The C++ work (which has objects needing unwinding) lives in *Impl; the SEH
    // wrapper has no destructible locals so __try is legal.
    // Record a marble's username→display pair as a side effect of seeing it in
    // any hook, so kill counts (keyed by killer display name) can be joined back
    // to standings names (usernames) at build time.
    static void NoteMarbleNames(UObject* marble, const std::wstring& username)
    {
        if (!marble || username.empty()) return;
        std::wstring disp;
        if (ReadStrProp(marble, STR("_Displayname"), disp))
            mik::RecordNamePair(username, disp);
    }

    static void HandleFinishImpl(UObject* marble)
    {
        std::wstring name = ResolveUsername(marble);
        NoteMarbleNames(marble, name);
        mik::RecordFinish(name);
    }
    __declspec(noinline) static void HandleFinish(UObject* marble) noexcept
    {
        __try { HandleFinishImpl(marble); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // OnMarbleEliminated(AMarble* Marble, FDamageInstigator DamageInstigator, FDamageEvent)
    // The killer's names live in the DamageInstigator param — its offset in the
    // param block is resolved from the UFunction's own param reflection at hook
    // registration (g_offKillerUser/Disp); within FDamageInstigator, Username is
    // at +0x00 and DisplayName at +0x10 (game SDK dump). The instigator is empty
    // for environmental deaths (fall/zone).
    static void HandleEliminationImpl(void* params)
    {
        if (!params) return;
        UObject* marble = nullptr;
        if (!SafeReadBytes(params, &marble, sizeof(marble))) return;
        std::wstring victim = ResolveUsername(marble);
        NoteMarbleNames(marble, victim);

        const char* base = static_cast<const char*>(params);
        const int32_t offU = mik::g_offKillerUser.load(), offD = mik::g_offKillerDisp.load();
        std::wstring killer, user, disp;
        if (offD >= 0 && SafeReadFString(base + offD, disp) && !disp.empty()) killer = disp;
        if (killer.empty() && offU >= 0 && SafeReadFString(base + offU, user) && !user.empty()) killer = user;

        // Log the first few eliminations of each match raw, so a killer-name
        // extraction failure is visible in UE4SS.log instead of silently
        // producing all-zero counts.
        {
            std::lock_guard<std::mutex> lk(mik::g_mtx);
            if (mik::g_elimDebugLeft > 0)
            {
                --mik::g_elimDebugLeft;
                Output::send<LogLevel::Default>(STR("[MikMarble] elim: victim={} killerUser='{}' killerDisp='{}' (off {}/{})\n"),
                    victim.empty() ? STR("(none)") : victim, user, disp, offU, offD);
            }
        }
        mik::RecordElimination(victim, killer);
    }
    __declspec(noinline) static void HandleElimination(void* params) noexcept
    {
        __try { HandleEliminationImpl(params); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Common resurrection handler. `offPS`/`offPawn` locate the respawned
    // player's PlayerState and the returned pawn in the hooked call's param
    // block (post-hook, so the return value is populated). Prefer the pawn's
    // _Username (same name source as the elimination records), fall back to
    // the PlayerState.
    static void HandleRespawnImpl(void* params, int32_t offPS, int32_t offPawn)
    {
        if (!params) return;
        const char* base = static_cast<const char*>(params);
        UObject* pawn = nullptr; UObject* ps = nullptr;
        if (offPawn >= 0) SafeReadBytes(base + offPawn, &pawn, sizeof(pawn));
        if (offPS   >= 0) SafeReadBytes(base + offPS,   &ps,   sizeof(ps));

        std::wstring name = ResolveUsername(pawn);
        if (!name.empty()) NoteMarbleNames(pawn, name);
        if (name.empty()) name = ResolveUsername(ps);
        if (name.empty()) return;

        mik::RecordRespawn(name);
        Output::send<LogLevel::Default>(STR("[MikMarble] respawn: {} (elimination erased)\n"), name);
    }
    // BP_RespawnPlayer(FPendingRoyalePlayerRespawn PlayerRespawn) -> APawn*
    __declspec(noinline) static void HandleRespawn(void* params) noexcept
    {
        __try { HandleRespawnImpl(params, mik::g_offRespawnPS.load(), mik::g_offRespawnPawn.load()); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // RespawnMarble(APlayerState* Player) -> AMarbleRoyaleMarble*
    // This is the function that actually performs a resurrection — verified by
    // forcing respawns in live 200-bot royales: RespawnMarble spawns the pawn
    // while BP_RespawnPlayer (the wave-respawn wrapper) never fires. Both are
    // hooked; RecordRespawn dedupes if a path ever triggers both.
    __declspec(noinline) static void HandleRespawn2(void* params) noexcept
    {
        __try { HandleRespawnImpl(params, mik::g_offRespawn2PS.load(), mik::g_offRespawn2Pawn.load()); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Resolve a UFunction by walking the class's full function chain and matching
    // on the materialized name. (GetFunctionByNameInChain's FName lookup does not
    // reliably find the inherited native HUD events.)
    static UFunction* FindFuncInChain(UClass* cls, const wchar_t* name)
    {
        if (!cls) return nullptr;
        for (UFunction* fn : TFieldRange<UFunction>(cls, EFieldIterationFlags::IncludeSuper))
        {
            if (!fn) continue;
            std::wstring n;
            try { n = fn->GetName(); } catch (...) { continue; }
            if (n == name) return fn;
        }
        return nullptr;
    }

    // Offset of a named parameter inside a UFunction's param block, read from
    // the function's own param reflection — no packing assumptions. -1 = absent.
    static int32_t FindParamOffset(UFunction* fn, const wchar_t* paramName)
    {
        if (!fn) return -1;
        for (FProperty* prop : TFieldRange<FProperty>(fn, EFieldIterationFlags::None))
        {
            if (!prop) continue;
            std::wstring n;
            try { n = prop->GetName(); } catch (...) { continue; }
            if (n == paramName)
            {
                try { return prop->GetOffset_Internal(); } catch (...) { return -1; }
            }
        }
        return -1;
    }

    // Call a `UObject* Fn(int32)` UFunction (e.g. GameMode.FindMarbleAtPosition)
    // via ProcessEvent. The params buffer is [int32 arg][ptr return]; ProcessEvent
    // writes the return pointer. SEH-guarded — a bad call must never crash.
    __declspec(noinline) static UObject* CallIntToObj(UObject* obj, UFunction* fn, int32_t arg) noexcept
    {
        if (!obj || !fn) return nullptr;
        struct Params { int32_t Arg; UObject* ReturnValue; } params{};
        params.Arg = arg;
        params.ReturnValue = nullptr;
        __try { obj->ProcessEvent(fn, &params); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        return params.ReturnValue;
    }

    // Call a no-arg `FVector Fn()` UFunction (Actor.K2_GetActorLocation /
    // SceneComponent.K2_GetComponentLocation) — returns world location.
    __declspec(noinline) static bool CallGetVector(UObject* obj, UFunction* fn, FVectorD& out) noexcept
    {
        if (!obj || !fn) return false;
        struct Params { FVectorD ReturnValue; } params{};
        __try { obj->ProcessEvent(fn, &params); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        out = params.ReturnValue;
        return true;
    }

    // Resolve + cache the world-location UFunction for an object's class, then call it.
    static bool GetWorldLocation(UObject* obj, const wchar_t* funcName, FVectorD& out)
    {
        if (!obj) return false;
        UClass* cls = SafeGetClassPrivate(obj);
        if (!cls) return false;
        UFunction* fn = FindFuncInChain(cls, funcName);
        return CallGetVector(obj, fn, out);
    }

    void TryRegisterHooks()
    {
        if (m_hooksRegistered) return;
        UObject* hud = UObjectGlobals::FindFirstOf(STR("MarbleRaceHUDY2"));
        if (!hud) hud = UObjectGlobals::FindFirstOf(STR("MarbleRoyaleHUDY2"));
        if (!hud) return;
        UClass* cls = SafeGetClassPrivate(hud);
        if (!cls) return;

        UFunction* fnFinish = FindFuncInChain(cls, L"OnMarbleFinishedRace");
        UFunction* fnElim   = FindFuncInChain(cls, L"OnMarbleEliminated");
        UFunction* fnEnd    = FindFuncInChain(cls, L"OnMatchEnded");

        if (!m_loggedResolve)
        {
            m_loggedResolve = true;
            std::wstring cn; try { cn = cls->GetName(); } catch (...) {}
            Output::send<LogLevel::Default>(STR("[MikMarble] HUD={} resolve finish={} elim={} end={}\n"),
                cn, fnFinish ? 1 : 0, fnElim ? 1 : 0, fnEnd ? 1 : 0);
        }
        if (!fnElim || !fnEnd) return;   // require at least elimination + end to be useful

        // Killer names: locate the DamageInstigator param in the elimination
        // event's param block; Username/DisplayName sit at +0x00/+0x10 inside it.
        {
            int32_t offInst = FindParamOffset(fnElim, L"DamageInstigator");
            if (offInst < 0) offInst = FindParamOffset(fnElim, L"DamagedDealer");
            if (offInst >= 0)
            {
                mik::g_offKillerUser.store(offInst + 0x00);
                mik::g_offKillerDisp.store(offInst + 0x10);
            }
            Output::send<LogLevel::Default>(STR("[MikMarble] elim param DamageInstigator offset={}\n"), offInst);
        }

        if (fnFinish)
            fnFinish->RegisterPostHook([](UnrealScriptFunctionCallableContext& ctx, void*) {
                HandleFinish(FirstObjParam(ctx));
            });
        fnElim->RegisterPostHook([](UnrealScriptFunctionCallableContext& ctx, void*) {
            HandleElimination(ParamsBase(ctx));
        });
        fnEnd->RegisterPostHook([](UnrealScriptFunctionCallableContext&, void*) {
            mik::g_endPending.store(true);
            Output::send<LogLevel::Default>(STR("[MikMarble] OnMatchEnded fired\n"));
        });

        m_hooksRegistered = true;
        Output::send<LogLevel::Default>(STR("[MikMarble] hooks registered (finish={})\n"), fnFinish ? 1 : 0);
    }

    // Royale resurrections. The respawn component is a native class, so its
    // UFunction is resolved by static path — no dependence on a component
    // instance existing yet (it may only be created during a royale, or lazily
    // at the first respawn, which an instance scan would register too late for).
    // Falls back to an instance scan and keeps retrying each tick until found.
    void TryRegisterRespawnHook()
    {
        if (m_respawnHookRegistered) return;

        UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, STR("/Script/MarblesOnStream.RoyaleGameRespawnComponent:BP_RespawnPlayer"));
        if (!fn)
        {
            if (UObject* comp = UObjectGlobals::FindFirstOf(STR("RoyaleGameRespawnComponent")))
                if (UClass* cls = SafeGetClassPrivate(comp))
                    fn = FindFuncInChain(cls, L"BP_RespawnPlayer");
        }
        if (!fn) return;   // retry next tick

        // PlayerState lives at +0x30 inside FPendingRoyalePlayerRespawn (SDK dump:
        // DeathLocation 0x00, SpawnLocation 0x18, Player 0x30); the return pawn
        // is the function's ReturnValue param.
        int32_t offRespawn = FindParamOffset(fn, L"PlayerRespawn");
        int32_t offRet     = FindParamOffset(fn, L"ReturnValue");
        mik::g_offRespawnPS.store(offRespawn >= 0 ? offRespawn + 0x30 : 0x30);
        mik::g_offRespawnPawn.store(offRet);

        fn->RegisterPostHook([](UnrealScriptFunctionCallableContext& ctx, void*) {
            HandleRespawn(ParamsBase(ctx));
        });

        // The function that actually resurrects (see HandleRespawn2): hook it
        // too — a powerup-triggered resurrection goes through here, NOT through
        // BP_RespawnPlayer (verified in live 200-bot royales).
        UFunction* fn2 = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, STR("/Script/MarblesOnStream.MarbleRoyaleGameMode:RespawnMarble"));
        int32_t off2PS = -1, off2Ret = -1;
        if (fn2)
        {
            off2PS  = FindParamOffset(fn2, L"Player");
            off2Ret = FindParamOffset(fn2, L"ReturnValue");
            mik::g_offRespawn2PS.store(off2PS >= 0 ? off2PS : 0);
            mik::g_offRespawn2Pawn.store(off2Ret);
            fn2->RegisterPostHook([](UnrealScriptFunctionCallableContext& ctx, void*) {
                HandleRespawn2(ParamsBase(ctx));
            });
        }

        m_respawnHookRegistered = true;
        Output::send<LogLevel::Default>(
            STR("[MikMarble] respawn hooks registered (BP_RespawnPlayer off {}/{}, RespawnMarble={} off {}/{})\n"),
            offRespawn, offRet, fn2 ? 1 : 0, off2PS, off2Ret);
    }

    // ── Build standings from the accumulated events ──────────────────────────

    std::vector<PlayerResult> BuildStandings(bool royale, UObject* gs)
    {
        // Marbles still alive at match end (e.g. the royale winner) were never
        // seen by a finish/elim hook, so their username→display pair is missing
        // from the kill-count join — sweep the live marbles to fill it in, and
        // remember who is still standing for the resurrection reconciliation.
        std::set<std::wstring> aliveSet;   // lowercase usernames of live marbles
        {
            std::vector<UObject*> marbles;
            UObjectGlobals::FindAllOf(STR("Marble"), marbles);
            for (UObject* m : marbles)
            {
                if (IsGhostMarble(m)) continue;
                std::wstring n = ResolveUsername(m);
                if (n.empty()) continue;
                NoteMarbleNames(m, n);
                aliveSet.insert(ToLowerW(n));
            }
        }

        std::vector<std::wstring> finishOrder, elimOrder;
        std::set<std::wstring>    elimSet;
        std::map<std::wstring, int> elimsBy;
        std::map<std::wstring, std::wstring> dispOf;
        {
            std::lock_guard<std::mutex> lk(mik::g_mtx);
            finishOrder = mik::g_finishOrder;
            elimOrder   = mik::g_elimOrder;
            elimSet     = mik::g_elimSet;
            elimsBy     = mik::g_elimsBy;
            dispOf      = mik::g_dispOf;
        }

        std::vector<PlayerResult> out;
        std::set<std::wstring> seen;
        // Kill counts are keyed by the killer's DISPLAY name (that's what the
        // DamageInstigator carries); standings names are usernames. Try the
        // direct (case-insensitive) match first — correct for real Twitch
        // players — then join through the username→display map (bots).
        auto killsOf = [&](const std::wstring& name) {
            std::wstring low = ToLowerW(name);
            auto it = elimsBy.find(low);
            if (it != elimsBy.end()) return it->second;
            auto d = dispOf.find(low);
            if (d != dispOf.end())
            {
                it = elimsBy.find(d->second);
                if (it != elimsBy.end()) return it->second;
            }
            return 0;
        };
        auto push = [&](const std::wstring& name, bool finished) {
            if (name.empty() || !seen.insert(name).second) return;
            out.push_back({name, finished, killsOf(name)});
        };

        if (royale)
        {
            // Placement = reverse elimination order (last eliminated is highest).
            // A respawned player's earlier elimination was erased by the respawn
            // hook, so elimSet/elimOrder reflect FINAL deaths only — a resurrected
            // marble that survives to the end is correctly absent here.
            // Winner: a marble that survived to match end (standings leader Top10[0]
            // not in the eliminated set) outranks everyone; otherwise — the usual
            // case, where even the winner is "eliminated" as the match concludes —
            // the last-eliminated marble is the winner. The very last entry is then
            // the first eliminated: the loser.
            std::wstring leader = ResolveUsername(ReadFirstArrayElem(gs, STR("Top10")));

            // Resurrection safety net (no hook dependency): if the standings
            // leader is in our eliminated set but their marble is demonstrably
            // ALIVE at match end, they died and were resurrected (powerup) by a
            // path our respawn hooks didn't see — the aliveness is the proof.
            // Un-eliminate them so they're crowned instead of being reported at
            // their stale pre-resurrection death slot.
            if (!leader.empty() && elimSet.count(leader) && aliveSet.count(ToLowerW(leader)))
            {
                Output::send<LogLevel::Default>(
                    STR("[MikMarble] leader {} was eliminated but is alive at match end (resurrected) — un-eliminating\n"),
                    leader);
                elimSet.erase(leader);
                elimOrder.erase(std::remove(elimOrder.begin(), elimOrder.end(), leader), elimOrder.end());
            }

            std::wstring winner;
            if (!leader.empty() && elimSet.find(leader) == elimSet.end()) winner = leader;
            else if (!elimOrder.empty())                                  winner = elimOrder.back();
            Output::send<LogLevel::Default>(STR("[MikMarble] royale leader(Top10[0])={} -> winner={}\n"),
                leader.empty() ? STR("(none)") : leader, winner.empty() ? STR("(none)") : winner);

            if (!winner.empty()) push(winner, true);
            for (auto it = elimOrder.rbegin(); it != elimOrder.rend(); ++it)
                if (*it != winner) push(*it, false);
        }
        else
        {
            // Race: finishers in finish order, then DNFs (eliminated/timed out),
            // last-eliminated first so the loser is the final entry.
            for (const auto& n : finishOrder) push(n, true);
            for (auto it = elimOrder.rbegin(); it != elimOrder.rend(); ++it)
                push(*it, false);
        }
        return out;
    }

    // ── Bullseye (target race) standings ─────────────────────────────────────
    //
    // Bullseye ranks marbles by how close they settle to the target centre, not by
    // finish position or time. The per-marble events never fire, Top10 is a time/
    // progress cache that doesn't even contain the winner, and the GameMode's
    // FindMarbleAtPosition() ranks by finish TIME (verified against the results
    // export) — none of which is the target score.
    //
    // The authoritative metric is geometric: distance from each settled marble to
    // the ABullseyeFinish.Bullseye centre component. We read world locations via
    // the engine's K2_GetActorLocation / K2_GetComponentLocation, then sort
    // ascending — closest first (winner), farthest last (loser). All finishers are
    // reported finished. If the centre or locations can't be read we fall back to
    // the (time-ordered) placement lookup so we still send something.
    std::vector<PlayerResult> BuildBullseyeStandings(UObject* gs)
    {
        std::vector<PlayerResult> out;

        // The target centre: GameState.BullseyeFinish -> Bullseye (USceneComponent).
        UObject* finishActor = ReadObjProp(gs, STR("BullseyeFinish"));
        UObject* centreComp  = finishActor ? ReadObjProp(finishActor, STR("Bullseye")) : nullptr;
        FVectorD centre{};
        bool haveCentre = centreComp && GetWorldLocation(centreComp, L"K2_GetComponentLocation", centre);

        // Build the full marble roster. FinishedMarbles only holds those that
        // settled on the target — a marble that shot past without settling (the
        // worst placement) is absent — so we also enumerate every marble via the
        // GameMode placement lookup. Order here is irrelevant; we re-rank below.
        std::vector<UObject*> roster;
        std::set<UObject*> rosterSet;
        auto addMarble = [&](UObject* m) {
            if (m && !IsGhostMarble(m) && rosterSet.insert(m).second) roster.push_back(m);
        };

        UObject* gm = UObjectGlobals::FindFirstOf(STR("MarbleRaceGameMode"));
        UFunction* fnPos = nullptr;
        if (gm) { if (UClass* c = SafeGetClassPrivate(gm)) fnPos = FindFuncInChain(c, L"FindMarbleAtPosition"); }
        int expected = ReadPlayerArrayNum(gs);
        int maxPos = (expected > 0 && expected < 1024) ? expected + 4 : 256;
        if (gm && fnPos)
            for (int pos = 1; pos <= maxPos; ++pos)   // scan ALL positions — large
            {                                          // fields have >=4-slot gaps
                UObject* m = CallIntToObj(gm, fnPos, pos);
                if (m) addMarble(m);
            }
        if (finishActor)
            for (UObject* m : ReadObjectArray(finishActor, STR("FinishedMarbles"))) addMarble(m);
        // Nobody despawns in bullseye, so every marble actor is still live at
        // match end — the sweep catches marbles the position lookup has no slot
        // for (e.g. never settled). Verified on 200-bot fields: the position
        // scan alone missed several marbles, including the true last place.
        {
            std::vector<UObject*> marbles;
            UObjectGlobals::FindAllOf(STR("Marble"), marbles);
            for (UObject* m : marbles) addMarble(m);
        }

        // Rank every marble by distance² to the centre — closest first (winner),
        // farthest last (loser, including any off-target marble).
        struct Ranked { std::wstring name; double dist2; };
        std::vector<Ranked> ranked;
        if (haveCentre)
            for (UObject* m : roster)
            {
                std::wstring name = ResolveUsername(m);
                if (name.empty()) continue;
                FVectorD loc{};
                if (!GetWorldLocation(m, L"K2_GetActorLocation", loc)) continue;
                double dx = loc.X - centre.X, dy = loc.Y - centre.Y, dz = loc.Z - centre.Z;
                ranked.push_back({name, dx * dx + dy * dy + dz * dz});
            }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const Ranked& a, const Ranked& b) { return a.dist2 < b.dist2; });

        std::set<std::wstring> seen;
        for (const auto& r : ranked)
            if (seen.insert(r.name).second) out.push_back({r.name, true});

        // Fallback: distances unavailable — send the roster in placement order so
        // we still report something rather than dropping the match.
        if (out.empty())
            for (UObject* m : roster) { std::wstring n = ResolveUsername(m); if (!n.empty() && seen.insert(n).second) out.push_back({n, true}); }

        // Marbles that flew off without settling get DESTROYED, so they have no
        // actor to rank — and the game places them at the very bottom (verified
        // on 200-bot fields: the missing names were exactly the last places).
        // Recover them from PlayerArray and append; skip spectators/inactive
        // PlayerStates (a non-racing host). Their order within this bottom block
        // is not recoverable (the game sorts them by an inaccessible leave time).
        if (!out.empty())
        {
            std::set<std::wstring> seenLow;
            for (const auto& p : out) seenLow.insert(ToLowerW(p.name));
            int appended = 0;
            for (UObject* ps : ReadObjectArray(gs, STR("PlayerArray")))
            {
                std::wstring n = ResolveUsername(ps);
                if (n.empty() || !seenLow.insert(ToLowerW(n)).second) continue;
                bool spec = false, onlySpec = false, inactive = false;
                ReadBoolProp(ps, STR("bIsSpectator"),   spec);
                ReadBoolProp(ps, STR("bOnlySpectator"), onlySpec);
                ReadBoolProp(ps, STR("bIsInactive"),    inactive);
                if (spec || onlySpec || inactive) continue;
                out.push_back({n, true});
                ++appended;
                Output::send<LogLevel::Default>(STR("[MikMarble] bullseye: appended off-map marble {} (no actor to rank)\n"), n);
            }
            if (appended > 0)
                Output::send<LogLevel::Default>(STR("[MikMarble] bullseye: {} off-map marbles appended at the bottom\n"), appended);
        }

        std::wstring first = out.empty() ? std::wstring(STR("(none)")) : out.front().name;
        std::wstring last  = out.empty() ? std::wstring(STR("(none)")) : out.back().name;
        Output::send<LogLevel::Default>(
            STR("[MikMarble] bullseye: centre={} roster={} ranked={} | winner={} loser={}\n"),
            haveCentre ? 1 : 0, (int)roster.size(), (int)ranked.size(), first, last);
        return out;
    }

    // ── HTTP ──────────────────────────────────────────────────────────────────

    void SendResults(const std::vector<PlayerResult>& players, const std::string& gameType)
    {
        bool expected = false;
        if (!m_state->httpBusy.compare_exchange_strong(expected, true)) return;  // one POST at a time
        m_state->inflight.fetch_add(1);

        std::string json = BuildResultsJson(players, gameType);
        auto playersCopy = players;
        { std::lock_guard<std::mutex> lock(m_state->mtx); m_state->lastGameType = gameType; }

        std::string host, path; int port = 0;
        { std::lock_guard<std::mutex> lock(m_configMutex); host = m_config.host; port = m_config.port; path = m_config.path; }
        std::string resultsPath = path + "/results";

        auto state = m_state;
        std::thread([state, json = std::move(json), playersCopy = std::move(playersCopy), host, port, resultsPath]() noexcept {
            try
            {
                HttpResult r = HttpPostJson(host, port, resultsPath, json);
                std::lock_guard<std::mutex> lock(state->mtx);
                state->lastSentPlayers = playersCopy;
                state->pendingLogs.push_back(std::move(r));
            }
            catch (...) {}
            state->httpBusy.store(false);
            state->inflight.fetch_sub(1);
        }).detach();
    }

    void MaybeSendReady()
    {
        if (!m_unrealReady.load()) return;
        bool expected = false;
        if (!m_readySent.compare_exchange_strong(expected, true)) return;

        std::string host, path; int port = 0;
        { std::lock_guard<std::mutex> lock(m_configMutex); host = m_config.host; port = m_config.port; path = m_config.path; }
        std::string readyPath = path + "/ready";

        auto state = m_state;
        state->inflight.fetch_add(1);
        std::thread([state, host, port, readyPath]() noexcept {
            try { HttpResult r = HttpPostJson(host, port, readyPath, "{}");
                  std::lock_guard<std::mutex> lock(state->mtx); state->pendingLogs.push_back(std::move(r)); }
            catch (...) {}
            state->inflight.fetch_sub(1);
        }).detach();
    }

    void DrainHttpLogs()
    {
        std::deque<HttpResult> drained;
        { std::lock_guard<std::mutex> lock(m_state->mtx); drained.swap(m_state->pendingLogs); }
        for (const auto& r : drained)
        {
            std::wstring wlabel(r.label.begin(), r.label.end());
            if (r.ok)
                Output::send<LogLevel::Default>(STR("[MikMarble] POST {} -> HTTP {}\n"), wlabel, r.statusCode);
            else
                Output::send<LogLevel::Warning>(STR("[MikMarble] POST {} FAILED (WinHTTP error {})\n"), wlabel, r.lastError);
        }
    }

    // ── Per-tick logic ──────────────────────────────────────────────────────

    void OnUpdateImpl()
    {
        static auto lastTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        m_pollTimer += std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        if (m_pollTimer < POLL_INTERVAL) return;
        m_pollTimer = 0.0f;

        DrainHttpLogs();
        MaybeSendReady();
        TryRegisterHooks();
        TryRegisterRespawnHook();

        // Detect match boundaries by GameState identity: a new level load = a new
        // GameState pointer = a fresh match. Reset the accumulator on every change.
        UObject* gs = FindGameState();
        if (gs != m_curGameState)
        {
            m_curGameState = gs;
            m_sentThisMatch = false;
            mik::ResetMatch();
        }

        // The OnMatchEnded hook flips g_endPending; build + POST exactly once.
        if (gs && !m_sentThisMatch && mik::g_endPending.load())
        {
            // Bullseye is read from the ranked leaderboard; race/royale are built
            // from the event accumulator exactly as before (untouched path).
            bool bullseye = IsBullseye();
            bool royale = IsRoyale();
            const wchar_t* modeName = bullseye ? STR("bullseye") : (royale ? STR("royale") : STR("race"));
            std::string wireType = bullseye ? "bullseye" : (royale ? "royale" : "race");
            auto players = bullseye ? BuildBullseyeStandings(gs) : BuildStandings(royale, gs);

            if (players.empty())
            {
                // Nothing accumulated (e.g. an unsupported mode whose events we
                // don't hook). Stay silent rather than POST an empty result; retry
                // is pointless this match, so latch it closed.
                m_sentThisMatch = true;
                mik::g_endPending.store(false);
                Output::send<LogLevel::Warning>(STR("[MikMarble] match ended but no events captured ({}); not sending\n"),
                                                modeName);
                return;
            }

            int expected = ReadPlayerArrayNum(gs);
            int finishedCount = 0; for (const auto& p : players) if (p.finished) ++finishedCount;
            Output::send<LogLevel::Default>(STR("[MikMarble] {} ended: {} players ({} finished, expected ~{})\n"),
                modeName, players.size(), finishedCount, expected);
            for (const auto& p : players)
                Output::send<LogLevel::Default>(STR("[MikMarble]   {} {} elims={}\n"),
                    p.name, p.finished ? STR("[fin]") : STR("[DNF]"), p.eliminations);

            SendResults(players, wireType);
            m_sentThisMatch = true;
            mik::g_endPending.store(false);
        }
    }

    __declspec(noinline) void SafeTick() noexcept
    {
        __try { OnUpdateImpl(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

public:
    MikMarbleMod() : CppUserModBase()
    {
        ModName        = STR("MikMarbleMod");
        ModVersion     = STR("3.20");
        ModDescription = STR("Mik marbles race result tracker (C++, event-driven)");
        ModAuthors     = STR("Draxi");

        m_config = LoadConfig();

        register_tab(STR("Mik Marble"), [](CppUserModBase* instance) {
            auto* self = static_cast<MikMarbleMod*>(instance);

            std::string host, path; int port = 0;
            { std::lock_guard<std::mutex> lock(self->m_configMutex); host = self->m_config.host; port = self->m_config.port; path = self->m_config.path; }

            ImGui::Text("MikMarbleMod v3.20 (C++, event-driven)");
            ImGui::Text("Endpoint: %s:%d%s", host.c_str(), port, path.c_str());
            ImGui::Text("Hooks: %s", self->m_hooksRegistered ? "registered" : "waiting for HUD...");
            ImGui::Text("Respawn hook: %s", self->m_respawnHookRegistered ? "registered" : "waiting for royale...");
            ImGui::Separator();

            std::vector<PlayerResult> snapshot;
            std::string lastGameType;
            {
                std::lock_guard<std::mutex> lock(self->m_state->mtx);
                snapshot     = self->m_state->lastSentPlayers;
                lastGameType = self->m_state->lastGameType;
            }
            ImGui::Text("HTTP: %s", self->m_state->httpBusy.load() ? "sending..." : "idle");
            if (!lastGameType.empty()) ImGui::Text("Last type: %s", lastGameType.c_str());
            ImGui::Text("Last sent: %d players", static_cast<int>(snapshot.size()));
            for (size_t i = 0; i < snapshot.size(); ++i)
            {
                const auto& p = snapshot[i];
                ImGui::Text("  %d. [%s] %s (elims: %d)",
                    static_cast<int>(i + 1), p.finished ? "fin" : "DNF",
                    WideToUtf8(p.name).c_str(), p.eliminations);
            }

            ImGui::Separator();
            if (ImGui::Button("Test Royale"))
                self->SendResults({{L"TestWinner", true}, {L"TestMiddle", false}, {L"TestLoser", false}}, "royale");
            ImGui::SameLine();
            if (ImGui::Button("Test Race"))
                self->SendResults({{L"First", true}, {L"Second", true}, {L"DnfLast", false}}, "race");
        });
    }

    ~MikMarbleMod() override
    {
        for (int i = 0; i < 100 && m_state->inflight.load() > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Output::send<LogLevel::Default>(STR("[MikMarble] Mod unloaded\n"));
    }

    auto on_ui_init() -> void override { UE4SS_ENABLE_IMGUI(); }

    auto on_unreal_init() -> void override
    {
        std::wstring wHost(m_config.host.begin(), m_config.host.end());
        std::wstring wPath(m_config.path.begin(), m_config.path.end());
        Output::send<LogLevel::Default>(STR("[MikMarble] Mod initialized (C++ v3.20) -> {}:{}{}\n"), wHost, m_config.port, wPath);
        m_unrealReady.store(true);
    }

    auto on_update() -> void override
    {
        try { SafeTick(); } catch (...) {}
    }
};

#define MIK_MOD_API __declspec(dllexport)
extern "C"
{
    MIK_MOD_API RC::CppUserModBase* start_mod() { return new MikMarbleMod(); }
    MIK_MOD_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
