// VOLUMETRIC CLOUDS: rt_clouds_volumetric.
//
// WHAT IT IS. A horizontal slab of cloud above the sky viewer, ray-marched by
// RTGL1 (Shaders/CmCloudMap.comp) once per frame into a 1024x256 map of WORLD
// DIRECTIONS, then composited by the two sky fragment shaders -- the one that
// draws the sky at render resolution into the albedo (what the player sees)
// and the one that draws it into the 256^2 cubemap every ray that misses the
// level samples (GI). One march, both consumers, and the level is lit by the
// clouds with nothing to keep in sync. That is the property the shell deck has
// too, and it is the reason both are sky geometry.
//
// WHY A DIRECTION MAP AND NOT A SCREEN BUFFER. Two reasons, and the second
// matters more than the first.
//
//   * Cost. The sky is rasterised TWICE; a march inside the sky fragment
//     shader would run per screen pixel (4M x 32 x 7 at 4K) and again for the
//     cubemap. The map is 262k texels whatever the screen is.
//   * Temporal history. A cloud needs a few frames of accumulation to hide
//     its step jitter. In a screen buffer that history has to be reprojected
//     and validated, and validation is exactly what let sprites punch holes
//     in the froxel fog's history (docs/rt-volumetric-weapon-trails.md: every
//     billboard silhouette rejected the medium's history under camera motion,
//     and each rejected band restarted from one noisy sample -- "any sprite
//     smears the shafts"). In a direction map the same texel IS the same
//     direction next frame: no motion vectors, no depth, no normals, nothing
//     a sprite in front of the sky can touch. The blend is a plain lerp.
//
// WHY THIS FILE HAS A COPY OF THE DENSITY FIELD. Two engine-side systems need
// to know what the clouds are doing and cannot wait for a GPU readback:
//
//   * The moon. The deck walks the moon's ray through its shells and dims
//     rt_sun_intensity by the product (hw_skyportal.cpp, RT_DrawCloudDeck);
//     without that the moonlight pours through an overcast sky at full
//     strength and the clouds are wallpaper. The same march, on the CPU, along
//     the one ray that matters.
//   * Strike placement. g_cloudCoverAz is read by RT_OnLightningFlash so a
//     bolt lands in cloud and not in a gap. 36 bearings, 8 steps each.
//
// The CPU field is the shader's field, function for function: same hash, same
// value noise, same octave ladder, same profile and erosion. It does not have
// to agree to the bit -- it is answering "how opaque is the sky that way" --
// but it has to be the same SHAPE, or the moon would dim behind cloud that is
// not drawn. If CmCloudMap.comp changes, this changes with it.
//
// OFF IS THE DECK, AND ON IS WHAT SHIPS. rt_clouds_volumetric 0 leaves
// RT_DrawCloudDeck exactly as it was and the only thing this file does then is
// report enabled=0 -- but since 2026-08-25 the cvar defaults to 1 and
// tools/d64rt-pins.cfg restates that, so the march is the cloud path on every
// map whose preset turns rt_clouds on, not just on the five fire maps. The deck
// is the fallback and stays one console line away.
//
// It had to become BOTH a default and a pin. This is the one cloud cvar the
// engine writes at runtime (rt_firesky.cpp, on the fire maps) and it is
// CVAR_ARCHIVE, so while it was unpinned a quit on MAP23 archived a 1 that
// nothing reset -- and a player who had never been to a hell map got the deck
// on MAP12 while a player who had got the march. Same build, same launcher.

#include "rt_internal.h"

#include "printf.h"

#include <cmath>

using namespace rtx;

