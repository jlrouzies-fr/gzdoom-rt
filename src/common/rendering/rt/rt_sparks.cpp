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
#include "rt_sparks_internal.h"

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
using namespace rtsp;

namespace rtsp
{





// ---------------------------------------------------------------------------

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






// NOTE ON WHAT IS *NOT* HERE. Projectile impact arcs were first built as a third
// SparkKind -- particles thrown along the surface instead of off it. It was the
// wrong construction and it is worth saying why, because the mistake is an easy
// one to make twice: a ring of flying quads is a spark shower whatever direction
// you aim it, and an electric remain is not made of moving fragments at all. It
// is a MARK ON THE WALL that stays put and crackles. So arcs are not particles
// and do not live in this pool; see ArcMark below.

// WHICH PROJECTILE this came from, which is the only thing an arc needs to know
// about its origin -- it selects the ramp and the flash colour and nothing else.







// Pool ceilings. These bound the fixed arrays; the cvars bound how much of them
// is used, so raising a cvar past its ceiling is clamped rather than corrupting.
// THE POOL HAD TO GROW WITH THE LIFETIMES. Sparks went 1.1 s -> 5 s and debris
// 1.4 s -> 20 s, and live count is spawn rate x lifetime, so the same firing
// makes roughly five times the sparks and fourteen times the chips. At the old
// 1024 a couple of seconds of chaingun filled the pool and the eviction rule
// became the thing you were watching. A Spark is ~72 bytes, so 4096 is ~290 KB
// -- irrelevant next to being able to honour the lifetimes that were asked for.






// The per-IMPACT flash. One of these per impact, never one per spark.


std::array< Spark, RT_SPARK_HARDMAX >          s_sparks{};
std::array< SparkFlash, RT_SPARK_FLASH_MAX >   s_flashes{};

// THE POOL IS SHARED, SO THE MASTER GATE IS THE UNION OF TWO CVARS.
//
// Sim, draw and lights all used to test rt_spark alone. Arcs live in the same
// pool, so leaving that in place would have made rt_spark -- which ships OFF --
// silently switch off the arcs too. That is precisely the coupling
// docs/plan-projectile-impact-fx.md 2 rejects for the smoke walk, and it would
// have been just as invisible: the arcs would spawn, be integrated by nothing,
// and be drawn by nothing.
//
// The two remain independent where it counts, at the SPAWN sites: a hitscan
// impact still tests rt_spark and a projectile impact still tests rt_arc, so
// each effect can be judged with the other out of the way.
bool SparkSystemOn()
{
    return cvar::rt_spark || cvar::rt_arc || cvar::rt_barrel;
}

// THE POOL ALLOCATOR, lifted out of RT_SpawnImpactSparks when the barrel gained
// a second spawn site. Duplicating the eviction rule would have been two places
// for it to drift, and the rule is the non-obvious part of the pool.
Spark* AllocSpark( SparkKind kind )
{
    const uint32_t cap =
        std::min( RT_SPARK_HARDMAX, uint32_t( std::max( 0, int{ cvar::rt_spark_max } ) ) );
    if( cap == 0 )
    {
        return nullptr;
    }

    uint32_t slot;
    if( g_sparkCount < cap )
    {
        slot = g_sparkCount++;
    }
    else
    {
        // OLDEST OF ITS OWN KIND, not oldest overall, and the difference is
        // load-bearing once the kinds have very different lifetimes.
        //
        // Sparks live ~5 s, debris ~20 s and barrel plate longer still, in one
        // shared pool. A plain oldest-out rule therefore evicts the LONG-LIVED
        // population almost every time -- it is reliably the older one -- so
        // chips would be culled within a second or two of spawning and their
        // long life would be a number that never happened. Evicting within the
        // kind bounds each population by its own spawn rate instead, so none can
        // starve another. Same shape as the ambient-first rule in
        // RT_SpawnSmokePuffs, and for the same reason.
        slot               = UINT32_MAX;
        uint32_t oldestAny = 0;
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
        // first chunk can still be born into a pool full of sparks.
        if( slot == UINT32_MAX )
        {
            slot = oldestAny;
        }
    }

    return &s_sparks[ slot ];
}

// A spark's identity, for its glow light's uniqueID and for every stable hash
// its geometry uses. Monotonic and never reused, so a light can never be
// inherited by a different particle.
uint32_t NextSparkSid()
{
    static uint32_t s_next = 1;
    return s_next++;
}

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
// Ember wisps spawned this second. Separate from the smoke system's own
// counters: "the embers are not smoking" and "the smoke system is full" look
// identical on screen and have different fixes.
int s_dbgEmberSmoke = 0;

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


} // namespace rtsp

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
        Spark* slotp = AllocSpark( kind );
        if( !slotp )
        {
            break;
        }

        // A disc sample, sqrt-weighted so the cone is uniform rather than
        // clustered on the axis.
        const float ang = rnd01() * 2.f * rt_pi();
        const float rad = std::sqrt( rnd01() ) * coneR;

        FVector3 dir = refl + ( tangent * std::cos( ang ) + bitangent * std::sin( ang ) ) * rad;
        dir.MakeUnit();

        Spark& sp = *slotp;

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
        sp.sid = NextSparkSid();

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
            fl.isArc       = false;
            fl.arc         = ArcFlavor::Plasma; // unread when isArc is false
        }
    }
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------
namespace rtsp
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


} // namespace rtsp

