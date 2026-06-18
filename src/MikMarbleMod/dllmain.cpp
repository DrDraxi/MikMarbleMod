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
//   • OnMarbleEliminated(Marble,…)  → elimination order (loser first) + per-eliminator counts
//   • OnMatchEnded()                → the one authoritative "results are final" trigger
// Each Marble exposes its player name (_Username) and PlayerState by reflection,
// so standings are reconstructed from authoritative, virtualization-proof events
// with zero guessed offsets and no dependence on the UI having rendered.
//
// Payload: {"type":"race"|"royale","players":[{"name","finished"}]} ordered
// finishers-first then DNFs (last entry = first eliminated = the loser).

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
#include <memory>
#include <deque>

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

// Read a string property (FString) by name.
static bool ReadStrProp(UObject* obj, const TCHAR* name, std::wstring& out)
{
    if (!obj) return false;
    void* fp = nullptr;
    try { fp = obj->GetValuePtrByPropertyNameInChain<FStringRaw>(name); } catch (...) { return false; }
    if (!fp) return false;
    return SafeReadFString(fp, out);
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
                R"(", "finished": )" + (players[i].finished ? "true" : "false") + "}";
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
namespace mik
{
    static std::mutex                  g_mtx;
    static std::vector<std::wstring>   g_finishOrder;   // race: finishers, in order
    static std::vector<std::wstring>   g_elimOrder;     // eliminated players, in order (first = loser)
    static std::set<std::wstring>      g_finishSet;
    static std::set<std::wstring>      g_elimSet;
    static std::atomic<bool>           g_endPending{false};

    static void ResetMatch()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_finishOrder.clear();
        g_elimOrder.clear();
        g_finishSet.clear();
        g_elimSet.clear();
        g_endPending.store(false);
    }

    static void RecordFinish(const std::wstring& name)
    {
        if (name.empty()) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_finishSet.insert(name).second) g_finishOrder.push_back(name);
    }

    static void RecordElimination(const std::wstring& victim)
    {
        if (victim.empty()) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_elimSet.insert(victim).second) g_elimOrder.push_back(victim);
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

    // SEH-guarded handlers — reading a stale marble's fields must never crash.
    // The C++ work (which has objects needing unwinding) lives in *Impl; the SEH
    // wrapper has no destructible locals so __try is legal.
    static void HandleFinishImpl(UObject* marble)
    {
        mik::RecordFinish(ResolveUsername(marble));
    }
    __declspec(noinline) static void HandleFinish(UObject* marble) noexcept
    {
        __try { HandleFinishImpl(marble); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void HandleEliminationImpl(UObject* marble)
    {
        mik::RecordElimination(ResolveUsername(marble));
    }
    __declspec(noinline) static void HandleElimination(UObject* marble) noexcept
    {
        __try { HandleEliminationImpl(marble); }
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

        if (fnFinish)
            fnFinish->RegisterPostHook([](UnrealScriptFunctionCallableContext& ctx, void*) {
                HandleFinish(FirstObjParam(ctx));
            });
        fnElim->RegisterPostHook([](UnrealScriptFunctionCallableContext& ctx, void*) {
            HandleElimination(FirstObjParam(ctx));
        });
        fnEnd->RegisterPostHook([](UnrealScriptFunctionCallableContext&, void*) {
            mik::g_endPending.store(true);
            Output::send<LogLevel::Default>(STR("[MikMarble] OnMatchEnded fired\n"));
        });

        m_hooksRegistered = true;
        Output::send<LogLevel::Default>(STR("[MikMarble] hooks registered (finish={})\n"), fnFinish ? 1 : 0);
    }

    // ── Build standings from the accumulated events ──────────────────────────

    std::vector<PlayerResult> BuildStandings(bool royale, UObject* gs)
    {
        std::vector<std::wstring> finishOrder, elimOrder;
        std::set<std::wstring>    elimSet;
        {
            std::lock_guard<std::mutex> lk(mik::g_mtx);
            finishOrder = mik::g_finishOrder;
            elimOrder   = mik::g_elimOrder;
            elimSet     = mik::g_elimSet;
        }

        std::vector<PlayerResult> out;
        std::set<std::wstring> seen;
        auto push = [&](const std::wstring& name, bool finished) {
            if (name.empty() || !seen.insert(name).second) return;
            out.push_back({name, finished});
        };

        if (royale)
        {
            // Placement = reverse elimination order (last eliminated is highest).
            // Winner: a marble that survived to match end (standings leader Top10[0]
            // not in the eliminated set) outranks everyone; otherwise — the usual
            // case, where even the winner is "eliminated" as the match concludes —
            // the last-eliminated marble is the winner. The very last entry is then
            // the first eliminated: the loser.
            std::wstring leader = ResolveUsername(ReadFirstArrayElem(gs, STR("Top10")));
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
            bool royale = IsRoyale();
            std::string wireType = royale ? "royale" : "race";
            auto players = BuildStandings(royale, gs);

            if (players.empty())
            {
                // Nothing accumulated (e.g. an unsupported mode whose events we
                // don't hook). Stay silent rather than POST an empty result; retry
                // is pointless this match, so latch it closed.
                m_sentThisMatch = true;
                mik::g_endPending.store(false);
                Output::send<LogLevel::Warning>(STR("[MikMarble] match ended but no events captured ({}); not sending\n"),
                                                royale ? STR("royale") : STR("race"));
                return;
            }

            int expected = ReadPlayerArrayNum(gs);
            int finishedCount = 0; for (const auto& p : players) if (p.finished) ++finishedCount;
            Output::send<LogLevel::Default>(STR("[MikMarble] {} ended: {} players ({} finished, expected ~{})\n"),
                royale ? STR("royale") : STR("race"), players.size(), finishedCount, expected);
            for (const auto& p : players)
                Output::send<LogLevel::Default>(STR("[MikMarble]   {} {}\n"),
                    p.name, p.finished ? STR("[fin]") : STR("[DNF]"));

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
        ModVersion     = STR("3.17");
        ModDescription = STR("Mik marbles race result tracker (C++, event-driven)");
        ModAuthors     = STR("Draxi");

        m_config = LoadConfig();

        register_tab(STR("Mik Marble"), [](CppUserModBase* instance) {
            auto* self = static_cast<MikMarbleMod*>(instance);

            std::string host, path; int port = 0;
            { std::lock_guard<std::mutex> lock(self->m_configMutex); host = self->m_config.host; port = self->m_config.port; path = self->m_config.path; }

            ImGui::Text("MikMarbleMod v3.17 (C++, event-driven)");
            ImGui::Text("Endpoint: %s:%d%s", host.c_str(), port, path.c_str());
            ImGui::Text("Hooks: %s", self->m_hooksRegistered ? "registered" : "waiting for HUD...");
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
                ImGui::Text("  %d. [%s] %s",
                    static_cast<int>(i + 1), p.finished ? "fin" : "DNF",
                    WideToUtf8(p.name).c_str());
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
        Output::send<LogLevel::Default>(STR("[MikMarble] Mod initialized (C++ v3.17) -> {}:{}{}\n"), wHost, m_config.port, wPath);
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
