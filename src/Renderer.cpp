#include "Renderer.h"

#include <cstdint>
#include <cmath>
#include <vector>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>   // IDXGIFactory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING

namespace
{
    // 백버퍼 포맷. 플립 모델 스왑체인이 허용하는 포맷 중 하나다.
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    // 화면을 지울 색. 아무것도 안 그려도 이 색이 보이면 파이프라인이 살아 있다는 뜻이다.
    constexpr float kClearColor[4] = { 0.08f, 0.08f, 0.10f, 1.0f };

    // 그릴 스프라이트 수. Naive 모드에서 이 수만큼 "정점 업로드 + 드로우"를 반복한다.
    // 그 per-object 비용이 배칭과의 격차를 만든다.
    // (측정 실험: 2000 / 5000 / 10000 / 20000 / 40000 으로 바꿔가며 갈라지는 지점을 본다.
    //  너무 크게 잡으면 Naive가 수십 ms로 느려져 데모가 버벅인다. 2만 정도가 균형점)
    constexpr UINT kSpriteCount = 20000;

    // 스프라이트를 작게 둔다(겹침=오버드로우를 줄이려고).
    // 크게 그리면 픽셀 셰이더가 병목이 되어 우리가 재려는 per-object 비용이 묻힌다.
    constexpr float kSpriteMin = 6.0f;
    constexpr float kSpriteMax = 16.0f;

    // 실행할 때마다 같은 장면을 만들기 위한 고정 시드 난수(xorshift32).
    // std::rand는 구현마다 결과가 다르고 시드도 전역이라, 재현 가능한 비교를 위해 직접 둔다.
    struct Rng
    {
        uint32_t s;
        explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
        uint32_t Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
        float Unit() { return (Next() >> 8) * (1.0f / 16777216.0f); }     // [0, 1)
        float Range(float lo, float hi) { return lo + Unit() * (hi - lo); }
        uint32_t Below(uint32_t n) { return Next() % n; }                 // [0, n)
    };

    // ── 텍스처 아틀라스 설정 ──
    // 한 장(256x256)에 도형 4종을 2x2로 담는다. 스프라이트는 이 중 하나를 UV로 뽑아 쓴다.
    // 여러 종류를 한 장에 모아두면 텍스처를 스프라이트마다 바꿀 필요가 없어, 배칭 한 번에 다 그린다.
    constexpr UINT kAtlasSize      = 256;
    constexpr UINT kAtlasCols      = 2;
    constexpr UINT kAtlasRows      = 2;
    constexpr UINT kAtlasTileCount = kAtlasCols * kAtlasRows;

    // 0 이하면 0, 1 이상이면 1, 사이는 직선. 가장자리를 부드럽게 하는 데 쓴다(간이 smoothstep).
    float LinStep(float edge0, float edge1, float x)
    {
        if (x <= edge0) return 0.0f;
        if (x >= edge1) return 1.0f;
        return (x - edge0) / (edge1 - edge0);
    }

    // 아틀라스를 코드로 그린다(외부 이미지·라이브러리 없음).
    // 흰색(RGB=255)에 알파로 도형 모양을 새긴다. 정점 색을 곱하면 그 도형이 그 색으로 물든다.
    // 반환은 R8G8B8A8_UNORM 픽셀(uint32, 메모리순 R,G,B,A -> 리틀엔디언 0xAABBGGRR).
    std::vector<uint32_t> GenerateAtlasPixels()
    {
        std::vector<uint32_t> pixels(kAtlasSize * kAtlasSize, 0u);
        const UINT tile = kAtlasSize / kAtlasCols;   // 128

        for (UINT ty = 0; ty < kAtlasRows; ++ty)
        for (UINT tx = 0; tx < kAtlasCols; ++tx)
        {
            const UINT shape = ty * kAtlasCols + tx;   // 0~3
            const UINT ox = tx * tile;
            const UINT oy = ty * tile;

            for (UINT py = 0; py < tile; ++py)
            for (UINT px = 0; px < tile; ++px)
            {
                // 타일 중심 기준 정규화 좌표 (-1 ~ 1)
                const float nx = (px + 0.5f) / (tile * 0.5f) - 1.0f;
                const float ny = (py + 0.5f) / (tile * 0.5f) - 1.0f;
                const float r  = std::sqrt(nx * nx + ny * ny);

                float alpha = 0.0f;
                switch (shape)
                {
                case 0:  // 채운 원
                    alpha = 1.0f - LinStep(0.78f, 0.95f, r);
                    break;
                case 1:  // 링(속 빈 원)
                    alpha = LinStep(0.45f, 0.60f, r) * (1.0f - LinStep(0.80f, 0.95f, r));
                    break;
                case 2:  // 다이아몬드
                {
                    const float d = std::fabs(nx) + std::fabs(ny);
                    alpha = 1.0f - LinStep(0.82f, 0.98f, d);
                    break;
                }
                default: // + 모양(네잎 별)
                {
                    const float barH = 1.0f - LinStep(0.20f, 0.30f, std::fabs(ny));
                    const float barV = 1.0f - LinStep(0.20f, 0.30f, std::fabs(nx));
                    const float cross = (barH > barV) ? barH : barV;
                    alpha = cross * (1.0f - LinStep(0.90f, 1.00f, r));  // 끝을 둥글게
                    break;
                }
                }

                if (alpha < 0.0f) alpha = 0.0f;
                if (alpha > 1.0f) alpha = 1.0f;
                const uint32_t a = static_cast<uint32_t>(alpha * 255.0f + 0.5f);

                pixels[(oy + py) * kAtlasSize + (ox + px)] = (a << 24) | 0x00FFFFFFu;
            }
        }
        return pixels;
    }

