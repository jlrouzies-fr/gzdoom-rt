// Impact sparks: the hot bits thrown off when a hitscan hits the world.
//
// THE TRIGGER IS A REAL GAME HOOK, and that is the one thing about this feature
// that differs from every other FX source in the renderer. rt_smoke.cpp's six
// sources all infer their trigger -- a sprite frame, a pointer disappearing, the
// rising edge of extralight -- because the ACTOR CLASSES belong to the
// Retribution WAD and nothing may require a DECORATE edit. That constraint does
// not apply to an impact: P_LineAttack is engine C++ we already patch, and it is
// the only place in the game where a hitscan's surface is known.
//
// WHY P_LineAttack AND NOT P_SpawnPuff. P_SpawnPuff is one level too late. It
// receives a yaw and an updown int, which is why RT_SpawnBlood_Thing sitting
// beside it has to reconstruct a fake normal and says so in a comment. In
// P_LineAttack the whole FTraceResults is still in scope, so the sparks get a
// TRUE surface normal off trace.Line/trace.Side and can be reflected the way a
// fragment bouncing off a wall actually goes. That reflection is the entire read
// of the effect.
//
// WHAT A SPARK IS. One flat, single-coloured, camera-facing square in world
// space, snapped to a world grid, coloured from the PUFF sprite's own 16-colour
// palette ramp. No texture, no gradient, no art: the quad IS the pixel. All live
// sparks go up as ONE batched primitive per frame.
//
// NOT SMOKE. A 1 cm bright dot cannot be represented in a 64-slice froxel
// volume, so nothing here touches rt_smoke.cpp or RgDrawFrameSmokeParams. The
// particles are an ADDITIVE rasterized overlay -- outside the acceleration
// structure, so they appear in no reflection and cast no GI. The ray-traced half
// of the effect is RT_UploadSparkLights: ONE short spherical light per impact.
//
// See docs/plan-impact-fx.md.

#include "rt_internal.h"

// Not pulled in by p_local.h, and tier 2 of the collision needs Trace() --
// the same function P_LineAttack itself uses.
#include "p_trace.h"
// FBitmap (the decoded texture) and averageColor(). rt_internal.h pulls in
// texturemanager.h but neither of these.
#include "bitmap.h"
#include "palutil.h"

#include <array>
#include <fstream>
#include <string>
#include <unordered_map>

// The shared internals (RG_CHECK, ONEGAMEUNIT_IN_METERS, the light-ID bases)
// come in unqualified, as in every other RT feature file.
using namespace rtx;

namespace
{

// PUFF's OWN PALETTE, sampled from the PLTE of PUFFA0..PUFFF0 in D64RTR_v15.WAD.
//
// A spark indexes this by age, so it cools through exactly the colours of the
// sprite it fires alongside -- the two match by construction rather than by eye,
// which is the whole reason the puff art was the starting point for this work.
// Hot pale yellow at birth, dark brown ember at death.
constexpr uint32_t RT_SPARK_RAMP[] = {
    0xF8F8B0, 0xB8A868, 0x786040, 0x604820, 0x482818, 0x301808, 0x180800,
};
constexpr int RT_SPARK_RAMP_N = int( std::size( RT_SPARK_RAMP ) );

// DEBRIS gets its own ramp, and it is deliberately NOT a dimmed spark ramp: a
// chip of wall is cool, dusty and slightly warm-grey, and it does not cool as it
// falls because it was never hot. It darkens slightly as it tumbles out of the
// light, which is the only change along its life.
//
// The real colour of a chip is the surface it came off, and this is the
// placeholder for that: FTraceResults carries HitTexture, so the hit texture's
// own average albedo is reachable and is the obvious upgrade once the metal
// classification exists to make debris worth looking at. Until then a neutral
// ramp is honest, and a WRONG per-surface colour would be worse than a neutral
// one.
// THESE ARE ALBEDOS, NOT PIXEL COLOURS, and that is why the first set read far
// too bright. Debris is ray-traced geometry now, so the ramp is what the path
// tracer MULTIPLIES the room's light by -- a value of 0.60 (0x9A) is a white
// laboratory tile, not a chip of wall. Real rock and concrete sit around
// 0.20-0.35, and the top of each ramp is chosen there. The old numbers were
// picked while debris was still a rasterized overlay, where the ramp WAS the
// final colour and 0.60 was reasonable; the move to traced geometry silently
// changed what they mean. Reported from play as "too bright colors".
constexpr uint32_t RT_DEBRIS_RAMP[] = {
    0x484440, 0x3D3A36, 0x33302D, 0x282623, 0x1E1C1A, 0x151412, 0x0E0D0C,
};
constexpr int RT_DEBRIS_RAMP_N = int( std::size( RT_DEBRIS_RAMP ) );

// CONCRETE gets its own, paler and dustier. Stone chips off light and throws
// pale dust; the generic "other" ramp above is darker because it covers panels,
// crates and machinery rather than masonry. Both are neutral placeholders for
// the hit texture's real albedo -- see the note above -- but a two-way split
// costs nothing and is already better than one grey for everything, now that the
// labeller distinguishes them.
constexpr uint32_t RT_CONCRETE_RAMP[] = {
    0x5A554D, 0x4E4941, 0x423E37, 0x36322D, 0x2A2723, 0x1E1C19, 0x14120F,
};
constexpr int RT_CONCRETE_RAMP_N = int( std::size( RT_CONCRETE_RAMP ) );

// Per-class fallback ramps. These are what a chip looks like BEFORE the hit
// texture's own colour is blended over them (rt_spark_debris_tint), so they only
// fully show on a texture whose average could not be read. All are albedos in
// the 0.2-0.35 band for the reason above.
constexpr uint32_t RT_WOOD_RAMP[] = {
    0x6B5334, 0x5C4830, 0x4D3D2A, 0x3F3224, 0x31281D, 0x241D15, 0x17130E,
};
constexpr int RT_WOOD_RAMP_N = int( std::size( RT_WOOD_RAMP ) );

constexpr uint32_t RT_DIRT_RAMP[] = {
    0x4A3B2A, 0x403325, 0x362B20, 0x2B231A, 0x211B14, 0x17130E, 0x0F0C09,
};
constexpr int RT_DIRT_RAMP_N = int( std::size( RT_DIRT_RAMP ) );

// Blood. Darker and less saturated than the sprite art, because this is an
// ALBEDO the room lights rather than a painted pixel -- a 0.8-red droplet under
// a bright lamp reads as neon.
constexpr uint32_t RT_FLESH_RAMP[] = {
    0x6E1512, 0x5E120F, 0x4E0F0D, 0x3F0C0A, 0x300908, 0x220706, 0x160404,
};
constexpr int RT_FLESH_RAMP_N = int( std::size( RT_FLESH_RAMP ) );

// Fluid barely uses its ramp: its whole point is to be the colour of the liquid
// it came out of, so its profile drives the tint to 1 and this is only the
// fallback for a texture that could not be sampled.
constexpr uint32_t RT_FLUID_RAMP[] = {
    0x40564A, 0x384C42, 0x304139, 0x28362F, 0x1F2A25, 0x161E1A, 0x0E1310,
};
constexpr int RT_FLUID_RAMP_N = int( std::size( RT_FLUID_RAMP ) );

// ---------------------------------------------------------------------------
// THE COLOUR OF THE WALL YOU JUST SHOT
//
// A chip should be the colour of what it was chipped off, and the whole reason
// that is now possible is the HitTexture fix: the hook derives the wall texture
// itself, so a valid FTextureID reaches this file where before every impact
// arrived with an empty one.
//
// Averaged with gzdoom's own averageColor() over the texture's BGRA bitmap --
// the same call FGameTexture::GetGlowColor uses. NOTE the third argument:
// GetGlowColor passes 153, which NORMALIZES the result so the peak channel hits
// that value, brightening a dull texture into a saturated glow hue. That is
// exactly wrong here. An albedo wants the TRUE mean, so this passes 0.
//
// CACHED PER TEXTURE, and it has to be: GetBgraBitmap decodes the whole image,
// which is far too expensive to do per impact, let alone per chip. Computed once
// on first hit and kept -- a level has a few dozen distinct wall textures.
std::unordered_map< int, uint32_t > s_texAvgCache;

uint32_t AverageTextureColor( FTextureID tex )
{
    if( !tex.isValid() )
    {
        return 0x808080;
    }

    const int key = tex.GetIndex();
    if( auto it = s_texAvgCache.find( key ); it != s_texAvgCache.end() )
    {
        return it->second;
    }

    uint32_t out = 0x808080;

    if( FGameTexture* gt = TexMan.GetGameTexture( tex ) )
    {
        if( FTexture* base = gt->GetTexture() )
        {
            FBitmap bmp = base->GetBgraBitmap( nullptr );
            const int n = bmp.GetWidth() * bmp.GetHeight();
            if( n > 0 )
            {
                // maxout 0 -> the honest mean. See the note above.
                const PalEntry c =
                    averageColor( reinterpret_cast< const uint32_t* >( bmp.GetPixels() ), n, 0 );
                out = ( uint32_t( c.r ) << 16 ) | ( uint32_t( c.g ) << 8 ) | uint32_t( c.b );
            }
        }
    }

    s_texAvgCache[ key ] = out;
    return out;
}

// The surface classes the renderer distinguishes. Anything upstream invents that
// is not in this list degrades to Other -- i.e. debris -- rather than erroring,
// so the labelling pipeline can grow a class without breaking the game.
// THE SURFACE CLASSES, in the order the profile table below is indexed. Adding
// one is: a value here, a name in the two functions under it, and a row in
// RT_DEBRIS_PROFILES. Nothing else in the file switches on the class.
//
// `Other` is last and is the fallback: a class the labeller invents that this
// build does not know parses to Other and therefore SPARKS, which is the
// shipped behaviour. A new label can never make the game look broken, only
// un-upgraded.
enum class SurfKind : uint8_t
{
    Metal,     // hot sparks -- no debris at all
    Concrete,  // grey chips
    Wood,      // long thin splinters
    Dirt,      // small dull crumbs, barely bounce
    Flesh,     // blood droplets
    Fluid,     // splash droplets, the fluid's own colour
    Other,     // unknown / unclassified -> sparks
    COUNT
};

SurfKind ParseSurfKind( const FString& s )
{
    if( s.IsEmpty() || s.CompareNoCase( "metal" ) == 0 )
    {
        // Empty means a bare NAME with no class column: that was the first
        // version of this file, where listing a texture at all meant metal.
        return SurfKind::Metal;
    }
    if( s.CompareNoCase( "concrete" ) == 0 ) return SurfKind::Concrete;
    if( s.CompareNoCase( "wood" ) == 0 )     return SurfKind::Wood;
    if( s.CompareNoCase( "dirt" ) == 0 )     return SurfKind::Dirt;
    if( s.CompareNoCase( "flesh" ) == 0 )    return SurfKind::Flesh;
    if( s.CompareNoCase( "fluid" ) == 0 )    return SurfKind::Fluid;
    return SurfKind::Other;
}

const char* SurfKindName( SurfKind k )
{
    switch( k )
    {
        case SurfKind::Metal: return "metal";
        case SurfKind::Concrete: return "concrete";
        case SurfKind::Wood: return "wood";
        case SurfKind::Dirt: return "dirt";
        case SurfKind::Flesh: return "flesh";
        case SurfKind::Fluid: return "fluid";
        default: return "other";
    }
}

// Which classes throw debris at all. Metal and Other spark -- see the opt-in
// rule: a texture must be positively labelled something else to change.
bool SurfThrowsDebris( SurfKind k )
{
    return k != SurfKind::Metal && k != SurfKind::Other;
}

// PER-CLASS DEBRIS, as MULTIPLIERS on the rt_spark_debris_* cvars.
//
// Same shape and the same reason as RT_SMOKE_PROFILES: tuning a cvar still moves
// every class together, and a row only states how that class DIFFERS. A row of
// all 1s would be plain concrete.
//
// The four axes that actually separate these at a glance, in order of how much
// they carry: ASPECT (a splinter is not a crumb), GRAVITY and BOUNCE (dirt drops
// dead, a wood chip floats down), COUNT, and only then colour -- which mostly
// comes from the hit texture anyway.
struct DebrisProfile
{
    const uint32_t* ramp;
    int             rampN;
    // Sprite to stamp on the quad, or nullptr for a flat colour. Left nullptr
    // everywhere for now ON PURPOSE: an RTGL1 material only exists once gzdoom
    // has actually drawn that texture, so naming a sprite the level has not
    // shown yet gets the 1x1 white fallback -- white blood is worse than red
    // untextured. Wire BLUD here only after confirming it resolves.
    const char*     texture;
    float           count;
    float           size;
    float           life;
    float           speed;
    float           gravity;
    float           bounce;
    float           friction;
    float           aspectLo;   // ABSOLUTE, not a multiplier: shape is the class
    float           aspectHi;
    float           spin;
    float           tint;       // x rt_spark_debris_tint
    float           albedo;     // x rt_spark_debris_albedo
};

// Indexed by SurfKind. Metal and Other are present but never used (they spark),
// and are filled with the concrete row so an indexing mistake is dull rather
// than undefined.
constexpr DebrisProfile RT_DEBRIS_PROFILES[ int( SurfKind::COUNT ) ] = {
    // Metal -- unused, sparks.
    { RT_CONCRETE_RAMP, RT_CONCRETE_RAMP_N, nullptr,
      1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.55f, 1.65f, 1.f, 1.f, 1.f },

    // CONCRETE: the reference row. Blocky chips, heavy, barely bouncy.
    { RT_CONCRETE_RAMP, RT_CONCRETE_RAMP_N, nullptr,
      1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.55f, 1.65f, 1.f, 1.f, 1.f },

    // WOOD: SPLINTERS. The aspect range is the whole read -- long and thin, and
    // never square. Lighter than stone so it falls slower and further, tumbles
    // much faster (a flat splinter catches air), and bounces a little more.
    { RT_WOOD_RAMP, RT_WOOD_RAMP_N, nullptr,
      1.1f, 1.15f, 1.f, 1.15f, 0.62f, 1.5f, 0.8f, 0.12f, 0.35f, 2.2f, 1.f, 1.1f },

    // DIRT: crumbs. Many, small, and they DO NOT BOUNCE -- dirt hits and stops,
    // which is what separates it from concrete at a glance. High friction so the
    // few that do skid stop immediately.
    //
    // LIFE 1.0, i.e. the full rt_spark_debris_life like concrete and wood. It was
    // 0.55 on the reasoning that loose earth does not lie around as debris; in
    // play that just made dirt vanish while the chips beside it stayed, which
    // reads as the effect failing rather than as a material difference. The
    // three solid classes now share one lifetime and differ only in how they
    // move, which is the axis that actually carries them.
    { RT_DIRT_RAMP, RT_DIRT_RAMP_N, nullptr,
      1.8f, 0.6f, 1.f, 0.85f, 1.15f, 0.12f, 2.2f, 0.7f, 1.4f, 1.f, 1.f, 0.9f },

    // FLESH: blood droplets. Round-ish, wet, and they SPLAT rather than bounce.
    // Slower and fatter than a chip, and short-lived: a droplet in flight is a
    // moment, and the lasting mark is the blood decal system's job, not this.
    { RT_FLESH_RAMP, RT_FLESH_RAMP_N, nullptr,
      1.6f, 0.85f, 0.35f, 0.9f, 1.25f, 0.05f, 2.5f, 0.8f, 1.25f, 0.5f, 0.75f, 1.15f },

    // FLUID: a splash. The most droplets of anything, small and fast, thrown
    // wide, gone quickly. tint 1.18 pushes rt_spark_debris_tint past 1 so it
    // CLAMPS at full texture colour -- "coloured the same as the texture" is the
    // requirement, so the neutral ramp must not show through at all. The albedo
    // is raised too: a liquid reads brighter than rubble.
    { RT_FLUID_RAMP, RT_FLUID_RAMP_N, nullptr,
      2.4f, 0.5f, 0.3f, 1.35f, 1.f, 0.08f, 2.2f, 0.75f, 1.3f, 1.f, 1.18f, 1.5f },

    // Other -- unused, sparks.
    { RT_CONCRETE_RAMP, RT_CONCRETE_RAMP_N, nullptr,
      1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.55f, 1.65f, 1.f, 1.f, 1.f },
};

const DebrisProfile& ProfileFor( SurfKind k )
{
    const int i = int( k );
    return RT_DEBRIS_PROFILES[ ( i >= 0 && i < int( SurfKind::COUNT ) ) ? i : int( SurfKind::Other ) ];
}

enum class SparkKind : uint8_t
{
    Spark,  // metal: hot, additive, casts a flash
    Debris, // everything else: dull, OPAQUE ray-traced geometry, casts nothing
};

// Pool ceilings. These bound the fixed arrays; the cvars bound how much of them
// is used, so raising a cvar past its ceiling is clamped rather than corrupting.
// THE POOL HAD TO GROW WITH THE LIFETIMES. Sparks went 1.1 s -> 5 s and debris
// 1.4 s -> 20 s, and live count is spawn rate x lifetime, so the same firing
// makes roughly five times the sparks and fourteen times the chips. At the old
// 1024 a couple of seconds of chaingun filled the pool and the eviction rule
// became the thing you were watching. A Spark is ~72 bytes, so 4096 is ~290 KB
// -- irrelevant next to being able to honour the lifetimes that were asked for.
constexpr uint32_t RT_SPARK_HARDMAX  = 4096;
constexpr uint32_t RT_SPARK_FLASH_MAX = 64;

// A single fixed mesh ID for the batched quad primitive. Same reasoning as
// RT_SPRITE_SHADOW_ID_BASE / RT_SPRITE_AO_ID_BASE in rt_draw.cpp, one bit down:
// bits 60-62 are all beyond the 0x00007FFF'FFFFFFFF ceiling on a Windows x64
// user-space pointer, so this can never collide with an actor-derived ID. RTGL1
// drops a duplicate ID silently, and it is the NEW primitive that loses.
constexpr uint64_t RT_SPARK_MESH_ID = 0x1000000000000000ull;

// The second batch's mesh ID. One clear value above the spark batch; both are in
// the same unreachable-pointer range, and they must differ or RTGL1 keeps only
// one of the two uploads.
constexpr uint64_t RT_DEBRIS_MESH_ID = 0x1000000000000001ull;

// The contact-occlusion decals. Well clear of the per-class debris IDs above,
// which run to RT_DEBRIS_MESH_ID + SurfKind::COUNT.
constexpr uint64_t RT_DEBRIS_AO_MESH_ID = 0x1000000000000100ull;

struct Spark
{
    FVector3  pos;      // METRES
    FVector3  vel;      // metres / second
    float     age;
    float     life;
    float     size;     // metres, edge of the square
    sector_t* sec;      // cached, so "did this step leave its sector" is free
    bool      settled;  // came to rest; no longer integrated
    SparkKind kind;
    // The class of the surface this came off, so debris can be coloured by what
    // it was chipped from rather than all alike.
    SurfKind  surf;
    // REALISTIC-style debris only: a chip is not a neat square. `phase` is its
    // starting orientation, `spin` how fast it tumbles, `aspect` how far from
    // square it is. Three cheap per-particle randoms are what turn a cloud of
    // identical pixels into rubble; the pixelated style ignores all of them
    // because there a chip is deliberately one square texel.
    float     phase;
    float     spin;
    float     aspect;
    // DEBRIS: the average colour of the texture it was chipped off, resolved
    // once at spawn rather than per frame. 0xRRGGBB.
    uint32_t  baseRgb;
    // The surface normal this came off. Used as DEBRIS's shading normal: the
    // quad itself is camera-facing so a chip is never edge-on and invisible, but
    // a camera-facing NORMAL would make the lighting swing as the player turns,
    // which on ray-traced geometry reads as the chip flickering. Sparks do not
    // use it -- they are additive and unlit.
    FVector3  nrm;
    // Monotonic at spawn. This is the spark's IDENTITY, and the only thing a
    // glow light's uniqueID may be derived from -- see SparkGlowId_Base. The
    // pool index cannot serve: removal is swap-with-back, so an index is
    // reassigned to an unrelated spark the moment one dies.
    uint32_t  sid;
};

// The per-IMPACT flash. One of these per impact, never one per spark.
struct SparkFlash
{
    FVector3 pos;   // METRES
    float    age;
    float    life;
};

std::array< Spark, RT_SPARK_HARDMAX >          s_sparks{};
std::array< SparkFlash, RT_SPARK_FLASH_MAX >   s_flashes{};

int s_lastTic = -1;

// Counters for the debug ladder and the `sparks` CCMD. Reset each second.
int s_dbgHits     = 0;
int s_dbgRejected = 0;
int s_dbgSpawned  = 0;
int s_dbgQuads    = 0;
int s_dbgLights   = 0;
int s_dbgTraces   = 0;
// AO blobs UPLOADED this frame. The distinction docs/sprite-shadows-and-ao.md
// insists on: emitted > 0 with nothing on screen is a POSITION bug, not a
// strength one, and the two are indistinguishable without this.
int s_dbgAo       = 0;

// Per-CLASS hit counters, so the ladder can report what the impacts were landing
// on rather than only how many there were. Reset with the rest of the counters.
int s_dbgMetal    = 0;
int s_dbgConcrete = 0;
int s_dbgOther    = 0;
int s_dbgUnlisted = 0;

// DISTINCT texture names that were hit and are NOT in the table. This is the
// single most useful output the feature produces while the labelling is in
// progress: it is a worklist, gathered by playing rather than by auditing 2331
// textures. Capped and first-seen-order, because it exists to be read.
constexpr size_t RT_UNLISTED_MAX = 48;
std::vector< FString > s_unlistedSeen;

void NoteUnlisted( const FString& name )
{
    if( name.IsEmpty() || s_unlistedSeen.size() >= RT_UNLISTED_MAX )
    {
        return;
    }
    for( const FString& n : s_unlistedSeen )
    {
        if( n.Compare( name ) == 0 )
        {
            return;
        }
    }
    s_unlistedSeen.push_back( name );
}

// A LOCAL generator on purpose. The gameplay RNG (M_Random and friends) is part
// of the simulation -- consuming it from the renderer would desync a demo or a
// netgame, and the desync would be invisible until someone recorded one. This
// matters more here than it does for smoke, because the spawn call happens
// inside playsim rather than at draw time. Nothing about a spark's jitter needs
// to be reproducible across machines.
uint32_t s_rng = 0x6C078965u;

float rnd11()
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return float( s_rng & 0xFFFF ) / 32767.5f - 1.f;
}

