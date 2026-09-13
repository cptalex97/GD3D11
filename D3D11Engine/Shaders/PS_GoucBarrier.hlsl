//--------------------------------------------------------------------------------------
// GOUC: magic barrier pixel shader, drawn with additive blending (SRC_ALPHA, ONE)
//--------------------------------------------------------------------------------------

cbuffer GoucBarrierPS : register( b0 )
{
	float GBP_Intensity;
	float GBP_Pad0;
	float GBP_Pad1;
	float GBP_Pad2;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
Texture2D	TX_Texture0 : register( t0 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord	: TEXCOORD0;
	float  vAlpha		: TEXCOORD1;
	float4 vPosition	: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float4 color = TX_Texture0.Sample( SS_Linear, Input.vTexcoord );
	return float4( color.rgb, Input.vAlpha * GBP_Intensity );
}
