#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/FText.hpp>
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
#include <cmath>
#include <memory>
#include <deque>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

using namespace RC;
using namespace RC::Unreal;

// ── Config ─────────────────────────────────────────────────────────────────

struct ModConfig
{
    std::string host = "127.0.0.1";
    int port = 5000;
    std::string path = "/winners";
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
            out << "path=/winners\n";
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
            else if (key == "port")
            {
                try { cfg.port = std::stoi(val); }
                catch (...) { /* keep default */ }
            }
            else if (key == "path") cfg.path = val;
        }
    }
    catch (...)
    {
        // Any filesystem/IO/STL exception during load → fall back to defaults.
    }
    return cfg;
}

// ── Types ──────────────────────────────────────────────────────────────────

struct RaceEntry
{
    int position;
    std::wstring name;
    bool isDnf;
    int points;
    int eliminations;
    int damage;
};

struct PlayerResult
{
    std::wstring name;
    bool finished;
    int eliminations;
    int damage;
};

// ── Safe raw-memory readers (SEH-protected) ────────────────────────────────
//
// We need to peek at the FRaceResult struct stored inside UPlayerRaceResultEntryDataObject.
// The data object is opaque to UE4SS reflection, so we read raw bytes. To avoid crashes
// on stale/freed UObjects (FindAllOf returns lots of those), every dereference goes
// through SafeReadBytes which catches access violations.

#include <excpt.h>

