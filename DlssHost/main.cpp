// gouc_dlss_host.exe -- x64 helper that runs NVIDIA DLSS for the 32-bit GD3D11 renderer.
// License: MIT (LICENSE in this folder), NOT the GPL of GD3D11. Uses the NVIDIA DLSS SDK (NOTICE.txt).
//
// Started by the renderer (D3D11Engine/GoucDlss.cpp) with:
//   gouc_dlss_host.exe --pipe <named pipe> --luid <high>:<low> --log <0|1>
// It creates its own D3D11 device on the same GPU, opens the textures the renderer shares with it,
// and answers every frame message with one DLSS evaluation. Wire format: GoucDlssProtocol.h.
//
// The helper only ever sees the image the renderer would show anyway and writes the upscaled
// version of it back -- it has no access to any renderer setting. It exits as soon as the pipe
// closes; the renderer also keeps it in a kill-on-close job object.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

#include "GoucDlssProtocol.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::ComPtr;
using namespace GoucDlssWire;

namespace {

// Own project id for NGX (custom engine). Only identifies this integration towards the driver.
constexpr const char* NGX_PROJECT_ID = "434a8fb3-8de5-425b-a647-0c4baecdff2f";
constexpr const char* NGX_ENGINE_VERSION = "gouc-dlss-1";

constexpr DWORD MUTEX_TIMEOUT_MS = 500;
constexpr int32_t RESULT_MUTEX_TIMEOUT = -2;
constexpr int32_t RESULT_NO_FEATURE = -3;

// ---------------------------------------------------------------------------------------------
// Log

CRITICAL_SECTION g_logLock;
FILE* g_log = nullptr;

void Log( const char* fmt, ... ) {
    if ( !g_log ) return;
    EnterCriticalSection( &g_logLock );
    SYSTEMTIME t;
    GetLocalTime( &t );
    fprintf( g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds );
    va_list ap;
    va_start( ap, fmt );
    vfprintf( g_log, fmt, ap );
    va_end( ap );
    fputc( '\n', g_log );
    fflush( g_log );
    LeaveCriticalSection( &g_logLock );
}

void NVSDK_CONV NgxLog( const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature ) {
    if ( !g_log || !message ) return;
    // NGX lines already end with a newline.
    std::string line( message );
    while ( !line.empty() && (line.back() == '\n' || line.back() == '\r') ) line.pop_back();
    Log( "[NGX] %s", line.c_str() );
}

// ---------------------------------------------------------------------------------------------
// Helpers

std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW( nullptr, path, MAX_PATH );
    std::wstring dir( path, n );
    size_t slash = dir.find_last_of( L"\\/" );
    return slash == std::wstring::npos ? std::wstring() : dir.substr( 0, slash + 1 );
}

std::string Utf8( const std::wstring& w ) {
    if ( w.empty() ) return std::string();
    int n = WideCharToMultiByte( CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr );
    std::string s( static_cast<size_t>(n), '\0' );
    WideCharToMultiByte( CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr );
    return s;
}

void CopyText( char* dst, size_t cap, const std::string& src ) {
    strncpy_s( dst, cap, src.c_str(), _TRUNCATE );
}

const char* QualityName( int q ) {
    switch ( q ) {
    case NVSDK_NGX_PerfQuality_Value_DLAA: return "DLAA";
    case NVSDK_NGX_PerfQuality_Value_MaxQuality: return "Quality";
    case NVSDK_NGX_PerfQuality_Value_Balanced: return "Balanced";
    case NVSDK_NGX_PerfQuality_Value_MaxPerf: return "Performance";
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance: return "Ultra Performance";
    case NVSDK_NGX_PerfQuality_Value_UltraQuality: return "Ultra Quality";
    }
    return "?";
}

std::string PresetName( uint32_t p ) {
    if ( p == 0 ) return "default";
    if ( p >= NVSDK_NGX_DLSS_Hint_Render_Preset_J && p <= NVSDK_NGX_DLSS_Hint_Render_Preset_M ) {
        return std::string( 1, static_cast<char>('A' + (p - 1)) );
    }
    return std::to_string( p );
}

std::string Hex( int32_t v ) {
    char b[16];
    sprintf_s( b, "0x%08X", static_cast<unsigned>(v) );
    return b;
}

// ---------------------------------------------------------------------------------------------
// Pipe

HANDLE g_pipe = INVALID_HANDLE_VALUE;

bool Send( const void* data, DWORD size ) {
    DWORD written = 0;
    return WriteFile( g_pipe, data, size, &written, nullptr ) && written == size;
}

// ---------------------------------------------------------------------------------------------
// DLSS state

