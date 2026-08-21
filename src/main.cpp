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

    // Space = 모드 순환, P = 일시정지, C = 컬링 on/off. (WASD 카메라는 Renderer가 직접 읽음)
    window.SetKeyDownCallback([&renderer](WPARAM key) {
        if (key == VK_SPACE)
            renderer.ToggleMode();
        else if (key == 'P')
            renderer.TogglePause();
        else if (key == 'C')
            renderer.ToggleCull();
    });

    window.Show(nCmdShow);

    FrameTimer timer;

    // 게임 루프. 메시지를 모두 처리한 뒤 한 프레임을 그린다.
    while (window.PumpMessages())
    {
        const float dt = static_cast<float>(timer.Tick());
        renderer.Update(dt);   // 스프라이트 이동 + 현재 정점 재생성 (측정 바깥의 공통 준비)
        renderer.Render();

        // 1초마다 평균값을 모아 제목에 띄운다. 여기가 곧 계측 결과 화면이다.
        double fps = 0.0, ms = 0.0;
        if (timer.TryGetStats(fps, ms))
        {
            const wchar_t* mode =
                (renderer.Mode() == RenderMode::Batched)   ? L"Batched"   :
                (renderer.Mode() == RenderMode::Instanced) ? L"Instanced" : L"Naive";
            const wchar_t* pause = renderer.IsPaused() ? L" [정지]" : L"";
            const wchar_t* cull  = renderer.IsCulling() ? L"컬링ON" : L"컬링OFF";
            wchar_t title[400]{};
            swprintf_s(title,
                       L"SpriteBatchDemo  |  %s%s  |  %s  |  그림 %u/%u  |  드로우콜 %u  |  CPU제출 %.3f ms (Map %.3f/copy %.3f)  |  프레임 %.3f ms  |  %.0f FPS   [Space]모드 [C]컬링 [P]정지 [WASD]카메라 [Esc]",
                       mode, pause, cull, renderer.VisibleCount(), renderer.SpriteCount(), renderer.DrawCallCount(), renderer.SubmitMs(), renderer.MapMs(), renderer.CopyMs(), ms, fps);
            SetWindowTextW(window.Handle(), title);
        }
    }

    return 0;
}
