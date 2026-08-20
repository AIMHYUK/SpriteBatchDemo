#pragma once

#include "Common.h"

#include <functional>

// Win32 창 하나를 감싼다.
//
// 이 클래스는 D3D11을 전혀 모른다. 창 크기가 바뀌면 콜백으로만 알리고,
// 그것을 받아 무엇을 할지는 바깥이 정한다. 의존 방향을 Window -> Renderer가
// 아니라 한쪽으로만 흐르게 두기 위한 선택이다.
class Window
{
public:
    Window() = default;
    ~Window();

    // 복사되면 HWND 소유권이 모호해지므로 막는다.
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // 클라이언트 영역이 정확히 width x height가 되도록 창을 만든다.
    bool Create(HINSTANCE hInstance, const wchar_t* title, UINT width, UINT height);

    void Show(int nCmdShow);

    // 쌓인 메시지를 전부 처리한다. 창이 닫혔으면 false를 돌려준다.
    // 반환값이 곧 "게임 루프를 계속 돌려도 되는가"이다.
    bool PumpMessages();

    HWND Handle() const { return m_hwnd; }
    UINT Width()  const { return m_width; }
    UINT Height() const { return m_height; }

    // 크기 변경 통지를 받을 대상을 등록한다. (width, height)는 클라이언트 영역 크기다.
    void SetResizeCallback(std::function<void(UINT, UINT)> callback)
    {
        m_onResize = std::move(callback);
    }

    // 키가 눌릴 때 통지를 받을 대상을 등록한다. 인자는 가상 키 코드(VK_SPACE 등).
    // Esc(종료)는 Window가 직접 처리하고, 나머지 키는 이 콜백으로 넘긴다.
    // "무슨 키로 무엇을 한다"는 게임 규칙이라 Renderer/main이 정하게 둔다.
    void SetKeyDownCallback(std::function<void(WPARAM)> callback)
    {
        m_onKeyDown = std::move(callback);
    }

private:
    // WndProc은 C 함수 포인터라서 멤버 함수를 직접 등록할 수 없다.
    // static 함수로 한 번 받아, HWND에 붙여둔 this로 실제 멤버 함수에 넘긴다.
    static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND  m_hwnd   = nullptr;
    UINT  m_width  = 0;
    UINT  m_height = 0;
    bool  m_closed = false;

    // 드래그로 창 크기를 바꾸는 동안에는 WM_SIZE가 수십 번 날아온다.
    // 그때마다 스왑체인을 다시 만들면 낭비이므로, 드래그가 끝난 뒤 한 번만 알린다.
    bool  m_resizing = false;

    std::function<void(UINT, UINT)> m_onResize;
    std::function<void(WPARAM)>     m_onKeyDown;
};