struct Shared {
    ComPtr<ID3D11Texture2D> Tex;
    ComPtr<IDXGIKeyedMutex> Mutex;
};

struct Host {
    ComPtr<ID3D11Device> Device;
    ComPtr<ID3D11Device1> Device1;
    ComPtr<ID3D11DeviceContext> Context;
    NVSDK_NGX_Parameter* Params = nullptr;
    bool NgxReady = false;

    NVSDK_NGX_Handle* Feature = nullptr;
    Shared Color, Depth, Motion, Out;
    ComPtr<ID3D11Texture2D> PrivateOut; // DLSS writes here (UAV), then it is copied into Out
    uint32_t FramesDone = 0;
    uint32_t FailuresLogged = 0;

    void ReleaseFeature() {
        if ( Feature ) {
            NVSDK_NGX_D3D11_ReleaseFeature( Feature );
            Feature = nullptr;
        }
        Color = {};
        Depth = {};
        Motion = {};
        Out = {};
        PrivateOut.Reset();
    }
};

Host g_host;

bool OpenShared( uint64_t handleValue, Shared& s ) {
    HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handleValue));
    if ( !h ) return false;
    HRESULT hr = g_host.Device1->OpenSharedResource1( h, IID_PPV_ARGS( s.Tex.GetAddressOf() ) );
    CloseHandle( h );
    if ( FAILED( hr ) ) {
        Log( "OpenSharedResource1 failed: 0x%08X", static_cast<unsigned>(hr) );
        return false;
    }
    if ( FAILED( s.Tex.As( &s.Mutex ) ) ) {
        Log( "shared texture has no keyed mutex" );
        return false;
    }
    return true;
}

bool QualityFits( int q, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh ) {
    unsigned optW = 0, optH = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
    float sharpness = 0.0f;
    NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS( g_host.Params, ow, oh, static_cast<NVSDK_NGX_PerfQuality_Value>(q),
        &optW, &optH, &maxW, &maxH, &minW, &minH, &sharpness );
    if ( NVSDK_NGX_FAILED( r ) || optW == 0 || optH == 0 ) {
        Log( "  %s: not offered (%s)", QualityName( q ), Hex( r ).c_str() );
        return false;
    }
    const bool fits = rw >= minW && rw <= maxW && rh >= minH && rh <= maxH;
    Log( "  %s: optimal %ux%u, range %ux%u .. %ux%u%s", QualityName( q ), optW, optH, minW, minH, maxW, maxH,
        fits ? "  <- fits" : "" );
    return fits;
}

