// The storm, and how much of the moon the cloud deck lets through.
//
// A strike is turned into a bearing, a bolt drawn on the sky dome at that
// bearing (RT_DrawSkyQuad, hw_skyportal.cpp) and an analytic directional light
// down the same bearing -- scenery and source, so they agree by construction.
// Driven from DLightningThinker via RT_OnLightningFlash.
//
// Split out of rt_main.cpp. Behaviour unchanged; this is a move.

#include "rt_internal.h"

// The shared internals (RG_CHECK, ONEGAMEUNIT_IN_METERS, RT_SectorHue, the
// light-ID bases) come in unqualified, exactly as when this code lived inside
// rt_main.cpp's anonymous namespace.
using namespace rtx;

//-----------------------------------------------------------------------------
//
// Doom64-RT: the storm.
//
// WHERE A STRIKE COMES FROM. Not from here, and not from any script in the map.
// MAP11's MAPINFO carries the Hexen `lightning` keyword, which spawns
// DLightningThinker (playsim/mapthinkers/a_lightning.cpp). That thinker
// self-schedules -- 5-20s to the first strike, then 16-31 TICS for a quick
// double-flash (~20% of the time) or 2-9s / 5-20s otherwise -- and on each
// strike it raises every F_SKY1 sector's lightlevel to 200+(rand&31), plays
// `world/thunder` (a $random alias over DSTHNDR1/DSTHNDR2), and runs any
// SCRIPT_Lightning. MAP11's script 671 LIGHTNING is one of those, and it
// brightens the two sectors of a skybox room that RT does not render.
//
// So the timing, the sound and the sector flash are all stock and all correct.
// What is missing under RT is everything you would actually SEE: the map's
// clouds live in that ignored skybox room, and a flat lightlevel bump casts no
// shadows and comes from no direction. RT_OnLightningFlash is the thinker's one
// call into the renderer, and it turns each strike into three things that agree
// with each other by construction:
//
//   1. a bearing and altitude, picked once per strike;
//   2. a bolt drawn on the sky dome at that bearing (RT_DrawSkyQuad,
//      hw_skyportal.cpp) -- scenery;
//   3. an analytic directional light down the same bearing -- the source.
//
// (2) and (3) are the moon's arrangement repeated, and for the same reason: the
// RT sky is a rasterised cubemap sampled on ray miss, not importance-sampled,
// so painting something bright into it does not light anything at 1 spp. See
// rt_moon_geo.
//
// STROKE STRUCTURE. A strike is 1-3 sub-strokes at 0-140ms, each an instant
// attack and an exponential decay. They combine with MAX, not sum: summing
// makes a three-stroke strike three times as bright as a one-stroke strike,
// which turns rt_lightning_intensity into a knob whose meaning depends on
// rt_lightning_strokes. With max, strokes change the RHYTHM only.
//
// WALL CLOCK, not tics. The whole envelope is ~0.5s and it is a pure visual, so
// it is driven by RT_GetCurrentTime rather than by playsim time. That also
// means it decays away correctly while the game is paused instead of freezing
// mid-flash on the menu.
//
//-----------------------------------------------------------------------------
namespace
{
struct LightningStroke
{
    float at;  // seconds after the strike
    float amp; // 0..1 peak
};

struct LightningState
{
    bool            active   = false;
    double          t0       = 0.0;
    float           azimuth  = 0.f;
    float           altitude = 35.f;
    int             variant  = 0; // 0..3 -> BOLT1..BOLT4
    int             nstrokes = 0;
    LightningStroke strokes[ 6 ] = {};
};

LightningState g_lightning;

std::mt19937& RT_LightningRng()
{
    static std::mt19937 rng{ 0xB01Fu };
    return rng;
}

float RT_LightningRand( float lo, float hi )
{
    return std::uniform_real_distribution< float >{ lo, hi }( RT_LightningRng() );
}
} // namespace

// How much of the moon gets through the cloud deck -- PER CHANNEL, so cloud
// colour reaches the light and not just the picture. Written once per frame by
// RT_DrawCloudDeck (hw_skyportal.cpp), which owns the deck geometry and so is
// the only place that can answer it without a second copy of that maths. Reset
// to white whenever the deck is off or absent, so a stale value cannot darken
// or tint a map with no clouds in it.
float g_cloudSunTransmittance[ 3 ] = { 1.f, 1.f, 1.f };


// Drop a strike that is still in flight. Called on level load: a flash carrying
// into the next map would keep lighting a level that has no storm at all unless
// its own MAPINFO says so. A function rather than a direct write to g_lightning
// because that state stays private to this file.
void RT_StopLightning()
{
    g_lightning.active = false;
}

void RT_SetCloudSunTransmittance( float r, float g, float b )
{
    // A hard floor well under rt_clouds_transmit, which is the knob that is
    // actually meant to bound this. Purely a guard against a cvar combination
    // that would black out a map lit only by the moon.
    //
    // Floored on LUMINANCE and applied as a scale, not clamped per channel. A
    // per-channel clamp destroys the hue exactly when it binds: three channels
    // all under the floor come out (0.02, 0.02, 0.02), which is grey -- so the
    // deck's colour vanished from the light in precisely the case, a thick
    // overcast, where it should have been strongest. Scaling all three together
    // lifts the brightness to the floor and keeps the ratio between them, so a
    // heavy purple deck still delivers purple, just dim.
    constexpr float FLOOR = 0.02f;

    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if( lum < FLOOR )
    {
        const float scale = FLOOR / std::max( lum, 1e-9f );
        r *= scale;
        g *= scale;
        b *= scale;
    }

    g_cloudSunTransmittance[ 0 ] = std::clamp( r, 0.f, 1.f );
    g_cloudSunTransmittance[ 1 ] = std::clamp( g, 0.f, 1.f );
    g_cloudSunTransmittance[ 2 ] = std::clamp( b, 0.f, 1.f );
}

