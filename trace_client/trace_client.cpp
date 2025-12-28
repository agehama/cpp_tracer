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

#include "../trace_common.hpp"

struct PendingData
{
    ModEvent data;
    std::string modulePath;
};

static inline bool spsc_push(RingHeader* h, EventArgs* buf, const EventArgs& v)
{
    const uint32_t cap = h->capacity;
    uint32_t w = h->writeIndex, r = h->readIndex;
    uint32_t next = (w + 1) & (cap - 1);
    if (next == r) { h->droppedCount++; return false; }
    buf[w] = v; 
    _ReadWriteBarrier();
    h->writeIndex = next;
    return true;
}

static inline bool spsc_pop(RingHeader* h, Command* buf, Command& out)
{
    uint32_t r = h->readIndex, w = h->writeIndex;
    if (r == w) return false;
    out = buf[r];
    _ReadWriteBarrier();
    h->readIndex = (r + 1) & (h->capacity - 1);
    return true;
}

static std::deque<PendingData> g_pendingq;
static ShmLayout* g_shm = nullptr;
static uint16_t g_charStart = 0;
static HANDLE g_hMap = nullptr;
static HANDLE g_evt_a2b = nullptr; // DR→Viewer
static HANDLE g_evt_b2a = nullptr; // Viewer→DR

static std::atomic<uint32_t> g_range_count{0};
static AddressRange g_ranges[256];

static void ipc_init(const wchar_t* channelNameOpt)
{
    const uint32_t pid = dr_get_process_id();

    g_hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)sizeof(ShmLayout), channelNameOpt);
    DWORD map_err = GetLastError();
    if (!g_hMap) { dr_printf("CFM failed: %lu\n", GetLastError()); return; }

    void* base = MapViewOfFile(g_hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmLayout));
    if (!base) { dr_printf("MVF failed: %lu\n", GetLastError()); return; }
    g_shm = (ShmLayout*)base;

    if (map_err != ERROR_ALREADY_EXISTS)
    {
        memset(g_shm, 0, sizeof(*g_shm));
        g_shm->header.magic = 0x52544252;
        g_shm->header.channel = (channelNameOpt[0] << 16) + channelNameOpt[1];
        g_shm->header.pid = pid;
        g_shm->header.eventsCapacity = 1u << 15;
        g_shm->header.commandsCapacity = 1024;
        g_shm->eventHeader.capacity = 1u << 15;
        g_shm->commandHeader.capacity = 1024;
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
        uint64_t b = g_ranges[i].base + g_ranges[i].beginRva;
        uint64_t e = g_ranges[i].base + g_ranges[i].endRva;
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
        uint32_t add = c.rangeCount;
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
        while (g_shm && spsc_pop(&g_shm->commandHeader, g_shm->commandBuffer, c))
            apply_command(c);
        dr_sleep(10); // 10ms 程度のポーリングで十分
    }
}

static volatile int g_ipc_ready = 0;
static size_t g_send_count = 0;

static void on_bb(void* drcontext, app_pc start, void* tag, app_pc end)
{
    if (!g_ipc_ready)
    {
        return;
    }

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

        EventArgs data;
        data.type = EventType::ModuleAdd;
        data.mod = modData.data;
        spsc_push(&g_shm->eventHeader, g_shm->eventBuffer, data);

        dr_printf("bbtrace-ipc: EventSlot send data : %d\n", data.type);
    }

    BBEvent ev = {};
    ev.pid  = dr_get_process_id();
    ev.tid  = (uint32_t)dr_get_thread_id(drcontext);
    ev.timestamp_us = (uint64_t)dr_get_microseconds();
    ev.app_pc = (uint64_t)start;
    ev.app_pc_end = (uint64_t)end;
    EventArgs data;
    data.type = BasicBlockHit;
    data.bb = ev;
    spsc_push(&g_shm->eventHeader, g_shm->eventBuffer, data);
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

static dr_emit_flags_t
event_bb_insert(void* drcontext, void* tag, instrlist_t* bb, instr_t* /*where*/,
                bool /*for_trace*/, bool /*translating*/, void* /*user*/)
{
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

static void on_module_load(void* drcontext, const module_data_t* info, bool loaded)
{
    // info->start ~ info->end がレンジ、info->full_path がパス
    uint64_t base = (uint64_t)info->start;
    uint64_t size = (uint64_t)((byte*)info->end - (byte*)info->start);

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
    EventArgs data;
    data.type = ModuleAdd;
    data.mod = ev;

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
        spsc_push(&g_shm->eventHeader, g_shm->eventBuffer, data);
    }
}

static void on_module_unload(void* drcontext, const module_data_t* info)
{
    uint64_t base = (uint64_t)info->start;
}

static void on_exit()
{
    ipc_close();
    drmgr_exit();
}

static wchar_t g_channelW[128];
static void parse_args(int argc, const char* argv[])
{
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

DR_EXPORT void dr_client_main(client_id_t, int argc, const char* argv[])
{
    dr_printf("bbtrace-ipc: dr_client_main\n");
    drmgr_init();
    parse_args(argc, argv);
    drmgr_register_module_load_event(on_module_load);
    drmgr_register_module_unload_event(on_module_unload);
    drmgr_register_exit_event(on_exit);

    dr_create_client_thread([](void*) {
        const wchar_t* name = (g_channelW[0] ? g_channelW : nullptr);
        
        ipc_init(name);

        dr_printf("ipc_init done\n");
        g_ipc_ready = 1;
        cmd_loop(nullptr);
        }, nullptr);

    drmgr_register_bb_instrumentation_event(nullptr, event_bb_insert, nullptr);
    dr_printf("bbtrace-ipc: started (pid=%d)\n", dr_get_process_id());
}