void HandleConfig( const MsgConfig& m ) {
    MsgConfigAck ack{};
    ack.H = MakeHeader( MSG_CONFIG_ACK, sizeof( ack ) );

    g_host.ReleaseFeature();
    g_host.FramesDone = 0;
    g_host.FailuresLogged = 0;

    Log( "config: render %ux%u -> output %ux%u, quality %s, preset %s, flags 0x%X", m.RenderWidth, m.RenderHeight,
        m.OutputWidth, m.OutputHeight, QualityName( m.PerfQuality ), PresetName( m.Preset ).c_str(), m.CreateFlags );

    if ( !OpenShared( m.ColorHandle, g_host.Color ) || !OpenShared( m.DepthHandle, g_host.Depth )
        || !OpenShared( m.MotionHandle, g_host.Motion ) || !OpenShared( m.OutputHandle, g_host.Out ) ) {
        CopyText( ack.Text, sizeof( ack.Text ), "could not open the shared textures" );
        g_host.ReleaseFeature();
        Send( &ack, sizeof( ack ) );
        return;
    }

    D3D11_TEXTURE2D_DESC od{};
    g_host.Out.Tex->GetDesc( &od );
    od.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    od.MiscFlags = 0;
    if ( FAILED( g_host.Device->CreateTexture2D( &od, nullptr, g_host.PrivateOut.GetAddressOf() ) ) ) {
        CopyText( ack.Text, sizeof( ack.Text ), "could not create the output texture" );
        g_host.ReleaseFeature();
        Send( &ack, sizeof( ack ) );
        return;
    }

    // Pick the NGX quality mode whose dynamic input range holds the render size the renderer uses.
    int quality = m.PerfQuality;
    if ( m.RenderWidth >= m.OutputWidth && m.RenderHeight >= m.OutputHeight ) {
        quality = NVSDK_NGX_PerfQuality_Value_DLAA;
    } else if ( !QualityFits( quality, m.RenderWidth, m.RenderHeight, m.OutputWidth, m.OutputHeight ) ) {
        const int order[] = {
            NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_UltraQuality,
            NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
            NVSDK_NGX_PerfQuality_Value_UltraPerformance,
        };
        for ( int q : order ) {
            if ( q != m.PerfQuality && QualityFits( q, m.RenderWidth, m.RenderHeight, m.OutputWidth, m.OutputHeight ) ) {
                quality = q;
                break;
            }
        }
    }

    const unsigned preset = m.Preset;
    NVSDK_NGX_Parameter_SetUI( g_host.Params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset );
    NVSDK_NGX_Parameter_SetUI( g_host.Params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset );
    NVSDK_NGX_Parameter_SetUI( g_host.Params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset );
    NVSDK_NGX_Parameter_SetUI( g_host.Params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, preset );
    NVSDK_NGX_Parameter_SetUI( g_host.Params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, preset );
    NVSDK_NGX_Parameter_SetUI( g_host.Params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, preset );

    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = m.RenderWidth;
    cp.Feature.InHeight = m.RenderHeight;
    cp.Feature.InTargetWidth = m.OutputWidth;
    cp.Feature.InTargetHeight = m.OutputHeight;
    cp.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(quality);
    cp.InFeatureCreateFlags = static_cast<int>(m.CreateFlags);
    cp.InEnableOutputSubrects = false;

    NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT( g_host.Context.Get(), &g_host.Feature, g_host.Params, &cp );
    ack.NgxResult = static_cast<int32_t>(r);
    if ( NVSDK_NGX_FAILED( r ) || !g_host.Feature ) {
        Log( "CreateFeature failed: %s", Hex( r ).c_str() );
        CopyText( ack.Text, sizeof( ack.Text ), "DLSS feature creation failed (NGX " + Hex( r ) + ")" );
        g_host.ReleaseFeature();
        Send( &ack, sizeof( ack ) );
        return;
    }

    char text[200];
    sprintf_s( text, "%ux%u -> %ux%u, %s, preset %s", m.RenderWidth, m.RenderHeight, m.OutputWidth, m.OutputHeight,
        QualityName( quality ), PresetName( preset ).c_str() );
    Log( "feature created: %s", text );
    ack.Ok = 1;
    CopyText( ack.Text, sizeof( ack.Text ), text );
    Send( &ack, sizeof( ack ) );
}

void HandleFrame( const MsgFrame& f ) {
    MsgFrameAck ack{};
    ack.H = MakeHeader( MSG_FRAME_ACK, sizeof( ack ) );
    ack.FrameIndex = f.FrameIndex;

    if ( !g_host.Feature ) {
        ack.NgxResult = RESULT_NO_FEATURE;
        Send( &ack, sizeof( ack ) );
        return;
    }

    // Inputs: renderer released them to key 1. Output: free at key 0.
    const bool cIn = g_host.Color.Mutex->AcquireSync( 1, MUTEX_TIMEOUT_MS ) == S_OK;
    const bool dIn = g_host.Depth.Mutex->AcquireSync( 1, MUTEX_TIMEOUT_MS ) == S_OK;
    const bool mIn = g_host.Motion.Mutex->AcquireSync( 1, MUTEX_TIMEOUT_MS ) == S_OK;
    const bool oIn = g_host.Out.Mutex->AcquireSync( 0, MUTEX_TIMEOUT_MS ) == S_OK;

    int32_t result = RESULT_MUTEX_TIMEOUT;
    if ( cIn && dIn && mIn && oIn ) {
        NVSDK_NGX_D3D11_DLSS_Eval_Params ev{};
        ev.Feature.pInColor = g_host.Color.Tex.Get();
        ev.Feature.pInOutput = g_host.PrivateOut.Get();
        ev.pInDepth = g_host.Depth.Tex.Get();
        ev.pInMotionVectors = g_host.Motion.Tex.Get();
        ev.InJitterOffsetX = f.JitterX;
        ev.InJitterOffsetY = f.JitterY;
        ev.InRenderSubrectDimensions.Width = f.RenderWidth;
        ev.InRenderSubrectDimensions.Height = f.RenderHeight;
        ev.InReset = f.Reset ? 1 : 0;
        ev.InMVScaleX = f.MotionScaleX;
        ev.InMVScaleY = f.MotionScaleY;
        ev.InFrameTimeDeltaInMsec = f.FrameTimeMs;

        NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT( g_host.Context.Get(), g_host.Feature, g_host.Params, &ev );
        result = static_cast<int32_t>(r);
        if ( NVSDK_NGX_SUCCEED( r ) ) {
            g_host.Context->CopyResource( g_host.Out.Tex.Get(), g_host.PrivateOut.Get() );
            result = NVSDK_NGX_Result_Success;
        }
    }

    if ( oIn ) g_host.Out.Mutex->ReleaseSync( 1 );
    if ( mIn ) g_host.Motion.Mutex->ReleaseSync( 0 );
    if ( dIn ) g_host.Depth.Mutex->ReleaseSync( 0 );
    if ( cIn ) g_host.Color.Mutex->ReleaseSync( 0 );
    g_host.Context->Flush();

    if ( result == NVSDK_NGX_Result_Success ) {
        if ( g_host.FramesDone++ == 0 ) {
            Log( "first frame evaluated (jitter %.3f/%.3f, mv scale %.0f/%.0f, %.2f ms)", f.JitterX, f.JitterY,
                f.MotionScaleX, f.MotionScaleY, f.FrameTimeMs );
        }
    } else if ( g_host.FailuresLogged < 20 ) {
        ++g_host.FailuresLogged;
        Log( "frame %u failed: %s (mutex c%d d%d m%d o%d)", f.FrameIndex, Hex( result ).c_str(), cIn, dIn, mIn, oIn );
    }

    ack.NgxResult = result;
    Send( &ack, sizeof( ack ) );
}