namespace
{

// Playsim time of the last sky draw, for the wind. The sky portal is drawn
// before RT_DrawFrame builds the params, so this is one frame fresh.
double g_vcloudTime = 0.0;
bool   g_vcloudSky  = false;

// Frames the mode has been continuously on. The first two send historyBlend 0
// so the map never blends with whatever it held before the switch.
int g_vcloudOnFrames = 0;

// ---------------------------------------------------------------------------
// The density field. Mirrors CmCloudMap.comp; see the header comment.
// ---------------------------------------------------------------------------

float frac( float x )
{
    return x - std::floor( x );
}

float cloudHash( float px, float py, float pz )
{
    px = frac( px * 0.3183099f + 0.1f ) * 17.f;
    py = frac( py * 0.3183099f + 0.2f ) * 17.f;
    pz = frac( pz * 0.3183099f + 0.3f ) * 17.f;
    return frac( px * py * pz * ( px + py + pz ) );
}

float cloudValueNoise( float x, float y, float z )
{
    const float ix = std::floor( x ), iy = std::floor( y ), iz = std::floor( z );
    float       fx = x - ix, fy = y - iy, fz = z - iz;
    fx = fx * fx * ( 3.f - 2.f * fx );
    fy = fy * fy * ( 3.f - 2.f * fy );
    fz = fz * fz * ( 3.f - 2.f * fz );

    auto mix = []( float a, float b, float t ) { return a + ( b - a ) * t; };

    const float c000 = cloudHash( ix, iy, iz ), c100 = cloudHash( ix + 1, iy, iz );
    const float c010 = cloudHash( ix, iy + 1, iz ), c110 = cloudHash( ix + 1, iy + 1, iz );
    const float c001 = cloudHash( ix, iy, iz + 1 ), c101 = cloudHash( ix + 1, iy, iz + 1 );
    const float c011 = cloudHash( ix, iy + 1, iz + 1 ), c111 = cloudHash( ix + 1, iy + 1, iz + 1 );

    return mix( mix( mix( c000, c100, fx ), mix( c010, c110, fx ), fy ),
                mix( mix( c001, c101, fx ), mix( c011, c111, fx ), fy ),
                fz );
}

float cloudFbm( float x, float y, float z, int octaves )
{
    float a = 0.5f, s = 0.f, n = 0.f;
    for( int i = 0; i < octaves; i++ )
    {
        s += a * cloudValueNoise( x, y, z );
        n += a;
        x = x * 2.03f + 13.1f;
        y = y * 2.03f + 7.7f;
        z = z * 2.03f + 3.3f;
        a *= 0.5f;
    }
    return s / n;
}

struct Slab
{
    // RTGL world space: Z is up (the sun's direction in RT_DrawFrame is built
    // with cos(theta) in Z). The basis is the shader's cloudBasis() for
    // up = (0,0,1): t1 = (-1,0,0), t2 = (0,-1,0).
    float bottom, thickness, coverage, density, invFeature, detail;
    float windU, windV;
    int   layers;
    float gapFrac;
};

float smoothstep( float a, float b, float x )
{
    const float t = std::clamp( ( x - a ) / ( b - a ), 0.f, 1.f );
    return t * t * ( 3.f - 2.f * t );
}

float densityAt( const Slab& s, float px, float py, float pz, int octaves )
{
    const float h = ( pz - s.bottom ) / s.thickness;

    // The shader's cloudDeckOf(): one slab, or two decks around a gap.
    float hl   = h;
    int   deck = -1;
    if( s.layers < 2 )
    {
        deck = ( h > 0.f && h < 1.f ) ? 0 : -1;
    }
    else
    {
        const float a1 = 0.5f - 0.5f * s.gapFrac;
        const float b0 = 0.5f + 0.5f * s.gapFrac;
        if( h > 0.f && h < a1 )
        {
            hl   = h / a1;
            deck = 0;
        }
        else if( h > b0 && h < 1.f )
        {
            hl   = ( h - b0 ) / ( 1.f - b0 );
            deck = 1;
        }
    }
    if( deck < 0 )
    {
        return 0.f;
    }
    const float profile = smoothstep( 0.f, 0.12f, hl ) * ( 1.f - smoothstep( 0.55f, 1.f, hl ) );

    // xz = (dot(p,t1), dot(p,t2)) + windOffset, q = (xz.x, dot(p,up), xz.y) * invFeature
    float u = -px + s.windU;
    float v = -py + s.windV;
    if( deck == 1 )
    {
        // The upper deck's own drift and offset, as in the shader.
        u += s.windU * 0.35f + 1700.f;
        v += s.windV * 0.35f + 900.f;
    }
    const float qx = u * s.invFeature, qy = pz * s.invFeature, qz = v * s.invFeature;

    const float base  = cloudFbm( qx, qy, qz, octaves );
    float       shape = std::clamp( ( base - ( 1.f - s.coverage ) ) / std::max( s.coverage, 1e-3f ), 0.f, 1.f );
    shape *= profile;
    if( shape <= 0.f )
    {
        return 0.f;
    }
    if( s.detail > 0.f && octaves >= 3 )
    {
        const float erode = s.detail * 0.45f * ( 1.f - shape );
        shape = std::clamp( ( shape - erode ) / std::max( 1.f - erode, 1e-3f ), 0.f, 1.f );
    }
    return shape;
}

// Transmittance from the viewer (origin) along a unit direction, `steps` samples.
float transmittanceAlong( const Slab& s, float dx, float dy, float dz, int steps, float horizonDeg )
{
    if( dz <= 1e-3f )
    {
        return 1.f;
    }
    const float altDeg = float( std::asin( std::clamp( dz, -1.f, 1.f ) ) * 180.0 / M_PI );
    const float fade   = smoothstep( 0.f, std::max( horizonDeg, 0.01f ), altDeg );
    if( fade <= 0.f )
    {
        return 1.f;
    }

    const float t0 = s.bottom / dz;
    float       t1 = ( s.bottom + s.thickness ) / dz;
    t1             = std::min( t1, t0 + s.thickness * 12.f );
    const float dt = ( t1 - t0 ) / float( steps );

    float tau = 0.f;
    for( int i = 0; i < steps; i++ )
    {
        const float t = t0 + ( float( i ) + 0.5f ) * dt;
        tau += densityAt( s, dx * t, dy * t, dz * t, 3 ) * s.density * dt;
    }
    const float T = std::exp( -tau );
    return 1.f - fade * ( 1.f - T );
}

Slab currentSlab()
{
    Slab s;
    s.bottom     = std::max( 1.f, float{ cvar::rt_vclouds_altitude } );
    s.thickness  = std::max( 1.f, float{ cvar::rt_vclouds_thick } );
    s.coverage   = std::clamp( float{ cvar::rt_vclouds_coverage }, 0.f, 1.f );
    s.density    = std::max( 0.f, float{ cvar::rt_vclouds_density } );
    s.invFeature = 1.f / std::max( 1.f, float{ cvar::rt_vclouds_feature } );
    s.detail     = std::clamp( float{ cvar::rt_vclouds_detail }, 0.f, 1.f );

    const double wdir = double( cvar::rt_clouds_wind_dir ) * M_PI / 180.0;
    const float  wspd = float{ cvar::rt_vclouds_wind };
    // The shader adds wind*time to (dot(p,t1), dot(p,t2)); the wind vector
    // itself is expressed in RTGL world xy and projected the same way.
    const float wx = float( std::cos( wdir ) ) * wspd;
    const float wy = float( std::sin( wdir ) ) * wspd;
    s.windU        = ( -wx ) * float( g_vcloudTime );
    s.windV        = ( -wy ) * float( g_vcloudTime );
    s.layers       = ( RT_FireSkyMap() || RT_FireSkyActive() )
                         ? std::clamp( int{ cvar::rt_vclouds_layers }, 1, 2 )
                         : 1;
    s.gapFrac      = std::clamp( float{ cvar::rt_vclouds_gap }, 0.02f, 0.8f );
    return s;
}

} // namespace

