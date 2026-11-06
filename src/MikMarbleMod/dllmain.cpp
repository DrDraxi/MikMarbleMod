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
//
// ── Live events (3.23+) ─────────────────────────────────────────────────────
// On top of the legacy ready/results pair, the mod can POST individual
// gameplay events to <target>/event:
//   map_entered, player_joined, marble_finished, marble_eliminated,
//   marble_damaged, countdown, grand_prix_finished.
// config.txt names targets (`target.<id>=<url>`; host/port/path is target
// "default") and routes each feature to target ids (`player_joined=a,b`,
// `all_events=a`). Every live event is off unless routed, so an untouched
// config behaves exactly like 3.22: ready + results to "default" only.
// Each target has its own sender thread + FIFO queue, so events arrive in the
// order they happened and a slow/dead endpoint never stalls the game or the
// other targets. Hook points were verified live (see CHANGELOG 3.23).

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>      // UClass, UFunction, RegisterPostHook
#include <Unreal/CoreUObject/UObject/UnrealType.hpp> // TFieldRange, EFieldIterationFlags
#include <Unreal/UFunctionStructs.hpp>               // UnrealScriptFunctionCallableContext
#include <Unreal/Hooks/Hooks.hpp>                    // ProcessEvent post-callback (BP events)
#include <Unreal/NameTypes.hpp>
#include <Unreal/FWeakObjectPtr.hpp>                 // weak GameMode reference
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
#include <condition_variable>
#include <sstream>
#include <cmath>

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

// Narrow copy of the mod version, for JSON payloads and ImGui (both of which
// want char, while ModVersion must be a wide STR() literal). Keep in sync with
// the ModVersion assignment in the constructor -- the log line derives from
// ModVersion, so those two are the only places a version string is written.
static constexpr const char* kModVersion = "3.23";

// Every live event the mod can emit. Each is OFF unless the config routes it
// to at least one target (`<event>=<ids>` or `all_events=<ids>`).
static const char* const kAllEvents[] = {
    "map_entered", "player_joined", "marble_finished", "marble_eliminated",
    "marble_damaged", "countdown", "grand_prix_finished", "marble_resurrected",
};

static constexpr const char* kLegacyTargetId = "default";

// One HTTP endpoint, after routing has been resolved. It receives
// {base}/ready (if it gets anything at all), {base}/results (if routed) and
// {base}/event for each routed event.
struct Target
{
    std::string id;             // "default" for the legacy host/port/path target
    std::string label;          // for logs / ImGui, e.g. "127.0.0.1:5000/mod"
    bool        https = false;
    std::string host;
    int         port = 0;
    std::string basePath;       // no trailing slash ("" = server root)
    bool        sendResults = false;
    std::set<std::string> events;   // routed live events
};

struct ModConfig
{
    std::string host = "127.0.0.1";
    int         port = 5000;
    std::string path = kDefaultPath;

    // target.<id>=<url>, in file order.
    std::vector<std::pair<std::string, std::string>> namedTargets;

    // Feature routing: feature -> target ids. "results" defaults to the legacy
    // target; live events default to nowhere (= off). Keys are only present
    // here if the config mentions them.
    std::map<std::string, std::vector<std::string>> routes;
    std::vector<std::string> allEventsRoute;     // all_events=<ids>

    // Optional: display names to omit from reported results, stored lower-cased.
    // Empty (the default) keeps every player, so behaviour is unchanged unless
    // the user opts in. Deliberately NOT applied to royale -- see SendResults --
    // and not to live events (those report what happened on screen).
    std::vector<std::string> ignoredNames;

    bool traceFunctions = false;         // trace_functions=1: discovery logging (see TraceFunction)

    std::vector<std::string> warnings;   // parse problems, echoed at startup
};

// ASCII-only lower-casing, used for case-insensitive name matching.
// std::tolower() takes an int and is UB for negative values, which is exactly
// what a plain `char` gives you for any non-ASCII UTF-8 byte. Restricting to
// A-Z sidesteps that entirely and leaves every other byte untouched, so
// non-ASCII names still compare correctly (just case-sensitively).
static std::string AsciiLower(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

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

static std::vector<std::string> SplitList(const std::string& val)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= val.size())
    {
        size_t comma = val.find(',', pos);
        size_t len   = (comma == std::string::npos) ? std::string::npos : comma - pos;
        std::string item = Trim(val.substr(pos, len));
        if (!item.empty()) out.push_back(item);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

static bool IsKnownEvent(const std::string& e)
{
    for (const char* k : kAllEvents) if (e == k) return true;
    return false;
}

// A route value: comma list of target ids. "none" / "off" / empty = nowhere.
static std::vector<std::string> ParseRoute(const std::string& val)
{
    std::string low = AsciiLower(Trim(val));
    if (low.empty() || low == "none" || low == "off") return {};
    std::vector<std::string> ids;
    for (const auto& id : SplitList(val)) ids.push_back(AsciiLower(id));
    return ids;
}

static std::string NormalizeBasePath(std::string p)
{
    p = Trim(p);
    while (!p.empty() && p.back() == '/') p.pop_back();
    if (!p.empty() && p.front() != '/') p = "/" + p;
    return p;
}

// <url> = [http://|https://]host[:port][/base/path]
static bool ParseTargetUrl(const std::string& urlIn, Target& t, std::vector<std::string>& warnings)
{
    std::string url = Trim(urlIn);
    if (url.empty()) { warnings.push_back("target." + t.id + " has no URL"); return false; }

    std::string rest = url;
    std::string lower = AsciiLower(url);
    if (lower.rfind("https://", 0) == 0) { t.https = true; rest = url.substr(8); }
    else if (lower.rfind("http://", 0) == 0) { rest = url.substr(7); }

    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    t.basePath = NormalizeBasePath(slash == std::string::npos ? "" : rest.substr(slash));
    size_t colon = hostport.rfind(':');
    t.port = t.https ? 443 : 80;
    if (colon != std::string::npos)
    {
        try { t.port = std::stoi(hostport.substr(colon + 1)); }
        catch (...) { warnings.push_back("bad port in target." + t.id + " '" + url + "'"); return false; }
        hostport = hostport.substr(0, colon);
    }
    t.host = hostport;
    if (t.host.empty()) { warnings.push_back("no host in target." + t.id + " '" + url + "'"); return false; }
    t.label = std::string(t.https ? "https://" : "") + t.host + ":" + std::to_string(t.port) + t.basePath;
    return true;
}

// Resolve the config into the list of targets that actually get traffic.
// The legacy host/port/path keys define target "default" (unless host is
// blank). Targets no feature routes to are dropped with a note in the log.
static std::vector<Target> BuildTargets(const ModConfig& cfg, std::vector<std::string>& warnings)
{
    std::vector<Target> all;
    if (!Trim(cfg.host).empty())
    {
        Target t;
        t.id = kLegacyTargetId;
        t.host = Trim(cfg.host);
        t.port = cfg.port;
        t.basePath = NormalizeBasePath(cfg.path);
        t.label = t.host + ":" + std::to_string(t.port) + t.basePath;
        all.push_back(std::move(t));
    }
    for (const auto& [id, url] : cfg.namedTargets)
    {
        bool dup = std::any_of(all.begin(), all.end(), [&](const Target& x) { return x.id == id; });
        if (dup) { warnings.push_back("target." + id + " is defined twice; keeping the first"); continue; }
        Target t;
        t.id = id;
        if (ParseTargetUrl(url, t, warnings)) all.push_back(std::move(t));
    }

    auto findTarget = [&](const std::string& id) -> Target* {
        for (auto& t : all) if (t.id == id) return &t;
        return nullptr;
    };
    auto route = [&](const std::string& feature, const std::vector<std::string>& ids) {
        for (const auto& id : ids)
        {
            Target* t = findTarget(id);
            if (!t) { warnings.push_back(feature + ": no target with id '" + id + "'"); continue; }
            if (feature == "results") t->sendResults = true;
            else t->events.insert(feature);
        }
    };

    // results: legacy behaviour unless the config says otherwise.
    auto rit = cfg.routes.find("results");
    if (rit != cfg.routes.end()) route("results", rit->second);
    else if (Target* d = findTarget(kLegacyTargetId)) d->sendResults = true;

    for (const char* e : kAllEvents)
    {
        auto it = cfg.routes.find(e);
        if (it != cfg.routes.end()) route(e, it->second);
        route(e, cfg.allEventsRoute);
    }

    std::vector<Target> used;
    for (auto& t : all)
    {
        if (t.sendResults || !t.events.empty()) used.push_back(std::move(t));
        else warnings.push_back("target '" + t.id + "' (" + t.label + ") has no features routed to it; not used");
    }
    return used;
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
            out << "# Target \"default\": where results go (same as every version before 3.23).\n";
            out << "host=127.0.0.1\n";
            out << "port=5000\n";
            out << "path=" << kDefaultPath << "\n";
            out << "\n";
            out << "# ── Optional: more targets, each with an id ────────────────────────\n";
            out << "# Each target gets POSTs at <url>/ready, <url>/results, <url>/event.\n";
            out << "# target.overlay=http://192.168.1.20:8080/marbles\n";
            out << "# target.bot=https://example.com/hooks\n";
            out << "\n";
            out << "# ── Features: which target ids get what ────────────────────────────\n";
            out << "# Comma-separated ids. Live events are OFF unless listed here.\n";
            out << "# results=default\n";
            out << "# map_entered=overlay\n";
            out << "# player_joined=overlay,bot\n";
            out << "# marble_finished=overlay\n";
            out << "# marble_eliminated=overlay\n";
            out << "# marble_damaged=overlay\n";
            out << "# countdown=overlay\n";
            out << "# grand_prix_finished=bot\n";
            out << "# marble_resurrected=overlay\n";
            out << "# all_events=bot        (shortcut: every live event to these ids)\n";
            out << "\n";
            out << "# Optional: players to leave out of reported results.\n";
            out << "# Comma-separated display names, matched exactly but\n";
            out << "# case-insensitively. The key may be repeated to add more.\n";
            out << "# Applies to race, death race and bullseye -- NOT to royale,\n";
            out << "# where placement is derived from elimination order.\n";
            out << "# ignore=SomeName,AnotherName\n";
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
            std::string lkey = AsciiLower(key);
            if (key == "host") cfg.host = val;
            else if (key == "port") { try { cfg.port = std::stoi(val); } catch (...) {} }
            else if (key == "path") cfg.path = val;
            else if (lkey == "trace_functions") cfg.traceFunctions = (val == "1" || AsciiLower(val) == "on" || AsciiLower(val) == "true");
            else if (lkey.rfind("target.", 0) == 0)
            {
                std::string id = AsciiLower(Trim(key.substr(7)));
                if (id.empty()) cfg.warnings.push_back("'" + key + "' needs an id, e.g. target.overlay=...");
                else cfg.namedTargets.emplace_back(id, val);
            }
            else if (lkey == "results" || IsKnownEvent(lkey))
            {
                // Repeating a feature key appends ids (like `ignore`).
                auto& ids = cfg.routes[lkey];
                for (auto& id : ParseRoute(val)) ids.push_back(id);
            }
            else if (lkey == "all_events")
            {
                for (auto& id : ParseRoute(val)) cfg.allEventsRoute.push_back(id);
            }
            else if (key == "ignore" || key == "ignored" || key == "ignore_names")
            {
                // Comma-separated. Repeating the key appends rather than
                // replaces, so a long list can be split across lines.
                size_t pos = 0;
                while (pos <= val.size())
                {
                    size_t comma = val.find(',', pos);
                    size_t len   = (comma == std::string::npos) ? std::string::npos : comma - pos;
                    std::string name = Trim(val.substr(pos, len));
                    if (!name.empty()) cfg.ignoredNames.push_back(AsciiLower(name));
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
            }
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

static int MikSehFilter(unsigned long code) noexcept
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case STATUS_GUARD_PAGE_VIOLATION:
    case 0xE06D7363:                       // MSVC C++ exception
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        return EXCEPTION_CONTINUE_SEARCH;  // e.g. stack overflow: don't hide it
    }
}

__declspec(noinline) static bool SafeReadBytes(const void* src, void* dst, size_t bytes) noexcept
{
    __try { memcpy(dst, src, bytes); return true; }
    __except (MikSehFilter(GetExceptionCode())) { return false; }
}

__declspec(noinline) static UClass* SafeGetClassPrivate(UObject* obj) noexcept
{
    UClass* cls = nullptr;
    __try { cls = obj->GetClassPrivate(); }
    __except (MikSehFilter(GetExceptionCode())) { cls = nullptr; }
    return cls;
}

struct FStringRaw { wchar_t* Data; int32_t Num; int32_t Max; };  // Num includes the null terminator
struct TArrayRaw  { void* Data; int32_t Num; int32_t Max; };
struct FVectorD   { double X, Y, Z; };  // UE5 FVector (double precision, size 0x18)

// Reads + validates an FString at `src`. Accepts 1..maxChars-1 chars (default
// 127, enough for any name); allows non-ASCII (Unicode stream names) but
// rejects control chars and obviously-bad memory.
static bool SafeReadFString(const void* src, std::wstring& out, int32_t maxChars = 128)
{
    FStringRaw fs{};
    if (!SafeReadBytes(src, &fs, sizeof(fs))) return false;
    if (!fs.Data || fs.Num <= 1 || fs.Num > maxChars || fs.Max < fs.Num) return false;

    wchar_t buf[1024]{};
    if (fs.Num > static_cast<int32_t>(std::size(buf))) return false;
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

// One POST over an already-open connection handle. Opening the session and
// connection once per target (see TargetSender) lets WinHTTP keep the TCP
// connection alive across requests — important for high-rate events like
// marble_damaged, where a fresh connection per hit would be wasteful.
static HttpResult HttpPostOn(HINTERNET hConnect, bool https,
                             const std::string& path, const std::string& jsonBody) noexcept
{
    HttpResult r;
    r.label = path;
    HINTERNET hRequest = nullptr;
    try
    {
        {
            std::wstring wPath(path.begin(), path.end());
            hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
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
                // Drain the body so the connection can be reused (keep-alive).
                DWORD avail = 0;
                char sink[512];
                while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0)
                {
                    DWORD got = 0;
                    if (!WinHttpReadData(hRequest, sink, avail < sizeof(sink) ? avail : sizeof(sink), &got) || got == 0) break;
                }
            }
            else { r.lastError = GetLastError(); }
        }
    }
    catch (...) { /* swallow */ }
done:
    if (hRequest) WinHttpCloseHandle(hRequest);
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
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            }
            else out += c;
            break;
        }
    }
    return out;
}