    // HLSL 파일 하나를 컴파일해서 결과 바이트코드를 blob에 담아 준다.
    //
    // 셰이더는 C++처럼 미리 exe에 박히는 게 아니라, 텍스트 상태로 두었다가
    // 실행할 때 GPU가 알아듣는 바이트코드로 번역한다. 그 번역기가 D3DCompileFromFile이다.
    bool CompileShaderFromFile(const wchar_t* fileName,
                               const char* entryPoint,
                               const char* target,        // "vs_5_0" / "ps_5_0"
                               ComPtr<ID3DBlob>& outBlob)
    {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        // 디버그 빌드에서는 최적화를 끄고 디버그 정보를 넣는다.
        // 그래야 그래픽 디버거에서 셰이더를 한 줄씩 따라갈 수 있다.
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        const std::wstring fullPath = ResolvePathFromExe(fileName);

        ComPtr<ID3DBlob> errorBlob;
        const HRESULT hr = D3DCompileFromFile(
            fullPath.c_str(),
            nullptr,                            // 매크로 정의 없음
            D3D_COMPILE_STANDARD_FILE_INCLUDE,  // #include를 파일 기준으로 처리
            entryPoint,
            target,
            flags, 0,
            outBlob.GetAddressOf(),
            errorBlob.GetAddressOf());

        if (FAILED(hr))
        {
            // 셰이더 오류의 진짜 정보("몇 번째 줄에서 무슨 문법 오류")는 errorBlob 안에 있다.
            if (errorBlob)
            {
                const char* text = static_cast<const char*>(errorBlob->GetBufferPointer());
                MessageBoxA(nullptr, text, "셰이더 컴파일 오류", MB_OK | MB_ICONERROR);
            }
            else
            {
                // errorBlob이 없으면 대개 파일을 못 찾은 것이다.
                std::wstring message = L"셰이더 파일을 열 수 없습니다.\n\n";
                message += fullPath;
                MessageBoxW(nullptr, message.c_str(), L"SpriteBatchDemo", MB_OK | MB_ICONERROR);
            }
            return false;
        }

        return true;
    }
}

bool Renderer::Initialize(HWND hwnd, UINT width, UINT height)
{
    m_width  = width;
    m_height = height;

    // CPU 제출 시간 측정용 주파수는 부팅 후 안 바뀌므로 한 번만 받아 둔다.
    QueryPerformanceFrequency(&m_qpcFreq);

    if (!CreateDeviceAndSwapChain(hwnd, width, height))
        return false;

    if (!CreateBackBufferView())
        return false;

    return CreateSpriteResources();
}