bool RT_VCloudsActive()
{
    return bool{ cvar::rt_clouds } && bool{ cvar::rt_clouds_volumetric };
}

void RT_VCloudsFrame( double now, bool haveSky )
{
    g_vcloudTime = now;
    g_vcloudSky  = haveSky;

    if( !RT_VCloudsActive() || !haveSky )
    {
        RT_SetCloudSunTransmittance( 1.f, 1.f, 1.f );
        for( int i = 0; i < RT_CLOUD_AZ_BINS; i++ )
        {
            g_cloudCoverAz[ i ] = 0.f;
        }
        return;
    }

    const Slab  s       = currentSlab();
    const float horizon = float{ cvar::rt_vclouds_horizon };

    // THE MOON. Same convention as the directional light in RT_DrawFrame:
    // toward the moon is (sin t cos b, sin t sin b, cos t), t = 90 - altitude.
    //
    // Only when the GPU is NOT doing it per ray (rt_vclouds_sunshadow): the
    // two would otherwise stack, and a covered moon would dim twice.
    if( bool{ cvar::rt_vclouds_sunshadow } )
    {
        RT_SetCloudSunTransmittance( 1.f, 1.f, 1.f );
    }
    else
    {
        const double alt = std::clamp( double( cvar::rt_sun_a ), -89.9, 89.9 );
        const double azi = double( cvar::rt_sun_b ) * M_PI / 180.0;
        const double t   = ( 90.0 - alt ) * M_PI / 180.0;
        const float  dx = float( std::sin( t ) * std::cos( azi ) );
        const float  dy = float( std::sin( t ) * std::sin( azi ) );
        const float  dz = float( std::cos( t ) );

        const float T = transmittanceAlong( s, dx, dy, dz, 12, horizon );

        // The deck's contract, kept: the clear fraction passes everything,
        // the covered fraction passes rt_clouds_transmit, coloured by the tint.
        const uint32_t tintc   = *( cvar::rt_clouds_tint );
        const float    tint[ 3 ] = { RPART( tintc ) / 255.f, GPART( tintc ) / 255.f, BPART( tintc ) / 255.f };
        const float    pass    = std::clamp( float{ cvar::rt_clouds_transmit }, 0.f, 1.f );
        const float    covered = 1.f - T;
        const float    k       = std::clamp( float{ cvar::rt_clouds_occlude }, 0.f, 1.f );

        float tr[ 3 ];
        for( int c = 0; c < 3; c++ )
        {
            const float full = T + covered * tint[ c ] * pass;
            tr[ c ]          = 1.f - k * ( 1.f - full );
        }
        RT_SetCloudSunTransmittance( tr[ 0 ], tr[ 1 ], tr[ 2 ] );
    }

    // STRIKE COVERAGE, per bearing, probed at the middle of the strike band.
    {
        const double probeAlt = std::clamp(
            0.5 * ( double( cvar::rt_lightning_alt_min ) + double( cvar::rt_lightning_alt_max ) ),
            1.0,
            89.0 );
        const double pt = ( 90.0 - probeAlt ) * M_PI / 180.0;
        for( int i = 0; i < RT_CLOUD_AZ_BINS; i++ )
        {
            const double a  = ( double( i ) + 0.5 ) * ( 360.0 / RT_CLOUD_AZ_BINS ) * M_PI / 180.0;
            const float  dx = float( std::sin( pt ) * std::cos( a ) );
            const float  dy = float( std::sin( pt ) * std::sin( a ) );
            const float  dz = float( std::cos( pt ) );
            g_cloudCoverAz[ i ] = 1.f - transmittanceAlong( s, dx, dy, dz, 8, horizon );
        }
    }
}