// Quoted JSON string from a wide (UTF-16) game string.
static std::string JStr(const std::wstring& w) { return "\"" + EscapeJsonString(WideToUtf8(w)) + "\""; }
static std::string JStr(const std::string& s)  { return "\"" + EscapeJsonString(s) + "\""; }
static std::string JNum(double d)
{
    if (!std::isfinite(d)) return "null";
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", d);
    return buf;
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
    std::map<std::string, int> eventCounts;    // events emitted this session, by name (ImGui)
    std::deque<std::string>   recentEvents;    // last few event bodies (ImGui)
};

// ── Per-target sender ───────────────────────────────────────────────────────
//
// One background thread per target drains a FIFO queue, so events reach each
// endpoint in the order they happened, a slow or dead endpoint only backs up
// its own queue, and the game thread never blocks on the network. Ready and
// results go through the same queue, so `results` always lands after the last
// marble_finished/marble_eliminated of that match.

struct OutJob
{
    std::string path;
    std::string body;
    bool        lowPriority = false;   // marble_damaged / countdown: first to drop when full
    bool        logSuccess  = false;   // ready/results: log every outcome like 3.22 did
};

struct SenderState
{
    Target                  target;
    std::mutex              m;
    std::condition_variable cv;
    std::deque<OutJob>      q;
    bool                    stop = false;
    std::atomic<bool>       exited{false};
    std::atomic<uint64_t>   sent{0}, failed{0}, dropped{0};
    std::atomic<int>        lastStatus{0};
    std::atomic<DWORD>      lastError{0};
    std::chrono::steady_clock::time_point lastFailLog{};
    std::shared_ptr<HttpSharedState> shared;

    static constexpr size_t kMaxQueue = 4000;

    void Enqueue(OutJob job)
    {
        {
            std::lock_guard<std::mutex> lk(m);
            if (q.size() >= kMaxQueue)
            {
                // Prefer dropping a stale low-priority event over anything
                // that matters (joins, deaths, results).
                auto it = std::find_if(q.begin(), q.end(), [](const OutJob& j) { return j.lowPriority; });
                if (it != q.end()) q.erase(it); else q.pop_front();
                dropped.fetch_add(1);
            }
            q.push_back(std::move(job));
        }
        cv.notify_one();
    }

    size_t QueueSize() { std::lock_guard<std::mutex> lk(m); return q.size(); }
};

static void SenderLoop(std::shared_ptr<SenderState> s) noexcept
{
    HINTERNET hSession = nullptr, hConnect = nullptr;
    auto closeAll = [&]() {
        if (hConnect) { WinHttpCloseHandle(hConnect); hConnect = nullptr; }
        if (hSession) { WinHttpCloseHandle(hSession); hSession = nullptr; }
    };
    try
    {
        std::wstring wHost(s->target.host.begin(), s->target.host.end());
        for (;;)
        {
            OutJob job;
            {
                std::unique_lock<std::mutex> lk(s->m);
                s->cv.wait(lk, [&] { return s->stop || !s->q.empty(); });
                // On stop, drop whatever is still queued: draining up to 4000
                // events into a dead endpoint (5 s timeouts each) would keep
                // this thread alive long after the mod is gone.
                if (s->stop) { s->q.clear(); break; }
                job = std::move(s->q.front());
                s->q.pop_front();
            }

            if (!hSession)
            {
                hSession = WinHttpOpen(L"MikMarbleMod/3", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
                if (hSession) WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);
            }
            if (hSession && !hConnect)
                hConnect = WinHttpConnect(hSession, wHost.c_str(), static_cast<INTERNET_PORT>(s->target.port), 0);

            HttpResult r;
            r.label = s->target.label + " " + job.path;
            if (!hConnect) { r.lastError = GetLastError(); }
            else
            {
                r = HttpPostOn(hConnect, s->target.https, job.path, job.body);
                r.label = (s->target.https ? "https://" : "") + s->target.host + ":" +
                          std::to_string(s->target.port) + job.path;
            }

            if (r.ok) { s->sent.fetch_add(1); s->lastStatus.store(r.statusCode); }
            else
            {
                s->failed.fetch_add(1);
                s->lastError.store(r.lastError);
                closeAll();   // reconnect on the next job
            }

            // Logging: ready/results always (as before); events only when they
            // fail or get a non-2xx, at most once per 5 s per target so a dead
            // endpoint can't flood UE4SS.log with one line per damage hit.
            bool bad = !r.ok || r.statusCode < 200 || r.statusCode >= 300;
            bool log = job.logSuccess;
            if (!log && bad)
            {
                auto now = std::chrono::steady_clock::now();
                if (now - s->lastFailLog > std::chrono::seconds(5)) { s->lastFailLog = now; log = true; }
            }
            if (log && s->shared)
            {
                std::lock_guard<std::mutex> lk(s->shared->mtx);
                s->shared->pendingLogs.push_back(std::move(r));
            }
        }
    }
    catch (...) {}
    closeAll();
    s->exited.store(true);
}

// The live set of senders, shared by the mod instance and the static hook
// handlers (which have no `this`). Replaced wholesale only at startup.
namespace mik_bus
{
    static std::mutex                                g_mtx;
    static std::vector<std::shared_ptr<SenderState>> g_senders;
    static std::shared_ptr<HttpSharedState>          g_shared;