// ---------------------------------------------------------------------------
// ITS OWN WALK, not a hook into the smoke one. See rt_internal.h for why: the
// smoke walk is gated three deep and none of its gates mean anything to arcs.
//
// Runs once a TIC rather than once a frame. The event it is looking for is a
// tic-long state change (MF_MISSILE clearing), so re-testing it at 200 fps buys
// nothing -- the same reasoning rt_smoke.cpp applies to its monster gunners.
// ---------------------------------------------------------------------------

void RT_UpdateSparks()
{
    if( !primaryLevel )
    {
        RT_ClearSparks();
        RT_ClearArcMarks();
        return;
    }

    if( !SparkSystemOn() )
    {
        g_sparkCount      = 0;
        g_sparkFlashCount = 0;
        RT_ClearArcMarks();
        s_lastTic         = primaryLevel->maptime;
        return;
    }

    // Arcs off on their own still has to drop the marks, or the last filigree
    // before the switch stays burnt onto the wall for the rest of the session.
    if( !cvar::rt_arc )
    {
        RT_ClearArcMarks();
    }

    const int tic = primaryLevel->maptime;

    int steps = s_lastTic < 0 ? 0 : tic - s_lastTic;
    // A backwards or enormous jump is a level change, a load or a warp. Do not
    // integrate across it: a spark advanced by a thousand tics ends up somewhere
    // arbitrary, and tier 2 would trace the whole way there.
    if( steps < 0 || steps > TICRATE )
    {
        RT_ClearSparks();
        RT_ClearArcMarks();
        s_lastTic = tic;
        return;
    }
    s_lastTic = tic;

    const float dt      = 1.f / float( TICRATE );

    // The arc marks age on the same clock. They do not move, so this is the
    // whole of their simulation -- there is no per-step integration to do and
    // the multi-step loop below would only be advancing a counter.
    AgeArcMarks( dt * float( steps ) );
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

    const bool  wantEmberSmoke = cvar::rt_ember && cvar::rt_ember_smoke && cvar::rt_smoke;
    const float smokeEvery     = std::max( 0.02f, float{ cvar::rt_ember_smoke_every } );
    const float smokeHot       = std::clamp( float{ cvar::rt_ember_smoke_hot }, 0.f, 1.f );

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
            const bool           isDbr = IsChunk( sp.kind );
            const DebrisProfile& pr    = ProfileFor( sp.surf );
            const float          kGrav = isDbr ? gravityD * pr.gravity : gravity;
            const float          kBnce =
                isDbr ? std::clamp( bounceD * pr.bounce, 0.f, 1.f ) : bounce;
            const float kFric =
                isDbr ? std::clamp( fric * pr.friction, 0.f, 1.f ) : fric;
            const float kDrag = drag;

            sp.vel.Z -= kGrav * dt;
            sp.vel *= std::max( 0.f, 1.f - kDrag * dt );
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
                        // A SHARD LIES DOWN. Debris is a billboard, so where its
                        // normal points is a shading choice and nothing more; a
                        // shard is a real oriented plate, and one frozen mid-
                        // tumble stands on its edge in the floor. Zeroing the
                        // spin is what makes this safe to state as a normal
                        // rather than as a rotation -- the draw only turns the
                        // plate about its own normal once spin is gone, so the
                        // face stays flat and only the yaw is arbitrary.
                        //
                        // Floors in this game are not all level, so it takes the
                        // plane's normal rather than world up. Reading the plane
                        // it settled ON is the point: on a sloped floor a chunk
                        // lying dead flat in world space is visibly sunk at one
                        // edge and floating at the other.
                        if( sp.kind == SparkKind::Shard )
                        {
                            const DVector3 fn = sec->floorplane.Normal();
                            FVector3       up{ float( fn.X ), float( fn.Y ), float( fn.Z ) };
                            if( up.LengthSquared() < 1e-6f )
                            {
                                up = FVector3{ 0, 0, 1 };
                            }
                            up.MakeUnit();

                            // NOT DEAD FLAT, and this is not decoration.
                            //
                            // A plate lying perfectly flush with the floor is
                            // seen from standing height at a very shallow angle,
                            // so it projects to a few pixels of sliver and
                            // vanishes at any distance -- which is exactly how
                            // the first capture came out: a scorch on an empty
                            // floor with the wreckage technically present and
                            // invisible. It is also wrong: torn sheet does not
                            // lie flush, it rocks on its own buckles.
                            //
                            // Tilting it gives the piece a silhouette against
                            // the floor and a face that can catch a light. The
                            // cap keeps it from standing up like a headstone.
                            const float tilt =
                                std::clamp( float{ cvar::rt_barrel_rest_tilt }, 0.f, 0.9f );
                            if( tilt > 0.f )
                            {
                                const FVector3 wob{ rnd11(), rnd11(), rnd11() };
                                FVector3       n = up + wob * tilt;
                                if( n.LengthSquared() > 1e-6f )
                                {
                                    n.MakeUnit();
                                    // Never point into the floor.
                                    if( ( n | up ) < 0.f )
                                    {
                                        n = n - up * ( 2.f * ( n | up ) );
                                        n.MakeUnit();
                                    }
                                    up = n;
                                }
                            }
                            sp.nrm = up;

                            // Lifted by a fraction of its own size, or a tilted
                            // plate is buried to its waist in the floor -- half
                            // the piece is below the plane it is resting on.
                            sp.pos.Z += sp.size * 0.35f * tilt;
                        }
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
namespace rtsp
{




} // namespace rtsp


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
namespace rtsp
{


} // namespace rtsp

// ---------------------------------------------------------------------------
// The traced half: one short spherical light per impact, plus the glows above.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
namespace rtsp
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

    // ARCS SHARE THE POOL, so the live count above lumps them in with sparks and
    // chips. Breaking them out is the difference between "arcs are not spawning"
    // and "arcs are spawning and not visible" -- the same distinction the ladder
    // exists for, one level down.
    if( cvar::rt_ember )
    {
        // THE WISP COUNT IS THE POINT OF THIS LINE. "the embers are not smoking"
        // has two causes that look identical: no wisps are being asked for, or
        // they are being asked for and the smoke system is refusing them
        // (rt_smoke off, or the ambient budget full). Non-zero here with no
        // smoke on screen is the second.
        uint32_t nEmbMarks = 0;
        for( uint32_t i = 0; i < s_arcCount; i++ )
        {
            if( s_arcs[ i ].fx == ImpactFx::Ember )
            {
                nEmbMarks++;
            }
        }
        Printf( "  embers: %u smouldering mark(s), %d wisp(s) spawned this second%s.\n",
                nEmbMarks,
                s_dbgEmberSmoke,
                !cvar::rt_smoke ? "  -- rt_smoke is OFF, so no wisp can ever spawn" : "" );
    }

    if( cvar::rt_arc )
    {
        Printf( "  arcs: %u mark(s) live of %d, %zu projectile(s) tracked right now.\n"
                "    Tracked 0 while shooting plasma means the CLASS MATCH failed;\n"
                "    tracked but no marks means the PROBE found no surface. Different fixes.\n",
                s_arcCount,
                int{ cvar::rt_arc_max },
                TrackedProjectileCount() );
    }

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

} // namespace rtsp

void RT_SparkDebugTick()
{
    if( !cvar::rt_spark_debug )
    {
        // Keep the counters from growing without bound while the log is off,
        // so turning it on mid-session reports this second and not this hour.
        s_dbgHits = s_dbgRejected = s_dbgSpawned = s_dbgTraces = 0;
        s_dbgMetal = s_dbgConcrete = s_dbgOther = s_dbgUnlisted = 0;
        s_dbgEmberSmoke = 0;
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
    s_dbgEmberSmoke = 0;
}

CCMD( sparks )
{
    SparkReport( "now" );
    SparkReportUnlisted();
}