bool Renderer::CreateSpriteResources()
{
    // ── 1. 셰이더 소스를 GPU 바이트코드로 컴파일한다 ──
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;

    if (!CompileShaderFromFile(L"shaders\\Sprite.vs.hlsl", "main", "vs_5_0", vsBlob))
        return false;
    if (!CompileShaderFromFile(L"shaders\\Sprite.ps.hlsl", "main", "ps_5_0", psBlob))
        return false;

    // ── 2. 바이트코드로 실제 셰이더 객체를 만든다 ──
    if (!HR_CHECK(m_device->CreateVertexShader(
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
            m_vertexShader.GetAddressOf()),
            L"CreateVertexShader"))
        return false;

    if (!HR_CHECK(m_device->CreatePixelShader(
            psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
            m_pixelShader.GetAddressOf()),
            L"CreatePixelShader"))
        return false;

    // ── 3. 입력 레이아웃 ──
    //
    // 정점 버퍼는 GPU에겐 그냥 바이트 뭉치다. 어디가 위치고 어디가 색인지 알려주는 규칙이다.
    //
    //   바이트  0   4      8   12     16  20  24  28
    //          [x] [y]    [u] [v]    [r] [g] [b] [a]
    //          └POSITION┘ └TEXCOORD┘ └───── COLOR ─────┘
    //           (8바이트)  (8바이트)       (16바이트)
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    // 만들 때 정점 셰이더 바이트코드를 같이 넘겨, 레이아웃이 셰이더 입력과 맞는지 검증받는다.
    if (!HR_CHECK(m_device->CreateInputLayout(
            layout, _countof(layout),
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
            m_inputLayout.GetAddressOf()),
            L"CreateInputLayout"))
        return false;

    // ── 4. 화면 크기용 상수 버퍼 (매 프레임 갱신하므로 DYNAMIC) ──
    D3D11_BUFFER_DESC constantDesc{};
    constantDesc.ByteWidth      = sizeof(FrameConstants);
    constantDesc.Usage          = D3D11_USAGE_DYNAMIC;
    constantDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (!HR_CHECK(m_device->CreateBuffer(&constantDesc, nullptr, m_constantBuffer.GetAddressOf()),
                  L"CreateBuffer(ConstantBuffer)"))
        return false;

    // ── 5. 래스터라이저 상태 (시계 방향을 앞면으로, 뒷면 컬링) ──
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode              = D3D11_FILL_SOLID;
    rd.CullMode              = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = FALSE;

    if (!HR_CHECK(m_device->CreateRasterizerState(&rd, m_rasterizerState.GetAddressOf()),
                  L"CreateRasterizerState"))
        return false;

    // ── 6. 텍스처 아틀라스 + 샘플러 + 블렌드 상태 ──
    if (!CreateAtlasResources())
        return false;

    // ── 7. 스프라이트 무리를 만들어 정점/인덱스 버퍼로 올린다 ──
    if (!BuildSprites())
        return false;

    // ── 8. 인스턴싱 리소스 (BuildSprites가 채운 m_instances 크기를 사용) ──
    return CreateInstanceResources();
}

bool Renderer::BuildSprites()
{
    m_spriteCount = kSpriteCount;
    m_indexCount  = kSpriteCount * 6;   // 사각형 하나 = 삼각형 2개 = 인덱스 6개

    std::vector<uint32_t> indices;
    indices.reserve(m_indexCount);

    m_sprites.clear();
    m_sprites.reserve(kSpriteCount);
    m_cpuVertices.assign(kSpriteCount * 4, Vertex{});     // Naive/Batched용 정점 N*4
    m_instances.assign(kSpriteCount, InstanceData{});     // Instanced용 인스턴스 N개

    Rng rng(12345u);   // 고정 시드 -> 매 실행 같은 장면

    for (UINT i = 0; i < kSpriteCount; ++i)
    {
        SpriteState s{};
        s.w = rng.Range(kSpriteMin, kSpriteMax);
        s.h = rng.Range(kSpriteMin, kSpriteMax);
        // 스프라이트가 화면 안에 들어오도록 위치 범위를 크기만큼 줄인다.
        s.x = rng.Range(0.0f, static_cast<float>(m_width)  - s.w);
        s.y = rng.Range(0.0f, static_cast<float>(m_height) - s.h);

        // 속도: 각 축 50~180 px/초, 방향은 랜덤. 화면 경계에서 튕긴다.
        s.vx = (rng.Unit() < 0.5f ? -1.0f : 1.0f) * rng.Range(50.0f, 180.0f);
        s.vy = (rng.Unit() < 0.5f ? -1.0f : 1.0f) * rng.Range(50.0f, 180.0f);

        // 스프라이트 하나에 색 하나. 눈으로 개수가 느껴지도록 알록달록하게.
        s.r = rng.Range(0.2f, 1.0f);
        s.g = rng.Range(0.2f, 1.0f);
        s.b = rng.Range(0.2f, 1.0f);

        // 아틀라스 4칸 중 하나를 골라 그 칸의 UV 범위를 구한다.
        // 절반 텍셀만큼 안으로 밀어(inset) 옆 칸 색이 새어드는 것을 막는다.
        const uint32_t tileIdx = rng.Below(kAtlasTileCount);
        const uint32_t col = tileIdx % kAtlasCols;
        const uint32_t row = tileIdx / kAtlasCols;
        const float inset = 0.5f / kAtlasSize;
        s.u0 = col / static_cast<float>(kAtlasCols) + inset;
        s.v0 = row / static_cast<float>(kAtlasRows) + inset;
        s.u1 = (col + 1) / static_cast<float>(kAtlasCols) - inset;
        s.v1 = (row + 1) / static_cast<float>(kAtlasRows) - inset;

        m_sprites.push_back(s);

        // 인덱스는 위치와 무관하게 고정이라 여기서 한 번만 만든다.
        const uint32_t base = i * 4;
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }

    // 초기 위치로 정점·인스턴스를 한 번 채운다(이후 매 프레임 Update가 활성 모드 것을 갱신).
    RebuildVertices();
    RebuildInstances();

    // 스프라이트가 매 프레임 움직이므로 정점 버퍼는 DYNAMIC. 매 프레임 Map으로 현재 정점을 올린다.
    // (위치가 고정이던 이전 단계에선 IMMUTABLE로 한 번만 구웠다. 이게 정적 vs 동적 배칭의 차이다.)
    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth      = static_cast<UINT>(m_cpuVertices.size() * sizeof(Vertex));
    vbDesc.Usage          = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_SUBRESOURCE_DATA vbData{};
    vbData.pSysMem = m_cpuVertices.data();

    if (!HR_CHECK(m_device->CreateBuffer(&vbDesc, &vbData, m_vertexBuffer.GetAddressOf()),
                  L"CreateBuffer(VertexBuffer)"))
        return false;

    D3D11_BUFFER_DESC ibDesc{};
    ibDesc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    ibDesc.Usage     = D3D11_USAGE_IMMUTABLE;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibData{};
    ibData.pSysMem = indices.data();

    if (!HR_CHECK(m_device->CreateBuffer(&ibDesc, &ibData, m_indexBuffer.GetAddressOf()),
                  L"CreateBuffer(IndexBuffer)"))
        return false;

    // ── Naive 경로용 작은 동적 버퍼 ──
    // (m_cpuVertices는 위에서 RebuildVertices가 채웠다. Naive는 여기서 스프라이트마다 4개씩 복사해 쓴다.)
    // 스프라이트 하나(정점 4개)를 매번 새로 올릴 작은 동적 버퍼. 초기 데이터는 없다.
    D3D11_BUFFER_DESC nvbDesc{};
    nvbDesc.ByteWidth      = static_cast<UINT>(4 * sizeof(Vertex));
    nvbDesc.Usage          = D3D11_USAGE_DYNAMIC;
    nvbDesc.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    nvbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (!HR_CHECK(m_device->CreateBuffer(&nvbDesc, nullptr, m_naiveVertexBuffer.GetAddressOf()),
                  L"CreateBuffer(NaiveVertexBuffer)"))
        return false;

    // 정점 4개짜리 사각형의 로컬 인덱스. Batched의 큰 인덱스 버퍼와 달리 오프셋이 0부터다.
    const uint32_t localIndices[] = { 0, 1, 2, 0, 2, 3 };

    D3D11_BUFFER_DESC nibDesc{};
    nibDesc.ByteWidth = sizeof(localIndices);
    nibDesc.Usage     = D3D11_USAGE_IMMUTABLE;
    nibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA nibData{};
    nibData.pSysMem = localIndices;

    if (!HR_CHECK(m_device->CreateBuffer(&nibDesc, &nibData, m_naiveIndexBuffer.GetAddressOf()),
                  L"CreateBuffer(NaiveIndexBuffer)"))
        return false;

    return true;
}

