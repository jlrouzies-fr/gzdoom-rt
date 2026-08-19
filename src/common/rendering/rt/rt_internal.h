#pragma once

// Shared internals of the RT renderer, for the feature files split out of
// rt_main.cpp (rt_lights_*.cpp, rt_smoke.cpp, rt_presets.cpp, ...).
//
// NOT a public header: nothing outside common/rendering/rt should include it.
// It exists because rt_main.cpp used to keep every one of these in a single
// anonymous namespace, which is exactly what made the file impossible to split
// -- internal linkage means no second translation unit can see any of it.
//
// Everything lives in `namespace rtx` rather than at global scope: names like
// `RG_CHECK` and `ONEGAMEUNIT_IN_METERS` were only ever safe because they were
// file-local, and promoting them to real external symbols across the whole
// gzdoom link is asking for a collision. Each RT translation unit says
// `using namespace rtx;` once, so the moved code needs no edits.

#ifndef NOMINMAX
    #define NOMINMAX
#endif

// The engine surface every RT feature file needs: the level, its sectors and
// sides, actors, textures and the light thinkers. Carried here rather than
// repeated in each file so a new feature file is one include, and so the set
// cannot drift between them.
#include "g_levellocals.h"
#include "a_dynlight.h"
#include "actor.h"
#include "d_player.h"
#include "r_state.h"
#include "r_utility.h"
#include "p_local.h"
#include "p_lnspec.h"
#include "texturemanager.h"
#include "c_dispatch.h"
#include "i_time.h"
#include "mapthinkers/a_lights.h"
// FVector4PalEntry and FRenderState, for the colour helpers below and for
// the render-state classes.
#include "hw_renderstate.h"

#include "rt_state.h"
#include "rt_cvars.h"

// Generated fist offsets + colours for RT_UploadHandGlowLights.
#include "rt_hand_lights.h"
// Generated lit-switch-face table for RT_UploadSwitchLights.
#include "rt_switch_lights.h"

#include "palentry.h"
#include "vectors.h"

#define RG_USE_SURFACE_WIN32
#include <RTGL1/RTGL1.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <vector>

extern RgInterface rt;

// Print level for RT/RTGL1 *diagnostics* -- boot timings, the denoiser path,
// ReSTIR settings, per-level banners. Under `rt_verbose 0` (the release
// default) these carry PRINT_NONOTIFY, which keeps every line in the console
// buffer AND the logfile but stops it painting over the game on the notify
// overlay. Nothing is lost: `~` and rt-console.log still have it all.
//
// Use it for anything the renderer says on its own initiative. Do NOT use it
// for the reply to a CCMD the user typed (`whatsthat`, `moon`, `rt_dump_*`) --
// an answer to a question has to be visible where the question was asked.
inline int RT_DiagPrintLevel()
{
    return bool{ cvar::rt_verbose } ? PRINT_HIGH : ( PRINT_HIGH | PRINT_NONOTIFY );
}

// The map name the RT side keys everything on -- presets, scenes, titles. Not
// always primaryLevel's: a cutscene and the first-start scene each answer with
// their own name. Defined in rt_main.cpp.
const char* RT_GetMapName();
bool        RT_ForceNoClassicMode();

// The window's pixel size, which the fullscreen images and title cards lay out
// against. Defined in rt_main.cpp.
RgExtent2D RT_GetCurrentWindowSize();

// Where the sky openings are, in world units -- recorded per frame by the draw
// path so `rt_sky_here` can name the one feeding a room. Defined in rt_main.cpp.
void RT_NoteSkyPrim( std::span< const RgPrimitiveVertex > verts );
void RT_SkyPrimsEndFrame();

// The player's active powerups, as RT_POWERUP_FLAG_* bits.
uint32_t RT_CalcPowerupFlags();

// The camera basis for a rotation, in RTGL1's coordinate convention.
auto RT_MakeUpRightForwardVectors( const DRotator& rotation )
    -> std::tuple< RgFloat3D, RgFloat3D, RgFloat3D >;

// Set while the Remix runtime is the active backend, which several light paths
// have to know about because Remix resolves emissive surfaces differently.
extern bool g_isremix;