bool InitDevice( const LUID& luid, std::string& adapterName ) {
    ComPtr<IDXGIFactory4> factory;
    if ( FAILED( CreateDXGIFactory1( IID_PPV_ARGS( factory.GetAddressOf() ) ) ) ) {
        Log( "CreateDXGIFactory1 failed" );
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if ( FAILED( factory->EnumAdapterByLuid( luid, IID_PPV_ARGS( adapter.GetAddressOf() ) ) ) ) {
        Log( "no adapter with LUID %08lX:%08lX", static_cast<unsigned long>(luid.HighPart), luid.LowPart );
        return false;
    }
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1( &desc );
    adapterName = Utf8( desc.Description );

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice( adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, ARRAYSIZE( levels ),
        D3D11_SDK_VERSION, g_host.Device.GetAddressOf(), &got, g_host.Context.GetAddressOf() );
    if ( FAILED( hr ) ) {
        Log( "D3D11CreateDevice failed: 0x%08X", static_cast<unsigned>(hr) );
        return false;
    }
    if ( FAILED( g_host.Device.As( &g_host.Device1 ) ) ) {
        Log( "no ID3D11Device1" );
        return false;
    }
    Log( "device on %s, feature level 0x%X", adapterName.c_str(), static_cast<unsigned>(got) );
    return true;
}

// Fills 'why' when DLSS cannot run on this machine.
bool InitNgx( const std::wstring& dir, bool verbose, std::string& why ) {
    const std::wstring dllDir = dir;
    const wchar_t* paths[] = { dllDir.c_str() };
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = NgxLog;
    info.LoggingInfo.MinimumLoggingLevel = verbose ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;
    info.LoggingInfo.DisableOtherLoggingSinks = true;

    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID( NGX_PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        NGX_ENGINE_VERSION, dir.c_str(), g_host.Device.Get(), &info );
    if ( NVSDK_NGX_FAILED( r ) ) {
        why = "NGX init failed (" + Hex( r ) + ") - driver too old or no RTX GPU";
        return false;
    }
    g_host.NgxReady = true;

    r = NVSDK_NGX_D3D11_GetCapabilityParameters( &g_host.Params );
    if ( NVSDK_NGX_FAILED( r ) || !g_host.Params ) {
        why = "NGX capability query failed (" + Hex( r ) + ")";
        return false;
    }

    int available = 0;
    NVSDK_NGX_Parameter_GetI( g_host.Params, NVSDK_NGX_Parameter_SuperSampling_Available, &available );
    if ( !available ) {
        int needsDriver = 0;
        unsigned major = 0, minor = 0;
        int initResult = 0;
        NVSDK_NGX_Parameter_GetI( g_host.Params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver );
        NVSDK_NGX_Parameter_GetUI( g_host.Params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major );
        NVSDK_NGX_Parameter_GetUI( g_host.Params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor );
        NVSDK_NGX_Parameter_GetI( g_host.Params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult );
        if ( needsDriver ) {
            why = "GPU driver too old for DLSS (needs " + std::to_string( major ) + "." + std::to_string( minor ) + ")";
        } else {
            why = "DLSS not available on this GPU (" + Hex( initResult ) + ")";
        }
        return false;
    }
    return true;
}

// --selftest: no renderer, no pipe. Initialises NGX on the first NVIDIA GPU, creates a DLSS feature
// with its own dummy textures and evaluates a few frames. Result in gouc_dlss_host_selftest.log.
int SelfTest( const std::wstring& dir ) {
    _wfopen_s( &g_log, (dir + L"gouc_dlss_host_selftest.log").c_str(), L"w" );
    Log( "selftest" );

    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1( IID_PPV_ARGS( factory.GetAddressOf() ) );
    LUID luid{};
    bool found = false;
    ComPtr<IDXGIAdapter1> a;
    for ( UINT i = 0; factory && factory->EnumAdapters1( i, a.ReleaseAndGetAddressOf() ) != DXGI_ERROR_NOT_FOUND; ++i ) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1( &d );
        if ( d.VendorId == 0x10DE ) {
            luid = d.AdapterLuid;
            found = true;
            break;
        }
    }
    if ( !found ) {
        Log( "RESULT: no NVIDIA adapter" );
        return 1;
    }
    std::string name, why;
    if ( !InitDevice( luid, name ) ) {
        Log( "RESULT: no device" );
        return 1;
    }
    if ( !InitNgx( dir, true, why ) ) {
        Log( "RESULT: %s", why.c_str() );
        return 1;
    }

    struct Case { uint32_t rw, rh, ow, oh; int q; };
    const Case cases[] = {
        { 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality },
        { 1286, 723, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality },  // 67 % as the menu sets it
        { 1920, 1080, 1920, 1080, NVSDK_NGX_PerfQuality_Value_DLAA },
    };
    int failures = 0;
    for ( const Case& c : cases ) {
        Log( "case %ux%u -> %ux%u %s", c.rw, c.rh, c.ow, c.oh, QualityName( c.q ) );
        if ( c.q != NVSDK_NGX_PerfQuality_Value_DLAA ) QualityFits( c.q, c.rw, c.rh, c.ow, c.oh );

        auto make = [&]( uint32_t w, uint32_t h, DXGI_FORMAT f, UINT bind ) {
            ComPtr<ID3D11Texture2D> t;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
            g_host.Device->CreateTexture2D( &d, nullptr, t.GetAddressOf() );
            return t;
        };
        auto color = make( c.rw, c.rh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET );
        auto depth = make( c.rw, c.rh, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET );
        auto motion = make( c.rw, c.rh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET );
        auto out = make( c.ow, c.oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS );

        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth = c.rw;
        cp.Feature.InHeight = c.rh;
        cp.Feature.InTargetWidth = c.ow;
        cp.Feature.InTargetHeight = c.oh;
        cp.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(c.q);
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        NVSDK_NGX_Handle* feature = nullptr;
        NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT( g_host.Context.Get(), &feature, g_host.Params, &cp );
        Log( "  create: %s", Hex( r ).c_str() );
        if ( NVSDK_NGX_FAILED( r ) || !feature ) {
            ++failures;
            continue;
        }
        for ( int i = 0; i < 3; ++i ) {
            NVSDK_NGX_D3D11_DLSS_Eval_Params ev{};
            ev.Feature.pInColor = color.Get();
            ev.Feature.pInOutput = out.Get();
            ev.pInDepth = depth.Get();
            ev.pInMotionVectors = motion.Get();
            ev.InJitterOffsetX = 0.25f;
            ev.InJitterOffsetY = -0.25f;
            ev.InRenderSubrectDimensions.Width = c.rw;
            ev.InRenderSubrectDimensions.Height = c.rh;
            ev.InReset = i == 0;
            ev.InMVScaleX = static_cast<float>(c.rw);
            ev.InMVScaleY = static_cast<float>(c.rh);
            ev.InFrameTimeDeltaInMsec = 16.6f;
            r = NGX_D3D11_EVALUATE_DLSS_EXT( g_host.Context.Get(), feature, g_host.Params, &ev );
            Log( "  evaluate %d: %s", i, Hex( r ).c_str() );
            if ( NVSDK_NGX_FAILED( r ) ) ++failures;
        }
        g_host.Context->Flush();
        NVSDK_NGX_D3D11_ReleaseFeature( feature );
    }

    NVSDK_NGX_D3D11_DestroyParameters( g_host.Params );
    NVSDK_NGX_D3D11_Shutdown1( g_host.Device.Get() );
    Log( "RESULT: %s (%d failures)", failures ? "FAILED" : "OK", failures );
    fclose( g_log );
    return failures ? 1 : 0;
}