void Renderer::RebuildVertices()
{
    // 스프라이트의 현재 상태로 정점 N*4개를 다시 만든다. 두 모드 모두 이 결과를 GPU에 올린다.
    // 시계 방향(0→1→2→3), UV는 코너에 맞춘다(좌상 u0v0, 우상 u1v0, 우하 u1v1, 좌하 u0v1).
    for (UINT i = 0; i < m_spriteCount; ++i)
    {
        const SpriteState& s = m_sprites[i];
        const uint32_t base = i * 4;
        m_cpuVertices[base + 0] = { s.x,       s.y,       s.u0, s.v0, s.r, s.g, s.b, 1.0f };
        m_cpuVertices[base + 1] = { s.x + s.w, s.y,       s.u1, s.v0, s.r, s.g, s.b, 1.0f };
        m_cpuVertices[base + 2] = { s.x + s.w, s.y + s.h, s.u1, s.v1, s.r, s.g, s.b, 1.0f };
        m_cpuVertices[base + 3] = { s.x,       s.y + s.h, s.u0, s.v1, s.r, s.g, s.b, 1.0f };
    }
}

void Renderer::RebuildInstances()
{
    // 스프라이트의 현재 상태를 인스턴스 하나로 그대로 옮긴다(정점 4개가 아니라 1개).
    for (UINT i = 0; i < m_spriteCount; ++i)
    {
        const SpriteState& s = m_sprites[i];
        m_instances[i] = { s.x, s.y, s.w, s.h, s.u0, s.v0, s.u1, s.v1, s.r, s.g, s.b };
    }
}

