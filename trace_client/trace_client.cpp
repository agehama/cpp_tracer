// bbtrace_ipc.cpp : DRクライアント（双方向IPC）最小ひな形
#include <cstdint>
#include <cstring>
#include <vector>
#include <deque>
#include <string>
#include <atomic>

#include "dr_api.h"
#include "drmgr.h"

#ifdef _WIN32
#  include <windows.h>
#endif

// ====== 共有メモリレイアウト ======
#pragma pack(push, 1)
struct RingHdr
{
    uint32_t cap;                 // 要素数(2のべき推奨)
    uint32_t widx;
    uint32_t ridx;
    uint32_t dropped;
};

struct ShmHeader {
    uint32_t magic;               // 'RBTR' = 0x52544252
    uint32_t channel;
    uint32_t pid;
    uint32_t cap_evt;             // A→B
    uint32_t cap_cmd;             // B→A
};

enum : uint16_t
{
    EV_BB_HIT,
    EV_MOD_ADD,
    EV_MOD_DEL,
};

// type == EV_BB_HIT
struct BBEvent
{
    uint32_t pid;
    uint32_t tid;
    uint64_t ts_us;   // dr_get_microseconds()
    uint64_t app_pc;  // BB先頭
    uint64_t app_pc_end;   // 1固定でOK（集計は受信側）
};

// type == EV_MOD_ADD / EV_MOD_DEL
struct ModEvent
{
    uint32_t pid;          // 発生元プロセス
    uint64_t base;         // module base (info->start)
    uint64_t size;         // image size
    uint32_t path_len;     // 後続の UTF-16 パス長（文字数）
    uint16_t pathIndex;
    // 直後に UTF-16 のパス本体（可変長）を詰める設計でもOK
};
struct PendingData
{
    ModEvent data;
    std::string modulePath;
};
struct EventSlot
{
    uint16_t type;  // EV_*
    uint16_t _pad;  // アライン調整（pack(1)でもBBと互換を保つ）

    union
    {
        BBEvent bb;
        ModEvent mod;
    };
};

struct ProcInfoEvent {
    uint16_t type, _pad;
    uint32_t pid;          // ターゲットPID
    // 将来のためのフィールド（例：ビット数、チャネル名など）を足してもよい
};

struct Range {
    uint64_t base;      // モジュールロードベース（ASLR考慮）
    uint64_t begin_rva; // [base + begin, base + end) にヒットで有効
    uint64_t end_rva;
};

enum : uint16_t
{
    CMD_ADD_RANGES = 0,
    CMD_CLEAR_RANGES = 1
};

struct Command {
    uint16_t type;    // CMD_*
    uint16_t n;       // ranges数（CMD_ADD_RANGESの場合）
   // uint32_t u32;     // 予約（閾値など）
    Range ranges[8];  // 最大小数の固定長（必要なら拡張）
};
#pragma pack(pop)

struct ShmLayout
{
    ShmHeader H;
    RingHdr   ring_evt;
    EventSlot buf_evt[1 << 15];   // 32768 events
    RingHdr   ring_cmd;
    Command   buf_cmd[1024];      // コマンドは固定長スロット
    char      strBuffer[16384];
};

// ====== SPSCリング操作 ======
//template<class T>
static inline bool spsc_push(RingHdr* h, EventSlot* buf, const EventSlot& v)
{
    const uint32_t cap = h->cap;
    uint32_t w = h->widx, r = h->ridx;
    uint32_t next = (w + 1) & (cap - 1);
    if (next == r) { h->dropped++; return false; }
    buf[w] = v; 
    _ReadWriteBarrier();
    h->widx = next;
    return true;
}
//template<class T>
static inline bool spsc_pop(RingHdr* h, Command* buf, Command& out)
{
    uint32_t r = h->ridx, w = h->widx;
    if (r == w) return false;
    out = buf[r];
    _ReadWriteBarrier();
    h->ridx = (r + 1) & (h->cap - 1);
    return true;
}

// ====== グローバル ======
//static std::deque<EventSlot> g_pendingq;

