// Doom64-RT: WHAT A PROJECTILE LEAVES BEHIND.
//
// Split out of rt_sparks.cpp. Everything downstream of a PROJECTILE dying: the
// thinker walk that spots the death, the trace that recovers what it hit, and
// the mark it burns onto that surface. Hitscan impacts are NOT here -- they come
// in through an entirely different door (the hook in P_LineAttack) and share
// only the particle pool.
//
// THE ONE THING TO UNDERSTAND BEFORE CHANGING ANYTHING HERE: a mark is not made
// of particles, and that was got wrong TWICE.
//
// Arcs were first built as particles thrown along the surface plane, on the
// theory that a ring reads differently from a cone. It does not -- what the eye
// reads as "sparks" is not the direction of travel, it is that the fragments are
// discrete, separate and MOVING. Reported from play as "just spawn many
// particles, square / rectangle over the place".
//
// Embers were then built as particles too -- long-lived glowing chips with
// gravity, drag, bounce and settling, landing on the floor. Same mistake, one
// feature later. Reported as "ambers are not sparks / particles falling on the
// floor. They stay on the wall / impact churn, just a few of them."
//
// An electric remain and a hot coal are both things burnt ONTO a surface. They
// stay put. So the primitive here is an ArcMark -- a point, a normal, an
// in-plane basis and a seed -- and the geometry is regenerated from that seed
// every frame, which is also what lets it flicker and re-path. No amount of
// gravity, drag or bounce tuning turns a fragment in flight into a mark on a
// wall; if this ever looks wrong, check the CONSTRUCTION before the values.

#include "rt_sparks_internal.h"

#include "p_trace.h"

// NOTE: no `using namespace rtsp;` up here. This file DEFINES things inside
// that namespace, and a using-directive in scope while doing so makes every
// unqualified name findable twice -- MSVC reports it as an ambiguous call to
// rtsp::rtsp::whatever, which is a confusing way to say it. The directive goes
// after the closing brace, for the global-scope entry point below.
using namespace rtx;

