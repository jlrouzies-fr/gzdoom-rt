// Localised smoke: the puff volumes emitted at the muzzle and behind rockets.
//
// The spawn is triggered from RT_AddMuzzleFlash over in rt_main.cpp so that the
// light and the smoke it lights come from one resolved muzzle position rather
// than two -- hence the declarations in rt_internal.h. See docs/rt-smoke.md.
//
// Split out of rt_main.cpp. Behaviour unchanged; this is a move.

#include "rt_internal.h"

// The shared internals (RG_CHECK, ONEGAMEUNIT_IN_METERS, RT_SectorHue, the
// light-ID bases) come in unqualified, exactly as when this code lived inside
// rt_main.cpp's anonymous namespace.
using namespace rtx;

// SmokePuff itself now lives in rt_internal.h: RT_DrawFrame sorts this pool by
// distance to pick the frame's budget, so it needs the layout.

// PER-WEAPON SMOKE, because one profile is wrong for every gun.
//
// A pistol should breathe a thin filament off the barrel; a shotgun should cough
// a fat cloud. The cvars carry the DEFAULT profile and the multipliers here bend
// it per weapon, so tuning rt_smoke_density still moves everything together and
// a weapon row only says how it differs.
//
// WHAT THE GRID CAN AND CANNOT DO. The froxel volume is 160x88 across the SCREEN
// and 64 in depth, so lateral resolution is angular and fine -- about 1.7 cm per
// cell at 1.5 m -- while depth is 0.47 m per slice. A narrow rising filament is
// therefore representable: it is thin in screen space, which is the axis with
// resolution. What it cannot be is thin ALONG THE VIEW, so a filament seen from
// the side reads crisp and the same filament pointing away from you smears to
// half a metre. That is a property of the volume, not of these numbers.
//
// Matched by substring against the ready weapon's class name, the same way
// MuzzleFlashTintFor picks the flash colour -- Retribution prefixes its classes
// (64Shotgun, 64Chaingun), so a substring is what works.

static constexpr SmokeProfile RT_SMOKE_PROFILES[] = {
    // A FILAMENT, not a puff: one small parcel, barely any lateral spread, and
    // it rises rather than travels. Long life because a wisp that vanishes in
    // half a second reads as a glitch; low density because at this radius a
    // thick one would be a bead.
    { "Pistol",      0.34f, 0.07f, 0.90f, 1.5f, 0.25f, 0.06f, 1.5f, 14, 3, 0.10f,
      "thin wisp off the barrel" },
    // The machine gun is the pistol: a thread, not a cloud. Slightly shorter and
    // faster so a burst does not stack fourteen parcels per shot -- at a 4-tic
    // fire rate that is what fills a screen.
    { "Chaingun",    0.34f, 0.07f, 0.70f, 1.1f, 0.30f, 0.07f, 1.6f, 6, 3, 0.10f,
      "small trail, like the pistol" },
    // Black powder from a wide bore: the fat one, and the reference for the rest.
    // SPARSE, not fat. A wide bore does make a cloud, but a cloud rendered as
    // one 0.4 m parcel per shot sits in the middle of the screen and blocks the
    // view -- the froxel grid cannot give it internal structure, so it reads as
    // a grey wall rather than as smoke. Several small parcels, well separated,
    // read as a burst and leave gaps to see through.
    { "Shotgun",     1.60f, 0.30f, 0.55f, 1.1f, 0.90f, 1.60f, 1.1f, 5, 4, 0.35f,
      "a scatter of small parcels, not one ball" },
    { "SuperShotgun",2.20f, 0.34f, 0.60f, 1.2f, 1.00f, 1.90f, 1.0f, 6, 4, 0.40f,
      "two barrels: wider scatter, same sparseness" },
    // Fast repeat: each burst must be SMALL or a held trigger fills the room.
    // The budget is what actually caps it, but small parcels read better than a
    // budget cull, which pops.
    // NO MUZZLE SMOKE. The launcher's smoke belongs to the ROCKET, not to the
    // barrel: a trail along the projectile's flight and a burst where it lands.
    // See RT_UpdateRocketSmoke.
    { "RocketLauncher", 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0, 0, 1.f,
      "none at the muzzle -- the rocket carries it" },
    // NO SMOKE AT ALL. These are not combustion, so powder smoke is simply the
    // wrong effect; the muzzle flash is what sells them. A count multiplier of 0
    // makes RT_SpawnSmokePuffs emit nothing and arms no trail.
    { "PlasmaRifle", 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0, 0, 1.f, "none" },
    { "BFG",         0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0, 0, 1.f, "none" },
    { "Unmaker",     0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0, 0, 1.f, "none" },
};