    static std::vector<std::shared_ptr<SenderState>> Snapshot()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        return g_senders;
    }

    static std::shared_ptr<HttpSharedState> Shared()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        return g_shared;
    }

    static uint64_t NowUnixMs()
    {
        using namespace std::chrono;
        return static_cast<uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }
}

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

    // ── Live-event state (all param offsets resolved from reflection) ──
    static std::atomic<int32_t> g_offFinishWinner{-1};      // OnMarbleFinishedRace.bRaceWinner
    static std::atomic<int32_t> g_offDmgActor{-1}, g_offDmgAmount{-1}, g_offDmgCauser{-1};
    static std::atomic<int32_t> g_offChatViewer{-1}, g_offChatMsg{-1};          // OnChatMessageReceived
    static std::atomic<int32_t> g_offViewerUser{-1}, g_offViewerDisp{-1};       // inside FMOSViewer
    static std::atomic<int32_t> g_offMsgText{-1};                               // inside FMOSViewerChatMessage
    static std::atomic<int32_t> g_offJoinedPS{-1};                              // OnPlayerJoinedGame.Player

    // Match context stamped on every event. Written on the game thread when a
    // map is entered; read under g_mtx by Emit.
    static std::string g_ctxMatchId, g_ctxMap, g_ctxMode;

    // Chat join commands seen recently: lower(username) and lower(display)
    // -> {command text, when}. Joined to the marble/PlayerState that spawns.
    struct PendingChat { std::wstring user, disp, command; std::chrono::steady_clock::time_point at; };
    static std::map<std::wstring, PendingChat> g_pendingChat;
    static std::set<std::wstring>              g_joinedLow;     // per match: names already reported
    static std::map<std::string, int>          g_lastCountdown; // phase -> last whole second sent
    static bool                                g_gpSent = false;
    // The pawn each eliminated player died as, and when — so a later hit on a
    // DIFFERENT pawn with the same name proves a resurrection (see
    // IsLateResurrection). Per match.
    struct DeathInfo { UObject* pawn; std::chrono::steady_clock::time_point at; };
    static std::map<std::wstring, DeathInfo> g_deathOf;
    // lower(name) -> when a marble_resurrected was last emitted, so the hook
    // and the damage-based detector can't both report the same resurrection.
    static std::map<std::wstring, std::chrono::steady_clock::time_point> g_resurrectedAt;
    static int                                 g_dmgDebugLeft = 0;
    static std::atomic<bool>                   g_trace{false};      // config: trace_functions=1
    static std::atomic<int>                    g_matchSerial{0};    // bumped by ResetMatch
    static std::atomic<bool>                   g_matchStarted{false}; // HUD OnMatchStarted fired (lobby closed)

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
        g_dmgDebugLeft = 3;
        g_endPending.store(false);
        g_joinedLow.clear();
        g_lastCountdown.clear();
        g_gpSent = false;
        g_deathOf.clear();
        g_resurrectedAt.clear();
        g_ctxMatchId.clear(); g_ctxMap.clear(); g_ctxMode.clear();
        g_matchSerial.fetch_add(1);
        g_matchStarted.store(false);
        // Pending chat commands survive the reset on purpose: viewers often
        // type !play during the map load, before the new GameState exists.
    }

    // Drop chat commands older than 10 minutes (EmitJoin ignores those anyway).
    // `force` (map at its cap) also clears everything if pruning freed nothing.
    // Caller holds g_mtx.
    static void PruneChatLocked(bool force)
    {
        auto cutoff = std::chrono::steady_clock::now() - std::chrono::minutes(10);
        for (auto it = g_pendingChat.begin(); it != g_pendingChat.end();)
            it = it->second.at < cutoff ? g_pendingChat.erase(it) : std::next(it);
        if (force && g_pendingChat.size() >= 4000) g_pendingChat.clear();
    }

    static void PruneChat()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        PruneChatLocked(false);
    }

    static void SetContext(const std::string& matchId, const std::string& map, const std::string& mode)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_ctxMatchId = matchId; g_ctxMap = map; g_ctxMode = mode;
    }

    static bool HasMatchId()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        return !g_ctxMatchId.empty();
    }

    static void SetMatchId(const std::string& matchId)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_ctxMatchId = matchId;
    }

    static bool TargetWants(const Target& t, const char* name)
    {
        return t.events.count(name) > 0;
    }

    // Build {"event": name, "ts": ..., "match_id": ..., "map": ..., "mode": ..., <fields>}
    // and queue it on every target whose filter accepts it. `fields` is a
    // pre-rendered JSON fragment ("\"name\": \"x\", ...") or empty.
    static void Emit(const char* name, const std::string& fields, bool lowPriority = false)
    {
        std::string matchId, map, mode;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            matchId = g_ctxMatchId; map = g_ctxMap; mode = g_ctxMode;
        }
        std::string body = std::string("{\"event\": ") + JStr(std::string(name)) +
                           ", \"ts\": " + std::to_string(mik_bus::NowUnixMs()) +
                           ", \"match_id\": " + (matchId.empty() ? "null" : JStr(matchId)) +
                           ", \"map\": " + (map.empty() ? "null" : JStr(map)) +
                           ", \"mode\": " + (mode.empty() ? "null" : JStr(mode)) +
                           (fields.empty() ? "" : ", " + fields) + "}";

        for (auto& s : mik_bus::Snapshot())
        {
            if (!TargetWants(s->target, name)) continue;
            OutJob job;
            job.path = s->target.basePath + "/event";
            job.body = body;
            job.lowPriority = lowPriority;
            s->Enqueue(std::move(job));
        }
        if (auto sh = mik_bus::Shared())
        {
            std::lock_guard<std::mutex> lk(sh->mtx);
            ++sh->eventCounts[name];
            if (!lowPriority || sh->recentEvents.empty() || sh->recentEvents.back().find(name) == std::string::npos)
            {
                sh->recentEvents.push_back(body.size() > 240 ? body.substr(0, 240) + "..." : body);
                while (sh->recentEvents.size() > 8) sh->recentEvents.pop_front();
            }
        }
    }

    // Remember a marble's username→displayname pair (for the kill-count join).
    static void RecordNamePair(const std::wstring& username, const std::wstring& display)
    {
        if (username.empty() || display.empty()) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        g_dispOf[ToLowerW(username)] = ToLowerW(display);
    }

    // Returns the 1-based finish position, or 0 if this marble was already recorded.
    static int RecordFinish(const std::wstring& name)
    {
        if (name.empty()) return 0;
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_finishSet.insert(name).second) return 0;
        g_finishOrder.push_back(name);
        return static_cast<int>(g_finishOrder.size());
    }

    // Returns how many marbles are currently counted as eliminated (this one
    // included), or 0 if it was a duplicate event for the same marble.
    static int RecordElimination(const std::wstring& victim, const std::wstring& killer, UObject* pawn = nullptr)
    {
        if (victim.empty()) return 0;
        std::lock_guard<std::mutex> lk(g_mtx);
        bool fresh = g_elimSet.insert(victim).second;
        if (fresh) g_elimOrder.push_back(victim);
        if (!killer.empty()) ++g_elimsBy[ToLowerW(killer)];
        if (fresh) g_deathOf[victim] = {pawn, std::chrono::steady_clock::now()};
        return fresh ? static_cast<int>(g_elimOrder.size()) : 0;
    }

    // Hook-free resurrection evidence: `name` is recorded as eliminated, yet a
    // DIFFERENT pawn carrying that name is taking damage at least 2 s after the
    // death. (Same pawn, or within 2 s, could be a hit landing on the dying
    // marble in the same frame — not proof.)
    static bool IsLateResurrection(const std::wstring& name, UObject* pawn)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_elimSet.count(name)) return false;
        auto it = g_deathOf.find(name);
        if (it == g_deathOf.end()) return false;
        if (it->second.pawn == pawn) return false;
        return std::chrono::steady_clock::now() - it->second.at >= std::chrono::seconds(2);
    }

    // True if this resurrection hasn't been reported in the last 10 s.
    static bool ClaimResurrectionReport(const std::wstring& name)
    {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(g_mtx);
        std::wstring low = ToLowerW(name);
        auto it = g_resurrectedAt.find(low);
        if (it != g_resurrectedAt.end() && now - it->second < std::chrono::seconds(10)) return false;
        g_resurrectedAt[low] = now;
        return true;
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
    // Hook registration flags are `inline static`: hooks are process-global and
    // the DLL is pinned (see PinModule), so if UE4SS ever re-creates the mod
    // instance (hot reload) the hooks from the first instance are still live
    // and must not be registered twice.
    inline static bool m_hooksRegistered = false;       // UFunction hooks are session-global, register once
    inline static bool m_respawnHookRegistered = false; // royale respawn hook (component is royale-only)
    bool     m_loggedResolve   = false;    // log the HUD/function resolution once
    bool     m_sentThisMatch   = false;

    float m_pollTimer = 0.0f;
    int   m_pruneTick = 0;
    bool  m_grandPrixActive = false;   // last map entered was a Grand Prix race / podium
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
        __except (MikSehFilter(GetExceptionCode())) { marble = nullptr; }
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
        __except (MikSehFilter(GetExceptionCode())) { base = nullptr; }
        return base;
    }

    // The hooked call's return slot (valid in post-hooks; may be null).
    __declspec(noinline) static void* ResultSlot(UnrealScriptFunctionCallableContext& ctx) noexcept
    {
        void* r = nullptr;
        __try { r = ctx.RESULT_DECL; }
        __except (MikSehFilter(GetExceptionCode())) { r = nullptr; }
        return r;
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

    static std::wstring DisplayNameOf(UObject* obj)
    {
        std::wstring d;
        if (!obj) return d;
        if (ReadStrProp(obj, STR("_Displayname"), d) && !d.empty()) return d;
        if (ReadStrProp(obj, STR("DisplayName"), d) && !d.empty()) return d;
        if (UObject* ps = ReadObjProp(obj, STR("PlayerState")))
            if (ReadStrProp(ps, STR("DisplayName"), d) && !d.empty()) return d;
        return d;
    }

    static std::string PlayerFields(const std::wstring& name, const std::wstring& disp)
    {
        return "\"name\": " + JStr(name) + ", \"display_name\": " + (disp.empty() ? JStr(name) : JStr(disp));
    }

    static void HandleFinishImpl(void* params)
    {
        if (!params) return;
        UObject* marble = nullptr;
        if (!SafeReadBytes(params, &marble, sizeof(marble))) return;
        std::wstring name = ResolveUsername(marble);
        NoteMarbleNames(marble, name);
        int position = mik::RecordFinish(name);
        if (position <= 0) return;

        bool winner = position == 1;
        int32_t offW = mik::g_offFinishWinner.load();
        if (offW >= 0)
        {
            uint8_t b = 0;
            if (SafeReadBytes(static_cast<const char*>(params) + offW, &b, 1)) winner = b != 0;
        }
        mik::Emit("marble_finished", PlayerFields(name, DisplayNameOf(marble)) +
                  ", \"position\": " + std::to_string(position) +
                  ", \"winner\": " + (winner ? "true" : "false"));
    }
    __declspec(noinline) static void HandleFinish(void* params) noexcept
    {
        __try { HandleFinishImpl(params); }
        __except (MikSehFilter(GetExceptionCode())) {}
    }

    // ── marble_damaged ───────────────────────────────────────────────────────
    // AMarbleRaceGameMode::OnMarbleTookAnyDamage(DamagedActor, Damage, DamageType,
    // InstigatedBy, DamageCauser) — bound to every marble's OnTakeAnyDamage, so
    // it fires once per hit (verified live in royale). Health is read off the
    // marble after the call (this is a post-hook).
    static bool ReadIntPropByName(UObject* obj, const TCHAR* name, int32_t& out)
    {
        if (!obj) return false;
        void* p = nullptr;
        try { p = obj->GetValuePtrByPropertyNameInChain<int32_t>(name); } catch (...) { return false; }
        return p && SafeReadBytes(p, &out, sizeof(out));
    }

    static std::wstring AttackerName(UObject* causer)
    {
        if (!causer) return {};
        std::wstring n = DisplayNameOf(causer);
        if (n.empty()) n = ResolveUsername(causer);
        if (n.empty())   // projectile (laser) -> OwningPlayer (AMOSPlayerState)
            if (UObject* owner = ReadObjProp(causer, STR("OwningPlayer"))) { n = DisplayNameOf(owner); if (n.empty()) n = ResolveUsername(owner); }
        if (n.empty())
            if (UObject* inst = ReadObjProp(causer, STR("Instigator"))) { n = DisplayNameOf(inst); if (n.empty()) n = ResolveUsername(inst); }
        return n;
    }

    static void HandleDamageImpl(void* params)
    {
        if (!params) return;
        const char* base = static_cast<const char*>(params);
        int32_t offA = mik::g_offDmgActor.load(), offD = mik::g_offDmgAmount.load(), offC = mik::g_offDmgCauser.load();
        if (offA < 0 || offD < 0) return;
        UObject* marble = nullptr; float dmg = 0.f; UObject* causer = nullptr;
        if (!SafeReadBytes(base + offA, &marble, sizeof(marble)) || !marble) return;
        if (!SafeReadBytes(base + offD, &dmg, sizeof(dmg))) return;
        if (offC >= 0) SafeReadBytes(base + offC, &causer, sizeof(causer));
        if (!(dmg > 0.f) || IsGhostMarble(marble)) return;

        std::wstring name = ResolveUsername(marble);
        if (name.empty()) return;
        int32_t hp = -1, maxHp = -1;
        ReadIntPropByName(marble, STR("CurrentHealth"), hp);
        ReadIntPropByName(marble, STR("MaxHealth"), maxHp);
        std::wstring attacker = causer && causer != marble ? AttackerName(causer) : std::wstring();

        // Backup resurrection detector, for any revive path the respawn hooks
        // don't see: an "eliminated" player whose NEW pawn is taking hits.
        if (mik::IsLateResurrection(name, marble))
        {
            mik::RecordRespawn(name);
            Output::send<LogLevel::Default>(STR("[MikMarble] respawn: {} (detected via damage to a new pawn)\n"), name);
            if (mik::ClaimResurrectionReport(name))
                mik::Emit("marble_resurrected", PlayerFields(name, DisplayNameOf(marble)) + ", \"detected_by\": \"damage\"");
        }

        bool logIt = false;
        { std::lock_guard<std::mutex> lk(mik::g_mtx); if (mik::g_dmgDebugLeft > 0) { --mik::g_dmgDebugLeft; logIt = true; } }
        if (logIt)
            Output::send<LogLevel::Default>(STR("[MikMarble] damage: {} -{} hp={}/{} by='{}'\n"), name, dmg, hp, maxHp, attacker);

        // CurrentHealth is still the pre-hit value when this handler runs (the
        // game applies the hit afterwards). Verified in a 30-marble royale: each
        // next reading equals trunc(previous - damage), e.g. 77 -5.23 -> 71,
        // 71 -9.37 -> 61. Report the post-hit value the same way.
        int32_t hpAfter = hp >= 0 ? (std::max)(0, static_cast<int32_t>(static_cast<float>(hp) - dmg)) : -1;
        mik::Emit("marble_damaged", PlayerFields(name, DisplayNameOf(marble)) +
                  ", \"damage\": " + JNum(dmg) +
                  ", \"health\": " + (hpAfter >= 0 ? std::to_string(hpAfter) : "null") +
                  ", \"health_before\": " + (hp >= 0 ? std::to_string(hp) : "null") +
                  ", \"max_health\": " + (maxHp >= 0 ? std::to_string(maxHp) : "null") +
                  ", \"attacker\": " + (attacker.empty() ? "null" : JStr(attacker)),
                  /*lowPriority*/ true);
    }
    __declspec(noinline) static void HandleDamage(void* params) noexcept
    {
        __try { HandleDamageImpl(params); }
        __except (MikSehFilter(GetExceptionCode())) {}
    }

    // ── player_joined ────────────────────────────────────────────────────────
    // Two verified signals, joined by name:
    //  1. ULiveStreamChatProcessor::OnChatMessageReceived(Viewer, ChatMessage) —
    //     fires for every chat message on every processor; we keep only the
    //     ViewerJoinGameCommandProcessor's, which carries the command text.
    //  2. UStartRaceFormWidgetY2::OnPlayerJoinedGame(AMOSPlayerState*) — the
    //     game's own "this viewer is in" broadcast (UMOSCoreDelegates). Fires for
    //     chat joins, not for bots.
    // A marble sweep on the tick (CheckNewMarbles) backs (2) up and also
    // reports bots, with via_chat=false.
    static UObject* ContextObject(UnrealScriptFunctionCallableContext& ctx) noexcept
    {
        UObject* o = nullptr;
        __try { o = ctx.Context; } __except (MikSehFilter(GetExceptionCode())) { o = nullptr; }
        return o;
    }

    static void HandleChatImpl(UObject* processor, void* params)
    {
        if (!processor || !params) return;
        UClass* cls = SafeGetClassPrivate(processor);
        if (!cls) return;
        std::wstring cn; try { cn = cls->GetName(); } catch (...) { return; }
        if (cn.find(L"JoinGameCommand") == std::wstring::npos) return;

        const char* base = static_cast<const char*>(params);
        int32_t offV = mik::g_offChatViewer.load(), offM = mik::g_offChatMsg.load();
        int32_t offU = mik::g_offViewerUser.load(), offD = mik::g_offViewerDisp.load(), offT = mik::g_offMsgText.load();
        if (offV < 0 || offM < 0 || offU < 0 || offT < 0) return;
        std::wstring user, disp, text;
        SafeReadFString(base + offV + offU, user);
        if (offD >= 0) SafeReadFString(base + offV + offD, disp);
        // Twitch caps chat lines at 500 chars; allow that much.
        SafeReadFString(base + offM + offT, text, 512);
        if (user.empty()) return;

        mik::PendingChat pc{user, disp, text, std::chrono::steady_clock::now()};
        std::lock_guard<std::mutex> lk(mik::g_mtx);
        // This processor sees EVERY chat line, so the map would otherwise grow
        // by one entry per chatter for the whole session. Keep it bounded.
        if (mik::g_pendingChat.size() >= 4000) mik::PruneChatLocked(/*force*/ true);
        mik::g_pendingChat[ToLowerW(user)] = pc;
        if (!disp.empty()) mik::g_pendingChat[ToLowerW(disp)] = pc;
    }
    __declspec(noinline) static void HandleChat(UObject* processor, void* params) noexcept
    {
        __try { HandleChatImpl(processor, params); }
        __except (MikSehFilter(GetExceptionCode())) {}
    }

    // Report a join once per name per match. Returns false if already reported.
    static bool EmitJoin(const std::wstring& name, const std::wstring& dispIn, bool confirmedByGame)
    {
        if (name.empty()) return false;
        std::wstring low = ToLowerW(name);
        std::wstring disp = dispIn;
        std::wstring command;
        bool viaChat = false;
        {
            std::lock_guard<std::mutex> lk(mik::g_mtx);
            if (!mik::g_joinedLow.insert(low).second) return false;
            if (!disp.empty()) mik::g_joinedLow.insert(ToLowerW(disp));
            auto it = mik::g_pendingChat.find(low);
            if (it == mik::g_pendingChat.end() && !disp.empty()) it = mik::g_pendingChat.find(ToLowerW(disp));
            // A command older than 10 minutes belongs to some earlier race.
            if (it != mik::g_pendingChat.end() &&
                std::chrono::steady_clock::now() - it->second.at < std::chrono::minutes(10))
            {
                command = it->second.command;
                viaChat = true;
                if (disp.empty()) disp = it->second.disp;
            }
        }
        mik::Emit("player_joined", PlayerFields(name, disp) +
                  ", \"command\": " + (command.empty() ? "null" : JStr(command)) +
                  ", \"via_chat\": " + ((viaChat || confirmedByGame) ? "true" : "false"));
        return true;
    }

    static void HandlePlayerJoinedImpl(void* params)
    {
        if (!params) return;
        int32_t off = mik::g_offJoinedPS.load();
        if (off < 0) return;   // param not found by reflection: don't guess
        UObject* ps = nullptr;
        if (!SafeReadBytes(static_cast<const char*>(params) + off, &ps, sizeof(ps)) || !ps) return;
        std::wstring name = ResolveUsername(ps);
        EmitJoin(name, DisplayNameOf(ps), /*confirmedByGame*/ true);
    }
    __declspec(noinline) static void HandlePlayerJoined(void* params) noexcept
    {
        __try { HandlePlayerJoinedImpl(params); }
        __except (MikSehFilter(GetExceptionCode())) {}
    }

    // ── countdown ────────────────────────────────────────────────────────────
    // One event per whole second per phase. "start" = the pre-race countdown
    // after the host presses Start; "race_end" = the force-race-end timer.
    static void EmitCountdown(const char* phase, double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0 || seconds > 36000) return;
        int whole = static_cast<int>(std::ceil(seconds - 1e-3));
        {
            std::lock_guard<std::mutex> lk(mik::g_mtx);
            auto it = mik::g_lastCountdown.find(phase);
            if (it != mik::g_lastCountdown.end() && it->second == whole) return;
            mik::g_lastCountdown[phase] = whole;
        }
        mik::Emit("countdown", std::string("\"phase\": ") + JStr(std::string(phase)) +
                  ", \"seconds_left\": " + std::to_string(whole), /*lowPriority*/ true);
    }

    struct TimerHookInfo { const char* phase; int32_t offset; bool isFloat; std::atomic<bool> seen{false}; const wchar_t* label; };
    static TimerHookInfo& TimerInfo(int i)
    {
        static TimerHookInfo infos[4] = {
            {"start",    -1, false, {}, L"MarbleRaceGameMode:OnStartMatchTimer"},
            {"start",    -1, false, {}, L"StartRaceWidgetY2:OnStartMatchTimerUpdated"},
            {"race_end", -1, true,  {}, L"MarbleRaceHUDY2:OnForceRaceEndTimerUpdated"},
            {"race_end", -1, true,  {}, L"MarbleRaceGameState:OnRaceEndTimerUpdated"},
        };
        return infos[i];
    }
    static void HandleTimerImpl(int which, void* params)
    {
        TimerHookInfo& ti = TimerInfo(which);
        if (!params || ti.offset < 0) return;
        if (!ti.seen.exchange(true))
            Output::send<LogLevel::Default>(STR("[MikMarble] countdown source active: {}\n"), ti.label);
        const char* p = static_cast<const char*>(params) + ti.offset;
        if (ti.isFloat) { float f = 0; if (SafeReadBytes(p, &f, sizeof(f))) EmitCountdown(ti.phase, f); }
        else            { int32_t v = 0; if (SafeReadBytes(p, &v, sizeof(v))) EmitCountdown(ti.phase, v); }
    }
    __declspec(noinline) static void HandleTimer(int which, void* params) noexcept
    {
        __try { HandleTimerImpl(which, params); }
        __except (MikSehFilter(GetExceptionCode())) {}
    }

    // ── grand_prix_finished ──────────────────────────────────────────────────
    // UGrandPrixFinalResultsWidget::BP_Setup(TArray<FRaceResult> FinalResults,
    // int32 TotalRaces) is a BlueprintImplementableEvent: the widget BP's
    // override is a different UFunction than the native declaration, so it's
    // caught with a ProcessEvent post-callback matched by function FName.
    static int32_t StructMemberOffset(const UStruct* s, const wchar_t* member)
    {
        if (!s) return -1;
        for (FProperty* prop : TFieldRange<FProperty>(const_cast<UStruct*>(s), EFieldIterationFlags::IncludeSuper))
        {
            if (!prop) continue;
            std::wstring n; try { n = prop->GetName(); } catch (...) { continue; }
            if (n == member) { try { return prop->GetOffset_Internal(); } catch (...) { return -1; } }
        }
        return -1;
    }

    static FProperty* FindParamProp(UFunction* fn, const wchar_t* paramName)
    {
        if (!fn) return nullptr;
        for (FProperty* prop : TFieldRange<FProperty>(fn, EFieldIterationFlags::None))
        {
            if (!prop) continue;
            std::wstring n; try { n = prop->GetName(); } catch (...) { continue; }
            if (n == paramName) return prop;
        }
        return nullptr;
    }

    // Offset of `member` inside the struct-typed parameter `paramName`, from
    // reflection. -1 if either is missing.
    static int32_t ParamStructMemberOffset(UFunction* fn, const wchar_t* paramName, const wchar_t* member)
    {
        FProperty* p = FindParamProp(fn, paramName);
        if (!p) return -1;
        const UStruct* s = nullptr;
        try
        {
            // A game update could change the parameter's type; never treat a
            // non-struct property as one.
            FStructProperty* sp = CastField<FStructProperty>(p);
            if (!sp) return -1;
            s = sp->GetStruct();
        }
        catch (...) { return -1; }
        return StructMemberOffset(s, member);
    }

    static void HandleGrandPrixImpl(UFunction* fn, void* params)
    {
        if (!fn || !params) return;
        {
            std::lock_guard<std::mutex> lk(mik::g_mtx);
            if (mik::g_gpSent) return;
            mik::g_gpSent = true;
        }
        FProperty* arrProp = FindParamProp(fn, L"FinalResults");
        int32_t offTotal = FindParamOffset(fn, L"TotalRaces");
        if (!arrProp) { Output::send<LogLevel::Warning>(STR("[MikMarble] grand prix: FinalResults param not found\n")); return; }

        // Element layout of FRaceResult from reflection (fallbacks = SDK dump).
        int32_t elemSize = 0xA0;
        const UStruct* rs = nullptr;
        try
        {
            FArrayProperty* ap = CastField<FArrayProperty>(arrProp);
            if (!ap) { Output::send<LogLevel::Warning>(STR("[MikMarble] grand prix: FinalResults is not an array\n")); return; }
            FProperty* inner = ap->GetInner();
            FStructProperty* sp = inner ? CastField<FStructProperty>(inner) : nullptr;
            if (!sp) { Output::send<LogLevel::Warning>(STR("[MikMarble] grand prix: FinalResults element is not a struct\n")); return; }
            elemSize = inner->GetElementSize();
            rs = sp->GetStruct();
        }
        catch (...) { return; }
        auto off = [&](const wchar_t* m, int32_t fb) { int32_t o = StructMemberOffset(rs, m); return o >= 0 ? o : fb; };
        const int32_t oPos = off(L"Position", 0x00), oUser = off(L"_Username", 0x20), oDisp = off(L"_Displayname", 0x30);
        const int32_t oPts = off(L"_CurrentPoints", 0x50), oWins = off(L"_CurrentWins", 0x5C);
        const int32_t oElims = off(L"_Eliminations", 0x70), oDmg = off(L"DamageDealt", 0x9C);

        const char* base = static_cast<const char*>(params);
        TArrayRaw a{};
        if (!SafeReadBytes(base + arrProp->GetOffset_Internal(), &a, sizeof(a))) return;
        int32_t total = -1;
        if (offTotal >= 0) SafeReadBytes(base + offTotal, &total, sizeof(total));
        if (a.Num < 0 || a.Num > 10000 || (a.Num > 0 && !a.Data) || elemSize <= 0) return;

        std::string standings;
        int count = 0;
        for (int i = 0; i < a.Num; ++i)
        {
            const char* e = static_cast<const char*>(a.Data) + static_cast<size_t>(i) * elemSize;
            int32_t pos = 0, pts = 0, wins = 0, elims = 0, dmg = 0;
            std::wstring user, disp;
            SafeReadBytes(e + oPos, &pos, sizeof(pos));
            SafeReadBytes(e + oPts, &pts, sizeof(pts));
            SafeReadBytes(e + oWins, &wins, sizeof(wins));
            SafeReadBytes(e + oElims, &elims, sizeof(elims));
            SafeReadBytes(e + oDmg, &dmg, sizeof(dmg));
            SafeReadFString(e + oUser, user);
            SafeReadFString(e + oDisp, disp);
            if (user.empty() && disp.empty()) continue;
            if (count++ > 0) standings += ", ";
            standings += "{\"position\": " + std::to_string(pos > 0 ? pos : i + 1) + ", " +
                         PlayerFields(user.empty() ? disp : user, disp) +
                         ", \"points\": " + std::to_string(pts) +
                         ", \"wins\": " + std::to_string(wins) +
                         ", \"eliminations\": " + std::to_string(elims) +
                         ", \"damage\": " + std::to_string(dmg) + "}";
        }
        Output::send<LogLevel::Default>(STR("[MikMarble] grand prix finished: {} players, {} races\n"), count, total);
        mik::Emit("grand_prix_finished", "\"races\": " + (total >= 0 ? std::to_string(total) : std::string("null")) +
                  ", \"standings\": [" + standings + "]");
    }
    __declspec(noinline) static void HandleGrandPrix(UFunction* fn, void* params) noexcept
    {
        __try { HandleGrandPrixImpl(fn, params); }
        __except (MikSehFilter(GetExceptionCode())) {}
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
        int order = mik::RecordElimination(victim, killer, marble);
        if (order > 0)
            mik::Emit("marble_eliminated", PlayerFields(victim, DisplayNameOf(marble)) +
                      ", \"killer\": " + (killer.empty() ? "null" : JStr(killer)) +
                      ", \"elimination_order\": " + std::to_string(order));
    }
    __declspec(noinline) static void HandleElimination(void* params) noexcept
    {
        __try { HandleEliminationImpl(params); }
        __except (MikSehFilter(GetExceptionCode())) {}
    }

    // Common resurrection handler. `offPS` locates the respawned player's
    // PlayerState in the hooked call's param block; `result` is the call's
    // return slot (UnrealScriptFunctionCallableContext::RESULT_DECL), which
    // holds the new pawn in a post-hook. NOT the ReturnValue offset inside the
    // param block: for Blueprint-originated calls UE4SS builds that block
    // without initialising the return slot, so it is stack garbage. Prefer
    // the pawn's _Username (same name source as the elimination records),
    // fall back to the PlayerState.
    // Name + display name of a pawn or PlayerState, each source in its own
    // fault guard: a bad pawn pointer must not stop the PlayerState fallback.
    static void ResolveNamesInto(UObject* obj, std::wstring* name, std::wstring* disp)
    {
        *name = ResolveUsername(obj);
        if (!name->empty()) *disp = DisplayNameOf(obj);
    }
    __declspec(noinline) static bool SafeResolveNames(UObject* obj, std::wstring* name, std::wstring* disp) noexcept
    {
        if (!obj) return false;
        __try { ResolveNamesInto(obj, name, disp); return true; }
        __except (MikSehFilter(GetExceptionCode())) { return false; }
    }

    static void HandleRespawnImpl(void* params, int32_t offPS, void* result)
    {
        if (!params) return;
        const char* base = static_cast<const char*>(params);
        UObject* pawn = nullptr; UObject* ps = nullptr;
        if (result)       SafeReadBytes(result, &pawn, sizeof(pawn));
        if (offPS   >= 0) SafeReadBytes(base + offPS,   &ps,   sizeof(ps));

        std::wstring name, disp;
        bool fromPawn = SafeResolveNames(pawn, &name, &disp) && !name.empty();
        if (fromPawn) NoteMarbleNames(pawn, name);
        else { name.clear(); disp.clear(); SafeResolveNames(ps, &name, &disp); }
        if (name.empty())
        {
            Output::send<LogLevel::Warning>(STR("[MikMarble] respawn hook fired but no name resolved (pawn={} ps={})\n"),
                reinterpret_cast<uintptr_t>(pawn), reinterpret_cast<uintptr_t>(ps));
            return;
        }

        mik::RecordRespawn(name);
        Output::send<LogLevel::Default>(STR("[MikMarble] respawn: {} (elimination erased, name from {})\n"),
            name, fromPawn ? STR("pawn") : STR("PlayerState"));
        if (mik::ClaimResurrectionReport(name))
            mik::Emit("marble_resurrected", PlayerFields(name, disp) + ", \"detected_by\": \"respawn_hook\"");
    }
    // BP_RespawnPlayer(FPendingRoyalePlayerRespawn PlayerRespawn) -> APawn*
    __declspec(noinline) static void HandleRespawn(void* params, void* result) noexcept
    {
        __try { HandleRespawnImpl(params, mik::g_offRespawnPS.load(), result); }
        __except (MikSehFilter(GetExceptionCode())) {}
    }
    // RespawnMarble(APlayerState* Player) -> AMarbleRoyaleMarble*
    // This is the function that actually performs a resurrection — verified by
    // forcing respawns in live 200-bot royales: RespawnMarble spawns the pawn
    // while BP_RespawnPlayer (the wave-respawn wrapper) never fires. Both are
    // hooked; RecordRespawn dedupes if a path ever triggers both.
    __declspec(noinline) static void HandleRespawn2(void* params, void* result) noexcept
    {
        __try { HandleRespawnImpl(params, mik::g_offRespawn2PS.load(), result); }
        __except (MikSehFilter(GetExceptionCode())) {}
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

    __declspec(noinline) static bool ParmsSizeIs(UFunction* fn, size_t expected) noexcept
    {
        uint16_t sz = 0;
        __try { sz = fn->GetParmsSize(); }
        __except (MikSehFilter(GetExceptionCode())) { return false; }
        return sz == expected;
    }

    // Call a `UObject* Fn(int32)` UFunction (e.g. GameMode.FindMarbleAtPosition)
    // via ProcessEvent. The params buffer is [int32 arg][ptr return]; ProcessEvent
    // writes the return pointer. SEH-guarded — a bad call must never crash.
    __declspec(noinline) static UObject* CallIntToObj(UObject* obj, UFunction* fn, int32_t arg) noexcept
    {
        if (!obj || !fn) return nullptr;
        struct Params { int32_t Arg; UObject* ReturnValue; } params{};
        // A game update that changed this signature would make ProcessEvent
        // write past our stack struct — something no __except can undo.
        if (!ParmsSizeIs(fn, sizeof(Params))) return nullptr;
        params.Arg = arg;
        params.ReturnValue = nullptr;
        __try { obj->ProcessEvent(fn, &params); }
        __except (MikSehFilter(GetExceptionCode())) { return nullptr; }
        return params.ReturnValue;
    }

    // Call a no-arg `FVector Fn()` UFunction (Actor.K2_GetActorLocation /
    // SceneComponent.K2_GetComponentLocation) — returns world location.
    __declspec(noinline) static bool CallGetVector(UObject* obj, UFunction* fn, FVectorD& out) noexcept
    {
        if (!obj || !fn) return false;
        struct Params { FVectorD ReturnValue; } params{};
        if (!ParmsSizeIs(fn, sizeof(Params))) return false;   // see CallIntToObj
        __try { obj->ProcessEvent(fn, &params); }
        __except (MikSehFilter(GetExceptionCode())) { return false; }
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
        UFunction* fnStart  = FindFuncInChain(cls, L"OnMatchStarted");

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
        {
            mik::g_offFinishWinner.store(FindParamOffset(fnFinish, L"bRaceWinner"));
            fnFinish->RegisterPostHook([](UnrealScriptFunctionCallableContext& ctx, void*) {
                HandleFinish(ParamsBase(ctx));
            });
        }
        fnElim->RegisterPostHook([](UnrealScriptFunctionCallableContext& ctx, void*) {
            HandleElimination(ParamsBase(ctx));
        });
        // Match start closes the join window (see the marble sweep in OnUpdateImpl).
        if (fnStart)
            fnStart->RegisterPostHook([](UnrealScriptFunctionCallableContext&, void*) {
                mik::g_matchStarted.store(true);
            });
        fnEnd->RegisterPostHook([](UnrealScriptFunctionCallableContext&, void*) {
            mik::g_endPending.store(true);
            Output::send<LogLevel::Default>(STR("[MikMarble] OnMatchEnded fired\n"));
        });

        m_hooksRegistered = true;
        Output::send<LogLevel::Default>(STR("[MikMarble] hooks registered (finish={})\n"), fnFinish ? 1 : 0);
    }

    // ── Discovery trace (config: trace_functions=1) ─────────────────────────
    // Logs each distinct UFunction the first time it goes through ProcessEvent
    // (per map), plus every call of functions whose name mentions a timer, so
    // a game update's renamed events can be found from one play-through.
    // Off by default: it only costs an atomic load per ProcessEvent then.
    inline static std::mutex g_traceMtx;
    static void TraceFunctionImpl(UObject* context, UFunction* fn)
    {
        // try_lock, never lock: if another thread is mid-trace, skip this call
        // rather than block a ProcessEvent caller.
        std::unique_lock<std::mutex> lk(g_traceMtx, std::try_to_lock);
        if (!lk.owns_lock()) return;
        static std::set<UFunction*> seen;
        static std::map<UFunction*, bool> timerish;
        static std::map<UFunction*, std::chrono::steady_clock::time_point> lastRepeat;
        static int lastSerial = -1;
        int serial = mik::g_matchSerial.load();
        if (serial != lastSerial) { lastSerial = serial; seen.clear(); lastRepeat.clear(); timerish.clear(); }

        bool first = seen.insert(fn).second;
        auto it = timerish.find(fn);
        if (it == timerish.end())
        {
            std::wstring n; try { n = fn->GetName(); } catch (...) {}
            std::wstring low = ToLowerW(n);
            bool t = low.find(L"timer") != std::wstring::npos || low.find(L"countdown") != std::wstring::npos ||
                     low.find(L"time") != std::wstring::npos;
            it = timerish.emplace(fn, t).first;
        }
        if (!first)
        {
            if (!it->second) return;
            // Timer-ish repeats: at most one line per 0.5 s per function.
            auto now = std::chrono::steady_clock::now();
            auto& last = lastRepeat[fn];
            if (now - last < std::chrono::milliseconds(500)) return;
            last = now;
        }

        std::wstring cn, fname, owner;
        try { if (UClass* c = SafeGetClassPrivate(context)) cn = c->GetName(); } catch (...) {}
        try { fname = fn->GetName(); } catch (...) {}
        try { if (UObject* o = fn->GetOuterPrivate()) owner = o->GetName(); } catch (...) {}
        Output::send<LogLevel::Default>(STR("[MikMarble] trace{} {}.{} (on {})\n"),
            first ? STR("") : STR(" (repeat)"), owner, fname, cn);
    }
    __declspec(noinline) static void TraceFunction(UObject* context, UFunction* fn) noexcept
    {
        __try { TraceFunctionImpl(context, fn); }
        __except (MikSehFilter(GetExceptionCode())) {}
    }

    // ── Live-event hooks ─────────────────────────────────────────────────────
    // Resolved by static /Script path (native classes exist from startup), each
    // registered at most once; retried on every tick until all are in.
    static UFunction* FindScriptFunction(const wchar_t* path)
    {
        UFunction* fn = nullptr;
        try { fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path); } catch (...) { fn = nullptr; }
        return fn;
    }

    inline static bool m_hookDamage = false, m_hookChat = false, m_hookJoined = false, m_hookGpCb = false;
    inline static bool m_hookTimer[4] = {false, false, false, false};
    inline static bool m_eventHooksLogged = false;

    void TryRegisterEventHooks()
    {
        if (!m_hookDamage)
            if (UFunction* fn = FindScriptFunction(STR("/Script/MarblesOnStream.MarbleRaceGameMode:OnMarbleTookAnyDamage")))
            {
                mik::g_offDmgActor.store(FindParamOffset(fn, L"DamagedActor"));
                mik::g_offDmgAmount.store(FindParamOffset(fn, L"Damage"));
                mik::g_offDmgCauser.store(FindParamOffset(fn, L"DamageCauser"));
                fn->RegisterPostHook([](UnrealScriptFunctionCallableContext& ctx, void*) { HandleDamage(ParamsBase(ctx)); });
                m_hookDamage = true;
                Output::send<LogLevel::Default>(STR("[MikMarble] damage hook registered (off {}/{}/{})\n"),
                    mik::g_offDmgActor.load(), mik::g_offDmgAmount.load(), mik::g_offDmgCauser.load());
            }

        if (!m_hookChat)
            if (UFunction* fn = FindScriptFunction(STR("/Script/MarblesOnStream.LiveStreamChatProcessor:OnChatMessageReceived")))
            {
                mik::g_offChatViewer.store(FindParamOffset(fn, L"Viewer"));
                mik::g_offChatMsg.store(FindParamOffset(fn, L"ChatMessage"));
                mik::g_offViewerUser.store(ParamStructMemberOffset(fn, L"Viewer", L"Username"));
                mik::g_offViewerDisp.store(ParamStructMemberOffset(fn, L"Viewer", L"DisplayName"));
                mik::g_offMsgText.store(ParamStructMemberOffset(fn, L"ChatMessage", L"Message"));
                fn->RegisterPostHook([](UnrealScriptFunctionCallableContext& ctx, void*) {
                    HandleChat(ContextObject(ctx), ParamsBase(ctx));
                });
                m_hookChat = true;
                Output::send<LogLevel::Default>(STR("[MikMarble] chat hook registered (viewer {} +{}/+{}, msg {} +{})\n"),
                    mik::g_offChatViewer.load(), mik::g_offViewerUser.load(), mik::g_offViewerDisp.load(),
                    mik::g_offChatMsg.load(), mik::g_offMsgText.load());
            }

        if (!m_hookJoined)
            if (UFunction* fn = FindScriptFunction(STR("/Script/MarblesOnStream.StartRaceFormWidgetY2:OnPlayerJoinedGame")))
            {
                mik::g_offJoinedPS.store(FindParamOffset(fn, L"Player"));
                fn->RegisterPostHook([](UnrealScriptFunctionCallableContext& ctx, void*) { HandlePlayerJoined(ParamsBase(ctx)); });
                m_hookJoined = true;
            }

        static const wchar_t* kTimerPaths[4] = {
            STR("/Script/MarblesOnStream.MarbleRaceGameMode:OnStartMatchTimer"),
            STR("/Script/MarblesOnStream.StartRaceWidgetY2:OnStartMatchTimerUpdated"),
            STR("/Script/MarblesOnStream.MarbleRaceHUDY2:OnForceRaceEndTimerUpdated"),
            STR("/Script/MarblesOnStream.MarbleRaceGameState:OnRaceEndTimerUpdated"),
        };
        static const wchar_t* kTimerParams[4] = { L"timeRemaining", L"Time", L"Seconds", L"Seconds" };
        for (int i = 0; i < 4; ++i)
        {
            if (m_hookTimer[i]) continue;
            UFunction* fn = FindScriptFunction(kTimerPaths[i]);
            if (!fn) continue;
            TimerInfo(i).offset = FindParamOffset(fn, kTimerParams[i]);
            switch (i)   // capture-less lambdas need a compile-time index
            {
            case 0: fn->RegisterPostHook([](UnrealScriptFunctionCallableContext& c, void*) { HandleTimer(0, ParamsBase(c)); }); break;
            case 1: fn->RegisterPostHook([](UnrealScriptFunctionCallableContext& c, void*) { HandleTimer(1, ParamsBase(c)); }); break;
            case 2: fn->RegisterPostHook([](UnrealScriptFunctionCallableContext& c, void*) { HandleTimer(2, ParamsBase(c)); }); break;
            case 3: fn->RegisterPostHook([](UnrealScriptFunctionCallableContext& c, void*) { HandleTimer(3, ParamsBase(c)); }); break;
            }
            m_hookTimer[i] = true;
        }

        if (!m_hookGpCb)
        {
            static FName sBpSetup(STR("BP_Setup"), FNAME_Add);
            Hook::FCallbackOptions opts{};
            opts.OwnerModName = STR("MikMarbleMod");
            opts.HookName = STR("GrandPrixFinalResults");
            Hook::RegisterProcessEventPostCallback(
                [](auto&, UObject* context, UFunction* fn, void* parms) {
                    if (!fn || !context) return;
                    if (mik::g_trace.load(std::memory_order_relaxed)) TraceFunction(context, fn);
                    if (fn->GetNamePrivate() != sBpSetup) return;   // cheap: one FName compare per ProcessEvent
                    UClass* cls = SafeGetClassPrivate(context);
                    if (!cls) return;
                    std::wstring cn; try { cn = cls->GetName(); } catch (...) { return; }
                    if (cn.find(L"GrandPrixFinalResults") == std::wstring::npos) return;
                    HandleGrandPrix(fn, parms);
                },
                opts);
            m_hookGpCb = true;
        }

        if (!m_eventHooksLogged && m_hookDamage && m_hookChat && m_hookJoined)
        {
            m_eventHooksLogged = true;
            Output::send<LogLevel::Default>(STR("[MikMarble] event hooks registered (timers {}{}{}{})\n"),
                m_hookTimer[0] ? 1 : 0, m_hookTimer[1] ? 1 : 0, m_hookTimer[2] ? 1 : 0, m_hookTimer[3] ? 1 : 0);
        }
    }

    // ── map_entered + bot/backup joins (tick-driven) ─────────────────────────

    static std::string ModeFromGameMode(UObject* gm)
    {
        if (UObjectGlobals::FindFirstOf(STR("BullseyeGameState"))) return "bullseye";
        UClass* cls = SafeGetClassPrivate(gm);
        for (int depth = 0; cls && depth < 16; ++depth)
        {
            std::wstring n; try { n = cls->GetName(); } catch (...) { break; }
            if (n == L"MarbleRoyaleGameMode")       return "royale";
            if (n == L"GrandPrixResultsGameMode")   return "grand_prix_results";
            if (n == L"MarbleLapsRaceGameMode")     return "laps";
            if (n == L"TiltedGameMode")             return "tilted";
            if (n == L"MarbleRaceGameMode")         return "race";
            try { cls = cls->GetSuperClass(); } catch (...) { break; }
        }
        return "other";
    }

    // Level asset name, e.g. "/Game/Maps/Season13Tracks/SpaceStation" -> "SpaceStation".
    static std::wstring LevelNameOf(UObject* obj)
    {
        std::wstring n;
        try { if (UObject* pkg = obj->GetOutermost()) n = pkg->GetName(); } catch (...) { return {}; }
        auto slash = n.rfind(L'/');
        return slash == std::wstring::npos ? n : n.substr(slash + 1);
    }

    bool     m_mapEnteredSent = false;
    int      m_mapEnteredTries = 0;
    UObject* m_curGpGameMode = nullptr;
    // GameMode of the current map (set with map_entered). Weak: the GameMode
    // is destroyed with its level, and a raw pointer kept across ticks could
    // dangle; FWeakObjectPtr::Get() returns null once the object is gone.
    FWeakObjectPtr m_curGameModeWeak{};
    bool           m_haveGameMode = false;
    UObject* CurGameMode() const { return m_haveGameMode ? m_curGameModeWeak.Get() : nullptr; }
    bool     m_joinWindowClosed = false;   // bCanJoinRace went false: final join sweep done

    // `anchor` is the current GameState (or the GameMode itself). Pick the
    // GameMode from the same level package, so a not-yet-collected GameMode of
    // the previous map can never be reported.
    void TryEmitMapEntered(UObject* anchor)
    {
        if (m_mapEnteredSent || !anchor) return;
        UObject* gm = nullptr;
        {
            UObject* pkg = nullptr;
            try { pkg = anchor->GetOutermost(); } catch (...) { return; }
            std::vector<UObject*> gms;
            UObjectGlobals::FindAllOf(STR("MOSGameMode"), gms);
            for (UObject* o : gms)
            {
                UObject* op = nullptr;
                try { op = o->GetOutermost(); } catch (...) { continue; }
                if (op == pkg) { gm = o; break; }
            }
        }
        if (!gm) return;
        std::wstring disp, matchId, ufeName;
        ReadStrProp(gm, STR("MyMapDisplayName"), disp);
        ReadStrProp(gm, STR("MatchID"), matchId);
        // The display name is filled in a moment after the GameMode spawns;
        // wait for it (up to ~10 s, then send whatever we have).
        disp = WideTrim(disp);
        if (disp.empty() && ++m_mapEnteredTries < 20) return;
        bool gp = false;
        ReadBoolProp(gm, STR("bGrandPrixMode"), gp);
        m_grandPrixActive = gp;   // a GP race: the results podium may follow
        if (UObject* ufe = ReadObjProp(gm, STR("UserFacingExperienceDefinition")))
            try { ufeName = ufe->GetName(); } catch (...) {}
        std::wstring level = LevelNameOf(gm);
        std::string mode = ModeFromGameMode(gm);

        m_mapEnteredSent = true;
        m_curGameModeWeak = gm;
        m_haveGameMode = true;
        mik::SetContext(WideToUtf8(matchId), WideToUtf8(disp.empty() ? level : disp), mode);
        Output::send<LogLevel::Default>(STR("[MikMarble] map entered: '{}' level={} mode={} gp={}\n"),
            disp, level, std::wstring(mode.begin(), mode.end()), gp ? 1 : 0);
        mik::Emit("map_entered",
                  "\"level\": " + JStr(level) +
                  ", \"experience\": " + (ufeName.empty() ? "null" : JStr(ufeName)) +
                  ", \"grand_prix\": " + (gp ? "true" : "false"));
    }

    static std::wstring WideTrim(const std::wstring& s)
    {
        size_t a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos) return {};
        size_t b = s.find_last_not_of(L" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // Report marbles we haven't announced yet (bots, or chat joins the widget
    // hook missed). Runs on the 0.5 s tick until the match ends.
    void CheckNewMarbles()
    {
        std::vector<UObject*> marbles;
        UObjectGlobals::FindAllOf(STR("Marble"), marbles);
        for (UObject* m : marbles)
        {
            if (IsGhostMarble(m)) continue;
            std::wstring n = ResolveUsername(m);
            if (n.empty()) continue;
            // A marble caught mid-spawn still carries its object name
            // ("Marble_BP_C_2147477240") as its username; wait for the real one.
            std::wstring objName;
            try { objName = m->GetName(); } catch (...) {}
            if (n == objName || n.rfind(L"Marble_BP_C_", 0) == 0) continue;
            {
                std::lock_guard<std::mutex> lk(mik::g_mtx);
                if (mik::g_joinedLow.count(ToLowerW(n))) continue;
            }
            EmitJoin(n, DisplayNameOf(m), /*confirmedByGame*/ false);
        }
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
            HandleRespawn(ParamsBase(ctx), ResultSlot(ctx));
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
                HandleRespawn2(ParamsBase(ctx), ResultSlot(ctx));
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
        // Snapshot the ignore list once, by value; it's needed before the JSON
        // is built.
        std::vector<std::string> ignored;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            ignored = m_config.ignoredNames;
        }

        // Optional ignore list. Applied to race / death race / bullseye, but NOT
        // royale: there, placement is reconstructed from elimination order, so
        // removing entries would misreport who actually went out first and last.
        // Filtering happens here on the game thread, before anything is copied
        // into the worker, so the POST body and the ImGui panel agree.
        std::vector<PlayerResult> outPlayers = players;
        if (!ignored.empty() && gameType != "royale")
        {
            const size_t before = outPlayers.size();
            outPlayers.erase(std::remove_if(outPlayers.begin(), outPlayers.end(),
                                            [&ignored](const PlayerResult& p) {
                                                const std::string n = AsciiLower(WideToUtf8(p.name));
                                                return std::find(ignored.begin(), ignored.end(), n) != ignored.end();
                                            }),
                             outPlayers.end());
            const size_t removed = before - outPlayers.size();
            if (removed > 0)
            {
                Output::send<LogLevel::Default>(STR("[MikMarble] ignore list: omitted {} player(s) from results\n"),
                                                removed);
            }
        }

        std::string json = BuildResultsJson(outPlayers, gameType);
        {
            std::lock_guard<std::mutex> lock(m_state->mtx);
            m_state->lastGameType = gameType;
            m_state->lastSentPlayers = outPlayers;
        }

        // Every target with results enabled gets the same body, queued behind
        // that target's pending events (so it lands after the last
        // marble_finished / marble_eliminated of the match).
        int queued = 0;
        for (auto& s : mik_bus::Snapshot())
        {
            if (!s->target.sendResults) continue;
            OutJob job;
            job.path = s->target.basePath + "/results";
            job.body = json;
            job.logSuccess = true;
            s->Enqueue(std::move(job));
            ++queued;
        }
        if (queued == 0)
            Output::send<LogLevel::Warning>(STR("[MikMarble] results not sent: no target has results enabled\n"));
    }

    void MaybeSendReady()
    {
        if (!m_unrealReady.load()) return;
        bool expected = false;
        if (!m_readySent.compare_exchange_strong(expected, true)) return;

        // Small handshake payload so the backend knows which build is talking
        // to it. `events` lists what this target will receive on {path}/event
        // (additive field; 3.21/3.22 sent only mod + version).
        for (auto& s : mik_bus::Snapshot())
        {
            std::string events;
            for (const char* e : kAllEvents)
            {
                if (!mik::TargetWants(s->target, e)) continue;
                if (!events.empty()) events += ", ";
                events += JStr(std::string(e));
            }
            OutJob job;
            job.path = s->target.basePath + "/ready";
            job.body = std::string(R"({"mod": "MikMarbleMod", "version": ")") + EscapeJsonString(kModVersion) +
                       R"(", "results": )" + (s->target.sendResults ? "true" : "false") +
                       R"(, "events": [)" + events + "]}";
            job.logSuccess = true;
            s->Enqueue(std::move(job));
        }
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
        if (++m_pruneTick >= 60) { m_pruneTick = 0; mik::PruneChat(); }   // every ~30 s
        TryRegisterHooks();
        TryRegisterRespawnHook();
        TryRegisterEventHooks();

        // Detect match boundaries by GameState identity: a new level load = a new
        // GameState pointer = a fresh match. Reset the accumulator on every change.
        UObject* gs = FindGameState();
        if (gs != m_curGameState)
        {
            m_curGameState = gs;
            m_sentThisMatch = false;
            m_mapEnteredSent = false;
            m_mapEnteredTries = 0;
            m_haveGameMode = false;
            m_joinWindowClosed = false;
            mik::ResetMatch();
        }
        // Grand-prix final results live on their own level with no race
        // GameState; still give it a map_entered so events carry context.
        if (!gs && m_grandPrixActive)
            if (UObject* gpgm = UObjectGlobals::FindFirstOf(STR("GrandPrixResultsGameMode")))
            {
                if (gpgm != m_curGpGameMode) { m_curGpGameMode = gpgm; m_mapEnteredSent = false; m_mapEnteredTries = 0; mik::ResetMatch(); }
                TryEmitMapEntered(gpgm);
            }

        if (gs)
        {
            TryEmitMapEntered(gs);
            // MatchID is assigned a little after the map loads (usually after
            // map_entered went out); pick it up for every later event. Lobbies
            // filled with the Bots button never get one.
            if (m_mapEnteredSent && !mik::HasMatchId())
                if (UObject* gmNow = CurGameMode())
            {
                std::wstring mid;
                if (ReadStrProp(gmNow, STR("MatchID"), mid) && !mid.empty())
                    mik::SetMatchId(WideToUtf8(mid));
            }

            // Joins only happen in the lobby. Sweep for new marbles until the
            // HUD's OnMatchStarted fires, then once more to catch the last
            // spawns, and stop (a 200-marble FindAllOf twice a second for the
            // whole race would be wasted work). Not bCanJoinRace: a full lobby
            // reports false from the start, before the bots have spawned.

            if (m_mapEnteredSent && !m_sentThisMatch && !m_joinWindowClosed)
            {
                bool started = mik::g_matchStarted.load();
                CheckNewMarbles();
                if (started) m_joinWindowClosed = true;
            }
        }

        // The OnMatchEnded hook flips g_endPending; build + POST exactly once.
        if (gs && !m_sentThisMatch && mik::g_endPending.load())
        {
            // Latch first: if building the standings faults (caught by
            // SafeTick), it must not be retried every 0.5 s for the rest of
            // the session.
            m_sentThisMatch = true;
            mik::g_endPending.store(false);
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
        __except (MikSehFilter(GetExceptionCode())) {}
    }

