#define NOGDI
#include <Windows.h>
#include <Siv3D.hpp> // Siv3D v0.6.16

#pragma pack(push, 1)

/////////////////////////////////////
// Event: Client -> Viewer

enum EventType : uint16_t
{
	BasicBlockHit,
	ModuleAdd,
	ModuleDelete,
};

//struct EventBasicBlockArgs
//{
//	EventType type, _pad;
//	uint32_t pid;
//	uint32_t tid;
//	uint64_t timestamp_us;
//	uint64_t app_pc;
//	uint64_t count;
//};

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
	// 直後に UTF-16 のパス本体（可変長）を詰める設計でもOK
};

struct EventArgs
{
	uint16_t type;  // EV_*
	uint16_t _pad;  // アライン調整（pack(1)でもBBと互換を保つ）

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

struct ModuleInfo
{
	uint64_t baseAddr = 0;
	uint64_t imageSize = 0;
};
HashTable<std::string, ModuleInfo> moduleInfoTable;

static inline bool spscPop(RingHeader* h, EventArgs* buf, EventArgs& out)
{
	const uint32_t r = h->readIndex, w = h->writeIndex;
	if (r == w)
	{
		return false;
	}

	out = buf[r];
	_ReadWriteBarrier();
	h->readIndex = (r + 1) & (h->capacity - 1);

	return true;
}

static inline bool spscPush(RingHeader* h, Command* buf, const Command& v)
{
	const uint32_t w = h->writeIndex, r = h->readIndex;
	const uint32_t next = (w + 1) & (h->capacity - 1);
	if (next == r)
	{
		++h->droppedCount;
		return false;
	}

	buf[w] = v;
	_ReadWriteBarrier();
	h->writeIndex = next;

	return true;
}

// Windows のコマンドライン引数クォート規則に従ってクォートする
// 参考: MSDN "Parsing C Command-Line Arguments"
static std::wstring QuoteArg(const std::wstring& s) {
	bool need_quotes = s.empty() || s.find_first_of(L" \t\"") != std::wstring::npos;
	if (!need_quotes) return s;

	std::wstring out;
	out.push_back(L'"');
	size_t bs_count = 0;
	for (wchar_t ch : s) {
		if (ch == L'\\') {
			++bs_count; // バックスラッシュは後でまとめて処理
		}
		else if (ch == L'"') {
			// 直前の \ を2倍にしてから " をエスケープ
			out.append(bs_count * 2, L'\\');
			bs_count = 0;
			out.append(L"\\\"");
		}
		else {
			// 通常文字：溜まっていた \ をそのまま出す
			if (bs_count) { out.append(bs_count, L'\\'); bs_count = 0; }
			out.push_back(ch);
		}
	}
	// 終端の " の直前にある \ は2倍に
	if (bs_count) out.append(bs_count * 2, L'\\');
	out.push_back(L'"');
	return out;
}

static std::wstring JoinCmdline(const std::vector<std::wstring>& args) {
	// CreateProcess に渡すコマンドラインは「可変」バッファが必須
	std::wstring cmd;
	bool first = true;
	for (const auto& a : args) {
		if (!first) cmd.push_back(L' ');
		first = false;
		cmd += QuoteArg(a);
	}
	return cmd;
}

Optional<DWORD> StartDebug(const std::wstring& exeFilePath, const std::wstring& clientArg)
{
	const std::wstring drrunPath = L"../../external/DynamoRIO/bin64/drrun.exe";
	const std::wstring clientPath = LR"(../../trace_client/build/Release/trace_client.dll)";
	const std::wstring appPath = exeFilePath;

	std::vector<std::wstring> argv;
	argv.push_back(drrunPath);
	argv.push_back(L"-c");
	argv.push_back(clientPath);
	argv.push_back(L"--channel");
	argv.push_back(clientArg);
	argv.push_back(L"--");
	argv.push_back(appPath);

	std::wstring cmdline = JoinCmdline(argv);
	std::vector<wchar_t> cmdlineBuf(cmdline.begin(), cmdline.end());
	cmdlineBuf.push_back(L'\0');

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};

	BOOL ok = CreateProcessW(
		drrunPath.c_str(),
		cmdlineBuf.data(),
		nullptr, nullptr,
		FALSE,
		CREATE_UNICODE_ENVIRONMENT,
		nullptr,
		nullptr,
		&si, &pi
	);