namespace rtsp
{

constexpr ArcStyle RT_ARC_STYLES[ int( ArcFlavor::COUNT ) ] = {
    // The reference. Every rt_arc_* default means "a plasma rifle impact".
    { RT_ARC_PLASMA_RAMP, RT_ARC_PLASMA_RAMP_N, 1.0f, "plasma" },
    // The arachnotron fires the same class of bolt, so it gets the same size --
    // only the palette differs. A monster's shot reading smaller than the
    // player's would be the effect telling the player something untrue about
    // how hard it hits.
    { RT_ARC_ARACH_RAMP, RT_ARC_ARACH_RAMP_N, 1.0f, "arachnotron" },
    // DOUBLE. The BFG is the heaviest weapon in the game and its impact should
    // not read as a plasma bolt in green.
    { RT_ARC_BFG_RAMP, RT_ARC_BFG_RAMP_N, 2.0f, "bfg" },
};

const ArcStyle& ArcStyleFor( ArcFlavor f )
{
    const int i = int( f );
    return RT_ARC_STYLES[ ( i >= 0 && i < int( ArcFlavor::COUNT ) ) ? i : 0 ];
}

// WHICH ACTORS ARC, matched by a substring of the class name.
//
// Substring rather than exact, because the WAD subclasses freely:
// "PlasmaBall" covers 64PlasmaBall, 64ClassicPlasmaBall and
// 64ClassicPlasmaBallNormal in one row.
//
// NO EXCLUSION ROWS ARE NEEDED, which is worth stating because the smoke table
// needed two and the reason they differ is not obvious. The near misses are
// 64PlasmaTrail, 64PlasmaRifle, 64BFG9000 and 64BFGExtra -- and none of them
// contains "PlasmaBall", "ArachnotronPlasma" or "BFGBall" as a substring. The
// keys were chosen to make that true; a lazier "Plasma" would have caught the
// trail and the weapon both.
// WHAT AN IMPACT PRODUCES. The projectile walk is shared -- the same MF_MISSILE
// edge and the same surface probe serve both -- and only the spawn differs.


struct ArcSource
{
    const char* cls;
    ArcFlavor   flavor; // meaningless for Ember, which has one ramp
    ImpactFx    fx;
};

constexpr ArcSource RT_ARC_SOURCES[] = {
    { "PlasmaBall", ArcFlavor::Plasma, ImpactFx::Arc },
    { "ArachnotronPlasma", ArcFlavor::Arach, ImpactFx::Arc },
    { "BFGBall", ArcFlavor::BFG, ImpactFx::Arc },
    // 64Rocket and 64CyberRocket -- the player's and the cyberdemon's, which is
    // the only enemy that fires one. THE ONLY IMPACTS THAT GET SMOKE; plasma and
    // the Unmaker deliberately get none.
    { "Rocket", ArcFlavor::Plasma, ImpactFx::Ember },
    // THE BIG MONSTER PROJECTILES, at the same size as a rocket rather than
    // scaled down. Judged in game: these read as large on screen, and a smaller
    // mark would say they hit softer than they do.
    //
    // 64TracerMissile (TRCR), the revenant's homing shot, and 64MotherBall
    // (RBAL) -- both RevenantTracer subclasses, so the sprite differs but the
    // thing does not. 64FatShot (MANF) is the mancubus.
    //
    // They already TRAIL smoke -- rt_smoke.cpp matches all three by sprite --
    // but that table is about the flight, not the impact, and nothing connected
    // the two. Which is why they were smoking all the way to a wall and then
    // leaving it unmarked.
    //
    // 64MotherBallTrail is not caught here despite containing "MotherBall": the
    // "Trail" exclusion takes it, the same rule that keeps 64RocketSmokeTrail
    // and 64PlasmaTrail out. A trail is never an impact.
    { "TracerMissile", ArcFlavor::Plasma, ImpactFx::Ember },
    { "MotherBall", ArcFlavor::Plasma, ImpactFx::Ember },
    { "FatShot", ArcFlavor::Plasma, ImpactFx::Ember },
};

// "Rocket" is the one key here loose enough to catch things that are not
// projectiles at all -- 64RocketLauncher and 64RocketSmokeTrail both contain it,
// and 64PlasmaTrail derives from RocketSmokeTrail. The MF_MISSILE test rejects
// the launcher on its own, but a trail actor IS a missile in DECORATE terms, so
// it has to be excluded by name. rt_smoke.cpp's table needed exactly the same
// two exclusions for the same key, which is why this is a rule rather than a
// one-off: a trail is never an impact.
bool ArcClassExcluded( const char* c )
{
    return strstr( c, "Trail" ) != nullptr || strstr( c, "Launcher" ) != nullptr;
}

// For the `sparks` ladder, which lives in rt_sparks.cpp and must not reach into
// this file's mark list directly. The count is the distinction that matters
// there: 0 tracked while shooting plasma is a CLASS MATCH failure, tracked with
// no marks is a PROBE failure, and the two have completely different fixes.
size_t TrackedProjectileCount();

// ---------------------------------------------------------------------------
// PROJECTILE IMPACT ARCS -- A MARK ON THE WALL, NOT PARTICLES.
//
// THE FIRST VERSION WAS PARTICLES AND IT WAS THE WRONG CONSTRUCTION. Arcs were
// a third SparkKind, thrown along the surface plane instead of off it, on the
// theory that a ring reads differently from a cone. It does not: reported from
// play as "just spawn many particles, square / rectangle over the place". A
// cloud of moving quads is a spark shower whichever direction you aim it,
// because what the eye reads as "sparks" is not the direction of travel -- it is
// that the fragments are discrete, separate, and MOVING.
//
// An electric remain is none of those things. It is a CONNECTED filigree that
// stays where it was put and crackles in place. So the primitive here is not a
// particle at all, it is a POLYLINE lying in the surface plane:
//
//   * one ArcMark per impact -- a point, a normal, an in-plane basis, a seed
//   * `rt_arc_branches` polylines walking outward from that point, each
//     `rt_arc_segments` steps long, each step turned by a random angle
//   * every segment drawn as a THIN in-plane quad, `rt_arc_width` across
//   * a small bright core at the impact -- the remains of the ball itself
//
// The random walk is what makes it read as lightning rather than as a starburst:
// straight rays out of a centre look like a sun, and the eye needs the kinks.
//
// THE GEOMETRY IS NOT STORED, it is regenerated every frame from the mark's
// seed. That is deliberate and not merely thrifty: it is what lets the filigree
// FLICKER -- individual branches drop out and return between frames, which is
// most of what sells it as electric -- while staying anchored to the same wall
// and re-deriving the identical skeleton. A stored mesh would have to be either
// static (dead) or rebuilt anyway.
// ---------------------------------------------------------------------------

// The pool. Its ceilings and the ArcMark layout are in rt_sparks_internal.h,
// because the draw walks this same array.
std::array< ArcMark, RT_ARC_MARK_MAX > s_arcs{};
uint32_t                               s_arcCount = 0;

// BRANCH LIGHTS ARE COLLECTED FROM THE GEOMETRY THAT WAS ACTUALLY DRAWN, not
// recomputed in the light pass. The draw already walks every branch, already
// knows which ones the crackle dropped this frame, and already has the re-pathed
// tip position -- recomputing all three in a second pass would be the same work
// twice AND two places for the churn to disagree, which would show up as lights
// sitting where no branch is. RT_DrawSparks fills this; RT_UploadSparkLights
// drains it, and rt_main.cpp:2130 already runs them in that order.

std::vector< ArcLightCand > s_arcLights;

void RT_ClearArcMarks()
{
    s_arcCount = 0;
}

// `at` is METRES and on the surface; `normal` is a unit surface normal.
// The defaults are on the DECLARATION in rt_sparks_internal.h; repeating them
// here is an error the moment a second file calls this.
void SpawnArcMark( const FVector3& at,
                   const FVector3& normal,
                   ArcFlavor       flavor,
                   bool            withArcs,
                   float           burnScale,
                   ImpactFx        fx,
                   float           emberScale,
                   bool            emberArt,
                   float           emberSize,
                   float           emberBright,
                   float           emberScatter )
{
    const uint32_t cap =
        std::min( RT_ARC_MARK_MAX, uint32_t( std::max( 0, int{ cvar::rt_arc_max } ) ) );
    if( cap == 0 )
    {
        return;
    }

    uint32_t slot;
    if( s_arcCount < cap )
    {
        slot = s_arcCount++;
    }
    else
    {
        // Oldest out. Marks all share one lifetime, so unlike the spark pool
        // there is no kind to evict within and plain oldest-out is correct.
        slot = 0;
        for( uint32_t j = 1; j < s_arcCount; j++ )
        {
            if( s_arcs[ j ].age > s_arcs[ slot ].age )
            {
                slot = j;
            }
        }
    }

    ArcMark& m = s_arcs[ slot ];

    // THE BASIS IS RESOLVED ONCE, HERE, AND STORED. Rebuilding it per frame from
    // the normal would give the same vectors -- it is deterministic -- but it
    // would also mean the filigree silently reorients if the derivation ever
    // changed, and a mark that rotates on the wall between frames is the exact
    // artefact this system cannot afford. Cheaper to store six floats.
    FVector3 t = std::abs( normal.Z ) < 0.9f ? ( normal ^ FVector3{ 0, 0, 1 } )
                                             : ( normal ^ FVector3{ 1, 0, 0 } );
    if( t.LengthSquared() < 1e-8f )
    {
        t = FVector3{ 1, 0, 0 };
    }
    t.MakeUnit();

    m.at   = at;
    m.nrm  = normal;
    m.tan  = t;
    m.bit  = normal ^ t;
    m.age       = 0.f;
    m.arcs      = withArcs;
    m.burnScale  = burnScale;
    m.fx         = fx;
    m.emberScale = std::max( 0.f, emberScale );
    m.emberArt   = emberArt;
    m.emberSize   = std::max( 0.05f, emberSize );
    m.emberBright  = std::max( 0.f, emberBright );
    m.emberScatter = std::max( 0.f, emberScatter );
    // THE DELAY IS THE FIX FOR DOUBLE SMOKE, designed in rather than tuned
    // later. The rocket's own death burst (RT_PROJECTILE_SMOKE / rt_smoke_boom)
    // goes off at this same point on this same frame. Embers breathing from
    // t = 0 give one opaque ball with the embers invisible inside it -- and the
    // embers are the entire point of the effect.
    m.nextSmoke = std::max( 0.f, float{ cvar::rt_ember_smoke_delay } );
    // SCALED BY THE WEAPON, and resolved here rather than at draw time: two
    // marks from different weapons can be alive at once.
    m.arcLife = withArcs
                    ? std::max( 0.05f, float{ cvar::rt_arc_life } ) * ArcStyleFor( flavor ).scale
                    : 0.f;
    // The SCORCH's life when there is one, otherwise just the arcs'. Never
    // shorter than the arcs, or the mark would be evicted out from under a
    // filigree that is still drawing.
    m.life = std::max( 0.05f, m.arcLife );
    if( cvar::rt_arc_burn )
    {
        // rt_arc_burn_life 0 MEANS FOREVER, and forever is really "until the
        // pool evicts it". A scorch has no reason to weather on the timescale of
        // a firefight -- what bounds how far back the wall damage goes is
        // rt_arc_max, not a clock, and saying so with a sentinel is clearer than
        // picking a number large enough to look permanent.
        const float bl = float{ cvar::rt_arc_burn_life };
        m.life = bl <= 0.f ? FLT_MAX : std::max( m.life, std::max( 0.05f, bl ) );
    }
    m.flavor = flavor;

    // The mark's IDENTITY. Every branch angle, every segment length and every
    // flicker phase is hashed from this, so the same impact always draws the
    // same skeleton and two impacts never draw the same one.
    static uint32_t s_nextArcUid = 1;
    m.uid                        = s_nextArcUid++;
    m.seed                       = m.uid * 2654435761u + 0x9E3779B9u;

    s_dbgSpawned++;
}

// WHERE A MARK'S EMBERS SIT, in world metres. Derived from the mark's seed, so
// the draw and the smoke agree by construction rather than by both being given
// the same numbers -- the arcs' creepers had exactly this split and it is the
// kind of duplication that ends with lights hanging where no geometry is.
//
// Scattered on a disc across the scorch, sqrt-weighted so they spread over the
// churn instead of clustering at its centre. They do NOT move: an ember is a hot
// spot burnt into the wall, and the whole correction that produced this version
// was that they are not fragments in flight.
FVector3 EmberPos( const ArcMark& m, int i, float burnRad )
{
    const uint32_t es = m.seed + 0x2545F491u + uint32_t( i ) * 2654435761u;
    const float    a  = hash01( es ) * 2.f * rt_pi();
    const float    r  = std::sqrt( hash01( es * 7919u ) ) * burnRad *
                     std::clamp( float{ cvar::rt_ember_scatter } * m.emberScatter, 0.f, 8.f );
    return m.at + m.tan * ( std::cos( a ) * r ) + m.bit * ( std::sin( a ) * r );
}

// ONE PLACE, because the draw and the smoke both need the answer and a mark
// whose coals are drawn N times but smoked M times puts smoke over bare floor.
int EmberCountFor( const ArcMark& m )
{
    const float base = float( std::max( 0, int{ cvar::rt_ember_count } ) );
    return std::clamp( int( std::lround( base * std::max( 0.f, m.emberScale ) ) ),
                       0,
                       RT_ARC_MAX_EMBER );
}

// Aged from the spark sim, which already owns the tic-delta machinery. Marks do
// not move, so there is nothing to integrate -- only the clock.
void AgeArcMarks( float dt )
{
    // THE WISPS ARE REAL PUFFS in the froxel volume, not a private particle
    // system, so with the smoke system off there is nothing for them to spawn
    // into and asking is pointless. Stated here rather than discovered as "the
    // embers do not smoke".
    const bool wantSmoke =
        cvar::rt_ember && cvar::rt_ember_smoke && cvar::rt_smoke && cvar::rt_arc_burn;

    const float emberLife = std::max( 0.05f, float{ cvar::rt_ember_life } );
    const float every     = std::max( 0.02f, float{ cvar::rt_ember_smoke_every } );
    const float hotFrac   = std::clamp( float{ cvar::rt_ember_smoke_hot }, 0.f, 1.f );
    const float burnRad   = std::max( 0.f, float{ cvar::rt_arc_burn_radius } );

    // PER TIC, ACROSS ALL MARKS. Several rockets landing together would
    // otherwise have every mark breathe on the same tic, which is a wall of
    // smoke rather than a few threads -- the density failure this feature was
    // specifically warned about before a line of it was written.
    int budget = std::max( 0, int{ cvar::rt_ember_smoke_max } );

    for( uint32_t i = 0; i < s_arcCount; )
    {
        ArcMark& m = s_arcs[ i ];
        m.age += dt;
        if( m.age >= m.life )
        {
            s_arcs[ i ] = s_arcs[ --s_arcCount ];
            continue;
        }

        // PER MARK, not once for the frame: a barrel's scorch carries more coals
        // than a rocket's, and both can be alive at the same moment.
        const int nEmber = EmberCountFor( m );

        if( wantSmoke && m.fx == ImpactFx::Ember && nEmber > 0 && budget > 0 &&
            m.age >= m.nextSmoke )
        {
            m.nextSmoke = m.age + every * ( 0.7f + 0.6f * rnd01() );

            // ONLY WHILE THE EMBERS ARE STILL HOT. A black ember is a smudge in
            // the scorch, and smoke still coming off one is the tell that the
            // two were never really connected.
            const float et = m.age / emberLife;
            if( et < hotFrac )
            {
                // A FEW EMBERS PER BREATH, SCALED TO HOW MANY THERE ARE, rather
                // than all of them and rather than always exactly one.
                //
                // All of them at once reads as a single plume, which is the
                // failure the delay above exists to avoid re-creating a second
                // later -- so it stays a rotating subset.
                //
                // But exactly ONE was tuned for a rocket's five coals, and it
                // does not survive the barrel's fifty: one wisp every 0.3 s
                // across fifty embers means any given coal breathes about once a
                // quarter of a MINUTE, so the bed reads as not smoking at all.
                // Reported as "there is no smoke trail on the embers". The
                // fraction below keeps a rocket at exactly its old single wisp
                // and gives the barrel a handful, and the per-tic budget still
                // bounds the whole thing.
                const int picks = std::clamp( nEmber / 6, 1, std::min( 8, budget ) );

                for( int pk = 0; pk < picks; pk++ )
                {
                const int pick = int( hash01( m.seed + uint32_t( m.age * 977.f ) +
                                              uint32_t( pk ) * 2654435761u ) *
                                      float( nEmber ) ) %
                                 std::max( 1, nEmber );

                SmokeProfile p{};
                p.cls = "ember";
                // EXACTLY ONE PARCEL. rt_smoke_count is the shared parcel count
                // and this profile wants a single wisp whatever it is set to, so
                // the multiplier has to cancel it -- the expression
                // RT_AmbientSmoke uses, for the same reason.
                p.count  = 1.f / std::max( 1.f, float( int{ cvar::rt_smoke_count } ) );
                p.radius = std::max( 0.01f, float{ cvar::rt_ember_smoke_radius } ) /
                           std::max( 0.001f, float{ cvar::rt_smoke_radius } );
                p.density = std::max( 0.f, float{ cvar::rt_ember_smoke_density } );
                p.life    = std::max( 0.05f, float{ cvar::rt_ember_smoke_life } );
                // IT MUST DRIFT OFF THE WALL, and speed 0 was the bug.
                //
                // An ambient emitter can have speed 0 because it stands in open
                // air and only needs to rise. An ember is ON a vertical surface:
                // with no outward velocity the parcel rises straight up HUGGING
                // the wall, half inside it, for its entire life. `forward` below
                // is the outward direction, and this is how much of it is used.
                p.speed = std::max( 0.f, float{ cvar::rt_ember_smoke_drift } ) /
                          std::max( 0.01f, float{ cvar::rt_smoke_speed } );
                p.spread  = 0.10f;
                p.rise    = 1.4f;  // the pistol's 1.5, near enough
                // THE PISTOL'S GROWTH, and this is the value that matters most.
                // Growth is what separates a thread from a ball: a long-lived
                // parcel keeps expanding at the shared rate, so a thin profile
                // has to slow its expansion as well as start small. Getting this
                // wrong is what once turned the pistol's own wisp into a 1.6 m
                // cloud, and an ember is a smaller emitter again.
                p.growth     = 0.14f;
                p.trail      = 0; // the countdown above IS the trail
                p.trailEvery = 0;
                p.note       = "rocket ember";
                // AMBIENT, so rt_smoke_ambient_budget bounds these as well and
                // a rocket barrage cannot starve the player's own muzzle smoke.
                p.ambient = true;

                if( p.density > 0.f )
                {
                    // THE LIFT HAS TO CLEAR THE PARCEL, NOT JUST THE SURFACE,
                    // and a fixed 4 cm did not -- which is why the wisps read as
                    // being inside the wall.
                    //
                    // Three things stack up, and only the first is obvious:
                    //   1. the parcel is a SPHERE of rt_smoke_radius *
                    //      prof.radius (~6 cm at the shipping values), so a
                    //      centre 4 cm out leaves two thirds of it buried;
                    //   2. RT_SpawnSmokePuffs then jitters the birthplace by
                    //      +/- 0.6 * radius on ALL THREE AXES -- including
                    //      straight into the wall, so some parcels were born
                    //      with their centres inside it;
                    //   3. with speed 0 it never drifts out, so it stays there
                    //      for its whole life.
                    //
                    // Derived from the parcel radius rather than being another
                    // magic number, so it stays correct if the radius moves.
                    const float parcelR =
                        std::max( 0.01f, float{ cvar::rt_ember_smoke_radius } );
                    const float lift =
                        parcelR * std::max( 0.f, float{ cvar::rt_ember_smoke_lift } );

                    const FVector3 ep = EmberPos( m, pick, burnRad * m.burnScale ) + m.nrm * lift;

                    // OUTWARD AND UP. On a wall the two are perpendicular and
                    // the parcel needs both -- away from the surface so it
                    // clears, upward because smoke rises. On a floor the normal
                    // IS up and the blend degenerates to straight up on its own,
                    // so this needs no special case for horizontal surfaces.
                    FVector3 dir = m.nrm * 0.8f + FVector3{ 0, 0, 0.6f };
                    if( dir.LengthSquared() < 1e-8f )
                    {
                        dir = m.nrm;
                    }
                    dir.MakeUnit();

                    RT_SpawnSmokePuffs( ep, ep, dir, FVector3{ 0, 0, 0 }, p );
                    budget--;
                    s_dbgEmberSmoke++;
                }
                } // picks
            }
        }

        i++;
    }
}


// ---------------------------------------------------------------------------
// The projectile-death walk that feeds the arcs.
// ---------------------------------------------------------------------------

struct ProjMark
{
    AActor*   mo;      // identity only; NEVER dereferenced after it stops being seen
    FVector3  lastPos; // MAP UNITS, the last position while still in flight
    FVector3  dir;     // unit, direction of travel
    float     speed;   // MAP UNITS per tic, its actual last speed
    ArcFlavor flavor;
    ImpactFx  fx;      // arcs or embers -- resolved at tracking, not at death
    int       lastTic;
};
std::vector< ProjMark > g_projs;

size_t TrackedProjectileCount()
{
    return g_projs.size();
}

// Which projectiles arc. The MF_MISSILE test is the load-bearing half and it is
// FIRST for the same reason rt_smoke.cpp:453 puts it first: P_ExplodeMissile
// clears the flag while the actor lives on, same class, through its whole death
// animation. So losing the flag IS the impact -- it is a real edge on the right
// frame, and it needs no DECORATE edit and no ZScript, which matters because
// these classes belong to the WAD and may not be edited.
const ArcSource* ArcSourceFor( AActor* mo )
{
    if( !mo || !mo->GetClass() || !( mo->flags & MF_MISSILE ) )
    {
        return nullptr;
    }
    const char* c = mo->GetClass()->TypeName.GetChars();
    if( !c || ArcClassExcluded( c ) )
    {
        return nullptr;
    }
    for( const ArcSource& s : RT_ARC_SOURCES )
    {
        if( strstr( c, s.cls ) != nullptr )
        {
            return &s;
        }
    }
    return nullptr;
}

// A dead projectile's last recorded position is up to a tic of flight short of
// whatever it hit, in mid air, with no normal and no surface. This recovers all
// three by re-tracing the last leg. Returns false when there is nothing there --
// a bolt that detonated on a monster or in open air, which is a legitimate
// outcome and simply produces no arcs.
bool ProbeImpactSurface( const ProjMark& m, FVector3* outAt, FVector3* outNrm, sector_t** outSec )
{
    // FROM THE PROJECTILE'S OWN SPEED, never a fixed distance. UnmakerLaser is
    // Speed 200 and 64PlasmaBall is Speed 40: one fixed probe cannot serve both,
    // and the failure is silent in both directions -- too short finds nothing
    // for the fast one, too long punches through the wall into the next room for
    // the slow one and puts the arcs on a surface the player never saw hit.
    float len = std::max( 1.f, m.speed ) * std::max( 0.1f, float{ cvar::rt_arc_probe } );
    len       = std::min( len, std::max( 1.f, float{ cvar::rt_arc_probe_max } ) );

    sector_t* sec = primaryLevel->PointInSector( double( m.lastPos.X ), double( m.lastPos.Y ) );

    const DVector3 start{ double( m.lastPos.X ), double( m.lastPos.Y ), double( m.lastPos.Z ) };
    const DVector3 dir{ double( m.dir.X ), double( m.dir.Y ), double( m.dir.Z ) };

    FTraceResults res{};
    // The flags MUST NOT include TRACE_Impact or TRACE_PCross: those trigger
    // SPAC_IMPACT and SPAC_PCROSS line specials, and renderer particles firing
    // map specials would be a gameplay change and a netgame desync in a system
    // the player cannot see. Empty ActorMask so the probe passes THROUGH
    // monsters -- a bolt that killed something still hit the wall behind it, and
    // stopping on the corpse would put the arcs in mid-air. TRACE_NoSky so a
    // bolt that reached sky produces nothing.
    if( !Trace( start,
                sec,
                dir,
                double( len ),
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
        return false;
    }

    FVector3 n;
    if( res.HitType == TRACE_HitFloor )
    {
        n = FVector3{ 0, 0, 1 };
    }
    else if( res.HitType == TRACE_HitCeiling )
    {
        n = FVector3{ 0, 0, -1 };
    }
    else if( res.HitType == TRACE_HitWall && res.Line != nullptr )
    {
        // Same construction as SparkHitWall: the linedef perpendicular, flipped
        // for the back side, then forced to oppose the direction of travel
        // whichever way the geometry happens to be wound.
        const double dx = res.Line->Delta().X;
        const double dy = res.Line->Delta().Y;
        n               = FVector3{ float( dy ), float( -dx ), 0.f };
        if( n.LengthSquared() < 1e-8f )
        {
            return false;
        }
        n.MakeUnit();
        if( res.Side != 0 )
        {
            n = -n;
        }
        if( ( m.dir | n ) > 0.f )
        {
            n = -n;
        }
    }
    else
    {
        return false;
    }

    *outAt  = FVector3{ float( res.HitPos.X ) * ONEGAMEUNIT_IN_METERS,
                       float( res.HitPos.Y ) * ONEGAMEUNIT_IN_METERS,
                       float( res.HitPos.Z ) * ONEGAMEUNIT_IN_METERS };
    *outNrm = n;
    *outSec = res.Sector ? res.Sector : sec;
    return true;
}

} // namespace rtsp

// The entry point below is at GLOBAL scope, beside every other RT_* call in
// rt_internal.h. The directive goes HERE rather than at the top of the file:
// with it in scope while defining things INSIDE the namespace, every unqualified
// name is findable twice and MSVC reports an ambiguous call to
// rtsp::rtsp::whatever, which is a confusing way to say it.
using namespace rtsp;

void RT_UpdateProjectileImpacts()
{
    // THE GATE IS THE UNION, because two features ride this one walk. Testing
    // rt_arc alone would have made turning the arcs off silently take barrel
    // destruction with it -- exactly the invisible coupling this walk exists to
    // avoid, and it would have looked like a barrel bug rather than a gate. Each
    // branch below still tests its OWN cvar, so either can be judged with the
    // other out of the way.
    if( !primaryLevel || !( cvar::rt_arc || cvar::rt_barrel ) )
    {
        g_projs.clear();
        BarrelForgetAll();
        return;
    }

    const int  tic = primaryLevel->maptime;
    static int s_lastProjTic = -1;

    // A backwards or enormous jump is a level change, a load or a warp. Every
    // AActor* held below belongs to the old level and must not be touched --
    // dropping the marks is the only safe response, and the cost is that a
    // projectile in flight across a save-load produces no arcs.
    if( tic < s_lastProjTic || tic - s_lastProjTic > TICRATE )
    {
        g_projs.clear();
        BarrelForgetAll();
    }
    if( tic == s_lastProjTic )
    {
        return;
    }
    s_lastProjTic = tic;

    auto it = primaryLevel->GetThinkerIterator< AActor >();
    while( AActor* mo = it.Next() )
    {
        // BARRELS RIDE THIS WALK. They are not projectiles and share none of the
        // logic below -- their edge is a sprite frame, not a disappearance --
        // but the walk itself is the expensive part and it is already happening.
        // See the note at the top of rt_barrel.cpp.
        BarrelWalkActor( mo, tic );

        const ArcSource* src = ArcSourceFor( mo );
        if( !src )
        {
            continue;
        }

        const FVector3 p{ float( mo->Pos().X ), float( mo->Pos().Y ), float( mo->Pos().Z ) };

        // DIRECTION FROM Vel, not from differencing positions. A FastProjectile
        // moves in substeps and its net per-tic displacement can be a poor guide
        // to where it was actually pointing; Vel is what the playsim itself
        // steered it by.
        FVector3    v{ float( mo->Vel.X ), float( mo->Vel.Y ), float( mo->Vel.Z ) };
        const float vlen = v.Length();

        ProjMark* mark = nullptr;
        for( ProjMark& r : g_projs )
        {
            if( r.mo == mo )
            {
                mark = &r;
                break;
            }
        }

        if( !mark )
        {
            if( vlen < 1e-4f )
            {
                continue; // not moving yet; nothing to aim a probe along
            }
            g_projs.push_back( ProjMark{ mo, p, v / vlen, vlen, src->flavor, src->fx, tic } );
            if( cvar::rt_arc_debug )
            {
                Printf( "rt_arc: tracking %s -> %s at %.0f %.0f %.0f (speed %.1f, tic %d)\n",
                        mo->GetClass()->TypeName.GetChars(),
                        RT_ARC_STYLES[ int( src->flavor ) ].name,
                        p.X,
                        p.Y,
                        p.Z,
                        vlen,
                        tic );
            }
            continue;
        }

        mark->lastPos = p;
        mark->lastTic = tic;
        if( vlen > 1e-4f )
        {
            // Keep the LAST GOOD heading. A projectile whose velocity is zeroed
            // on the tic it dies would otherwise leave the probe with no
            // direction at all, which is the one case that must still work.
            mark->dir   = v / vlen;
            mark->speed = vlen;
        }
    }

    BarrelSweep( tic );

    // THE SWEEP IS THE IMPACT. Anything tracked last tic and not seen this one
    // has either lost MF_MISSILE (it exploded) or left the thinker list. Its
    // AActor* is not dereferenced here -- only the position, direction and
    // speed recorded while it was alive.
    for( size_t i = 0; i < g_projs.size(); )
    {
        ProjMark& m = g_projs[ i ];
        if( m.lastTic == tic )
        {
            i++;
            continue;
        }

        // The distance cull, before the trace rather than after: the probe is
        // the only unbounded cost in this system, and an impact across the map
        // should not pay for one. Same reasoning as rt_spark_far.
        const auto&    vp = r_viewpoint;
        const FVector3 eye{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                            float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                            float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };
        const FVector3 lastM = m.lastPos * ONEGAMEUNIT_IN_METERS;
        // Each effect has its own reach. Reading rt_arc_far for a rocket would
        // be a silent coupling of exactly the kind this file keeps having to
        // undo -- turning the arcs' cull down would quietly stop distant rockets
        // leaving embers, with nothing to connect the two.
        const float    far_m = std::max( 0.f,
                                         m.fx == ImpactFx::Ember ? float{ cvar::rt_ember_far }
                                                                 : float{ cvar::rt_arc_far } );

        if( ( lastM - eye ).LengthSquared() <= far_m * far_m )
        {
            FVector3  at{};
            FVector3  n{};
            sector_t* sec = nullptr;
            if( ProbeImpactSurface( m, &at, &n, &sec ) )
            {
                if( m.fx == ImpactFx::Ember )
                {
                    // THE ROCKET: a churn on the floor plus scattered embers.
                    // The scorch is the SAME decal the arcs leave, at a bigger
                    // radius and with the filigree switched off -- a rocket
                    // blast is not electric.
                    SpawnArcMark( at,
                                  n,
                                  m.flavor,
                                  /*withArcs=*/false,
                                  std::max( 0.f, float{ cvar::rt_ember_burn_scale } ),
                                  ImpactFx::Ember );
                }
                else
                {
                    (void)sec; // a mark does not move, so it needs no sector
                    SpawnArcMark( at, n, m.flavor );
                }
                if( cvar::rt_arc_debug )
                {
                    Printf( "rt_arc: IMPACT %s at %.2f %.2f %.2f  n %.2f %.2f %.2f (tic %d)\n",
                            RT_ARC_STYLES[ int( m.flavor ) ].name,
                            at.X,
                            at.Y,
                            at.Z,
                            n.X,
                            n.Y,
                            n.Z,
                            tic );
                }
            }
            else if( cvar::rt_arc_debug )
            {
                // The distinction that costs a session if it is not printed:
                // "the projectile was never tracked" and "tracked, but the probe
                // found no surface" are identical on screen and have completely
                // different fixes -- a class-match bug against a probe-length or
                // geometry one.
                Printf( "rt_arc: died with NO SURFACE (%s) at %.0f %.0f %.0f "
                        "speed %.1f (tic %d) -- monster, mid-air or sky\n",
                        RT_ARC_STYLES[ int( m.flavor ) ].name,
                        m.lastPos.X,
                        m.lastPos.Y,
                        m.lastPos.Z,
                        m.speed,
                        tic );
            }
        }

        m = g_projs.back();
        g_projs.pop_back();
    }
}

// ---------------------------------------------------------------------------
// THE LAB HOOK. Plant a mark on whatever the camera is looking at.
//
//   arc_here            a plasma mark (scorch + filigree)
//   arc_here bfg        the BFG's, at double scale
//   arc_here arach      the arachnotron's palette
//   arc_here ember      a rocket mark: scorch + embers, no filigree
//
// WHY THIS EXISTS. Judging the scorch by firing a rocket at a wall means the
// mark lands wherever the blast happened to be, at whatever angle, with the
// explosion's own smoke and light on top of it for the first second. None of
// that is the thing being looked at, and none of it repeats between runs -- so
// two captures of "the same" mark differ in a dozen ways and the one difference
// you changed is impossible to isolate.
//
// This puts one mark, of a stated kind, exactly where the crosshair is, with
// nothing else happening. It is what makes a screenshot loop worth running:
// change a value, rebuild, capture, compare, and the ONLY thing that moved is
// the value. The seed still varies per mark, which is deliberate -- the shape
// is supposed to be different every time and a capture that hid that would be
// lying about the effect.
CCMD( arc_here )
{
    if( !primaryLevel )
    {
        Printf( "arc_here: no level\n" );
        return;
    }

    ArcFlavor flavor  = ArcFlavor::Plasma;
    ImpactFx  fx      = ImpactFx::Arc;
    bool      wantArc = true;
    float     scale   = 1.f;

    if( argv.argc() > 1 )
    {
        const FString a = argv[ 1 ];
        if( a.CompareNoCase( "bfg" ) == 0 )
        {
            flavor = ArcFlavor::BFG;
        }
        else if( a.CompareNoCase( "arach" ) == 0 )
        {
            flavor = ArcFlavor::Arach;
        }
        else if( a.CompareNoCase( "ember" ) == 0 )
        {
            fx      = ImpactFx::Ember;
            wantArc = false;
            scale   = std::max( 0.f, float{ cvar::rt_ember_burn_scale } );
        }
        else if( a.CompareNoCase( "plasma" ) != 0 )
        {
            Printf( "arc_here: expected plasma | bfg | arach | ember\n" );
            return;
        }
    }

    const auto&    vp    = r_viewpoint;
    const double   yaw   = vp.Angles.Yaw.Radians();
    const double   pitch = vp.Angles.Pitch.Radians();
    const DVector3 dir{ std::cos( yaw ) * std::cos( pitch ),
                        std::sin( yaw ) * std::cos( pitch ),
                        -std::sin( pitch ) };

    sector_t* sec = primaryLevel->PointInSector( vp.Pos.X, vp.Pos.Y );

    FTraceResults res{};
    // Same flags as the projectile probe: no TRACE_Impact or TRACE_PCross, so
    // this cannot fire a line special, and an empty ActorMask so it goes through
    // monsters to the wall behind.
    if( !Trace( vp.Pos,
                sec,
                dir,
                2048.,
                ActorFlags::FromInt( 0 ),
                ML_BLOCKEVERYTHING,
                nullptr,
                res,
                TRACE_NoSky ) ||
        res.HitType == TRACE_HasHitSky )
    {
        Printf( "arc_here: nothing in front of you within 2048 units\n" );
        return;
    }

    FVector3 n;
    if( res.HitType == TRACE_HitFloor )
    {
        n = FVector3{ 0, 0, 1 };
    }
    else if( res.HitType == TRACE_HitCeiling )
    {
        n = FVector3{ 0, 0, -1 };
    }
    else if( res.HitType == TRACE_HitWall && res.Line != nullptr )
    {
        const double dx = res.Line->Delta().X;
        const double dy = res.Line->Delta().Y;
        n               = FVector3{ float( dy ), float( -dx ), 0.f };
        if( n.LengthSquared() < 1e-8f )
        {
            Printf( "arc_here: degenerate linedef\n" );
            return;
        }
        n.MakeUnit();
        if( res.Side != 0 )
        {
            n = -n;
        }
        const FVector3 d3{ float( dir.X ), float( dir.Y ), float( dir.Z ) };
        if( ( d3 | n ) > 0.f )
        {
            n = -n;
        }
    }
    else
    {
        Printf( "arc_here: hit nothing usable (type %d)\n", int( res.HitType ) );
        return;
    }

    const FVector3 at{ float( res.HitPos.X ) * ONEGAMEUNIT_IN_METERS,
                       float( res.HitPos.Y ) * ONEGAMEUNIT_IN_METERS,
                       float( res.HitPos.Z ) * ONEGAMEUNIT_IN_METERS };

    SpawnArcMark( at, n, flavor, wantArc, scale, fx );

    Printf( "arc_here: %s mark at %.0f %.0f %.0f, normal %.2f %.2f %.2f (%u live)\n",
            fx == ImpactFx::Ember ? "ember" : RT_ARC_STYLES[ int( flavor ) ].name,
            res.HitPos.X,
            res.HitPos.Y,
            res.HitPos.Z,
            n.X,
            n.Y,
            n.Z,
            s_arcCount );
}

