// Localised smoke: the puff volumes emitted at the muzzle, behind rockets, and
// off a monster's gun.
//
// THREE SOURCES, THREE DIFFERENT TRIGGERS, and none of them is a game hook:
//
//   player weapon   RT_AddMuzzleFlash calls in (rt_weapon.cpp), so the light and
//                   the smoke it lights come from ONE resolved muzzle position
//                   rather than two calculations that can drift apart -- hence
//                   the declarations in rt_internal.h.
//   rocket          tracked by pointer; DISAPPEARANCE (MF_MISSILE cleared) is
//                   the explosion.
//   monster gun     the SPRITE FRAME. See RT_MONSTER_GUNS.
//
// All three are that way for one reason: the actor classes are the WAD's, not
// ours, so nothing here may require a DECORATE edit or a ZScript.
//
// See docs/rt-smoke.md.

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
    //
    // MORE SMOKE ON THIS GUN IS A LONGER TRAIL, never a bigger parcel and never
    // a bigger burst. The wisp IS the trail -- 20 releases at 2-tic spacing is
    // about 1.1 s of the barrel breathing, against 14 at 3 tics (0.8 s) -- so
    // lengthening it thickens the thread along its own axis, which is the one
    // direction a filament is allowed to grow.
    { "Pistol",      0.34f, 0.07f, 0.90f, 1.5f, 0.25f, 0.06f, 1.5f, 20, 2, 0.10f,
      "thin wisp off the barrel" },
    // The machine gun is the pistol: a thread, not a cloud. Shorter than the
    // pistol's and left at 3-tic spacing on purpose: a held trigger re-arms this
    // every rt_smoke_repeat (5) tics, so at 2 tics the emitter alone would run
    // the pool to its 32-puff ceiling and the oldest parcels -- the tail you
    // actually read as lingering -- would be culled to make room.
    { "Chaingun",    0.34f, 0.07f, 0.70f, 1.1f, 0.30f, 0.07f, 1.6f, 9, 3, 0.10f,
      "small trail, like the pistol" },
    // Black powder from a wide bore: the fat one, and the reference for the rest.
    // SPARSE, not fat. A wide bore does make a cloud, but a cloud rendered as
    // one 0.4 m parcel per shot sits in the middle of the screen and blocks the
    // view -- the froxel grid cannot give it internal structure, so it reads as
    // a grey wall rather than as smoke. Several small parcels, well separated,
    // read as a burst and leave gaps to see through.
    { "Shotgun",     1.60f, 0.30f, 0.55f, 1.1f, 0.90f, 1.60f, 1.1f, 8, 3, 0.35f,
      "a scatter of small parcels, not one ball" },
    { "SuperShotgun",2.20f, 0.34f, 0.60f, 1.2f, 1.00f, 1.90f, 1.0f, 10, 3, 0.40f,
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

// MONSTERS WITH GUNS, which until now were the one obvious hole: the player's
// pistol breathed a wisp and the zombieman shooting back at him did not.
//
// THE TRIGGER IS THE SPRITE FRAME, not a code hook. A monster's attack is a
// DECORATE state and there is nothing in the renderer that sees it fire --
// A_PosAttack is called by the playsim and leaves no trace we can read. What is
// readable, every frame, for free, is which sprite frame the actor is drawing:
//
//     Missile:
//         POSS E 10 A_FaceTarget     <- the aim frame. No smoke.
//         POSS F  8                  <- THE SHOT. This is the edge we want.
//         POSS E  8
//
// So the test is (sprite, frame) and the event is ENTERING that frame, exactly
// the way `gen_fx_emissives.py` decides which frames get a muzzle emissive --
// same sprites, same "…F fires, …E stays dark" rule, so the light on the sprite
// and the smoke off the barrel agree by construction. It needs no DECORATE edit
// and no ZScript, which matters for the same reason it did for the rocket: these
// classes are the WAD's, not ours.
//
// Frame F is unambiguous on all five: See is A-D, Pain is G, death is H and up.
struct MonsterGun
{
    const char*  sprite;   // full 4-character sprite name
    uint8_t      frame;    // 0 == 'A'; 5 == 'F', the fire frame on every row here
    int          parcels;  // ABSOLUTE, not a multiplier on rt_smoke_count. That
                           // cvar is the player's "more smoke" knob and has been
                           // moved once already; a monster's shot should not get
                           // bigger every time the shotgun does. The row's count
                           // multiplier is derived from this at spawn.
    SmokeProfile prof;
};

// Multipliers on the rt_smoke_* cvars, like RT_SMOKE_PROFILES.
//
// ALL OF THEM CARRY trail = 0, and that is a constraint rather than a taste: the
// trail emitter rebuilds its release point from the PLAYER's viewpoint every few
// tics (see g_smokeTrail), because it exists to keep a filament coming off the
// gun you are holding while you turn. Handing a monster a trail would hang its
// smoke off the camera. A monster shot is therefore one or two parcels and done,
// which is also what the budget wants when six of them are shooting at once.
static constexpr MonsterGun RT_MONSTER_GUNS[] = {
    // 64ZombieMan / 64TargetRangeZombieMan -- a rifle. One small parcel, the
    // same read as the player's pistol minus the filament.
    { "POSS", 5, 1, { "zombieman rifle", 0.f, 0.35f, 0.55f, 0.85f, 0.8f, 0.5f, 1.2f, 0, 0, 0.7f,
                   "one small parcel off the barrel" } },
    // 64ShotgunGuy -- a wide bore, so a SCATTER, for the reason the player's
    // shotgun is sparse: one fat parcel is a grey wall, several small ones read
    // as a burst and leave gaps to see through.
    { "SPOS", 5, 2, { "shotgun guy", 0.f, 0.50f, 0.50f, 0.95f, 0.7f, 1.6f, 1.1f, 0, 0, 0.9f,
                   "a scatter, like the player's shotgun" } },
    // Not in Retribution (no 64ChaingunGuy), kept because the engine loads on
    // DOOM2.WAD and the actor is one -file away.
    { "CPOS", 5, 1, { "chaingun guy", 0.f, 0.32f, 0.45f, 0.70f, 0.8f, 0.6f, 1.2f, 0, 0, 0.7f,
                   "thinner and shorter -- it fires again immediately" } },
    // 64MarineBot, which uses A_PosAttack on the player sprite.
    { "PLAY", 5, 1, { "marine bot", 0.f, 0.35f, 0.55f, 0.85f, 0.8f, 0.5f, 1.2f, 0, 0, 0.7f,
                   "as the zombieman" } },
    { "SSWV", 5, 1, { "wolfenstein ss", 0.f, 0.32f, 0.45f, 0.70f, 0.8f, 0.6f, 1.2f, 0, 0, 0.7f,
                   "as the chaingun guy" } },
};

static const MonsterGun* RT_MonsterGunFor( AActor* mo )
{
    if( !mo || mo->sprite < 0 || mo->sprite >= int( sprites.Size() ) )
    {
        return nullptr;
    }
    const char* sn = sprites[ mo->sprite ].name;
    if( !sn )
    {
        return nullptr;
    }
    for( const MonsterGun& g : RT_MONSTER_GUNS )
    {
        // Full four characters, never a prefix: POSS and SPOS differ in one
        // position, and prefix matching on four-character sprite names is how
        // RT_FlameKindOf already handed three torches the same colour once.
        //
        // Matched on the SPRITE only. The frame decides whether this actor is
        // firing right now, and that is the caller's business -- a mark has to
        // be kept for a gunner standing in its aim frame too, or the rising edge
        // has nothing to rise from.
        if( strnicmp( sn, g.sprite, 4 ) == 0 )
        {
            return &g;
        }
    }
    return nullptr;
}

// Which gunners are CURRENTLY in their fire frame, so the spawn happens on the
// edge rather than once per tic for the eight or ten the frame lasts. Pointer
// identity is enough for the same reason it is for a rocket, and the sweep at
// the end of the walk drops anything not seen this tic -- including an actor
// that died mid-frame, whose pointer must not be dereferenced next tic.
struct GunnerMark
{
    AActor* mo;
    int     lastTic;
    bool    firing;
};
static std::vector< GunnerMark > g_gunners;

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
    // Same for the gunners: a stale pointer here is worse than a wrong puff,
    // because the next tic would dereference it.
    g_gunners.clear();
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

    // Round UP, so a 0.34 multiplier still gives one parcel rather than silently
    // disabling smoke for that weapon -- but a multiplier of exactly 0 means the
    // weapon makes NO smoke, and must not round up to one.
    //
    // THE EPSILON GUARDS THE N / rt_smoke_count ROUND TRIP. Every caller that
    // wants an exact number of parcels states it that way -- the rocket trail,
    // the muzzle trail, the monster rows, the explosion -- and float32 does not
    // always give it back: over (count 1..32, parcels 1..16), twelve pairs land
    // an ulp ABOVE the integer and ceil to one more parcel than was asked for
    // (count 21, 3 parcels -> 3.000000238 -> 4). The shipping 4 is not one of
    // them and neither was 3, so this is a trap laid for the next person who
    // moves the cvar, not a bug being fixed. Nothing legitimately asks for a
    // count within 1e-4 of an integer from below, so trimming it costs nothing.
    const int want =
        prof.count <= 0.f
            ? 0
            : std::clamp( int( std::ceil( float( int{ cvar::rt_smoke_count } ) * prof.count -
                                          1e-4f ) ),
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

// One gunner, one tic. Called for every actor in the walk, so it has to be cheap
// on the rejection path: the sprite lookup is five strnicmp and everything else
// is behind it.
static void RT_MonsterGunSmoke( AActor*         mo,
                                int             tic,
                                const FVector3& camPos,
                                float           farMetres )
{
    // A networked player's body draws PLAY F too, and the local player's shot
    // already went through RT_AddMuzzleFlash with a real weapon profile and a
    // traced muzzle position. Let the weapon path own every player.
    if( !mo || mo->player || mo->health <= 0 )
    {
        return;
    }

    const MonsterGun* gun = RT_MonsterGunFor( mo );
    if( !gun )
    {
        return;
    }

    GunnerMark* mark = nullptr;
    for( GunnerMark& g : g_gunners )
    {
        if( g.mo == mo )
        {
            mark = &g;
            break;
        }
    }
    if( !mark )
    {
        g_gunners.push_back( GunnerMark{ mo, tic, false } );
        mark = &g_gunners.back();
    }
    mark->lastTic = tic;

    const bool firing = ( mo->frame == gun->frame );
    const bool edge   = firing && !mark->firing;
    mark->firing      = firing;

    if( !edge )
    {
        return;
    }

    // The gun point: out of the chest along the way the actor is FACING. Monsters
    // aim with A_FaceTarget, which is yaw only, and the sprite is billboarded, so
    // there is no pitch to follow here even when the shot itself has slope.
    const double   yaw = mo->Angles.Yaw.Radians();
    const FVector3 fwd{ float( std::cos( yaw ) ), float( std::sin( yaw ) ), 0.f };

    // 0.58 of the actor's height, which is where these sprites hold the weapon,
    // and a little past its own radius so the puff is not born inside the body.
    // Derived from the actor rather than hardcoded because Retribution's monsters
    // are 80 units tall where the stock ones are 56.
    const double gz = mo->Z() + mo->Height * 0.58;
    const double gr = mo->radius + 6.0;

    const FVector3 muzzle{ float( mo->X() + std::cos( yaw ) * gr ) * ONEGAMEUNIT_IN_METERS,
                           float( mo->Y() + std::sin( yaw ) * gr ) * ONEGAMEUNIT_IN_METERS,
                           float( gz ) * ONEGAMEUNIT_IN_METERS };

    if( farMetres > 0.f && ( muzzle - camPos ).LengthSquared() > farMetres * farMetres )
    {
        // Beyond this the puff is smaller than a froxel AND it would evict a
        // near one: the pool overflows oldest-out while the upload keeps the
        // nearest, so a firefight across the map would push the smoke off your
        // own barrel out of the array. See rt_smoke_monster_far.
        return;
    }

    SmokeProfile p = gun->prof;

    // One amount knob for the whole family, because the honest answer to "how
    // much" is a matter of taste and six of them shooting at once is a different
    // picture from one. It scales the parcel COUNT and the density together --
    // halving the count alone just makes the remaining parcels conspicuous.
    const float scale = std::max( 0.f, float{ cvar::rt_smoke_monster_scale } );

    // Divide by rt_smoke_count so RT_SpawnSmokePuffs's ceil( count x cvar )
    // multiplies straight back to gun->parcels. The row states an ABSOLUTE
    // number of parcels: rt_smoke_count is the player's "more smoke" knob and
    // moving it must not make every zombieman in the level smokier as well.
    p.count = ( float( gun->parcels ) * scale ) /
              std::max( 1.f, float( int{ cvar::rt_smoke_count } ) );
    p.density *= scale;

    // eye = a third of a metre behind the gun point, so rt_smoke_offset's 0.7
    // lands the parcel just off the muzzle rather than somewhere along a segment
    // that has no meaning for a monster. Same shape as the rocket trail's call.
    RT_SpawnSmokePuffs( muzzle - fwd * 0.33f, muzzle, fwd, FVector3{ 0, 0, 0 }, p );

    if( cvar::rt_smoke_debug )
    {
        Printf( "rt_smoke MONSTER: %s (%s) fired at %.2f %.2f %.2f (tic %d)\n",
                mo->GetClass() ? mo->GetClass()->TypeName.GetChars() : "?",
                gun->prof.note,
                muzzle.X,
                muzzle.Y,
                muzzle.Z,
                tic );
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

    // ONE walk of the thinker list for both actor-driven sources. Rockets are
    // read every frame (the trail self-gates on nextTrail); monster gunners only
    // when the map clock actually moved, because the edge they are tested for is
    // a tic-long state change and re-testing it at 200 fps buys nothing.
    const bool wantRocket  = bool{ cvar::rt_smoke_rocket };
    const bool wantMonster = bool{ cvar::rt_smoke_monster } && steps > 0;

    if( wantRocket || wantMonster )
    {
        // The camera, for the monster distance cull below. A puff further away
        // than this is not merely small, it is a puff the froxel volume cannot
        // resolve at all -- and worse, it would EVICT a near one, because the
        // pool's overflow rule is oldest-out while the upload picks nearest.
        const auto&    vp = r_viewpoint;
        const FVector3 camPos{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                               float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                               float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };
        const float    monFar = std::max( 0.f, float{ cvar::rt_smoke_monster_far } );

        auto it = primaryLevel->GetThinkerIterator< AActor >();
        while( AActor* mo = it.Next() )
        {
            if( !RT_IsRocketProjectile( mo ) )
            {
                if( wantMonster )
                {
                    RT_MonsterGunSmoke( mo, tic, camPos, monFar );
                }
                continue;
            }
            if( !wantRocket )
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
                // EXACTLY one parcel per drop, whatever rt_smoke_count is.
                // Spelling this as a bare 0.3 worked only while that cvar was 3;
                // raising it to 4 turned ceil(4 x 0.3) into two, which empties
                // the whole budget on a single rocket in under a second. The
                // division is what makes the player's knob a player's knob.
                p2.count = 1.f / std::max( 1.f, float( int{ cvar::rt_smoke_count } ) );
                p2.radius = std::max( 0.01f, float{ cvar::rt_smoke_rocket_radius } ) /
                            std::max( 0.001f, float{ cvar::rt_smoke_radius } );
                p2.density = 0.45f;
                p2.life    = 1.0f;  // a shorter trail is a SHORTER trail: the
                                    // length you see is life x flight speed, so
                                    // this is the knob that shortens the plume
                                    // without thinning the parcels themselves
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
        for( size_t i = 0; wantRocket && i < g_rockets.size(); )
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

        // Drop every gunner not seen in this walk. A monster that died, was
        // removed or simply left the thinker list must not leave a pointer
        // behind for the next tic to dereference -- and re-entering the list
        // later is harmless, because a fresh mark starts !firing, which is the
        // state that arms the edge.
        if( wantMonster )
        {
            for( size_t i = 0; i < g_gunners.size(); )
            {
                if( g_gunners[ i ].lastTic < tic )
                {
                    g_gunners[ i ] = g_gunners.back();
                    g_gunners.pop_back();
                    continue;
                }
                i++;
            }
        }
    }

    // Release one trail parcel when it is due. A single parcel, deliberately:
    // the filament is made of separated beads rising at different ages, and
    // emitting them in pairs collapses that back into a clump.
    if( g_smokeTrail.left > 0 && tic >= g_smokeTrail.nextTic )
    {
        SmokeProfile p = g_smokeTrail.prof;
        // ONE parcel per release, whatever the weapon asks and whatever
        // rt_smoke_count is. The literal 0.34 that used to be here meant one
        // only while that cvar was 3 -- at 4 it is ceil( 1.36 ) = 2, and a
        // filament made of PAIRS is a clump, which is the failure §8 of the doc
        // exists to describe. The whole trail is a shape in time; releasing two
        // at a time collapses it.
        p.count = 1.f / std::max( 1.f, float( int{ cvar::rt_smoke_count } ) );

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