namespace rtx
{

inline void RG_CHECK( RgResult r )
{
    assert( ( r ) == RG_RESULT_SUCCESS );
}

#define RG_TRANSFORM_IDENTITY              \
    {                                      \
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 \
    }

constexpr auto ORIGINAL_DOOM_RESOLUTION_HEIGHT = 200;
// Is this flat one of the lava textures? PREFIX match, and for the usual reason:
// HLAVA1 is frame 1 of a 5-frame ANIMDEFS ping-pong, so the name on the sector is
// almost never the name the renderer is holding. D64LAVA1/2 are the warp2 patches.
inline bool RT_IsLavaFlat( const char* nm )
{
    if( !nm || !*nm )
    {
        return false;
    }
    return strncmp( nm, "HLAVA", 5 ) == 0 || strncmp( nm, "D64LAVA", 7 ) == 0;
}

constexpr auto ONEGAMEUNIT_IN_METERS           = 1.0f / 32.0f; // https://doomwiki.org/wiki/Map_unit

constexpr auto RG_PACKED_COLOR_WHITE = RgColor4DPacked32{ 0xFFFFFFFF };

// Doom64-RT: localised smoke. Defined next to RT_UploadFlameLights, which it is
// modelled on, but declared here because the spawn is triggered from
// RT_AddMuzzleFlash -- far earlier in this file -- so that the light and the
// smoke it lights come from one resolved muzzle position rather than two.
struct SmokeProfile
{
    const char* cls;      // substring of the weapon class name; nullptr = default
    float       count;    // multiplier on rt_smoke_count, rounded up
    float       radius;   // multiplier on rt_smoke_radius
    float       density;  // multiplier on rt_smoke_density
    float       life;     // multiplier on rt_smoke_life
    float       speed;    // multiplier on rt_smoke_speed (along the barrel)
    float       spread;   // multiplier on rt_smoke_spread (lateral scatter)
    float       rise;     // multiplier on rt_smoke_rise (buoyancy)
    int         trail;    // EXTRA parcels emitted one at a time AFTER the shot,
                          // every trailEvery tics, from the muzzle point. This is
                          // what makes a filament: a single burst is a ball at
                          // any radius, because a filament is a shape in TIME --
                          // a thin column only exists because the barrel keeps
                          // breathing while the earlier parcels rise away.
    int         trailEvery;
    float       growth;   // multiplier on rt_smoke_growth (expansion). SCALING
                          // LIFE WITHOUT THIS is what turned the pistol's wisp
                          // into a 1.6 m ball: a longer-lived parcel keeps
                          // expanding at the shared rate, so a thin profile has
                          // to slow its expansion as well as start small.
    const char* note;
    // Marks the puffs this profile spawns as ambient. TRAILING on purpose:
    // every existing row is positionally initialised, so a field added at the
    // end value-initialises to false and no row has to be touched.
    bool        ambient;
};

// The GLDEFS offset that puts a flame light ON the flame rather than at the
// actor's feet, for the sprites in RT_FLAME_KINDS. Ambient smoke has to rise
// from the same point the light comes from, and a second copy of those offsets
// would be a second thing to keep in sync. Defined in rt_lights_fx.cpp.
bool         RT_FlameSpriteOffset( AActor* mo, float* upMapUnits );

SmokeProfile RT_SmokeProfileFor( AActor* viewactor );
void         RT_SpawnSmokePuffs( const FVector3&     eye,
                                        const FVector3&     muzzle,
                                        const FVector3&     forward,
                                        const FVector3&     inheritVel,
                                        const SmokeProfile& prof );
void RT_ClearSmokePuffs();
// The live puff count, so the `smoke` CCMD can report it without duplicating
// any of the simulation's state.
extern uint32_t g_smokePuffCount;
// How many of those are AMBIENT (flame/torch) puffs. Bounded by
// rt_smoke_ambient_budget so a room full of torches can never spend the pool
// the player's own smoke needs -- see the eviction rule in RT_SpawnSmokePuffs.
extern uint32_t g_smokeAmbientCount;


enum
{
    RT_VINTAGE_OFF,
    RT_VINTAGE_CRT,
    RT_VINTAGE_VHS,
    RT_VINTAGE_VHS_CRT,
    RT_VINTAGE_200,
    RT_VINTAGE_200_DITHER,
    RT_VINTAGE_480,
    RT_VINTAGE_480_DITHER,
};


constexpr uint64_t FlashlightLightId  = 0xFFFFFFF + 0;
constexpr uint64_t SunLightId         = 0xFFFFFFF + 1;
constexpr uint64_t MuzzleFlashLightId = 0xFFFFFFF + 2;
// NOT 0xFFFFFFF + n: SectorLightId_Base starts at +3 and runs to +sectorCount, so any
// small offset here would collide with a sector's light. Own decade, like the lattices.
// Bit 50, far above SoloLatticeId_Base's (1<<40) sector-derived range.
constexpr uint64_t GunGlowLightId     = 1ull << 50;
// NOTE: lightning has NO id of its own, on purpose. RTGL1 accepts exactly one
// directional light per frame -- LightManager::Add answers a second one with
// debug::Error("Only one directional light is allowed"), which is fatal here --
// so the strike takes over SunLightId rather than adding a light. See the
// directional-light block in RT_DrawFrame.
constexpr uint64_t SectorLightId_Base = 0xFFFFFFF + 3;
// Keep clear of sector-light IDs (base + sectorIndex).
constexpr uint64_t DynLightId_Base    = 0xA0000000ull;

// DLSS-RR: any transient-light source that wants a full temporal-history
// flush (flashlight on/off, a dynlight appearing/disappearing) sets this.
// Consumed (and cleared) once per frame at RgDrawFrameInfo.resetHistory,
// rate-limited by rt_rr_reset_min_ms via g_rt_lastresetat. Declared here
// (rather than beside the similar g_resetposteffects further down) because
// RT_AddFlashlight() is an inline method of a class nested in this same
// anonymous namespace and needs ordinary forward-visible lookup.
inline bool   g_rt_lightcut    = false;
inline double g_rt_lastresetat = -1e9;
// Which setter raised g_rt_lightcut, for rt_rr_reset_debug. Static string only.
inline const char* g_rt_lightcut_why = "?";
// Self-emission threshold for rt_sector_emis, derived from THIS map's own lightlevel
// distribution rather than an absolute number. 180 means "glowing panel" in a dark
// corridor and "ordinary lit room" in a bright engineering deck, so a fixed global
// threshold cannot mean the right thing in both — it either misses the panels or makes
// every wall in the level a light. Recomputed on map change; starts closed (nothing
// emits) so a frame before the first update cannot flash the whole map bright.
inline float g_sectorEmisThreshold = 255.f;

constexpr uint64_t CeilingLampId_Base = 0xB0000000ull;
constexpr uint64_t HangLampId_Base    = 0xC0000000ull;
constexpr uint64_t WallStripId_Base   = 0xD0000000ull;
constexpr uint64_t CeilingEdgeId_Base = 0xE0000000ull;
// Faux panel lattice lights. Own range because their IDs are derived from a sector index
// and a lattice cell rather than from a linedef, so they cannot share the edge encoding.
constexpr uint64_t FauxLatticeId_Base = 0xF0000000ull;
// Solo bulb lattice lights. NOT the next round number after FauxLatticeId_Base: that
// formula is secIndex<<20 + ..., and secIndex alone can push the faux range past 32 bits
// on a map with thousands of sectors, so "the next range" is not a safe assumption here.
// Bit 40 is comfortably above anything that formula can produce.
constexpr uint64_t SoloLatticeId_Base = 1ull << 40;
// Hell Knight fist lights. Bit 42, not "the next value after SoloLatticeId_Base": that
// base is 1<<40 and its IDs add secIndex<<20 + cell, which can climb well past 1<<40
// itself. Bit 42 clears the whole reachable solo range with room to spare. Two lights per
// actor, so the low bit of the id is the hand index.
constexpr uint64_t HandLightId_Base      = 1ull << 42;
// Torch / fire / candle lights. Bit 43, one clear bit above the fist range: hand IDs are
// HandLightId_Base + (ptr & 0xFFFFFFFF) << 1 + hand, which spans 33 bits above 1<<42 and
// so climbs into 1<<43 territory on its own... it does NOT, because 1<<42 + 2^33 < 1<<43,
// but the margin is only there by arithmetic. Flame IDs use the same ptr-derived low bits
// with NO shift, so they occupy 1<<43 + 32 bits and cannot reach 1<<44.
constexpr uint64_t FlameLightId_Base     = 1ull << 43;
// Real bulb-array lattice lights (SFLATAS/SFLATAQ). Bit 44, one clear bit above the flame
// range, and NOT sharing CeilingEdgeId_Base even though these come from the same walk and
// the same budget: that base's IDs are line-derived (line->Index() * 2 * segsPerLine + sg)
// while a lattice ID is secIndex<<20 + cell, so the two encodings would overlap
// unpredictably inside one range. Bit 41 is technically free but the solo range below it
// is secIndex-derived and its headroom is only there by arithmetic — see SoloLatticeId_Base.
constexpr uint64_t CeilingLatticeId_Base = 1ull << 44;
// Spinning panel lights (CTEL). Bit 45, one clear bit above the lattice range: these IDs
// are secIndex<<1 + face, which cannot reach 1<<45 for any sector count a Doom map can
// hold. Two per sector, so the low bit is the face (0 floor, 1 ceiling).
constexpr uint64_t SpinPanelId_Base      = 1ull << 45;
// Switch faces. Keyed on the SIDEDEF index and part, not on a pointer: a switch is
// static geometry, so the id must be stable across frames or RTGL1 sees the whole set
// die and respawn every frame and throws away its temporal reservoirs. Room for
// sidedefIndex * 4 + part, which at Retribution's map sizes is far inside the next bit.
constexpr uint64_t SwitchLightId_Base    = 1ull << 46;
// Lava. Bit 47, keyed on the SECTOR index times the grid slot within it, so a lake's
// lights keep the same id every frame -- an id that moves makes RTGL1 see the whole
// set die and respawn, and it throws away its temporal reservoirs, which on lights
// this large reads as the entire room boiling.
constexpr uint64_t LavaLightId_Base      = 1ull << 47;
// Lights carried by FLYING sparks, as opposed to the impact flash below. Bit 49,
// the last free bit under GunGlowLightId at 1<<50. Keyed on the spark's own
// monotonic spawn id (low 32 bits), NOT on the light slot: a slot reassigned to a
// different spark each frame would be one light teleporting across the room,
// which is worse for ReSTIR than a light dying. Tied to the spark, a glow is born
// and dies with it and never jumps.
constexpr uint64_t SparkGlowId_Base      = 1ull << 49;
// Lights carried by ARC BRANCHES -- one per visible branch of a projectile impact
// mark. NOT a new bit: bit 50 is GunGlowLightId and bit 49 is the last one free,
// so this shares bit 49 with the spark glows above and stays disjoint from them
// by ARITHMETIC. A spark glow is SparkGlowId_Base + a monotonic uint32, so it can
// never reach 1<<40; starting the arcs there leaves the spark range 256x the room
// it can use and still lands far below 1<<50.
//
// The low bits are markUid * RT_ARC_MAX_BRANCH + branchIndex. The mark's uid is
// monotonic and never reused, so a branch light keeps ONE id for the whole life
// of the mark it belongs to -- which matters more here than anywhere else in this
// file, because an arc branch re-paths ~18 times a second and its light MOVES
// with it. A moving light with a stable id is a light RTGL1 can track; the same
// motion under a rotating id is the whole set dying and respawning every frame.
constexpr uint64_t ArcGlowId_Base        = SparkGlowId_Base + ( 1ull << 40 );
// Impact-spark flashes. Bit 48, one clear bit above the lava range: a lava ID is
// LavaLightId_Base + secIndex * LavaSlotsPerSector + cell, and secIndex * 65536 at
// any sector count a Doom map can hold stays far below 1<<48, so the two cannot
// meet. GunGlowLightId at 1<<50 is the next occupant above, leaving 49 spare.
//
// The low bits are the POOL SLOT, never the age or the tick. That is the rule
// SwitchLightId_Base and LavaLightId_Base both state: an id that moves makes RTGL1
// see the whole set die and respawn every frame and throw away its temporal
// reservoirs. A flash lives ~0.18 s, so it would be reborn for its entire life.
constexpr uint64_t SparkFlashId_Base     = 1ull << 48;
// 16 bits of slot: the grid CELL packed as (cellY & 0xFF) * 256 + (cellX & 0xFF).
// 256 was not enough -- a lava hall is easily more than 256 grid cells, and the
// wrap gave two lights in the same sector the same id, which RTGL1 asserts on and
// otherwise resolves by keeping one of them.
constexpr uint64_t LavaSlotsPerSector    = 65536;
// Segments per line, so a line's light IDs never collide with the next line's.
constexpr uint64_t WallStripSegsPerLine = 16;
// Bounds the id packing in RT_UploadCeilingEdgeLamps: line->Index() * 2 * this stays well
// under the 0x02000000 offset the debug markers add, so the two can never collide.
constexpr uint64_t CeilingEdgeSegsPerLine = 32;



// Doom 64 stores room atmosphere as a per-sector colormap (MAP02's blue armor room
// is lightcolor 0x0050FF). That is a post-shade filter from the original renderer,
// not a light: it has no position and no falloff, and GZDoom bakes it into vertex
// color together with lightlevel. Handing that to the path tracer as albedo
// double-counts shading — it is what produced the yellow key-door neon wash and the
// black light-absorbing rooms that rt_mod_compat's force-white works around.
//
// So: throw away the baked shading, keep the art intent. Normalize to hue only (peak
// channel == 1, so this can never darken a surface the way raw lightcolor did), then
// lerp toward white by `strength`. Feed most of it to light color — then emissive
// blobs, bloom, speculars and GI bounce all inherit the tint physically — and only a
// little to albedo, as a fallback for sectors that have no analytic light at all.
inline FVector3 RT_SectorHue( float r, float g, float b, float strength )
{
    const float peak = std::max( { r, g, b } );
    if( peak <= 0.001f )
    {
        return FVector3{ 1.0f, 1.0f, 1.0f };
    }

    const float s = std::clamp( strength, 0.0f, 1.0f );
    return FVector3{ 1.0f + ( r / peak - 1.0f ) * s,
                     1.0f + ( g / peak - 1.0f ) * s,
                     1.0f + ( b / peak - 1.0f ) * s };
}

inline FVector3 RT_SectorHue( const PalEntry& lightcolor, float strength )
{
    return RT_SectorHue( lightcolor.r / 255.0f,
                         lightcolor.g / 255.0f,
                         lightcolor.b / 255.0f,
                         strength );
}


// Colour, gamma and matrix helpers. Free functions in a header, so inline.
constexpr auto RT_BIT( uint32_t b )
{
    return 1u << b;
}
enum rt_powerupflag_t
{
    RT_POWERUP_FLAG_BONUS_BIT          = RT_BIT( 1 ),
    RT_POWERUP_FLAG_BERSERK_BIT        = RT_BIT( 2 ),
    RT_POWERUP_FLAG_RADIATIONSUIT_BIT  = RT_BIT( 3 ),
    RT_POWERUP_FLAG_INVUNERABILITY_BIT = RT_BIT( 4 ),
    RT_POWERUP_FLAG_INVISIBILITY_BIT   = RT_BIT( 5 ),
    RT_POWERUP_FLAG_NIGHTVISION_BIT    = RT_BIT( 6 ),
    RT_POWERUP_FLAG_THERMALVISION_BIT  = RT_BIT( 7 ),
    RT_POWERUP_FLAG_FLASHLIGHT_BIT     = RT_BIT( 8 ),
};
// NOTE: only the FLAGS live here. RT_CalcPowerupFlags() itself stays private to
// rt_main.cpp -- it reads the local player, which is the frame loop's business.



// rt_pi, not `pi`: gzdoom has a `namespace pi` (pi::pif()), and while a
// file-local pi() quietly shadowed it, an rtx:: one pulled in by a
// using-directive is merely ambiguous with it.
constexpr float rt_pi()
{
    return pi::pif();
}

constexpr float to_rad( float degrees )
{
    return degrees * ( rt_pi() / 180.0f );
}

constexpr FVector3 gzvec3(const RgFloat3D &v)
{
    return { v.data[ 0 ], v.data[ 1 ], v.data[ 2 ] };
}

constexpr DVector3 gzvec3d(const RgFloat3D &v)
{
    return { v.data[ 0 ], v.data[ 1 ], v.data[ 2 ] };
}

template< typename T >
auto applygamma( T x ) = delete;
template<>
inline auto applygamma( float x )
{
    return std::clamp( x * x, 0.f, 1.f );
}
template<>
inline auto applygamma( uint8_t x )
{
    return static_cast< uint8_t >( applygamma( float( x ) / 255.f ) * 255.f );
}

inline auto rtcolor( const PalEntry& e ) -> RgColor4DPacked32
{
    return rt.rgUtilPackColorByte4D( e.r, e.g, e.b, e.a );
}

inline auto rtcolor( const FVector4PalEntry& e ) -> RgColor4DPacked32
{
    return rt.rgUtilPackColorFloat4D( e.r, e.g, e.b, e.a );
}

inline auto cvarcolor_to_rtcolor( const FColorCVarRef& cvarcolor ) -> RgColor4DPacked32
{
    uint32_t ba = *( cvarcolor );

    int r = RPART( ba );
    int g = GPART( ba );
    int b = BPART( ba );

    return rt.rgUtilPackColorByte4D( r, g, b, 255 );
}

inline float lightlevel_to_classic( bool isui, float lightlevel )
{
    if( isui )
    {
        return 1.0f;
    }

    if( lightlevel < 0.0f )
    {
        return 1.0f;
    }

    float lmin = std::max( float( cvar::rt_classic_llmin ), 0.0f );
    float lmax = std::min( float( cvar::rt_classic_llmax ), 1.0f );

    float lrange = std::max( lmax - lmin, 0.0f );
    if( lrange < 0.001f )
    {
        lmin   = 0.0f;
        lmax   = 1.0f;
        lrange = 1.0f;
    }

    float t01 = std::clamp( lightlevel, 0.0f, 1.0f );
    t01       = std::pow( t01, float( cvar::rt_classic_llpow ) );
    
    return lmin + t01 * lrange;
}

inline auto rtcolor_multiply( const FVector4PalEntry& e, const FVector4& b, bool forcealpha1 ) -> RgColor4DPacked32
{
    return rt.rgUtilPackColorFloat4D( e.r * b[ 0 ], //
                                      e.g * b[ 1 ],
                                      e.b * b[ 2 ],
                                      forcealpha1 ? 1.0f : e.a * b[ 3 ] );
}

inline auto rtcolor_bgr_alphagamma( const PalEntry& e ) -> RgColor4DPacked32
{
    return rt.rgUtilPackColorByte4D( e.b, e.g, e.r, applygamma( e.a ) );
}

template< typename T >
void ApplyMat33ToVec3_row( const T row_mat[ 3 ][ 3 ], float ( &v )[ 3 ] )
{
    RgFloat3D r;
    for( int i = 0; i < 3; i++ )
    {
        r.data[ i ] = row_mat[ i ][ 0 ] * T( v[ 0 ] ) + row_mat[ i ][ 1 ] * T( v[ 1 ] ) +
                      row_mat[ i ][ 2 ] * T( v[ 2 ] );
    }
    v[ 0 ] = r.data[ 0 ];
    v[ 1 ] = r.data[ 1 ];
    v[ 2 ] = r.data[ 2 ];
}

template< typename T >
RgFloat4D ApplyMat44ToVec4( const T column_mat[ 4 ][ 4 ], const RgFloat4D& vs )
{
    const auto* v = vs.data;
    RgFloat4D   r;
    for( int i = 0; i < 4; i++ )
    {
        r.data[ i ] = column_mat[ 0 ][ i ] * T( v[ 0 ] ) + column_mat[ 1 ][ i ] * T( v[ 1 ] ) +
                      column_mat[ 2 ][ i ] * T( v[ 2 ] ) + column_mat[ 3 ][ i ] * T( v[ 3 ] );
    }
    return r;
}

template< typename T >
RgFloat4D ApplyMat44ToVec4( const T* column_mat, const RgFloat4D& vs )
{
    return ApplyMat44ToVec4< T >( reinterpret_cast< const T( * )[ 4 ] >( column_mat ), vs );
}

inline RgFloat3D FromHomogeneous( const RgFloat4D& v )
{
    return RgFloat3D{ v.data[ 0 ] / v.data[ 3 ],
                      v.data[ 1 ] / v.data[ 3 ],
                      v.data[ 2 ] / v.data[ 3 ] };
}

} // namespace rtx


