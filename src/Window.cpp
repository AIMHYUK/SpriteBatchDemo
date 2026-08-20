#include "Window.h"

namespace
{
    constexpr const wchar_t* kWindowClassName = L"SpriteBatchDemoWindowClass";
}

Window::~Window()
{
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool Window::Create(HINSTANCE hInstance, const wchar_t* title, UINT width, UINT height)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    // 창 크기가 바뀔 때 내용을 다시 그리도록 요청한다.
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &Window::WndProcStatic;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    // 배경 브러시를 두지 않는다. 매 프레임 D3D가 전체를 덮어 그리므로
    // GDI가 먼저 흰색으로 칠하면 깜빡임만 생긴다.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&wc))
    {
        MessageBoxW(nullptr, L"윈도우 클래스 등록에 실패했습니다.", L"SpriteBatchDemo", MB_OK | MB_ICONERROR);
        return false;
    }

    // CreateWindow에 넘기는 크기는 테두리와 제목 표시줄을 포함한 바깥 크기다.
    // 우리가 원하는 건 그림이 그려지는 안쪽(클라이언트) 크기이므로,
    // AdjustWindowRect로 테두리 두께만큼 부풀린 값을 구해서 넘긴다.
    // 이 보정을 빼먹으면 1280x720을 요청했는데 실제 렌더 타깃은 1264x681 같은 값이 된다.
    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rect{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRect(&rect, style, FALSE);

    m_hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        title,
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr, nullptr, hInstance,
        this);  // 마지막 인자가 WM_NCCREATE에서 lpCreateParams로 돌아온다.

    if (!m_hwnd)
    {
        MessageBoxW(nullptr, L"창 생성에 실패했습니다.", L"SpriteBatchDemo", MB_OK | MB_ICONERROR);
        return false;
    }

    m_width  = width;
    m_height = height;
    return true;
}

void Window::Show(int nCmdShow)
{
    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);
}

bool Window::PumpMessages()
{
    MSG msg{};

    // GetMessage가 아니라 PeekMessage를 쓴다.
    // GetMessage는 메시지가 올 때까지 스레드를 재운다. 그래도 되는 건 문서 편집기처럼
    // 입력이 있을 때만 다시 그리면 되는 프로그램이고, 렌더러는 입력이 없어도
    // 계속 다음 프레임을 그려야 하므로 큐가 비었으면 그냥 빠져나와야 한다.
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
            m_closed = true;

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return !m_closed;
}

LRESULT CALLBACK Window::WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // 창이 만들어지는 첫 메시지(WM_NCCREATE)에서 this 포인터를 HWND에 붙여둔다.
    // 이후 메시지부터는 그것을 꺼내 멤버 함수로 넘긴다.
    if (msg == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<Window*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    }

    if (auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
        return self->WndProc(hwnd, msg, wParam, lParam);

    // WM_NCCREATE보다 먼저 오는 메시지가 몇 개 있다. 그때는 기본 처리로 넘긴다.
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
    {
        const UINT width  = LOWORD(lParam);
        const UINT height = HIWORD(lParam);

        // 최소화하면 0x0이 온다. 크기 0인 스왑체인은 만들 수 없으므로 무시한다.
        if (wParam == SIZE_MINIMIZED || width == 0 || height == 0)
            return 0;

        m_width  = width;
        m_height = height;

        // 드래그 중에는 알리지 않는다. 놓는 순간 WM_EXITSIZEMOVE에서 한 번만 알린다.
        if (!m_resizing && m_onResize)
            m_onResize(m_width, m_height);

        return 0;
    }

    case WM_ENTERSIZEMOVE:
        m_resizing = true;
        return 0;

    case WM_EXITSIZEMOVE:
        m_resizing = false;
        if (m_onResize)
            m_onResize(m_width, m_height);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        // 메시지 큐에 WM_QUIT을 넣는다. PumpMessages가 이것을 보고 루프를 끝낸다.
        PostQuitMessage(0);
        m_hwnd = nullptr;
        return 0;

    case WM_KEYDOWN:
        // Esc는 창을 닫는 일이라 Window가 직접 처리한다.
        if (wParam == VK_ESCAPE)
        {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        // 그 밖의 키는 바깥(main)이 정한 규칙으로 넘긴다.
        // 키를 누르고 있으면 자동 반복이 오지만, 토글은 눌린 그 순간에만 반응하면 되므로
        // 반복 여부(lParam bit 30)는 여기서 굳이 거르지 않는다. 필요해지면 그때 나눈다.
        if (m_onKeyDown)
            m_onKeyDown(wParam);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
