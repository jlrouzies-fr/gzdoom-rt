// Doom64-RT: WHAT AN EXPLODING BARREL LEAVES BEHIND.
//
// A barrel currently goes off, throws a smoke burst, and leaves the room
// exactly as it found it. It should leave a SCENE: a scorch churned into the
// floor under it, and pieces of the barrel itself lying about.
//
// THREE THINGS TO KNOW BEFORE CHANGING ANYTHING HERE.
//
// 1. THE TRIGGER IS NOT THE ACTOR DISAPPEARING, and this is the one place the
//    projectile rule in rt_impacts.cpp cannot be reused. A projectile is gone
//    the tic after it explodes, which is what makes "tracked last tic, absent
//    this one" a reliable impact edge. `64ExplosiveBarrel` does the opposite:
//    after A_Explode it lingers for a 1050-tic respawn timer, so that rule
//    would put the whole effect roughly twenty SECONDS late. The health test
//    the monster paths use is no help either -- a barrel in its death state has
//    no health, that is the point of the state.
//
//    So the edge is the SPRITE FRAME, exactly as RT_BarrelSmoke already finds
//    it: BEXP frame E is where A_Explode sits in the Death state. Frame E is
//    held for 5 + 7 + 9 tics across three state lines, so the rising edge has
//    to be detected rather than merely tested for, or everything below fires
//    twenty-one times.
//
// 2. IT DOES NOT RIDE rt_smoke'S WALK, though that is where the barrel's smoke
//    burst lives and the edge detection there is identical. Hanging off it
//    would make barrel debris depend on rt_smoke AND rt_smoke_barrel as well as
//    its own cvar -- three-deep gating for an effect that is not smoke, which
//    is the coupling docs/plan-projectile-impact-fx.md 2 rejects and the reason
//    the projectile impacts got their own walk. This rides the IMPACT walk
//    instead, which is already visiting every actor once a tic for free.
//
// 3. THE CHUNKS ARE NOT DEBRIS CHIPS. That was the explicit ask -- "bigger
//    pieces that really look like the barrel metal sprite parts, not just
//    square particle pixels" -- and it is a construction difference, not a size
//    multiplier. A debris chip is a camera-facing billboard, which is invisible
//    as a cheat at 2 cm and glaring at 25 cm. SparkKind::Shard exists for that
//    reason and rt_spark_draw.cpp builds it as world-oriented curved plate.
//    See the note there before reaching for a size cvar.

#include "rt_sparks_internal.h"

// NOTE: no `using namespace rtsp;` up here -- this file DEFINES things in that
// namespace, and a using-directive in scope while doing so makes every
// unqualified name findable twice. It goes after the closing brace, for the
// global-scope CCMD below. Same as rt_impacts.cpp.
using namespace rtx;