__declspec(noinline) static bool SafeReadBytes(const void* src, void* dst, size_t bytes)
{
    __try
    {
        memcpy(dst, src, bytes);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// SEH-protected vtable call: GetClassPrivate() can crash if obj is a stale GC'd
// UObject (the vtable pointer is freed/reused memory). We protect just the
// vtable dereference; the returned UClass* (if non-null) is generally stable
// for the session since UE keeps UClass instances pinned.
__declspec(noinline) static UClass* SafeGetClassPrivate(UObject* obj) noexcept
{
    UClass* cls = nullptr;
    __try { cls = obj->GetClassPrivate(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { cls = nullptr; }
    return cls;
}

// Inner helper — has the wstring with destructor. Wraps the wstring construction
// in its own try/catch so any C++ exception from GetName()/allocation can't
// propagate up through the noexcept boundary into std::terminate.
static bool CopyClassNameImpl(UClass* cls, wchar_t* out, int outCap) noexcept
{
    try
    {
        std::wstring s = cls->GetName();
        int n = static_cast<int>(s.size());
        if (n > outCap - 1) n = outCap - 1;
        for (int i = 0; i < n; ++i) out[i] = s[i];
        out[n] = 0;
        return true;
    }
    catch (...)
    {
        if (outCap > 0) out[0] = 0;
        return false;
    }
}

__declspec(noinline) static bool SafeClassName(UClass* cls, wchar_t* out, int outCap) noexcept
{
    bool ok = false;
    __try { ok = CopyClassNameImpl(cls, out, outCap); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (!ok && outCap > 0) out[0] = 0;
    return ok;
}

struct FStringRaw
{
    wchar_t* Data;
    int32_t  Num;   // includes null terminator
    int32_t  Max;
};

struct DataObjResult
{
    int32_t      position;
    std::wstring username;
    bool         isEliminated;
    int32_t      eliminationOrder;
    int32_t      eliminations;
    int32_t      damage;
};

// Reads + validates an FString sitting at `src`. Returns false on bad memory or
// junk data. Validates: pointer non-null, Num in 2..64, Max >= Num, last char is
// null, all other chars are printable ASCII. The final assign is guarded by a
// C++ try/catch so a bad_alloc can't kill the polling tick.
static bool SafeReadFString(const void* src, std::wstring& out)
{
    FStringRaw fs{};
    if (!SafeReadBytes(src, &fs, sizeof(fs))) return false;
    if (!fs.Data || fs.Num <= 1 || fs.Num > 64 || fs.Max < fs.Num) return false;

    wchar_t buf[64]{};
    int32_t bytes = fs.Num * static_cast<int32_t>(sizeof(wchar_t));
    if (!SafeReadBytes(fs.Data, buf, bytes)) return false;
    if (buf[fs.Num - 1] != 0) return false;
    for (int i = 0; i < fs.Num - 1; ++i)
    {
        wchar_t c = buf[i];
        if (c < 0x20 || c > 0x7E) return false;
    }
    try { out.assign(buf, buf + (fs.Num - 1)); }
    catch (...) { return false; }
    return true;
}

// Try parsing FRaceResult starting at `base + fr`. Validates ranges on bool/int fields.
static bool TryReadDataObjAt(uint8_t* base, size_t fr, DataObjResult& out)
{
    if (!SafeReadFString(base + fr + 0x20, out.username)) return false;

    int32_t pos = 0, eo = 0, elim = 0, dmg = 0;
    uint8_t dnf = 0;
    if (!SafeReadBytes(base + fr + 0x00, &pos,  sizeof(pos))) return false;
    if (!SafeReadBytes(base + fr + 0x68, &dnf,  sizeof(dnf))) return false;
    if (!SafeReadBytes(base + fr + 0x6C, &eo,   sizeof(eo)))  return false;
    if (!SafeReadBytes(base + fr + 0x70, &elim, sizeof(elim))) return false;
    if (!SafeReadBytes(base + fr + 0x9C, &dmg,  sizeof(dmg))) return false;

    if (dnf > 1) return false;
    if (elim < 0 || elim > 200) return false;
    if (dmg  < 0 || dmg  > 1000000) return false;

    out.position         = pos;
    out.isEliminated     = dnf != 0;
    out.eliminationOrder = eo;
    out.eliminations     = elim;
    out.damage           = dmg;
    return true;
}


// ── HTTP POST (runs on background thread) ──────────────────────────────────
//
// Crash-hardening notes:
//   - All exceptions are swallowed (noexcept). Nothing is allowed to escape
//     into the std::thread runtime, where it would call std::terminate.
//   - No Output::send calls in here — UE4SS log writes are not safe to call
//     from a non-game thread. Status is returned in HttpResult and queued for
//     the game thread to log.
//   - Explicit timeouts so a slow/unreachable server can't keep the thread
//     alive past mod unload.

struct HttpResult
{
    bool ok = false;
    int  statusCode = 0;
    DWORD lastError = 0;
    std::string label;  // human-readable endpoint label for the log
};

static HttpResult HttpPostJson(const std::string& host, int port,
                                const std::string& path, const std::string& jsonBody) noexcept
{
    HttpResult r;
    r.label = path;
    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hRequest = nullptr;

    try
    {
        hSession = WinHttpOpen(L"MikMarbleMod/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
        if (!hSession) { r.lastError = GetLastError(); goto done; }

        // resolve / connect / send / receive timeouts in milliseconds
        WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

        std::wstring wHost(host.begin(), host.end());
        hConnect = WinHttpConnect(hSession, wHost.c_str(), static_cast<INTERNET_PORT>(port), 0);
        if (!hConnect) { r.lastError = GetLastError(); goto done; }

        std::wstring wPath(path.begin(), path.end());
        hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!hRequest) { r.lastError = GetLastError(); goto done; }

        const wchar_t* headers = L"Content-Type: application/json\r\n";
        BOOL ok = WinHttpSendRequest(hRequest, headers, static_cast<DWORD>(-1),
                                      const_cast<char*>(jsonBody.c_str()), static_cast<DWORD>(jsonBody.size()),
                                      static_cast<DWORD>(jsonBody.size()), 0);
        if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);

        if (ok)
        {
            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
            r.ok = true;
            r.statusCode = static_cast<int>(statusCode);
        }
        else
        {
            r.lastError = GetLastError();
        }
    }
    catch (...) { /* swallow */ }

done:
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return r;
}

// ── JSON helpers ───────────────────────────────────────────────────────────

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

static std::string BuildResultsJson(const std::vector<PlayerResult>& players,
                                     const std::string& gameType)
{
    std::string json = R"({"type": ")" + EscapeJsonString(gameType) + R"(", "players": [)";
    for (size_t i = 0; i < players.size(); ++i)
    {
        if (i > 0) json += ", ";
        json += R"({"name": ")" + EscapeJsonString(WideToUtf8(players[i].name)) +
                R"(", "finished": )" + (players[i].finished ? "true" : "false") +
                R"(, "eliminations": )" + std::to_string(players[i].eliminations) +
                R"(, "damage": )" + std::to_string(players[i].damage) + "}";
    }
    json += "]}";
    return json;
}

// ── Mod class ──────────────────────────────────────────────────────────────

// Shared state held by HTTP worker threads. Captured by std::shared_ptr so the
// worker is decoupled from the MikMarbleMod instance lifetime — if UE4SS unloads
// the mod while a request is in flight, the worker writes into shared state
// instead of dangling 'this'.
struct HttpSharedState
{
    std::mutex mtx;
    std::vector<PlayerResult> lastSentPlayers;
    std::string lastGameType;
    std::deque<HttpResult> pendingLogs;  // drained by on_update on game thread
    std::atomic<bool> httpBusy{false};
    std::atomic<int>  inflight{0};       // count of running workers
};

class MikMarbleMod : public RC::CppUserModBase
{
    std::shared_ptr<HttpSharedState> m_state = std::make_shared<HttpSharedState>();
    std::mutex m_configMutex;             // guards m_config reads from UI thread
    float m_pollTimer = 0.0f;
    ModConfig m_config;
    std::atomic<bool> m_unrealReady{false};
    std::atomic<bool> m_readySent{false};

    // True after we've sent a result for the currently-open result widget.
    // Reset to false when entries.empty() (result widget closed). Prevents
    // re-sending when the result widget switches tabs / views and the
    // visible widgets flip flags (e.g. royale "view by eliminations" tab
    // makes everyone look DNF).
    bool m_sentForCurrentMatch = false;

    // Offsets within UPlayerRaceResultEntryDataObject, discovered at runtime by
    // cross-referencing widget truth against listview items. -1 = not yet discovered.
    // No bEliminated flag — DNF is derived from Position (0 / out-of-range = DNF,
    // matching the in-game "Place" column which renders a number or "DNF").
    int m_posOff   = -1;
    int m_elimOff  = -1;
    int m_dmgOff   = -1;
    int m_timeOff  = -1;  // FinishTime float — finishers > 0, DNFs == 0
    int m_dnfOff   = -1;  // bEliminated-like int — finishers == 0, DNFs == non-zero (same value)
    int m_finOff   = -1;  // Finisher flag (inverted) — finishers == 1 or 2 (winner=2), DNFs == 0
    int m_elimOrderOff = -1;  // EliminationOrder int — finishers==0, DNFs have distinct 1..N

    // v3.9: count consecutive empty-entry polls before resetting m_sentForCurrentMatch.
    // Single-poll dips (e.g. UMG list virtualizing widgets out during a scroll, or
    // mid-frame transitions) shouldn't trigger re-sends. Reset only after 5+ polls
    // (~5 seconds) confirm the result widget is genuinely gone.
    int m_consecutiveEmptyPolls = 0;

    // Sticky DNF cache: names that we've ever seen marked DNF in the visible widgets
    // during the current match. Used so off-screen reads remember a player's DNF
    // status even after they scroll out of view (UListView virtualization). Cleared
    // between matches when m_lastSentPlayers resets.
    std::set<std::wstring> m_knownDnfNames;

    // One-shot diagnostic dump for target/Bullseye races. Set true after the first
    // successful dump in a given target match. Reset when the result widget closes
    // (entries empty). Used to capture raw data-object bytes so we can locate the
    // distance/score field that drives the on-screen Bullseye ranking.
    bool m_targetDumped = false;

    // Cross-match stale-listview rejection. Sorted (name, position) pairs from the
    // last successfully-sent result. Persists across matches (NOT cleared on widget
    // close) so we can reject any future candidate listview whose entries match this
    // signature exactly — two consecutive races having identical (name, position)
    // pairs across all entries is essentially impossible in practice; if we see it,
    // we picked a stale listview from the previous match.
    std::vector<std::pair<std::wstring, int>> m_lastRaceSignature;

    static constexpr float POLL_INTERVAL = 1.0f; // seconds

    // ── Read race results from UMG widgets ─────────────────────────────────

    // Inner helper for reading a single widget's text-block as digits, capped to
    // avoid signed overflow on long digit strings (UB).
    static int ParseDigits(const std::wstring& s)
    {
        int value = 0;
        int digits = 0;
        for (wchar_t ch : s)
        {
            if (ch < L'0' || ch > L'9') continue;
            if (++digits > 9) break;  // cap at 9 digits → max 999,999,999
            value = value * 10 + (ch - L'0');
        }
        return value;
    }

    // Inner: does the actual reflective reads. Wrapped in C++ try/catch to swallow
    // any std::* exception, then called from an SEH-guarded outer to swallow AVs
    // from stale/freed widgets that FindAllOf may hand back.
    bool ReadOneWidgetImpl(UObject* widget, RaceEntry& out, int& dnfCounter) noexcept
    {
        try
        {
            if (!widget) return false;

            // v3.0: visibility filter. UMG ListView widgets are pooled — recycled
            // entries can carry stale name/position text from a prior binding while
            // the listview swaps in fresh data. ESlateVisibility values: 0=Visible,
            // 1=Collapsed, 2=Hidden, 3=HitTestInvisible, 4=SelfHitTestInvisible.
            // Reject Collapsed/Hidden so stale pooled rows don't poison reads.
            auto* visPtr = widget->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Visibility"));
            if (visPtr)
            {
                uint8_t v = *visPtr;
                if (v == 1 || v == 2) return false;
            }

            auto** ppPosBlock = widget->GetValuePtrByPropertyNameInChain<UObject*>(STR("PlayerPositionTextBlock"));
            if (!ppPosBlock || !*ppPosBlock) return false;
            auto* posFText = (*ppPosBlock)->GetValuePtrByPropertyNameInChain<FText>(STR("Text"));
            if (!posFText) return false;
            std::wstring posStr = posFText->ToString();
            bool isDnf = posStr.find(STR("DNF")) != std::wstring::npos;

            int pos = 0;
            if (isDnf)
            {
                pos = 20000 - dnfCounter++;
            }
            else
            {
                pos = ParseDigits(posStr);
                if (pos == 0) return false;
            }

            auto** ppNameBlock = widget->GetValuePtrByPropertyNameInChain<UObject*>(STR("PlayerNameTextBlock"));
            if (!ppNameBlock || !*ppNameBlock) return false;
            auto* nameFText = (*ppNameBlock)->GetValuePtrByPropertyNameInChain<FText>(STR("Text"));
            if (!nameFText) return false;
            std::wstring name = nameFText->ToString();
            name.erase(std::remove_if(name.begin(), name.end(),
                [](wchar_t c) { return c == L'\r' || c == L'\n'; }), name.end());
            if (name.empty()) return false;

            auto readIntBlock = [&](const wchar_t* propName) -> int {
                auto** ppBlock = widget->GetValuePtrByPropertyNameInChain<UObject*>(propName);
                if (!ppBlock || !*ppBlock) return 0;
                auto* fText = (*ppBlock)->GetValuePtrByPropertyNameInChain<FText>(STR("Text"));
                if (!fText) return 0;
                return ParseDigits(fText->ToString());
            };

            int points       = readIntBlock(STR("PointsTextBlock"));
            int eliminations = readIntBlock(STR("EliminationsTextBlock"));
            int damage       = readIntBlock(STR("DamageDealtTextBlock"));

            out = RaceEntry{pos, name, isDnf, points, eliminations, damage};
            return true;
        }
        catch (...) { return false; }
    }

    __declspec(noinline) bool SafeReadOneWidget(UObject* widget, RaceEntry& out, int& dnfCounter) noexcept
    {
        bool ok = false;
        __try { ok = ReadOneWidgetImpl(widget, out, dnfCounter); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        return ok;
    }

    auto ReadVisibleWidgets(const std::vector<RC::Unreal::UObject*>& widgets) -> std::vector<RaceEntry>
    {
        std::vector<RaceEntry> entries;
        // v3.0: dedup by name. FindAllOf returns ALL widgets of the class, including
        // pooled-but-invisible ones that may carry stale name/position/stats from a
        // prior match. Two widgets reading the same name = one is stale; keep the
        // first (entries are read in FindAllOf order; the freshest are typically
        // returned first by UE's iteration, but we prefer the one with non-zero
        // stats if both are seen).
        std::set<std::wstring> seenNames;
        int dnfCounter = 0;
        for (UObject* widget : widgets)
        {
            RaceEntry e;
            if (!SafeReadOneWidget(widget, e, dnfCounter)) continue;
            auto inserted = seenNames.insert(e.name);
            if (!inserted.second)
            {
                // Already seen; if existing one is all-zero phantom and this one isn't,
                // upgrade by replacing the existing entry.
                bool existingPhantom = false;
                size_t existingIdx = 0;
                for (size_t i = 0; i < entries.size(); ++i)
                {
                    if (entries[i].name == e.name)
                    {
                        existingIdx = i;
                        existingPhantom = (entries[i].points == 0 && entries[i].eliminations == 0 && entries[i].damage == 0);
                        break;
                    }
                }
                bool incomingPhantom = (e.points == 0 && e.eliminations == 0 && e.damage == 0);
                if (existingPhantom && !incomingPhantom)
                {
                    entries[existingIdx] = std::move(e);
                }
                Output::send<LogLevel::Warning>(STR("[MikMarble] dedup: duplicate widget name={} (kept {})\n"),
                    entries[existingIdx].name, existingPhantom && !incomingPhantom ? STR("incoming") : STR("existing"));
                continue;
            }
            entries.push_back(std::move(e));
        }
        return entries;
    }

    // For each 4-byte offset in the data object, check if int32 there equals the
    // expected value across ALL given (data-obj, expected) pairs. Returns the
    // unique offset if exactly one matches, else -1. Requires at least one pair
    // with a non-zero expected value (so we have discrimination).
    // Discover a float-valued offset whose semantics are: > 1.0 for finishers, == 0 for
    // DNFs. Maps to FinishTime in FRaceResult. Requires at least 2 finishers with varying
    // values (so discovery has discrimination). DNFs must read 0 if any are matched.
    // Returns -1 if no unique offset matches.
    static int DiscoverFinishTimeOffset(const std::vector<std::pair<RC::Unreal::UObject*, RaceEntry>>& matched)
    {
        int finisherCount = 0;
        for (const auto& m : matched) if (!m.second.isDnf) ++finisherCount;
        if (finisherCount < 2) return -1;

        std::vector<int> hits;
        for (size_t off = 0; off + 4 <= 0x120; off += 4)
        {
            bool ok = true;
            // Collect (position, time) for finishers; verify DNFs read 0.
            std::vector<std::pair<int, float>> posTimes;
            for (const auto& m : matched)
            {
                if (!m.first) { ok = false; break; }
                float val = 0.0f;
                if (!SafeReadBytes(reinterpret_cast<uint8_t*>(m.first) + off, &val, sizeof(val)))
                {
                    ok = false; break;
                }
                if (m.second.isDnf)
                {
                    if (!std::isfinite(val) || std::fabs(val) > 0.001f) { ok = false; break; }
                }
                else
                {
                    if (!std::isfinite(val) || val < 1.0f || val > 99999.0f) { ok = false; break; }
                    posTimes.push_back({m.second.position, val});
                }
            }
            if (!ok) continue;

            // Require strict variation across finishers.
            bool varies = false;
            for (size_t k = 1; k < posTimes.size(); ++k)
            {
                if (posTimes[k].second != posTimes[0].second) { varies = true; break; }
            }
            if (!varies) continue;

            // Disambiguator: FinishTime is monotonically increasing with Place
            // (1st is fastest, last is slowest). Distance/other fields don't satisfy this.
            std::sort(posTimes.begin(), posTimes.end(), [](auto& a, auto& b){ return a.first < b.first; });
            bool monotonic = true;
            for (size_t k = 1; k < posTimes.size(); ++k)
            {
                if (posTimes[k].second < posTimes[k-1].second) { monotonic = false; break; }
            }
            if (!monotonic) continue;

            hits.push_back(static_cast<int>(off));
        }
        return hits.size() == 1 ? hits[0] : -1;
    }

    // Find an int32 offset where finishers read exactly 0 and DNFs read the same
    // non-zero value (matches bEliminated bool: 0=finisher, 1=eliminated). Requires
    // BOTH at least 1 finisher and 1 DNF in matched widget data.
    static int DiscoverEliminatedFlagOffset(const std::vector<std::pair<RC::Unreal::UObject*, RaceEntry>>& matched)
    {
        int finisherCount = 0, dnfCount = 0;
        for (const auto& m : matched) {
            if (m.second.isDnf) ++dnfCount;
            else ++finisherCount;
        }
        if (finisherCount < 1 || dnfCount < 1) return -1;

        // Pass 1: strict bool — finishers exactly 0, DNFs exactly 1.
        std::vector<int> strictHits;
        // Pass 2: relaxed — finishers 0, DNFs all same non-zero value.
        std::vector<int> relaxedHits;
        for (size_t off = 0; off + 4 <= 0x120; off += 4)
        {
            bool strictOk = true;
            bool relaxedOk = true;
            int32_t dnfVal = 0;
            bool dnfValSet = false;
            for (const auto& m : matched)
            {
                if (!m.first) { strictOk = relaxedOk = false; break; }
                int32_t val = 0;
                if (!SafeReadBytes(reinterpret_cast<uint8_t*>(m.first) + off, &val, sizeof(val)))
                {
                    strictOk = relaxedOk = false; break;
                }
                if (m.second.isDnf)
                {
                    if (val == 0) { strictOk = relaxedOk = false; break; }
                    if (val != 1) strictOk = false;
                    if (!dnfValSet) { dnfVal = val; dnfValSet = true; }
                    else if (val != dnfVal) relaxedOk = false;
                }
                else
                {
                    if (val != 0) { strictOk = relaxedOk = false; break; }
                }
            }
            if (dnfValSet)
            {
                if (strictOk)  strictHits.push_back(static_cast<int>(off));
                if (relaxedOk) relaxedHits.push_back(static_cast<int>(off));
            }
        }
        // Prefer strict (bool 0/1). If exactly one strict hit, use it. Else if exactly
        // one relaxed hit, use that. Else give up.
        if (strictHits.size()  == 1) return strictHits[0];
        if (relaxedHits.size() == 1) return relaxedHits[0];
        return -1;
    }

    // Finisher flag discovery — INVERSE of bEliminated. The Marbles data object
    // stores something like "race result state" at one offset:
    //   0 = DNF, 1 = finished, 2 = winner.
    // We scan the listview for an offset whose values match the pattern
    // [non-zero, …, non-zero, 0, …, 0] (finishers first, then DNFs), with values
    // in [0, 5]. Cross-validates against any visible widgets too.
    static int DiscoverFinisherFlagOffsetByListview(
        UObject** items, int32_t count,
        const std::vector<std::pair<RC::Unreal::UObject*, RaceEntry>>& matched)
    {
        if (count < 4 || !items) return -1;

        std::vector<int> hits;
        for (size_t off = 0; off + 4 <= 0x120; off += 4)
        {
            std::vector<int32_t> vals;
            vals.reserve(count);
            bool ok = true;
            for (int32_t i = 0; i < count; ++i)
            {
                UObject* p = nullptr;
                if (!SafeReadBytes(&items[i], &p, sizeof(p)) || !p) { ok = false; break; }
                int32_t v = 0;
                if (!SafeReadBytes(reinterpret_cast<uint8_t*>(p) + off, &v, sizeof(v)))
                {
                    ok = false; break;
                }
                if (v < 0 || v > 5) { ok = false; break; }
                vals.push_back(v);
            }
            if (!ok || (int32_t)vals.size() != count) continue;

            // Find boundary K: index of first 0. Require 1 <= K < count.
            int K = -1;
            for (size_t i = 0; i < vals.size(); ++i)
                if (vals[i] == 0) { K = (int)i; break; }
            if (K <= 0 || K >= count) continue;

            // All values from K..end must be 0.
            bool tail_zero = true;
            for (size_t i = (size_t)K; i < vals.size(); ++i)
                if (vals[i] != 0) { tail_zero = false; break; }
            if (!tail_zero) continue;

            // Cross-validate with visible widgets: every matched widget's data
            // object must have value!=0 if widget is finisher, value==0 if DNF.
            bool widget_ok = true;
            for (const auto& m : matched)
            {
                if (!m.first) continue;
                int32_t v = 0;
                if (!SafeReadBytes(reinterpret_cast<uint8_t*>(m.first) + off, &v, sizeof(v)))
                {
                    widget_ok = false; break;
                }
                if (m.second.isDnf ? (v != 0) : (v == 0)) { widget_ok = false; break; }
            }
            if (!widget_ok) continue;

            hits.push_back((int)off);
        }
        return hits.size() == 1 ? hits[0] : -1;
    }

    // Unsupervised bEliminated discovery: doesn't need any visible DNF widget.
    // Scans the listview for an offset where the int32 field follows the pattern
    // [0,0,...,0,X,X,...,X] — finishers first (value 0), then DNFs (same non-zero
    // value). The Marbles result widget always sorts that way, so this signature
    // is unique to bEliminated. Useful when the visible widgets are all finishers
    // (e.g. target race with DNFs scrolled below the visible window).
    static int DiscoverEliminatedFlagOffsetByListview(
        UObject** items, int32_t count,
        const std::vector<std::pair<RC::Unreal::UObject*, RaceEntry>>& matched)
    {
        if (count < 4 || !items) return -1;

        std::vector<int> hits;
        for (size_t off = 0; off + 4 <= 0x120; off += 4)
        {
            std::vector<int32_t> vals;
            vals.reserve(count);
            bool ok = true;
            for (int32_t i = 0; i < count; ++i)
            {
                UObject* p = nullptr;
                if (!SafeReadBytes(&items[i], &p, sizeof(p)) || !p) { ok = false; break; }
                int32_t v = 0;
                if (!SafeReadBytes(reinterpret_cast<uint8_t*>(p) + off, &v, sizeof(v)))
                {
                    ok = false; break;
                }
                vals.push_back(v);
            }
            if (!ok || (int32_t)vals.size() != count) continue;

            // Find boundary K: index of first non-zero. Require 1 <= K < count.
            int K = -1;
            for (size_t i = 0; i < vals.size(); ++i)
                if (vals[i] != 0) { K = (int)i; break; }
            if (K <= 0 || K >= count) continue;

            // All values from K..end must equal the same non-zero value.
            int32_t X = vals[K];
            bool tail_uniform = true;
            for (size_t i = (size_t)K; i < vals.size(); ++i)
                if (vals[i] != X) { tail_uniform = false; break; }
            if (!tail_uniform) continue;

            // Cross-validate with visible widgets: every matched widget's data
            // object must have value=0 if widget is finisher, value=X if DNF.
            bool widget_ok = true;
            for (const auto& m : matched)
            {
                if (!m.first) continue;
                int32_t v = 0;
                if (!SafeReadBytes(reinterpret_cast<uint8_t*>(m.first) + off, &v, sizeof(v)))
                {
                    widget_ok = false; break;
                }
                if (m.second.isDnf ? (v != X) : (v != 0)) { widget_ok = false; break; }
            }
            if (!widget_ok) continue;

            hits.push_back((int)off);
        }
        return hits.size() == 1 ? hits[0] : -1;
    }

    static int DiscoverOffset(const std::vector<std::pair<RC::Unreal::UObject*, int>>& pairs)
    {
        if (pairs.size() < 3) return -1;
        bool hasNonZero = false;
        for (const auto& p : pairs)
            if (p.second != 0) { hasNonZero = true; break; }
        if (!hasNonZero) return -1;

        std::vector<int> hits;
        for (size_t off = 0; off + 4 <= 0x120; off += 4)
        {
            bool allMatch = true;
            for (const auto& p : pairs)
            {
                if (!p.first) { allMatch = false; break; }
                int32_t val = 0;
                if (!SafeReadBytes(reinterpret_cast<uint8_t*>(p.first) + off, &val, sizeof(val)))
                {
                    allMatch = false;
                    break;
                }
                if (val != p.second) { allMatch = false; break; }
            }
            if (allMatch) hits.push_back(static_cast<int>(off));
        }
        if (hits.size() == 1) return hits[0];
        return -1;
    }

    // One-shot diagnostic dump: writes widget readings, every visible ListView's
    // class/count, and a 0x120-byte hex dump of the first ~8 ResultEntry data
    // objects. Used to identify the distance/score offset for target/Bullseye
    // mode where the displayed rank doesn't match `Place`.
    void DumpResultDiagnostic(const std::vector<RaceEntry>& widgets, const char* tag)
    {
        std::wstring wTag(tag, tag + std::char_traits<char>::length(tag));
        Output::send<LogLevel::Default>(STR("[MikMarble] === DIAG START ({}) ===\n"), wTag);
        Output::send<LogLevel::Default>(STR("[MikMarble] DIAG widget count={}\n"), widgets.size());
        for (size_t i = 0; i < widgets.size(); ++i)
        {
            const auto& w = widgets[i];
            Output::send<LogLevel::Default>(STR("[MikMarble] DIAG   w[{}] pos={} dnf={} pts={} elim={} dmg={} name={}\n"),
                i, w.position, w.isDnf ? 1 : 0, w.points, w.eliminations, w.damage, w.name);
        }

        struct TArrayRaw { void* Data; int32_t Num; int32_t Max; };
        std::vector<UObject*> listViews;
        UObjectGlobals::FindAllOf(STR("ListView"), listViews);
        Output::send<LogLevel::Default>(STR("[MikMarble] DIAG listview count={}\n"), listViews.size());

        int lvIdx = 0;
        int dumpedLvs = 0;
        for (UObject* lv : listViews)
        {
            int thisIdx = lvIdx++;
            if (!lv) continue;
            TArrayRaw arr{};
            if (!SafeReadBytes(reinterpret_cast<uint8_t*>(lv) + 0xBD8, &arr, sizeof(arr))) continue;
            if (!arr.Data || arr.Num <= 0 || arr.Num > 1000) continue;

            auto** items = static_cast<UObject**>(arr.Data);
            UObject* probe = nullptr;
            if (!SafeReadBytes(&items[0], &probe, sizeof(probe)) || !probe) continue;
            UClass* probeCls = SafeGetClassPrivate(probe);
            wchar_t clsName[128] = {0};
            SafeClassName(probeCls, clsName, 128);

            Output::send<LogLevel::Default>(STR("[MikMarble] DIAG   LV[{}] num={} cls={}\n"),
                thisIdx, arr.Num, std::wstring(clsName));

            if (!wcsstr(clsName, L"ResultEntry")) continue;
            if (++dumpedLvs > 3) continue;

            int dumpLimit = arr.Num < 8 ? arr.Num : 8;
            for (int i = 0; i < dumpLimit; ++i)
            {
                UObject* obj = nullptr;
                if (!SafeReadBytes(&items[i], &obj, sizeof(obj)) || !obj) continue;
                std::wstring user, disp;
                auto* base = reinterpret_cast<uint8_t*>(obj);
                SafeReadFString(base + 0x28, user);
                SafeReadFString(base + 0x38, disp);
                Output::send<LogLevel::Default>(STR("[MikMarble] DIAG     item[{}] disp={} user={}\n"),
                    i, disp, user);

                uint8_t bytes[0x120] = {0};
                if (!SafeReadBytes(base, bytes, sizeof(bytes))) continue;

                for (int row = 0; row < 0x120; row += 16)
                {
                    Output::send<LogLevel::Default>(STR(
                        "[MikMarble] DIAG     +{:#06x}: "
                        "{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}  "
                        "{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}\n"),
                        row,
                        bytes[row+0], bytes[row+1], bytes[row+2], bytes[row+3],
                        bytes[row+4], bytes[row+5], bytes[row+6], bytes[row+7],
                        bytes[row+8], bytes[row+9], bytes[row+10], bytes[row+11],
                        bytes[row+12], bytes[row+13], bytes[row+14], bytes[row+15]);
                }

                Output::send<LogLevel::Default>(STR("[MikMarble] DIAG     -- nonzero int/float candidates --\n"));
                for (int off = 0; off + 4 <= 0x120; off += 4)
                {
                    int32_t v = 0;
                    float f = 0.0f;
                    SafeReadBytes(base + off, &v, 4);
                    SafeReadBytes(base + off, &f, 4);
                    bool intish = (v != 0 && v > -1000000 && v < 1000000);
                    bool floatish = (std::isfinite(f) && f != 0.0f &&
                                     std::fabs(f) > 1e-4f && std::fabs(f) < 1e6f);
                    if (intish || floatish)
                    {
                        Output::send<LogLevel::Default>(STR("[MikMarble] DIAG     +{:#06x}: int={} float={}\n"),
                            off, v, f);
                    }
                }
            }
        }
        Output::send<LogLevel::Default>(STR("[MikMarble] === DIAG END ===\n"));
    }

    auto GetCurrentRaceEntries() -> std::vector<RaceEntry>
    {
        // Gate everything on the result widget being open. PlayerRaceResultEntryWidget
        // instances only exist while the post-match result screen is visible.
        std::vector<UObject*> resultWidgets;
        UObjectGlobals::FindAllOf(STR("PlayerRaceResultEntryWidget"), resultWidgets);
        if (resultWidgets.empty()) return {};

        // Read widget ground-truth for the visible ~24 rows up-front. We use these for
        // cross-validation: matching listview data objects by username, then scanning to
        // discover where eliminations/damage/position live in the data object.
        auto widgetEntries = ReadVisibleWidgets(resultWidgets);

        // v3.2: Detect game type now (used by target-mode anchored handling below).
        // Target mode no longer takes the widget-only early return — that capped at
        // ~24 visible widgets, missing players in larger races. Instead it goes
        // through the same anchored listview path as race mode but uses the
        // listview's array index as the on-screen position (the listview is sorted
        // by distance for target display, so items[0] = closest = #1).
        auto gameType = DetectGameType();

        // One-shot diagnostic dump for target mode (still useful for confirming
        // distance offset etc.). Fires once per match; reset on widget close.
        if (gameType == "target" && !m_targetDumped && !widgetEntries.empty())
        {
            DumpResultDiagnostic(widgetEntries, "target");
            m_targetDumped = true;
        }

        // Update sticky DNF cache from any visible DNF widgets so off-screen reads can
        // still classify them correctly after they scroll out of view.
        for (const auto& w : widgetEntries)
        {
            if (w.isDnf) m_knownDnfNames.insert(w.name);
        }

        // v3.8: DIRECT-DATA-OBJECT WITH PLAYERARRAY NAME FILTER. v3.7 picked by
        // InternalIndex which proved unreliable — UE recycles index slots when
        // GC frees old objects, so a NEW data object can get a LOWER index than
        // a still-alive prior-race object. The fix: filter data objects to only
        // those whose name is in the current GameState.PlayerArray. Random bot
        // names don't overlap across races, so this cleanly isolates the current
        // match's data objects from any prior race's leftover ones.
        struct TArrayRaw { void* Data; int32_t Num; int32_t Max; };

        std::vector<UObject*> bestObjs;  // owns the picked batch's pointers
        UObject** bestItems = nullptr;
        int32_t bestCount = 0;

        {
            // Build the set of current-match player names from GameState.PlayerArray.
            std::set<std::wstring> currentPlayerNames;
            UObject* gs = UObjectGlobals::FindFirstOf(STR("MarbleRaceGameState"));
            if (!gs) gs = UObjectGlobals::FindFirstOf(STR("MarbleRoyaleGameState"));
            if (!gs) gs = UObjectGlobals::FindFirstOf(STR("BullseyeGameState"));
            if (!gs) gs = UObjectGlobals::FindFirstOf(STR("TiltedGameState"));
            if (!gs) gs = UObjectGlobals::FindFirstOf(STR("CMBGameState"));
            if (gs)
            {
                TArrayRaw pa{};
                if (SafeReadBytes(reinterpret_cast<uint8_t*>(gs) + 0x2A8, &pa, sizeof(pa))
                    && pa.Data && pa.Num > 0 && pa.Num < 1000)
                {
                    auto** psPtrs = static_cast<UObject**>(pa.Data);
                    for (int32_t i = 0; i < pa.Num; ++i)
                    {
                        UObject* ps = nullptr;
                        if (!SafeReadBytes(&psPtrs[i], &ps, sizeof(ps)) || !ps) continue;
                        std::wstring uname;
                        // AMOSPlayerState.Username at +0x380 (per CXXHeaderDump).
                        SafeReadFString(reinterpret_cast<uint8_t*>(ps) + 0x380, uname);
                        if (!uname.empty()) currentPlayerNames.insert(uname);
                    }
                }
            }

            std::vector<UObject*> allDataObjs;
            UObjectGlobals::FindAllOf(STR("PlayerRaceResultEntryDataObject"), allDataObjs);

            // Filter to data objects whose name matches a current player. If we
            // couldn't read PlayerArray, fall back to all data objects + take by
            // InternalIndex (v3.7 behavior).
            struct Cand { int32_t idx; UObject* obj; std::wstring name; };
            std::vector<Cand> candidates;
            for (UObject* o : allDataObjs)
            {
                if (!o) continue;
                std::wstring user, disp;
                auto* base = reinterpret_cast<uint8_t*>(o);
                SafeReadFString(base + 0x28, user);
                SafeReadFString(base + 0x38, disp);
                std::wstring name = !disp.empty() ? disp : user;
                if (name.empty()) continue;
                // Match: data object's username (or display) appears in current
                // PlayerArray's username set.
                if (!currentPlayerNames.empty() &&
                    currentPlayerNames.count(user) == 0 &&
                    currentPlayerNames.count(disp) == 0)
                {
                    continue;
                }
                int32_t idx = 0;
                try { idx = o->GetInternalIndex(); } catch (...) {}
                candidates.push_back({idx, o, name});
            }

            // Dedup by name: same name can have multiple data objects (e.g.
            // streamer's name + prior race's stale instance). Keep the one with
            // the highest InternalIndex per name.
            std::map<std::wstring, Cand> bestPerName;
            for (auto& c : candidates)
            {
                auto it = bestPerName.find(c.name);
                if (it == bestPerName.end() || c.idx > it->second.idx)
                    bestPerName[c.name] = c;
            }

            bestObjs.reserve(bestPerName.size());
            for (auto& kv : bestPerName) bestObjs.push_back(kv.second.obj);

            if (!bestObjs.empty())
            {
                bestItems = bestObjs.data();
                bestCount = static_cast<int32_t>(bestObjs.size());
                Output::send<LogLevel::Default>(STR("[MikMarble] data-obj source: {} unique current-player matches from {} total data-objs (PlayerArray={})\n"),
                    bestObjs.size(), allDataObjs.size(), currentPlayerNames.size());
            }
        }

        if (!bestItems)
        {
            // No data objects available yet — either the result widget hasn't
            // produced them yet, or game initialization hasn't created them.
            // Fall back to visible widgets if any.
            if (!widgetEntries.empty()) return widgetEntries;
            return {};
        }

        // v3.15b: discover Position offset (m_posOff) BEFORE target early-return.
        // The original v3.14 royale-pattern discovery sat inside the listview-
        // supervised path which target mode bypasses, leaving m_posOff=-1 and
        // forcing target mode to fall back to alphabetical iteration index.
        // Pattern: ALL bestCount values are positive, distinct, in [1, K] with
        // K close to bestCount. Works for both race-mode all-finishers and
        // target/Bullseye races.
        if (m_posOff < 0 && bestItems && bestCount >= 4)
        {
            std::vector<int> hits;
            for (int off = 0; off + 4 <= 0x120; off += 4)
            {
                if (off == m_elimOff || off == m_dmgOff) continue;
                std::set<int32_t> vals;
                bool ok = true;
                int total = 0;
                for (int32_t i = 0; i < bestCount; ++i)
                {
                    UObject* obj = nullptr;
                    if (!SafeReadBytes(&bestItems[i], &obj, sizeof(obj)) || !obj) continue;
                    int32_t v = 0;
                    if (!SafeReadBytes(reinterpret_cast<uint8_t*>(obj) + off, &v, 4)) { ok = false; break; }
                    ++total;
                    if (v <= 0 || v > 200) { ok = false; break; }
                    if (!vals.insert(v).second) { ok = false; break; }
                }
                if (!ok || total < 4) continue;
                if (*vals.begin() != 1) continue;
                if (*vals.rbegin() > total + 5) continue;
                hits.push_back(off);
            }
            if (hits.size() == 1)
            {
                m_posOff = hits[0];
                Output::send<LogLevel::Default>(STR("[MikMarble] discovered position offset 0x{:x} (early pattern-scan)\n"), m_posOff);
            }
        }

        // v3.2: Target/Bullseye special path — use the anchored listview's array
        // ORDER as on-screen rank. The listview is sorted by distance-to-bullseye
        // for target display, so items[0] = closest = #1. We don't need to read
        // Place from the data object (which holds listview-insertion order, not
        // distance rank), and we don't need to discover the Distance offset.
        // Stats come from the matched widget if visible, otherwise empty.
        if (gameType == "target")
        {
            auto readNamesT = [](UObject* obj) -> std::pair<std::wstring, std::wstring> {
                std::wstring user, disp;
                auto* base = reinterpret_cast<uint8_t*>(obj);
                SafeReadFString(base + 0x28, user);
                SafeReadFString(base + 0x38, disp);
                return {user, disp};
            };

            std::vector<RaceEntry> entries;
            std::set<std::wstring> seenNames;
            for (int32_t i = 0; i < bestCount; ++i)
            {
                UObject* obj = nullptr;
                if (!SafeReadBytes(&bestItems[i], &obj, sizeof(obj)) || !obj) continue;
                auto [user, disp] = readNamesT(obj);
                if (user.empty() && disp.empty()) continue;
                std::wstring name = !disp.empty() ? disp : user;

                if (!seenNames.insert(name).second) continue;

                // v3.15: target mode position = data object's Position field (rank by
                // distance-to-bullseye). v3.2-v3.14 used array iteration index which
                // became alphabetical-by-name after v3.8's dedup-by-name map iteration,
                // producing wrong winner. Use m_posOff (at 0xC8) discovered via
                // royale-pattern OR widget cross-validation.
                int32_t rank = 0;
                if (m_posOff >= 0)
                    SafeReadBytes(reinterpret_cast<uint8_t*>(obj) + m_posOff, &rank, sizeof(rank));

                RaceEntry e{};
                e.name = name;
                e.position = (rank > 0 && rank <= 1000) ? rank : (i + 1);
                e.isDnf = false;             // target: every player is a finisher
                e.points = 0;
                e.eliminations = 0;
                e.damage = 0;

                // Pull stats from widget if this player is visible.
                for (const auto& w : widgetEntries)
                {
                    if (w.name == name)
                    {
                        e.points = w.points;
                        e.eliminations = w.eliminations;
                        e.damage = w.damage;
                        break;
                    }
                }
                entries.push_back(std::move(e));
            }

            // v3.5: cross-match stale-listview rejection for target mode too. The
            // race-mode path has had this since v3.0, but target-mode early-returns
            // bypassed it. Stale data carryover across same-count target races (e.g.
            // 100-player race 2 reading race 1's data) needs this guard.
            if (!entries.empty() && !m_lastRaceSignature.empty())
            {
                std::vector<std::pair<std::wstring, int>> sig;
                sig.reserve(entries.size());
                for (const auto& e : entries) sig.push_back({e.name, e.position});
                std::sort(sig.begin(), sig.end());
                if (sig == m_lastRaceSignature)
                {
                    Output::send<LogLevel::Warning>(STR("[MikMarble] cross-match (target): listview matches previous race signature exactly; treating as stale carryover\n"));
                    return {};
                }
            }
            // v3.15: sort by Position (rank). With m_posOff-based ranks, entries
            // are no longer in array-iteration order so an explicit sort is needed
            // to put winner (rank 1) first in the sent payload.
            std::sort(entries.begin(), entries.end(),
                      [](const RaceEntry& a, const RaceEntry& b) { return a.position < b.position; });
            return entries;
        }

        // Cross-validation path: match listview data objects to widgets by username,
        // then discover stat offsets in the data object using widget values as ground truth.
        if (bestItems && bestCount > 0)
        {
            // Helper: read both Username (0x28) and DisplayName (0x38) FStrings.
            auto readNames = [](RC::Unreal::UObject* obj) -> std::pair<std::wstring, std::wstring> {
                std::wstring user, disp;
                auto* base = reinterpret_cast<uint8_t*>(obj);
                SafeReadFString(base + 0x28, user);
                SafeReadFString(base + 0x38, disp);
                return {user, disp};
            };

            // Build matched (data_obj, widget_truth) pairs. Widget shows DisplayName so
            // try that first; fall back to Username for matches.
            std::vector<std::pair<UObject*, RaceEntry>> matched;
            for (int32_t i = 0; i < bestCount; ++i)
            {
                UObject* obj = nullptr;
                if (!SafeReadBytes(&bestItems[i], &obj, sizeof(obj)) || !obj) continue;
                auto [user, disp] = readNames(obj);
                if (user.empty() && disp.empty()) continue;
                for (const auto& w : widgetEntries)
                {
                    if (w.name == disp || w.name == user) { matched.push_back({obj, w}); break; }
                }
            }

            // Discover offsets. Retry every poll until found — some offsets need a
            // visible DNF widget to validate (m_dnfOff via DiscoverEliminatedFlagOffset),
            // and that may not happen in the first frame the result widget appears.
            {
                std::vector<std::pair<UObject*, int>> elimPairs, dmgPairs, posPairs;
                for (const auto& m : matched)
                {
                    elimPairs.push_back({m.first, m.second.eliminations});
                    dmgPairs .push_back({m.first, m.second.damage});
                    // Position: finishers have their actual placement (1, 2, …); DNFs
                    // must read 0 at the real Place offset. Including DNFs in this pair
                    // list excludes look-alike offsets like the listview-index field.
                    posPairs.push_back({m.first, m.second.isDnf ? 0 : m.second.position});
                }
                if (m_posOff  < 0) { int o = DiscoverOffset(posPairs);  if (o >= 0) { m_posOff  = o;
                    Output::send<LogLevel::Default>(STR("[MikMarble] discovered position offset 0x{:x}\n"), o); } }
                if (m_elimOff < 0) { int o = DiscoverOffset(elimPairs); if (o >= 0) { m_elimOff = o;
                    Output::send<LogLevel::Default>(STR("[MikMarble] discovered eliminations offset 0x{:x}\n"), o); } }
                if (m_dmgOff  < 0) { int o = DiscoverOffset(dmgPairs);  if (o >= 0) { m_dmgOff  = o;
                    Output::send<LogLevel::Default>(STR("[MikMarble] discovered damage offset 0x{:x}\n"), o); } }
                if (m_timeOff < 0) { int o = DiscoverFinishTimeOffset(matched); if (o >= 0) { m_timeOff = o;
                    Output::send<LogLevel::Default>(STR("[MikMarble] discovered FinishTime offset 0x{:x}\n"), o); } }
                if (m_dnfOff  < 0) { int o = DiscoverEliminatedFlagOffset(matched); if (o >= 0) { m_dnfOff = o;
                    Output::send<LogLevel::Default>(STR("[MikMarble] discovered eliminated-flag offset 0x{:x}\n"), o); } }

                // Fallback: if widget-supervised discovery failed (e.g. all visible
                // widgets are finishers), scan the listview itself for the
                // [0,0,…,0,X,X,…,X] pattern that uniquely identifies bEliminated.
                if (m_dnfOff < 0)
                {
                    int o = DiscoverEliminatedFlagOffsetByListview(bestItems, bestCount, matched);
                    if (o >= 0)
                    {
                        m_dnfOff = o;
                        Output::send<LogLevel::Default>(STR("[MikMarble] discovered eliminated-flag offset 0x{:x} (listview-pattern)\n"), o);
                    }
                }

                // Finisher-flag (inverted): [non-zero,…,non-zero,0,…,0]. This is the
                // actual signal in the Marbles data object — value 2 = winner,
                // 1 = finished, 0 = DNF.
                if (m_finOff < 0)
                {
                    int o = DiscoverFinisherFlagOffsetByListview(bestItems, bestCount, matched);
                    if (o >= 0)
                    {
                        m_finOff = o;
                        Output::send<LogLevel::Default>(STR("[MikMarble] discovered finisher-flag offset 0x{:x}\n"), o);
                    }
                }

                // v3.10: discover EliminationOrder offset by scanning ALL data objects
                // for a pattern unique to EliminationOrder: many entries read 0 (finishers),
                // some entries read distinct positive integers in [1..K] where K = count
                // of those entries (DNFs in elimination order). No need to know which
                // entries are DNFs in advance — the pattern itself identifies the field.
                // v3.14: ROYALE Position discovery. For royale, the Position field
                // (FRaceResult.Position at 0xC8) holds the elimination RANK for every
                // player (1=winner, 2..N=DNFs sorted by elimination order). The standard
                // DiscoverOffset(posPairs) fails because it requires DNFs to read 0 — but
                // royale DNFs read positive rank values. Pattern: ALL positive distinct
                // integers in [1, K] for some K close to bestCount.
                if (m_posOff < 0 && bestItems && bestCount >= 4)
                {
                    std::vector<int> hits;
                    for (int off = 0; off + 4 <= 0x120; off += 4)
                    {
                        if (off == m_elimOff || off == m_dmgOff) continue;
                        std::set<int32_t> vals;
                        bool ok = true;
                        int total = 0;
                        for (int32_t i = 0; i < bestCount; ++i)
                        {
                            UObject* obj = nullptr;
                            if (!SafeReadBytes(&bestItems[i], &obj, sizeof(obj)) || !obj) continue;
                            int32_t v = 0;
                            if (!SafeReadBytes(reinterpret_cast<uint8_t*>(obj) + off, &v, 4)) { ok = false; break; }
                            ++total;
                            if (v <= 0 || v > 200) { ok = false; break; }
                            if (!vals.insert(v).second) { ok = false; break; }
                        }
                        if (!ok || total < 4) continue;
                        // All distinct positive in [1, K]. Need K close to total.
                        if (*vals.begin() != 1) continue;
                        if (*vals.rbegin() > total + 5) continue;  // allow some sparseness
                        hits.push_back(off);
                    }
                    if (hits.size() == 1)
                    {
                        m_posOff = hits[0];
                        Output::send<LogLevel::Default>(STR("[MikMarble] discovered position offset 0x{:x} (royale-pattern: all-distinct-positive)\n"), m_posOff);
                    }
                    else if (hits.size() > 1)
                    {
                        Output::send<LogLevel::Warning>(STR("[MikMarble] royale-pos discovery: {} candidates {} {} ...\n"),
                            hits.size(), hits[0], hits.size() > 1 ? hits[1] : 0);
                    }
                }

                // v3.12: dump ANY int32 offset whose values look interesting so we
                // can see what data the object actually holds. Once per match.
                if (m_elimOrderOff < 0 && bestItems && bestCount >= 4)
                {
                    Output::send<LogLevel::Default>(STR("[MikMarble] === ELIM-ORDER SCAN START (bestCount={}) ===\n"), bestCount);
                    // Pick a known DNF and a known finisher from matched widgets.
                    UObject* dnfObj = nullptr;
                    UObject* finObj = nullptr;
                    for (const auto& m : matched)
                    {
                        if (m.second.isDnf && !dnfObj) dnfObj = m.first;
                        if (!m.second.isDnf && !finObj) finObj = m.first;
                        if (dnfObj && finObj) break;
                    }
                    if (dnfObj && finObj)
                    {
                        Output::send<LogLevel::Default>(STR("[MikMarble] Dumping fields where DNF != FIN (looking for EliminationOrder):\n"));
                        for (int off = 0; off + 4 <= 0x120; off += 4)
                        {
                            int32_t dv = 0, fv = 0;
                            if (!SafeReadBytes(reinterpret_cast<uint8_t*>(dnfObj) + off, &dv, 4)) continue;
                            if (!SafeReadBytes(reinterpret_cast<uint8_t*>(finObj) + off, &fv, 4)) continue;
                            if (dv != fv && (dv != 0 || fv != 0))
                            {
                                Output::send<LogLevel::Default>(STR("[MikMarble]   +0x{:x}: dnf={} fin={}\n"), off, dv, fv);
                            }
                        }
                    }
                    // Also try the original pattern but with relaxed value range.
                    std::vector<int> hits;
                    int candidatesLogged = 0;
                    for (int off = 0; off + 4 <= 0x120; off += 4)
                    {
                        if (off == m_posOff || off == m_elimOff || off == m_dmgOff)
                            continue;
                        std::set<int32_t> nonZeroSet;
                        int zeroCount = 0;
                        bool seenAny = false, distinct = true;
                        for (int32_t i = 0; i < bestCount; ++i)
                        {
                            UObject* obj = nullptr;
                            if (!SafeReadBytes(&bestItems[i], &obj, sizeof(obj)) || !obj) continue;
                            int32_t v = 0;
                            if (!SafeReadBytes(reinterpret_cast<uint8_t*>(obj) + off, &v, 4)) continue;
                            seenAny = true;
                            if (v == 0) { ++zeroCount; }
                            else
                            {
                                if (!nonZeroSet.insert(v).second) { distinct = false; }
                            }
                        }
                        if (!seenAny) continue;
                        if (nonZeroSet.size() >= 5 && distinct && zeroCount >= 1 && candidatesLogged < 12)
                        {
                            ++candidatesLogged;
                            Output::send<LogLevel::Default>(STR("[MikMarble]   off=+0x{:x}: {} distinct non-zero (range {}..{}), {} zeros\n"),
                                off, nonZeroSet.size(), *nonZeroSet.begin(), *nonZeroSet.rbegin(), zeroCount);
                            hits.push_back(off);
                        }
                    }
                    Output::send<LogLevel::Default>(STR("[MikMarble] === ELIM-ORDER SCAN END: {} candidates ===\n"), hits.size());
                    // Pick the best candidate: prefer one whose non-zero count is closest
                    // to widgetDnfs (count of visible DNF widgets). This is heuristic;
                    // the diagnostic above will show actual values.
                    int widgetDnfs = 0;
                    for (const auto& m : matched) if (m.second.isDnf) ++widgetDnfs;
                    int bestOff = -1;
                    int bestScore = 1000000;
                    for (int off : hits)
                    {
                        std::set<int32_t> nz;
                        for (int32_t i = 0; i < bestCount; ++i)
                        {
                            UObject* obj = nullptr;
                            if (!SafeReadBytes(&bestItems[i], &obj, sizeof(obj)) || !obj) continue;
                            int32_t v = 0;
                            if (SafeReadBytes(reinterpret_cast<uint8_t*>(obj) + off, &v, 4) && v > 0) nz.insert(v);
                        }
                        int diff = std::abs(static_cast<int>(nz.size()) - widgetDnfs);
                        if (diff < bestScore) { bestScore = diff; bestOff = off; }
                    }
                    if (bestOff >= 0 && bestScore <= 5)
                    {
                        m_elimOrderOff = bestOff;
                        Output::send<LogLevel::Default>(STR("[MikMarble] discovered EliminationOrder offset 0x{:x} (best of {}, score={} vs widgetDnfs={})\n"),
                            m_elimOrderOff, hits.size(), bestScore, widgetDnfs);
                    }
                }
            }

            // Build the full 40-player list. For each data object: if its name matches
            // a visible widget, take widget data (authoritative). If unmatched
            // (off-screen row), use discovered offsets for stats and mark as DNF.
            std::vector<RaceEntry> entries;
            int unmatchedCount = 0;
            // v3.0: dedup by name. Stale data objects can leak into the picked listview
            // (e.g. melog_VR's data alive from a prior match where they actually played);
            // matching against a stale pooled widget would emit a phantom row.
            std::set<std::wstring> seenNames;
            for (int32_t i = 0; i < bestCount; ++i)
            {
                UObject* obj = nullptr;
                if (!SafeReadBytes(&bestItems[i], &obj, sizeof(obj)) || !obj) continue;

                auto [user, disp] = readNames(obj);
                if (user.empty() && disp.empty()) continue;
                // Prefer display name (matches UI/widget). Fall back to username.
                std::wstring name = !disp.empty() ? disp : user;

                if (!seenNames.insert(name).second)
                {
                    Output::send<LogLevel::Warning>(STR("[MikMarble] dedup: skipping duplicate listview entry name={}\n"), name);
                    continue;
                }

                const RaceEntry* widgetMatch = nullptr;
                for (const auto& w : widgetEntries)
                {
                    if (w.name == disp || w.name == user) { widgetMatch = &w; break; }
                }

                if (widgetMatch)
                {
                    // Use widget's authoritative finisher/DNF flag and stats.
                    // For DNFs: prefer royale Position-as-rank, then EliminationOrder,
                    // then alphabetical iteration index.
                    RaceEntry e = *widgetMatch;
                    if (e.isDnf)
                    {
                        int32_t v = 0;
                        // v3.14: royale stores elimination rank in Position field;
                        // higher rank = first eliminated. Map to highest mod position.
                        if (gameType == "royale" && m_posOff >= 0 &&
                            SafeReadBytes(reinterpret_cast<uint8_t*>(obj) + m_posOff, &v, sizeof(v)) &&
                            v > 1 && v < 1000)
                            e.position = 29950 + v;  // higher rank → higher pos → loser
                        else if (m_elimOrderOff >= 0 &&
                                 SafeReadBytes(reinterpret_cast<uint8_t*>(obj) + m_elimOrderOff, &v, sizeof(v)) &&
                                 v > 0)
                            e.position = 30000 - v;
                        else
                            e.position = 30000 - (bestCount - i);
                    }
                    // v3.14: for royale, override "winner" flag using Position field.
                    // Only Position 1 is the actual winner.
                    if (gameType == "royale" && m_posOff >= 0)
                    {
                        int32_t v = 0;
                        if (SafeReadBytes(reinterpret_cast<uint8_t*>(obj) + m_posOff, &v, sizeof(v)))
                        {
                            if (v == 1) { e.isDnf = false; e.position = 1; }
                            else if (v > 1 && v < 1000) { e.isDnf = true; e.position = 29950 + v; }
                        }
                    }
                    entries.push_back(e);
                    continue;
                }

                // Off-screen row. Read stats from data object if offsets known.
                ++unmatchedCount;
                auto* base = reinterpret_cast<uint8_t*>(obj);
                int32_t pos = 0, elim = 0, dmg = 0;
                if (m_posOff  >= 0) SafeReadBytes(base + m_posOff,  &pos,  sizeof(pos));
                if (m_elimOff >= 0) SafeReadBytes(base + m_elimOff, &elim, sizeof(elim));
                if (m_dmgOff  >= 0) SafeReadBytes(base + m_dmgOff,  &dmg,  sizeof(dmg));

                RaceEntry e;
                e.name         = name;
                e.eliminations = elim;
                e.damage       = dmg;
                e.points       = 0;

                // DNF detection precedence (most reliable first):
                //   1. Finisher-flag offset (0xF4 in MoS) → 0 means DNF, non-zero finisher.
                //   2. bEliminated-style flag offset discovered → trust it directly.
                //   3. FinishTime offset discovered → value ≈ 0 means DNF.
                //   4. Sticky cache hit → DNF (we saw this name as DNF in widgets).
                //   5. Position offset present + valid placement → finisher (PAR fallback).
                //   6. Otherwise default to DNF.
                bool isDnf = true;
                if (m_finOff >= 0)
                {
                    int32_t flag = 0;
                    SafeReadBytes(base + m_finOff, &flag, sizeof(flag));
                    isDnf = (flag == 0);
                }
                else if (m_dnfOff >= 0)
                {
                    int32_t flag = 0;
                    SafeReadBytes(base + m_dnfOff, &flag, sizeof(flag));
                    isDnf = (flag != 0);
                }
                else if (m_timeOff >= 0)
                {
                    float t = 0.0f;
                    SafeReadBytes(base + m_timeOff, &t, sizeof(t));
                    isDnf = !std::isfinite(t) || std::fabs(t) < 0.001f;
                }
                else
                {
                    bool stickyDnf = m_knownDnfNames.count(name) > 0
                                  || (!user.empty() && m_knownDnfNames.count(user) > 0);
                    if (stickyDnf)
                        isDnf = true;
                    else if (m_posOff >= 0 && pos > 0 && pos <= 200)
                        isDnf = false;
                    else
                        isDnf = true;
                }

                // v3.14: royale uses Position field as elimination rank — pos 1 is
                // winner, pos > 1 are DNFs in elimination order.
                if (gameType == "royale" && m_posOff >= 0 && pos > 0 && pos < 1000)
                {
                    if (pos == 1) { e.isDnf = false; e.position = 1; }
                    else          { e.isDnf = true;  e.position = 29950 + pos; }
                }
                else if (!isDnf && pos > 0 && pos <= 200)
                {
                    e.isDnf    = false;
                    e.position = pos;
                }
                else
                {
                    e.isDnf    = true;
                    int32_t eo = 0;
                    if (m_elimOrderOff >= 0 &&
                        SafeReadBytes(reinterpret_cast<uint8_t*>(obj) + m_elimOrderOff, &eo, sizeof(eo)) &&
                        eo > 0)
                        e.position = 30000 - eo;
                    else
                        e.position = 30000 - (bestCount - i);
                }
                entries.push_back(e);
            }

            // v3.0: dedup by position among finishers. A stale/pooled widget can bind
            // a fresh name to a stale position number, producing two non-DNF entries
            // with the same `position`. Drop the all-zero-stats phantom; if both look
            // real, drop the later-encountered (less likely to be the freshly-bound row).
            {
                std::map<int, size_t> firstWithPos;
                std::vector<size_t> drops;
                for (size_t i = 0; i < entries.size(); ++i)
                {
                    if (entries[i].isDnf) continue;
                    auto it = firstWithPos.find(entries[i].position);
                    if (it == firstWithPos.end())
                    {
                        firstWithPos[entries[i].position] = i;
                        continue;
                    }
                    size_t j = it->second;
                    auto phantomLooking = [](const RaceEntry& e) {
                        return e.points == 0 && e.eliminations == 0 && e.damage == 0;
                    };
                    bool ip = phantomLooking(entries[i]);
                    bool jp = phantomLooking(entries[j]);
                    size_t drop;
                    if (ip && !jp) drop = i;
                    else if (jp && !ip) { drop = j; firstWithPos[entries[i].position] = i; }
                    else drop = i;
                    Output::send<LogLevel::Warning>(STR("[MikMarble] dedup: dropping duplicate pos={} name={} (phantom-heuristic)\n"),
                        entries[drop].position, entries[drop].name);
                    drops.push_back(drop);
                }
                if (!drops.empty())
                {
                    std::sort(drops.begin(), drops.end(), std::greater<size_t>());
                    drops.erase(std::unique(drops.begin(), drops.end()), drops.end());
                    for (size_t idx : drops) entries.erase(entries.begin() + idx);
                }
            }

            if (!entries.empty())
            {
                std::sort(entries.begin(), entries.end(), [](const RaceEntry& a, const RaceEntry& b) {
                    return a.position < b.position;
                });

                // v3.0: cross-match stale-listview rejection. If the picked listview's
                // entries match the previous race's signature exactly, we picked a
                // stale carryover. Refuse to send and wait for the next poll. Two
                // races never legitimately produce identical (name, position) sets.
                if (!m_lastRaceSignature.empty())
                {
                    std::vector<std::pair<std::wstring, int>> sig;
                    sig.reserve(entries.size());
                    for (const auto& e : entries) sig.push_back({e.name, e.position});
                    std::sort(sig.begin(), sig.end());
                    if (sig == m_lastRaceSignature)
                    {
                        Output::send<LogLevel::Warning>(STR("[MikMarble] cross-match: picked listview matches previous race signature exactly; treating as stale carryover\n"));
                        return {};
                    }
                }

                return entries;
            }
        }

        // Fallback: widget-only data (24 rows max, but reliable).
        auto entries = widgetEntries;
        std::sort(entries.begin(), entries.end(), [](const RaceEntry& a, const RaceEntry& b) {
            if (a.position != b.position) return a.position < b.position;
            return a.points > b.points;
        });
        return entries;
    }

    // ── Detect game mode type ─────────────────────────────────────────────

    // v3.5: read AGameStateBase::PlayerArray.Num() to know how many players are
    // actually in the current match. Used as a sanity guard against the listview
    // returning stale/truncated data — if our entry count is way below the actual
    // player count, refuse to POST (better silent than a wrong winner).
    // PlayerArray sits at offset 0x2A8 within AGameStateBase in this UE version
    // (per CXXHeaderDump cross-reference).
    int GetExpectedPlayerCount()
    {
        UObject* gs = UObjectGlobals::FindFirstOf(STR("MarbleRaceGameState"));
        if (!gs) gs = UObjectGlobals::FindFirstOf(STR("MarbleRoyaleGameState"));
        if (!gs) gs = UObjectGlobals::FindFirstOf(STR("BullseyeGameState"));
        if (!gs) gs = UObjectGlobals::FindFirstOf(STR("TiltedGameState"));
        if (!gs) gs = UObjectGlobals::FindFirstOf(STR("CMBGameState"));
        if (!gs) return -1;
        struct TArrayRaw { void* Data; int32_t Num; int32_t Max; };
        TArrayRaw arr{};
        if (!SafeReadBytes(reinterpret_cast<uint8_t*>(gs) + 0x2A8, &arr, sizeof(arr))) return -1;
        if (arr.Num < 0 || arr.Num > 1000) return -1;
        return arr.Num;
    }

    auto DetectGameType() -> std::string
    {
        if (UObjectGlobals::FindFirstOf(STR("MarbleRoyaleGameState")))
            return "royale";
        // Target race / Precision Aim Race — players are ranked by distance, not
        // finish time. The game's bEliminated flag is unreliable here (some "false
        // DNFs"), so we treat every player as a finisher in this mode.
        if (UObjectGlobals::FindFirstOf(STR("BullseyeGameState")))
            return "target";
        if (UObjectGlobals::FindFirstOf(STR("MarbleRaceGameState")))
            return "race";
        if (UObjectGlobals::FindFirstOf(STR("TiltedGameState")))
            return "tilted";
        if (UObjectGlobals::FindFirstOf(STR("CMBGameState")))
            return "bumperballs";
        return "unknown";
    }

    // ── Send winners on background thread ──────────────────────────────────

    auto SendResults(const std::vector<PlayerResult>& players,
                     const std::string& gameType) -> void
    {
        if (m_state->httpBusy.load()) return;
        m_state->httpBusy.store(true);
        m_state->inflight.fetch_add(1);

        std::string json = BuildResultsJson(players, gameType);
        auto playersCopy = players;
        {
            std::lock_guard<std::mutex> lock(m_state->mtx);
            m_state->lastGameType = gameType;
        }
        std::string host;
        int port = 0;
        std::string path;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            host = m_config.host;
            port = m_config.port;
            path = m_config.path;
        }

        std::string resultsPath = path + "/results";
        auto state = m_state;  // shared_ptr copy — survives mod destruction
        std::thread([state, json = std::move(json), playersCopy = std::move(playersCopy), host, port, resultsPath]() noexcept {
            try
            {
                HttpResult r = HttpPostJson(host, port, resultsPath, json);
                std::lock_guard<std::mutex> lock(state->mtx);
                state->lastSentPlayers = playersCopy;
                state->pendingLogs.push_back(std::move(r));
            }
            catch (...) { /* swallow — never let exceptions escape into std::thread */ }
            state->httpBusy.store(false);
            state->inflight.fetch_sub(1);
        }).detach();
    }

public:
    MikMarbleMod() : CppUserModBase()
    {
        ModName = STR("MikMarbleMod");
        ModVersion = STR("3.16");
        ModDescription = STR("Mik marbles race result tracker (C++)");
        ModAuthors = STR("Draxi");

        m_config = LoadConfig();

        register_tab(STR("Mik Marble"), [](CppUserModBase* instance) {
            auto* self = static_cast<MikMarbleMod*>(instance);

            // Snapshot config under lock — register_tab callback runs on the
            // GUI render thread by default (UE4SS PR #768).
            std::string host; int port = 0; std::string path;
            {
                std::lock_guard<std::mutex> lock(self->m_configMutex);
                host = self->m_config.host;
                port = self->m_config.port;
                path = self->m_config.path;
            }

            ImGui::Text("MikMarbleMod v2.9 (C++)");
            ImGui::Text("Endpoint: %s:%d%s", host.c_str(), port, path.c_str());
            ImGui::Separator();

            // Snapshot shared HTTP state under one lock.
            std::vector<PlayerResult> snapshot;
            std::string lastGameType;
            {
                std::lock_guard<std::mutex> lock(self->m_state->mtx);
                snapshot      = self->m_state->lastSentPlayers;
                lastGameType  = self->m_state->lastGameType;
            }
            ImGui::Text("Last sent: %d players", static_cast<int>(snapshot.size()));
            ImGui::Text("HTTP: %s", self->m_state->httpBusy.load() ? "sending..." : "idle");
            if (!lastGameType.empty())
                ImGui::Text("Last type: %s", lastGameType.c_str());

            if (!snapshot.empty())
            {
                ImGui::Separator();
                ImGui::Text("Last sent players:");
                for (size_t i = 0; i < snapshot.size(); ++i)
                {
                    const auto& p = snapshot[i];
                    std::string narrow = WideToUtf8(p.name);
                    ImGui::Text("  %d. [%s] %s  elim=%d dmg=%d",
                        static_cast<int>(i + 1),
                        p.finished ? "OK " : "DNF",
                        narrow.c_str(),
                        p.eliminations,
                        p.damage);
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Test Race"))
            {
                std::vector<PlayerResult> players = {
                    {L"TestPlayer1", true, 0, 0},
                    {L"TestPlayer2", true, 0, 0},
                    {L"TestPlayer3", true, 0, 0}
                };
                self->SendResults(players, "race");
            }
            ImGui::SameLine();
            if (ImGui::Button("Test Royale"))
            {
                std::vector<PlayerResult> players = {
                    {L"TestWinner", true, 3, 450},
                    {L"TestMiddle", true, 1, 200},
                    {L"TestLoser", false, 0, 50}
                };
                self->SendResults(players, "royale");
            }
            ImGui::SameLine();
            if (ImGui::Button("Test All DNF"))
            {
                std::vector<PlayerResult> players = {
                    {L"DnfA", false, 0, 30},
                    {L"DnfB", false, 1, 80},
                    {L"DnfC", false, 0, 10}
                };
                self->SendResults(players, "royale");
            }
        });
    }

    ~MikMarbleMod() override
    {
        // Wait briefly for any in-flight HTTP workers so they can't write into
        // shared state after destruction (m_state itself survives via shared_ptr,
        // but bounded waiting avoids surprise teardown ordering bugs).
        for (int i = 0; i < 100 && m_state->inflight.load() > 0; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        Output::send<LogLevel::Default>(STR("[MikMarble] Mod unloaded\n"));
    }

    auto on_ui_init() -> void override
    {
        UE4SS_ENABLE_IMGUI();
    }

    auto on_unreal_init() -> void override
    {
        Output::send<LogLevel::Default>(STR("[MikMarble] Mod initialized (C++ v3.16)\n"));
        std::wstring wHost(m_config.host.begin(), m_config.host.end());
        std::wstring wPath(m_config.path.begin(), m_config.path.end());
        Output::send<LogLevel::Default>(STR("[MikMarble] Config: {}:{}{}\n"), wHost, m_config.port, wPath);
        m_unrealReady.store(true);
    }

    // Drain any HTTP results queued by background workers and log them on the
    // game thread, where Output::send is safe.
    void DrainHttpLogs()
    {
        std::deque<HttpResult> drained;
        {
            std::lock_guard<std::mutex> lock(m_state->mtx);
            drained.swap(m_state->pendingLogs);
        }
        for (const auto& r : drained)
        {
            std::wstring wLabel(r.label.begin(), r.label.end());
            if (r.ok)
            {
                Output::send<LogLevel::Default>(STR("[MikMarble] POST {} -> HTTP {}\n"), wLabel, r.statusCode);
            }
            else
            {
                Output::send<LogLevel::Warning>(STR("[MikMarble] POST {} failed, error {}\n"), wLabel, r.lastError);
            }
        }
    }

    // Send the one-shot /ready notification once unreal init is done.
    void MaybeSendReady()
    {
        if (!m_unrealReady.load()) return;
        bool expected = false;
        if (!m_readySent.compare_exchange_strong(expected, true)) return;

        std::string host; int port = 0; std::string path;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            host = m_config.host;
            port = m_config.port;
            path = m_config.path + "/ready";
        }
        auto state = m_state;
        state->inflight.fetch_add(1);
        std::thread([state, host, port, path]() noexcept {
            try
            {
                HttpResult r = HttpPostJson(host, port, path, "");
                std::lock_guard<std::mutex> lock(state->mtx);
                state->pendingLogs.push_back(std::move(r));
            }
            catch (...) {}
            state->inflight.fetch_sub(1);
        }).detach();
    }

    // Top-level last-line-of-defense: wrap the per-tick work in C++ catch-all
    // and an outer SEH guard via SafeTickTrampoline. An access violation deep
    // in widget reads will be caught here so the mod survives instead of
    // crashing the whole DLL ("fatal error" dialog).
    void OnUpdateImpl()
    {
        // Throttle to ~1 check per second
        static auto lastTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        m_pollTimer += dt;
        if (m_pollTimer < POLL_INTERVAL) return;
        m_pollTimer = 0.0f;

        // Game-thread-only: drain HTTP log queue and fire /ready once unreal is ready.
        DrainHttpLogs();
        MaybeSendReady();

        // Bound DNF cache growth — pathological result widgets that never close
        // could otherwise grow this set unboundedly.
        if (m_knownDnfNames.size() > 512) m_knownDnfNames.clear();

        auto entries = GetCurrentRaceEntries();
        if (entries.empty())
        {
            // v3.9: don't reset on a SINGLE empty-entry poll. UMG list virtualization
            // (e.g. during user scrolling in the result widget) makes
            // PlayerRaceResultEntryWidget instances briefly disappear, which used to
            // trigger m_sentForCurrentMatch reset → re-send when widgets returned.
            // Require 5 consecutive empty polls (~5 seconds) before resetting.
            if (++m_consecutiveEmptyPolls >= 5)
            {
                {
                    std::lock_guard<std::mutex> lock(m_state->mtx);
                    if (!m_state->lastSentPlayers.empty()) m_state->lastSentPlayers.clear();
                }
                if (!m_knownDnfNames.empty()) m_knownDnfNames.clear();
                m_sentForCurrentMatch = false;
                m_targetDumped = false;
                m_consecutiveEmptyPolls = 0;
            }
            return;
        }
        m_consecutiveEmptyPolls = 0;

        // Match-lock: once we've successfully sent a result for the current open
        // result widget, ignore further changes until the widget closes. The result
        // widget can switch tabs/views (e.g. royale "view by eliminations") and flip
        // visible widget flags; we want to keep the FIRST authoritative send.
        if (m_sentForCurrentMatch) return;

        auto gameType = DetectGameType();
        if (gameType == "unknown") return;

        // In target race the game's bEliminated flag is noisy ("false DNFs" for
        // players who placed but didn't hit the target perfectly). Treat every
        // player as a finisher and report on the wire as plain "race".
        bool isTarget = (gameType == "target");
        std::string wireType = isTarget ? "race" : gameType;

        // Build players list (all participants with finished flag, kills, damage).
        // Skip mik_VR in races/target.
        std::vector<PlayerResult> players;
        for (const auto& entry : entries)
        {
            if ((gameType == "race" || isTarget) && entry.name == STR("mik_VR")) continue;
            bool finished = isTarget ? true : !entry.isDnf;
            players.push_back({entry.name, finished, entry.eliminations, entry.damage});
        }

        // Content dedup: only send if the list differs from the last successful send.
        // Snapshot under lock so the HTTP thread can't resize mid-compare.
        std::vector<PlayerResult> lastSnapshot;
        {
            std::lock_guard<std::mutex> lock(m_state->mtx);
            lastSnapshot = m_state->lastSentPlayers;
        }
        bool changed = players.size() != lastSnapshot.size();
        if (!changed)
        {
            for (size_t i = 0; i < players.size(); ++i)
            {
                const auto& a = players[i];
                const auto& b = lastSnapshot[i];
                if (a.name != b.name || a.finished != b.finished ||
                    a.eliminations != b.eliminations || a.damage != b.damage)
                {
                    changed = true;
                    break;
                }
            }
        }
        if (!changed) return;

        // v3.5: undercount guard. Compare our entry count against the GameState's
        // PlayerArray.Num() — the authoritative count of players in this match.
        // The anchored URaceResultsWidgetY2.RaceResultsListView has been observed
        // to retain a stale 25-item binding from the first match across subsequent
        // races (50 / 75 / 100-player races all reported 25 players). When the
        // listview is undercount, refuse to POST so we don't lie. Mod will retry
        // on the next 1Hz poll; if data is genuinely sparse vs. expected, we
        // stay silent rather than send a wrong winner.
        int expected = GetExpectedPlayerCount();
        if (expected > 0 && static_cast<int>(entries.size()) + 2 < expected)
        {
            Output::send<LogLevel::Warning>(STR("[MikMarble] undercount guard: entries={} but GameState.PlayerArray.Num={} — refusing to POST stale/truncated data\n"),
                entries.size(), expected);
            return;
        }

        for (const auto& e : entries)
        {
            Output::send<LogLevel::Default>(STR("[MikMarble]   pos={} pts={} dnf={} elim={} dmg={} name={}\n"),
                e.position, e.points, e.isDnf ? 1 : 0, e.eliminations, e.damage, e.name);
        }

        std::wstring wType(gameType.begin(), gameType.end());
        int finishedCount = 0;
        for (const auto& p : players) if (p.finished) ++finishedCount;
        Output::send<LogLevel::Default>(STR("[MikMarble] New results ({}): {} players, {} finished\n"),
            wType, players.size(), finishedCount);
        SendResults(players, wireType);
        m_sentForCurrentMatch = true;

        // Snapshot signature for cross-match stale-listview rejection on the NEXT
        // race. Persists until overwritten by another successful send.
        m_lastRaceSignature.clear();
        m_lastRaceSignature.reserve(entries.size());
        for (const auto& e : entries) m_lastRaceSignature.push_back({e.name, e.position});
        std::sort(m_lastRaceSignature.begin(), m_lastRaceSignature.end());
    }

    // SEH trampoline: noexcept, no destructible locals, calls into OnUpdateImpl.
    // Catches access violations from stale UMG widgets etc.
    __declspec(noinline) void SafeTick() noexcept
    {
        __try { OnUpdateImpl(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { /* swallow */ }
    }

    auto on_update() -> void override
    {
        try { SafeTick(); }
        catch (...) { /* belt & suspenders for any C++ exception */ }
    }
};

#define MIK_MOD_API __declspec(dllexport)
extern "C"
{
    MIK_MOD_API RC::CppUserModBase* start_mod()
    {
        return new MikMarbleMod();
    }

    MIK_MOD_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
