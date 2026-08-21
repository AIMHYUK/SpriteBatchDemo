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
    float u, v;          // 아틀라스 UV (0~1). 어느 도형을 뽑을지 정한다
    float r, g, b, a;    // 색. 각 0 ~ 1. 아틀라스 샘플에 곱해 색을 입힌다
};

// 스프라이트 하나의 '상태'. 이제 위치가 매 프레임 바뀌므로(움직임) 정점에 굽는 대신
// 이 상태를 CPU에 들고 있다가, 매 프레임 현재 위치로 정점을 다시 만든다.
// 이것이 정적 배칭(한 번 굽고 끝)과 동적 배칭(매 프레임 다시 올림)의 갈림길이다.
struct SpriteState
{
    float x, y;              // 현재 위치(좌상단, 픽셀)
    float vx, vy;            // 속도(픽셀/초)
    float w, h;              // 크기
    float u0, v0, u1, v1;    // 아틀라스 UV 사각형(어느 도형인지)
    float r, g, b;           // 색
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

// 스프라이트 하나당 하나(per-instance)로 올리는 데이터. Batched의 정점(N*4)과 달리
// 인스턴스는 N개뿐이라 업로드 대역폭이 약 1/3로 준다. shaders/SpriteInstanced.vs.hlsl과 짝.
struct InstanceData
{
    float x, y;              // 좌상단 픽셀 위치
    float w, h;              // 크기
    float u0, v0, u1, v1;    // 아틀라스 UV 사각형
    float r, g, b;           // 색
};

// 그리는 방식. 이것만 바꿔가며 성능을 비교하는 것이 이 프로젝트의 전부다.
enum class RenderMode
{
    // 스프라이트마다 정점을 올리고 DrawIndexed. 드로우 콜 = 스프라이트 수.
    Naive,
    // 모든 정점을 한 버퍼에 모아 한 번 올리고 DrawIndexed 한 번. 드로우 콜 = 1.
    Batched,
    // 단위 사각형 1개 + per-instance 데이터로 DrawIndexedInstanced 한 번. 업로드가 가장 적다.
    Instanced,
};

// D3D11 디바이스·스왑체인·백버퍼 뷰를 소유하고, 스프라이트 무리를 한 프레임 그린다.
class Renderer
{
public:
    bool Initialize(HWND hwnd, UINT width, UINT height);

    // 창 크기가 바뀌었을 때 스왑체인 버퍼를 다시 잡는다.
    void Resize(UINT width, UINT height);

    // 스프라이트를 dt(초)만큼 움직이고, 현재 위치로 정점을 다시 만든다. 매 프레임 Render 전에 부른다.
    void Update(float dt);

    void Render();

    // Naive → Batched → Instanced 순환. main이 Space 키에 연결한다.
    void ToggleMode()
    {
        m_mode = (m_mode == RenderMode::Naive)   ? RenderMode::Batched
               : (m_mode == RenderMode::Batched) ? RenderMode::Instanced
                                                 : RenderMode::Naive;
    }

    // 움직임 일시정지(숫자를 읽거나 스크린샷 찍을 때). main이 P 키에 연결한다.
    void TogglePause() { m_paused = !m_paused; }
    bool IsPaused() const { return m_paused; }

    // 컬링 on/off. main이 C 키에 연결한다. 카메라는 Update가 WASD로 직접 읽는다.
    void ToggleCull() { m_cull = !m_cull; }
    bool IsCulling()    const { return m_cull; }
    UINT VisibleCount() const { return m_visibleCount; }   // 이번 프레임 실제로 그린(화면에 걸친) 수

    RenderMode Mode()          const { return m_mode; }
    UINT       DrawCallCount() const { return m_drawCalls; }     // 직전 프레임에 부른 드로우 콜 수
    UINT       SpriteCount()   const { return m_spriteCount; }
    // 직전 프레임에 드로우를 "제출"하는 데 든 CPU 시간(ms). 배칭이 진짜로 이기는 지표.
    // 총 프레임 시간은 GPU 바닥에 묶이지만, 이 값은 렌더 스레드 CPU 부하를 그대로 드러낸다.
    double     SubmitMs()      const { return m_lastSubmitMs; }

    // 제출 시간 세부(Batched/Instanced의 단일 업로드 한정). 정지 vs 이동 차이의 출처를 가린다.
    double     MapMs()         const { return m_mapMs; }    // Map() 호출 시간 (드라이버/rename/GPU대기)
    double     CopyMs()        const { return m_copyMs; }   // memcpy 시간 (CPU/캐시/클럭)

private:
    bool CreateDeviceAndSwapChain(HWND hwnd, UINT width, UINT height);
    bool CreateBackBufferView();
    void ReleaseBackBufferView();

    // 셰이더 -> 입력 레이아웃 -> 정점/인덱스 버퍼 -> 상수 버퍼 -> 래스터라이저 상태.
    bool CreateSpriteResources();

    // 텍스처 아틀라스(코드로 생성) + SRV + 샘플러 + 알파 블렌드 상태를 만든다.
    // 여러 도형을 한 장에 담아 두면 텍스처 바인딩을 스프라이트마다 바꿀 필요가 없다.
    bool CreateAtlasResources();

    // 스프라이트 N개의 초기 상태(위치·속도·크기·색·UV)를 만들고 버퍼를 준비한다.
    // 위치·속도는 고정 시드 난수라 실행할 때마다 같은 장면이 나온다(비교 재현성).
    bool BuildSprites();

