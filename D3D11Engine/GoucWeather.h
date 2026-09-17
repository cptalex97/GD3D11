#pragma once

// GOUC: weather and season overlay, driven by the game server.
//
// A script-created vob whose name starts with GOUC_WEATHER carries the target look as tokens, e.g.
// "GOUC_WEATHER FADE=60 SUN=0.85 SHADOW=0.6 FOG=2.5 TEMP=-0.4". The name is only read when the vob
// enters the world, so the script replaces the vob whenever the look changes.
//
// The overlay is applied where the renderer READS its settings and is never written back into
// GothicRendererSettings. It can therefore never end up in UserSettings.ini, and the player's own
// settings stay untouched underneath.
//
// Every value is clamped here on purpose. The ranges are deliberately narrow: the look should shift
// with the weather, not turn into a filter, and nothing here may brighten dark scenes.
//
// While a control vob is present, the renderer also stops rolling its own random rain times per
// client (GothicAPI::OnWorldUpdate), because the server decides when it rains.

// Included from GothicAPI.h after pch.h, which already brings in Windows (DWORD) with NOMINMAX.
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <string>

#include "Toolbox.h"

class zCVob;

struct GoucWeatherParams {
    float SunR = 1.0f;      // SUNR     sun light tint, red
    float SunG = 1.0f;      // SUNG     sun light tint, green
    float SunB = 1.0f;      // SUNB     sun light tint, blue
    float Sun = 1.0f;       // SUN      sun light strength
    float Shadow = 1.0f;    // SHADOW   dynamic shadow strength (overcast = softer)
    float Fog = 1.0f;       // FOG      fog density
    float FogH = 0.0f;      // FOGH     fog height offset in cm
    float FogR = 1.0f;      // FOGR     fog tint, red
    float FogG = 1.0f;      // FOGG     fog tint, green
    float FogB = 1.0f;      // FOGB     fog tint, blue
    float Wind = 1.0f;      // WIND     vegetation wind strength
    float Wet = 0.0f;       // WET      minimum scene wetness (dew, wet ground after rain)
    float Rain = 1.0f;      // RAIN     rain intensity (drizzle < 1)
    float RainX = 0.0f;     // RAINX    added rain velocity (slanted rain in a storm)
    float RainZ = 0.0f;     // RAINZ
    float SkyDesat = 0.0f;  // SKYDESAT sky colour towards grey
    float SkyCool = 0.0f;   // SKYCOOL  sky colour cooler (+) or warmer (-)
    float SkySun = 1.0f;    // SKYSUN   sky brightness
    float Rays = 1.0f;      // RAYS     god ray weight
    float Temp = 0.0f;      // TEMP     colour temperature of the scene, -1 cool .. +1 warm
    float Sat = 1.0f;       // SAT      saturation of the scene
    float Contrast = 1.0f;  // CON      contrast of the scene
};

struct GoucWeatherField {
    const char* Key;
    float GoucWeatherParams::* Member;
    float Min;
    float Max;
};

inline const GoucWeatherField GOUC_WEATHER_FIELDS[] = {
    { "SUNR",     &GoucWeatherParams::SunR,     0.85f,   1.15f },
    { "SUNG",     &GoucWeatherParams::SunG,     0.85f,   1.15f },
    { "SUNB",     &GoucWeatherParams::SunB,     0.85f,   1.15f },
    { "SUN",      &GoucWeatherParams::Sun,      0.50f,   1.20f },
    { "SHADOW",   &GoucWeatherParams::Shadow,   0.30f,   1.00f },
    { "FOG",      &GoucWeatherParams::Fog,      0.50f,   8.00f },
    { "FOGH",     &GoucWeatherParams::FogH,  -3000.0f, 8000.0f },
    { "FOGR",     &GoucWeatherParams::FogR,     0.80f,   1.20f },
    { "FOGG",     &GoucWeatherParams::FogG,     0.80f,   1.20f },
    { "FOGB",     &GoucWeatherParams::FogB,     0.80f,   1.20f },
    { "WIND",     &GoucWeatherParams::Wind,     0.50f,   3.00f },
    { "WET",      &GoucWeatherParams::Wet,      0.00f,   1.00f },
    { "RAIN",     &GoucWeatherParams::Rain,     0.20f,   1.00f },
    { "RAINX",    &GoucWeatherParams::RainX, -1500.0f, 1500.0f },
    { "RAINZ",    &GoucWeatherParams::RainZ, -1500.0f, 1500.0f },
    { "SKYDESAT", &GoucWeatherParams::SkyDesat, 0.00f,   0.60f },
    { "SKYCOOL",  &GoucWeatherParams::SkyCool, -1.00f,   1.00f },
    { "SKYSUN",   &GoucWeatherParams::SkySun,   0.60f,   1.10f },
    { "RAYS",     &GoucWeatherParams::Rays,     0.00f,   1.20f },
    { "TEMP",     &GoucWeatherParams::Temp,    -1.00f,   1.00f },
    { "SAT",      &GoucWeatherParams::Sat,      0.85f,   1.10f },
    { "CON",      &GoucWeatherParams::Contrast, 0.95f,   1.05f },
};