float RT_VCloudsCoverAt( float azDeg, float altDeg )
{
    if( !RT_VCloudsActive() || !g_vcloudSky )
    {
        return 0.f;
    }
    const Slab   s   = currentSlab();
    const double azi = double( azDeg ) * M_PI / 180.0;
    const double t   = ( 90.0 - std::clamp( double( altDeg ), -89.0, 89.0 ) ) * M_PI / 180.0;
    const float  dx  = float( std::sin( t ) * std::cos( azi ) );
    const float  dy  = float( std::sin( t ) * std::sin( azi ) );
    const float  dz  = float( std::cos( t ) );
    return 1.f - transmittanceAlong( s, dx, dy, dz, 12, float{ cvar::rt_vclouds_horizon } );
}

void RT_VCloudsParams( RgDrawFrameVolumetricCloudParams* out )
{
    const bool on = RT_VCloudsActive() && g_vcloudSky;

    g_vcloudOnFrames = on ? g_vcloudOnFrames + 1 : 0;

    // WHICH CLOUD RENDERER ACTUALLY RAN, on the edge only.
    //
    // There was no such line off the fire maps: rt_firesky.cpp announces itself
    // on MAP22/23/24/28/32 and nowhere else, so "the volumetric clouds are not
    // enabled for him" needed a VIDEO to notice instead of one grep of
    // rt-console.log. That report was a stale gzdoom-rt2.ini deciding the cloud
    // path, and it was invisible from both sides.
    //
    // It is edge-triggered HERE, where the params are built, and not at level
    // load, because this is the last word. RT_ApplyCloudPreset and
    // RT_FireSkyOnLevelLoad both write rt_clouds after one another on the same
    // load -- rt_firesky.cpp's note above RT_FireSkyAnnounce records what that
    // ordering cost -- so a print from either can be true when it runs and
    // false a moment later. And like that one it reads the cvars BACK rather
    // than reporting what anything just set.
    {
        static int s_lastPath = -1;

        // 2 vs 3 separates the two ways the march can be armed but not drawing:
        // g_vcloudSky latches on the first sky portal of the session, so a
        // sealed opening map legitimately reports "waiting".
        const int path = !bool{ cvar::rt_clouds }             ? 0
                         : on                                 ? 3
                         : bool{ cvar::rt_clouds_volumetric } ? 2
                                                              : 1;
        if( path != s_lastPath )
        {
            s_lastPath                = path;
            const char* const kPath[] = { "OFF",
                                          "shell deck",
                                          "VOLUMETRIC (no sky portal drawn yet)",
                                          "VOLUMETRIC" };
            Printf( RT_DiagPrintLevel(),
                    "rt_clouds: %s -> %s  (rt_clouds %d, rt_clouds_volumetric %d)\n",
                    RT_GetMapName() ? RT_GetMapName() : "(no map)",
                    kPath[ path ],
                    int( bool{ cvar::rt_clouds } ),
                    int( bool{ cvar::rt_clouds_volumetric } ) );
        }
    }

    auto col255 = []( uint32_t hex ) {
        return RgFloat3D{ RPART( hex ) / 255.f, GPART( hex ) / 255.f, BPART( hex ) / 255.f };
    };

    const uint32_t tintc = *( cvar::rt_clouds_tint );
    const RgFloat3D tint = col255( tintc );

    // ABSOLUTE UNITS. Whatever the sky shader writes is multiplied by
    // skyColorMultiplier (rt_sky) downstream, and rt_sky is a per-map value
    // tuned for each map's PAINTED sky -- 1 on most, 175 and 400 on the fire
    // maps whose flipbook carries almost no light. The clouds are not a
    // painting: their light terms are divided by it here so "1.0" means the
    // same brightness on every map, and the fire-map presets cannot turn an
    // under-light of 1 into 400x white (which is exactly what the first MAP23
    // capture was).
    const float skyNorm = 1.f / std::max( 0.01f, float{ cvar::rt_sky } );

    // The moon's direction, toward it. Same formula as RT_VCloudsFrame.
    const double alt = std::clamp( double( cvar::rt_sun_a ), -89.9, 89.9 );
    const double azi = double( cvar::rt_sun_b ) * M_PI / 180.0;
    const double t   = ( 90.0 - alt ) * M_PI / 180.0;
    const RgFloat3D lightDir{ float( std::sin( t ) * std::cos( azi ) ),
                              float( std::sin( t ) * std::sin( azi ) ),
                              float( std::cos( t ) ) };

    // Moonlight on the cloud: rt_sun_color at rt_vclouds_light. Gated on the
    // moon existing at all, the same way the moon disc is.
    const bool moonOn = bool{ cvar::rt_sun } && float{ cvar::rt_sun_intensity } > 0.f;
    RgFloat3D  lightColor{ 0, 0, 0 };
    if( moonOn )
    {
        const RgFloat3D sc = col255( *( cvar::rt_sun_color ) );
        const float     g  = std::max( 0.f, float{ cvar::rt_vclouds_light } ) * skyNorm;
        lightColor         = { sc.data[ 0 ] * g, sc.data[ 1 ] * g, sc.data[ 2 ] * g };
    }

    // THE FIRE TERMS EXIST ONLY ON THE FIRE MAPS. rt_vclouds_fire / _back /
    // _cascade / _under are ordinary cvars, and an arm that sets them for
    // MAP23 would otherwise set a burning sky over MAP12's storm too (reported
    // 2026-08-23: "why is fire sky enabled on map 12"). The five maps whose
    // WAD sky is the fire flipbook are the only ones that get them.
    const bool  fireMap  = RT_FireSkyMap() || RT_FireSkyActive();
    const float fireGate = fireMap ? 1.f : 0.f;

    // Fire from below: the engine's own knob plus the deck's rt_clouds_fireback,
    // which the fire sky mode already sets on its maps.
    const float under = ( std::max( 0.f, float{ cvar::rt_vclouds_under } ) +
                          std::max( 0.f, float{ cvar::rt_clouds_fireback } ) ) *
                        skyNorm * fireGate;
    const RgFloat3D underColor = col255( *( cvar::rt_clouds_fireback_color ) );

    // Ambient: a flat floor tinted by the cloud colour, plus the strike's
    // flash in the strike's colour -- the cloud lit from inside.
    const float amb   = std::max( 0.f, float{ cvar::rt_vclouds_ambient } ) * skyNorm;
    const float flash = std::clamp( RT_LightningFlashLevel() * float{ cvar::rt_vclouds_flash }, 0.f, 4.f ) *
                        skyNorm;
    const RgFloat3D fc = col255( *( cvar::rt_lightning_color ) );
    const RgFloat3D ambient{ amb * tint.data[ 0 ] + flash * fc.data[ 0 ],
                             amb * tint.data[ 1 ] + flash * fc.data[ 1 ],
                             amb * tint.data[ 2 ] + flash * fc.data[ 2 ] };

    const double wdir = double( cvar::rt_clouds_wind_dir ) * M_PI / 180.0;
    const float  wspd = float{ cvar::rt_vclouds_wind };

    *out = RgDrawFrameVolumetricCloudParams{
        .sType         = RG_STRUCTURE_TYPE_DRAW_FRAME_VOLUMETRIC_CLOUD_PARAMS,
        .pNext         = nullptr,
        .enabled       = on,
        .altitude      = std::max( 1.f, float{ cvar::rt_vclouds_altitude } ),
        .thickness     = std::max( 1.f, float{ cvar::rt_vclouds_thick } ),
        .coverage      = std::clamp( float{ cvar::rt_vclouds_coverage }, 0.f, 1.f ),
        .density       = std::max( 0.f, float{ cvar::rt_vclouds_density } ),
        .featureSize   = std::max( 1.f, float{ cvar::rt_vclouds_feature } ),
        .detail        = std::clamp( float{ cvar::rt_vclouds_detail }, 0.f, 1.f ),
        // In the shader's (t1, t2) basis, which for Z-up is (-x, -y): the
        // shader adds wind*time straight to its t1/t2 coordinates, and
        // currentSlab() above does the same projection.
        .wind          = { -float( std::cos( wdir ) ) * wspd, -float( std::sin( wdir ) ) * wspd },
        .time          = float( g_vcloudTime ),
        .steps         = uint32_t( std::clamp( int{ cvar::rt_vclouds_steps }, 4, 128 ) ),
        .lightSteps    = uint32_t( std::clamp( int{ cvar::rt_vclouds_lightsteps }, 1, 16 ) ),
        .tint          = tint,
        .lightDir      = lightDir,
        .lightColor    = lightColor,
        .underColor    = underColor,
        .underStrength = under,
        .ambient       = ambient,
        .asymmetry     = std::clamp( float{ cvar::rt_vclouds_asym }, -0.95f, 0.95f ),
        // rt_clouds_transmit is about the LIGHT (applied to the moon above),
        // not the picture: a dense cloud hides the stars behind it.
        .transmitFloor = 0.f,
        .horizonFade   = std::max( 0.01f, float{ cvar::rt_vclouds_horizon } ),
        // No history for the first two frames after switching on, so the map
        // starts from this frame rather than from whatever it last held.
        .historyBlend  = g_vcloudOnFrames > 2
                             ? std::clamp( float{ cvar::rt_vclouds_history }, 0.f, 0.97f )
                             : 0.f,
        .debugMode     = uint32_t( std::clamp( int{ cvar::rt_vclouds_debug }, 0, 3 ) ),
        // Per-ray occlusion of the directional light -- never of a strike,
        // which is inside the deck. When off, RT_VCloudsFrame dims the moon
        // globally instead.
        .sunOcclusion  = bool{ cvar::rt_vclouds_sunshadow } && !g_rtSunIsLightning,
        .lightTransmit = std::clamp( float{ cvar::rt_clouds_transmit }, 0.f, 1.f ),
        .lightOcclude  = std::clamp( float{ cvar::rt_clouds_occlude }, 0.f, 1.f ),
        // The fire sky: glow behind, pockets within. Same colour as the
        // under-light, both absolute.
        .backColor     = underColor,
        .backStrength  = std::max( 0.f, float{ cvar::rt_vclouds_back } ) * skyNorm * fireGate,
        .fireStrength  = std::max( 0.f, float{ cvar::rt_vclouds_fire } ) * skyNorm * fireGate,
        .fireScale     = std::max( 1.f, float{ cvar::rt_vclouds_fire_scale } ),
        .fireCover     = std::clamp( float{ cvar::rt_vclouds_fire_cover }, 0.f, 1.f ),
        .fireLit       = std::max( 0.f, float{ cvar::rt_vclouds_fire_lit } ),
        // Two decks are the fire sky's arrangement (the sheet between them);
        // off the fire maps it is one slab whatever the cvar says.
        .layers        = fireMap ? uint32_t( std::clamp( int{ cvar::rt_vclouds_layers }, 1, 2 ) ) : 1u,
        .gapFraction   = std::clamp( float{ cvar::rt_vclouds_gap }, 0.02f, 0.8f ),
        .sheetExtinction = std::max( 0.f, float{ cvar::rt_vclouds_sheet_ext } ),
        .firePulse       = std::clamp( float{ cvar::rt_vclouds_fire_pulse }, 0.f, 1.f ),
        .firePulseSpeed  = std::max( 0.f, float{ cvar::rt_vclouds_fire_pulse_speed } ),
        .fireFlicker     = std::clamp( float{ cvar::rt_vclouds_fire_flicker }, 0.f, 1.f ),
        .cascadeStrength = std::max( 0.f, float{ cvar::rt_vclouds_cascade } ) * skyNorm * fireGate,
        .cascadeLength   = std::max( 1.f, float{ cvar::rt_vclouds_cascade_len } ),
        .cascadeCover    = std::clamp( float{ cvar::rt_vclouds_cascade_cover }, 0.f, 1.f ),
        .cascadeSpeed    = float{ cvar::rt_vclouds_cascade_speed },
        .cascadeWidth    = std::max( 1.f, float{ cvar::rt_vclouds_cascade_width } ),
    };
}
