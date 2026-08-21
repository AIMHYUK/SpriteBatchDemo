// 인스턴싱 전용 정점 셰이더
//
// Batched는 스프라이트마다 정점 4개(위치·UV·색)를 다 만들어 올렸다.
// 인스턴싱은 "단위 사각형 1개"(정점 4개)만 두고, 스프라이트별 데이터는 per-instance로 따로 받는다.
// 정점 셰이더가 (단위 사각형 코너 × 인스턴스 크기 + 인스턴스 위치)로 실제 위치를 만든다.
//
// 이러면 GPU에 올리는 데이터가 확 준다:
//   Batched   : 스프라이트당 정점 4개 (각 위치+UV+색)  → 업로드 큼
//   Instanced : 스프라이트당 인스턴스 1개 (위치·크기·UV·색) → 약 1/3

cbuffer FrameConstants : register(b0)
{
    float2 screenSize;
    float2 padding;
};

struct VSInput
{
    // 슬롯 0: 단위 사각형 코너 (0,0)~(1,1). 4개 정점이 모든 인스턴스에 공유된다.
    float2 corner : CORNER;

    // 슬롯 1: 스프라이트 하나당 하나(per-instance). 인스턴스마다 값이 바뀐다.
    float2 iPos  : INST_POS;    // 좌상단 픽셀 위치
    float2 iSize : INST_SIZE;   // 크기(픽셀)
    float4 iUV   : INST_UV;     // 아틀라스 UV 사각형 (u0,v0,u1,v1)
    float3 iCol  : INST_COLOR;  // 색
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    // 단위 코너(0~1)를 이 인스턴스의 위치·크기로 펼쳐 실제 픽셀 좌표를 만든다.
    float2 pixel = input.iPos + input.corner * input.iSize;

    // 픽셀 -> NDC (Batched와 동일)
    float2 ndc = pixel / screenSize;
    ndc.x = ndc.x * 2.0f - 1.0f;
    ndc.y = 1.0f - ndc.y * 2.0f;
    output.position = float4(ndc, 0.0f, 1.0f);

    // UV도 코너로 보간: 코너 (0,0)->(u0,v0), (1,1)->(u1,v1)
    output.uv    = lerp(input.iUV.xy, input.iUV.zw, input.corner);
    output.color = float4(input.iCol, 1.0f);

    return output;
}