	if (!ok)
	{
		Logger << U"CreateProcessW  failed";
		return none;
	}

	return pi.dwProcessId;
}

#include <rpc.h>
#pragma comment(lib ,"rpcrt4.lib")

std::wstring CreateUUID()
{
	UUID uuid;
	UuidCreate(&uuid);
	RPC_WSTR lpszUuid = NULL;
	::UuidToString(&uuid, &lpszUuid);
	const std::wstring uuidStr((LPCTSTR)lpszUuid);
	::RpcStringFree(&lpszUuid);
	return uuidStr;
}

#include <dia2.h>
#include <atlbase.h>
#pragma comment(lib, "diaguids.lib")

typedef HRESULT(STDAPICALLTYPE* PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID*);

HRESULT CreateDiaDataSource_NoReg(const wchar_t* msdiaPath, IDiaDataSource** out)
{
	*out = nullptr;
	HMODULE h = LoadLibraryW(msdiaPath);
	if (!h) return HRESULT_FROM_WIN32(GetLastError());

	auto pGet = reinterpret_cast<PFN_DllGetClassObject>(
		GetProcAddress(h, "DllGetClassObject"));
	if (!pGet) { FreeLibrary(h); return E_NOINTERFACE; }

	CComPtr<IClassFactory> cf;
	HRESULT hr = pGet(__uuidof(DiaSource), IID_IClassFactory, (void**)&cf);
	if (FAILED(hr)) { FreeLibrary(h); return hr; }

	hr = cf->CreateInstance(nullptr, __uuidof(IDiaDataSource), (void**)out);
	if (FAILED(hr))
	{
		switch (hr)
		{
		case S_OK:
			Logger << U"CoCreateInstance failed: S_OK";
			break;
		case REGDB_E_CLASSNOTREG:
			Logger << U"CoCreateInstance failed: REGDB_E_CLASSNOTREG";
			break;
		case CLASS_E_NOAGGREGATION:
			Logger << U"CoCreateInstance failed: CLASS_E_NOAGGREGATION";
			break;
		case E_NOINTERFACE:
			Logger << U"CoCreateInstance failed: E_NOINTERFACE";
			break;
		case E_POINTER:
			Logger << U"CoCreateInstance failed: E_POINTER";
			break;

		}
	}

	return hr;
}

struct DiaSession
{
	CComPtr<IDiaDataSource> src;
	CComPtr<IDiaSession>    ses;
	CComPtr<IDiaSymbol>     global;
};

bool OpenDiaForExe(const std::wstring& exePath,
				   const std::wstring& symbolPath, // e.g. L"srv*C:\\Symbols*https://msdl.microsoft.com/download/symbols"
				   DiaSession& out)
{
	/*HRESULT hr = CoCreateInstance(__uuidof(DiaSource), nullptr, CLSCTX_INPROC_SERVER,
								  __uuidof(IDiaDataSource), (void**)&out.src);
	if (FAILED(hr))
	{
		switch (hr)
		{
		case S_OK:
			Logger << U"CoCreateInstance failed: S_OK";
			break;
		case REGDB_E_CLASSNOTREG:
			Logger << U"CoCreateInstance failed: REGDB_E_CLASSNOTREG";
			break;
		case CLASS_E_NOAGGREGATION:
			Logger << U"CoCreateInstance failed: CLASS_E_NOAGGREGATION";
			break;
		case E_NOINTERFACE:
			Logger << U"CoCreateInstance failed: E_NOINTERFACE";
			break;
		case E_POINTER:
			Logger << U"CoCreateInstance failed: E_POINTER";
			break;

		}
		return false;
	}*/
	;

	//hr = out.src->loadDataForExe(exePath.c_str(), symbolPath.empty() ? nullptr : CW2A(symbolPath.c_str()), nullptr);
	HRESULT hr = out.src->loadDataForExe(exePath.c_str(), symbolPath.empty() ? nullptr : symbolPath.c_str(), nullptr);
	if (FAILED(hr))
	{
		Logger << U"loadDataForExe failed";
		return false;
	}

	hr = out.src->openSession(&out.ses);
	if (FAILED(hr))
	{
		Logger << U"openSession failed";
		return false;
	}

	hr = out.ses->get_globalScope(&out.global);
	if (FAILED(hr))
	{
		Logger << U"get_globalScope failed";
		return false;
	}

	GUID g{};
	DWORD age = 0;
	out.global->get_guid(&g);
	out.global->get_age(&age);

	Logger << U"pdb guid: " << g.Data1 << U", " << g.Data2 << U", " << g.Data3 << U", " << g.Data4;

	return SUCCEEDED(hr);
}

