#pragma once
#include <string>
#include <rpc.h>
#pragma comment(lib ,"rpcrt4.lib")

// Windows のコマンドライン引数クォート規則に従ってクォートする
// 参考: MSDN "Parsing C Command-Line Arguments"
inline std::wstring QuoteArg(const std::wstring& s)
{
	bool need_quotes = s.empty() || s.find_first_of(L" \t\"") != std::wstring::npos;
	if (!need_quotes) return s;

	std::wstring out;
	out.push_back(L'"');
	size_t bs_count = 0;
	for (wchar_t ch : s)
	{
		if (ch == L'\\')
		{
			++bs_count; // バックスラッシュは後でまとめて処理
		}
		else if (ch == L'"')
		{
			// 直前の \ を2倍にしてから " をエスケープ
			out.append(bs_count * 2, L'\\');
			bs_count = 0;
			out.append(L"\\\"");
		}
		else
		{
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

inline std::wstring JoinCmdline(const std::vector<std::wstring>& args)
{
	std::wstring cmd;
	bool first = true;
	for (const auto& a : args) {
		if (!first) cmd.push_back(L' ');
		first = false;
		cmd += QuoteArg(a);
	}
	return cmd;
}

inline std::wstring CreateUUID()
{
	UUID uuid;
	UuidCreate(&uuid);
	RPC_WSTR lpszUuid = NULL;
	::UuidToString(&uuid, &lpszUuid);
	const std::wstring uuidStr((LPCTSTR)lpszUuid);
	::RpcStringFree(&lpszUuid);
	return uuidStr;
}

// 末尾一致（大文字小文字無視）ヘルパ
inline bool PathEndsWithI(const std::wstring& full, const std::wstring& tail)
{
	if (tail.size() > full.size()) return false;
	auto* p = full.c_str() + (full.size() - tail.size());
	return _wcsicmp(p, tail.c_str()) == 0;
}