// 픽셀 셰이더 (Pixel Shader)
//
// 삼각형이 덮은 픽셀 하나당 한 번 실행된다.
// 아틀라스에서 도형의 색·알파를 읽어, 정점 색을 곱해 최종 색을 만든다.

// t0/s0 슬롯. C++의 PSSetShaderResources(0,...) / PSSetSamplers(0,...)와 짝이다.
Texture2D    gAtlas   : register(t0);
SamplerState gSampler : register(s0);

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR;
};

// 반환값의 SV_Target은 "이 색을 렌더 타깃(백버퍼)에 쓴다"는 뜻이다.
float4 main(PSInput input) : SV_Target
{
    // 아틀라스는 흰색 도형 + 알파(도형 안쪽 1, 바깥 0)로 구워져 있다.
    // 여기에 정점 색을 곱하면 그 도형이 그 색으로 물든다.
    float4 texel = gAtlas.Sample(gSampler, input.uv);
    return texel * input.color;
    // 알파는 블렌드 상태(SrcAlpha, InvSrcAlpha)가 받아 가장자리를 배경과 부드럽게 섞는다.
}
