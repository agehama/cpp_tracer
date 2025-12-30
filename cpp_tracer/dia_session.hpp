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