// Without a control vob for this long, the look fades back to neutral and the renderer rolls its
// own rain again. Long enough to survive the script replacing the vob and a world change.
inline constexpr DWORD GOUC_WEATHER_LOST_GRACE_MS = 20000;
inline constexpr float GOUC_WEATHER_LOST_FADE_MS = 10000.0f;
inline constexpr float GOUC_WEATHER_MAX_FADE_SECONDS = 600.0f;

struct GoucWeatherState {
    GoucWeatherParams Start;
    GoucWeatherParams Target;
    GoucWeatherParams Current;
    float FadeMs = 0.0f;
    DWORD FadeStartMs = 0;
    zCVob* ControlVob = nullptr;
    DWORD LostSinceMs = 0;
    bool Active = false;
};

inline GoucWeatherState& GoucWeather() {
    static GoucWeatherState state;
    return state;
}

inline const GoucWeatherParams& GoucWeatherCur() {
    return GoucWeather().Current;
}

/** True while the server controls the weather. The renderer then leaves the rain times alone. */
inline bool GoucWeatherControlsRain() {
    return GoucWeather().Active;
}

/** Reads a control vob name. Gothic stores object names in upper case; unknown keys are ignored. */
inline bool GoucWeatherParseName( const std::string& rawName, GoucWeatherParams& params, float& fadeSeconds ) {
    std::string name = rawName;
    std::transform( name.begin(), name.end(), name.begin(),
        []( unsigned char c ) { return static_cast<char>(std::toupper( c )); } );

    static const char* const prefix = "GOUC_WEATHER";
    if ( name.rfind( prefix, 0 ) != 0 ) {
        return false;
    }

    params = GoucWeatherParams();
    fadeSeconds = 30.0f;

    size_t pos = 0;
    while ( pos < name.size() ) {
        size_t end = name.find_first_of( " ;", pos );
        if ( end == std::string::npos ) {
            end = name.size();
        }
        const std::string token = name.substr( pos, end - pos );
        pos = end + 1;

        const size_t eq = token.find( '=' );
        if ( eq == std::string::npos ) {
            continue;
        }

        // from_chars is locale independent, unlike atof
        float value = 0.0f;
        const char* first = token.data() + eq + 1;
        const char* last = token.data() + token.size();
        if ( std::from_chars( first, last, value ).ec != std::errc() ) {
            continue;
        }

        const std::string key = token.substr( 0, eq );
        if ( key == "FADE" ) {
            fadeSeconds = std::clamp( value, 0.0f, GOUC_WEATHER_MAX_FADE_SECONDS );
            continue;
        }
        for ( const auto& field : GOUC_WEATHER_FIELDS ) {
            if ( key == field.Key ) {
                params.*(field.Member) = std::clamp( value, field.Min, field.Max );
                break;
            }
        }
    }

    return true;
}

inline void GoucWeatherBeginFade( const GoucWeatherParams& target, float fadeMs ) {
    auto& s = GoucWeather();
    s.Start = s.Current;
    s.Target = target;
    s.FadeMs = fadeMs;
    s.FadeStartMs = Toolbox::timeSinceStartMs();
}

inline void GoucWeatherOnControlVobAdded( zCVob* vob, const GoucWeatherParams& params, float fadeSeconds ) {
    auto& s = GoucWeather();
    s.ControlVob = vob;
    s.Active = true;
    GoucWeatherBeginFade( params, fadeSeconds * 1000.0f );
}

inline void GoucWeatherOnVobRemoved( zCVob* vob ) {
    auto& s = GoucWeather();
    if ( vob && vob == s.ControlVob ) {
        s.ControlVob = nullptr;
        s.LostSinceMs = Toolbox::timeSinceStartMs();
    }
}

/** The world is being unloaded: the vob pointer becomes invalid, the look is kept for the grace time. */
inline void GoucWeatherOnWorldReset() {
    auto& s = GoucWeather();
    if ( s.ControlVob ) {
        s.ControlVob = nullptr;
        s.LostSinceMs = Toolbox::timeSinceStartMs();
    }
}

/** Once per frame: advances the fade and falls back to neutral if the server stopped sending. */
inline void GoucWeatherUpdate() {
    auto& s = GoucWeather();
    const DWORD now = Toolbox::timeSinceStartMs();

    if ( s.Active && !s.ControlVob && now - s.LostSinceMs > GOUC_WEATHER_LOST_GRACE_MS ) {
        s.Active = false;
        GoucWeatherBeginFade( GoucWeatherParams(), GOUC_WEATHER_LOST_FADE_MS );
    }

    float t = 1.0f;
    if ( s.FadeMs > 0.0f ) {
        t = std::clamp( static_cast<float>(now - s.FadeStartMs) / s.FadeMs, 0.0f, 1.0f );
    }
    t = t * t * (3.0f - 2.0f * t);

    for ( const auto& field : GOUC_WEATHER_FIELDS ) {
        const float a = s.Start.*(field.Member);
        const float b = s.Target.*(field.Member);
        s.Current.*(field.Member) = a + (b - a) * t;
    }
}

/** The colour grading pass only runs when it would change the image. */
inline bool GoucWeatherGradeActive() {
    const auto& c = GoucWeatherCur();
    return std::abs( c.Temp ) > 0.001f || std::abs( c.Sat - 1.0f ) > 0.001f || std::abs( c.Contrast - 1.0f ) > 0.001f;
}