// The per-frame light uploaders, split out of rt_main.cpp. Declared at global
// scope because that is where they have always been defined, and they are all
// still driven from the one call site in RT_DrawFrame.

// rt_lights_sector.cpp
void RT_MakeLightstyles();
void RT_UploadExportableSectorLights();
void RT_UploadGzDoomDynamicLights();
void RT_WatchLightlevels();
void RT_UpdateSectorEmisThreshold();
void RT_UpdateAnimatedSectorLights();

// rt_lights_fixtures.cpp
void RT_UploadSpinPanelLights();
void RT_UploadCeilingInsetLamps();
void RT_UploadWallStripLights();
void RT_UploadCeilingEdgeLamps();
void RT_UploadHangingTechLamps();
void RT_UploadHandGlowLights();
void RT_DebugNearbyWallTextures();

// rt_lights_fx.cpp
void RT_UploadSwitchLights();
void RT_UploadLavaLights();
void RT_UploadFlameLights();

// rt_light_shafts.cpp -- which fixtures get visible air around them.
//
// Not a light source of its own: the fixture walks in rt_lights_fixtures.cpp
// upload their lights exactly as before and OFFER them here, and this file only
// decides which of the offers are worth a shadow ray per froxel. That split is
// deliberate -- "is this lamp bright enough to light the room" and "does this
// lamp deserve a beam" are different questions, and answering the second inside
// the placement walks would bury it in three unrelated loops.
//
// Order in RT_DrawFrame matters: RT_ShaftLightsBegin() before the fixture
// uploads, RT_ShaftLightsSelect() after them and before the params are built.
enum RtShaftSrc : uint32_t
{
    RT_SHAFT_SRC_CEILING_INSET = 1, // one light at a lamp sector's centre
    RT_SHAFT_SRC_CEILING_EDGE  = 2, // the perimeter walk + the bulb/faux lattices
    RT_SHAFT_SRC_SOLO          = 4, // SFLATDE / SFLATCH / SFLATAS single bulbs
};

