//--------------------------------------------------------------------------------------
// GOUC: magic barrier vertex shader
//
// Recreates Gothic 1's oCBarrier::RenderLayer (Gothic2.exe 2.6 fix, 0x006B9CF0), which
// rewrites the texture coordinates of every vertex each frame:
//
//   u = u0 * uvScale + 0.105s * sin(0.098s * (x + z) + 0.42s + t) + frac(0.15 * t) + s
//   v = v0 * uvScale + 0.105s * sin(0.098s * y       + 0.42s + t)                 + s
//   alpha = heightFade * (sin(x + z + t) + 1) / 2
//
// s is 1.0 for the first and 1.5 for the second layer, t is the time in seconds and
// x/y/z is the mesh-space vertex position in centimetres. heightFade is the vertex
// alpha oCBarrier::Init bakes into the mesh: fade in over the lowest 80 m, fade out
// above 92.5 % of the mesh height.
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer Matrices_PerInstances : register( b1 )
{
	VS_ExConstantBuffer_PerInstance cbInstance;
};

cbuffer GoucBarrierInfo : register( b2 )
{
	float GB_Time;
	float GB_LayerScale;
	float GB_UVScale;
	float GB_Wave;

	float GB_ScrollOffset;
	float GB_GroundFade;
	float GB_TopY;
	float GB_TopFade;

	float GB_Flicker;
	float GB_Pad0;
	float GB_Pad1;
	float GB_Pad2;
};

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
	float3 vPosition	: POSITION;
	float3 vNormal		: NORMAL;
	float2 vTex1		: TEXCOORD0;
	float2 vTex2		: TEXCOORD1;
	float4 vDiffuse		: DIFFUSE;
};

struct VS_OUTPUT
{
	float2 vTexcoord	: TEXCOORD0;
	float  vAlpha		: TEXCOORD1;
	float4 vPosition	: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;

	float3 p = Input.vPosition;
	float3 positionWorld = mul( float4( p, 1 ), cbInstance.M_World ).xyz;
	Output.vPosition = mul( float4( positionWorld, 1 ), frame.M_ViewProj );

	float s = GB_LayerScale;
	float amplitude = 0.105f * s * GB_Wave;
	float frequency = 0.098f * s;
	float phase = 0.42f * s;

	float2 uv = Input.vTex1 * GB_UVScale;
	uv.x += amplitude * sin( (p.x + p.z) * frequency + phase + GB_Time ) + GB_ScrollOffset + s;
	uv.y += amplitude * sin( p.y * frequency + phase + GB_Time ) + s;
	Output.vTexcoord = uv;

	float alpha = 1.0f;
	float topStart = GB_TopY * GB_TopFade;
	if ( GB_TopFade < 1.0f && p.y > topStart )
	{
		alpha = (GB_TopY - p.y) / max( GB_TopY - topStart, 1.0f );
	}
	else if ( GB_GroundFade > 0.0f )
	{
		alpha = p.y / GB_GroundFade;
	}
	alpha = saturate( alpha );

	float flicker = (sin( p.x + p.z + GB_Time ) + 1.0f) * 0.5f;
	alpha *= lerp( 1.0f, flicker, saturate( GB_Flicker ) );

	Output.vAlpha = alpha;
	return Output;
}