namespace rtsp
{

// One tracked barrel. Deliberately the smallest thing that can answer "was this
// actor already in frame E last time I looked": the AActor* is NEVER
// dereferenced outside the walk that just handed it to us, so a barrel removed
// between tics can only ever leave a stale entry, which the sweep drops.
struct BarrelMark
{
    AActor* mo;
    int     lastTic;
    bool    blowing;
};

std::vector< BarrelMark > g_barrels;

// ---------------------------------------------------------------------------

// The barrel's own palette, one entry per shard, held for its whole life.
//
// Spread across the sprite's value range rather than drawn from the sprite by
// pixel frequency -- see the note on RT_BARREL_SHADES. A `sid`-keyed hash rather
// than the sim RNG so that the same shard is the same colour every frame, which
// matters here in a way it does not for a chip: a quarter-metre plate that
// changed shade between frames would be the single most visible thing on screen.
uint32_t ShardShade( uint32_t sid )
{
    const int i = std::clamp( int( hash01( sid * 0x27D4EB2Fu ) * float( RT_BARREL_SHADES_N ) ),
                              0,
                              RT_BARREL_SHADES_N - 1 );
    return RT_BARREL_SHADES[ i ];
}

// `at` is METRES; `up` is the direction the burst is biased toward, normally the
// floor's normal under the barrel.
void SpawnBarrelShards( const FVector3& at, const FVector3& up )
{
    if( !primaryLevel || !cvar::rt_barrel || !cvar::rt_barrel_debris )
    {
        return;
    }

    const int want = std::max( 0, int{ cvar::rt_barrel_count } );
    if( want == 0 )
    {
        return;
    }

    const DebrisProfile& pr    = ProfileFor( SurfKind::Barrel );
    const float          size  = std::max( 0.01f, float{ cvar::rt_barrel_size } );
    const float          life  = std::max( 0.5f, float{ cvar::rt_barrel_life } );
    const float          speed = std::max( 0.f, float{ cvar::rt_barrel_speed } );
    const float          bias  = std::max( 0.f, float{ cvar::rt_barrel_up } );

    FVector3 upn = up;
    if( upn.LengthSquared() < 1e-6f )
    {
        upn = FVector3{ 0, 0, 1 };
    }
    upn.MakeUnit();

    // The sector the burst happens in, resolved ONCE. Every shard starts in it,
    // and the sim's tier-1 collision needs a starting sector or it cannot find a
    // floor to bounce off -- a shard born with a null sector falls forever.
    sector_t* sec = primaryLevel->PointInSector( double( at.X ) / double{ ONEGAMEUNIT_IN_METERS },
                                                double( at.Y ) / double{ ONEGAMEUNIT_IN_METERS } );

    int made = 0;
    for( int i = 0; i < want; i++ )
    {
        Spark* slotp = AllocSpark( SparkKind::Shard );
        if( !slotp )
        {
            break;
        }
        Spark& sp = *slotp;

        // A FULL SPHERE, then biased up. A barrel bursts outward in every
        // direction -- unlike an impact, there is no surface to reflect off and
        // no cone to build -- but with a purely spherical throw half the plate
        // goes straight into the floor and is never seen. The bias is what puts
        // the burst in the air where it can be watched.
        FVector3 dir{ rnd11(), rnd11(), rnd11() };
        if( dir.LengthSquared() < 1e-6f )
        {
            dir = upn;
        }
        dir.MakeUnit();
        dir += upn * bias;
        dir.MakeUnit();

        // Born a little off the centre rather than all from one point, or the
        // burst reads as a fountain from a pinhole instead of a barrel coming
        // apart. Scaled by the piece size so it stays proportionate.
        const FVector3 jitter{ rnd11(), rnd11(), rnd11() };

        sp.pos     = at + jitter * ( size * 1.5f );
        sp.vel     = dir * ( speed * ( 0.45f + 0.85f * rnd01() ) );
        sp.age     = 0.f;
        sp.life    = life * ( 0.75f + 0.5f * rnd01() );
        // A WIDE size spread, wider than debris takes. A barrel does not come
        // apart into equal pieces: there are two or three big plates and a
        // scatter of smaller stuff, and uniform chunks read as manufactured.
        sp.size    = size * ( 0.45f + 1.25f * rnd01() );
        sp.sec     = sec;
        sp.settled = false;
        sp.kind    = SparkKind::Shard;
        sp.surf    = SurfKind::Barrel;

        // THE BIRTH ORIENTATION, and for a shard this is real geometry rather
        // than the shading hint it is for debris: it is the plate's own face
        // normal, which the draw tumbles about a world axis and which the sim
        // overwrites with the floor's normal when the piece comes to rest.
        // Random, because a barrel's staves point every way round it.
        FVector3 n{ rnd11(), rnd11(), rnd11() };
        if( n.LengthSquared() < 1e-6f )
        {
            n = upn;
        }
        n.MakeUnit();
        sp.nrm = n;

        sp.phase  = rnd01() * 2.f * rt_pi();
        sp.spin   = rnd11() * 3.2f * pr.spin;
        sp.aspect = pr.aspectLo + rnd01() * ( pr.aspectHi - pr.aspectLo );
        sp.sid    = NextSparkSid();
        // Resolved from the sid so it cannot change frame to frame.
        sp.baseRgb = ShardShade( sp.sid );

        made++;
        s_dbgSpawned++;
    }

    if( cvar::rt_barrel_debug )
    {
        Printf( "rt_barrel: %d shards at %.2f %.2f %.2f (pool %u)\n",
                made,
                at.X,
                at.Y,
                at.Z,
                g_sparkCount );
    }
}

// ---------------------------------------------------------------------------

// Everything one exploding barrel produces. `mo` is alive here -- this is called
// straight out of the walk that found it.
void BarrelBlow( AActor* mo )
{
    // THE SCORCH GOES ON THE FLOOR, NOT AT THE BARREL'S MIDDLE. A barrel is 50
    // units tall and the blast is modelled at its centre, so a mark placed at
    // the actor's position would hang in mid-air a metre up, facing nothing.
    const double px = mo->X();
    const double py = mo->Y();

    sector_t* sec = mo->Sector ? mo->Sector : primaryLevel->PointInSector( px, py );

    double   fz = mo->Z();
    FVector3 fn{ 0, 0, 1 };
    if( sec )
    {
        fz = sec->floorplane.ZatPoint( px, py );
        const DVector3 n = sec->floorplane.Normal();
        FVector3       nf{ float( n.X ), float( n.Y ), float( n.Z ) };
        if( nf.LengthSquared() > 1e-6f )
        {
            nf.MakeUnit();
            fn = nf;
        }
    }

    const FVector3 floorAt{ float( px ) * ONEGAMEUNIT_IN_METERS,
                            float( py ) * ONEGAMEUNIT_IN_METERS,
                            float( fz ) * ONEGAMEUNIT_IN_METERS };

    // The burst centre: the barrel's own middle, which is where the fireball is.
    const FVector3 midAt{ float( px ) * ONEGAMEUNIT_IN_METERS,
                          float( py ) * ONEGAMEUNIT_IN_METERS,
                          float( mo->Z() + mo->Height * 0.5 ) * ONEGAMEUNIT_IN_METERS };

    // A SPAWN cull, not a render cull, for the reason rt_spark_far is one:
    // eviction is oldest-out while the thing worth keeping is the blast in front
    // of you, so a barrel going off across the map must not push it out.
    {
        const auto&    vp = r_viewpoint;
        const FVector3 eye{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                            float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                            float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };
        const float    far_m = std::max( 0.f, float{ cvar::rt_barrel_far } );
        if( ( midAt - eye ).LengthSquared() > far_m * far_m )
        {
            if( cvar::rt_barrel_debug )
            {
                Printf( "rt_barrel: blast at %.0f %.0f CULLED (beyond %.0f m)\n",
                        px,
                        py,
                        far_m );
            }
            return;
        }
    }

    if( cvar::rt_barrel_debug )
    {
        Printf( "rt_barrel: BLAST %s at %.0f %.0f %.0f, floor %.0f (tic %d)\n",
                mo->GetClass()->TypeName.GetChars(),
                px,
                py,
                mo->Z(),
                fz,
                primaryLevel->maptime );
    }

    // THE SCORCH IS THE ROCKET'S, WIDER. Not a new primitive and deliberately
    // not new knobs: it is one kind of thing -- burnt floor -- and the whole
    // rt_arc_burn_* group (darkness, mottle, blob count, lifetime) already
    // describes it. Only the RADIUS is the barrel's own, because that is the
    // only thing a barrel scorch has that a rocket's does not.
    //
    // ImpactFx::Ember rather than Arc is what puts coals in it: the same
    // mechanism the rocket uses, which also gives them their smoke threads for
    // free. An exploding barrel leaving embers in the char is exactly right, and
    // rt_barrel_embers switches it back to a bare scorch if it ever is not.
    if( cvar::rt_barrel_scorch )
    {
        SpawnArcMark( floorAt,
                      fn,
                      ArcFlavor::Plasma, // unread: the ember path never indexes the arc ramp
                      /*withArcs=*/false,
                      std::max( 0.f, float{ cvar::rt_barrel_scorch_scale } ),
                      cvar::rt_barrel_embers ? ImpactFx::Ember : ImpactFx::Arc );
    }

    SpawnBarrelShards( midAt, fn );
}

// ---------------------------------------------------------------------------

void BarrelWalkActor( AActor* mo, int tic )
{
    if( !cvar::rt_barrel || !mo )
    {
        return;
    }

    // BY SPRITE, NOT BY CLASS NAME, and that is the same choice RT_BarrelSmoke
    // makes. The actor classes belong to the Retribution WAD; matching the art
    // catches every barrel that plays the explosion, including any a future WAD
    // adds, without this file having to hold a list of class names it cannot
    // edit.
    if( mo->sprite < 0 || mo->sprite >= int( sprites.Size() ) )
    {
        return;
    }
    const char* sn = sprites[ mo->sprite ].name;
    if( !sn || strnicmp( sn, "BEXP", 4 ) != 0 )
    {
        return;
    }

    BarrelMark* mark = nullptr;
    for( BarrelMark& b : g_barrels )
    {
        if( b.mo == mo )
        {
            mark = &b;
            break;
        }
    }
    if( !mark )
    {
        g_barrels.push_back( BarrelMark{ mo, tic, false } );
        mark = &g_barrels.back();
    }
    mark->lastTic = tic;

    // Frame 'E', where A_Explode sits. Held for 21 tics across three state
    // lines, so this MUST be an edge and not a test.
    const bool blowing = ( mo->frame == 4 );
    const bool edge    = blowing && !mark->blowing;
    mark->blowing      = blowing;

    if( edge )
    {
        BarrelBlow( mo );
    }
}

void BarrelSweep( int tic )
{
    for( size_t i = 0; i < g_barrels.size(); )
    {
        if( g_barrels[ i ].lastTic == tic )
        {
            i++;
            continue;
        }
        g_barrels[ i ] = g_barrels.back();
        g_barrels.pop_back();
    }
}

void BarrelForgetAll()
{
    g_barrels.clear();
}

} // namespace rtsp