struct SrcPos
{
	std::wstring file;
	DWORD line = 0;
	DWORD col = 0;
};

bool QueryLineByRva(IDiaSession* ses, uint64_t pc, uint64_t module_base, SrcPos& out)
{
	DWORD rva = (DWORD)(pc - module_base);
	CComPtr<IDiaEnumLineNumbers> lines;
	HRESULT hr = ses->findLinesByRVA(rva, 16, &lines);
	if (FAILED(hr)) return false;

	CComPtr<IDiaSymbol> fn = nullptr;
	hr = ses->findSymbolByRVA(rva, SymTagFunction, &fn);

	if (FAILED(hr))
	{
		Logger << U"findSymbolByRVA failed";
		return false;
	}

	if (!fn)
	{
		Logger << U"fn nullptr";
		return false;
	}
	Logger << U"findSymbolByRVA success";

	CComPtr<IDiaLineNumber> ln; ULONG f = 0;
	while (lines->Next(1, &ln, &f) == S_OK)
	{
		DWORD r0 = 0, len = 0; ln->get_relativeVirtualAddress(&r0);
		ln->get_length(&len);
		if (rva >= r0 && rva < r0 + (len ? len : 1))
		{
			CComPtr<IDiaSourceFile> sf;
			ln->get_sourceFile(&sf);
			BSTR b = nullptr;
			sf->get_fileName(&b);
			ULONG lnno = 0, col = 0;
			ln->get_lineNumber(&lnno);
			ln->get_columnNumber(&col);
			out.file = b ? b : L"";
			if (b) SysFreeString(b);
			out.line = (DWORD)lnno;
			out.col = (DWORD)col;
			return true;
		}
		ln.Release();
	}
	return false;
}

bool VaToSourceLine(IDiaSession* ses, uint64_t va, SrcPos& out)
{
	CComPtr<IDiaEnumLineNumbers> lines;

	static ULONGLONG val = 0;
	if (val == 0)
	{
		//ses->get_loadAddress(&val);
		//Logger << U"loadaddr : " << val;
		//Logger << U"app_pc : " << va;
	}


	//for (int i = 0; i < 50; ++i)
	auto isExe = false;
	uint64_t module_base = 0;
	{
		//const uint64_t addr = va + 32 * i - 64*10;
		const uint64_t addr = va;
		for (const auto& [path, info] : moduleInfoTable)
		{
			if (info.baseAddr <= addr && addr < info.baseAddr + info.imageSize)
			{
				//Logger << U"addr from " << Unicode::FromUTF8(path);

				if (path.ends_with(std::string(".exe")))
				{
					module_base = info.baseAddr;
					isExe = true;
				}
				break;
			}
		}
	}

	if (!isExe)
	{
		return false;
	}
	//Logger << U"exe app_pc : " << va;

	//return QuerySourceLineRich(ses, va, module_base, out);
	return QueryLineByRva(ses, va, module_base, out);

	//if (FAILED(ses->findLinesByVA(va, /*length*/1, &lines)))
	//{
	//	Logger << U"findLinesByVA";
	//	return false;
	//}

	//CComPtr<IDiaLineNumber> ln;
	//ULONG fetched = 0;
	//HRESULT hr = lines->Next(1, &ln, &fetched);
	//if (FAILED(hr) || fetched == 0)
	//{
	//	Logger << U"lines->Next: " << hr;
	//	return false;
	//}

	//CComPtr<IDiaSourceFile> sf;
	//if (FAILED(ln->get_sourceFile(&sf)))
	//{
	//	Logger << U"get_sourceFile";
	//	return false;
	//}

	//BSTR bpath = nullptr;
	//if (FAILED(sf->get_fileName(&bpath)))
	//{
	//	Logger << U"get_fileName";
	//	return false;
	//}

	//DWORD lnnum = 0, col = 0;
	//ln->get_lineNumber(&lnnum);
	//ln->get_columnNumber(&col);

	//out.file = bpath;
	//SysFreeString(bpath);
	//out.line = lnnum;
	//out.col = col;
	return true;
}

