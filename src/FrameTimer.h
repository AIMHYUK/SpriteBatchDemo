#pragma once

#include "Common.h"

// 프레임 시간을 재는 최소한의 타이머.
//
// 이 프로젝트의 목적이 "드로우 콜 방식에 따라 성능이 어떻게 달라지는가를 실측하는 것"이므로
// 계측은 처음부터 코드 안에 둔다. 창 제목에 평균 FPS와 평균 프레임 시간(ms)을 함께 띄운다.
//
// 왜 ms까지 보나: FPS는 큰 수라 차이가 눈에 잘 안 들어온다(2000 -> 1800).
// 프레임 시간(ms)은 작은 수라 배칭 전후 차이가 선명하게 보인다(0.5ms -> 0.05ms).
// 게다가 프레임 시간은 더하고 나눌 수 있어(선형) 성능을 논할 때 FPS보다 정직하다.
class FrameTimer
{
public:
    FrameTimer()
    {
        // QueryPerformanceFrequency가 돌려주는 값은 부팅 후 바뀌지 않는다.
        // 그래서 매번 부르지 않고 한 번만 받아 둔다.
        QueryPerformanceFrequency(&m_frequency);
        QueryPerformanceCounter(&m_lastTick);
        m_windowStartTick = m_lastTick;
    }

    // 매 프레임 한 번 부른다. 직전 프레임에 걸린 시간(초)을 돌려준다.
    double Tick()
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        const double delta = static_cast<double>(now.QuadPart - m_lastTick.QuadPart)
                           / static_cast<double>(m_frequency.QuadPart);
        m_lastTick = now;

        ++m_framesInWindow;
        return delta;
    }

    // 마지막 집계 이후 1초가 지났으면 평균 FPS와 평균 프레임 시간(ms)을 담고 true를 돌려준다.
    //
    // 매 프레임의 순간값을 쓰지 않는 이유: 순간값은 심하게 튀어서 눈으로 비교할 수 없다.
    // 일정 구간의 평균이라야 "이 방식이 저 방식보다 빠르다"를 말할 수 있다.
    bool TryGetStats(double& outFps, double& outMsPerFrame)
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        const double elapsed = static_cast<double>(now.QuadPart - m_windowStartTick.QuadPart)
                             / static_cast<double>(m_frequency.QuadPart);

        if (elapsed < 1.0)
            return false;

        outFps        = m_framesInWindow / elapsed;
        outMsPerFrame = (elapsed * 1000.0) / m_framesInWindow;

        m_framesInWindow  = 0;
        m_windowStartTick = now;
        return true;
    }

private:
    LARGE_INTEGER m_frequency{};
    LARGE_INTEGER m_lastTick{};
    LARGE_INTEGER m_windowStartTick{};
    int           m_framesInWindow = 0;
};
