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
    float HdrMin = 0.0f;    // HDRMIN   floor for the HDR auto exposure, 0 = untouched
    float Fog = 1.0f;       // FOG      fog density
    float FogH = 0.0f;      // FOGH     fog height offset in cm
    float FogR = 1.0f;      // FOGR     fog tint, red
    float FogG = 1.0f;      // FOGG     fog tint, green
    float FogB = 1.0f;      // FOGB     fog tint, blue
    float Wind = 1.0f;      // WIND     vegetation wind strength
    float Wet = 0.0f;       // WET      minimum scene wetness (dew, wet ground after rain)
    float Rain = 1.0f;      // RAIN     rain intensity (drizzle < 1)
    float RainAmt = 1.0f;   // RAINAMT  rain particle count factor (heavy rain above 1)
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
    { "HDRMIN",   &GoucWeatherParams::HdrMin,   0.00f,   0.50f },
    { "FOG",      &GoucWeatherParams::Fog,      0.50f,   8.00f },
    { "FOGH",     &GoucWeatherParams::FogH,  -3000.0f, 8000.0f },
    { "FOGR",     &GoucWeatherParams::FogR,     0.80f,   1.20f },
    { "FOGG",     &GoucWeatherParams::FogG,     0.80f,   1.20f },
    { "FOGB",     &GoucWeatherParams::FogB,     0.80f,   1.20f },
    { "WIND",     &GoucWeatherParams::Wind,     0.50f,   3.00f },
    { "WET",      &GoucWeatherParams::Wet,      0.00f,   1.00f },
    { "RAIN",     &GoucWeatherParams::Rain,     0.20f,   1.00f },
    { "RAINAMT",  &GoucWeatherParams::RainAmt,  1.00f,   3.00f },
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

/** Fewest rain particles a client may end up with: the F11 menu and the INI can both set
 *  [Rain] NumParticles, and 0 would simply switch the rain off. Weather is server policy,
 *  so a floor is enforced instead (GothicGraphicsState default is 50000). */
inline constexpr UINT GOUC_WEATHER_MIN_RAIN_PARTICLES = 50000;

/** Rain particle factor for the rain effect. The particle buffers are rebuilt whenever the
 *  count changes, so the faded value is snapped to coarse steps: a 60 s fade would otherwise
 *  rebuild them every frame. 0.5 steps mean at most three rebuilds while the weather fades
 *  from clear to a storm; the drop textures are loaded once and are not part of that. */
inline float GoucWeatherRainParticleFactor() {
    const float amt = GoucWeather().Current.RainAmt;
    if ( amt <= 1.0f ) {
        return 1.0f;
    }
    const float step = 0.5f;
    float snapped = std::floor( amt / step ) * step;
    if ( snapped < 1.0f ) snapped = 1.0f;
    if ( snapped > 3.0f ) snapped = 3.0f;
    return snapped;
}

/** The engine's own outdoor fog density (GothicGraphicsState, new world). FOG is a factor on
 *  a value the PLAYER owns -- F11 and [Fog] GlobalDensity in UserSettings.ini -- and five
 *  times almost nothing is still almost nothing. A weather fog therefore gets a floor
 *  derived from this reference instead of from whatever the player left in the INI. */
inline constexpr float GOUC_WEATHER_FOG_REFERENCE_DENSITY = 0.00004f;

/** How far the distance ramp may be pulled in. The height fog is weighted by distance
 *  (HF_WeightZNear/ZFar, driven by the player's fog range), so a fog only ever thickened
 *  hundreds of metres out -- exactly where nobody is looking. Pulling the ramp in brings the
 *  soup close, and the cap keeps it from turning into a white wall in front of the nose.
 *  Costs nothing: same pass, same shader, only different constants. */
inline constexpr float GOUC_WEATHER_FOG_MAX_NEAR_PULL = 2.5f;

/** Fog density for a weather look, given the player's own setting. */
inline float GoucWeatherFogDensity( float playerDensity ) {
    const float fog = GoucWeatherCur().Fog;
    const float scaled = playerDensity * fog;
    if ( fog <= 1.0f ) {
        return scaled;
    }
    return std::max( scaled, GOUC_WEATHER_FOG_REFERENCE_DENSITY * fog );
}