// --selftest-ipc: plays the renderer. Starts a second copy of this exe as the real helper, shares
// textures with it exactly like GoucDlss.cpp does, sends a mid-grey frame and reads the result back.
// Checks the whole path (pipe, NT handles, keyed mutexes, DLSS) and that DLSS keeps the brightness.
int SelfTestIpc( const std::wstring& dir ) {
    _wfopen_s( &g_log, (dir + L"gouc_dlss_host_ipctest.log").c_str(), L"w" );
    Log( "ipc selftest" );

    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1( IID_PPV_ARGS( factory.GetAddressOf() ) );
    LUID luid{};
    bool found = false;
    ComPtr<IDXGIAdapter1> a;
    for ( UINT i = 0; factory && factory->EnumAdapters1( i, a.ReleaseAndGetAddressOf() ) != DXGI_ERROR_NOT_FOUND; ++i ) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1( &d );
        if ( d.VendorId == 0x10DE ) { luid = d.AdapterLuid; found = true; break; }
    }
    std::string name;
    if ( !found || !InitDevice( luid, name ) ) {
        Log( "RESULT: no NVIDIA device" );
        return 1;
    }
    ID3D11Device* dev = g_host.Device.Get();
    ID3D11DeviceContext* ctx = g_host.Context.Get();

    wchar_t pipeName[128];
    swprintf_s( pipeName, L"\\\\.\\pipe\\gouc_dlss_ipctest_%lu", GetCurrentProcessId() );
    HANDLE pipe = CreateNamedPipeW( pipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr );

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW( nullptr, exe, MAX_PATH );
    wchar_t args[512];
    swprintf_s( args, L"\"%s\" --pipe %s --luid %08lX:%08lX --log 1", exe, pipeName,
        static_cast<unsigned long>(luid.HighPart), static_cast<unsigned long>(luid.LowPart) );
    std::wstring cmd = args;
    STARTUPINFOW si{ sizeof( si ) };
    PROCESS_INFORMATION pi{};
    if ( pipe == INVALID_HANDLE_VALUE || !CreateProcessW( exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, dir.c_str(), &si, &pi ) ) {
        Log( "RESULT: could not start the child" );
        return 1;
    }
    CloseHandle( pi.hThread );
    ConnectNamedPipe( pipe, nullptr );

    uint8_t buf[1024];
    DWORD got = 0;
    auto send = [&]( const void* p, DWORD n ) { DWORD w = 0; return WriteFile( pipe, p, n, &w, nullptr ) && w == n; };
    auto recv = [&]() { got = 0; return ReadFile( pipe, buf, sizeof( buf ), &got, nullptr ) != FALSE; };

    if ( !recv() || got != sizeof( MsgHello ) || !reinterpret_cast<MsgHello*>(buf)->Ok ) {
        Log( "RESULT: no hello (%lu bytes)", got );
        return 1;
    }
    Log( "hello: %s", reinterpret_cast<MsgHello*>(buf)->Text );

    struct Tex { ComPtr<ID3D11Texture2D> T; ComPtr<IDXGIKeyedMutex> M; ComPtr<ID3D11RenderTargetView> Rtv; uint64_t Remote = 0; };
    auto make = [&]( Tex& t, UINT w, UINT h, DXGI_FORMAT f ) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if ( FAILED( dev->CreateTexture2D( &d, nullptr, t.T.GetAddressOf() ) ) ) return false;
        t.T.As( &t.M );
        dev->CreateRenderTargetView( t.T.Get(), nullptr, t.Rtv.GetAddressOf() );
        ComPtr<IDXGIResource1> r;
        HANDLE local = nullptr, remote = nullptr;
        if ( FAILED( t.T.As( &r ) ) || FAILED( r->CreateSharedHandle( nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &local ) ) ) return false;
        BOOL ok = DuplicateHandle( GetCurrentProcess(), local, pi.hProcess, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS );
        CloseHandle( local );
        t.Remote = reinterpret_cast<uintptr_t>(remote);
        return ok != FALSE;
    };
    const UINT rw = 1280, rh = 720, ow = 1920, oh = 1080;
    Tex color, depth, motion, out;
    if ( !make( color, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT ) || !make( depth, rw, rh, DXGI_FORMAT_R32_FLOAT )
        || !make( motion, rw, rh, DXGI_FORMAT_R16G16_FLOAT ) || !make( out, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM ) ) {
        Log( "RESULT: shared textures failed" );
        return 1;
    }

    MsgConfig cfg{};
    cfg.H = MakeHeader( MSG_CONFIG, sizeof( cfg ) );
    cfg.ColorHandle = color.Remote; cfg.DepthHandle = depth.Remote; cfg.MotionHandle = motion.Remote; cfg.OutputHandle = out.Remote;
    cfg.RenderWidth = rw; cfg.RenderHeight = rh; cfg.OutputWidth = ow; cfg.OutputHeight = oh;
    cfg.PerfQuality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    cfg.CreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if ( !send( &cfg, sizeof( cfg ) ) || !recv() || got != sizeof( MsgConfigAck ) || !reinterpret_cast<MsgConfigAck*>(buf)->Ok ) {
        Log( "RESULT: config failed: %s", got == sizeof( MsgConfigAck ) ? reinterpret_cast<MsgConfigAck*>(buf)->Text : "?" );
        return 1;
    }
    Log( "config ack: %s", reinterpret_cast<MsgConfigAck*>(buf)->Text );

    D3D11_TEXTURE2D_DESC sd{};
    out.T->GetDesc( &sd );
    sd.BindFlags = 0; sd.MiscFlags = 0; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    dev->CreateTexture2D( &sd, nullptr, staging.GetAddressOf() );

    int failures = 0;
    for ( uint32_t frame = 1; frame <= 8; ++frame ) {
        const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f }, half[4] = { 0.5f, 0, 0, 0 }, zero[4] = {};
        if ( color.M->AcquireSync( 0, 500 ) != S_OK || depth.M->AcquireSync( 0, 500 ) != S_OK || motion.M->AcquireSync( 0, 500 ) != S_OK ) {
            Log( "RESULT: inputs locked in frame %u", frame );
            return 1;
        }
        ctx->ClearRenderTargetView( color.Rtv.Get(), grey );
        ctx->ClearRenderTargetView( depth.Rtv.Get(), half );
        ctx->ClearRenderTargetView( motion.Rtv.Get(), zero );
        color.M->ReleaseSync( 1 ); depth.M->ReleaseSync( 1 ); motion.M->ReleaseSync( 1 );

        MsgFrame f{};
        f.H = MakeHeader( MSG_FRAME, sizeof( f ) );
        f.FrameIndex = frame; f.Reset = frame == 1;
        f.JitterX = 0.25f; f.JitterY = -0.25f;
        f.MotionScaleX = static_cast<float>(rw); f.MotionScaleY = static_cast<float>(rh);
        f.FrameTimeMs = 16.6f; f.RenderWidth = rw; f.RenderHeight = rh;
        const ULONGLONG t0 = GetTickCount64();
        if ( !send( &f, sizeof( f ) ) || !recv() || got != sizeof( MsgFrameAck ) ) {
            Log( "RESULT: no frame ack" );
            return 1;
        }
        const auto* ack = reinterpret_cast<MsgFrameAck*>(buf);
        if ( out.M->AcquireSync( 1, 500 ) != S_OK ) {
            Log( "RESULT: output locked in frame %u", frame );
            return 1;
        }
        ctx->CopyResource( staging.Get(), out.T.Get() );
        out.M->ReleaseSync( 0 );

        D3D11_MAPPED_SUBRESOURCE m{};
        double sum = 0.0;
        unsigned minV = 255, maxV = 0;
        if ( SUCCEEDED( ctx->Map( staging.Get(), 0, D3D11_MAP_READ, 0, &m ) ) ) {
            for ( UINT y = 0; y < oh; y += 7 ) {
                const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                for ( UINT x = 0; x < ow; x += 7 ) {
                    const unsigned g = row[x * 4 + 1];
                    sum += g;
                    minV = std::min( minV, g );
                    maxV = std::max( maxV, g );
                }
            }
            ctx->Unmap( staging.Get(), 0 );
        }
        const double samples = static_cast<double>(((oh + 6) / 7) * ((ow + 6) / 7));
        const double avg = sum / samples;
        const bool ok = ack->NgxResult == NVSDK_NGX_Result_Success && avg > 124.0 && avg < 132.0;
        Log( "frame %u: ngx %s, %llu ms round trip, output green avg %.2f (min %u max %u), input 0.5 = 127.5 %s",
            frame, Hex( ack->NgxResult ).c_str(), GetTickCount64() - t0, avg, minV, maxV, ok ? "OK" : "WRONG" );
        if ( !ok ) ++failures;
    }

    const Header quit = MakeHeader( MSG_QUIT, sizeof( Header ) );
    send( &quit, sizeof( quit ) );
    WaitForSingleObject( pi.hProcess, 3000 );
    CloseHandle( pi.hProcess );
    CloseHandle( pipe );
    Log( "RESULT: %s (%d failures)", failures ? "FAILED" : "OK", failures );
    fclose( g_log );
    return failures ? 1 : 0;
}

