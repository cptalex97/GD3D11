#pragma once
// GOUC: NVIDIA DLSS super resolution / DLAA through an out-of-process x64 helper.
//
// DLSS only exists for 64-bit processes, Gothic is 32-bit. The renderer therefore copies the
// finished scene colour (after HDR tonemapping, SMAA/TAA-free, GOUC grading and underwater FX,
// before the HUD), the depth and the motion vectors into shared textures, and the helper
// (GD3D11\Bin\dlss\gouc_dlss_host.exe) runs DLSS on them and hands back the upscaled image.
//
// DLSS gets exactly the image the player would otherwise see, so every GOUC clamp (fog, rain,
// HDR exposure floor, gamma/brightness, draw distance) stays in force. The input is the tonemapped
// LDR image, DLSS runs without its own exposure handling and nothing here can brighten a scene.
//
// While the helper is not ready yet, or whenever it fails, the frame falls back to FSR 3.

#include <string>

class D3D11GraphicsEngine;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;

namespace GoucDlss {

struct FrameInputs {
    ID3D11Texture2D* Color = nullptr;   // render resolution, the HDR back buffer after post FX
    ID3D11Texture2D* Depth = nullptr;   // render resolution, R32 (inverted, infinite far)
    ID3D11Texture2D* Motion = nullptr;  // render resolution, R16G16 in UV units (prev - curr)
    ID3D11RenderTargetView* Output = nullptr; // display resolution back buffer
    int RenderWidth = 0, RenderHeight = 0;
    int OutputWidth = 0, OutputHeight = 0;
    float JitterX = 0.0f, JitterY = 0.0f;     // render pixels, as applied to the projection
    float FrameTimeMs = 0.0f;
    int ResolutionScalePercent = 100;
};

// Runs DLSS for this frame. Returns false if the caller has to produce the frame itself (helper
// still starting, not available on this machine, or failed).
bool Evaluate( D3D11GraphicsEngine& engine, const FrameInputs& in );

// DLSS is not selected (any more): stop the helper and free its textures.
void Stop();

// One line for the settings menu.
std::string StatusText();
bool IsRunning();

// Test switches for the settings menu (not saved; defaults come from dlss\gouc_dlss.ini).
struct TestOptions {
    bool InvertJitterX = false;
    bool InvertJitterY = false;
    bool InvertMotionX = false;
    bool InvertMotionY = false;
    int Preset = 0;                     // NVSDK_NGX_DLSS_Hint_Render_Preset, 0 = DLSS default
};
TestOptions& Options();

} // namespace GoucDlss
