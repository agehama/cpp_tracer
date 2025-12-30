#define NOGDI
#include <Windows.h>
#include <Siv3D.hpp> // Siv3D v0.6.16
#include "../trace_common.hpp"
#include "../utility.hpp"
#include "dia_session.hpp"

struct ModuleInfo
{
	uint64_t baseAddr = 0;
	uint64_t imageSize = 0;

	bool inRange(uint64_t address) const
	{
		return baseAddr <= address && address < baseAddr + imageSize;
	}
};

inline bool spscPop(RingHeader* h, EventArgs* buf, EventArgs& out)
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

inline bool spscPush(RingHeader* h, Command* buf, const Command& v)
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

struct LineRange
{
	uint32 startLine = 0;
	uint32 endLine = 0;
};

void Main()
{
	::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	const wchar_t* msdiaPath = L".\\dia_sdk\\amd64\\msdia140.dll";
	CComPtr<IDiaDataSource> src;
	if (FAILED(CreateDiaDataSource(msdiaPath, &src)))
	{
		CoUninitialize();
		throw Error{ Format(U"CreateDiaDataSource failed by error: ", GetLastError()) };
		return;
	}

	CComPtr<IDiaSession> ses;
	Optional<DWORD> processId;

	uint32_t channel = 0;
	bool running = false;
	ShmLayout* shm = nullptr;
	HANDLE hMap = nullptr;

	Optional<ModuleInfo> exeModuleInfo;

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

	std::mutex mutex;

	bool terminateRequest = false;
	auto readMessage = [&]()
		{
			while (!terminateRequest)
			{
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

								if (exeModuleInfo &&
									exeModuleInfo.value().inRange(data.app_pc) &&
									exeModuleInfo.value().inRange(data.app_pc_end))
								{
									if (VaToLine(ses, data.app_pc, srcPosBegin) &&
										VaToLine(ses, data.app_pc_end, srcPosEnd))
									{
										if (Unicode::FromWstring(srcPosBegin.file).ends_with(U"\\main.cpp"))
										{
											mutex.lock();

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


											mutex.unlock();
											Logger << U"SrcPos: " << Unicode::FromWstring(srcPosBegin.file) << U", [" << srcPosBegin.line << U", " << srcPosEnd.line << U"]";
										}
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
									exeModuleInfo = ModuleInfo
									{
										.baseAddr = data.base,
										.imageSize = data.size,
									};

									ses->put_loadAddress(data.base);
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

				//std::this_thread::sleep_for(std::chrono::milliseconds(0));
			}
		}
	;

	std::thread readMessageThread(readMessage);

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

				Console << U"start debug " << Unicode::FromWstring(targetAppPath);
				processId = StartDebug(targetAppPath, shmName);

				if (processId)
				{
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

					if (!OpenDiaForExe(targetAppPath.c_str(), src, ses))
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

		if (shm && 0 < shm->eventHeader.droppedCount)
		{
			Window::SetTitle(shm->eventHeader.droppedCount);
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

		mutex.lock();
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

		mutex.unlock();
		for (int xi = 0; xi < rulerCountX; ++xi)
		{
			const auto x = cellStartX + rulerWidth * xi;

			Rect(20, 20).setCenter(x, 10).draw(Color(230));
			font(xi * 10).drawAt(x, 10, Palette::Black);
		}
	}

	terminateRequest = true;
	readMessageThread.join();

	if (shm)
	{
		UnmapViewOfFile(shm);
	}
	if (hMap)
	{
		CloseHandle(hMap);
	}
}