float rnd01()
{
    return 0.5f * ( rnd11() + 1.f );
}

// A stable per-particle random. Keyed on the spark's own `sid`, so a chip's
// shape and orientation are fixed for its whole life instead of reshuffling
// every frame -- which at a 20 s lifetime would be the most visible thing debris
// does. Not the sim RNG: that advances per spawn and cannot be re-queried.
float hash01( uint32_t x )
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return float( x & 0xFFFFu ) / 65535.f;
}

// Snap to a world-space grid. WORLD space, never screen space: screen-space
// blocks crawl as soon as the camera turns and the eye reads that as noise
// rather than as style. Same reasoning as rt_smoke_stylize_grid.
float snap( float v, float grid )
{
    return grid > 1e-6f ? std::round( v / grid ) * grid : v;
}

// ---------------------------------------------------------------------------
// Which surfaces are METAL
//
// DOOM 64 TEXTURE NAMES CARRY NO MEANING -- C1, C102B, C307B1, SPACEAO1 -- so
// there is no prefix rule that separates metal from stone and this cannot be a
// pattern the way l_waterflag is. It has to be an explicit list, and an explicit
// list of a thousand entries has to be editable without a rebuild, or nobody will
// ever finish it. Hence a data file, reloadable in-game with `spark_surfaces`.
//
// The file lives in Retribution-RT-Materials/rt/data/ (tracked, and staged into
// the build tree by build-gzdoom-rt.cmd) rather than in the gitignored build rt/,
// which would be wiped by the next restage.
// ---------------------------------------------------------------------------

struct SurfEntry
{
    FString  name;
    SurfKind kind;
};

std::vector< SurfEntry > s_surfExact;
std::vector< SurfEntry > s_surfPrefix;
bool                     s_surfacesLoaded = false;
// What the last load attempt tried and whether it found anything. Reported by
// the `spark_surfaces` CCMD -- a table that silently fails to load is
// indistinguishable in play from a feature that does nothing.
FString                  s_surfacesPath;
bool                     s_surfacesFound = false;

