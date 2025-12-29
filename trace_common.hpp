#pragma once
#define NOGDI
#include <Windows.h>

#pragma pack(push, 1)

/////////////////////////////////////
// Event: Client -> Viewer

enum EventType : uint16_t
{
	BasicBlockHit,
	ModuleAdd,
	ModuleDelete,
};

// type == EV_BB_HIT
struct BBEvent
{
	uint32_t pid;
	uint32_t tid;
	uint64_t timestamp_us;   // dr_get_microseconds()
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
};

struct EventArgs
{
	uint16_t type; // EV_*
	uint16_t _pad;

	union
	{
		BBEvent bb;
		ModEvent mod;
	};
};


/////////////////////////////////////


/////////////////////////////////////
// Command: Viewer -> Client

struct AddressRange
{
	uint64_t base;
	uint64_t beginRva, endRva;
};

enum CommandType : uint16_t
{
	CMD_ADD_RANGES = 0,
	CMD_CLEAR_RANGES = 1,
};

struct Command
{
	CommandType type;
	uint16_t rangeCount;
	//uint32_t reserve;
	AddressRange ranges[8];
};

/////////////////////////////////////


/////////////////////////////////////
// Client, Viewer 間の共有メモリ

struct RingHeader
{
	uint32_t capacity;
	uint32_t writeIndex;
	uint32_t readIndex;
	uint32_t droppedCount;
};

struct ShmHeader
{
	uint32_t magic;
	uint32_t channel;
	uint32_t pid;
	uint32_t eventsCapacity;
	uint32_t commandsCapacity;
};

/////////////////////////////////////


#pragma pack(pop)

struct ShmLayout
{
	ShmHeader				header;
	RingHeader				eventHeader;
	EventArgs				eventBuffer[1 << 15];
	RingHeader				commandHeader;
	Command					commandBuffer[1024];
	char					strBuffer[16384];
};
