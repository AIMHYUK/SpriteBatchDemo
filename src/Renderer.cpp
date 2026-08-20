#include "Renderer.h"

#include <cstdint>
#include <vector>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

namespace
{
    // 백버퍼 포맷. 플립 모델 스왑체인이 허용하는 포맷 중 하나다.
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    // 화면을 지울 색. 아무것도 안 그려도 이 색이 보이면 파이프라인이 살아 있다는 뜻이다.
    constexpr float kClearColor[4] = { 0.08f, 0.08f, 0.10f, 1.0f };

    // 그릴 스프라이트 수. Naive 모드에서 이 수가 곧 드로우 콜 수가 된다.
    // 값을 키우면 Naive가 급격히 느려지고 Batched는 거의 그대로다 -> 격차가 커진다.
    constexpr UINT kSpriteCount = 5000;

    // 스프라이트를 작게 둔다(겹침=오버드로우를 줄이려고).
    // 크게 그리면 픽셀 셰이더가 병목이 되어 "드로우 콜 비용"이 묻힌다.
    // 우리가 재려는 건 드로우 콜 개수의 영향이므로 픽셀 비용은 일부러 낮춘다.
    constexpr float kSpriteMin = 6.0f;
    constexpr float kSpriteMax = 18.0f;

    // 실행할 때마다 같은 장면을 만들기 위한 고정 시드 난수(xorshift32).
    // std::rand는 구현마다 결과가 다르고 시드도 전역이라, 재현 가능한 비교를 위해 직접 둔다.
    struct Rng
    {
        uint32_t s;
        explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
        uint32_t Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
        float Unit() { return (Next() >> 8) * (1.0f / 16777216.0f); }     // [0, 1)
        float Range(float lo, float hi) { return lo + Unit() * (hi - lo); }
    };

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
    //   바이트  0   4      8   12  16  20
    //          [x] [y]    [r] [g] [b] [a]
    //          └POSITION┘ └───── COLOR ─────┘
    //           (8바이트)        (16바이트)
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
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

    // ── 6. 스프라이트 무리를 만들어 정점/인덱스 버퍼로 올린다 ──
    return BuildSprites();
}

bool Renderer::BuildSprites()
{
    m_spriteCount = kSpriteCount;
    m_indexCount  = kSpriteCount * 6;   // 사각형 하나 = 삼각형 2개 = 인덱스 6개

    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(kSpriteCount * 4);
    indices.reserve(m_indexCount);

    Rng rng(12345u);   // 고정 시드 -> 매 실행 같은 장면

    for (UINT i = 0; i < kSpriteCount; ++i)
    {
        const float w = rng.Range(kSpriteMin, kSpriteMax);
        const float h = rng.Range(kSpriteMin, kSpriteMax);
        // 스프라이트가 화면 안에 들어오도록 위치 범위를 크기만큼 줄인다.
        const float x = rng.Range(0.0f, static_cast<float>(m_width)  - w);
        const float y = rng.Range(0.0f, static_cast<float>(m_height) - h);

        // 스프라이트 하나에 색 하나. 눈으로 개수가 느껴지도록 알록달록하게.
        const float r = rng.Range(0.2f, 1.0f);
        const float g = rng.Range(0.2f, 1.0f);
        const float b = rng.Range(0.2f, 1.0f);

        // 네 꼭짓점을 픽셀 좌표로 직접 굽는다. 시계 방향(0→1→2→3)이라 앞면으로 인정된다.
        //
        //   0 ────── 1
        //   │        │      위쪽 삼각형: 0, 1, 2
        //   │        │      아래쪽 삼각형: 0, 2, 3
        //   3 ────── 2
        const uint32_t base = i * 4;
        vertices.push_back({ x,     y,     r, g, b, 1.0f });   // 0 좌상
        vertices.push_back({ x + w, y,     r, g, b, 1.0f });   // 1 우상
        vertices.push_back({ x + w, y + h, r, g, b, 1.0f });   // 2 우하
        vertices.push_back({ x,     y + h, r, g, b, 1.0f });   // 3 좌하

        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }

    // 위치가 고정이라 두 버퍼 모두 IMMUTABLE로 둘 수 있다. GPU가 가장 빠른 메모리에 올린다.
    // (스프라이트를 매 프레임 움직이게 되면 DYNAMIC + Map으로 바꾼다. 그게 진짜 배칭의 다음 단계다.)
    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
    vbDesc.Usage     = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData{};
    vbData.pSysMem = vertices.data();

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

    if (!HR_CHECK(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0),
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

    // ── 파이프라인에 필요한 것 꽂기 (드로우 방식과 무관하게 공통) ──
    UINT stride = sizeof(Vertex);
    UINT offset = 0;

    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    // 인덱스가 uint32라 R32_UINT. (단일 스프라이트 예제는 uint16이라 R16이었다)
    m_context->IASetIndexBuffer(m_indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->RSSetState(m_rasterizerState.Get());

    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);

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
    // 두 모드는 완전히 같은 정점·인덱스 버퍼에서 똑같은 픽셀을 그린다.
    // 다른 것은 오직 DrawIndexed를 몇 번 부르느냐뿐이다.
    // 그래서 프레임 시간의 차이는 순수하게 "드로우 콜 개수"의 비용이라고 말할 수 있다.
    if (m_mode == RenderMode::Naive)
    {
        // 스프라이트마다 한 번씩. 인덱스 6개를 i*6 위치부터 읽는다.
        // 드로우 콜 하나하나가 CPU->드라이버 왕복 비용을 낸다. 이게 쌓여 느려진다.
        for (UINT i = 0; i < m_spriteCount; ++i)
            m_context->DrawIndexed(6, i * 6, 0);

        m_drawCalls = m_spriteCount;
    }
    else // Batched
    {
        // 전부 한 방에. 인덱스 N*6개를 0번부터 통째로.
        m_context->DrawIndexed(m_indexCount, 0, 0);

        m_drawCalls = 1;
    }

    // 첫 인자 SyncInterval = 0 -> VSync OFF.
    // 성능 비교가 목적이라 반드시 0이어야 한다. 1이면 프레임 시간이 모니터 주사율(예: 60Hz)에
    // 고정돼 두 모드가 똑같이 16.6ms로 보이고, 드로우 콜 비용 차이가 통째로 묻힌다.
    m_swapChain->Present(0, 0);
}