using namespace rtsp;

// ---------------------------------------------------------------------------
// THE LAB HOOK, and it exists for the reason `arc_here` does.
//
// Judging barrel plate by finding a barrel and shooting it means the burst
// lands wherever the barrel happened to be, in whatever room, with the
// explosion's own smoke and light on top of it for the first second, and the
// pieces gone before you can walk round them. None of that is the thing being
// looked at and none of it repeats between runs.
//
// This drops one burst at the crosshair with nothing else happening, so a value
// can be changed, rebuilt, captured and compared with only the value having
// moved. The per-shard seeds still vary, which is deliberate: the shapes are
// SUPPOSED to differ, and a capture that hid that would be lying about the
// effect.
//
//   barrel_here          scorch + plate, where you are looking
//   barrel_here shards   plate only, no scorch
//   barrel_here scorch   scorch only, no plate
CCMD( barrel_here )
{
    if( !primaryLevel )
    {
        Printf( "barrel_here: no level\n" );
        return;
    }

    bool wantShards = true;
    bool wantScorch = true;
    if( argv.argc() > 1 )
    {
        if( stricmp( argv[ 1 ], "shards" ) == 0 )
        {
            wantScorch = false;
        }
        else if( stricmp( argv[ 1 ], "scorch" ) == 0 )
        {
            wantShards = false;
        }
    }

    // ON THE FLOOR A SHORT WAY IN FRONT, not at whatever a forward trace hits.
    // A barrel sits on a floor and bursts upward off it; a lab that planted the
    // burst on a wall would be reproducing something the effect never does.
    // Ignoring pitch is the point -- where you happen to be looking must not
    // change the capture.
    // 220 UNITS, AND THE FIRST TRY AT 72 IS WORTH RECORDING. At 72 the burst is
    // about a metre away and lands directly BEHIND THE WEAPON SPRITE, which
    // covers the bottom centre of the frame. The ladder said 12 shards live and
    // 17 quads uploaded while the capture showed an empty room -- so this looked
    // exactly like a draw bug and was a framing one. 220 puts it clear of the
    // gun and still fills a useful part of the frame.
    const auto&  vp  = r_viewpoint;
    const double yaw = vp.Angles.Yaw.Radians();
    const double ox  = vp.Pos.X + std::cos( yaw ) * 220.0;
    const double oy  = vp.Pos.Y + std::sin( yaw ) * 220.0;

    sector_t* sec = primaryLevel->PointInSector( ox, oy );

    double   fz = vp.Pos.Z;
    FVector3 fn{ 0, 0, 1 };
    if( sec )
    {
        fz               = sec->floorplane.ZatPoint( ox, oy );
        const DVector3 n = sec->floorplane.Normal();
        FVector3       nf{ float( n.X ), float( n.Y ), float( n.Z ) };
        if( nf.LengthSquared() > 1e-6f )
        {
            nf.MakeUnit();
            fn = nf;
        }
    }

    const FVector3 floorAt{ float( ox ) * ONEGAMEUNIT_IN_METERS,
                            float( oy ) * ONEGAMEUNIT_IN_METERS,
                            float( fz ) * ONEGAMEUNIT_IN_METERS };
    const FVector3 midAt{ floorAt.X, floorAt.Y, floorAt.Z + 25.f * ONEGAMEUNIT_IN_METERS };

    if( wantScorch )
    {
        SpawnArcMark( floorAt,
                      fn,
                      ArcFlavor::Plasma,
                      false,
                      std::max( 0.f, float{ cvar::rt_barrel_scorch_scale } ),
                      cvar::rt_barrel_embers ? ImpactFx::Ember : ImpactFx::Arc );
    }
    if( wantShards )
    {
        SpawnBarrelShards( midAt, fn );
    }

    Printf( "barrel_here: %s%s at %.0f %.0f %.0f\n",
            wantScorch ? "scorch " : "",
            wantShards ? "shards" : "",
            ox,
            oy,
            fz );
}