void RT_ShaftLightsBegin();
void RT_ShaftLightOffer( uint64_t     id,
                         double       mapX,
                         double       mapY,
                         double       mapZ,
                         float        intensity,
                         RtShaftSrc   src );
// Nearest-first, deduped, culled and capped. Valid until the next
// RT_ShaftLightsBegin(); empty when the feature is off.
//
// SAFE TO CALL TWICE IN A FRAME: the selection runs once and is cached, because
// two callers need it -- the volumetric params and the dust, which wants to know
// where the shafts are so it can be visible in them and barely anywhere else.
const std::vector< uint64_t >& RT_ShaftLightsSelect();

// The same set, with POSITIONS -- in MAP UNITS, the space the fixture walks
// offered them in. For anything that needs to ask "is this point in a shaft"
// without a ray: the answer is a proximity weight, and visibility is somebody
// else's problem (for dust, the path tracer's).
struct RtShaftLight
{
    uint64_t id;
    double   x, y, z;
    float    intensity;
};
const std::vector< RtShaftLight >& RT_ShaftLightsSelected();

// rt_dust.cpp -- dust motes in the air. One batched primitive per frame, and no
// state at all: the motes live on a hashed lattice fixed in WORLD space and this
// draws the cells near the camera, so it is a pure function of (camera, time).
void RT_DrawDust();

