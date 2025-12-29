#pragma once
#include <dia2.h>
#include <atlbase.h>

#pragma comment(lib, "diaguids.lib")

HRESULT CreateDiaDataSource(const wchar_t* msdiaDllPath, IDiaDataSource** outSrc)
{
    *outSrc = nullptr;

    // msdia140.dll を読み込む
    HMODULE h = LoadLibraryW(msdiaDllPath);
    if (!h) return HRESULT_FROM_WIN32(GetLastError());

    using DllGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto getClassObject = reinterpret_cast<DllGetClassObjectFn>(
        GetProcAddress(h, "DllGetClassObject"));
    if (!getClassObject) { FreeLibrary(h); return E_NOINTERFACE; }

    // COM インターフェースを用いて DiaDataSource のインスタンスを作る
    CComPtr<IClassFactory> factory;
    HRESULT hr = getClassObject(__uuidof(DiaSource), IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { FreeLibrary(h); return hr; }

    return factory->CreateInstance(nullptr, __uuidof(IDiaDataSource), (void**)outSrc);
}

bool OpenDiaForExe(const wchar_t* exePath, const CComPtr<IDiaDataSource>& src, CComPtr<IDiaSession>& outSes)
{
    // loadDataForExe を呼ぶと EXE に紐づけられた PDB が自動的に読み込まれる
    if (FAILED(src->loadDataForExe(exePath, nullptr, nullptr))) return false;
    if (FAILED(src->openSession(&outSes))) return false;

    return true;
}

struct SrcPos
{
	std::wstring file;
	DWORD line = 0;
	DWORD col = 0;
};

bool VaToLine(IDiaSession* ses, ULONGLONG va, SrcPos& out)
{
    out = {};

    // findLinesByVA: アドレスvaに対応する行情報のエントリを列挙する
    CComPtr<IDiaEnumLineNumbers> e;
    if (FAILED(ses->findLinesByVA(va, 1, &e))) return false;

    // エントリの最初の要素を取得
    CComPtr<IDiaLineNumber> ln;
    ULONG fetched = 0;
    if (e->Next(1, &ln, &fetched) != S_OK || fetched == 0) return false;

    // 行情報からソースファイル情報を取得
    CComPtr<IDiaSourceFile> sf;
    if (FAILED(ln->get_sourceFile(&sf))) return false;

    // ソースファイルのパスを取得
    BSTR b = nullptr;
    if (FAILED(sf->get_fileName(&b))) return false;
    out.file = b ? b : L"";
    SysFreeString(b);

    // 行番号を取得
    DWORD line = 0;
    if (FAILED(ln->get_lineNumber(&line))) return false;
    out.line = line;

    return true;
}

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

struct ModuleInfo
{
	uint64_t baseAddr = 0;
	uint64_t imageSize = 0;
};
HashTable<std::string, ModuleInfo> moduleInfoTable;

bool VaToSourceLine(IDiaSession* ses, uint64_t va, SrcPos& out)
{
	CComPtr<IDiaEnumLineNumbers> lines;

	auto isExe = false;
	uint64_t module_base = 0;
	const uint64_t addr = va;
	for (const auto& [path, info] : moduleInfoTable)
	{
		if (info.baseAddr <= addr && addr < info.baseAddr + info.imageSize)
		{
			if (path.ends_with(std::string(".exe")))
			{
				module_base = info.baseAddr;
				isExe = true;
			}
			break;
		}
	}

	if (!isExe)
	{
		return false;
	}

	return QueryLineByRva(ses, va, module_base, out);
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
				while (SUCCEEDED(lines->Next(1, &ln, &gotLn)) && gotLn == 1)
				{
					ULONG lnnum = 0;
					if (SUCCEEDED(ln->get_lineNumber(&lnnum)) && lnnum == wantLine)
					{
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