// 0..1 flash strength right now. Cheap and side-effect-light, so both the
// renderer (hw_skyportal.cpp) and the light upload can just call it rather than
// sharing a per-frame cached value; they are microseconds apart.
float RT_LightningFlashLevel()
{
    if( !g_lightning.active || !bool{ cvar::rt_lightning } )
    {
        return 0.f;
    }

    const float tau = std::max( 0.02f, float{ cvar::rt_lightning_decay } );
    const float t   = float( RT_GetCurrentTime() - g_lightning.t0 );

    if( t < 0.f )
    {
        return 0.f;
    }
    // Four time constants past the last stroke is down to ~2% -- below anything
    // the tonemapper can show, and the point where holding a light on stops
    // being worth an upload.
    if( t > g_lightning.strokes[ g_lightning.nstrokes - 1 ].at + 4.f * tau )
    {
        g_lightning.active = false;
        return 0.f;
    }

    float e = 0.f;
    for( int i = 0; i < g_lightning.nstrokes; i++ )
    {
        const float dt = t - g_lightning.strokes[ i ].at;
        if( dt >= 0.f )
        {
            e = std::max( e, g_lightning.strokes[ i ].amp * std::exp( -dt / tau ) );
        }
    }
    return std::clamp( e, 0.f, 1.f );
}

// Where the current strike is, for whoever wants to draw or light along it.
// Returns false when nothing is happening.
bool RT_LightningAim( float* azimuth, float* altitude, int* variant )
{
    if( !g_lightning.active || !bool{ cvar::rt_lightning } )
    {
        return false;
    }
    if( azimuth )  *azimuth  = g_lightning.azimuth;
    if( altitude ) *altitude = g_lightning.altitude;
    if( variant )  *variant  = g_lightning.variant;
    return true;
}

// Does the stock thinker still get to flash sector lightlevels? See
// rt_lightning_sectorflash for why that is a question worth asking under RT.
bool RT_LightningWantsSectorFlash()
{
    return bool{ cvar::rt_lightning_sectorflash };
}

void RT_OnLightningFlash()
{
    if( !bool{ cvar::rt_lightning } )
    {
        return;
    }

    g_lightning.active   = true;
    g_lightning.t0       = RT_GetCurrentTime();
    g_lightning.azimuth  = RT_LightningRand( 0.f, 360.f );
    g_lightning.variant  = int( RT_LightningRand( 0.f, 3.999f ) );

    // Guard the order rather than trusting the pair: these are two independent
    // archived cvars and nothing stops a console session from setting min above
    // max. rt_lightlevel_min/max spent a session inverted (200/1) and silently
    // defeated four A/B arms; not repeating that here.
    const float alo = std::min( float{ cvar::rt_lightning_alt_min },
                                float{ cvar::rt_lightning_alt_max } );
    const float ahi = std::max( float{ cvar::rt_lightning_alt_min },
                                float{ cvar::rt_lightning_alt_max } );
    g_lightning.altitude = std::clamp( RT_LightningRand( alo, ahi ), -89.f, 89.f );

    const int maxstrokes = std::clamp( int{ cvar::rt_lightning_strokes }, 1, 6 );
    const int n          = 1 + int( RT_LightningRand( 0.f, float( maxstrokes ) - 0.001f ) );
    g_lightning.nstrokes = n;

    float at = 0.f;
    for( int i = 0; i < n; i++ )
    {
        g_lightning.strokes[ i ].at = at;
        // First stroke is always full; later ones are weaker, which is both
        // what real return strokes do and what keeps the burst from reading as
        // three separate strikes.
        g_lightning.strokes[ i ].amp = ( i == 0 ) ? 1.f : RT_LightningRand( 0.35f, 0.8f );
        at += RT_LightningRand( 0.04f, 0.14f );
    }

    // DLSS-RR: a strike is the largest transient light in the game by an order
    // of magnitude. Flush temporal history or the flash smears for several
    // frames after it is over.
    g_rt_lightcut     = true;
    g_rt_lightcut_why = "lightning";

    if( int{ cvar::rt_lightning_debug } )
    {
        Printf( "rt_lightning: STRIKE az %.0f alt %.0f BOLT%d, %d stroke(s) at",
                g_lightning.azimuth, g_lightning.altitude,
                g_lightning.variant + 1, n );
        for( int i = 0; i < n; i++ )
        {
            Printf( " %.0fms(%.2f)", g_lightning.strokes[ i ].at * 1000.f,
                    g_lightning.strokes[ i ].amp );
        }
        Printf( "\n" );
    }
}



// Force a strike from the console: `thunder`.
//
// Worth having because the real trigger is deliberately unpredictable -- the
// storm thinker waits 5-20 seconds for its first strike and then 2-20 for
// each of the rest, so tuning rt_lightning_* by relaunching and waiting is
// most of an evening. This drives the same RT_OnLightningFlash the thinker
// calls, so it exercises the exact path, and it works on any map: the strike
// is renderer-side, so a map with no `lightning` in its MAPINFO can still be
// used to look at one. It does NOT play the thunder sound -- that is
// S_Sound in the thinker, not here.
CCMD( thunder )
{
    if( !bool{ cvar::rt_lightning } )
    {
        Printf( "thunder: rt_lightning is 0, nothing will happen\n" );
        return;
    }
    RT_OnLightningFlash();
    Printf( "thunder: forced a strike (no sound -- that is the storm thinker's)\n" );
}