void Renderer::Update(float dt)
{
    if (m_paused)
        return;

    // 큰 dt(첫 프레임·정지 해제 직후)에 스프라이트가 화면 밖으로 튀는 것을 막는다.
    if (dt > 0.05f) dt = 0.05f;

    const float maxX = static_cast<float>(m_width);
    const float maxY = static_cast<float>(m_height);

    // 물리 갱신: 이동 후 화면 경계에서 튕긴다(속도 반전 + 경계 안으로 되돌림).
    // 이 계산은 두 모드가 똑같이 필요한 공통 준비라, CPU 제출 시간 측정 바깥에 둔다.
    for (SpriteState& s : m_sprites)
    {
        s.x += s.vx * dt;
        s.y += s.vy * dt;

        if      (s.x < 0.0f)         { s.x = 0.0f;        s.vx = -s.vx; }
        else if (s.x > maxX - s.w)   { s.x = maxX - s.w;  s.vx = -s.vx; }
        if      (s.y < 0.0f)         { s.y = 0.0f;        s.vy = -s.vy; }
        else if (s.y > maxY - s.h)   { s.y = maxY - s.h;  s.vy = -s.vy; }
    }

    // 현재 모드가 쓰는 데이터만 다시 만든다(불필요한 준비를 줄인다).
    if (m_mode == RenderMode::Instanced)
        RebuildInstances();
    else
        RebuildVertices();
}

bool Renderer::CreateAtlasResources()
{
    // ── 1. 아틀라스 픽셀을 코드로 그려 IMMUTABLE 텍스처로 올린다 ──
    const std::vector<uint32_t> pixels = GenerateAtlasPixels();

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = kAtlasSize;
    td.Height           = kAtlasSize;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_IMMUTABLE;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA srd{};
    srd.pSysMem     = pixels.data();
    srd.SysMemPitch = kAtlasSize * sizeof(uint32_t);   // 한 줄(가로 한 줄)의 바이트 수

    ComPtr<ID3D11Texture2D> atlas;
    if (!HR_CHECK(m_device->CreateTexture2D(&td, &srd, atlas.GetAddressOf()),
                  L"CreateTexture2D(Atlas)"))
        return false;

    // 텍스처 자체가 아니라 "셰이더가 읽는 뷰(SRV)"를 파이프라인에 바인딩한다.
    if (!HR_CHECK(m_device->CreateShaderResourceView(atlas.Get(), nullptr, m_atlasSRV.GetAddressOf()),
                  L"CreateShaderResourceView(Atlas)"))
        return false;

    // ── 2. 샘플러: UV로 텍스처를 어떻게 읽을지. 선형 필터 + 가장자리 클램프 ──
    D3D11_SAMPLER_DESC sd{};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;

    if (!HR_CHECK(m_device->CreateSamplerState(&sd, m_sampler.GetAddressOf()),
                  L"CreateSamplerState"))
        return false;

    // ── 3. 알파 블렌드 상태: 도형 가장자리(알파<1)를 배경과 부드럽게 섞는다 ──
    //    최종색 = src.rgb * src.a + dst.rgb * (1 - src.a)
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (!HR_CHECK(m_device->CreateBlendState(&bd, m_blendState.GetAddressOf()),
                  L"CreateBlendState"))
        return false;

    return true;
}

bool Renderer::CreateInstanceResources()
{
    // ── 1. 전용 정점 셰이더 (픽셀 셰이더는 Sprite.ps.hlsl 공용) ──
    ComPtr<ID3DBlob> vsBlob;
    if (!CompileShaderFromFile(L"shaders\\SpriteInstanced.vs.hlsl", "main", "vs_5_0", vsBlob))
        return false;
    if (!HR_CHECK(m_device->CreateVertexShader(
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
            m_instanceVS.GetAddressOf()),
            L"CreateVertexShader(Instanced)"))
        return false;

    // ── 2. 입력 레이아웃: 슬롯0=단위 사각형(per-vertex), 슬롯1=인스턴스 데이터(per-instance) ──
    // 마지막 인자(InstanceDataStepRate)가 1이면 "인스턴스 하나당 한 번 전진"이다.
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "CORNER",     0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D11_INPUT_PER_VERTEX_DATA,   0 },
        { "INST_POS",   0, DXGI_FORMAT_R32G32_FLOAT,       1,  0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INST_SIZE",  0, DXGI_FORMAT_R32G32_FLOAT,       1,  8, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INST_UV",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INST_COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT,    1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };
    if (!HR_CHECK(m_device->CreateInputLayout(
            layout, _countof(layout),
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
            m_instanceLayout.GetAddressOf()),
            L"CreateInputLayout(Instanced)"))
        return false;

    // ── 3. 단위 사각형 4정점(코너 0~1). 모든 인스턴스가 공유한다. 인덱스는 naive의 {0,1,2,0,2,3} 재사용 ──
    const float quad[] = {
        0.0f, 0.0f,   // 0 좌상
        1.0f, 0.0f,   // 1 우상
        1.0f, 1.0f,   // 2 우하
        0.0f, 1.0f,   // 3 좌하
    };
    D3D11_BUFFER_DESC qd{};
    qd.ByteWidth = sizeof(quad);
    qd.Usage     = D3D11_USAGE_IMMUTABLE;
    qd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA qdata{};
    qdata.pSysMem = quad;
    if (!HR_CHECK(m_device->CreateBuffer(&qd, &qdata, m_baseQuadVB.GetAddressOf()),
                  L"CreateBuffer(BaseQuad)"))
        return false;

    // ── 4. 인스턴스 버퍼 (DYNAMIC, N개). 매 프레임 Map으로 현재 인스턴스를 올린다 ──
    D3D11_BUFFER_DESC ib{};
    ib.ByteWidth      = static_cast<UINT>(m_instances.size() * sizeof(InstanceData));
    ib.Usage          = D3D11_USAGE_DYNAMIC;
    ib.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    ib.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA idata{};
    idata.pSysMem = m_instances.data();
    if (!HR_CHECK(m_device->CreateBuffer(&ib, &idata, m_instanceBuffer.GetAddressOf()),
                  L"CreateBuffer(Instance)"))
        return false;

    return true;
}