/** Divisor for the distance ramp, 1.0 while no fog weather is running. */
inline float GoucWeatherFogNearPull() {
    const float fog = GoucWeatherCur().Fog;
    if ( fog <= 1.0f ) {
        return 1.0f;
    }
    return std::min( fog, GOUC_WEATHER_FOG_MAX_NEAR_PULL );
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

// ---------------------------------------------------------------------------------------------
// Lightning. A thunderstorm that only rains harder still reads as heavy rain, so the sky has to
// flash and the flash has to light the world. The server decides WHEN (all players see the same
// bolt), the trigger arrives the same way the weather does: a vob named "GOUC_LIGHTNING STR=1.6
// MS=320", which the script removes again right after. The vob is never drawn.
// ---------------------------------------------------------------------------------------------

inline constexpr float GOUC_LIGHTNING_MAX_STRENGTH = 3.0f;
inline constexpr float GOUC_LIGHTNING_MIN_MS = 60.0f;
inline constexpr float GOUC_LIGHTNING_MAX_MS = 1500.0f;
/** How much of the flash the sky gets on top of the scene light. The sky is what one looks at. */
inline constexpr float GOUC_LIGHTNING_SKY_FACTOR = 1.6f;

struct GoucLightningState {
    DWORD StartMs = 0;
    float Strength = 0.0f;   // 0 = nothing is flashing
    float DurationMs = 0.0f;
};

inline GoucLightningState& GoucLightning() {
    static GoucLightningState state;
    return state;
}

/** Brightness added by a running flash, 0 while idle.
 *  Two peaks and a fast decay: a real bolt flickers, a single ramp looks like a light switch. */
inline float GoucLightningBoost() {
    auto& s = GoucLightning();
    if ( s.Strength <= 0.0f || s.DurationMs <= 0.0f ) {
        return 0.0f;
    }

    const DWORD now = Toolbox::timeSinceStartMs();
    const float t = static_cast<float>(now - s.StartMs) / s.DurationMs;
    if ( t < 0.0f || t >= 1.0f ) {
        s.Strength = 0.0f;
        return 0.0f;
    }

    // 0.00-0.08 rise, 0.08-0.18 first peak, 0.18-0.30 dip, 0.30-0.45 second peak, then decay
    float shape;
    if ( t < 0.08f ) {
        shape = t / 0.08f;
    } else if ( t < 0.18f ) {
        shape = 1.0f;
    } else if ( t < 0.30f ) {
        shape = 0.35f;
    } else if ( t < 0.45f ) {
        shape = 0.85f;
    } else {
        const float d = (t - 0.45f) / 0.55f;
        shape = 0.85f * (1.0f - d) * (1.0f - d);
    }
    return s.Strength * shape;
}

/** Reads a trigger vob name. Returns false for anything that is not a lightning vob. */
inline bool GoucLightningParseName( const std::string& rawName, float& strength, float& durationMs ) {
    std::string name = rawName;
    std::transform( name.begin(), name.end(), name.begin(),
        []( unsigned char c ) { return static_cast<char>(std::toupper( c )); } );

    static const char* const prefix = "GOUC_LIGHTNING";
    if ( name.rfind( prefix, 0 ) != 0 ) {
        return false;
    }

    strength = 1.5f;
    durationMs = 320.0f;

    size_t pos = 0;
    while ( pos < name.size() ) {
        size_t end = name.find_first_of( " ;", pos );
        if ( end == std::string::npos ) {
            end = name.size();
        }
        const std::string token = name.substr( pos, end - pos );
        pos = end + 1;

        const size_t eq = token.find( '=' );
        if ( eq == std::string::npos || eq + 1 >= token.size() ) {
            continue;
        }
        const std::string key = token.substr( 0, eq );
        const std::string raw = token.substr( eq + 1 );
        float value = 0.0f;
        try {
            value = std::stof( raw );
        } catch ( ... ) {
            continue;
        }
        if ( key == "STR" ) {
            strength = std::clamp( value, 0.0f, GOUC_LIGHTNING_MAX_STRENGTH );
        } else if ( key == "MS" ) {
            durationMs = std::clamp( value, GOUC_LIGHTNING_MIN_MS, GOUC_LIGHTNING_MAX_MS );
        }
    }
    return true;
}

inline void GoucLightningStrike( float strength, float durationMs ) {
    auto& s = GoucLightning();
    s.Strength = std::clamp( strength, 0.0f, GOUC_LIGHTNING_MAX_STRENGTH );
    s.DurationMs = std::clamp( durationMs, GOUC_LIGHTNING_MIN_MS, GOUC_LIGHTNING_MAX_MS );
    s.StartMs = Toolbox::timeSinceStartMs();
}

// ---------------------------------------------------------------------------------------------
// HDR. The tone mapping divides by the average scene luminance -- an auto exposure. In a dark
// scene the divisor goes towards zero and the image is lifted without limit: night becomes day,
// and the dimming the weather sets is undone. Players asked for HDR back, so instead of forcing
// the switch off the exposure gets a floor: HDR stays, the night stays dark.
// ---------------------------------------------------------------------------------------------

/** Floor that applies even without a weather control vob. A dark scene may still be lifted,
 *  just not arbitrarily. The server can raise it through the HDRMIN token (GoucWeatherCur). */
inline constexpr float GOUC_HDR_MIN_LUM_FLOOR = 0.05f;

/** Exposure target. The default is 0.8; a higher value in the INI would brighten everything,
 *  so it is capped rather than forced -- less is allowed, more is not. */
inline constexpr float GOUC_HDR_MAX_MIDDLEGRAY = 0.80f;

/** Floor for the auto exposure, handed to the HDR shaders (hdr.h). */
inline float GoucHdrMinLum() {
    return std::max( GOUC_HDR_MIN_LUM_FLOOR, GoucWeatherCur().HdrMin );
}

/** The colour grading pass only runs when it would change the image. */
inline bool GoucWeatherGradeActive() {
    const auto& c = GoucWeatherCur();
    return std::abs( c.Temp ) > 0.001f || std::abs( c.Sat - 1.0f ) > 0.001f || std::abs( c.Contrast - 1.0f ) > 0.001f;
}