static std::deque<PendingData> g_pendingq;
static ShmLayout* g_shm = nullptr;
static uint16_t g_charStart = 0;
static HANDLE g_hMap = nullptr;
static HANDLE g_evt_a2b = nullptr; // DR→Viewer
static HANDLE g_evt_b2a = nullptr; // Viewer→DR

// 計測対象レンジ（簡易版）
static std::atomic<uint32_t> g_range_count{0};
static Range g_ranges[256];  // 必要なら動的化

// ====== IPC初期化／終了 ======
/*
static void ipc_init() {
    const uint32_t pid = dr_get_process_id();
    char shm[64], e1[64], e2[64];
    dr_snprintf(shm, sizeof(shm), "Local\\bbtrace_shm_%u", pid);
    dr_snprintf(e1,  sizeof(e1),  "Local\\bbtrace_evt_a2b_%u", pid);
    dr_snprintf(e2,  sizeof(e2),  "Local\\bbtrace_evt_b2a_%u", pid);

    const uint32_t CAP_EVT = (1u << 15);
    const uint32_t CAP_CMD = 1024;

    const size_t sz = sizeof(ShmLayout);
    g_hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                0, (DWORD)sz, shm);
    void* base = MapViewOfFile(g_hMap, FILE_MAP_ALL_ACCESS, 0, 0, sz);
    g_shm = (ShmLayout*)base;

    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        memset(g_shm, 0, sizeof(*g_shm));
        g_shm->H.magic = 0x52544252;
        g_shm->H.version = 1;
        g_shm->H.pid = pid;
        g_shm->H.cap_evt = CAP_EVT;
        g_shm->H.cap_cmd = CAP_CMD;
        g_shm->ring_evt.cap = CAP_EVT;
        g_shm->ring_cmd.cap = CAP_CMD;
    }

    g_evt_a2b = CreateEventA(NULL, FALSE, FALSE, e1);
    g_evt_b2a = CreateEventA(NULL, FALSE, FALSE, e2);
}
*/

//static void ipc_init() {
//    const uint32_t pid = dr_get_process_id();
//    char shm[64], e1[64], e2[64];
//    dr_snprintf(shm, sizeof(shm), "Local\\bbtrace_shm_%u", pid);
//    dr_snprintf(e1, sizeof(e1), "Local\\bbtrace_evt_a2b_%u", pid);
//    dr_snprintf(e2, sizeof(e2), "Local\\bbtrace_evt_b2a_%u", pid);
//
//    const size_t sz = sizeof(ShmLayout);
//    g_hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)sz, shm);
//    if (!g_hMap) {
//        dr_printf("CreateFileMappingA failed: %lu\n", GetLastError());
//        return;
//    }
//    void* base = MapViewOfFile(g_hMap, FILE_MAP_ALL_ACCESS, 0, 0, sz);
//    if (!base) {
//        dr_printf("MapViewOfFile failed: %lu\n", GetLastError());
//        return;
//    }
//    g_shm = (ShmLayout*)base;
//
//    if (GetLastError() != ERROR_ALREADY_EXISTS) {
//        memset(g_shm, 0, sizeof(*g_shm));
//        g_shm->H.magic = 0x52544252; // 'RBTR'
//        g_shm->H.version = 1;
//        g_shm->H.pid = pid;
//        g_shm->H.cap_evt = 1u << 15;
//        g_shm->H.cap_cmd = 1024;
//        g_shm->ring_evt.cap = 1u << 15;
//        g_shm->ring_cmd.cap = 1024;
//    }
//
//    g_evt_a2b = CreateEventA(NULL, FALSE, FALSE, e1);
//    if (!g_evt_a2b) {
//        dr_printf("CreateEventA(A->B) failed: %lu\n", GetLastError());
//        return;
//    }
//    g_evt_b2a = CreateEventA(NULL, FALSE, FALSE, e2);
//    if (!g_evt_b2a) {
//        dr_printf("CreateEventA(B->A) failed: %lu\n", GetLastError());
//        return;
//    }
//}