bool ParseArgs( std::wstring& pipe, LUID& luid, bool& log ) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW( GetCommandLineW(), &argc );
    if ( !argv ) return false;
    bool havePipe = false, haveLuid = false;
    for ( int i = 1; i + 1 < argc; ++i ) {
        if ( wcscmp( argv[i], L"--pipe" ) == 0 ) {
            pipe = argv[++i];
            havePipe = true;
        } else if ( wcscmp( argv[i], L"--luid" ) == 0 ) {
            unsigned long hi = 0, lo = 0;
            if ( swscanf_s( argv[++i], L"%lx:%lx", &hi, &lo ) == 2 ) {
                luid.HighPart = static_cast<LONG>(hi);
                luid.LowPart = static_cast<DWORD>(lo);
                haveLuid = true;
            }
        } else if ( wcscmp( argv[i], L"--log" ) == 0 ) {
            log = wcscmp( argv[++i], L"0" ) != 0;
        }
    }
    LocalFree( argv );
    return havePipe && haveLuid && pipe.rfind( L"\\\\.\\pipe\\gouc_dlss_", 0 ) == 0;
}

} // namespace

int WINAPI wWinMain( HINSTANCE, HINSTANCE, LPWSTR, int ) {
    InitializeCriticalSection( &g_logLock );

    if ( wcsstr( GetCommandLineW(), L"--selftest-ipc" ) ) {
        return SelfTestIpc( ExeDir() );
    }
    if ( wcsstr( GetCommandLineW(), L"--selftest" ) ) {
        return SelfTest( ExeDir() );
    }

    std::wstring pipeName;
    LUID luid{};
    bool logEnabled = true;
    if ( !ParseArgs( pipeName, luid, logEnabled ) ) {
        return 2; // not started by the renderer
    }

    const std::wstring dir = ExeDir();
    if ( logEnabled ) {
        _wfopen_s( &g_log, (dir + L"gouc_dlss_host.log").c_str(), L"w" );
    }
    Log( "gouc_dlss_host started, pipe %s", Utf8( pipeName ).c_str() );

    g_pipe = CreateFileW( pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr );
    if ( g_pipe == INVALID_HANDLE_VALUE ) {
        Log( "could not open the pipe (%lu)", GetLastError() );
        return 3;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState( g_pipe, &mode, nullptr, nullptr );

    MsgHello hello{};
    hello.H = MakeHeader( MSG_HELLO, sizeof( hello ) );
    std::string adapterName, why;
    if ( !InitDevice( luid, adapterName ) ) {
        why = "helper could not create a D3D11 device";
    } else if ( !InitNgx( dir, logEnabled, why ) ) {
        // why is set
    } else {
        hello.Ok = 1;
        why = adapterName;
    }
    Log( "hello: ok=%u %s", hello.Ok, why.c_str() );
    CopyText( hello.Text, sizeof( hello.Text ), why );
    if ( !Send( &hello, sizeof( hello ) ) || !hello.Ok ) {
        Log( "exit after hello" );
        if ( g_host.Params ) NVSDK_NGX_D3D11_DestroyParameters( g_host.Params );
        if ( g_host.NgxReady ) NVSDK_NGX_D3D11_Shutdown1( g_host.Device.Get() );
        CloseHandle( g_pipe );
        return 0;
    }

    alignas(8) uint8_t buf[1024];
    for ( ;; ) {
        DWORD got = 0;
        if ( !ReadFile( g_pipe, buf, sizeof( buf ), &got, nullptr ) ) {
            Log( "pipe closed (%lu)", GetLastError() );
            break;
        }
        if ( got < sizeof( Header ) ) continue;
        const auto* h = reinterpret_cast<const Header*>(buf);
        if ( h->Magic != MAGIC || h->Version != VERSION || h->Size != got ) {
            Log( "bad message (type %u, %lu bytes)", h->Type, got );
            continue;
        }
        if ( h->Type == MSG_QUIT ) {
            Log( "quit" );
            break;
        }
        if ( h->Type == MSG_CONFIG && got == sizeof( MsgConfig ) ) {
            HandleConfig( *reinterpret_cast<const MsgConfig*>(buf) );
        } else if ( h->Type == MSG_FRAME && got == sizeof( MsgFrame ) ) {
            HandleFrame( *reinterpret_cast<const MsgFrame*>(buf) );
        } else {
            Log( "unexpected message type %u", h->Type );
        }
    }

    g_host.ReleaseFeature();
    if ( g_host.Params ) NVSDK_NGX_D3D11_DestroyParameters( g_host.Params );
    if ( g_host.NgxReady ) NVSDK_NGX_D3D11_Shutdown1( g_host.Device.Get() );
    CloseHandle( g_pipe );
    Log( "exit" );
    if ( g_log ) fclose( g_log );
    return 0;
}
