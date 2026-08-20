// 정점 셰이더 (Vertex Shader)
//
// 정점 하나당 한 번 실행된다. 하는 일은 "이 정점이 화면 어디에 놓이는가"를 정하는 것.
// 여기서는 픽셀 좌표를 받아 NDC(-1~1)로만 바꿔준다. 스프라이트의 실제 위치·크기는
// 이미 C++이 정점에 구워 넣었으므로, 셰이더는 좌표계 변환만 담당한다.

// C++이 매 프레임 채워 넣는 값. Renderer.h의 FrameConstants와 짝이다.
// register(b0)의 0이 C++ VSSetConstantBuffers(0, ...)의 0과 만난다.
cbuffer FrameConstants : register(b0)
{
    float2 screenSize;   // 0 ~ 7   창 크기 (픽셀)
    float2 padding;      // 8 ~ 15  버퍼 전체를 16의 배수로 맞추는 용도
};

// IA(Input Assembler)가 정점 버퍼에서 꺼내 넣어주는 값.
// 뒤에 붙은 POSITION, COLOR가 시맨틱 이름이다. C++ 입력 레이아웃과 글자까지 같아야 한다.
struct VSInput
{
    float2 position : POSITION;   // 화면 픽셀 좌표 (좌상단 원점, y 아래로 +)
    float4 color    : COLOR;
};

struct VSOutput
{
    // SV_Position은 "이게 최종 화면 좌표다"라는 약속된 이름. 래스터라이저가 이 값으로
    // 삼각형을 픽셀로 쪼갠다. 반드시 있어야 한다.
    float4 position : SV_Position;
    float4 color    : COLOR;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    // 픽셀 -> 0~1 -> NDC. 두 단계다.
    float2 ndc = input.position / screenSize;   // ① 픽셀 -> 0~1

    // ② 0~1 -> NDC. 화면은 아래가 +y인데 NDC는 위가 +y라, y만 부호를 뒤집는다.
    ndc.x = ndc.x * 2.0f - 1.0f;
    ndc.y = 1.0f - ndc.y * 2.0f;

    // z=0(가장 앞), w=1. 2D라 원근이 없어 w로 나눠도 그대로 통과한다.
    output.position = float4(ndc, 0.0f, 1.0f);
    output.color    = input.color;

    return output;
}
