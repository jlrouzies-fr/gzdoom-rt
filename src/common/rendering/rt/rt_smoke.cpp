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
    // THE 2.5 cm THREAD WAS UNRENDERABLE, and that is why this gun's smoke was
    // invisible in a lit room while a barrel's was a big grey cloud.
    //
    // The packer pads a puff's ALONG-view radius up to half a froxel slice
    // (23.4 cm at rt_volume_far 30) so the grid can resolve it, and divides the
    // density by the same factor to keep the optical depth honest. At 0.07 the
    // pistol's parcels were 2.5 cm and kept ONE TENTH of their density; the
    // barrel's 40 cm burst keeps all of it. Effective density 1.3 against 11.2.
    // And since smoke_blendTint weights colour by density, a puff that thin
    // barely wins against the room's medium -- so it took the room's colour
    // rather than showing its own grey, which is precisely how it was reported.
    //
    // AND IT STAYS 2.5 cm ANYWAY, because it looks better. Judged in play
    // against 10.5 cm: the bigger parcel is more visible and reads as a puff,
    // and the whole point of this row is that a pistol does not make puffs.
    //
    // So the visibility problem is solved on the COLOUR side instead
    // (rt_smoke_tint): the thinning is what let the room's medium win the
    // blend, and biasing a smoke cell back toward its own albedo fixes that
    // without touching the size. Density and colour were doing two different
    // jobs here and only one of them needed changing.
    //
    // MORE SMOKE ON THIS GUN IS A LONGER TRAIL -- MORE PARCELS, at the SAME
    // spacing and the SAME lifetime. Not a bigger parcel, not a bigger burst,
    // and above all not a longer-lived one.
    //
    // THE LIFE MULTIPLIER HOLDS THIS PROFILE'S ABSOLUTE LIFETIME FIXED, and
    // that is the whole reason it moved from 1.5 to 1.1 when rt_smoke_life went
    // 1.6 -> 2.2. Both give ~2.4 s. Pairing life with growth keeps a puff's
    // SIZE constant, which is what that pairing was for -- but a filament's
    // SHAPE depends on absolute life twice more, and neither is size:
    //
    //   RISE integrates over time. At 0.5 m/s terminal climb, 2.4 s puts the
    //   top of the plume ~0.96 m above the barrel; 3.3 s puts it at ~1.42 m,
    //   which is above the player's head. The smoke stops reading as coming off
    //   the gun and starts reading as hanging over you.
    //
    //   CURL GOES AS AGE SQUARED (rt_smoke_curl, deliberately, so a wisp leaves
    //   laminar and only breaks up once it has risen). So a 37% longer life is
    //   an 89% stronger lateral push at the end of it. The thread sprays out
    //   instead of staying a thread.
    //
    // Both were measured after the fact, from a report that the pistol "appears
    // above me and is too sprayed out". A profile whose read depends on absolute
    // time has to restate it whenever the shared cvar moves.
    //
    // growth 0.10 -> 0.14 for the same reason in reverse: rt_smoke_growth fell
    // 0.7 -> 0.5, and this row wants the same final radius it always had.
    { "Pistol",      0.34f, 0.07f, 0.90f, 1.1f, 0.25f, 0.06f, 1.5f, 22, 2, 0.14f,
      "thin wisp off the barrel" },
    // The machine gun is the pistol: a thread, not a cloud, and its lifetime is
    // pinned the same way (0.8 x 2.2 = 1.76 s, exactly what 1.1 x 1.6 gave).
    // Left at 3-tic spacing on purpose: a held trigger re-arms this every
    // rt_smoke_repeat (5) tics, so at 2 tics the emitter alone would run the
    // pool to its ceiling and the oldest parcels -- the tail you actually read
    // as lingering -- would be culled to make room.
    { "Chaingun",    0.34f, 0.07f, 0.70f, 0.8f, 0.30f, 0.07f, 1.6f, 9, 3, 0.14f,
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
// Defined below, next to the table it indexes. A pointer to an incomplete
// type is all RocketMark needs, and declaring it here keeps the projectile
// table beside the rows it documents rather than hoisted above this struct.
struct ProjectileSmoke;

struct RocketMark
{
    AActor*  mo;
    FVector3 lastPos;   // metres
    FVector3 dir;
    int      lastTic;
    int      nextTrail;
    // The row matched when this projectile was FIRST seen, kept rather than
    // re-derived at the burst -- because by then MF_MISSILE is gone and
    // RT_ProjectileSmokeFor answers nullptr. The death event is precisely the
    // moment the actor stops being matchable, so the match has to outlive it.
    const ProjectileSmoke* row;
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
    uint8_t      frame;    // 0 == 'A'; the fire frame. F on the soldiers, E on CYBR
    int          parcels;  // ABSOLUTE, not a multiplier on rt_smoke_count. That
                           // cvar is the player's "more smoke" knob and has been
                           // moved once already; a monster's shot should not get
                           // bigger every time the shotgun does. The row's count
                           // multiplier is derived from this at spawn.
    // WHERE THE GUN IS, stated per row rather than assumed. The soldiers hold a
    // weapon at chest height on the centre line; the Cyberdemon's arm cannon is
    // neither -- its DECORATE says A_CustomMissile( "64CyberRocket", 81, -31 ),
    // i.e. 81 units up and 31 to the side of a 160-tall actor. Guessing a
    // fraction of the height would put its smoke in the middle of its chest.
    float        zFrac;    // fraction of the actor's height
    float        side;     // MAP UNITS across the facing direction; + is right
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
    { "POSS", 5, 1, 0.58f, 0.f, { "zombieman rifle", 0.f, 0.35f, 0.55f, 0.85f, 0.8f, 0.5f, 1.2f, 0, 0, 0.7f,
                   "one small parcel off the barrel" } },
    // 64ShotgunGuy -- a wide bore, so a SCATTER, for the reason the player's
    // shotgun is sparse: one fat parcel is a grey wall, several small ones read
    // as a burst and leave gaps to see through.
    { "SPOS", 5, 2, 0.58f, 0.f, { "shotgun guy", 0.f, 0.50f, 0.50f, 0.95f, 0.7f, 1.6f, 1.1f, 0, 0, 0.9f,
                   "a scatter, like the player's shotgun" } },
    // Not in Retribution (no 64ChaingunGuy), kept because the engine loads on
    // DOOM2.WAD and the actor is one -file away.
    { "CPOS", 5, 1, 0.58f, 0.f, { "chaingun guy", 0.f, 0.32f, 0.45f, 0.70f, 0.8f, 0.6f, 1.2f, 0, 0, 0.7f,
                   "thinner and shorter -- it fires again immediately" } },
    // 64MarineBot, which uses A_PosAttack on the player sprite.
    { "PLAY", 5, 1, 0.58f, 0.f, { "marine bot", 0.f, 0.35f, 0.55f, 0.85f, 0.8f, 0.5f, 1.2f, 0, 0, 0.7f,
                   "as the zombieman" } },
    { "SSWV", 5, 1, 0.58f, 0.f, { "wolfenstein ss", 0.f, 0.32f, 0.45f, 0.70f, 0.8f, 0.6f, 1.2f, 0, 0, 0.7f,
                   "as the chaingun guy" } },
    // THE CYBERDEMON, and it is the odd row in three ways.
    //
    // Its fire frame is E, not F -- F is the one it faces you on. Its Missile
    // state fires THREE times, and the edge test handles that for free because
    // DECORATE returns to F between shots:
    //
    //     CYBR FF   4 A_FaceTarget
    //     CYBR E    4 A_CustomMissile( "64CyberRocket", 81, -31, ... )   <- shot
    //     CYBR EE   4 A_FaceTarget
    //     CYBR FFFF 4 A_FaceTarget
    //     CYBR E    4 A_CustomMissile( ... )                             <- shot
    //
    // And the gun is an arm cannon at 81 units up and 31 to the left of a
    // 160-tall, 52-radius actor, so zFrac/side carry those rather than the
    // soldiers' chest-height assumption.
    //
    // Its ROCKETS already smoke: 64CyberRocket matches the rocket row in
    // RT_PROJECTILE_SMOKE by class name. This is only the muzzle -- a big dirty
    // one, because it is a siege gun.
    { "CYBR", 4, 4, 0.506f, -31.f, { "cyberdemon", 0.f, 1.30f, 0.65f, 1.20f, 0.7f, 2.2f, 0.9f, 0, 0, 1.0f,
                   "arm cannon -- big and dirty" } },
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

// When each emitter is next due. Unlike the gunners this is not an edge test --
// a flame is always burning -- so the state is a countdown rather than a bool.
struct AmbientMark
{
    AActor* mo;
    int     nextTic;
    int     lastTic;
};
static std::vector< AmbientMark > g_ambient;

// How many ambient puffs are alive right now, maintained by the spawn and the
// expiry rather than recounted. rt_smoke_ambient_budget bounds it.
uint32_t rtx::g_smokeAmbientCount = 0;


// SMOKING PROJECTILES. A trail along the flight and a burst where it dies.
//
// Keyed by SPRITE, not by class name, for the reason the monster guns are: the
// sprite is what the art shows and it is stable across the WAD's class naming,
// where "Rocket" needed two exclusions to stop matching the launcher and its own
// smoke actor. The rocket rows keep a class match as well, because the rocket
// family is genuinely named rather than sprited alike.
//
// The rows are ABSOLUTE numbers, not multipliers on the puff cvars: a rocket's
// trail should not change because the player's shotgun got smokier.
struct ProjectileSmoke
{
    const char* sprite;      // 4-character sprite name; nullptr = match by class
    const char* cls;         // substring of the class name, when sprite is null
    int         trailEvery;  // tics between trail parcels
    float       trailRadius; // metres
    float       trailDens;   // multiplier on rt_smoke_density
    float       trailLife;   // multiplier on rt_smoke_life
    int         boom;        // parcels in the death burst; 0 = no burst
    float       boomRadius;  // metres
    float       boomDens;
    const char* note;
};

static const ProjectileSmoke RT_PROJECTILE_SMOKE[] = {
    // The rocket keeps its cvars, because those are the ones already tuned in
    // play and exposed to the player -- this row reads them rather than
    // restating them. Matched by CLASS, since 64Rocket and 64CyberRocket share
    // no sprite but are the same thing, and the launcher and 64RocketSmokeTrail
    // have to be excluded by name.
    { nullptr, "Rocket", 0, 0.f, 0.f, 0.f, 0, 0.f, 0.f, "rocket -- uses rt_smoke_rocket_*" },
    // 64TracerMissile, the revenant's homing shot. It already trails its own
    // sprite puffs; this is the medium those sprites only imply. Thin and
    // short-lived, because it is FAST and a long trail on a seeker turns into a
    // wall of smoke as it circles.
    { "TRCR", nullptr, 2, 0.10f, 0.30f, 0.55f, 4, 0.26f, 0.5f, "tracer -- thin, fast, short" },
    // 64MotherBall, the same actor family from the Mother Demon.
    { "RBAL", nullptr, 2, 0.11f, 0.32f, 0.60f, 4, 0.28f, 0.5f, "mother ball -- as the tracer" },
    // 64FatShot, the mancubus. Fat and slow, so it can afford a wider trail --
    // and a wider one is what sells the difference from the tracer.
    // Thinned twice over: it fires in VOLLEYS, so what matters is not how one
    // fireball looks but how six of them look at once -- and the life is a
    // multiplier on rt_smoke_life, which grew 1.6 -> 2.2 earlier and took this
    // with it. 27 parcels alive per shot became ~11.
    { "MANF", nullptr, 3, 0.16f, 0.30f, 0.45f, 3, 0.34f, 0.6f, "fat shot -- wider, slower" },
    // 64MotherFire. SHARES ITS SPRITE with 64BigFire, the ambient bonfire that
    // stands in 117 places across nine maps -- so this row is only ever reached
    // because the MF_MISSILE test below runs FIRST. A bonfire is not a
    // projectile; matching FIRE without that flag would have put a rocket trail
    // on every torch in the game.
    { "FIRE", nullptr, 2, 0.14f, 0.35f, 0.60f, 5, 0.32f, 0.6f, "mother fire -- burning, not powder" },
};

// MF_MISSILE IS THE TEST THAT MATTERS, and leaving it out was a real bug.
//
// P_ExplodeMissile clears MF_MISSILE but the actor LIVES ON, same class, through
// its whole death animation. Matching on the name alone therefore kept seeing a
// rocket that had already hit the wall -- stationary, still "alive" -- and kept
// dropping trail parcels into it for as long as the explosion played. That is
// the cluster of small puffs that sat against a wall and never faded: they were
// not failing to expire, they were being continuously replaced.
//
// It also fixes WHEN the burst happens. Losing the flag is the explosion, so the
// burst fires on that frame rather than whenever the actor finally leaves the
// thinker list.
//
// And it is what keeps FIRE's two owners apart: see the row above.
static const ProjectileSmoke* RT_ProjectileSmokeFor( AActor* mo )
{
    if( !mo || !mo->GetClass() || !( mo->flags & MF_MISSILE ) )
    {
        return nullptr;
    }

    const char* c  = mo->GetClass()->TypeName.GetChars();
    const char* sn = ( mo->sprite >= 0 && mo->sprite < int( sprites.Size() ) )
                         ? sprites[ mo->sprite ].name
                         : nullptr;

    for( const ProjectileSmoke& p : RT_PROJECTILE_SMOKE )
    {
        if( p.sprite )
        {
            if( sn && strnicmp( sn, p.sprite, 4 ) == 0 )
            {
                return &p;
            }
        }
        else if( p.cls && c && strstr( c, p.cls ) && !strstr( c, "Launcher" ) &&
                 !strstr( c, "Smoke" ) )
        {
            return &p;
        }
    }
    return nullptr;
}

// The rocket row is the one whose numbers live in cvars, so it is identified
// rather than special-cased at every use.
static bool RT_IsRocketRow( const ProjectileSmoke* p )
{
    return p && p->sprite == nullptr;
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
    g_ambient.clear();
    g_smokeAmbientCount = 0;
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
        //
        // AMBIENT FIRST, THOUGH. Torch smoke is continuous and a gunshot is not,
        // so a plain oldest-out rule hands the pool to whichever source never
        // stops -- and the smoke that matters, the one at the end of the gun you
        // are holding, is exactly the one that would be pushed out. So the
        // eviction looks for the oldest AMBIENT puff first and only falls back
        // to the oldest of all when there is none. An ambient spawn arriving at
        // a full pool therefore replaces ambient smoke, never yours.
        uint32_t slot;
        if( g_smokePuffCount < g_smokePuffs.size() )
        {
            slot = g_smokePuffCount++;
        }
        else
        {
            slot                = UINT32_MAX;
            uint32_t oldestAny  = 0;
            for( uint32_t j = 0; j < g_smokePuffCount; j++ )
            {
                if( g_smokePuffs[ j ].age > g_smokePuffs[ oldestAny ].age )
                {
                    oldestAny = j;
                }
                if( g_smokePuffs[ j ].ambient &&
                    ( slot == UINT32_MAX || g_smokePuffs[ j ].age > g_smokePuffs[ slot ].age ) )
                {
                    slot = j;
                }
            }
            if( slot == UINT32_MAX )
            {
                slot = oldestAny;
            }
            if( g_smokePuffs[ slot ].ambient && g_smokeAmbientCount > 0 )
            {
                g_smokeAmbientCount--;
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
        puff.ambient = prof.ambient;
        if( puff.ambient )
        {
            g_smokeAmbientCount++;
        }

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

    // Height as a FRACTION of the actor's own, and a little past its own radius
    // so the puff is not born inside the body. Both derived from the actor
    // rather than hardcoded, because Retribution's soldiers are 80 units tall
    // where the stock ones are 56 -- and the Cyberdemon is 160.
    const double gz = mo->Z() + mo->Height * double( gun->zFrac );
    const double gr = mo->radius + 6.0;

    // The side offset is across the facing direction: (cos, sin) rotated by 90
    // degrees is (-sin, cos). This is what puts the Cyberdemon's smoke on its
    // arm cannon instead of in the middle of its chest.
    const double sx = -std::sin( yaw ) * double( gun->side );
    const double sy = std::cos( yaw ) * double( gun->side );

    const FVector3 muzzle{ float( mo->X() + std::cos( yaw ) * gr + sx ) * ONEGAMEUNIT_IN_METERS,
                           float( mo->Y() + std::sin( yaw ) * gr + sy ) * ONEGAMEUNIT_IN_METERS,
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

// AMBIENT SMOKE: a thin column rising off every flame in the level.
//
// THIS IS A DIFFERENT KIND OF SOURCE FROM EVERYTHING ABOVE, and the difference
// is what all the extra machinery here is for. A shot, an explosion and a
// projectile are EVENTS: they happen, they emit, they are done, and the budget
// question answers itself because the player can only fire so fast. A torch
// never stops. There are twelve torch sprites, four loose-fire ones and 117
// placements of 64BigFire alone across nine maps -- so ambient smoke would win
// every contest for a slot simply by outlasting everything else, and the smoke
// off your own barrel would be evicted by a sconce in the next room.
//
// Three things keep that from happening, and none of them is optional:
//
//   1. rt_smoke_ambient_far   -- a hard distance cull, tighter than the weapon
//                                one, because a torch across the map is not
//                                resolvable by the froxel grid anyway.
//   2. rt_smoke_ambient_budget-- a ceiling on how many ambient puffs may be
//                                ALIVE. Emission stops at the cap instead of
//                                pushing other smoke out.
//   3. the ambient flag       -- the pool evicts ambient puffs before any
//                                other, so even at the cap a shot always finds
//                                room. See RT_SpawnSmokePuffs.
//
// The offsets come from RT_FLAME_KINDS via RT_FlameSpriteOffset, so the smoke
// leaves the flame from the same point the light does.
struct AmbientFlame
{
    const char* sprite;
    float       upFallback; // map units, used only when the sprite is not a
                            // known flame kind -- SKUL is the case that needs it
    int         every;      // tics between parcels
    float       radius;     // metres
    float       density;    // multiplier on rt_smoke_density
    float       life;       // multiplier on rt_smoke_life
    float       rise;       // multiplier on rt_smoke_rise
    const char* note;
};

// A flame's smoke is THIN and SLOW, which is the opposite of a gunshot's. It has
// to read as a heat shimmer carrying soot upward, not as a puff -- a torch that
// coughs like a shotgun looks like it is about to go out. Low density, long
// life, strong rise, and a spacing wide enough that one torch is a few parcels
// rather than a column of them.
static const AmbientFlame RT_AMBIENT_FLAMES[] = {
    // Standing torches, the tallest flames in the game.
    { "TLBL", 80.f, 14, 0.20f, 0.22f, 1.5f, 2.2f, "long torch" },
    { "TLGR", 80.f, 14, 0.20f, 0.22f, 1.5f, 2.2f, "long torch" },
    { "TLRD", 80.f, 14, 0.20f, 0.22f, 1.5f, 2.2f, "long torch" },
    { "TLYL", 80.f, 14, 0.20f, 0.22f, 1.5f, 2.2f, "long torch" },
    { "TSBL", 64.f, 14, 0.18f, 0.20f, 1.5f, 2.2f, "short torch" },
    { "TSGR", 64.f, 14, 0.18f, 0.20f, 1.5f, 2.2f, "short torch" },
    { "TSRD", 64.f, 14, 0.18f, 0.20f, 1.5f, 2.2f, "short torch" },
    { "TSYL", 64.f, 14, 0.18f, 0.20f, 1.5f, 2.2f, "short torch" },
    // Wall sconces: smaller flame, so a smaller and slower wisp.
    { "A030", 24.f, 18, 0.15f, 0.18f, 1.4f, 2.0f, "wall sconce" },
    { "A031", 24.f, 18, 0.15f, 0.18f, 1.4f, 2.0f, "wall sconce" },
    { "A032", 24.f, 18, 0.15f, 0.18f, 1.4f, 2.0f, "wall sconce" },
    { "GTCH", 24.f, 18, 0.15f, 0.18f, 1.4f, 2.0f, "wall sconce" },
    // Loose fires burning on the floor.
    { "BFLM", 8.f, 16, 0.17f, 0.20f, 1.4f, 2.0f, "floor fire" },
    { "GFLM", 8.f, 16, 0.17f, 0.20f, 1.4f, 2.0f, "floor fire" },
    { "RFLM", 8.f, 16, 0.17f, 0.20f, 1.4f, 2.0f, "floor fire" },
    { "YFLM", 8.f, 16, 0.17f, 0.20f, 1.4f, 2.0f, "floor fire" },
    // 64BigFire, the bonfire -- by far the most common flame in the game and the
    // only one big enough to earn a real plume. Also the one to watch if ambient
    // smoke ever looks like too much: 117 placements across nine maps.
    //
    // The row is only reached for the AMBIENT bonfire. 64MotherFire shares this
    // sprite and is a projectile, and the walk hands anything with MF_MISSILE to
    // RT_PROJECTILE_SMOKE before it gets here.
    { "FIRE", 32.f, 10, 0.30f, 0.30f, 1.6f, 2.4f, "bonfire" },
    // THE LOST SOUL, which is a flame that flies at you. Its head IS the fire,
    // so the wisp comes off the sprite's middle rather than above it, and
    // because the actor moves the parcels are left behind as a trail -- the one
    // ambient emitter whose output reads as motion. SKUL is not in
    // RT_FLAME_KINDS (its light rides on the sprite itself; see
    // AGENTS.md), so upFallback is what places this one.
    { "SKUL", 20.f, 5, 0.16f, 0.22f, 1.1f, 1.2f, "lost soul -- a burning head" },
};

static const AmbientFlame* RT_AmbientFlameFor( AActor* mo )
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
    for( const AmbientFlame& f : RT_AMBIENT_FLAMES )
    {
        if( strnicmp( sn, f.sprite, 4 ) == 0 )
        {
            return &f;
        }
    }
    return nullptr;
}

static void RT_AmbientFlameSmoke( AActor* mo, int tic, const FVector3& camPos, float farMetres )
{
    const AmbientFlame* fl = RT_AmbientFlameFor( mo );
    if( !fl )
    {
        return;
    }
    // A dead Lost Soul is a corpse, not a flame. The other rows are props and
    // have no health to speak of, so this only ever bites on SKUL -- which is
    // the point: its fire goes out when it dies.
    if( mo->health <= 0 && ( mo->flags3 & MF3_ISMONSTER ) )
    {
        return;
    }

    // The flame's own height above the origin, from the table the LIGHT uses,
    // so the smoke and the glow leave the same point.
    float up = fl->upFallback;
    RT_FlameSpriteOffset( mo, &up );

    const FVector3 at{ float( mo->X() ) * ONEGAMEUNIT_IN_METERS,
                       float( mo->Y() ) * ONEGAMEUNIT_IN_METERS,
                       float( mo->Z() + double( up ) ) * ONEGAMEUNIT_IN_METERS };

    // CULL BEFORE TAKING A SLOT IN THE LIST. A level can hold a hundred of
    // these and only the few in this room can matter.
    if( farMetres > 0.f && ( at - camPos ).LengthSquared() > farMetres * farMetres )
    {
        return;
    }

    AmbientMark* mark = nullptr;
    for( AmbientMark& a : g_ambient )
    {
        if( a.mo == mo )
        {
            mark = &a;
            break;
        }
    }
    if( !mark )
    {
        // Stagger the first release by the pointer's low bits. Without this
        // every torch in a room that came into range together would breathe in
        // perfect unison, which reads as a pulse rather than as fire.
        const int jitter = int( ( uintptr_t( mo ) >> 4 ) % uintptr_t( std::max( 1, fl->every ) ) );
        g_ambient.push_back( AmbientMark{ mo, tic + jitter, tic } );
        mark = &g_ambient.back();
    }
    mark->lastTic = tic;

    if( tic < mark->nextTic )
    {
        return;
    }
    mark->nextTic = tic + std::max( 1, fl->every );

    // THE CAP, checked at the last moment so the schedule keeps running: an
    // emitter held off by the budget resumes on its own cadence rather than
    // catching up in a burst the instant a slot frees.
    if( g_smokeAmbientCount >= uint32_t( std::max( 0, int{ cvar::rt_smoke_ambient_budget } ) ) )
    {
        return;
    }

    const float scale = std::max( 0.f, float{ cvar::rt_smoke_ambient_scale } );

    SmokeProfile p{};
    p.cls     = fl->note;
    p.count   = 1.f / std::max( 1.f, float( int{ cvar::rt_smoke_count } ) );
    p.radius  = std::max( 0.01f, fl->radius ) /
                std::max( 0.001f, float{ cvar::rt_smoke_radius } );
    p.density = fl->density * scale;
    p.life    = fl->life;
    p.speed   = 0.f;    // it rises, it does not travel: there is no barrel here
    p.spread  = 0.12f;
    p.rise    = fl->rise;
    p.growth  = 0.35f;
    p.trail      = 0;
    p.trailEvery = 0;
    p.note       = fl->note;
    p.ambient    = true;

    if( p.density <= 0.f )
    {
        return;
    }

    // Straight up. The parcels of a MOVING emitter (the Lost Soul) are left
    // where they were born, so its trail comes from the actor travelling rather
    // than from any velocity given here -- the same split the muzzle trail uses.
    RT_SpawnSmokePuffs( at, at, FVector3{ 0, 0, 1 }, FVector3{ 0, 0, 0 }, p );
}

// AN EXPLODING BARREL, which is the sprite-frame trigger again on an actor that
// is not a monster and never becomes a projectile.
//
// 64ExplosiveBarrel's death is a state, and A_Explode sits on a specific frame:
//
//     Death:
//         BEXP ABC 5
//         BEXP D   5 A_Scream
//         BEXP E   0 A_SpawnItemEx( "64BarrelExplosion", ... )
//         BEXP E   5 A_Explode                                  <- the bang
//
// So the burst goes on entering frame E (index 4), not on the actor's removal:
// the barrel lingers for a full 1050-tic respawn timer afterwards, so
// disappearance -- the rule the projectiles use -- would put the smoke somewhere
// around twenty seconds late.
//
// The health test the gunner path uses is wrong here for the same reason. A
// barrel in its death state HAS no health; that is the whole point.
static void RT_BarrelSmoke( AActor* mo, int tic, const FVector3& camPos, float farMetres )
{
    if( !mo || mo->sprite < 0 || mo->sprite >= int( sprites.Size() ) )
    {
        return;
    }
    const char* sn = sprites[ mo->sprite ].name;
    if( !sn || strnicmp( sn, "BEXP", 4 ) != 0 )
    {
        return;
    }

    // Shares the gunner marks: it is the same rising-edge question, and one
    // list means one sweep and one place a stale pointer can be dropped.
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

    const bool blowing = ( mo->frame == 4 );   // 'E'
    const bool edge    = blowing && !mark->firing;
    mark->firing       = blowing;

    if( !edge )
    {
        return;
    }

    const FVector3 at{ float( mo->X() ) * ONEGAMEUNIT_IN_METERS,
                       float( mo->Y() ) * ONEGAMEUNIT_IN_METERS,
                       float( mo->Z() + mo->Height * 0.5 ) * ONEGAMEUNIT_IN_METERS };

    if( farMetres > 0.f && ( at - camPos ).LengthSquared() > farMetres * farMetres )
    {
        return;
    }

    // The fat one. An explosion is the one case where a cloud that obscures is
    // correct, and unlike the rocket's this one does not have to leave the shot
    // you just took visible.
    const float scale = std::max( 0.f, float{ cvar::rt_smoke_barrel_scale } );

    SmokeProfile b{};
    b.cls   = "barrel";
    b.count = ( 6.f * scale ) / std::max( 1.f, float( int{ cvar::rt_smoke_count } ) );
    b.radius  = 0.40f / std::max( 0.001f, float{ cvar::rt_smoke_radius } );
    b.density = 0.8f * scale;
    b.life    = 2.0f;
    b.speed   = 0.f;
    b.spread  = 4.5f;   // thrown outward: a barrel goes off in every direction,
                        // where a rocket's burst still has the flight in it
    b.rise    = 2.0f;
    b.growth  = 1.2f;
    b.trail      = 0;
    b.trailEvery = 0;

    if( b.count <= 0.f )
    {
        return;
    }

    // Straight up, because a barrel has no facing that means anything -- the
    // spread is what shapes this, not the direction.
    RT_SpawnSmokePuffs( at, at, FVector3{ 0, 0, 1 }, FVector3{ 0, 0, 0 }, b );

    if( cvar::rt_smoke_debug )
    {
        Printf( "rt_smoke BARREL: burst at %.2f %.2f %.2f (tic %d)\n", at.X, at.Y, at.Z, tic );
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
        g_smokePuffCount    = 0;
        g_smokeAmbientCount = 0;
        g_smokeLastTic      = primaryLevel->maptime;
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

            // A NAMED PROFILE, or the identity if none was asked for. Autospawn
            // runs with no ready weapon, so RT_SmokeProfileFor answers the
            // identity row -- which has trail 0, i.e. the autospawn diagnostic
            // could never show the filament the pistol row exists to make. That
            // is why judging the wisp needed a rig rather than this cvar alone.
            const int    wi = int{ cvar::rt_smoke_autoweapon };
            SmokeProfile ap = RT_SmokeProfileFor( vp.camera );
            if( wi > 0 && wi <= int( std::size( RT_SMOKE_PROFILES ) ) )
            {
                ap = RT_SMOKE_PROFILES[ wi - 1 ];
            }

            // THE REAL MUZZLE GEOMETRY, not a round 1.5 m. RT_AddMuzzleFlash
            // builds its point as eye + forward * rt_mzlflsh_f + up *
            // rt_mzlflsh_u, which at the shipping 3.0 / -0.9 is 3.1 m out --
            // and rt_smoke_offset then births the puff at 0.7 of that, ~2.2 m.
            //
            // Autospawn used a flat 1.5 m, so the diagnostic put smoke at 1.05 m
            // and the lab looked healthy while the game did not. A diagnostic
            // that does not reproduce the geometry it is diagnosing is worse
            // than none: it produces confident readings about a different case.
            const FVector3 up{ 0, 0, 1 };
            const FVector3 muzzleNow = eye + fwd * float{ cvar::rt_mzlflsh_f } +
                                       up * float{ cvar::rt_mzlflsh_u };
            RT_SpawnSmokePuffs( eye, muzzleNow, fwd, FVector3{ 0, 0, 0 }, ap );

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
    const bool wantRocket     = bool{ cvar::rt_smoke_rocket };
    const bool wantProjectile = bool{ cvar::rt_smoke_projectile };
    const bool wantMonster    = bool{ cvar::rt_smoke_monster } && steps > 0;
    const bool wantBarrel     = bool{ cvar::rt_smoke_barrel } && steps > 0;
    const bool wantAmbient    = bool{ cvar::rt_smoke_ambient_fx } && steps > 0;

    if( wantRocket || wantProjectile || wantMonster || wantBarrel || wantAmbient )
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
        // Tighter than the weapon cull on purpose: there are far more flames in
        // a level than there are gunmen, and a torch two rooms away is a slot
        // spent on something the froxel grid cannot resolve anyway.
        const float    ambFar = std::max( 0.f, float{ cvar::rt_smoke_ambient_far } );

        auto it = primaryLevel->GetThinkerIterator< AActor >();
        while( AActor* mo = it.Next() )
        {
            const ProjectileSmoke* row = RT_ProjectileSmokeFor( mo );
            if( !row )
            {
                // Not in flight. The two sprite-frame sources look at it
                // instead -- a firing soldier and a barrel mid-explosion are
                // both ordinary actors, which is exactly why neither could be
                // found the way a projectile is.
                if( wantMonster )
                {
                    RT_MonsterGunSmoke( mo, tic, camPos, monFar );
                }
                if( wantBarrel )
                {
                    RT_BarrelSmoke( mo, tic, camPos, monFar );
                }
                if( wantAmbient )
                {
                    RT_AmbientFlameSmoke( mo, tic, camPos, ambFar );
                }
                continue;
            }
            // rt_smoke_rocket gates the rocket rows, rt_smoke_projectile
            // everything else that flies. Two switches because they are two
            // different judgements: a rocket obviously smokes, a mancubus
            // fireball is a taste call.
            if( RT_IsRocketRow( row ) ? !wantRocket : !wantProjectile )
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
                g_rockets.push_back( RocketMark{ mo, p, FVector3{ 0, 0, 1 }, tic, tic, row } );
                mark = &g_rockets.back();
                if( cvar::rt_smoke_debug )
                {
                    Printf( "rt_smoke PROJECTILE: tracking %s (%s) at %.2f %.2f %.2f (tic %d)\n",
                            mo->GetClass()->TypeName.GetChars(), row->note, p.X, p.Y, p.Z, tic );
                }
            }

            const FVector3 d = p - mark->lastPos;
            if( d.LengthSquared() > 0.0001f )
            {
                mark->dir = d.Unit();
            }
            mark->lastPos = p;
            mark->lastTic = tic;

            // The rocket's numbers are cvars because they are the ones tuned
            // in play and exposed to the player; every other projectile carries
            // its own, because nobody is going to tune five of them by hand.
            const bool  isRocket = RT_IsRocketRow( row );
            const int   every    = isRocket ? std::max( 1, int{ cvar::rt_smoke_rocket_every } )
                                            : std::max( 1, row->trailEvery );
            const float trailR   = isRocket ? float{ cvar::rt_smoke_rocket_radius }
                                            : row->trailRadius;

            if( tic >= mark->nextTrail )
            {
                mark->nextTrail = tic + every;

                SmokeProfile p2{};
                p2.cls = row->note;
                // EXACTLY one parcel per drop, whatever rt_smoke_count is.
                // Spelling this as a bare 0.3 worked only while that cvar was 3;
                // raising it to 4 turned ceil(4 x 0.3) into two, which empties
                // the whole budget on a single rocket in under a second. The
                // division is what makes the player's knob a player's knob.
                p2.count = 1.f / std::max( 1.f, float( int{ cvar::rt_smoke_count } ) );
                p2.radius = std::max( 0.01f, trailR ) /
                            std::max( 0.001f, float{ cvar::rt_smoke_radius } );
                // 1.0 was 77 parcels alive from ONE rocket -- the life
                // multiplies rt_smoke_life, which went 1.6 -> 2.2 earlier, so
                // this row silently grew 37% along with it. Against a 48-puff
                // upload budget that meant a single rocket owned the whole
                // frame and a second one evicted the first. 0.55 puts it at 21.
                p2.density = isRocket ? 0.35f : row->trailDens;
                p2.life    = isRocket ? 0.55f : row->trailLife;
                                    // a shorter trail is a SHORTER trail: the
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
        for( size_t i = 0; i < g_rockets.size(); )
        {
            if( g_rockets[ i ].lastTic < tic )
            {
                const ProjectileSmoke* r  = g_rockets[ i ].row;
                const bool             rk = RT_IsRocketRow( r );

                SmokeProfile b{};
                b.cls   = "boom";
                b.count = float( std::max( 0, rk ? int{ cvar::rt_smoke_boom } : r->boom ) ) /
                          std::max( 1.f, float( int{ cvar::rt_smoke_count } ) );
                b.radius = std::max( 0.02f, rk ? float{ cvar::rt_smoke_boom_radius }
                                               : r->boomRadius ) /
                           std::max( 0.001f, float{ cvar::rt_smoke_radius } );
                // A burst of ten dense parcels was the noisiest thing in the
                // game by construction: each is an independent one-sample
                // estimate stacked on the others. Fewer and thinner, which is
                // the same lesson the shotgun taught -- less smoke reads better
                // AND is cheaper to light.
                b.density = rk ? 0.7f : r->boomDens;
                b.life    = 1.8f;
                b.speed   = 0.f;
                b.spread  = 4.0f;   // thrown outward, which is what makes it a burst
                b.rise    = 1.6f;
                b.growth  = 1.1f;
                b.trail = 0;
                b.trailEvery = 0;
                if( cvar::rt_smoke_debug )
                {
                    Printf( "rt_smoke PROJECTILE: burst (%s) at %.2f %.2f %.2f (tic %d) -- "
                            "MF_MISSILE cleared or actor gone\n",
                            r->note,
                            g_rockets[ i ].lastPos.X,
                            g_rockets[ i ].lastPos.Y,
                            g_rockets[ i ].lastPos.Z,
                            tic );
                }
                if( b.count > 0.f )
                {
                    RT_SpawnSmokePuffs( g_rockets[ i ].lastPos - g_rockets[ i ].dir,
                                        g_rockets[ i ].lastPos,
                                        g_rockets[ i ].dir,
                                        FVector3{ 0, 0, 0 },
                                        b );
                }
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
        if( wantMonster || wantBarrel )
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

        // The same sweep for the flame emitters. It matters more here: this
        // list gains an entry for every torch that comes into range, so without
        // it a long level would accumulate a mark per flame ever visited, all
        // of them holding pointers to actors that may since have been removed.
        if( wantAmbient )
        {
            for( size_t i = 0; i < g_ambient.size(); )
            {
                if( g_ambient[ i ].lastTic < tic )
                {
                    g_ambient[ i ] = g_ambient.back();
                    g_ambient.pop_back();
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
                if( puff.ambient && g_smokeAmbientCount > 0 )
                {
                    g_smokeAmbientCount--;
                }
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
    // The ambient split is the number worth having here. "48 live" says nothing
    // about whether a room full of torches has spent the pool; "48 live, 40 of
    // them ambient" says it outright -- and 52 IS rt_smoke_ambient_budget, so a
    // reading sitting at the cap tells you the flames are being held back on
    // purpose rather than that something is broken.
    Printf( "smoke: rt_smoke %d, %u puff(s) live (%u ambient, cap %d), probe mode %d -- %s\n",
            int{ cvar::rt_smoke },
            g_smokePuffCount,
            g_smokeAmbientCount,
            int{ cvar::rt_smoke_ambient_budget },
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