// The profile for the weapon currently held, or the identity if it is not listed
// (and when rt_smoke_perweapon is off).
SmokeProfile rtx::RT_SmokeProfileFor( AActor* viewactor )
{
    static constexpr SmokeProfile identity = {
        nullptr, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0, 0, 1.f, "default"
    };

    if( !cvar::rt_smoke_perweapon || !viewactor || !viewactor->player ||
        !viewactor->player->ReadyWeapon )
    {
        return identity;
    }

    const char* c = viewactor->player->ReadyWeapon->GetClass()->TypeName.GetChars();
    if( !c )
    {
        return identity;
    }

    // SuperShotgun before Shotgun: the first is a substring of nothing, but the
    // second IS a substring of "64SuperShotgun", so order decides correctness.
    for( const SmokeProfile& p : RT_SMOKE_PROFILES )
    {
        if( p.cls && strstr( c, "Super" ) && strstr( p.cls, "Super" ) && strstr( c, p.cls ) )
        {
            return p;
        }
    }
    for( const SmokeProfile& p : RT_SMOKE_PROFILES )
    {
        if( p.cls && !strstr( p.cls, "Super" ) && strstr( c, p.cls ) )
        {
            // "Shotgun" would match "64SuperShotgun" too; that case was taken above.
            if( strstr( c, "Super" ) && strcmp( p.cls, "Shotgun" ) == 0 )
            {
                continue;
            }
            return p;
        }
    }
    return identity;
}

// Room for RG_MAX_SMOKE_PUFFS; rt_smoke_budget decides how many of them are
// actually uploaded.
std::array< SmokePuff, RG_MAX_SMOKE_PUFFS > rtx::g_smokePuffs{};
uint32_t                                           rtx::g_smokePuffCount = 0;
static int                                         g_smokeLastTic   = -1;

// A shot leaves an EMITTER behind, not just a burst. It stays where the barrel
// was and keeps releasing one small parcel every few tics; because each parcel
// rises as soon as it is born, the result is a thin column rather than a ball.
// World-anchored on purpose -- following the gun would glue the filament to the
// camera, which is the failure rt_smoke_inherit already exists to avoid.
struct SmokeTrail
{
    float        dist;   // metres from the EYE to the muzzle, captured at the
                         // shot. The release point is rebuilt from the CURRENT
                         // viewpoint each time, so the thread keeps coming off
                         // the barrel while you turn and walk.
                         //
                         // Only the emitter follows. Parcels already released
                         // stay where they were born, which is what makes the
                         // thread trail behind a moving player instead of
                         // sliding along with the camera -- world-anchoring the
                         // EMITTER, which is what this did first, left the smoke
                         // hanging where the shot happened while the gun walked
                         // away from it.
    float        dz;
    SmokeProfile prof;
    int          left;
    int          nextTic;
};
static SmokeTrail g_smokeTrail{};

// ROCKETS carry their own smoke, and the renderer can do the whole thing without
// touching game code.
//
// A projectile is tracked by pointer while it lives; each tic it drops a small
// parcel where it is. When a tracked rocket is no longer in the thinker list it
// has exploded, so the burst is spawned at the last position seen. That is the
// trick worth remembering: DISAPPEARANCE is the death event, and it needs no
// hook, no DECORATE edit and no ZScript -- which matters because the projectile
// classes here are the WAD's (64Rocket), not ours.
struct RocketMark
{
    AActor*  mo;
    FVector3 lastPos;   // metres
    FVector3 dir;
    int      lastTic;
    int      nextTrail;
};
static std::vector< RocketMark > g_rockets;