bool Renderer::CreateDeviceAndSwapChain(HWND hwnd, UINT width, UINT height)
{
    UINT deviceFlags = 0;
#ifdef _DEBUG
    // 디버그 레이어를 켜면 잘못된 API 사용이 출력 창에 문장으로 찍힌다.
    deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL requestedLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL acquiredLevel{};

    // 플립 모델 스왑체인을 쓰려면 디바이스를 먼저 만들고 DXGI 팩토리로 스왑체인을 따로 만든다.
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags,
        requestedLevels, _countof(requestedLevels), D3D11_SDK_VERSION,
        m_device.GetAddressOf(), &acquiredLevel, m_context.GetAddressOf());

#ifdef _DEBUG
    if (FAILED(hr))
    {
        // 그래픽 도구가 없으면 디버그 레이어 요청이 실패한다. 그때는 레이어 없이 재시도.
        deviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags,
            requestedLevels, _countof(requestedLevels), D3D11_SDK_VERSION,
            m_device.GetAddressOf(), &acquiredLevel, m_context.GetAddressOf());
    }
#endif

    if (!HR_CHECK(hr, L"D3D11CreateDevice"))
        return false;

    // 스왑체인을 만들려면 이 디바이스를 만든 그 어댑터의 팩토리가 필요하다.
    // ID3D11Device -> IDXGIDevice -> IDXGIAdapter -> IDXGIFactory2 순으로 거슬러 올라간다.
    ComPtr<IDXGIDevice> dxgiDevice;
    if (!HR_CHECK(m_device.As(&dxgiDevice), L"ID3D11Device -> IDXGIDevice"))
        return false;

    ComPtr<IDXGIAdapter> adapter;
    if (!HR_CHECK(dxgiDevice->GetAdapter(adapter.GetAddressOf()), L"IDXGIDevice::GetAdapter"))
        return false;

    ComPtr<IDXGIFactory2> factory;
    if (!HR_CHECK(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())), L"IDXGIAdapter::GetParent"))
        return false;

    // 이 GPU/드라이버가 tearing(프레임 상한 해제)을 지원하는지 물어본다.
    // IDXGIFactory5부터 이 질의가 가능하다. 지원하면 스왑체인·Present에 플래그를 붙인다.
    // 이게 없으면 창모드 플립 모델은 60Hz 같은 주사율에 묶여 성능 차이가 안 보인다.
    if (ComPtr<IDXGIFactory5> factory5; SUCCEEDED(factory.As(&factory5)))
    {
        BOOL allowTearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
        {
            m_allowTearing = (allowTearing == TRUE);
        }
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width       = width;
    desc.Height      = height;
    desc.Format      = kBackBufferFormat;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;                                  // 플립 모델은 2개 이상
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count   = 1;                           // FLIP 모델은 MSAA 불가
    desc.SampleDesc.Quality = 0;
    desc.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;
    // tearing을 쓰려면 스왑체인을 만들 때부터 이 플래그가 있어야 한다.
    desc.Flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

    if (!HR_CHECK(factory->CreateSwapChainForHwnd(
            m_device.Get(), hwnd, &desc, nullptr, nullptr, m_swapChain.GetAddressOf()),
            L"IDXGIFactory2::CreateSwapChainForHwnd"))
        return false;

    // Alt+Enter로 DXGI가 멋대로 전체화면을 토글하지 못하게 막는다.
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    return true;
}

bool Renderer::CreateBackBufferView()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    if (!HR_CHECK(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())),
                  L"IDXGISwapChain::GetBuffer"))
        return false;

    if (!HR_CHECK(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, m_backBufferView.GetAddressOf()),
                  L"ID3D11Device::CreateRenderTargetView"))
        return false;

    // 뷰포트는 정규화 좌표를 픽셀로 옮기는 규칙이다. 창 크기가 바뀌면 같이 바뀐다.
    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width    = static_cast<float>(m_width);
    viewport.Height   = static_cast<float>(m_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &viewport);

    return true;
}