// rt_smoke.cpp
void RT_UpdateSmokePuffs();

// rt_sparks.cpp -- impact sparks. Three calls per frame, in this order: the sim
// steps the pool, the upload draws it, the lights go out with it. The SPAWN entry
// point is NOT here: it is called from playsim (P_LineAttack) and so lives at
// global scope, exactly as RT_SpawnFluid does -- see rt_sparks.cpp.
void RT_UpdateSparks();
// PROJECTILE IMPACT ARCS. Its own walk of the thinker list, once a tic, and its
// own master cvar -- NOT a hook into the smoke walk, which returns early when
// rt_smoke is off, again when its five sub-cvars are off, and a third time for
// any class outside RT_PROJECTILE_SMOKE (which contains no plasma and no BFG).
// Hooking it would have made plasma arcs vanish when rocket trails were turned
// off. The cost is one extra AActor iteration per TIC, against the smoke walk's
// one per FRAME, so this is the cheaper of the two that already ship.
void RT_UpdateProjectileImpacts();
void RT_DrawSparks();
void RT_UploadSparkLights();
void RT_ClearSparks();
void RT_SparkDebugTick();
// Live counts, so the `sparks` CCMD can report them without duplicating any of
// the simulation's state. Mirrors g_smokePuffCount above.
extern uint32_t g_sparkCount;
extern uint32_t g_sparkFlashCount;