/*
static void ipc_init() {
    const uint32_t pid = dr_get_process_id();
    char shm[128], e1[128], e2[128];
    dr_snprintf(shm, sizeof(shm), "Local\\bbtrace_shm_%u", pid);
    dr_snprintf(e1, sizeof(e1), "Local\\bbtrace_evt_a2b_%u", pid);
    dr_snprintf(e2, sizeof(e2), "Local\\bbtrace_evt_b2a_%u", pid);

    g_hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, (DWORD)sizeof(ShmLayout), shm);
    DWORD map_err = GetLastError();
    if (!g_hMap) { dr_printf("CFM failed: %lu\n", GetLastError()); return; }

    void* base = MapViewOfFile(g_hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmLayout));
    if (!base) { dr_printf("MVF failed: %lu\n", GetLastError()); return; }
    g_shm = (ShmLayout*)base;

    if (map_err != ERROR_ALREADY_EXISTS)
    {
        memset(g_shm, 0, sizeof(*g_shm));
        g_shm->H.magic = 0x52544252;
        g_shm->H.version = 1;
        g_shm->H.pid = pid;
        g_shm->H.cap_evt = 1u << 15;
        g_shm->H.cap_cmd = 1024;
        g_shm->ring_evt.cap = 1u << 15;
        g_shm->ring_cmd.cap = 1024;
    }

    g_evt_a2b = CreateEventA(NULL, FALSE, FALSE, e1);
    if (!g_evt_a2b) { dr_printf("CreateEvent A2B failed: %lu\n", GetLastError()); return; }
    g_evt_b2a = CreateEventA(NULL, FALSE, FALSE, e2);
    if (!g_evt_b2a) { dr_printf("CreateEvent B2A failed: %lu\n", GetLastError()); return; }
}
*/

// 共有メモリだけ使う版：CreateEvent* は使わない
static void ipc_init(const wchar_t* channelNameOpt)
{
    const uint32_t pid = dr_get_process_id();
    //wchar_t shm[128];
    //dr_snprintf(shm, sizeof(shm), "Local\\bbtrace_shm_%u", pid);
    //dr_printf(L"ipc_init shm: %ls\n", channelNameOpt);

    g_hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)sizeof(ShmLayout), channelNameOpt);
    DWORD map_err = GetLastError();
    if (!g_hMap) { dr_printf("CFM failed: %lu\n", GetLastError()); return; }

    void* base = MapViewOfFile(g_hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmLayout));
    if (!base) { dr_printf("MVF failed: %lu\n", GetLastError()); return; }
    g_shm = (ShmLayout*)base;

    if (map_err != ERROR_ALREADY_EXISTS)
    {
        memset(g_shm, 0, sizeof(*g_shm));
        g_shm->H.magic = 0x52544252;
        g_shm->H.channel = (channelNameOpt[0] << 16) + channelNameOpt[1];
        g_shm->H.pid = pid;
        g_shm->H.cap_evt = 1u << 15;
        g_shm->H.cap_cmd = 1024;
        g_shm->ring_evt.cap = 1u << 15;
        g_shm->ring_cmd.cap = 1024;
        g_charStart = 0;
    }
}

static void ipc_close() {
    if (g_evt_a2b) { CloseHandle(g_evt_a2b); g_evt_a2b = nullptr; }
    if (g_evt_b2a) { CloseHandle(g_evt_b2a); g_evt_b2a = nullptr; }
    if (g_shm)     { UnmapViewOfFile(g_shm);  g_shm = nullptr; }
    if (g_hMap)    { CloseHandle(g_hMap);     g_hMap = nullptr; }
}

// ====== フィルタ判定（行→addr範囲はビューアで解決済み前提） ======
static bool should_instrument(app_pc pc) {
    uint32_t n = g_range_count.load(std::memory_order_acquire);
    if (n == 0) return true; // 指定が無ければ全BB
    uint64_t addr = (uint64_t)pc;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t b = g_ranges[i].base + g_ranges[i].begin_rva;
        uint64_t e = g_ranges[i].base + g_ranges[i].end_rva;
        if (b <= addr && addr < e) return true;
    }
    return false;
}