// The launcher's projectile, not the launcher. "Rocket" matches both, so the
// weapon has to be excluded by name -- Retribution's classes are 64Rocket and
// 64RocketLauncher.
static bool RT_IsRocketProjectile( AActor* mo )
{
    if( !mo || !mo->GetClass() )
    {
        return false;
    }
    // MF_MISSILE IS THE TEST THAT MATTERS, and leaving it out was a real bug.
    //
    // P_ExplodeMissile clears MF_MISSILE but the actor LIVES ON, same class,
    // through its whole death animation. Matching on the class name alone
    // therefore kept seeing a rocket that had already hit the wall -- stationary,
    // still "alive" -- and kept dropping trail parcels into it for as long as the
    // explosion played. That is the cluster of small puffs that sat against a
    // wall and never faded: they were not failing to expire, they were being
    // continuously replaced.
    //
    // It also fixes WHEN the burst happens. Losing the flag is the explosion, so
    // the burst now fires on that frame instead of whenever the actor finally
    // left the thinker list.
    if( !( mo->flags & MF_MISSILE ) )
    {
        return false;
    }

    const char* c = mo->GetClass()->TypeName.GetChars();
    return c && strstr( c, "Rocket" ) && !strstr( c, "Launcher" ) && !strstr( c, "Smoke" );
}

void rtx::RT_ClearSmokePuffs()
{
    g_smokePuffCount   = 0;
    g_smokeLastTic     = -1;
    g_smokeTrail.left  = 0;
    // Rocket pointers do not survive a level change, and a stale one would be
    // read as "exploded" and burst somewhere in the new map.
    g_rockets.clear();
}

