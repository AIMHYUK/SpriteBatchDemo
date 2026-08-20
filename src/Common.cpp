#include "Common.h"

#include <comdef.h>
#include <cstdio>

bool CheckHR(HRESULT hr, const wchar_t* what, const wchar_t* file, int line)
{
    if (SUCCEEDED(hr))
        return true;

    // _com_error가 HRESULT를 사람이 읽을 수 있는 문장으로 바꿔준다.
    // 예: 0x887A0005 -> "The GPU device instance has been suspended..."
    const _com_error err(hr);

    wchar_t message[1024]{};
    swprintf_s(message,
               L"%s 실패\n\n"
               L"HRESULT : 0x%08X\n"
               L"내용    : %s\n"
               L"위치    : %s(%d)",
               what, static_cast<unsigned>(hr), err.ErrorMessage(), file, line);

    MessageBoxW(nullptr, message, L"SpriteBatchDemo", MB_OK | MB_ICONERROR);
    return false;
}

std::wstring ResolvePathFromExe(const wchar_t* relativePath)
{
    // 첫 인자에 nullptr을 주면 "지금 실행 중인 exe"의 전체 경로를 준다.
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring path(exePath);

    // 마지막 역슬래시까지만 남겨 폴더 경로로 만든다.
    // "C:\...\Release\App.exe" -> "C:\...\Release\"
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        path.resize(slash + 1);

    path += relativePath;
    return path;
}
