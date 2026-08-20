#pragma once

#include "Common.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <vector>

// 정점 하나가 담는 것. C++ 쪽 정의다.
//
// 이 구조체와 HLSL의 VSInput, 그리고 입력 레이아웃 셋이 서로 맞아야 한다.
// 하나라도 어긋나면 그림이 엉뚱한 데 뜨거나 아예 안 뜬다.
//
// 앞선 단일 스프라이트 예제와 달리 좌표를 "픽셀"로 직접 담는다. 스프라이트마다 위치가
// 다르므로 단위 사각형 + 상수 버퍼로는 한 번에 여러 개를 못 그린다. 대신 각 스프라이트의
// 네 꼭짓점을 픽셀 좌표로 미리 구워 정점 버퍼에 넣고, 정점 셰이더가 픽셀 -> NDC만 바꾼다.
struct Vertex
{
    float x, y;          // 화면 픽셀 좌표 (좌상단 원점, y 아래로 +)
    float r, g, b, a;    // 색. 각 0 ~ 1
};

// 매 프레임 정점 셰이더로 넘기는 값. shaders/Sprite.vs.hlsl의 cbuffer와 짝이다.
// 여기서는 화면 크기 하나만 있으면 픽셀 -> NDC 변환이 된다.
//
// 상수 버퍼 규칙: 전체 크기가 16의 배수여야 한다. float2(8바이트)만으로는 모자라
// padding으로 16을 채운다.
struct FrameConstants
{
    float screenWidth, screenHeight;   // 0 ~ 7   창 크기 (픽셀)
    float padding[2];                  // 8 ~ 15  16바이트 정렬용
};
static_assert(sizeof(FrameConstants) % 16 == 0, "상수 버퍼는 16바이트 배수여야 한다");

// 그리는 방식. 이 하나만 바꿔가며 성능을 비교하는 것이 이 프로젝트의 전부다.
enum class RenderMode
{
    // 스프라이트마다 DrawIndexed를 한 번씩 부른다. 드로우 콜 = 스프라이트 수.
    Naive,
    // 모든 스프라이트를 한 버퍼로 묶어 DrawIndexed를 딱 한 번 부른다. 드로우 콜 = 1.
    Batched,
};

// D3D11 디바이스·스왑체인·백버퍼 뷰를 소유하고, 스프라이트 무리를 한 프레임 그린다.
class Renderer
{
public:
    bool Initialize(HWND hwnd, UINT width, UINT height);

    // 창 크기가 바뀌었을 때 스왑체인 버퍼를 다시 잡는다.
    void Resize(UINT width, UINT height);

    void Render();

    // Naive <-> Batched 전환. main이 Space 키에 연결한다.
    void ToggleMode() { m_mode = (m_mode == RenderMode::Naive) ? RenderMode::Batched : RenderMode::Naive; }

    RenderMode Mode()          const { return m_mode; }
    UINT       DrawCallCount() const { return m_drawCalls; }   // 직전 프레임에 부른 드로우 콜 수
    UINT       SpriteCount()   const { return m_spriteCount; }

private:
    bool CreateDeviceAndSwapChain(HWND hwnd, UINT width, UINT height);
    bool CreateBackBufferView();
    void ReleaseBackBufferView();

    // 셰이더 -> 입력 레이아웃 -> 정점/인덱스 버퍼 -> 상수 버퍼 -> 래스터라이저 상태.
    bool CreateSpriteResources();

    // 스프라이트 N개의 꼭짓점·인덱스를 CPU에서 만들어 IMMUTABLE 버퍼로 올린다.
    // 위치는 고정 시드 난수라 실행할 때마다 같은 장면이 나온다(비교 재현성).
    bool BuildSprites();

    ComPtr<ID3D11Device>           m_device;
    ComPtr<ID3D11DeviceContext>    m_context;
    ComPtr<IDXGISwapChain1>        m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_backBufferView;

    // ── 스프라이트용 ──
    ComPtr<ID3D11VertexShader>    m_vertexShader;
    ComPtr<ID3D11PixelShader>     m_pixelShader;
    ComPtr<ID3D11InputLayout>     m_inputLayout;     // 정점 버퍼의 바이트를 해석하는 규칙
    ComPtr<ID3D11Buffer>          m_vertexBuffer;    // [Batched] 스프라이트 N개의 꼭짓점 (N*4개)
    ComPtr<ID3D11Buffer>          m_indexBuffer;     // [Batched] N*6개 인덱스
    ComPtr<ID3D11Buffer>          m_constantBuffer;  // 화면 크기

    // [Naive] 스프라이트 하나씩 GPU에 올려 그리는 경로.
    // 매 스프라이트마다 이 작은 동적 버퍼에 정점 4개를 Map으로 올리고 그린다.
    // 바로 이 "per-object 업로드 + 드로우"가 배칭이 없애는 비용이다.
    ComPtr<ID3D11Buffer>          m_naiveVertexBuffer;  // DYNAMIC, 정점 4개
    ComPtr<ID3D11Buffer>          m_naiveIndexBuffer;   // 로컬 인덱스 6개 {0,1,2,0,2,3}
    std::vector<Vertex>           m_cpuVertices;        // 구워둔 전체 정점 (Naive가 4개씩 복사)
    ComPtr<ID3D11RasterizerState> m_rasterizerState; // 컬링 규칙

    // 플립 모델 스왑체인은 창모드에서 DWM이 주사율(예: 60Hz)로 프레임을 묶는다.
    // Present(0,0)만으로는 안 풀리고, tearing(테어링)을 허용해야 진짜 상한이 사라진다.
    // GPU/드라이버가 지원할 때만 켠다.
    bool m_allowTearing = false;

    RenderMode m_mode        = RenderMode::Naive;   // 처음엔 느린 쪽으로 켠다(비교 대비를 위해)
    UINT       m_spriteCount = 0;
    UINT       m_indexCount  = 0;   // = m_spriteCount * 6
    UINT       m_drawCalls   = 0;

    UINT m_width  = 0;
    UINT m_height = 0;
};