// Called from RT_AddMuzzleFlash on the rising edge of a shot -- or on the
// rt_smoke_repeat interval while the trigger is held, because a fast weapon
// re-enters its Flash state before A_Light0 clears extralight and the edge alone
// would fire once per burst. The position is the muzzle flash's own resolved
// one, so the light and the smoke it lights are the same point by construction
// rather than two similar calculations that can drift apart.
void rtx::RT_SpawnSmokePuffs( const FVector3& eye,
                                const FVector3& muzzle,
                                const FVector3& forward,
                                const FVector3& inheritVel,
                                const SmokeProfile& prof )
{
    if( !cvar::rt_smoke )
    {
        return;
    }

    // Arm the trail for this shot. rt_smoke_trail scales it so the whole effect
    // can be turned off or lengthened without touching the table.
    if( prof.trail > 0 && prof.trailEvery > 0 && primaryLevel )
    {
        const float scale = std::max( 0.f, float{ cvar::rt_smoke_trail } );
        const FVector3 off = muzzle - eye;
        g_smokeTrail.dist    = FVector3{ off.X, off.Y, 0.f }.Length();
        g_smokeTrail.dz      = off.Z;
        g_smokeTrail.prof    = prof;
        g_smokeTrail.prof.trail = 0; // the trail's own parcels must not re-arm it
        g_smokeTrail.left    = int( prof.trail * scale );
        g_smokeTrail.nextTic = primaryLevel->maptime + prof.trailEvery;
    }

    // Round UP, so a 0.34 multiplier on the shipping 3 still gives one parcel
    // rather than silently disabling smoke for that weapon.
    // Round UP so a 0.34 multiplier still gives one parcel -- but a multiplier of
    // exactly 0 means the weapon makes NO smoke, and must not round up to one.
    const int want =
        prof.count <= 0.f
            ? 0
            : std::clamp( int( std::ceil( float( int{ cvar::rt_smoke_count } ) * prof.count ) ),
                          0,
                          RG_MAX_SMOKE_PUFFS );

    if( want <= 0 )
    {
        return;
    }

    // A LOCAL generator on purpose. The gameplay RNG (M_Random and friends) is
    // part of the simulation -- consuming it from the renderer would desync a
    // demo or a netgame, and the desync would be invisible until someone
    // recorded one. Nothing about a puff's jitter needs to be reproducible
    // across machines, so a private xorshift is both safer and cheaper.
    static uint32_t s_rng = 0x9E3779B9u;
    auto rnd = [ & ]() {
        s_rng ^= s_rng << 13;
        s_rng ^= s_rng >> 17;
        s_rng ^= s_rng << 5;
        return float( s_rng & 0xFFFF ) / 32767.5f - 1.f;
    };

    for( int i = 0; i < want; i++ )
    {
        // Oldest-out when full: the puff about to be overwritten is the one
        // closest to vanishing anyway, and a shot that silently produced no
        // smoke because the array was full would be the more confusing failure.
        uint32_t slot;
        if( g_smokePuffCount < g_smokePuffs.size() )
        {
            slot = g_smokePuffCount++;
        }
        else
        {
            slot = 0;
            for( uint32_t j = 1; j < g_smokePuffCount; j++ )
            {
                if( g_smokePuffs[ j ].age > g_smokePuffs[ slot ].age )
                {
                    slot = j;
                }
            }
        }

        const float spread = std::max( 0.f, float{ cvar::rt_smoke_spread } * prof.spread );

        SmokePuff& puff = g_smokePuffs[ slot ];
        // Along the traced segment, never past its end. See rt_smoke_offset.
        puff.pos = eye + ( muzzle - eye ) * std::clamp( float{ cvar::rt_smoke_offset }, 0.f, 1.f );
        // Jitter the birthplace by a fraction of the radius as well as the
        // velocity: three puffs launched from one point still read as one ball
        // for the first few frames, which is exactly when the flash is lighting
        // them and they are most visible.
        puff.pos += FVector3{ rnd(), rnd(), rnd() } *
                    ( float{ cvar::rt_smoke_radius } * prof.radius ) * 0.6f;
        puff.vel = inheritVel * std::clamp( float{ cvar::rt_smoke_inherit }, 0.f, 1.f ) +
                   forward * ( float{ cvar::rt_smoke_speed } * prof.speed ) +
                   FVector3{ rnd(), rnd(), rnd() } * spread;
        puff.radius  = std::max( 0.01f, float{ cvar::rt_smoke_radius } * prof.radius );
        puff.growth  = std::max( 0.f, float{ cvar::rt_smoke_growth } * prof.growth );
        puff.radius0 = puff.radius;
        puff.phase   = float( slot ) * 1.7f + rnd() * 3.14f;
        puff.age     = 0.f;
        puff.life    = std::max( 0.05f, float{ cvar::rt_smoke_life } * prof.life );
        puff.density = std::max( 0.f, float{ cvar::rt_smoke_density } * prof.density );
        puff.rise    = float{ cvar::rt_smoke_rise } * prof.rise;

        if( cvar::rt_smoke_debug )
        {
            Printf( "rt_smoke B/spawn: slot=%u live=%u at %.2f %.2f %.2f r=%.2f "
                    "vel %.2f %.2f %.2f\n",
                    slot,
                    g_smokePuffCount,
                    puff.pos.X,
                    puff.pos.Y,
                    puff.pos.Z,
                    puff.radius,
                    puff.vel.X,
                    puff.vel.Y,
                    puff.vel.Z );
        }
    }
}