bool QueryLine(IDiaSession* ses, uint64_t pc, uint64_t moduleBase, uint64_t imageSize, SrcPos& out)
{
	// exeのアドレス範囲外
	if (pc < moduleBase || moduleBase + imageSize <= pc) return false;

	DWORD rva = (DWORD)(pc - moduleBase);
	CComPtr<IDiaEnumLineNumbers> lines;
	HRESULT hr = ses->findLinesByRVA(rva, 16, &lines);
	if (FAILED(hr)) return false;

	CComPtr<IDiaSymbol> fn = nullptr;
	hr = ses->findSymbolByRVA(rva, SymTagFunction, &fn);
	if (FAILED(hr) || !fn)return false;

	CComPtr<IDiaLineNumber> ln;
	ULONG f = 0;
	while (lines->Next(1, &ln, &f) == S_OK)
	{
		DWORD r0 = 0, len = 0;
		ln->get_relativeVirtualAddress(&r0);
		ln->get_length(&len);
		if (rva >= r0 && rva < r0 + (len ? len : 1))
		{
			CComPtr<IDiaSourceFile> sf;
			ln->get_sourceFile(&sf);
			BSTR b = nullptr;
			sf->get_fileName(&b);
			ULONG lnno = 0, col = 0;
			ln->get_lineNumber(&lnno);
			ln->get_columnNumber(&col);
			out.file = b ? b : L"";
			if (b) SysFreeString(b);
			out.line = (DWORD)lnno;
			out.col = (DWORD)col;
			return true;
		}
		ln.Release();
	}
	return false;
}

// 末尾一致（大文字小文字無視）ヘルパ
static bool PathEndsWithI(const std::wstring& full, const std::wstring& tail) {
	if (tail.size() > full.size()) return false;
	auto* p = full.c_str() + (full.size() - tail.size());
	return _wcsicmp(p, tail.c_str()) == 0;
}

// (file,line) → [RVA範囲]×N （インラインや最適化で複数返る想定）
bool FileLineToRvaRanges(IDiaSession* ses,
						 const std::wstring& wantPath, DWORD wantLine,
						 std::vector<std::pair<DWORD, DWORD>>& outRvaRanges)
{
	outRvaRanges.clear();

	// 1) まず全 compiland（翻訳単位）を列挙
	CComPtr<IDiaSymbol> global;
	if (FAILED(ses->get_globalScope(&global))) return false;

	CComPtr<IDiaEnumSymbols> compis;
	if (FAILED(global->findChildren(SymTagCompiland, nullptr, nsNone, &compis)))
	{
		return false;
	}

	// 2) compiland ごとに、その TU 内の “対象ファイル” を探す
	CComPtr<IDiaSymbol> compi;
	ULONG fetched = 0;
	while (SUCCEEDED(compis->Next(1, &compi, &fetched)) && fetched == 1)
	{
		// この compiland 内でファイル名を引く
		CComPtr<IDiaEnumSourceFiles> files;
		if (SUCCEEDED(ses->findFile(compi, wantPath.c_str(), nsCaseInsensitive, &files)))
		{
			CComPtr<IDiaSourceFile> sf;
			ULONG f2 = 0;
			while (SUCCEEDED(files->Next(1, &sf, &f2)) && f2 == 1)
			{
				// パス表記の差異を吸収（末尾一致で確認）
				BSTR bname = nullptr;
				if (FAILED(sf->get_fileName(&bname))) { sf.Release(); continue; }
				std::wstring got = bname; SysFreeString(bname);
				if (!PathEndsWithI(got, wantPath)) { sf.Release(); continue; }

				// 3) compiland + file の全行情報を取得
				CComPtr<IDiaEnumLineNumbers> lines;
				if (FAILED(ses->findLines(compi, sf, &lines))) { sf.Release(); continue; }

				// 4) その中から wantLine に一致する行を抜き出し、RVAと長さを収集
				CComPtr<IDiaLineNumber> ln;
				ULONG gotLn = 0;
				while (SUCCEEDED(lines->Next(1, &ln, &gotLn)) && gotLn == 1) {
					ULONG lnnum = 0;
					if (SUCCEEDED(ln->get_lineNumber(&lnnum)) && lnnum == wantLine) {
						DWORD rva = 0, len = 0;
						ln->get_relativeVirtualAddress(&rva);
						ln->get_length(&len);
						if (len == 0) len = 1; // 保険
						outRvaRanges.emplace_back(rva, rva + len);
					}
					ln.Release();
				}
				sf.Release();
			}
		}
		compi.Release();
	}

	// ※最適化やインラインで複数範囲になるのが普通。呼び出し側で base + rva にして送る。
	return !outRvaRanges.empty();
}