    // m_sprites의 현재 상태로 m_cpuVertices(정점 N*4개)를 다시 채운다.
    void RebuildVertices();

    // m_sprites의 현재 상태로 m_instances(인스턴스 N개)를 다시 채운다. Instanced 모드용.
    void RebuildInstances();

    // 인스턴싱 리소스(단위 사각형 VB, 인스턴스 버퍼, 전용 셰이더·입력 레이아웃)를 만든다.
    bool CreateInstanceResources();

    ComPtr<ID3D11Device>           m_device;
    ComPtr<ID3D11DeviceContext>    m_context;
    ComPtr<IDXGISwapChain1>        m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_backBufferView;

    // ── 스프라이트용 ──
    ComPtr<ID3D11VertexShader>    m_vertexShader;
    ComPtr<ID3D11PixelShader>     m_pixelShader;
    ComPtr<ID3D11InputLayout>     m_inputLayout;     // 정점 버퍼의 바이트를 해석하는 규칙
    ComPtr<ID3D11Buffer>          m_vertexBuffer;    // [Batched] 정점 N*4개. 움직이므로 DYNAMIC(매 프레임 Map)
    ComPtr<ID3D11Buffer>          m_indexBuffer;     // [Batched] N*6개 인덱스 (안 바뀌므로 IMMUTABLE)
    ComPtr<ID3D11Buffer>          m_constantBuffer;  // 화면 크기

    // [Naive] 스프라이트 하나씩 GPU에 올려 그리는 경로.
    // 매 스프라이트마다 이 작은 동적 버퍼에 정점 4개를 Map으로 올리고 그린다.
    // 바로 이 "per-object 업로드 + 드로우"가 배칭이 없애는 비용이다.
    ComPtr<ID3D11Buffer>          m_naiveVertexBuffer;  // DYNAMIC, 정점 4개
    ComPtr<ID3D11Buffer>          m_naiveIndexBuffer;   // 로컬 인덱스 6개 {0,1,2,0,2,3}
    std::vector<SpriteState>      m_sprites;            // 스프라이트 상태(월드 좌표). 매 프레임 갱신
    std::vector<Vertex>           m_cpuVertices;        // 화면에 걸친 정점만 앞에서부터 채움 (Naive/Batched가 업로드)
    bool                          m_paused = false;     // 움직임 일시정지

    // ── 카메라 + 컬링 ──
    // 스프라이트는 화면보다 큰 월드를 돌아다니고, 카메라(좌상단 오프셋)가 그 일부를 본다.
    // 컬링 ON이면 화면 밖 스프라이트를 CPU에서 걸러 그리기 목록에 안 넣는다.
    float m_camX = 0.0f, m_camY = 0.0f;     // 카메라 좌상단 (월드 좌표)
    float m_worldW = 0.0f, m_worldH = 0.0f; // 월드 크기 (화면보다 큼)
    bool  m_cull = false;                   // 컬링 on/off (C 키)
    UINT  m_visibleCount = 0;               // 이번 프레임 실제로 그리는 스프라이트 수

    // [Instanced] 단위 사각형 1개 + per-instance 데이터로 N개를 한 번에 그린다.
    ComPtr<ID3D11VertexShader>    m_instanceVS;         // 전용 정점 셰이더
    ComPtr<ID3D11InputLayout>     m_instanceLayout;     // 슬롯0=단위사각형, 슬롯1=per-instance
    ComPtr<ID3D11Buffer>          m_baseQuadVB;         // 단위 사각형 4정점(코너 0~1), 모든 인스턴스 공유
    ComPtr<ID3D11Buffer>          m_instanceBuffer;     // DYNAMIC, 인스턴스 N개
    std::vector<InstanceData>     m_instances;          // 현재 상태로 만든 인스턴스 N개
    ComPtr<ID3D11RasterizerState> m_rasterizerState; // 컬링 규칙

    // ── 텍스처 아틀라스 ──
    ComPtr<ID3D11ShaderResourceView> m_atlasSRV;      // 픽셀 셰이더가 샘플할 아틀라스
    ComPtr<ID3D11SamplerState>       m_sampler;       // UV -> 색을 어떻게 읽을지(선형/클램프)
    ComPtr<ID3D11BlendState>         m_blendState;    // 알파 블렌딩(도형 가장자리를 부드럽게)

    // 플립 모델 스왑체인은 창모드에서 DWM이 주사율(예: 60Hz)로 프레임을 묶는다.
    // Present(0,0)만으로는 안 풀리고, tearing(테어링)을 허용해야 진짜 상한이 사라진다.
    // GPU/드라이버가 지원할 때만 켠다.
    bool m_allowTearing = false;

    RenderMode m_mode        = RenderMode::Naive;   // 처음엔 느린 쪽으로 켠다(비교 대비를 위해)
    UINT       m_spriteCount = 0;
    UINT       m_indexCount  = 0;   // = m_spriteCount * 6
    UINT       m_drawCalls   = 0;

    LARGE_INTEGER m_qpcFreq{};        // 성능 카운터 주파수 (CPU 제출 시간 측정용, 한 번만 조회)
    double        m_lastSubmitMs = 0.0;
    double        m_mapMs        = 0.0;   // 직전 Map() 호출 시간
    double        m_copyMs       = 0.0;   // 직전 memcpy 시간

    UINT m_width  = 0;
    UINT m_height = 0;
};