// Advance the puffs. Driven by maptime rather than wall clock so a paused game
// freezes the smoke -- the same reason RT_UploadFlameLights uses it for the
// flicker phase. At 35 tics/s the step is coarse, but smoke is slow and the
// froxel grid it lands in is coarser still.
void RT_UpdateSmokePuffs()
{
    if( !primaryLevel )
    {
        RT_ClearSmokePuffs();
        return;
    }

    if( !cvar::rt_smoke )
    {
        g_smokePuffCount = 0;
        g_smokeLastTic   = primaryLevel->maptime;
        return;
    }

    const int tic = primaryLevel->maptime;

    int steps = g_smokeLastTic < 0 ? 0 : tic - g_smokeLastTic;
    // A backwards or enormous jump is a level change, a load or a warp. Do not
    // integrate across it -- a puff advanced by a thousand tics ends up
    // somewhere arbitrary, and the froxel volume would show it there.
    if( steps < 0 || steps > TICRATE )
    {
        RT_ClearSmokePuffs();
        g_smokeLastTic = tic;
        return;
    }
    g_smokeLastTic = tic;

    // DIAGNOSTIC: spawn without a weapon. Straight ahead of the eye, at the
    // distance a muzzle puff would be, so what it exercises is the same
    // packing, uniform and shader path -- only the trigger differs.
    if( int{ cvar::rt_smoke_autospawn } > 0 && steps > 0 )
    {
        static int s_autoTic = -1;
        const int  every     = std::max( 1, int{ cvar::rt_smoke_autospawn } );
        if( s_autoTic < 0 || tic - s_autoTic >= every || tic < s_autoTic )
        {
            s_autoTic = tic;

            const auto&    vp = r_viewpoint;
            const FVector3 eye{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                                float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                                float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };

            const double   yaw   = vp.Angles.Yaw.Radians();
            const double   pitch = vp.Angles.Pitch.Radians();
            const FVector3 fwd{ float( std::cos( yaw ) * std::cos( pitch ) ),
                                float( std::sin( yaw ) * std::cos( pitch ) ),
                                float( -std::sin( pitch ) ) };

            RT_SpawnSmokePuffs( eye,
                                eye + fwd * 1.5f,
                                fwd,
                                FVector3{ 0, 0, 0 },
                                RT_SmokeProfileFor( vp.camera ) );

            if( cvar::rt_smoke_debug )
            {
                Printf( "rt_smoke AUTO: spawned at eye %.2f %.2f %.2f + fwd %.2f %.2f %.2f "
                        "(tic %d)\n",
                        eye.X, eye.Y, eye.Z, fwd.X, fwd.Y, fwd.Z, tic );
            }
        }
    }

    // Rockets: a parcel per tic of flight, and a burst where one stops existing.
    if( cvar::rt_smoke && cvar::rt_smoke_rocket )
    {
        auto it = primaryLevel->GetThinkerIterator< AActor >();
        while( AActor* mo = it.Next() )
        {
            if( !RT_IsRocketProjectile( mo ) )
            {
                continue;
            }

            const FVector3 p{ float( mo->X() ) * ONEGAMEUNIT_IN_METERS,
                              float( mo->Y() ) * ONEGAMEUNIT_IN_METERS,
                              float( mo->Z() ) * ONEGAMEUNIT_IN_METERS };

            RocketMark* mark = nullptr;
            for( RocketMark& r : g_rockets )
            {
                if( r.mo == mo )
                {
                    mark = &r;
                    break;
                }
            }
            if( !mark )
            {
                g_rockets.push_back( RocketMark{ mo, p, FVector3{ 0, 0, 1 }, tic, tic } );
                mark = &g_rockets.back();
                if( cvar::rt_smoke_debug )
                {
                    Printf( "rt_smoke ROCKET: tracking %s at %.2f %.2f %.2f (tic %d)\n",
                            mo->GetClass()->TypeName.GetChars(), p.X, p.Y, p.Z, tic );
                }
            }

            const FVector3 d = p - mark->lastPos;
            if( d.LengthSquared() > 0.0001f )
            {
                mark->dir = d.Unit();
            }
            mark->lastPos = p;
            mark->lastTic = tic;

            if( tic >= mark->nextTrail )
            {
                mark->nextTrail = tic + std::max( 1, int{ cvar::rt_smoke_rocket_every } );

                SmokeProfile p2{};
                p2.cls = "rocket-trail";
                p2.count = 0.3f; // exactly one parcel per drop: ceil(3 * 0.3) == 1.
                                 // 0.34 rounds to two, which empties the whole
                                 // budget on a single rocket in under a second.
                p2.radius = std::max( 0.01f, float{ cvar::rt_smoke_rocket_radius } ) /
                            std::max( 0.001f, float{ cvar::rt_smoke_radius } );
                p2.density = 0.55f;
                p2.life    = 1.2f;
                p2.speed   = 0.f;   // it must HANG where it was dropped, not fly on
                p2.spread  = 0.15f;
                p2.rise    = 0.5f;
                p2.growth  = 0.55f; // enough to merge consecutive drops, no more
                p2.trail = 0;
                p2.trailEvery = 0;
                RT_SpawnSmokePuffs( p - mark->dir, p, mark->dir, FVector3{ 0, 0, 0 }, p2 );
            }
        }

        // Anything not seen this tic has exploded: burst, then forget it.
        for( size_t i = 0; i < g_rockets.size(); )
        {
            if( g_rockets[ i ].lastTic < tic )
            {
                SmokeProfile b{};
                b.cls = "rocket-boom";
                b.count = float( std::max( 0, int{ cvar::rt_smoke_boom } ) ) /
                          std::max( 1.f, float( int{ cvar::rt_smoke_count } ) );
                b.radius = std::max( 0.02f, float{ cvar::rt_smoke_boom_radius } ) /
                           std::max( 0.001f, float{ cvar::rt_smoke_radius } );
                // A burst of ten dense parcels was the noisiest thing in the
                // game by construction: each is an independent one-sample
                // estimate stacked on the others. Fewer and thinner, which is
                // the same lesson the shotgun taught -- less smoke reads better
                // AND is cheaper to light.
                b.density = 0.7f;
                b.life    = 1.8f;
                b.speed   = 0.f;
                b.spread  = 4.0f;   // thrown outward, which is what makes it a burst
                b.rise    = 1.6f;
                b.growth  = 1.1f;
                b.trail = 0;
                b.trailEvery = 0;
                if( cvar::rt_smoke_debug )
                {
                    Printf( "rt_smoke ROCKET: burst at %.2f %.2f %.2f (tic %d) -- "
                            "MF_MISSILE cleared or actor gone\n",
                            g_rockets[ i ].lastPos.X,
                            g_rockets[ i ].lastPos.Y,
                            g_rockets[ i ].lastPos.Z,
                            tic );
                }
                RT_SpawnSmokePuffs( g_rockets[ i ].lastPos - g_rockets[ i ].dir,
                                    g_rockets[ i ].lastPos,
                                    g_rockets[ i ].dir,
                                    FVector3{ 0, 0, 0 },
                                    b );
                g_rockets[ i ] = g_rockets.back();
                g_rockets.pop_back();
                continue;
            }
            i++;
        }
    }

    // Release one trail parcel when it is due. A single parcel, deliberately:
    // the filament is made of separated beads rising at different ages, and
    // emitting them in pairs collapses that back into a clump.
    if( g_smokeTrail.left > 0 && tic >= g_smokeTrail.nextTic )
    {
        SmokeProfile p = g_smokeTrail.prof;
        p.count        = 0.34f; // one parcel per release, whatever the weapon asks

        // Rebuild the muzzle from where the player is looking NOW.
        const auto&    vp = r_viewpoint;
        const FVector3 eyeNow{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                               float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                               float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };
        const double   yaw   = vp.Angles.Yaw.Radians();
        const double   pitch = vp.Angles.Pitch.Radians();
        const FVector3 fwdNow{ float( std::cos( yaw ) * std::cos( pitch ) ),
                               float( std::sin( yaw ) * std::cos( pitch ) ),
                               float( -std::sin( pitch ) ) };

        const FVector3 muzzleNow =
            eyeNow + fwdNow * g_smokeTrail.dist + FVector3{ 0, 0, g_smokeTrail.dz };

        RT_SpawnSmokePuffs( eyeNow, muzzleNow, fwdNow, FVector3{ 0, 0, 0 }, p );
        g_smokeTrail.left--;
        g_smokeTrail.nextTic = tic + std::max( 1, g_smokeTrail.prof.trailEvery );
    }

    const float dt     = 1.f / float( TICRATE );
    const float drag   = std::max( 0.f, float{ cvar::rt_smoke_drag } );

    for( int step = 0; step < steps; step++ )
    {
        for( uint32_t i = 0; i < g_smokePuffCount; )
        {
            SmokePuff& puff = g_smokePuffs[ i ];

            // The curl. A real barrel wisp leaves laminar and only breaks up
            // once it has risen a little, so the lateral push scales with AGE --
            // straight for the first fraction of a second, then increasingly
            // wandering. Two incommensurate frequencies keep it from reading as
            // a sine wave.
            const float curl = std::max( 0.f, float{ cvar::rt_smoke_curl } );
            if( curl > 0.f )
            {
                const float a = puff.age;
                const float w = curl * a * a;
                puff.vel.X += w * std::sin( a * 3.1f + puff.phase ) * dt;
                puff.vel.Y += w * std::cos( a * 2.3f + puff.phase * 1.7f ) * dt;
            }

            puff.vel.Z += puff.rise * dt;
            puff.vel *= std::max( 0.f, 1.f - drag * dt );
            puff.pos += puff.vel * dt;
            puff.radius += puff.growth * dt;
            puff.age += dt;

            // The world reaction, and the reason this loop is on the CPU: a
            // puff that would pass through the ceiling flattens against it
            // instead, and loses its vertical velocity so it spreads rather
            // than pressing on. Same for the floor.
            {
                const double mx = double( puff.pos.X ) / double{ ONEGAMEUNIT_IN_METERS };
                const double my = double( puff.pos.Y ) / double{ ONEGAMEUNIT_IN_METERS };

                if( sector_t* sec = primaryLevel->PointInSector( mx, my ) )
                {
                    const float zf =
                        float( sec->floorplane.ZatPoint( mx, my ) ) * ONEGAMEUNIT_IN_METERS;
                    const float zc =
                        float( sec->ceilingplane.ZatPoint( mx, my ) ) * ONEGAMEUNIT_IN_METERS;

                    // Only clamp if the sector is actually taller than the puff.
                    // In a crawlspace both limits fight and the puff would jitter
                    // between them every step; leaving it alone there is invisible,
                    // because a puff wider than the room is fog anyway.
                    if( zc - zf > puff.radius * 2.f )
                    {
                        const float lo = zf + puff.radius;
                        const float hi = zc - puff.radius;

                        if( puff.pos.Z > hi )
                        {
                            puff.pos.Z = hi;
                            puff.vel.Z = std::min( puff.vel.Z, 0.f );
                        }
                        else if( puff.pos.Z < lo )
                        {
                            puff.pos.Z = lo;
                            puff.vel.Z = std::max( puff.vel.Z, 0.f );
                        }
                    }
                }
            }

            if( puff.age >= puff.life )
            {
                g_smokePuffs[ i ] = g_smokePuffs[ --g_smokePuffCount ];
                continue;
            }
            i++;
        }
    }
}