// ====== コマンド受信スレッド ======
static void apply_command(const Command& c)
{
    if (c.type == CMD_CLEAR_RANGES) {
        g_range_count.store(0, std::memory_order_release);
        return;
    }
    if (c.type == CMD_ADD_RANGES) {
        uint32_t cur = g_range_count.load();
        uint32_t add = c.n;
        if (add > 8) add = 8;
        uint32_t cap = (uint32_t)(sizeof(g_ranges)/sizeof(g_ranges[0]));
        uint32_t can = (cur + add <= cap) ? add : (cap - cur);
        for (uint32_t i = 0; i < can; ++i)
            g_ranges[cur + i] = c.ranges[i];
        g_range_count.store(cur + can, std::memory_order_release);
        return;
    }
}

// コマンド受信ループは Wait をやめて軽ポーリング
static void cmd_loop(void*) {
    for (;;) {
        Command c;
        while (g_shm && spsc_pop(&g_shm->ring_cmd, g_shm->buf_cmd, c))
            apply_command(c);
        dr_sleep(10); // 10ms 程度のポーリングで十分
    }
}

/*
static void cmd_loop(void*)
{
    uint32_t last_widx = g_shm ? g_shm->ring_evt.widx : 0;
    for (;;) {
        // 1) Viewer→DR のコマンド受信
        DWORD w = WaitForSingleObject(g_evt_b2a, 10); // 10ms タイムアウト
        if (w == WAIT_OBJECT_0) {
            Command c;
            while (spsc_pop(&g_shm->ring_cmd, g_shm->buf_cmd, c))
                apply_command(c);
        }

        // 2) DR→Viewer の通知（リングが進んだら SetEvent）
        if (g_shm) {
            uint32_t cur = g_shm->ring_evt.widx;
            if (cur != last_widx) {
                last_widx = cur;
                SetEvent(g_evt_a2b);  // ★ Win32呼び出しはここだけ
            }
        }

        // TODO: 終了フラグを見るならここでbreak
    }
}
*/

static volatile int g_ipc_ready = 0;
static size_t g_send_count = 0;

// ====== BBヒット clean-call ======
static void on_bb(void* drcontext, app_pc start, void* tag, app_pc end)
{
    if (!g_ipc_ready)
    {
        return;
    }

    /*
    app_pc chosen = start;
    {
        void* drctx = dr_get_current_drcontext();

        app_pc apc_from_arg = start;

        app_pc apc_from_tag = dr_fragment_app_pc(tag);

        dr_mcontext_t mc = { sizeof(mc), DR_MC_CONTROL };//pc
        dr_get_mcontext(drctx, &mc);           // mc.pc は Code Cache のPC
        app_pc apc_from_cc = dr_app_pc_from_cache_pc(mc.pc);

        auto dump_one = [](const char* label, app_pc p) {
            if (p == NULL) {
                dr_fprintf(STDERR, "%s: NULL\n", label);
                return;
            }
            module_data_t* m = dr_lookup_module(p);
            if (!m) {
                dr_fprintf(STDERR, "%s: %p NO-MODULE\n", label, p);
            }
            else {
                dr_fprintf(STDERR, "%s: %p MOD=%s [%p..%p)\n",
                    label, p, m->full_path, m->start, m->end);
                dr_free_module_data(m);
            }
            };

        dump_one("ARG", apc_from_arg);
        dump_one("TAG", apc_from_tag);
        dump_one("CC2APP", apc_from_cc);

        chosen = apc_from_tag ? apc_from_tag :
            (apc_from_arg ? apc_from_arg : apc_from_cc);
    }
    start = chosen;
    */

    //{
    //    module_data_t* m = dr_lookup_module(start);
    //    if (m == NULL) {
    //        dr_fprintf(STDERR, "NO-MODULE apc=%p\n", start); // ← ここが頻発ならまだPCが壊れてる
    //    }
    //    else {
    //        dr_fprintf(STDERR, "OK apc=%p mod=%s [%p..%p)\n",
    //            start, m->full_path, m->start, m->end);
    //        dr_free_module_data(m);
    //    }
    //}

    while (!g_pendingq.empty())
    {
        auto modData = g_pendingq.front();
        g_pendingq.pop_front();

        const auto currentIndex = modData.data.pathIndex;
        dr_printf("bbtrace-ipc: on_bb sending on_module_load event deferred: %u\n", currentIndex);
        {
            auto pp = g_shm->strBuffer + currentIndex;
            //strcpy_s(pp, modData.modulePath.size(), modData.modulePath.data());
            strcpy(pp, modData.modulePath.data());
        }

        EventSlot data;
        data.type = EV_MOD_ADD;
        data.mod = modData.data;
        spsc_push(&g_shm->ring_evt, g_shm->buf_evt, data);

        dr_printf("bbtrace-ipc: EventSlot send data : %d\n", data.type);
    }

    (void)drcontext;
    
    BBEvent ev = {};
    ev.pid  = dr_get_process_id();
    ev.tid  = (uint32_t)dr_get_thread_id(drcontext);
    ev.ts_us = (uint64_t)dr_get_microseconds();
    ev.app_pc = (uint64_t)start;
    ev.app_pc_end = (uint64_t)end;
    EventSlot data;
    data.type = EV_BB_HIT;
    data.bb = ev;
    spsc_push(&g_shm->ring_evt, g_shm->buf_evt, data);
    //++g_send_count;

    //dr_printf("bbtrace-ipc: g_send_count : %lld\n", g_send_count);
}