public:
    MikMarbleMod() : CppUserModBase()
    {
        ModName        = STR("MikMarbleMod");
        ModVersion     = STR("3.23");
        ModDescription = STR("Mik marbles race result tracker + live events (C++, event-driven)");
        ModAuthors     = STR("Draxi");

        PinModule();
        m_config = LoadConfig();
        StartSenders();

        register_tab(STR("Mik Marble"), [](CppUserModBase* instance) {
            auto* self = static_cast<MikMarbleMod*>(instance);

            ImGui::Text("MikMarbleMod v%s (C++, event-driven)", kModVersion);
            ImGui::Text("Hooks: %s", self->m_hooksRegistered ? "registered" : "waiting for HUD...");
            ImGui::Text("Respawn hook: %s", self->m_respawnHookRegistered ? "registered" : "waiting for royale...");
            ImGui::Text("Event hooks: damage=%d chat=%d join=%d timers=%d%d%d%d",
                self->m_hookDamage, self->m_hookChat, self->m_hookJoined,
                self->m_hookTimer[0], self->m_hookTimer[1], self->m_hookTimer[2], self->m_hookTimer[3]);
            ImGui::Separator();

            auto senders = mik_bus::Snapshot();
            ImGui::Text("Targets: %d", static_cast<int>(senders.size()));
            for (auto& s : senders)
            {
                std::string filter = DescribeEvents(s->target);
                ImGui::BulletText("%s = %s  [results %s; events: %s]", s->target.id.c_str(), s->target.label.c_str(),
                    s->target.sendResults ? "on" : "off", filter.c_str());
                ImGui::Text("    sent %llu  failed %llu  dropped %llu  queued %d  last HTTP %d  last err %lu",
                    static_cast<unsigned long long>(s->sent.load()), static_cast<unsigned long long>(s->failed.load()),
                    static_cast<unsigned long long>(s->dropped.load()), static_cast<int>(s->QueueSize()),
                    s->lastStatus.load(), static_cast<unsigned long>(s->lastError.load()));
            }
            ImGui::Separator();

            std::vector<PlayerResult> snapshot;
            std::string lastGameType;
            std::map<std::string, int> counts;
            std::deque<std::string> recent;
            {
                std::lock_guard<std::mutex> lock(self->m_state->mtx);
                snapshot     = self->m_state->lastSentPlayers;
                lastGameType = self->m_state->lastGameType;
                counts       = self->m_state->eventCounts;
                recent       = self->m_state->recentEvents;
            }
            std::string countLine;
            for (const auto& [k, v] : counts) { if (!countLine.empty()) countLine += "  "; countLine += k + "=" + std::to_string(v); }
            ImGui::TextWrapped("Events: %s", countLine.empty() ? "(none yet)" : countLine.c_str());
            for (const auto& e : recent) ImGui::TextWrapped("  %s", e.c_str());
            ImGui::Separator();

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
            ImGui::SameLine();
            if (ImGui::Button("Test Events"))
                SendTestEvents();
        });
    }

    static std::string DescribeEvents(const Target& t)
    {
        if (t.events.empty()) return "none";
        if (t.events.size() == std::size(kAllEvents)) return "all";
        std::string out;
        for (const auto& e : t.events) { if (!out.empty()) out += ","; out += e; }
        return out;
    }

    // One of each event with obviously fake data, for wiring up a backend
    // without playing a match.
    static void SendTestEvents()
    {
        mik::Emit("map_entered", "\"level\": \"TestLevel\", \"experience\": null, \"grand_prix\": false, \"test\": true");
        mik::Emit("player_joined", PlayerFields(L"testviewer", L"TestViewer") + ", \"command\": \"!play\", \"via_chat\": true, \"test\": true");
        mik::Emit("countdown", "\"phase\": \"start\", \"seconds_left\": 3, \"test\": true");
        mik::Emit("marble_damaged", PlayerFields(L"testviewer", L"TestViewer") +
                  ", \"damage\": 12.50, \"health\": 87, \"health_before\": 100, \"max_health\": 100, \"attacker\": \"TestRival\", \"test\": true");
        mik::Emit("marble_finished", PlayerFields(L"testviewer", L"TestViewer") + ", \"position\": 1, \"winner\": true, \"test\": true");
        mik::Emit("marble_eliminated", PlayerFields(L"testrival", L"TestRival") +
                  ", \"killer\": \"TestViewer\", \"elimination_order\": 1, \"test\": true");
        mik::Emit("marble_resurrected", PlayerFields(L"testrival", L"TestRival") + ", \"detected_by\": \"respawn_hook\", \"test\": true");
        mik::Emit("grand_prix_finished", "\"races\": 3, \"standings\": [{\"position\": 1, " + PlayerFields(L"testviewer", L"TestViewer") +
                  ", \"points\": 900, \"wins\": 2, \"eliminations\": 0, \"damage\": 0}], \"test\": true");
    }

    // Keep this DLL mapped until the process exits. UE4SS FreeLibrary()s a
    // C++ mod at shutdown (and on hot reload), but this mod leaves code running
    // that can outlive the instance: detached sender threads that may sit in a
    // WinHTTP timeout, plus UFunction hooks and a ProcessEvent callback that
    // UE4SS keeps calling. Unmapping the DLL under any of them would crash the
    // game on exit. Pinned, they always have code to run, and the process tear-
    // down ends the threads.
    static void PinModule()
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&GetModDirectory), &self))
            Output::send<LogLevel::Warning>(STR("[MikMarble] could not pin module (error {})\n"), GetLastError());
    }

    void StartSenders()
    {
        { std::lock_guard<std::mutex> lk(mik_bus::g_mtx); mik_bus::g_shared = m_state; }
        std::vector<std::string> warnings = m_config.warnings;
        std::vector<Target> targets = BuildTargets(m_config, warnings);
        m_configWarnings = warnings;

        std::vector<std::shared_ptr<SenderState>> senders;
        for (auto& t : targets)
        {
            auto s = std::make_shared<SenderState>();
            s->target = t;
            s->shared = m_state;
            try { std::thread(SenderLoop, s).detach(); }
            catch (...)
            {
                Output::send<LogLevel::Error>(STR("[MikMarble] could not start sender thread for {}\n"),
                                              std::wstring(t.label.begin(), t.label.end()));
                continue;
            }
            senders.push_back(std::move(s));
        }
        std::lock_guard<std::mutex> lk(mik_bus::g_mtx);
        mik_bus::g_senders = std::move(senders);
    }

    std::vector<std::string> m_configWarnings;

    ~MikMarbleMod() override
    {
        // The base destructor removes the tab only after our members (m_state,
        // m_configMutex) are gone; the GUI thread could render it in between.
        // Remove it first.
        for (const auto& tab : GUITabs)
            if (tab) UE4SSProgram::get_program().remove_gui_tab(tab);
        GUITabs.clear();

        // Let each sender flush what it has (bounded), then stop. The threads
        // hold their own shared_ptr to SenderState, so this is safe even if
        // one is stuck in a 5 s WinHTTP timeout.
        std::vector<std::shared_ptr<SenderState>> senders;
        {
            std::lock_guard<std::mutex> lk(mik_bus::g_mtx);
            senders.swap(mik_bus::g_senders);
        }
        for (auto& s : senders)
        {
            { std::lock_guard<std::mutex> lk(s->m); s->stop = true; }
            s->cv.notify_all();
        }
        for (int i = 0; i < 100; ++i)
        {
            bool all = true;
            for (auto& s : senders) if (!s->exited.load()) { all = false; break; }
            if (all) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        Output::send<LogLevel::Default>(STR("[MikMarble] Mod unloaded\n"));
    }

    auto on_ui_init() -> void override { UE4SS_ENABLE_IMGUI(); }

    auto on_unreal_init() -> void override
    {
        std::wstring wHost(m_config.host.begin(), m_config.host.end());
        std::wstring wPath(m_config.path.begin(), m_config.path.end());
        // Version comes from ModVersion so this line can't drift out of sync --
        // it is the string the install instructions tell users to check for.
        Output::send<LogLevel::Default>(STR("[MikMarble] Mod initialized (C++ v{}) -> {}:{}{}\n"),
                                        ModVersion, wHost, m_config.port, wPath);
        for (auto& s : mik_bus::Snapshot())
        {
            std::string line = s->target.id + " = " + s->target.label + " (results " +
                               (s->target.sendResults ? "on" : "off") + "; events: " + DescribeEvents(s->target) + ")";
            Output::send<LogLevel::Default>(STR("[MikMarble] target: {}\n"), std::wstring(line.begin(), line.end()));
        }
        for (const auto& w : m_configWarnings)
            Output::send<LogLevel::Warning>(STR("[MikMarble] config: {}\n"), std::wstring(w.begin(), w.end()));
        mik::g_trace.store(m_config.traceFunctions);
        if (m_config.traceFunctions)
            Output::send<LogLevel::Warning>(STR("[MikMarble] trace_functions is ON (discovery logging, noisy)\n"));
        if (!m_config.ignoredNames.empty())
        {
            // Echo the loaded list so a misspelled or mis-cased entry is
            // obvious without having to play a whole match to find out.
            std::wstring names;
            for (const auto& n : m_config.ignoredNames)
            {
                if (!names.empty()) names += L", ";
                names += std::wstring(n.begin(), n.end());
            }
            Output::send<LogLevel::Default>(STR("[MikMarble] ignore list: {} name(s) loaded -> {}\n"),
                                            m_config.ignoredNames.size(), names);
        }
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
