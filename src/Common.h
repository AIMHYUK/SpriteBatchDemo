#pragma once

// Windows 헤더가 min/max 매크로를 정의해 std::min/std::max와 충돌하는 것을 막는다.
#define NOMINMAX
// 잘 안 쓰는 Windows API를 걷어내 헤더 파싱 시간을 줄인다.
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <wrl/client.h>

#include <string>

// D3D11 인터페이스는 전부 COM 객체다. 참조 카운트를 직접 AddRef/Release 하는 대신
// ComPtr에 맡긴다. 스코프를 벗어나면 자동으로 Release가 불린다.
template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// HRESULT를 검사한다. 실패하면 어느 파일 몇 번째 줄에서 무엇이 실패했는지
// 메시지 박스로 알리고 false를 돌려준다.
//
// 예외를 던지지 않는 이유: 이 프로그램에서 HRESULT 실패는 대부분 복구 불가능한
// 초기화 실패이고, 호출부가 곧바로 false를 반환하며 종료하는 것이 가장 단순하다.
bool CheckHR(HRESULT hr, const wchar_t* what, const wchar_t* file, int line);

// 호출부에서 파일명·줄번호를 자동으로 넘기기 위한 매크로.
#define HR_CHECK(expr, what) CheckHR((expr), (what), __FILEW__, __LINE__)

// 실행 파일이 있는 폴더를 기준으로 상대 경로를 절대 경로로 만든다.
//
// 왜 필요한가: 셰이더를 파일에서 읽는데, 프로그램의 "현재 작업 폴더"는
// 어디서 실행했느냐에 따라 달라진다. Visual Studio에서 F5로 돌리면 프로젝트 폴더,
// 탐색기에서 더블클릭하면 exe 폴더가 된다. 그래서 작업 폴더에 기대지 않고
// exe 위치를 직접 구해서 붙인다. 제출용으로 폴더째 압축해 넘길 때도 이래야 안전하다.
std::wstring ResolvePathFromExe(const wchar_t* relativePath);