void Renderer::ReleaseBackBufferView()
{
    // ResizeBuffers는 백버퍼를 참조하는 것이 하나라도 남아 있으면 실패한다.
    m_backBufferView.Reset();
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_context->Flush();
}

void Renderer::Resize(UINT width, UINT height)
{
    if (!m_swapChain || (width == m_width && height == m_height))
        return;

    m_width  = width;
    m_height = height;

    ReleaseBackBufferView();

    // 리사이즈 후에도 tearing을 유지하려면 만들 때와 같은 플래그를 넘겨야 한다.
    const UINT swapFlags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    if (!HR_CHECK(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapFlags),
                  L"IDXGISwapChain::ResizeBuffers"))
        return;

    CreateBackBufferView();
    // 주의: 스프라이트 위치는 초기 창 크기 기준으로 구워졌다. 리사이즈해도 다시 굽지 않으므로
    // 화면을 키우면 오른쪽/아래에 여백이 생긴다. 데모 측정에는 영향 없어 그대로 둔다.
}

void Renderer::Render()
{
    if (!m_backBufferView)
        return;

    // FLIP 모델에서는 Present 뒤에 렌더 타깃 바인딩이 풀린다. 매 프레임 다시 바인딩한다.
    ID3D11RenderTargetView* views[] = { m_backBufferView.Get() };
    m_context->OMSetRenderTargets(1, views, nullptr);
    m_context->ClearRenderTargetView(m_backBufferView.Get(), kClearColor);

    // ── 파이프라인 공통 설정 ──
    // 입력 레이아웃·정점 셰이더·버퍼는 모드마다 다르므로 아래 각 분기에서 꽂는다.
    // (픽셀 셰이더·토폴로지·래스터·블렌드·아틀라스·상수버퍼는 세 모드 공통)
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->RSSetState(m_rasterizerState.Get());

    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);

    // 아틀라스 텍스처·샘플러를 픽셀 셰이더에 꽂는다(t0/s0).
    // 도형 4종이 한 장에 있으므로 스프라이트마다 텍스처를 바꿀 필요가 없다 -> 배칭 한 번에 다 그린다.
    m_context->PSSetShaderResources(0, 1, m_atlasSRV.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    // 알파 블렌딩 켜기(도형 가장자리를 배경과 부드럽게). 두 모드 공통.
    const float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);

    // 화면 크기를 상수 버퍼로. 정점 셰이더가 픽셀 -> NDC 변환에 쓴다.
    FrameConstants constants{};
    constants.screenWidth  = static_cast<float>(m_width);
    constants.screenHeight = static_cast<float>(m_height);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &constants, sizeof(constants));
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }
    m_context->VSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

    // ── 여기가 이 프로젝트의 핵심 ──
    //
    // 두 모드는 똑같은 스프라이트 무리를 똑같은 픽셀로 그린다. 다른 것은 그 방법이다.
    // 아래 드로우 "제출"에 든 CPU 시간을 잰다. 총 프레임 시간은 GPU 바닥에 묶이지만,
    // 이 CPU 제출 시간이야말로 배칭이 렌더 스레드에서 실제로 절약하는 양이다.
    LARGE_INTEGER submitStart{};
    QueryPerformanceCounter(&submitStart);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;

    if (m_mode == RenderMode::Naive)
    {
        // 스프라이트 하나씩: 정점 4개를 작은 동적 버퍼에 Map으로 올리고 -> 그린다. N번 반복.
        // 이게 실무의 순진한 경로다. Map(WRITE_DISCARD)마다 드라이버가 버퍼를 새로 잡고,
        // 드로우 콜마다 CPU->드라이버 왕복이 붙는다. 이 per-object 비용이 쌓여 느려진다.
        m_mapMs = 0.0; m_copyMs = 0.0;   // Naive는 작은 Map을 N번이라 단일 분리가 의미 없음
        m_context->IASetInputLayout(m_inputLayout.Get());
        m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        m_context->IASetVertexBuffers(0, 1, m_naiveVertexBuffer.GetAddressOf(), &stride, &offset);
        m_context->IASetIndexBuffer(m_naiveIndexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);

        for (UINT i = 0; i < m_spriteCount; ++i)
        {
            D3D11_MAPPED_SUBRESOURCE mappedVB{};
            if (SUCCEEDED(m_context->Map(m_naiveVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedVB)))
            {
                memcpy(mappedVB.pData, &m_cpuVertices[i * 4], 4 * sizeof(Vertex));
                m_context->Unmap(m_naiveVertexBuffer.Get(), 0);
            }
            m_context->DrawIndexed(6, 0, 0);
        }

        m_drawCalls = m_spriteCount;
    }
    else if (m_mode == RenderMode::Batched)
    {
        // 모든 스프라이트의 현재 정점을 '한 번의' Map으로 통째로 올리고 '한 번' 그린다.
        // Naive가 스프라이트마다 나눠 하던 업로드+드로우를, 각각 1회로 합친 것이 배칭이다.
        m_context->IASetInputLayout(m_inputLayout.Get());
        m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);

        // Map() 호출과 memcpy를 따로 잰다 — 정지 vs 이동 차이가 어디서 나는지 가리기 위해.
        // Map()이 늘면 드라이버/rename/GPU대기, memcpy가 늘면 CPU측(캐시·클럭).
        D3D11_MAPPED_SUBRESOURCE mappedVB{};
        LARGE_INTEGER t0{}, t1{}, t2{};
        QueryPerformanceCounter(&t0);
        const HRESULT hr = m_context->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedVB);
        QueryPerformanceCounter(&t1);
        if (SUCCEEDED(hr))
        {
            memcpy(mappedVB.pData, m_cpuVertices.data(), m_cpuVertices.size() * sizeof(Vertex));
            QueryPerformanceCounter(&t2);
            m_context->Unmap(m_vertexBuffer.Get(), 0);
        }
        else { t2 = t1; }
        m_mapMs  = double(t1.QuadPart - t0.QuadPart) * 1000.0 / m_qpcFreq.QuadPart;
        m_copyMs = double(t2.QuadPart - t1.QuadPart) * 1000.0 / m_qpcFreq.QuadPart;

        m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
        m_context->IASetIndexBuffer(m_indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);

        m_context->DrawIndexed(m_indexCount, 0, 0);

        m_drawCalls = 1;
    }
    else // Instanced
    {
        // 정점은 단위 사각형 4개로 고정. 스프라이트별 데이터(인스턴스 N개)만 올린다.
        // Batched가 올리던 정점 N*4개(위치·UV·색) 대신 인스턴스 N개라 업로드가 약 1/3.
        // 그리기는 DrawIndexedInstanced 한 번 — GPU가 단위 사각형을 N번 찍으며 각자 인스턴스를 읽는다.
        m_context->IASetInputLayout(m_instanceLayout.Get());
        m_context->VSSetShader(m_instanceVS.Get(), nullptr, 0);

        // Batched와 동일하게 Map()과 memcpy를 따로 잰다.
        D3D11_MAPPED_SUBRESOURCE mappedInst{};
        LARGE_INTEGER t0{}, t1{}, t2{};
        QueryPerformanceCounter(&t0);
        const HRESULT hr = m_context->Map(m_instanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedInst);
        QueryPerformanceCounter(&t1);
        if (SUCCEEDED(hr))
        {
            memcpy(mappedInst.pData, m_instances.data(), m_instances.size() * sizeof(InstanceData));
            QueryPerformanceCounter(&t2);
            m_context->Unmap(m_instanceBuffer.Get(), 0);
        }
        else { t2 = t1; }
        m_mapMs  = double(t1.QuadPart - t0.QuadPart) * 1000.0 / m_qpcFreq.QuadPart;
        m_copyMs = double(t2.QuadPart - t1.QuadPart) * 1000.0 / m_qpcFreq.QuadPart;

        // 슬롯 0 = 단위 사각형(공유), 슬롯 1 = 인스턴스 데이터
        ID3D11Buffer* vbs[2]     = { m_baseQuadVB.Get(), m_instanceBuffer.Get() };
        UINT          strides[2] = { sizeof(float) * 2, sizeof(InstanceData) };
        UINT          offsets[2] = { 0, 0 };
        m_context->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        m_context->IASetIndexBuffer(m_naiveIndexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);

        m_context->DrawIndexedInstanced(6, m_spriteCount, 0, 0, 0);

        m_drawCalls = 1;
    }

    LARGE_INTEGER submitEnd{};
    QueryPerformanceCounter(&submitEnd);
    m_lastSubmitMs = double(submitEnd.QuadPart - submitStart.QuadPart) * 1000.0
                   / double(m_qpcFreq.QuadPart);

    // 첫 인자 SyncInterval = 0 -> VSync OFF.
    // 하지만 창모드 플립 모델은 이것만으로 안 풀린다. DWM(윈도우 합성기)이 주사율(예: 60Hz)로
    // 다시 묶기 때문에, ALLOW_TEARING 플래그까지 줘야 진짜로 프레임 상한이 사라진다.
    // 이게 없으면 두 모드가 똑같이 16.6ms(=60FPS)로 보이고 드로우 콜 비용 차이가 통째로 묻힌다.
    const UINT presentFlags = m_allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    m_swapChain->Present(0, presentFlags);
}