static app_pc g_exe_start = 0, g_exe_end = 0;
static inline bool is_exe_pc(app_pc pc)
{
    return (pc >= g_exe_start && pc < g_exe_end);
}

// どのモジュールか判定
static bool is_user_module(app_pc pc) {
    module_data_t* m = dr_lookup_module(pc);
    bool ok = false;
    if (m) {
        // 例: exe本体だけ通す
        ok = (m->names.file_name != NULL && m->names.exe_name); // or パス名で判定
        dr_free_module_data(m);
    }
    return ok;
}

// ====== BB挿入：先頭で clean-call（フィルタに一致したBBのみ） ======
static dr_emit_flags_t
event_bb_insert(void* drcontext, void* tag, instrlist_t* bb, instr_t* /*where*/,
                bool /*for_trace*/, bool /*translating*/, void* /*user*/) {
    instr_t* first = instrlist_first_app(bb);
    if (!first) return DR_EMIT_DEFAULT;
    app_pc start = instr_get_app_pc(first);
    app_pc app_pc2 = dr_fragment_app_pc(tag);
    instr_t* last = instrlist_last_app(bb);
    /*if (!is_user_module(start))
    {
        return DR_EMIT_DEFAULT;
    }*/
    if (!is_exe_pc(start) || last == NULL)
    {
        return DR_EMIT_DEFAULT;
    }

    app_pc end = instr_get_app_pc(last);
    int len = instr_length(drcontext, last);
    app_pc bb_end_excl = end + len;

    dr_insert_clean_call(drcontext, bb, instrlist_first(bb),
                         (void*)on_bb, false, 4,
        OPND_CREATE_INTPTR(drcontext), OPND_CREATE_INTPTR(start), OPND_CREATE_INTPTR(tag), OPND_CREATE_INTPTR(bb_end_excl));
    return DR_EMIT_DEFAULT;
}

//static dr_emit_flags_t
//event_bb_insert(void* drcontext, void* tag, instrlist_t* bb, instr_t* /*where*/,
//    bool /*for_trace*/, bool /*translating*/, void* /*user*/)
//{
//    instr_t* first = instrlist_first_app(bb);
//    if (!first) return DR_EMIT_DEFAULT;
//
//    app_pc app_pc1 = instr_get_app_pc(first);
//    app_pc app_pc2 = dr_fragment_app_pc(tag);
//
//    if (!is_user_module(app_pc1))
//    {
//        return DR_EMIT_DEFAULT;
//    }
//
//    dr_insert_clean_call(drcontext, bb, instrlist_first(bb),
//        (void*)on_bb, false, 2,
//        OPND_CREATE_INTPTR(app_pc1),
//        OPND_CREATE_INTPTR(tag));
//    return DR_EMIT_DEFAULT;
//}

