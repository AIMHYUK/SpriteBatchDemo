#include "Common.h"
#include "FrameTimer.h"
#include "Renderer.h"
#include "Window.h"

#include <cstdio>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    constexpr UINT kInitialWidth  = 1280;
    constexpr UINT kInitialHeight = 720;   // 창 크기

    Window window;
    if (!window.Create(hInstance, L"SpriteBatchDemo", kInitialWidth, kInitialHeight))
        return -1;

    Renderer renderer;
    if (!renderer.Initialize(window.Handle(), window.Width(), window.Height()))
        return -1;

    // Window는 Renderer의 존재를 모른다. 무슨 일이 일어났는지만 알리고,
    // 그것으로 무엇을 할지는 여기서 연결한다.
    window.SetResizeCallback([&renderer](UINT width, UINT height) {
        renderer.Resize(width, height);
    });

    // Space 를 누르면 Naive <-> Batched 전환. 그 외 키는 무시한다.
    window.SetKeyDownCallback([&renderer](WPARAM key) {
        if (key == VK_SPACE)
            renderer.ToggleMode();
    });

    window.Show(nCmdShow);

    FrameTimer timer;

    // 게임 루프. 메시지를 모두 처리한 뒤 한 프레임을 그린다.
    while (window.PumpMessages())
    {
        timer.Tick();
        renderer.Render();

        // 1초마다 평균값을 모아 제목에 띄운다. 여기가 곧 계측 결과 화면이다.
        double fps = 0.0, ms = 0.0;
        if (timer.TryGetStats(fps, ms))
        {
            const wchar_t* mode = (renderer.Mode() == RenderMode::Batched) ? L"Batched" : L"Naive";
            wchar_t title[256]{};
            swprintf_s(title,
                       L"SpriteBatchDemo  |  %s  |  스프라이트 %u개  |  드로우콜 %u  |  CPU제출 %.3f ms  |  프레임 %.3f ms  |  %.0f FPS   [Space] 전환  [Esc] 종료",
                       mode, renderer.SpriteCount(), renderer.DrawCallCount(), renderer.SubmitMs(), ms, fps);
            SetWindowTextW(window.Handle(), title);
        }
    }

    return 0;
}
