//--------------------------------------------------------------------------------------
// GOUC: weather/season colour grading of the 3D scene (see GoucWeather.h)
//
// Runs after HDR/SMAA and before the HUD. Deliberately subtle:
//  - colour temperature shifts each channel by at most 5 % and keeps the luminance
//  - saturation 0.85 .. 1.10
//  - contrast 0.95 .. 1.05 as a power curve around mid grey, so black stays black
//    (a linear contrast pivot would lift blacks and let players see better at night)
//--------------------------------------------------------------------------------------

cbuffer GoucGrade : register( b0 )
{
	float GG_Temp;
	float GG_Sat;
	float GG_Contrast;
	float GG_Pad;
};

SamplerState SS_Linear : register( s0 );
Texture2D	TX_Texture0 : register( t0 );

struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float3 vEyeRay			: TEXCOORD1;
	float4 vPosition		: SV_POSITION;
};

static const float3 GOUC_LUMA = float3( 0.2126, 0.7152, 0.0722 );

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float4 color = TX_Texture0.Sample( SS_Linear, Input.vTexcoord );
	float3 c = max( color.rgb, 0.0 );
	float lumaIn = dot( c, GOUC_LUMA );

	// White balance, luminance preserving
	float t = clamp( GG_Temp, -1.0, 1.0 );
	c *= float3( 1.0 + 0.05 * t, 1.0 + 0.01 * t, 1.0 - 0.05 * t );
	float lumaTemp = dot( c, GOUC_LUMA );
	c *= lumaIn / max( lumaTemp, 1e-5 );

	// Saturation
	c = lerp( lumaIn.xxx, c, clamp( GG_Sat, 0.85, 1.10 ) );
	c = max( c, 0.0 );

	// Contrast: power curve around mid grey, 0 stays 0
	c = 0.5 * pow( c / 0.5, clamp( GG_Contrast, 0.95, 1.05 ) );

	return float4( c, color.a );
}