struct LineRange
{
	uint32 startLine = 0;
	uint32 endLine = 0;
};

void Main()
{
	::CoInitializeEx(NULL, COINIT_MULTITHREADED);

	//CComPtr<IDiaDataSource> src;
	DiaSession diaSession;
	HRESULT hr = CreateDiaDataSource_NoReg(L".\\dia_sdk\\amd64\\msdia140.dll", &diaSession.src);
	if (FAILED(hr))
	{
		Logger << U"CreateDiaDataSource_NoReg failed";
	}
	Logger << U"test";
	Optional<DWORD> processId;

	uint32_t channel = 0;
	bool running = false;
	ShmLayout* shm = nullptr;
	HANDLE hMap = nullptr;
	//diaSession.src = src;

	Optional<DWORD64> baseOpt;
	Optional<DWORD64> sizeOpt;

	HANDLE hFile;
	HANDLE hFileMapping;
	LPVOID pvView;
	IMAGE_DOS_HEADER* pDosHeader;
	uint64 readCount = 0;

	bool loaded = false;
	Array<String> lines;
	Font font(12);
	int lineMargin = 20;
	Array<int> lineCount(300, 0);

	Scene::SetBackground(Palette::White);

	std::map<uint32, LineRange> basicBlockLinesDef;
	const auto updateLinesDef = [&basicBlockLinesDef]()
		{
			if (basicBlockLinesDef.size() < 2)
			{
				return;
			}

			auto it = basicBlockLinesDef.begin();
			while (true)
			{
				auto nextIt = std::next(it);
				if (nextIt == basicBlockLinesDef.end())
				{
					break;
				}

				// ブロックの終端が次のブロックと被る場合は、ブロックの開始行の方を優先して、前の要素の範囲を切り詰める
				it->second.endLine = Min(it->second.endLine, nextIt->second.startLine - 1);

				it = nextIt;
			}
		};
	;
	Array<uint32> lineHits;

	int32 topLine = 0;
	Console << U"console";

	Window::Resize(1280, 720);
	while (System::Update())
	{
		if (DragDrop::HasNewFilePaths())
		{
			const auto items = DragDrop::GetDroppedFilePaths();
			const auto filepath = items[0].path;
			if (FileSystem::Exists(filepath) && FileSystem::Extension(filepath) == U"exe")
			{
				auto targetAppPath = Unicode::ToWstring(filepath);

				const auto uuidStr = CreateUUID();
				wchar_t shmName[128];
				swprintf_s(shmName, L"Local\\bbtrace_shm_%ls", uuidStr.c_str());
				channel = (shmName[0] << 16) + shmName[1];

				processId = StartDebug(targetAppPath, shmName);

				//Logger << Unicode::FromWstring(shmName);

				if (processId)
				{
					//char shmName[128];
					//sprintf_s(shmName, "Local\\bbtrace_shm_%u", processId.value());
					//Logger << U"connecting shm: " << Unicode::FromWstring(shmName);

					// DynamoRIOの起動待機
					for (int i = 0; i < 300; ++i)
					{
						hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmName);
						if (hMap) break;
						Sleep(100);
					}
					if (!hMap)
					{
						Logger << U"OpenFileMapping failed (name not found)";
						continue;
					}

					if (!OpenDiaForExe(targetAppPath, {}, diaSession))
					{
						Logger << U"OpenDiaForExe failed: " << GetLastError();
						continue;
					}

					shm = (ShmLayout*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
					if (!shm)
					{
						Logger << U"MapViewOfFile failed: " << GetLastError();
						continue;
					}

					if (shm->header.magic != 0x52544252 || shm->header.channel != channel)
					{
						Logger << U"shm header mismatch (magic/pid)";
						continue;
					}

					Logger << U"connected. cap_evt=" << shm->header.eventsCapacity << U" cap_cmd=" << shm->header.commandsCapacity;

					Logger << U"commands: add <base hex> <begin_rva hex> <end_rva hex> | clear | quit";
					running = true;
				}
			}
		}

		if (KeyD.down())
		{
			Logger << U"eventHeader dropped : " << shm->eventHeader.droppedCount;
			Logger << U"commandHeader dropped: " << shm->commandHeader.droppedCount;
			Logger << U"readCount: " << readCount;
		}

		if (running)
		{
			// 受信（A->B）
			EventArgs ev;
			while (spscPop(&shm->eventHeader, shm->eventBuffer, ev))
			{
				int cnt = 0;
				if (++cnt <= 8)
				{
					switch (ev.type)
					{
					case EventType::BasicBlockHit:
					{
						BBEvent& data = ev.bb;
						++readCount;

						/*Logger << U"BB pc=0x" << std::hex << ev.bb.app_pc
							<< U" tid=" << std::dec << ev.bb.tid
							<< U" ts(us)=" << ev.bb.timestamp_us;*/

						SrcPos srcPosBegin = {};
						SrcPos srcPosEnd = {};

						if (baseOpt && sizeOpt && QueryLine(diaSession.ses, data.app_pc, baseOpt.value(), sizeOpt.value(), srcPosBegin) && QueryLine(diaSession.ses, data.app_pc_end, baseOpt.value(), sizeOpt.value(), srcPosEnd))
						{
							if (Unicode::FromWstring(srcPosBegin.file).ends_with(U"\\main.cpp"))
							{
								if (!loaded)
								{
									const auto filepath = Unicode::FromWstring(srcPosBegin.file);
									if (FileSystem::Exists(filepath))
									{
										TextReader reader(filepath);
										reader.readLines(lines);
										loaded = true;
									}
								}

								if (srcPosBegin.line < lineCount.size())
								{
									//lineCount[srcPosBegin.line-1]++;

									const auto beginLine = srcPosBegin.line - 1;
									const auto endLine = srcPosEnd.line - 1;
									//const auto key = beginLine << 16 + endLine;

									auto& range = basicBlockLinesDef[beginLine];
									//if (range.startLine != beginLine || range.endLine != endLine)
									if (range.startLine == 0 || range.endLine == 0)
									{
										range.startLine = beginLine;
										range.endLine = endLine;
										updateLinesDef();

										//topLine = static_cast<int>(beginLine) - 20;
									}

									lineHits.push_back(beginLine);

									/*auto& blockData = blockHit[key];

									blockData.startLine = beginLine;
									blockData.endLine = endLine;
									++blockData.hitCount;*/
								}
								Logger << U"SrcPos: " << Unicode::FromWstring(srcPosBegin.file) << U", [" << srcPosBegin.line << U", " << srcPosEnd.line << U"]";
							}
						}
						else
						{
							//Logger << U"VaToSourceLine failed: ";
						}
					}
					break;
					case EventType::ModuleAdd:
					{
						ModEvent& data = ev.mod;

						//const std::string str(&shm->strBuffer[data.pathIndex], shm->strBuffer + data.pathIndex + data.path_len);
						const std::string str(&shm->strBuffer[data.pathIndex], data.path_len);
						//Logger << U"module add data.pathIndex: " << data.pathIndex << U", data.path_len: " << data.path_len;
						Logger << U"module path: " << Unicode::FromUTF8(str) << U", base: " << data.base;

						if (str.ends_with(std::string(".exe")))
						{
							auto& modInfo = moduleInfoTable[str];
							modInfo.baseAddr = data.base;
							modInfo.imageSize = data.size;

							baseOpt = data.base;
							sizeOpt = data.size;
						}
					}
					break;
					case EventType::ModuleDelete:
						break;

					default:
						break;
					};
				}
			}

			// 非ブロッキング入力（簡易版）
			if (GetAsyncKeyState(VK_RETURN) & 0x8000) {
				// Enter を押したら行入力
				std::string line;
				std::cout << "> ";
				std::getline(std::cin, line);
				if (line == "quit" || line == "q") { running = false; break; }
				if (line == "clear") {
					Command c{};
					c.type = CMD_CLEAR_RANGES;
					c.rangeCount = 0;
					if (spscPush(&shm->commandHeader, shm->commandBuffer, c))
					{
						Logger << U"sent CLEAR";
					}
					else
					{
						Logger << U"cmd ring full";
					}
				}
				else if (line.rfind("add ", 0) == 0)
				{
					unsigned long long base, beg, end;
					if (sscanf_s(line.c_str() + 4, "%llx %llx %llx", &base, &beg, &end) == 3)
					{
						Command c{};
						c.type = CMD_ADD_RANGES;
						c.rangeCount = 1;
						c.ranges[0].base = base;
						c.ranges[0].beginRva = beg;
						c.ranges[0].endRva = end;
						if (spscPush(&shm->commandHeader, shm->commandBuffer, c))
						{
							Logger << U"sent ADD";
						}
						else
						{
							Logger << U"cmd ring full";
						}
					}
					else {
						Logger << U"usage: add <base hex> <begin_rva hex> <end_rva hex>";
					}
				}
				else {
					Logger << U"unknown cmd";
				}
			}
		}

		topLine += Mouse::Wheel();
		topLine = Max(0, topLine);

		const auto bottomLine = topLine + 50;

		/*
		for (const auto& [key, val] : blockHit)
		{
			if (bottomLine <= val.startLine || val.endLine < topLine)
			{
				continue;
			}
			const auto yBegin = (val.startLine - topLine) * lineMargin;
			const auto yEnd = ((val.endLine + 1) - topLine) * lineMargin;
			const float b = Saturate(val.hitCount / 1000.0f);
			Rect(0, yBegin, Scene::Width(), yEnd - yBegin).drawFrame(1, ColorF(Palette::Red, b));
		}
		*/

		const int leftMargin = 50;

		int cellWidth = 12;
		int cellStartX = leftMargin + 0;
		int cellCountX = (Scene::Width() - cellStartX) / cellWidth;

		for (const auto& [key, val] : basicBlockLinesDef)
		{
			if (bottomLine <= val.startLine || val.endLine < topLine)
			{
				continue;
			}

			const auto yBegin = (val.startLine - topLine) * lineMargin;
			const auto yEnd = ((val.endLine + 1) - topLine) * lineMargin;

			for (int xi = 0; xi < cellCountX; ++xi)
			{
				const auto x = cellStartX + cellWidth * xi;
				Rect(x, yBegin, cellWidth, yEnd - yBegin).stretched(-1).drawFrame(0.5, Color(200));
			}
		}

		int rulerWidth = cellWidth * 10;
		int rulerCountX = 1 + (Scene::Width() - cellStartX) / rulerWidth;
		for (int xi = 0; xi < rulerCountX; ++xi)
		{
			const auto x = cellStartX + rulerWidth * xi;
			Line(x, 0, x, Scene::Height()).draw(1.0, Color(160));
		}

		uint32 currentXIndex = 0;
		uint32 lastHitLine = 0;
		for (const auto hitLine : lineHits)
		{
			if (hitLine < lastHitLine)
			{
				++currentXIndex;
			}

			lastHitLine = hitLine;

			const auto& val = basicBlockLinesDef[hitLine];
			if (bottomLine <= val.startLine || val.endLine < topLine)
			{
				continue;
			}

			const auto yBegin = (val.startLine - topLine) * lineMargin;
			const auto yEnd = ((val.endLine + 1) - topLine) * lineMargin;

			const int xi = currentXIndex;
			{
				const auto x = cellStartX + cellWidth * xi;
				Rect(x, yBegin, cellWidth, yEnd - yBegin).stretched(-1).draw(HSV(104, 0.27, 1.0));
			}
		}

		for (int32 i = 0; i < 50; ++i)
		{
			const auto lineIndex = topLine + i;
			if (static_cast<int32>(lines.size()) <= lineIndex)
			{
				break;
			}

			const auto& currentLineStr = lines[lineIndex];
			const auto y = lineMargin * i;
			/*
			const float b = Saturate(lineCount[lineIndex] / 1000.0f);
			Rect(0, y, Scene::Width(), lineMargin).draw(ColorF(Palette::Red, b));*/
			font(currentLineStr).draw(leftMargin, y, Palette::Black);

			font(U"{:0>4}"_fmt(lineIndex)).draw(5, y, Palette::Gray);
		}

		for (int xi = 0; xi < rulerCountX; ++xi)
		{
			const auto x = cellStartX + rulerWidth * xi;

			Rect(20, 20).setCenter(x, 10).draw(Color(230));
			font(xi * 10).drawAt(x, 10, Palette::Black);
		}
	}

	if (shm)
	{
		UnmapViewOfFile(shm);
	}
	if (hMap)
	{
		CloseHandle(hMap);
	}
}