static void on_module_load(void* drcontext, const module_data_t* info, bool loaded) {
    // info->start ~ info->end がレンジ、info->full_path がパス
    uint64_t base = (uint64_t)info->start;
    uint64_t size = (uint64_t)((byte*)info->end - (byte*)info->start);

    //if()
    std::string path(info->full_path);

    if (path.ends_with(".exe"))
    {
        g_exe_start = info->start;
        g_exe_end = info->end;
        dr_printf("bbtrace-ipc: (g_exe_start, g_exe_end) set to (%llu, %llu)\n", g_exe_start, g_exe_end);
    }
    dr_printf("bbtrace-ipc: on_module_load: base: %lld, size: %u, path: %s\n", base, size, info->full_path);

    ModEvent ev = {};
    ev.pid = (uint32_t)dr_get_process_id();
    ev.base = (uint64_t)info->start;
    ev.size = (uint64_t)((byte*)info->end - (byte*)info->start);
    ev.path_len = 0; // まずは送らない（必要なら別経路）
    EventSlot data;
    data.type = EV_MOD_ADD;
    data.mod = ev;
    // 

    if (!g_shm)
    {
        //dr_printf("bbtrace-ipc: on_module_load event dropped !! : g_shm==nullptr \n", base, size, info->full_path);
        dr_printf("bbtrace-ipc: on_module_load event pending : g_shm==nullptr \n");

        PendingData modData;
        modData.data = data.mod;
        modData.modulePath = std::string(info->full_path);
        modData.data.pathIndex = g_charStart;
        g_charStart += modData.modulePath.size() + 1;
        modData.data.path_len = modData.modulePath.size();
        g_pendingq.push_back(modData);
    }
    else
    {
        dr_printf("bbtrace-ipc: on_module_load event spsc_push : \n");
        spsc_push(&g_shm->ring_evt, g_shm->buf_evt, data);

    }
    // ここで共有メモリの ring_cmd に「MODULE_ADD」を送る
    // 例：type=CMD_MOD_ADD, base, size, path を格納
}

static void on_module_unload(void* drcontext, const module_data_t* info)
{
    uint64_t base = (uint64_t)info->start;
    // type=CMD_MOD_DEL, base を送る（size/path は省略可）
}

// ====== exit ======
static void on_exit() {
    ipc_close();
    drmgr_exit();
}

static wchar_t g_channelW[128];
static void parse_args(int argc, const char* argv[]) {
    // 例: --channel=Local\bbtrace_shm_1234-5678-...
    for (int i = 0; i < argc; ++i) {
        if (strncmp(argv[i], "--channel", 9) == 0)
        {
            // ANSI→UTF-16 変換：初期化スレッド内でのみ Win32 を使う
            int needed = MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, nullptr, 0);
            if (needed > 0 && needed < (int)std::size(g_channelW)) {
                MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, g_channelW, needed);
            }
        }
    }
}

//static char g_channelW[128]; // W版で統一
//static void parse_args(int argc, const char* argv[]) {
//    // 例: --channel=Local\bbtrace_shm_1234-5678-...
//    for (int i = 0; i < argc; ++i)
//    {
//        dr_printf("argv[%d]: %s\n",i, argv[i]);
//        if (strncmp(argv[i], "--channel", 10) == 0)
//        {
//            dr_printf("--channel= matched\n");
//            dr_snprintf(g_channelW, sizeof(g_channelW), argv[i + 1]);
//            return;
//        }
//    }
//}

// ==== 起動：専用スレッドで Win32 初期化・ループを開始 ====
DR_EXPORT void dr_client_main(client_id_t, int argc, const char* argv[]) {
    dr_printf("bbtrace-ipc: dr_client_main\n");
    drmgr_init();
    parse_args(argc, argv);
    drmgr_register_module_load_event(on_module_load);
    drmgr_register_module_unload_event(on_module_unload);
    drmgr_register_exit_event(on_exit);

    dr_create_client_thread([](void*) {
        const wchar_t* name = (g_channelW[0] ? g_channelW : nullptr);
        //const char* name = (g_channelW[0] ? g_channelW : nullptr);
        //dr_printf("g_channelW: %s\n", g_channelW);
        
        ipc_init(name);

        dr_printf("ipc_init done\n");
        g_ipc_ready = 1;
        cmd_loop(nullptr);
        }, nullptr);

    drmgr_register_bb_instrumentation_event(nullptr, event_bb_insert, nullptr);
    dr_printf("bbtrace-ipc: started (pid=%d)\n", dr_get_process_id());
}