void LoadSparkSurfaces()
{
    s_surfacesLoaded = true;
    s_surfExact.clear();
    s_surfPrefix.clear();

    // A PLAIN FILE, NOT A LUMP, and getting this wrong is why the table silently
    // did nothing on first ship.
    //
    // rt/data/ is RTGL1's own directory on disk, next to gzdoom.exe. It is NOT in
    // gzdoom's lump filesystem: that holds the IWAD, the -file PWADs and rt/wad,
    // and nothing else. fileSystem.CheckNumForFullName( "rt/data/..." ) therefore
    // always answers -1, however present the file is. Every other RT reference to
    // this tree reads it the same way this now does -- see the
    // std::filesystem::exists( "rt/bin_remix/..." ) checks in remix_launcher.cpp.
    //
    // The original version used the lump path AND treated "not found" as a silent
    // normal state, so the failure produced no output at all and read from play as
    // "the classification does nothing". Hence s_surfacesPath / s_surfacesFound
    // below: the `spark_surfaces` CCMD reports what was tried and what happened,
    // and never claims success it did not have.
    s_surfacesPath  = "rt/data/spark_surfaces.txt";
    s_surfacesFound = false;

    std::ifstream f( s_surfacesPath.GetChars(), std::ios::binary );
    if( !f.is_open() )
    {
        // Console + logfile always; the notify overlay only under rt_verbose.
        // NOT silent, whatever the cvars say: this is the state where the whole
        // classification does nothing, and it previously looked exactly like a
        // feature that had been implemented and did not work.
        Printf( RT_DiagPrintLevel(),
                "rt_spark surfaces: '%s' NOT FOUND -- every surface will classify as "
                "'other'. Run: python tools/build_spark_surfaces.py\n",
                s_surfacesPath.GetChars() );
        return;
    }

    std::string raw( ( std::istreambuf_iterator< char >( f ) ),
                     std::istreambuf_iterator< char >() );
    s_surfacesFound = true;

    FString text{ raw.c_str(), raw.size() };

    FString cur;
    auto    l_flush = [ & ]() {
        cur.StripLeftRight();
        if( cur.IsEmpty() || cur[ 0 ] == '#' )
        {
            cur = "";
            return;
        }
        // `NAME class`, with the class optional. Split on the first run of
        // whitespace; a line with no class is a bare NAME, which the first
        // version of this file used to mean metal, so ParseSurfKind maps an
        // empty class to Metal and old files keep working.
        FString nameTok = cur;
        FString kindTok;
        for( size_t k = 0; k < cur.Len(); k++ )
        {
            const char c = cur[ int( k ) ];
            if( c == ' ' || c == '\t' )
            {
                nameTok = cur.Left( k );
                kindTok = cur.Mid( k );
                kindTok.StripLeftRight();
                break;
            }
        }

        nameTok.ToUpper();
        const SurfKind kind = ParseSurfKind( kindTok );

        if( nameTok.Len() > 1 && nameTok[ nameTok.Len() - 1 ] == '*' )
        {
            nameTok.Truncate( nameTok.Len() - 1 );
            s_surfPrefix.push_back( SurfEntry{ nameTok, kind } );
        }
        else
        {
            s_surfExact.push_back( SurfEntry{ nameTok, kind } );
        }
        cur = "";
    };
    // filled in after the parse loop below

    for( size_t i = 0; i < text.Len(); i++ )
    {
        const char c = text[ int( i ) ];
        if( c == '\n' || c == '\r' )
        {
            l_flush();
        }
        else
        {
            cur += c;
        }
    }
    l_flush();

    int n[ 3 ]{};
    for( const SurfEntry& e : s_surfExact )
    {
        n[ int( e.kind ) ]++;
    }
    for( const SurfEntry& e : s_surfPrefix )
    {
        n[ int( e.kind ) ]++;
    }

    // One line per load, console + logfile. It answers the two questions that
    // matter -- did the table load at all, and how far has the labelling got --
    // without needing the CCMD or a debug cvar, which is the point: the previous
    // version said nothing on either success or failure.
    Printf( RT_DiagPrintLevel(),
            "rt_spark surfaces: %d entries from %s  (metal %d -> sparks, "
            "concrete %d + other %d -> debris)\n",
            int( s_surfExact.size() + s_surfPrefix.size() ),
            s_surfacesPath.GetChars(),
            n[ int( SurfKind::Metal ) ],
            n[ int( SurfKind::Concrete ) ],
            n[ int( SurfKind::Other ) ] );
}

// The surface class of a texture. Unlisted is Other, i.e. debris -- so a texture
// the labeller has not reached yet degrades to the dull effect rather than to a
// wrong hot one. `outListed` distinguishes "labelled as other" from "not
// labelled", which is the difference the surface probe has to show.
SurfKind SurfaceKindOf( FTextureID tex, FString* outName, bool* outListed )
{
    if( !s_surfacesLoaded )
    {
        LoadSparkSurfaces();
    }

    if( outListed )
    {
        *outListed = false;
    }

    FString name;
    if( tex.isValid() )
    {
        if( FGameTexture* gt = TexMan.GetGameTexture( tex ) )
        {
            name = gt->GetName();
        }
    }
    if( outName )
    {
        *outName = name;
    }
    if( name.IsEmpty() )
    {
        return SurfKind::Other;
    }

    FString up = name;
    up.ToUpper();

    // Exact wins over prefix, so a single texture can be excepted out of a
    // family rule without reordering the file.
    for( const SurfEntry& e : s_surfExact )
    {
        if( up.Compare( e.name ) == 0 )
        {
            if( outListed )
            {
                *outListed = true;
            }
            return e.kind;
        }
    }
    for( const SurfEntry& p : s_surfPrefix )
    {
        if( up.Len() >= p.name.Len() && up.Left( p.name.Len() ).Compare( p.name ) == 0 )
        {
            if( outListed )
            {
                *outListed = true;
            }
            return p.kind;
        }
    }
    return SurfKind::Other;
}

} // namespace

uint32_t g_sparkCount      = 0;
uint32_t g_sparkFlashCount = 0;

void RT_ClearSparks()
{
    g_sparkCount      = 0;
    g_sparkFlashCount = 0;
}