// rt_presets.cpp -- the per-map moon/cloud/tint/fog tables.

// The two fog sentinels resolved against the map's own MAPINFO. Rebuilt every
// frame (it is a few reads), so `fog 00A0A0` in the console takes effect
// immediately instead of at the next level load.
struct ResolvedFog
{
    bool  on;
    float r, g, b;      // 0..1, the medium's scattering albedo AT THE CAMERA
    float rf, gf, bf;   // ... and at far_m. Equal to the above = uniform
    float density;      // at the camera
    float density_far;  // at far_m
    float curve;        // shape of the ramp between them
    float far_m;
    float ambient;
    bool  illum;
};

ResolvedFog RT_ResolveFog();
void        RT_ResolveFogIfPending();
void        RT_OnLevelLoadPresets( const char* mapname );


// rt_weather.cpp -- the storm. Several of these are also declared by hand in
// hw_skyportal.cpp and playsim/mapthinkers/a_lightning.cpp, which is where the
// strike and the cloud deck are driven from.
float RT_LightningFlashLevel();
bool  RT_LightningAim( float* azimuth, float* altitude, int* variant );
bool  RT_LightningWantsSectorFlash();
void  RT_OnLightningFlash();
void  RT_SetCloudSunTransmittance( float r, float g, float b );
void  RT_StopLightning();