// Doom64-RT: the smoke probe, live. `smoke` alone reports the state.
//
// This exists because three diagnostic runs in a row came back with
// DEBUGMODE=1 -- the launcher passed `+rt_smoke_debug 1 +rt_smoke_debug 4`
// and the first one won, so the probe never ran and every result was
// uninformative. A cvar you can set in the console and read back cannot fail
// that way.
CCMD( smoke )
{
    if( argv.argc() >= 2 )
    {
        const int m = atoi( argv[ 1 ] );
        UCVarValue v;
        v.Int = std::clamp( m, 0, 4 );
        cvar::rt_smoke_debug->ForceSet( v, CVAR_Int );
    }

    const int m = int{ cvar::rt_smoke_debug };
    Printf( "smoke: rt_smoke %d, %u puff(s) live, probe mode %d -- %s\n",
            int{ cvar::rt_smoke },
            g_smokePuffCount,
            m,
            m == 0   ? "off"
            : m == 1 ? "logging only"
            : m == 2 ? "MAGENTA in froxels a puff covers"
            : m == 3 ? "GREEN everywhere while puffs exist"
                     : "BLUE everywhere, unconditionally" );
    if( m < 2 )
    {
        Printf( "  `smoke 4` should turn the whole screen blue immediately. If it "
                "does not, the shader cannot read the smoke uniforms at all.\n"
                "  `smoke 2` paints only the froxels a puff actually covers.\n" );
    }
}