// ---------------------------------------------------------------------------
// Spawn -- called from playsim (P_LineAttack), so GLOBAL scope, exactly as
// RT_SpawnFluid is. `pos` and `normal` are MAP UNITS and a unit vector; `indir`
// is the direction the shot was travelling.
// ---------------------------------------------------------------------------
void RT_SpawnImpactSparks( const DVector3& pos,
                           const FVector3& normal,
                           const FVector3& indir,
                           sector_t*       sec,
                           FTextureID      hitTexture )
{
    if( !primaryLevel )
    {
        return;
    }

    // THE SURFACE PROBE RUNS EVEN WITH SPARKS OFF, on purpose. It is a surface
    // IDENTIFICATION tool in the same family as `whatsthat` and rt_tex_probe --
    // "what is this wall called and what is it made of" is a question worth
    // answering while labelling, and it should not require the effect being
    // labelled for to be switched on. rt_spark ships off, so gating the probe on
    // it would have made the pin below print nothing, ever.
    if( cvar::rt_spark_surface_debug )
    {
        FString        pName;
        bool           pListed = false;
        const SurfKind pSurf   = SurfaceKindOf( hitTexture, &pName, &pListed );
        const bool     pDebris = ( cvar::rt_spark_debris && pSurf == SurfKind::Concrete );

        // THE SAMPLED COLOUR IS PART OF THE PROBE, because "the tint does not
        // work" has two completely different causes that look identical on
        // screen: the average came back grey (the sampling failed, and
        // AverageTextureColor's fallback is 0x808080), or it came back coloured
        // and the tint maths washed it out. Printing the value separates them
        // without a single guess at a magnitude.
        FString pCol;
        if( SurfThrowsDebris( pSurf ) )
        {
            const uint32_t avg = AverageTextureColor( hitTexture );

            // BOTH ENDS OF THE COLOUR PATH, because the raw mean alone was
            // misleading: it looked plausible (#574D4B is warm) while producing
            // grey chips, and only comparing it against the corrected value
            // shows how little chroma the mean actually carries.
            float ar = ( ( avg >> 16 ) & 0xFF ) / 255.f;
            float ag = ( ( avg >> 8 ) & 0xFF ) / 255.f;
            float ab = ( avg & 0xFF ) / 255.f;
            {
                const float sat  = std::max( 0.f, float{ cvar::rt_spark_debris_sat } );
                const float grey = 0.2126f * ar + 0.7152f * ag + 0.0722f * ab;
                ar = std::clamp( grey + ( ar - grey ) * sat, 0.f, 1.f );
                ag = std::clamp( grey + ( ag - grey ) * sat, 0.f, 1.f );
                ab = std::clamp( grey + ( ab - grey ) * sat, 0.f, 1.f );
                const float lum = 0.2126f * ar + 0.7152f * ag + 0.0722f * ab;
                if( lum > 0.01f )
                {
                    const float tgt = std::clamp(
                        float{ cvar::rt_spark_debris_albedo } * ProfileFor( pSurf ).albedo,
                        0.02f,
                        1.f );
                    const float k = tgt / lum;
                    ar            = std::min( 1.f, ar * k );
                    ag            = std::min( 1.f, ag * k );
                    ab            = std::min( 1.f, ab * k );
                }
            }
            const uint32_t fin = ( uint32_t( ar * 255.f + 0.5f ) << 16 ) |
                                 ( uint32_t( ag * 255.f + 0.5f ) << 8 ) |
                                 uint32_t( ab * 255.f + 0.5f );

            pCol.Format( "  avg=#%06X -> chip=#%06X%s",
                         avg,
                         fin,
                         avg == 0x808080u ? " (avg is the FALLBACK/grey)" : "" );
        }

        Printf( "rt_spark surface: '%s' -> %s%s   (%s)%s\n",
                pName.IsEmpty() ? "?" : pName.GetChars(),
                SurfKindName( pSurf ),
                pListed ? "" : " [UNLISTED]",
                !cvar::rt_spark ? "sparks OFF"
                                : ( pDebris ? "debris" : "sparks" ),
                pCol.GetChars() );
    }

    if( !cvar::rt_spark )
    {
        return;
    }

    s_dbgHits++;

    // THE SURFACE CLASS COMES FROM THE PBR MATERIAL LABELLER.
    // rt/data/spark_surfaces.txt is generated by tools/build_spark_surfaces.py
    // out of the `surface` field in tools/_material_labels/*.json -- the same
    // hand-labelling pass that authors metallicDefault and roughnessDefault, and
    // which was already recording "metal" / "concrete" / "other" while nothing
    // consumed it. So this needs no classification effort of its own; it rides
    // the PBR work.
    //
    // METAL sparks; everything else throws debris -- but only once the labelling
    // is worth trusting. With rt_spark_debris off, every impact sparks, which is
    // the shipped behaviour and does not depend on the table at all.
    FString        texName;
    bool           listed = false;
    const SurfKind surf   = SurfaceKindOf( hitTexture, &texName, &listed );
    const bool     metal  = ( surf == SurfKind::Metal );

    // SPARKS ARE THE DEFAULT; ONLY CONCRETE THROWS DEBRIS.
    //
    // The first rule was the other way round -- metal sparks, everything else is
    // debris -- and it was wrong for the state the data is actually in. The
    // labelling covers 83 textures of 2331, so "everything not metal" means
    // almost the whole game, and one unlabelled map would have turned every
    // surface in it to chips. This rule is OPT-IN: a texture has to be
    // positively labelled `concrete` to behave differently, so metal, other and
    // anything the labeller has not reached all keep the shipped spark, and the
    // effect can only ever improve as the labelling advances.
    const SparkKind kind = ( cvar::rt_spark_debris && SurfThrowsDebris( surf ) )
                               ? SparkKind::Debris
                               : SparkKind::Spark;
    (void)metal;

    // Tally by class. These feed the once-a-second ladder and the `sparks` CCMD,
    // so "what am I actually shooting" is answerable without turning on the
    // per-impact spam.
    if( !listed )
    {
        s_dbgUnlisted++;
        NoteUnlisted( texName );
    }
    else if( surf == SurfKind::Metal )
    {
        s_dbgMetal++;
    }
    else if( surf == SurfKind::Concrete )
    {
        s_dbgConcrete++;
    }
    else
    {
        s_dbgOther++;
    }

    // ASKING FOR DEBRIS WITH NO TABLE IS A MISCONFIGURATION, and it must say so
    // once rather than quietly behaving like a feature that does not work. Only
    // fires when debris is actually switched on, so normal play stays silent.
    if( cvar::rt_spark_debris && !s_surfacesFound )
    {
        static bool s_warned = false;
        if( !s_warned )
        {
            s_warned = true;
            Printf( TEXTCOLOR_ORANGE
                    "rt_spark_debris is ON but '%s' was not found -- nothing can be "
                    "recognised as concrete, so no surface will throw debris.\n"
                    TEXTCOLOR_NORMAL
                    "  Run: python tools/build_spark_surfaces.py   then: spark_surfaces\n",
                    s_surfacesPath.GetChars() );
        }
    }

    // The probe itself printed at the top of this function, before the rt_spark
    // gate, so that it works with the effect switched off. It also prints BEFORE
    // the distance cull below, so a surface can be identified from wherever you
    // happen to be standing.

    const FVector3 at{ float( pos.X ) * ONEGAMEUNIT_IN_METERS,
                       float( pos.Y ) * ONEGAMEUNIT_IN_METERS,
                       float( pos.Z ) * ONEGAMEUNIT_IN_METERS };

    // A SPAWN cull, not a render cull. Eviction is oldest-out while the sparks
    // that matter are the ones in front of your face, so a firefight across the
    // map would otherwise push your own impact out of the pool. Same reasoning
    // as rt_smoke_monster_far.
    {
        const auto&    vp = r_viewpoint;
        const FVector3 eye{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                            float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                            float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };
        const float    far_m = std::max( 0.f, float{ cvar::rt_spark_far } );
        if( ( at - eye ).LengthSquared() > far_m * far_m )
        {
            s_dbgRejected++;
            return;
        }
    }

    // The direction the shot REFLECTS off the surface. This is what P_SpawnPuff
    // cannot give us and P_LineAttack can.
    FVector3 refl = indir - normal * ( 2.f * ( indir | normal ) );
    if( refl.LengthSquared() < 1e-8f )
    {
        refl = normal;
    }
    refl.MakeUnit();

    const bool dbr = ( kind == SparkKind::Debris );

    // The class row. Multipliers on the debris cvars, so a class states only how
    // it differs -- see RT_DEBRIS_PROFILES. Sparks ignore it entirely.
    const DebrisProfile& prof = ProfileFor( surf );

    const uint32_t want =
        dbr ? uint32_t( std::max( 0, int( std::lround(
                  float( std::max( 0, int{ cvar::rt_spark_debris_count } ) ) * prof.count ) ) ) )
            : uint32_t( std::max( 0, int{ cvar::rt_spark_count } ) );
    const uint32_t cap =
        std::min( RT_SPARK_HARDMAX, uint32_t( std::max( 0, int{ cvar::rt_spark_max } ) ) );
    if( want == 0 || cap == 0 )
    {
        return;
    }

    // Spread and the cone are shared: a chip, a droplet and a spark all come off
    // a wall the same way. Everything else about the shape is the class row.
    const float speed =
        std::max( 0.f,
                  dbr ? float{ cvar::rt_spark_debris_speed } * prof.speed
                      : float{ cvar::rt_spark_speed } );
    const float spread = std::clamp( float{ cvar::rt_spark_spread }, 0.f, 90.f );
    const float life =
        std::max( 0.05f,
                  dbr ? float{ cvar::rt_spark_debris_life } * prof.life
                      : float{ cvar::rt_spark_life } );
    const float size =
        std::max( 0.002f,
                  dbr ? float{ cvar::rt_spark_debris_size } * prof.size
                      : float{ cvar::rt_spark_size } );

    // An orthonormal basis around the reflected direction, so the cone can be
    // built without a trig call per spark.
    FVector3 tangent =
        std::abs( refl.Z ) < 0.9f ? ( refl ^ FVector3{ 0, 0, 1 } ) : ( refl ^ FVector3{ 1, 0, 0 } );
    tangent.MakeUnit();
    const FVector3 bitangent = refl ^ tangent;

    // The cone half-angle as a radius on the unit disc perpendicular to refl.
    const float coneR = std::tan( to_rad( spread ) );

    for( uint32_t i = 0; i < want; i++ )
    {
        uint32_t slot;
        if( g_sparkCount < cap )
        {
            slot = g_sparkCount++;
        }
        else
        {
            // OLDEST OF ITS OWN KIND, not oldest overall, and the difference is
            // load-bearing once sparks and debris have very different lifetimes.
            //
            // Sparks live ~5 s and debris ~20 s in one shared pool. A plain
            // oldest-out rule therefore evicts DEBRIS almost every time -- it is
            // reliably the older population -- so chips would be culled within a
            // second or two of spawning and their 20 s life would be a number
            // that never happened. Evicting within the kind bounds each
            // population by its own spawn rate instead, so neither can starve
            // the other. Same shape as the ambient-first rule in
            // RT_SpawnSmokePuffs, and for the same reason.
            slot                 = UINT32_MAX;
            uint32_t oldestAny   = 0;
            for( uint32_t j = 0; j < g_sparkCount; j++ )
            {
                if( s_sparks[ j ].age > s_sparks[ oldestAny ].age )
                {
                    oldestAny = j;
                }
                if( s_sparks[ j ].kind == kind &&
                    ( slot == UINT32_MAX || s_sparks[ j ].age > s_sparks[ slot ].age ) )
                {
                    slot = j;
                }
            }
            // None of this kind alive yet: fall back to the oldest of all, so a
            // first chip can still be born into a pool full of sparks.
            if( slot == UINT32_MAX )
            {
                slot = oldestAny;
            }
        }

        // A disc sample, sqrt-weighted so the cone is uniform rather than
        // clustered on the axis.
        const float ang = rnd01() * 2.f * rt_pi();
        const float rad = std::sqrt( rnd01() ) * coneR;

        FVector3 dir = refl + ( tangent * std::cos( ang ) + bitangent * std::sin( ang ) ) * rad;
        dir.MakeUnit();

        Spark& sp = s_sparks[ slot ];

        // BORN OFF THE SURFACE, NOT ON IT. A spark spawned exactly on the face
        // it came from makes tier 2's first trace report an immediate self-hit,
        // and the spark sticks to the wall it should be leaving.
        // RT_SpawnBlood_Thing offsets for the same reason.
        sp.pos     = at + normal * 0.02f;
        sp.vel     = dir * ( speed * ( 0.55f + 0.45f * rnd01() ) );
        sp.age     = 0.f;
        sp.life    = life * ( 0.7f + 0.6f * rnd01() );
        // Debris spreads much wider than a spark: 0.4x..1.9x against 0.7x..1.3x.
        // Rubble is visibly assorted; sparks off one impact are not.
        sp.size = size * ( dbr ? ( 0.4f + 1.5f * rnd01() ) : ( 0.7f + 0.6f * rnd01() ) );
        sp.sec     = sec;
        sp.settled = false;
        sp.kind    = kind;
        sp.surf    = surf;
        // SCATTERED NORMALS, and this is the fix for debris going white.
        //
        // Every chip used to carry the SURFACE normal it came off. The
        // flashlight is at the camera and chips fly toward the camera, so
        // N.L was near 1 for every chip at once -- maximum irradiance,
        // identically, on all of them. Reported as "in path traced view,
        // especially with flashlight, they become white". Real rubble has its
        // normals pointing everywhere, so only some fragments catch a light.
        //
        // Still BIASED toward the surface normal rather than fully random: a
        // chip did just come off that wall, and a uniform sphere of normals
        // makes half of them face into it.
        if( dbr )
        {
            const FVector3 rnd3{ rnd11(), rnd11(), rnd11() };
            FVector3       n = normal * 0.35f + rnd3 * 0.65f;
            if( n.LengthSquared() < 1e-6f )
            {
                n = normal;
            }
            n.MakeUnit();
            sp.nrm = n;
        }
        else
        {
            sp.nrm = normal;
        }
        // Rubble randomness. Debris also gets a much wider SIZE spread than a
        // spark (below): chips off a wall come in obviously different sizes,
        // and uniform ones read as manufactured.
        sp.phase  = rnd01() * 2.f * rt_pi();
        sp.spin   = rnd11() * 3.2f * ( dbr ? prof.spin : 1.f );
        // ASPECT IS THE CLASS, not a multiplier: a splinter is 0.12-0.35 and a
        // crumb is 0.7-1.4, and no amount of scaling one produces the other.
        sp.aspect = dbr ? ( prof.aspectLo + rnd01() * ( prof.aspectHi - prof.aspectLo ) )
                        : ( 0.55f + rnd01() * 1.1f );
        // Resolved here, once per impact, because the lookup decodes a bitmap on
        // first sight of a texture. Sparks never read it.
        sp.baseRgb = dbr ? AverageTextureColor( hitTexture ) : 0u;
        // The spark's identity, for its glow light's uniqueID. Monotonic and
        // never reused, so a light can never be inherited by a different spark.
        static uint32_t s_nextSid = 1;
        sp.sid                    = s_nextSid++;

        s_dbgSpawned++;
    }

    // The IMPACT flash: one per impact, never one per spark, and separate from
    // the flying sparks' own lights (rt_spark_glow) which ride the particles.
    // Driven from the impact POINT, which does not move -- a light chasing a
    // grid-snapped spark reads as the two coming apart.
    //
    // Debris does not flash. A chip of concrete is not hot, and a flash on a
    // stone wall is the tell that the classification is wrong.
    if( cvar::rt_spark_light && kind == SparkKind::Spark )
    {
        const uint32_t fcap =
            std::min( RT_SPARK_FLASH_MAX,
                      uint32_t( std::max( 0, int{ cvar::rt_spark_light_max } ) ) );
        if( fcap > 0 )
        {
            uint32_t fslot;
            if( g_sparkFlashCount < fcap )
            {
                fslot = g_sparkFlashCount++;
            }
            else
            {
                fslot = 0;
                for( uint32_t j = 1; j < g_sparkFlashCount; j++ )
                {
                    if( s_flashes[ j ].age > s_flashes[ fslot ].age )
                    {
                        fslot = j;
                    }
                }
            }

            SparkFlash& fl = s_flashes[ fslot ];
            fl.pos         = at + normal * 0.06f;
            fl.age         = 0.f;
            fl.life        = std::max( 0.02f, float{ cvar::rt_spark_light_life } );
        }
    }
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------
namespace
{

// Tier 2. Only ever called when a step LEFT its sector, which is false for the
// large majority of steps -- that test is free because tier 1 already fetched
// the sector. Returns true if the spark was stopped by a wall.
//
// The trace flags MUST NOT include TRACE_Impact or TRACE_PCross: those trigger
// SPAC_IMPACT and SPAC_PCROSS line specials, and renderer particles firing map
// specials would be a gameplay change and a netgame desync in a system the
// player cannot see. With them unset and no callback, Trace() is a pure query.
// ActorMask is empty so sparks pass through monsters; TRACE_NoSky so a spark
// that reaches sky simply dies.
bool SparkHitWall( Spark& sp, const FVector3& from, const FVector3& to, float bounce, float fric )
{
    FVector3 delta = to - from;
    const float len = delta.Length();
    if( len < 1e-5f )
    {
        return false;
    }
    delta /= len;

    const DVector3 start{ double( from.X ) / double{ ONEGAMEUNIT_IN_METERS },
                          double( from.Y ) / double{ ONEGAMEUNIT_IN_METERS },
                          double( from.Z ) / double{ ONEGAMEUNIT_IN_METERS } };
    const DVector3 dir{ double( delta.X ), double( delta.Y ), double( delta.Z ) };

    FTraceResults res{};
    if( !Trace( start,
                sp.sec,
                dir,
                double( len ) / double{ ONEGAMEUNIT_IN_METERS },
                ActorFlags::FromInt( 0 ),
                ML_BLOCKEVERYTHING,
                nullptr,
                res,
                TRACE_NoSky ) )
    {
        return false;
    }

    if( res.HitType == TRACE_HasHitSky )
    {
        // Gone. Kill it rather than bouncing it off the sky plane.
        sp.age = sp.life;
        return true;
    }
    if( res.HitType != TRACE_HitWall || res.Line == nullptr )
    {
        return false;
    }

    // The wall normal, from the linedef delta and which side was hit. A linedef
    // runs v1->v2 and its FRONT side is to the right of that, so the outward
    // normal of the side we came from is the perpendicular, flipped when the
    // trace reports the back side.
    const double dx = res.Line->Delta().X;
    const double dy = res.Line->Delta().Y;
    FVector3     n{ float( dy ), float( -dx ), 0.f };
    if( n.LengthSquared() < 1e-8f )
    {
        return false;
    }
    n.MakeUnit();
    if( res.Side != 0 )
    {
        n = -n;
    }
    // Whichever way the geometry is wound, the normal must oppose the travel.
    if( ( delta | n ) > 0.f )
    {
        n = -n;
    }

    sp.pos = FVector3{ float( res.HitPos.X ) * ONEGAMEUNIT_IN_METERS,
                       float( res.HitPos.Y ) * ONEGAMEUNIT_IN_METERS,
                       float( res.HitPos.Z ) * ONEGAMEUNIT_IN_METERS } +
             n * 0.01f;

    const float vn = sp.vel | n;
    FVector3    vt = sp.vel - n * vn;
    sp.vel         = vt * ( 1.f - fric ) - n * ( vn * bounce );

    sp.sec = res.Sector ? res.Sector : sp.sec;
    return true;
}

} // namespace

void RT_UpdateSparks()
{
    if( !primaryLevel )
    {
        RT_ClearSparks();
        return;
    }

    if( !cvar::rt_spark )
    {
        g_sparkCount      = 0;
        g_sparkFlashCount = 0;
        s_lastTic         = primaryLevel->maptime;
        return;
    }

    const int tic = primaryLevel->maptime;

    int steps = s_lastTic < 0 ? 0 : tic - s_lastTic;
    // A backwards or enormous jump is a level change, a load or a warp. Do not
    // integrate across it: a spark advanced by a thousand tics ends up somewhere
    // arbitrary, and tier 2 would trace the whole way there.
    if( steps < 0 || steps > TICRATE )
    {
        RT_ClearSparks();
        s_lastTic = tic;
        return;
    }
    s_lastTic = tic;

    const float dt      = 1.f / float( TICRATE );
    // Resolved per KIND inside the loop below: debris is heavier and deader than
    // a spark, and those two numbers are most of what separates a chip of wall
    // from a hot fragment before the colour is even read.
    const float gravity = std::max( 0.f, float{ cvar::rt_spark_gravity } );
    const float gravityD = std::max( 0.f, float{ cvar::rt_spark_debris_gravity } );
    const float bounceD  = std::clamp( float{ cvar::rt_spark_debris_bounce }, 0.f, 1.f );
    const float drag    = std::max( 0.f, float{ cvar::rt_spark_drag } );
    const bool  collide = cvar::rt_spark_collide;
    const float bounce  = std::clamp( float{ cvar::rt_spark_bounce }, 0.f, 1.f );
    const float fric    = std::clamp( float{ cvar::rt_spark_friction }, 0.f, 1.f );
    const float rest    = std::max( 0.f, float{ cvar::rt_spark_rest } );
    const int   traceCap = std::max( 0, int{ cvar::rt_spark_trace_max } );

    for( int step = 0; step < steps; step++ )
    {
        int tracesLeft = traceCap;

        for( uint32_t i = 0; i < g_sparkCount; )
        {
            Spark& sp = s_sparks[ i ];

            sp.age += dt;
            if( sp.age >= sp.life )
            {
                s_sparks[ i ] = s_sparks[ --g_sparkCount ];
                continue;
            }

            // A settled spark lies where it fell, glowing down the palette ramp.
            // Skipping the integration is not just an optimisation: it is what
            // stops the sub-step jitter a nearly-stopped bouncer would show.
            if( sp.settled )
            {
                i++;
                continue;
            }

            const FVector3 from = sp.pos;

            // Per CLASS, not merely per kind: dirt drops dead where a wood
            // splinter floats down, and that difference is most of what tells
            // them apart in motion.
            const bool           isDbr = ( sp.kind == SparkKind::Debris );
            const DebrisProfile& pr    = ProfileFor( sp.surf );
            const float          kGrav = isDbr ? gravityD * pr.gravity : gravity;
            const float          kBnce =
                isDbr ? std::clamp( bounceD * pr.bounce, 0.f, 1.f ) : bounce;
            const float kFric =
                isDbr ? std::clamp( fric * pr.friction, 0.f, 1.f ) : fric;

            sp.vel.Z -= kGrav * dt;
            sp.vel *= std::max( 0.f, 1.f - drag * dt );
            sp.pos += sp.vel * dt;

            if( collide )
            {
                const double mx = double( sp.pos.X ) / double{ ONEGAMEUNIT_IN_METERS };
                const double my = double( sp.pos.Y ) / double{ ONEGAMEUNIT_IN_METERS };

                sector_t* nowSec = primaryLevel->PointInSector( mx, my );

                // TIER 2, and it is gated on the sector CHANGING rather than run
                // every step. PointInSector was needed for tier 1 anyway, so the
                // test costs nothing, and it is false for most steps.
                if( nowSec != sp.sec && sp.sec != nullptr && tracesLeft > 0 )
                {
                    tracesLeft--;
                    s_dbgTraces++;
                    if( SparkHitWall( sp, from, sp.pos, kBnce, kFric ) )
                    {
                        nowSec = sp.sec;
                    }
                    else
                    {
                        sp.sec = nowSec;
                    }
                }
                else if( nowSec != nullptr )
                {
                    sp.sec = nowSec;
                }

                // TIER 1: floor and ceiling, every step. Smoke's mechanism
                // (rt_smoke.cpp) with one difference -- where a puff CLAMPS and
                // loses its vertical velocity so it spreads, a spark REFLECTS.
                // A spark is a point, so smoke's radius term and its crawlspace
                // guard both drop out.
                if( sector_t* sec = sp.sec )
                {
                    const double px = double( sp.pos.X ) / double{ ONEGAMEUNIT_IN_METERS };
                    const double py = double( sp.pos.Y ) / double{ ONEGAMEUNIT_IN_METERS };

                    const float zf = float( sec->floorplane.ZatPoint( px, py ) ) *
                                     ONEGAMEUNIT_IN_METERS;
                    const float zc = float( sec->ceilingplane.ZatPoint( px, py ) ) *
                                     ONEGAMEUNIT_IN_METERS;

                    if( zc > zf )
                    {
                        if( sp.pos.Z < zf )
                        {
                            sp.pos.Z = zf;
                            if( sp.vel.Z < 0.f )
                            {
                                sp.vel.Z = -sp.vel.Z * kBnce;
                                sp.vel.X *= ( 1.f - kFric );
                                sp.vel.Y *= ( 1.f - kFric );
                            }
                        }
                        else if( sp.pos.Z > zc )
                        {
                            sp.pos.Z = zc;
                            if( sp.vel.Z > 0.f )
                            {
                                sp.vel.Z = -sp.vel.Z * kBnce;
                                sp.vel.X *= ( 1.f - kFric );
                                sp.vel.Y *= ( 1.f - kFric );
                            }
                        }
                    }

                    // TIER 3: settle. Only on the floor -- a spark drifting
                    // slowly in mid-air is still falling and must not freeze
                    // there.
                    if( sp.vel.LengthSquared() < rest * rest && sp.pos.Z <= zf + 0.02f )
                    {
                        sp.pos.Z   = zf;
                        sp.vel     = FVector3{ 0, 0, 0 };
                        sp.settled = true;
                        // Freeze the tumble where it landed. Baking the current
                        // angle into `phase` and clearing `spin` keeps the draw
                        // expression uniform, so a chip does not snap back to
                        // its birth orientation the instant it comes to rest --
                        // which, at a 20 s life, would be the most visible thing
                        // debris does.
                        sp.phase += sp.spin * sp.age;
                        sp.spin = 0.f;
                    }
                }
            }

            i++;
        }
    }

    for( uint32_t i = 0; i < g_sparkFlashCount; )
    {
        SparkFlash& fl = s_flashes[ i ];
        fl.age += dt * float( steps );
        if( fl.age >= fl.life )
        {
            s_flashes[ i ] = s_flashes[ --g_sparkFlashCount ];
            continue;
        }
        i++;
    }
}

// ---------------------------------------------------------------------------
// Draw -- TWO batched primitives, and they differ by more than colour.
//
//   SPARKS  additive TRANSLUCENT, i.e. a RASTERIZED overlay. Self-luminous, and
//           correctly independent of the room's light. Casts nothing (the
//           analytic lights below are the traced half).
//   DEBRIS  opaque, no flags, primitive alpha 1 -- RTGL1's rule for entering the
//           ACCELERATION STRUCTURE. So a chip is real ray-traced geometry that
//           the path tracer lights, shadows and reflects, which is the only way
//           it can actually respond to the room.
//
// They cannot share a batch: the flags and the blend mode are per primitive, and
// the whole difference between a spark and a chip of wall is that one of them
// glows and the other is lit.
// ---------------------------------------------------------------------------
namespace
{

struct QuadBatch
{
    std::vector< RgPrimitiveVertex > verts;
    std::vector< uint32_t >          idx;
};

// CONTACT OCCLUSION UNDER SETTLED DEBRIS.
//
// Straight from docs/sprite-shadows-and-ao.md 2: a chip lying on a floor has the
// same problem a dropped weapon does -- with no contact term it reads as
// hovering, and a cast shadow cannot supply one because a flat fragment lit from
// a shallow angle throws almost nothing. A blob answers "is something touching
// this floor", which has the same answer from every light direction.
//
// Everything about the technique is that document's, and the three rules it
// bought the hard way are all load-bearing here too:
//   - WORLD-SPACE VERTS, IDENTITY TRANSFORM. RsDecal.vert writes
//     outWorldPos = position untransformed while transforming gl_Position, so a
//     transform makes the quad rasterize in the right place and then discard
//     100% of its fragments -- invisible, silently, with nothing logged.
//   - A SINGLE FAN per blob, centre at alpha and rim at 0. A high-alpha ring
//     creases along every quad diagonal; more segments does not fix it.
//   - emissive = 0, or the decal shader falls back to ldrEmis = albedo and the
//     blob GLOWS.
QuadBatch s_batchDebrisAo;

QuadBatch s_batchSpark;

// DEBRIS IS BATCHED BY COLOUR, and that is forced by how RTGL1 shades traced
// geometry rather than by anything about the effect.
//
// HitInfo.inl, for a surface with no albedo texture:
//
//     // if no albedo textures, use primary color
//     dst = mix( unpackUintColor( layerColors[0] ).rgb, dst, float( hasAnyAlbedoTexture ) );
//
// `layerColors[0]` is the PER-PRIMITIVE colour (RgMeshPrimitiveInfo::color).
// The per-VERTEX colour is stored -- ShVertex has a `color` field -- but the
// traced albedo path never reads it. So while debris was rasterized the vertex
// colours worked, and the moment it became traced geometry every chip took
// prim.color, which was RG_PACKED_COLOR_WHITE. Chips were white BY CONSTRUCTION,
// and no value of rt_spark_debris_albedo or _tint could move them; the arm that
// proved it set albedo to 0.02 and they stayed white.
//
// A primitive therefore carries exactly one albedo, so per-particle colour needs
// either one primitive per particle (thousands of uploads and BLAS entries) or
// particles grouped by colour. Grouping is far cheaper and the quantisation is
// invisible: chip colour comes from a handful of wall textures and a slow age
// ramp, so a room in play uses a few buckets out of the cap.
//
// The key carries the CLASS as well as the colour, so a class that later names a
// sprite in its profile still gets its own primitive and its own material.
constexpr int RT_DEBRIS_BUCKETS = 32;

struct DebrisBucket
{
    uint32_t  key;   // class << 12 | quantised rgb
    uint32_t  rgb;   // the representative colour uploaded as prim.color
    SurfKind  kind;
    QuadBatch batch;
};

DebrisBucket s_debrisBuckets[ RT_DEBRIS_BUCKETS ];
int          s_debrisBucketCount = 0;

// 4 bits a channel. Finer buckets do not buy anything the eye can see here and
// only raise the chance of hitting the cap in a room with many wall textures.
uint32_t DebrisColorKey( SurfKind k, float r, float g, float b )
{
    const uint32_t qr = uint32_t( std::clamp( r, 0.f, 1.f ) * 15.f + 0.5f );
    const uint32_t qg = uint32_t( std::clamp( g, 0.f, 1.f ) * 15.f + 0.5f );
    const uint32_t qb = uint32_t( std::clamp( b, 0.f, 1.f ) * 15.f + 0.5f );
    return ( uint32_t( k ) << 12 ) | ( qr << 8 ) | ( qg << 4 ) | qb;
}

QuadBatch& DebrisBucketFor( SurfKind k, float r, float g, float b )
{
    const uint32_t key = DebrisColorKey( k, r, g, b );

    for( int i = 0; i < s_debrisBucketCount; i++ )
    {
        if( s_debrisBuckets[ i ].key == key )
        {
            return s_debrisBuckets[ i ].batch;
        }
    }

    if( s_debrisBucketCount < RT_DEBRIS_BUCKETS )
    {
        DebrisBucket& nb = s_debrisBuckets[ s_debrisBucketCount++ ];
        nb.key           = key;
        nb.kind          = k;
        nb.rgb           = ( uint32_t( std::clamp( r, 0.f, 1.f ) * 255.f + 0.5f ) << 16 ) |
                 ( uint32_t( std::clamp( g, 0.f, 1.f ) * 255.f + 0.5f ) << 8 ) |
                 uint32_t( std::clamp( b, 0.f, 1.f ) * 255.f + 0.5f );
        return nb.batch;
    }

    // Cap reached: fold into the first bucket rather than dropping the chip. A
    // slightly wrong colour is a far smaller artefact than debris vanishing,
    // and the cap is generous enough that this should not be reachable in play.
    return s_debrisBuckets[ 0 ].batch;
}

void UploadBatch( const QuadBatch&  b,
                  uint64_t          meshId,
                  bool              additive,
                  const char*       texName,
                  RgColor4DPacked32 primColor )
{
    if( b.verts.empty() )
    {
        return;
    }

    // WORLD-SPACE VERTICES, IDENTITY TRANSFORM. Not for the decal shader's sake
    // -- this is not a decal -- but because the quads are already positioned in
    // world space and a transform would move them twice.
    auto mesh = RgMeshInfo{
        .sType          = RG_STRUCTURE_TYPE_MESH_INFO,
        .pNext          = nullptr,
        .flags          = 0,
        .uniqueObjectID = meshId,
        // No mesh name, or RTGL1 hunts for an rt/replace/*.gltf substitute and
        // could swap a model in for the whole batch.
        .pMeshName = nullptr,
        .transform =
            RgTransform{ {
                { 1, 0, 0, 0 },
                { 0, 1, 0, 0 },
                { 0, 0, 1, 0 },
            } },
        .isExportable  = false,
        .animationTime = 0.f,
        // 1.0 IS THE DOCUMENTED DEFAULT (RgMeshInfo, "Default: 1.0"). This was
        // 0, which is a real value meaning "take no local light" -- harmless for
        // the additive spark batch, which is unlit either way, but wrong for
        // debris the moment it became traced geometry.
        .localLightsIntensity = 1.f,
    };

    // EXPLICIT PBR FOR THE TRACED BATCH, and it is worth being clear about what
    // this does and does not fix.
    //
    // Asked from play, after debris still blew out under the flashlight: "should
    // we give them light absorption property? like PBR?" Right instinct, wrong
    // lever -- and the header says why. RgMeshPrimitivePBREXT documents its
    // defaults as roughness 1.0 and metallic 0.0 when no roughness-metallic
    // texture is present, which debris has none of. So it was ALREADY the
    // roughest, least shiny dielectric available, and a rough diffuse surface
    // returns albedo * E / pi whatever its roughness. Nothing here can reduce a
    // whiteout; that is albedo against irradiance, and rt_spark_debris_albedo is
    // the only surface-side term in it.
    //
    // Set anyway, because "no material at all" leaves what ASManager falls back
    // to unstated, and an unstated default is exactly the kind of thing that has
    // cost this project days. Stating it costs one struct and removes the doubt.
    auto pbr = RgMeshPrimitivePBREXT{
        .sType           = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_PBR_EXT,
        .pNext           = nullptr,
        .metallicDefault = std::clamp( float{ cvar::rt_spark_debris_metal }, 0.f, 1.f ),
        .roughnessDefault = std::clamp( float{ cvar::rt_spark_debris_rough }, 0.f, 1.f ),
    };

    auto prim = RgMeshPrimitiveInfo{
        .sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
        // Only the traced batch takes it: the additive spark overlay is not
        // shaded at all, so a PBR block on it would be noise.
        .pNext = additive ? nullptr : &pbr,
        // THE EMISSIVE BELOW IS WHAT PICKS THE BLEND MODE, and that is the entire
        // reason these are two batches rather than one.
        // RasterizedDataCollector::ToPipelineState turns a TRANSLUCENT primitive
        // with emissive > 0 into ADDITIVE (SRC_ALPHA, ONE); with emissive == 0 it
        // stays an ordinary alpha blend (SRC_ALPHA, ONE_MINUS_SRC_ALPHA).
        //
        // For a translucent SPRITE the additive promotion is the bug behind
        // rt_spectre_alpha reading as inert; for a hot spark it is precisely the
        // blend wanted, which is why sparks need no art and no alpha tuning.
        //
        // The cost is real and applies to SPARKS ONLY: a translucent primitive
        // is a rasterized overlay kept out of the acceleration structure, so the
        // spark batch appears in no reflection and casts no GI. The lights in
        // RT_UploadSparkLights are its traced half. DEBRIS pays none of that --
        // it is opaque and traced, which is exactly why it takes the room's
        // light and sparks do not.
        // ADDITIVE (sparks) -> TRANSLUCENT, a rasterized overlay.
        // OPAQUE (debris)    -> no flags at all, which with the alpha-1 colour
        //                       below is RTGL1's rule for entering the
        //                       ACCELERATION STRUCTURE -- so debris is real
        //                       ray-traced geometry, lit by the scene, casting
        //                       and receiving shadows, visible in reflections.
        .flags                = additive ? RG_MESH_PRIMITIVE_TRANSLUCENT
                                         : RgMeshPrimitiveFlags( 0 ),
        .primitiveIndexInMesh = 0,
        .pVertices            = b.verts.data(),
        .vertexCount          = uint32_t( b.verts.size() ),
        .pIndices             = b.idx.data(),
        .indexCount           = uint32_t( b.idx.size() ),
        // No texture: RTGL1 samples its 1x1 white, so the colour is entirely the
        // vertex colour. That is what makes a spark one flat pixel.
        .pTextureName = texName,
        .textureFrame = 0,
        // THE ALBEDO, for the traced batch -- see the note above the buckets.
        // Alpha must stay 1: below 0.98 RTGL1 demotes the primitive to the
        // rasterized overlay and it goes fullbright again.
        .color        = primColor,
        .emissive     = additive ? 1.f : 0.f,
        .classicLight = 1.f,
    };

    RgResult r = rt.rgUploadMeshPrimitive( &mesh, &prim );
    RG_CHECK( r );
}

// ONE PRIMITIVE PER BLOB, and that is not an arbitrary choice -- it is the only
// structural difference between this and the sprite AO in rt_draw.cpp, which
// ships and is correct.
//
// The first version batched every blob into a single decal primitive. It is
// geometrically sound -- no triangle spans two blobs, and each fan interpolates
// only its own vertices -- and it still produced AO "lines" reaching away from
// the chips, through a distance cull that ruled out the documented 5 cm
// grazing-floor limit. Rather than keep reasoning about what a batched decal
// means to RTGL1's rasterizer, this matches the shape of the implementation that
// is known to work: one fan, one primitive, one uniqueObjectID, exactly as
// docs/sprite-shadows-and-ao.md describes.
//
// The cost is bounded by rt_spark_debris_ao_max rather than by the pool, because
// only SETTLED chips get a blob and a decal upload is not free.
void UploadAoBlob( const QuadBatch& b, uint64_t meshId )
{
    if( b.verts.empty() )
    {
        return;
    }

    auto mesh = RgMeshInfo{
        .sType          = RG_STRUCTURE_TYPE_MESH_INFO,
        .pNext          = nullptr,
        .flags          = 0,
        .uniqueObjectID = meshId,
        .pMeshName      = nullptr,
        // IDENTITY -- see the note above the batch.
        .transform =
            RgTransform{ {
                { 1, 0, 0, 0 },
                { 0, 1, 0, 0 },
                { 0, 0, 1, 0 },
            } },
        .isExportable         = false,
        .animationTime        = 0.f,
        .localLightsIntensity = 0.f,
    };

    auto prim = RgMeshPrimitiveInfo{
        .sType                = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
        .pNext                = nullptr,
        .flags                = RG_MESH_PRIMITIVE_DECAL,
        .primitiveIndexInMesh = 0,
        .pVertices            = b.verts.data(),
        .vertexCount          = uint32_t( b.verts.size() ),
        .pIndices             = b.idx.data(),
        .indexCount           = uint32_t( b.idx.size() ),
        // No texture: the falloff is vertex-colour interpolation, so this ships
        // no art and touches no material. RTGL1 samples its 1x1 white.
        .pTextureName = nullptr,
        .textureFrame = 0,
        .color        = RG_PACKED_COLOR_WHITE,
        // MUST be 0 -- see the note above the batch.
        .emissive     = 0.f,
        .classicLight = 1.f,
    };

    RgResult r = rt.rgUploadMeshPrimitive( &mesh, &prim );
    RG_CHECK( r );
}

} // namespace

void RT_DrawSparks()
{
    s_dbgQuads = 0;

    if( !cvar::rt_spark || g_sparkCount == 0 || !primaryLevel )
    {
        return;
    }

    // A camera-facing basis, built once for both batches.
    const auto&  vp    = r_viewpoint;
    const double yaw   = vp.Angles.Yaw.Radians();
    const double pitch = vp.Angles.Pitch.Radians();

    const FVector3 fwd{ float( std::cos( yaw ) * std::cos( pitch ) ),
                        float( std::sin( yaw ) * std::cos( pitch ) ),
                        float( -std::sin( pitch ) ) };

    FVector3 right = fwd ^ FVector3{ 0, 0, 1 };
    if( right.LengthSquared() < 1e-6f )
    {
        right = FVector3{ 1, 0, 0 };
    }
    right.MakeUnit();
    FVector3 up = right ^ fwd;
    up.MakeUnit();

    // THE TWO STYLES. Same particles, same palette, same sizes -- only how they
    // are quantized and shaped. Pixelated snaps to a world grid and steps
    // through seven hard palette entries; realistic interpolates and lets a
    // spark stretch along its own velocity. See rt_spark_style.
    const bool  pixel  = ( int{ cvar::rt_spark_style } == 0 );
    const float grid   = pixel ? std::max( 0.f, float{ cvar::rt_spark_grid } ) : 0.f;
    const float streak = std::max( 1.f, float{ cvar::rt_spark_streak } );
    const float bright = std::max( 0.f, float{ cvar::rt_spark_bright } );
    const float aoStrength =
        std::clamp( float{ cvar::rt_spark_debris_ao_strength }, 0.f, 1.f );
    const float aoFar = std::max( 0.f, float{ cvar::rt_spark_debris_ao_dist } );
    const int   aoMax = std::max( 0, int{ cvar::rt_spark_debris_ao_max } );
    int         aoEmitted = 0;
    s_dbgAo               = 0;

    const FVector3 eyeM{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                         float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                         float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };

    s_batchSpark.verts.clear();
    s_batchSpark.idx.clear();
    s_batchDebrisAo.verts.clear();
    s_batchDebrisAo.idx.clear();
    for( int i = 0; i < s_debrisBucketCount; i++ )
    {
        s_debrisBuckets[ i ].batch.verts.clear();
        s_debrisBuckets[ i ].batch.idx.clear();
    }
    s_debrisBucketCount = 0;

    const RgNormalPacked32 nrm = rt.rgUtilPackNormal( -fwd.X, -fwd.Y, -fwd.Z );

    for( uint32_t i = 0; i < g_sparkCount; i++ )
    {
        const Spark& sp = s_sparks[ i ];

        const bool           isDbr = ( sp.kind == SparkKind::Debris );
        const DebrisProfile& pr    = ProfileFor( sp.surf );

        const uint32_t* ramp  = isDbr ? pr.ramp : RT_SPARK_RAMP;
        const int       rampN = isDbr ? pr.rampN : RT_SPARK_RAMP_N;

        const float t = std::clamp( sp.age / std::max( 1e-4f, sp.life ), 0.f, 1.f );

        float r, g, b;
        if( pixel )
        {
            // NOT INTERPOLATED. A blend between two entries would put colours on
            // screen that are not in the sprite's palette, which is exactly the
            // smooth look this mode exists to avoid.
            const int      ci  = std::min( rampN - 1, int( t * float( rampN ) ) );
            const uint32_t rgb = ramp[ ci ];
            r                  = ( ( rgb >> 16 ) & 0xFF ) / 255.f;
            g                  = ( ( rgb >> 8 ) & 0xFF ) / 255.f;
            b                  = ( rgb & 0xFF ) / 255.f;
        }
        else
        {
            // REALISTIC: the same seven colours, blended. The ramp is unchanged
            // on purpose -- this is the Doom 64 palette drawn smoothly, not a
            // different palette. "Like the base game" means the colours and the
            // mood; only the quantization is being relaxed.
            const float f  = t * float( rampN - 1 );
            const int   i0 = std::clamp( int( f ), 0, rampN - 1 );
            const int   i1 = std::min( i0 + 1, rampN - 1 );
            const float fr = f - float( i0 );

            const uint32_t c0 = ramp[ i0 ];
            const uint32_t c1 = ramp[ i1 ];

            auto l_lerp = [ & ]( int shift ) {
                const float a0 = ( ( c0 >> shift ) & 0xFF ) / 255.f;
                const float a1 = ( ( c1 >> shift ) & 0xFF ) / 255.f;
                return a0 + ( a1 - a0 ) * fr;
            };
            r = l_lerp( 16 );
            g = l_lerp( 8 );
            b = l_lerp( 0 );
        }

        // A CHIP TAKES THE COLOUR OF THE WALL IT CAME OFF.
        //
        // The built-in ramp is kept as the AGE CURVE rather than thrown away:
        // the texture average supplies the hue, and the ramp supplies how much
        // that hue darkens over the chip's life. Replacing the ramp outright
        // would give every chip one flat colour for 20 seconds; using only the
        // ramp gives grey rubble off a rust-red wall. This is the same "keep the
        // art, add shading on top" rule the material work follows.
        //
        // NORMALISED TO A TARGET ALBEDO, not used raw. A Doom wall texture
        // averages wherever its art happens to sit -- some are near-white -- and
        // an albedo of 0.8 is the mistake that made the first debris ramps read
        // as "too bright colors". rt_spark_debris_albedo pins the LUMINANCE and
        // lets the texture supply only the hue, so a pale wall and a dark one
        // produce chips of the same believable darkness in different colours.
        if( isDbr && sp.baseRgb != 0u )
        {
            // The class scales both, and fluid pushes tint past 1 so it CLAMPS at
            // full texture colour -- see its row.
            const float tint =
                std::clamp( float{ cvar::rt_spark_debris_tint } * pr.tint, 0.f, 1.f );
            if( tint > 0.f )
            {
                float tr = ( ( sp.baseRgb >> 16 ) & 0xFF ) / 255.f;
                float tg = ( ( sp.baseRgb >> 8 ) & 0xFF ) / 255.f;
                float tb = ( sp.baseRgb & 0xFF ) / 255.f;

                // A WHOLE-TEXTURE MEAN IS A CHROMA KILLER, and this is the step
                // that makes the tint visible at all.
                //
                // Measured from play: a wall that reads plainly beige/orange
                // averages to #574D4B, and another to #504848. Those ARE warm --
                // R > G > B in both -- but by about 14%, because beige highlights
                // average against shadow and black gaps and collapse toward
                // neutral. Normalising luminance then scales all three channels
                // equally and preserves that 14%, so the chip came out grey and
                // the whole feature read as not working. The sampling was right
                // and the DATA was too dull to use.
                //
                // gzdoom hits the same wall: FGameTexture::GetGlowColor passes
                // maxout=153 to averageColor precisely because a plain mean is
                // too dull to be a glow colour. It solves it by normalising the
                // PEAK channel, which also blows the brightness out -- wrong for
                // an albedo. So the chroma is expanded about the pixel's own
                // luminance instead, which leaves brightness for the step below
                // to pin and keeps the two corrections independent.
                {
                    const float sat = std::max( 0.f, float{ cvar::rt_spark_debris_sat } );
                    const float grey = 0.2126f * tr + 0.7152f * tg + 0.0722f * tb;
                    tr = std::clamp( grey + ( tr - grey ) * sat, 0.f, 1.f );
                    tg = std::clamp( grey + ( tg - grey ) * sat, 0.f, 1.f );
                    tb = std::clamp( grey + ( tb - grey ) * sat, 0.f, 1.f );
                }

                const float lum = 0.2126f * tr + 0.7152f * tg + 0.0722f * tb;
                if( lum > 0.01f )
                {
                    const float target =
                        std::clamp( float{ cvar::rt_spark_debris_albedo } * pr.albedo, 0.02f, 1.f );
                    const float k = target / lum;
                    tr = std::min( 1.f, tr * k );
                    tg = std::min( 1.f, tg * k );
                    tb = std::min( 1.f, tb * k );
                }

                // The ramp's own darkening, as a fraction of its first entry, so
                // a chip still fades with age whatever colour it took.
                const uint32_t r0    = ramp[ 0 ];
                const float    l0    = ( ( r0 >> 16 ) & 0xFF ) / 255.f;
                const float    lnow  = r;   // the ramp value resolved just above
                const float    curve = l0 > 0.01f ? std::clamp( lnow / l0, 0.f, 1.f ) : 1.f;

                tr *= curve;
                tg *= curve;
                tb *= curve;

                r += ( tr - r ) * tint;
                g += ( tg - g ) * tint;
                b += ( tb - b ) * tint;
            }
        }

        // DEBRIS IS RAY-TRACED GEOMETRY, and that is what makes it take the
        // room's light. See UploadBatch: the debris batch carries no TRANSLUCENT
        // flag and a primitive alpha of 1, which is RTGL1's rule
        // (VulkanDevice::IsRasterized) for going into the acceleration structure
        // instead of the rasterized overlay. Its colour below is therefore an
        // ALBEDO the path tracer shades, not a final pixel.
        //
        // The first attempt kept debris rasterized and multiplied the sector's
        // light level into the vertex colour by hand. That was wrong twice over:
        // it read the PAINTED lightlevel rather than the traced result, and it
        // could not escape the raster path's
        //
        //     outColor.rgb *= max( vec3( 1 ), tonemapping.avgLuminance )
        //
        // whose avgLuminance is EYE ADAPTATION -- it decays over seconds, so
        // chips visibly took time to darken on walking into an unlit room even
        // though the sector term was instant. Reported as "debris take time to
        // get dark as the room". No engine-side multiply can fix a term the
        // engine cannot see; the fix was to stop being rasterized.

        // Snapped to a whole number of grid cells, and never to ZERO: the
        // smallest a spark may be is exactly one cell, i.e. one pixel. Rounding
        // a sub-cell spark down would delete it silently. In realistic style
        // grid is 0 and both snaps collapse to the identity.
        // DEBRIS FADES BY SHRINKING, NEVER BY ALPHA. Its batch is in the
        // acceleration structure only while its primitive alpha stays at or
        // above RTGL1's MESH_TRANSLUCENT_ALPHA_THRESHOLD (0.98); fading it out
        // would silently demote the whole batch to the rasterized overlay --
        // i.e. back to being fullbright -- for the last part of every chip's
        // life. So the last quarter of the life shrinks the quad to nothing
        // instead, and the alpha below stays at 1.
        float sizeScale = 1.f;
        if( isDbr )
        {
            constexpr float kShrinkFrom = 0.75f;
            if( t > kShrinkFrom )
            {
                sizeScale = 1.f - ( t - kShrinkFrom ) / ( 1.f - kShrinkFrom );
            }
        }

        const float rawSize = sp.size * sizeScale;
        const float edge    = grid > 0.f ? std::max( grid, snap( rawSize, grid ) ) : rawSize;
        const float half    = edge * 0.5f;

        const FVector3 c{ snap( sp.pos.X, grid ), snap( sp.pos.Y, grid ), snap( sp.pos.Z, grid ) };

        FVector3 ex = right * half;
        FVector3 ey = up * half;

        if( isDbr && !pixel )
        {
            // RUBBLE, NOT PIXELS. A chip gets its own orientation, its own
            // tumble and its own aspect ratio, so a burst reads as assorted
            // fragments rather than as a grid of identical squares. It costs
            // three floats per particle and one sin/cos per frame.
            //
            // The tumble is deliberately SLOW relative to the spin of a real
            // fragment: at 20 s of life a fast spin turns into a flicker as the
            // quad passes edge-on-ish every rotation, and a flickering chip
            // reads as a rendering fault rather than as motion. A settled chip
            // stops turning: the sim bakes its final angle into `phase` and
            // zeroes `spin`, so this expression stays uniform and the chip does
            // not SNAP back to its birth orientation the moment it lands.
            const float ang = sp.phase + sp.spin * sp.age;
            const float ca  = std::cos( ang );
            const float sa  = std::sin( ang );

            const FVector3 rx = right * ca + up * sa;
            const FVector3 ry = up * ca - right * sa;

            ex = rx * ( half * sp.aspect );
            ey = ry * ( half / std::max( 0.2f, sp.aspect ) );
        }
        else if( !pixel && streak > 1.f )
        {
            // A REAL SPARK IS A STREAK, because it moves far within one exposure.
            // Project its velocity onto the screen basis and stretch the quad
            // along that direction, keeping the across-axis at the original
            // width so the fragment gets LONGER rather than BIGGER.
            //
            // CAPPED, not proportional to speed: an uncapped streak turns a fast
            // spark into a long thin line that reads as tracer fire rather than
            // as a fragment. This is the "not overdone" constraint, and it is a
            // cap rather than a judgement call for that reason.
            const float vs = sp.vel.Length();
            if( vs > 0.05f )
            {
                const FVector3 vdir = sp.vel / vs;
                FVector3       sdir = right * ( vdir | right ) + up * ( vdir | up );
                if( sdir.LengthSquared() > 1e-6f )
                {
                    sdir.MakeUnit();
                    const FVector3 sperp = up * ( sdir | right ) - right * ( sdir | up );

                    // Ramp into the cap with speed, so a slow-moving ember stays
                    // a dot and only a fast one draws a line.
                    const float k = std::clamp( vs / 6.f, 0.f, 1.f );
                    const float L = half * ( 1.f + ( streak - 1.f ) * k );

                    ex = sdir * L;
                    ey = sperp * half;
                }
            }
        }

        // ALPHA CARRIES BOTH THE FADE AND THE BRIGHTNESS, and for the additive
        // spark batch it is the only thing that can. RsWorld.inl's emission line
        // is
        //
        //     outColor.rgb += ldrEmis * ldrColor.a * emissionMaxScreenColor
        //
        // of which ldrEmis is the palette colour (already f8f8b0 at the hot end,
        // so scaling it clips to white and loses the match to the PUFF sprite)
        // and emissionMaxScreenColor is rt_emis_maxscrcolor, GLOBAL to every
        // world emissive in the game. That leaves alpha. See rt_spark_bright.
        //
        // The clamp is the mechanism, not a safety net: alpha is packed to 8
        // bits, so a multiplier above 1 does not raise the peak, it WIDENS it --
        // the spark holds full brightness for a while instead of dimming from
        // the frame it was born.
        //
        // DEBRIS DOES NOT TAKE rt_spark_bright, AND ITS ALPHA IS PINNED AT 1.
        // It is opaque ray-traced geometry: alpha there is not a look knob at
        // all, it is the flag that decides whether the batch is traced or
        // rasterized (see the shrink above). rt_spark_bright is meaningless for
        // it either way -- that knob widens an ADDITIVE peak, and debris does
        // not add.
        // The bucket can only be chosen once the colour is resolved, since the
        // colour IS the batch key.
        QuadBatch& batch = isDbr ? DebrisBucketFor( sp.surf, r, g, b ) : s_batchSpark;

        const float fade = 1.f - t * t;
        const float a    = isDbr ? 1.f : std::clamp( bright * fade, 0.f, 1.f );

        const RgColor4DPacked32 col = rt.rgUtilPackColorFloat4D( r, g, b, a );

        // Sparks face the camera and are unlit, so their normal is cosmetic.
        // Debris is shaded, so it takes the surface it was knocked off -- a
        // camera-facing normal would swing the lighting as the player turns.
        const RgNormalPacked32 vnrm =
            isDbr ? rt.rgUtilPackNormal( sp.nrm.X, sp.nrm.Y, sp.nrm.Z ) : nrm;

        const uint32_t base = uint32_t( batch.verts.size() );

        if( isDbr && !pixel )
        {
            // AN IRREGULAR POLYGON, NOT A QUAD. A rotated rectangle is still a
            // rectangle: reported as debris looking like "just a pixel /
            // square" even with the tumble and the aspect ratio already in.
            // Rubble is angular and no two pieces are the same outline, so each
            // chip gets 5-7 corners at its own radii -- stable for its life,
            // because the radii are hashed from its `sid` rather than drawn
            // fresh each frame.
            //
            // Fan-triangulated from corner 0, which is safe here because the
            // polygon is convex by construction (radii vary, angles are
            // monotonic). Pixelated style keeps the quad below: there a chip is
            // deliberately one square texel.
            const int n = 5 + int( hash01( sp.sid * 2654435761u ) * 3.f );

            for( int k = 0; k < n; k++ )
            {
                const float a =
                    ( 2.f * rt_pi() * float( k ) ) / float( n ) +
                    hash01( sp.sid * 40503u + uint32_t( k ) ) * ( rt_pi() / float( n ) );
                const float rr = 0.55f + 0.45f * hash01( sp.sid * 22695477u + uint32_t( k ) * 7919u );

                const FVector3 pv = c + ex * ( std::cos( a ) * rr ) + ey * ( std::sin( a ) * rr );

                batch.verts.push_back( RgPrimitiveVertex{
                    .position     = { pv.X, pv.Y, pv.Z },
                    .normalPacked = vnrm,
                    .texCoord     = { 0.5f + 0.5f * std::cos( a ), 0.5f + 0.5f * std::sin( a ) },
                    .color        = col,
                } );
            }

            for( int k = 1; k + 1 < n; k++ )
            {
                batch.idx.push_back( base );
                batch.idx.push_back( base + uint32_t( k ) );
                batch.idx.push_back( base + uint32_t( k + 1 ) );
            }
        }
        else
        {
            const FVector3 corner[ 4 ] = {
                c - ex - ey,
                c + ex - ey,
                c + ex + ey,
                c - ex + ey,
            };
            const float uv[ 4 ][ 2 ] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

            for( int k = 0; k < 4; k++ )
            {
                batch.verts.push_back( RgPrimitiveVertex{
                    .position     = { corner[ k ].X, corner[ k ].Y, corner[ k ].Z },
                    .normalPacked = vnrm,
                    .texCoord     = { uv[ k ][ 0 ], uv[ k ][ 1 ] },
                    .color        = col,
                } );
            }

            batch.idx.push_back( base + 0 );
            batch.idx.push_back( base + 1 );
            batch.idx.push_back( base + 2 );
            batch.idx.push_back( base + 0 );
            batch.idx.push_back( base + 2 );
            batch.idx.push_back( base + 3 );
        }

        s_dbgQuads++;

        // The blob, for debris that has come to REST on a floor. Only settled
        // pieces get one: the whole claim of contact occlusion is that the thing
        // is touching this floor, and a chip still bouncing is not. That is the
        // same honesty rt_sprite_ao_fade buys for a flying enemy.
        // AO IS DISTANCE-CULLED, and not for cost. RsDecal.frag keeps a fragment
        // only where the traced surface under it is within 5 cm of the quad, and
        // at range a pixel footprint on a grazing floor exceeds that -- the same
        // limit docs/sprite-shadows-and-ao.md bounds with rt_sprite_ao_dist 30
        // and the game's own bullet-hole decals share. Reported here as AO
        // "lines" reaching away from the chips.
        const bool aoNear = ( sp.pos - eyeM ).LengthSquared() <= aoFar * aoFar;

        if( isDbr && sp.settled && aoNear && aoStrength > 0.f && cvar::rt_spark_debris_ao &&
            aoEmitted < aoMax )
        {
            const float rad = edge * std::max( 0.f, float{ cvar::rt_spark_debris_ao_radius } );
            if( rad > 1e-4f )
            {
                s_batchDebrisAo.verts.clear();
                s_batchDebrisAo.idx.clear();
                const uint32_t abase = 0;

                const RgNormalPacked32  up = rt.rgUtilPackNormal( 0.f, 0.f, 1.f );
                const RgColor4DPacked32 cc =
                    rt.rgUtilPackColorFloat4D( 0.f, 0.f, 0.f, aoStrength );
                const RgColor4DPacked32 cr = rt.rgUtilPackColorFloat4D( 0.f, 0.f, 0.f, 0.f );

                // 1 mm of bias keeps the quad off the floor's exact plane
                // without spending the decal shader's 5 cm budget.
                const float az = sp.pos.Z + 0.001f;

                s_batchDebrisAo.verts.push_back( RgPrimitiveVertex{
                    .position     = { sp.pos.X, sp.pos.Y, az },
                    .normalPacked = up,
                    .texCoord     = { 0.5f, 0.5f },
                    .color        = cc,
                } );

                constexpr int kAoSegs = 10;
                for( int k = 0; k < kAoSegs; k++ )
                {
                    const float a = ( 2.f * rt_pi() * float( k ) ) / float( kAoSegs );
                    s_batchDebrisAo.verts.push_back( RgPrimitiveVertex{
                        .position     = { sp.pos.X + std::cos( a ) * rad,
                                          sp.pos.Y + std::sin( a ) * rad,
                                          az },
                        .normalPacked = up,
                        .texCoord     = { 0.5f + 0.5f * std::cos( a ), 0.5f + 0.5f * std::sin( a ) },
                        .color        = cr,
                    } );
                }
                for( int k = 0; k < kAoSegs; k++ )
                {
                    s_batchDebrisAo.idx.push_back( abase );
                    s_batchDebrisAo.idx.push_back( abase + 1 + uint32_t( k ) );
                    s_batchDebrisAo.idx.push_back( abase + 1 + uint32_t( ( k + 1 ) % kAoSegs ) );
                }

                // A distinct ID per blob. RTGL1 keeps one upload per ID and it is
                // the LATER one that loses, so a shared ID would silently show
                // only the first chip's blob.
                UploadAoBlob( s_batchDebrisAo, RT_DEBRIS_AO_MESH_ID + uint64_t( aoEmitted ) );
                aoEmitted++;
                s_dbgAo++;
            }
        }
    }

    UploadBatch( s_batchSpark, RT_SPARK_MESH_ID, true, nullptr, RG_PACKED_COLOR_WHITE );

    for( int i = 0; i < s_debrisBucketCount; i++ )
    {
        const DebrisBucket& db = s_debrisBuckets[ i ];

        // Distinct mesh IDs: RTGL1 keeps only one upload per ID and it is the
        // LATER one that loses, so a shared ID would silently drop every bucket
        // after the first.
        UploadBatch( db.batch,
                     RT_DEBRIS_MESH_ID + uint64_t( i ),
                     false,
                     ProfileFor( db.kind ).texture,
                     rt.rgUtilPackColorFloat4D( ( ( db.rgb >> 16 ) & 0xFF ) / 255.f,
                                                ( ( db.rgb >> 8 ) & 0xFF ) / 255.f,
                                                ( db.rgb & 0xFF ) / 255.f,
                                                1.0f ) );
    }
}

// ---------------------------------------------------------------------------
// Lights carried by the FLYING SPARKS themselves.
//
// Reported from play: "the sparks themselves don't seem to emit any light, only
// the impact location." That was accurate and it was by construction. The
// particles are an ADDITIVE RASTERIZED overlay, and RTGL1 keeps translucent
// primitives out of the acceleration structure entirely -- so however bright a
// spark is on screen, it is not a light source and nothing in rt_spark_bright
// can make it one. Screen brightness and cast light are separate channels here.
// The only thing that casts is an analytic light, so here are some.
//
// NOT ONE PER SPARK. That was costed and rejected when the feature was designed:
// a super-shotgun is 20 impacts x 8 sparks = 160 MOVING lights in a frame, which
// is rt-lighting-practices section 20 exactly. rt_spark_glow_max caps it at a
// handful, nearest and youngest first, and that reads as all of them glowing --
// the eye cannot audit which dot is lighting the wall.
//
// DEBRIS NEVER GLOWS. A chip of stone is not hot; if debris lights the room, the
// surface classification is wrong and this is where you would see it.
// ---------------------------------------------------------------------------
namespace
{

void UploadSparkGlowLights()
{
    if( !cvar::rt_spark_glow )
    {
        return;
    }

    const float gi = std::max( 0.f, float{ cvar::rt_spark_glow_intensity } );
    const int   gmax = std::max( 0, int{ cvar::rt_spark_glow_max } );
    if( gi <= 0.f || gmax == 0 || g_sparkCount == 0 )
    {
        return;
    }

    const auto&    vp = r_viewpoint;
    const FVector3 eye{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                        float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                        float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };

    struct GCand
    {
        float    key;
        uint32_t idx;
    };
    static std::vector< GCand > cand;
    cand.clear();
    cand.reserve( g_sparkCount );

    for( uint32_t i = 0; i < g_sparkCount; i++ )
    {
        const Spark& sp = s_sparks[ i ];
        if( sp.kind != SparkKind::Spark )
        {
            continue;
        }
        const float t = std::clamp( sp.age / std::max( 1e-4f, sp.life ), 0.f, 1.f );
        // Rank by distance, biased by age: a young spark is both brighter and
        // more interesting than an old one at the same range, and without the
        // bias the cap fills with settled embers while the shower you just made
        // stays dark.
        const float d2 = ( sp.pos - eye ).LengthSquared();
        cand.push_back( GCand{ d2 * ( 1.f + 4.f * t * t ), i } );
    }

    if( cand.size() > size_t( gmax ) )
    {
        std::partial_sort( cand.begin(),
                           cand.begin() + gmax,
                           cand.end(),
                           []( const GCand& a, const GCand& b ) { return a.key < b.key; } );
        cand.resize( size_t( gmax ) );
    }

    for( const GCand& c : cand )
    {
        const Spark& sp = s_sparks[ c.idx ];

        const float t = std::clamp( sp.age / std::max( 1e-4f, sp.life ), 0.f, 1.f );
        const float k = ( 1.f - t ) * ( 1.f - t );
        if( k <= 0.001f )
        {
            continue;
        }

        // The spark's CURRENT palette colour, so the light it casts is the same
        // colour as the dot casting it -- a spark that has cooled to brown must
        // not still be throwing pale yellow on the wall.
        const int      ci  = std::min( RT_SPARK_RAMP_N - 1,
                                       int( t * float( RT_SPARK_RAMP_N ) ) );
        const uint32_t rgb = RT_SPARK_RAMP[ ci ];

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( ( ( rgb >> 16 ) & 0xFF ) / 255.f,
                                                    ( ( rgb >> 8 ) & 0xFF ) / 255.f,
                                                    ( rgb & 0xFF ) / 255.f,
                                                    1.0f ),
            .intensity = gi * k,
            .position  = { sp.pos.X, sp.pos.Y, sp.pos.Z },
            .radius    = 0.03f,
        };
        auto info = RgLightInfo{
            .sType = RG_STRUCTURE_TYPE_LIGHT_INFO,
            // THE EXTENSION HANGS OFF pNext, AND OMITTING IT IS SILENT-ISH.
            // RTGL1's UploadLight looks for RgLightSphericalEXT (or one of its
            // three siblings) by walking pNext; find nothing and it warns
            // "Couldn't find RgLightDirectionalEXT, RgLightSphericalEXT, ... on
            // RgLightInfo (uniqueID=N)" and DROPS the light. Every other field
            // is valid, so nothing else complains -- the light simply never
            // exists. This shipped broken exactly once, and the symptom was
            // "the sparks cast no light, only the impact does": the flash below
            // set pNext and these did not.
            .pNext = &sph,
            // THE SPARK'S OWN ID, never the candidate slot. A slot is reassigned
            // to a different spark as sparks die, so a slot-keyed light would
            // teleport across the room between frames -- worse for ReSTIR than
            // dying, because RTGL1 would carry the old reservoir to a new place.
            // Keyed on the spark, a glow is born and dies with it.
            .uniqueID     = SparkGlowId_Base + uint64_t( sp.sid ),
            .isExportable = false,
        };
        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        s_dbgLights++;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// The traced half: one short spherical light per impact, plus the glows above.
// ---------------------------------------------------------------------------
void RT_UploadSparkLights()
{
    s_dbgLights = 0;

    if( !cvar::rt_spark )
    {
        return;
    }

    UploadSparkGlowLights();

    if( !cvar::rt_spark_light || g_sparkFlashCount == 0 )
    {
        return;
    }

    const float intensity = std::max( 0.f, float{ cvar::rt_spark_light_intensity } );
    if( intensity <= 0.f )
    {
        return;
    }

    const auto&    vp = r_viewpoint;
    const FVector3 eye{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                        float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                        float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };

    // Nearest-first, then truncate -- the RT_UploadFlameLights pattern. The
    // INDEX is the light's identity, so sorting a copy rather than the pool
    // itself is not optional: a light whose id moved between frames would make
    // RTGL1 see the whole set die and respawn and throw away its temporal
    // reservoirs, and a flash lives ~0.18 s, so it would be reborn for its
    // entire life.
    struct Cand
    {
        float    d2;
        uint32_t slot;
    };
    static std::vector< Cand > cand;
    cand.clear();
    cand.reserve( g_sparkFlashCount );

    for( uint32_t i = 0; i < g_sparkFlashCount; i++ )
    {
        cand.push_back( Cand{ float( ( s_flashes[ i ].pos - eye ).LengthSquared() ), i } );
    }

    const size_t budget =
        size_t( std::max( 0, int{ cvar::rt_spark_light_max } ) );
    if( cand.size() > budget )
    {
        std::partial_sort( cand.begin(),
                           cand.begin() + budget,
                           cand.end(),
                           []( const Cand& a, const Cand& b ) { return a.d2 < b.d2; } );
        cand.resize( budget );
    }

    for( const Cand& c : cand )
    {
        const SparkFlash& fl = s_flashes[ c.slot ];

        // Fade over the life. The flash is the moment of impact, not a lamp.
        const float t = std::clamp( fl.age / std::max( 1e-4f, fl.life ), 0.f, 1.f );
        const float k = ( 1.f - t ) * ( 1.f - t );
        if( k <= 0.001f )
        {
            continue;
        }

        // The hot end of PUFF's own ramp, so the light and the particles agree.
        const uint32_t rgb = RT_SPARK_RAMP[ 0 ];
        const float    kR  = ( ( rgb >> 16 ) & 0xFF ) / 255.f;
        const float    kG  = ( ( rgb >> 8 ) & 0xFF ) / 255.f;
        const float    kB  = ( rgb & 0xFF ) / 255.f;

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( kR, kG, kB, 1.0f ),
            .intensity = intensity * k,
            .position  = { fl.pos.X, fl.pos.Y, fl.pos.Z },
            .radius    = 0.05f,
        };
        auto info = RgLightInfo{
            .sType = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext = &sph,
            // THE SLOT, never the age or the tick -- see SparkFlashId_Base.
            .uniqueID     = SparkFlashId_Base + uint64_t( c.slot ),
            .isExportable = false,
        };
        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        s_dbgLights++;
    }
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
namespace
{

void SparkReport( const char* why )
{
    Printf( "rt_spark %s: A/hit %d impacts (%d rejected: far)  "
            "B/spawn %d sparks, %u live of %d  "
            "C/sent %d quads, %d lights of %d  (%d wall traces)\n",
            why,
            s_dbgHits,
            s_dbgRejected,
            s_dbgSpawned,
            g_sparkCount,
            int{ cvar::rt_spark_max },
            s_dbgQuads,
            s_dbgLights,
            int{ cvar::rt_spark_light_max },
            s_dbgTraces );

    if( cvar::rt_spark_debris_ao )
    {
        Printf( "  AO blobs uploaded: %d of %d max.  >0 with nothing on screen is a "
                "POSITION bug, not a strength one.\n",
                s_dbgAo,
                int{ cvar::rt_spark_debris_ao_max } );
    }

    // WHAT THOSE IMPACTS LANDED ON. Separate line because it answers a different
    // question from the ladder above -- the ladder is "is the feature running",
    // this is "is the classification right" -- and because a per-impact print
    // (rt_spark_surface_debug) is too noisy to leave on while playing.
    Printf( "  types: metal %d (sparks)  concrete %d  other %d  UNLISTED %d%s\n",
            s_dbgMetal,
            s_dbgConcrete,
            s_dbgOther,
            s_dbgUnlisted,
            cvar::rt_spark_debris ? "" : "   [rt_spark_debris 0 -- all of these spark]" );
}

// The unlisted worklist. Kept out of SparkReport so the once-a-second ladder
// stays one or two lines, while `sparks` on demand can be as long as it needs.
void SparkReportUnlisted()
{
    if( s_unlistedSeen.empty() )
    {
        Printf( "  unlisted textures hit: none\n" );
        return;
    }

    FString list;
    for( size_t i = 0; i < s_unlistedSeen.size(); i++ )
    {
        if( i )
        {
            list += " ";
        }
        list += s_unlistedSeen[ i ];
    }
    Printf( "  unlisted textures hit (%d%s): %s\n",
            int( s_unlistedSeen.size() ),
            s_unlistedSeen.size() >= RT_UNLISTED_MAX ? ", capped" : "",
            list.GetChars() );
    Printf( "  ^ these are the ones the material labeller has not reached; they all "
            "fall through to debris.\n" );
}

} // namespace

void RT_SparkDebugTick()
{
    if( !cvar::rt_spark_debug )
    {
        // Keep the counters from growing without bound while the log is off,
        // so turning it on mid-session reports this second and not this hour.
        s_dbgHits = s_dbgRejected = s_dbgSpawned = s_dbgTraces = 0;
        s_dbgMetal = s_dbgConcrete = s_dbgOther = s_dbgUnlisted = 0;
        return;
    }

    static double s_next = 0.0;
    const double  now    = RT_GetCurrentTime();
    if( now < s_next )
    {
        return;
    }
    s_next = now + 1.0;

    // The whole point of the ladder: "nothing spawned" and "spawned but not
    // drawn" are identical on screen, and this project has lost sessions to not
    // being able to tell them apart.
    SparkReport( "1s" );

    s_dbgHits = s_dbgRejected = s_dbgSpawned = s_dbgTraces = 0;
        s_dbgMetal = s_dbgConcrete = s_dbgOther = s_dbgUnlisted = 0;
}

CCMD( sparks )
{
    SparkReport( "now" );
    SparkReportUnlisted();
}

// Reload the metal list without restarting, because classifying 2331 textures is
// an edit-check-edit loop and a rebuild or even a level reload per entry would
// make it unbearable. Pair it with rt_spark_surface_debug 1.
CCMD( spark_surfaces )
{
    LoadSparkSurfaces();

    int n[ 3 ]{};
    for( const SurfEntry& e : s_surfExact )
    {
        n[ int( e.kind ) ]++;
    }
    for( const SurfEntry& e : s_surfPrefix )
    {
        n[ int( e.kind ) ]++;
    }

    if( !s_surfacesFound )
    {
        Printf( TEXTCOLOR_ORANGE
                "rt_spark surfaces: FILE NOT FOUND -- '%s'\n" TEXTCOLOR_NORMAL
                "  Every surface therefore classifies as 'other', i.e. debris when\n"
                "  rt_spark_debris is on and sparks when it is off. Run:\n"
                "    python tools/build_spark_surfaces.py\n"
                "  It writes the engine build tree AND Retribution-RT-Materials.\n",
                s_surfacesPath.GetChars() );
        return;
    }

    Printf( "rt_spark surfaces: %d entries (%d exact, %d prefix) from %s\n",
            int( s_surfExact.size() + s_surfPrefix.size() ),
            int( s_surfExact.size() ),
            int( s_surfPrefix.size() ),
            s_surfacesPath.GetChars() );
    Printf( "  metal %d (sparks)   concrete %d (debris)   other %d (debris)\n",
            n[ int( SurfKind::Metal ) ],
            n[ int( SurfKind::Concrete ) ],
            n[ int( SurfKind::Other ) ] );
    Printf( "  regenerate with: python tools/build_spark_surfaces.py  "
            "(reads the PBR labeller's `surface` field)\n" );
    SparkReportUnlisted();
    Printf( "  rt_spark_debris is %s. Set rt_spark_surface_debug 1 and shoot a "
            "wall to see what each surface is called.\n",
            cvar::rt_spark_debris ? "ON (non-metal throws debris)"
                                  : "off (everything sparks)" );
}