// Per-channel, so cloud colour reaches the moon light and not just the picture.
// Written by RT_DrawCloudDeck, read by RT_DrawFrame.
extern float g_cloudSunTransmittance[ 3 ];


// rt_titles.cpp -- the map title cards. RT_StartTitleImage and the fullscreen
// image calls are also declared by hand in rt_cutscene.cpp, which drives them.
void RT_DrawTitle();
void RT_ClearTitles();
void RT_InjectTitleIntoDoomMap( const char* mapname );

// Per-sector lightlevel snapshot, handed to RTGL1 as pLightstyleValues8 in
// RT_DrawFrame. Owned by rt_lights_sector.cpp.
extern std::vector< uint8_t > g_sectorlightlevels;

namespace rtx
{

// The live puff pool. RT_DrawFrame does its own distance sort over this to pick
// the frame's budget, so the storage has to be visible outside rt_smoke.cpp.
struct SmokePuff
{
    FVector3 pos;     // metres
    FVector3 vel;     // metres / second
    float    radius;  // metres
    float    radius0; // metres at birth. Expansion must DILUTE: a parcel that
                      // grows 37x and keeps its density is 37x more smoke than
                      // was fired, which is exactly what "too much smoke" looks
                      // like. Real smoke conserves what it started with.
    float    density; // optical depth per metre at the core, AFTER the weapon
                      // profile -- captured at spawn so two weapons' smoke can
                      // be in the air at once with different densities
    float    rise;    // m/s^2, likewise per weapon
    float    growth;  // m/s of expansion, likewise per weapon
    float    phase;   // radians, per parcel, so neighbours in a trail curl on
                      // different schedules -- without it the whole column
                      // waves in unison and reads as a ribbon, not smoke
    float    age;     // seconds
    float    life;    // seconds, captured at spawn so a cvar change mid-flight
                      // cannot make a live puff immortal or kill it instantly
    // AMBIENT SMOKE MUST NEVER STARVE THE PLAYER'S, and this flag is how.
    //
    // Every other source is an EVENT -- a shot, an explosion, a projectile that
    // ends. Torch smoke is continuous and there are dozens of torches, so left
    // to the plain oldest-out overflow rule it would win every contest for a
    // slot simply by never stopping. The pool evicts ambient puffs first, and
    // rt_smoke_ambient_budget caps how many may be alive at once.
    bool     ambient;
};

extern std::array< SmokePuff, RG_MAX_SMOKE_PUFFS > g_smokePuffs;

} // namespace rtx
