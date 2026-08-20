// 픽셀 셰이더 (Pixel Shader)
//
// 삼각형이 덮은 픽셀 하나당 한 번 실행된다.
// 이 데모는 스프라이트를 작게 두어 픽셀 수를 낮췄다. 픽셀 셰이더가 병목이 되면
// 우리가 재려는 "드로우 콜 개수의 비용"이 묻히기 때문이다.

struct PSInput
{
    float4 position : SV_Position;
    float4 color    : COLOR;
};

// 반환값의 SV_Target은 "이 색을 렌더 타깃(백버퍼)에 쓴다"는 뜻이다.
float4 main(PSInput input) : SV_Target
{
    // 지금은 정점 색을 그대로 돌려준다.
    // 다음 단계에서 텍스처 아틀라스를 넣으면 여기서 Sample 결과와 곱하게 된다.
    return input.color;
}
