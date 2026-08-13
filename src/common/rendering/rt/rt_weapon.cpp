// First-person weapon lighting: the flashlight, the muzzle flash and the gun's
// own glow.
//
// The one part of the render state that is about the PLAYER rather than the
// scene. It resolves a muzzle position once and hands the same point to both the
// light and the smoke it lights, which is why RT_SpawnSmokePuffs is called from
// here rather than from the smoke simulation.
//
// Split out of the class body in rt_renderstate.h. Behaviour unchanged; the only
// edits are the RTRenderState:: qualification and one level of de-indentation.

#include "rt_renderstate.h"

// The shared internals come in unqualified, exactly as when this code lived
// inside rt_main.cpp's anonymous namespace.
using namespace rtx;

void RTRenderState::RT_AddFlashlight( const RgFloat3D& basePosition,
                       const RgFloat3D& forward,
                       const RgFloat3D& up,
                       const RgFloat3D& right )
{
    auto enabled = []() {
        if( cvar::rt_pw_lightamp == 2 )
        {
            if( RT_CalcPowerupFlags() & RT_POWERUP_FLAG_FLASHLIGHT_BIT )
            {
                return true;
            }
        }
        if( cvar::rt_flsh )
        {
            return true;
        }
        return false;
    };

    const bool wantLight = enabled();

    // Battery cycle: on → dying flicker → recharge → repeat (horror lantern).
    enum BattState : int
    {
        BattOff      = 0,
        BattOn       = 1,
        BattDying    = 2,
        BattRecharge = 3,
    };

    static bool     s_wasOn       = false;
    static int      s_state       = BattOff;
    // Persistent charge (0..1). Survives switching the flashlight off, so a
    // toggle no longer refills the cell; it keeps trickling back up instead.
    static float    s_charge      = 1.f;
    static int      s_lastTime    = -1;
    static int      s_onLen       = 0; // tics of a full drain (rolled per cycle)
    static int      s_dieLen      = 0; // tics of dying flicker at the end of it
    static int      s_offLen      = 0; // tics of a full post-burnout recharge
    static int      s_dyingStart  = -1;
    static bool     s_inFade      = false;
    static int      s_maptoken    = -1;
    static int      s_nextBlinkAt = -1;
    static int      s_blinkStart  = -1;
    static int      s_blinkDur    = 0;
    // Bumped every time the beam starts a fade-out (mid-cycle blink, dying
    // gutter, burnout). The flashlight pk3 watches this and plays a sound.
    static int      s_flickerSeq  = 0;
    // DLSS-RR light-cut edge detector (see below); reset on level change
    // alongside the other statics so a fresh map doesn't read a stale edge.
    static bool     s_rrPrevWant  = false;
    static float    s_rrPrevScale = 0.f;

    const int maptime = primaryLevel ? primaryLevel->maptime : 0;
    const int maptoken =
        primaryLevel ? int( reinterpret_cast< uintptr_t >( primaryLevel ) & 0x7fffffff ) : -1;

    auto rollSecs = [ maptime ]( float base, float jitter, int salt ) -> float {
        const float j = std::clamp( float{ cvar::rt_flsh_jitter }, 0.f, 1.f );
        const uint32_t h =
            uint32_t( maptime + salt ) * 1103515245u + 12345u + uint32_t( salt * 97 );
        const float u = float( ( h >> 16 ) & 0x7fffu ) / 32767.f; // [0,1]
        return std::max( 0.5f, base * ( 1.f + j * ( u * 2.f - 1.f ) ) );
    };

    auto rollUnit = [ maptime ]( int salt ) -> float {
        const uint32_t h =
            uint32_t( maptime + salt * 131 ) * 1664525u + 1013904223u;
        return float( ( h >> 16 ) & 0x7fffu ) / 32767.f;
    };

    // Smooth valley: 1 → 0 → 1 over u in [0,1] (slow fade out / fade in).
    auto fadeValley = []( float u ) -> float {
        u = std::clamp( u, 0.f, 1.f );
        return 1.f - std::sin( u * rt_pi() );
    };

    auto scheduleNextBlink = [ & ]( int minSecs, int maxSecs, int salt ) {
        const float u   = rollUnit( salt );
        const float sec = float( minSecs ) + u * float( std::max( 0, maxSecs - minSecs ) );
        s_nextBlinkAt   = maptime + std::max( 1, int( sec * TICRATE ) );
        s_blinkStart    = -1;
        s_blinkDur      = 0;
    };

    // Durations of one battery cycle, jittered. Rolled when the cell reaches
    // a full charge, not when the player flicks the switch.
    auto rollCycle = [ & ]() {
        const float onSecs  = rollSecs( float{ cvar::rt_flsh_on_secs }, float{ cvar::rt_flsh_jitter }, 11 );
        const float dieSecs = std::clamp(
            rollSecs( float{ cvar::rt_flsh_die_secs }, float{ cvar::rt_flsh_jitter }, 29 ),
            0.5f,
            onSecs * 0.8f );
        const float offSecs =
            rollSecs( float{ cvar::rt_flsh_off_secs }, float{ cvar::rt_flsh_jitter }, 47 );
        s_onLen  = std::max( 1, int( onSecs * TICRATE ) );
        s_dieLen = std::max( 1, int( dieSecs * TICRATE ) );
        s_offLen = std::max( 1, int( offSecs * TICRATE ) );
    };

    if( maptoken != s_maptoken )
    {
        s_maptoken    = maptoken;
        s_wasOn       = false;
        s_state       = BattOff;
        s_charge      = 1.f;
        s_lastTime    = maptime;
        s_onLen       = 0;
        s_dyingStart  = -1;
        s_inFade      = false;
        s_nextBlinkAt = -1;
        s_blinkStart  = -1;
        s_rrPrevWant  = false;
        s_rrPrevScale = 0.f;
    }
    if( s_onLen <= 0 )
    {
        rollCycle();
    }

    // Game time elapsed since the last frame, clamped so a pause, a load or
    // a menu doesn't dump a whole cycle into one step.
    int dt = 0;
    if( s_lastTime >= 0 && maptime > s_lastTime )
    {
        dt = std::min( maptime - s_lastTime, TICRATE );
    }
    s_lastTime = maptime;

    float battScale = 1.f;
    float charge    = 0.f;
    int   battState = BattOff;

    if( !cvar::rt_flsh_battery )
    {
        s_wasOn       = wantLight;
        s_state       = wantLight ? BattOn : BattOff;
        s_charge      = 1.f;
        s_nextBlinkAt = -1;
        s_blinkStart  = -1;
        battScale     = wantLight ? 1.f : 0.f;
        charge        = wantLight ? 1.f : 0.f;
        battState     = s_state;
    }
    else
    {
        const float drainPerTic  = 1.f / float( std::max( 1, s_onLen ) );
        const float chargePerTic = 1.f / float( std::max( 1, s_offLen ) );
        const float idleMult =
            std::clamp( float{ cvar::rt_flsh_idle_recharge }, 0.f, 4.f );

        if( s_state == BattRecharge )
        {
            // Burned out: the cell recharges at full rate and the beam stays
            // dead until it is topped up, whatever the switch says.
            s_charge += chargePerTic * float( dt );
            if( s_charge >= 1.f )
            {
                s_charge     = 1.f;
                s_dyingStart = -1;
                s_inFade     = false;
                rollCycle();
                if( wantLight )
                {
                    s_state = BattOn;
                    scheduleNextBlink( 3, 9, 71 );
                }
                else
                {
                    s_state = BattOff;
                }
            }
            else
            {
                battState = BattRecharge;
                battScale = 0.f;
            }
        }

        if( s_state != BattRecharge )
        {
            if( wantLight )
            {
                if( !s_wasOn )
                {
                    // Switched back on: resume from the remembered charge.
                    s_state = BattOn;
                    scheduleNextBlink( 3, 9, 71 );
                }

                s_charge -= drainPerTic * float( dt );

                if( s_charge <= 0.f )
                {
                    s_charge      = 0.f;
                    s_state       = BattRecharge;
                    s_dyingStart  = -1;
                    s_inFade      = false;
                    s_nextBlinkAt = -1;
                    s_blinkStart  = -1;
                    s_flickerSeq++; // last gutter before it dies
                    battState     = BattRecharge;
                    battScale     = 0.f;
                }
                else
                {
                    const float dieFrac =
                        float( s_dieLen ) / float( std::max( 1, s_onLen ) );

                    if( s_charge <= dieFrac )
                    {
                        if( s_state != BattDying )
                        {
                            s_state       = BattDying;
                            s_dyingStart  = maptime;
                            s_inFade      = false;
                            s_nextBlinkAt = -1;
                            s_blinkStart  = -1;
                        }
                        battState = BattDying;
                        // Intermittent slow fade-outs (~every 2.2s, ~0.9s soft valley).
                        constexpr int kDiePeriod = 77; // ~2.2s
                        constexpr int kDieFade   = 32; // ~0.9s
                        const int     local      = ( maptime - s_dyingStart ) % kDiePeriod;
                        const bool    inFade     = local < kDieFade;
                        if( inFade )
                        {
                            if( !s_inFade )
                            {
                                s_flickerSeq++;
                            }
                            battScale = fadeValley( float( local ) / float( kDieFade - 1 ) );
                        }
                        else
                        {
                            battScale = 1.f;
                        }
                        s_inFade = inFade;
                    }
                    else
                    {
                        s_state   = BattOn;
                        battState = BattOn;
                        battScale = 1.f;
                        s_inFade  = false;

                        // Rare single mid-cycle blinks (slow one-shot fade).
                        if( s_blinkStart >= 0 )
                        {
                            const int bt = maptime - s_blinkStart;
                            if( bt >= s_blinkDur )
                            {
                                scheduleNextBlink( 4, 12, maptime + 3 );
                            }
                            else
                            {
                                battScale = fadeValley( float( bt ) / float( std::max( 1, s_blinkDur - 1 ) ) );
                            }
                        }
                        else if( s_nextBlinkAt >= 0 && maptime >= s_nextBlinkAt )
                        {
                            // ~0.35–0.55s single fade-out.
                            const float u = rollUnit( maptime + 5 );
                            s_blinkDur    = 12 + int( u * 8.f ); // 12–20 tics
                            s_blinkStart  = maptime;
                            s_flickerSeq++;
                            battScale     = fadeValley( 0.f );
                        }
                    }
                }
            }
            else
            {
                // Switched off with charge left: trickle back up, slower than
                // the forced recharge that follows a burnout.
                s_charge      = std::min( 1.f, s_charge + chargePerTic * idleMult * float( dt ) );
                s_state       = BattOff;
                s_dyingStart  = -1;
                s_inFade      = false;
                s_nextBlinkAt = -1;
                s_blinkStart  = -1;
                battState     = BattOff;
                battScale     = 0.f;
            }
        }

        s_wasOn = wantLight;
        charge  = s_charge;
    }

    cvar::rt_flsh_charge    = std::clamp( charge, 0.f, 1.f );
    cvar::rt_flsh_battstate = battState;
    cvar::rt_flsh_flicker   = s_flickerSeq;

    // DLSS-RR: flag an abrupt cut (rt_flsh toggle, or recharge<->on) for a
    // history flush. fadeValley() dying-flicker and mid-cycle blinks ramp
    // smoothly over 12-32 tics -- RR tracks those fine, and flushing on
    // every one of them would make the ~4s dying phase permanently noisy.
    if( bool{ cvar::rt_rr_reset_on_lightcut } )
    {
        const float emitted = wantLight ? battScale : 0.f;

        if( wantLight != s_rrPrevWant ||
            std::abs( emitted - s_rrPrevScale ) > float{ cvar::rt_rr_reset_delta } )
        {
            g_rt_lightcut     = true;
            g_rt_lightcut_why = "flashlight";
        }
        s_rrPrevWant  = wantLight;
        s_rrPrevScale = emitted;
    }

    if( !wantLight || battScale <= 0.01f )
    {
        return;
    }

    auto pos = gzvec3( basePosition );
    {
        pos += gzvec3( up ) * cvar::rt_flsh_u;
        pos += gzvec3( right ) * cvar::rt_flsh_r;
        pos += gzvec3( forward ) * cvar::rt_flsh_f;
    }

    // Tip the beam toward the ground (horror lantern wash on floor).
    const float pitchRad = to_rad( float{ cvar::rt_flsh_pitch } );
    const float cp       = std::cos( pitchRad );
    const float sp       = std::sin( pitchRad );
    auto        aim =
        gzvec3( forward ) * cp - gzvec3( up ) * sp;
    if( aim.LengthSquared() < 1.e-8f )
    {
        aim = gzvec3( forward );
    }
    else
    {
        aim = aim.Unit();
    }

    // AIM FROM THE LAMP, not from the eye. This built its target from
    // basePosition while the beam originates at `pos`, which is offset down and
    // to the left (rt_flsh_u -0.58, rt_flsh_r -0.3) -- so the beam converged on
    // the eye's aim line at 20 m and near the player it sat 1.6 degrees HIGHER
    // than the rt_flsh_pitch it was set to.
    //
    // That is small and it matters at the muzzle. Smoke is born ~2.4 m out and
    // 0.7 m below eye level, which is 18.2 degrees off the beam axis against an
    // outer cone of 21 -- right at the edge -- so a degree and a half of stray
    // pitch decides whether the puff is lit or not. It was reported as the
    // flashlight only catching the smoke just above the gun.
    //
    // Aiming from the origin makes the beam actually point where rt_flsh_pitch
    // says, at every distance rather than only at 20 m.
    auto target = pos + 20 * aim;
    auto dir    = ( target - pos ).Unit();

    const float intensity =
        float{ cvar::rt_flsh_intensity } * battScale;

    auto spot = RgLightSpotEXT{
        .sType      = RG_STRUCTURE_TYPE_LIGHT_SPOT_EXT,
        .pNext      = nullptr,
        .color      = cvarcolor_to_rtcolor( cvar::rt_flsh_color ),
        .intensity  = intensity,
        .position   = { pos.X, pos.Y, pos.Z },
        .direction  = { dir.X, dir.Y, dir.Z },
        .radius     = cvar::rt_flsh_radius,
        .angleOuter = to_rad( cvar::rt_flsh_angle ),
        .angleInner = 0,
    };

    auto light = RgLightInfo{
        .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
        .pNext        = &spot,
        .uniqueID     = FlashlightLightId,
        .isExportable = false,
    };

    RgResult r = rt.rgUploadLight( &light );
    RG_CHECK( r );
}

// Doom 64's flash art is not all orange: the chaingun's is blue-purple, the plasma
// rifle's blue, the BFG's green, the unmaker's red. Substring match because the mod
// REPLACES the IWAD classes with 64-prefixed ones (64Chaingun, 64BFG9000, ...) and
// both names must hit. 64Nailgun inherits ChaingunBackup but is not named "Chaingun",
// so it keeps the default — deliberate, its flash art is warm.
// Relative luminance of a colour cvar, Rec.709.
float RTRenderState::cvarcolor_luma( const FColorCVarRef& c )
{
    const uint32_t ba = *( c );
    return 0.2126f * float( RPART( ba ) ) + 0.7152f * float( GPART( ba ) ) +
           0.0722f * float( BPART( ba ) );
}



RTRenderState::MuzzleTint RTRenderState::MuzzleFlashTintFor( AActor* viewactor ) const
{
    const FColorCVarRef* pick = &cvar::rt_mzlflsh_color;

    if( cvar::rt_mzlflsh_perweapon && viewactor && viewactor->player &&
        viewactor->player->ReadyWeapon )
    {
        if( const char* c = viewactor->player->ReadyWeapon->GetClass()->TypeName.GetChars() )
        {
            if( strstr( c, "PlasmaRifle" ) )
            {
                pick = &cvar::rt_mzlflsh_color_plasma;
            }
            else if( strstr( c, "BFG" ) )
            {
                pick = &cvar::rt_mzlflsh_color_bfg;
            }
            else if( strstr( c, "Unmaker" ) )
            {
                pick = &cvar::rt_mzlflsh_color_unmaker;
            }
            else if( strstr( c, "Chaingun" ) )
            {
                pick = &cvar::rt_mzlflsh_color_chaingun;
            }
        }
    }

    float scale = 1.f;
    if( cvar::rt_mzlflsh_luma_compensate )
    {
        const float lum = cvarcolor_luma( *pick );
        if( lum > 1.f )
        {
            scale = std::clamp( cvarcolor_luma( cvar::rt_mzlflsh_color ) / lum, 0.5f, 3.f );
        }
    }
    return { cvarcolor_to_rtcolor( *pick ), scale };
}

void RTRenderState::RT_AddMuzzleFlash( AActor*          viewactor,
                        int              extralight,
                        const RgFloat3D& basePosition,
                        const RgFloat3D& forward,
                        const RgFloat3D& up )
{
    // Soft fade-out after extralight ends so ReSTIR/RR history is not hard-cut
    // (peak intensity unchanged). Fade lives across frames via statics.
    static float    s_fade     = 0.f;
    static FVector3 s_lastPos  = {};
    static bool     s_havePos  = false;
    // Latched with the position, for the same reason: the fade outlives the shot, and
    // a weapon switch mid-fade would otherwise recolour a flash already in the air.
    static RgColor4DPacked32 s_color = 0;
    static float             s_intensityScale = 1.f;
    // Doom64-RT: rising-edge latch for the smoke spawn below, and the
    // maptime of the last spawn for the held-trigger repeat. -1 rather than
    // INT_MIN: maptime is non-negative, so -1 is an unreachable sentinel,
    // and `tic - INT_MIN` would be signed overflow.
    static bool              s_wasFiring    = false;
    static int               s_lastSpawnTic = -1;

    const bool wantFlash =
        extralight > 0 && cvar::rt_mzlflsh && viewactor && viewactor->Sector;

    // STAGE A0. Logged even when the flash does NOT fire, and rate-limited,
    // because the most important thing this can tell us is that extralight
    // never rises at all -- in which case nothing downstream is at fault and
    // the trigger has to come from somewhere else. Without this line, "no
    // smoke" and "no shot detected" look identical in the log.
    if( cvar::rt_smoke_debug )
    {
        static int s_a0 = 0;
        static int s_lastExtra = -999;
        if( extralight != s_lastExtra || ( s_a0++ % 105 ) == 0 )
        {
            s_lastExtra = extralight;
            Printf( "rt_smoke A0/gate: extralight=%d mzlflsh=%d actor=%d sector=%d "
                    "-> wantFlash=%d\n",
                    extralight,
                    int{ cvar::rt_mzlflsh },
                    viewactor ? 1 : 0,
                    ( viewactor && viewactor->Sector ) ? 1 : 0,
                    wantFlash ? 1 : 0 );
        }
    }

    if( wantFlash )
    {
        s_fade = 1.f;
    }
    else
    {
        // Re-arm the smoke edge as soon as the shot stops, before any of
        // the early returns below: the fade outlives extralight, so
        // clearing this at the bottom of the function would miss a second
        // shot fired during the first one's fade.
        s_wasFiring = false;

        const float fadeTics = std::max( 0.f, float( cvar::rt_mzlflsh_fade ) );
        if( fadeTics <= 0.f || s_fade <= 0.f || !s_havePos )
        {
            s_fade    = 0.f;
            s_havePos = false;
            return;
        }
        s_fade -= 1.f / fadeTics;
        if( s_fade <= 0.f )
        {
            s_fade    = 0.f;
            s_havePos = false;
            return;
        }
    }

    FVector3 pos;
    if( wantFlash )
    {
        auto desiredPos = gzvec3( basePosition );
        {
            desiredPos += gzvec3( up ) * cvar::rt_mzlflsh_u;
            desiredPos += gzvec3( forward ) * cvar::rt_mzlflsh_f;
        }

        {
            // metric to game units
            auto units_desiredPos   = DVector3{ desiredPos } / double{ ONEGAMEUNIT_IN_METERS };
            auto units_basePosition = gzvec3d( basePosition ) / double{ ONEGAMEUNIT_IN_METERS };

            auto dir = units_desiredPos - units_basePosition;
            auto len = dir.Length();

            if( len > 0.01 )
            {
                dir /= len;

                float hitT = 1.0f;

                FTraceResults trace;
                if( Trace( units_basePosition,
                           viewactor->Sector,
                           dir,
                           len,
                           0,
                           0,
                           viewactor,
                           trace,
                           TRACE_NoSky ) )
                {
                    if( trace.HitType != TRACE_HitNone )
                    {
                        hitT = float( ( trace.HitPos - units_basePosition ).Length() / len );
                        // hit point must be between base and desired positions
                        assert( hitT >= 0 && hitT <= 1 );
                    }
                }

                hitT *= std::clamp( float( cvar::rt_mzlflsh_offset ), 0.0f, 1.0f );

                // lerp
                pos = gzvec3( basePosition ) + hitT * ( desiredPos - gzvec3( basePosition ) );
            }
            else
            {
                pos = gzvec3( basePosition );
            }
        }

        s_lastPos = pos;
        s_havePos = true;
        const MuzzleTint tint = MuzzleFlashTintFor( viewactor );
        s_color                = tint.color;
        s_intensityScale       = tint.intensityScale;

        // Doom64-RT: localised smoke, born HERE rather than from the gun
        // anchor, so the puff and the flash that lights it are the same
        // point by construction -- `pos` has already been traced back out
        // of any wall the desired offset would have put the flash inside.
        //
        // On the RISING EDGE, plus a repeat while the trigger is held.
        //
        // Not per frame: extralight stays raised for several tics of one
        // shot, and spawning every frame would empty the budget into one
        // trigger pull. But not the edge alone either -- extralight is
        // raised by the weapon's Flash state (A_Light1/2) and cleared by
        // A_Light0, and a fast weapon re-enters its flash before A_Light0
        // runs, so on the chaingun and the plasma rifle it never returns to
        // 0 and the edge would fire once for an entire burst.
        const int tic    = primaryLevel ? primaryLevel->maptime : 0;
        const int repeat = std::max( 0, int{ cvar::rt_smoke_repeat } );

        if( cvar::rt_smoke_debug )
        {
            // The READY WEAPON by name. Several rounds of lab captures were taken
            // believing a rocket launcher was equipped when it was in fact the
            // pistol, and nothing in any log said otherwise -- the ammo counter
            // in the corner was the only clue, and it took a user to spot it.
            // The profile that shapes the smoke is chosen from this name, so it
            // belongs in the trace.
            Printf( "rt_smoke A/trigger: weapon=%s extralight=%d wasFiring=%d tic=%d "
                    "lastSpawn=%d flashpos %.2f %.2f %.2f\n",
                    ( viewactor && viewactor->player && viewactor->player->ReadyWeapon )
                        ? viewactor->player->ReadyWeapon->GetClass()->TypeName.GetChars()
                        : "<none>",
                    extralight,
                    s_wasFiring ? 1 : 0,
                    tic,
                    s_lastSpawnTic,
                    pos.X,
                    pos.Y,
                    pos.Z );
        }
        // A NEGATIVE delta is the sentinel above, or a level change resetting
        // maptime to 0 while this static still holds the last map's value.
        // Treating it as "not due" would leave the repeat dead for the whole
        // of the next map, so it counts as due instead.
        const int  since = tic - s_lastSpawnTic;
        const bool due   = repeat > 0 && ( since < 0 || since >= repeat );

        if( !s_wasFiring || due )
        {
            s_lastSpawnTic = tic;
            // Inheriting the player's velocity is what keeps the smoke from
            // reading as glued to the camera when you strafe. Actor
            // velocities are map units per TIC, so convert to metres/second.
            FVector3 inherit{ 0, 0, 0 };
            if( viewactor )
            {
                inherit = FVector3{ float( viewactor->Vel.X ),
                                    float( viewactor->Vel.Y ),
                                    float( viewactor->Vel.Z ) } *
                          ( ONEGAMEUNIT_IN_METERS * float( TICRATE ) );
            }
            // THE FLASH'S HEIGHT IS NOT THE SMOKE'S. Both are born at `pos` so
            // the light and the smoke it lights are one point by construction,
            // and that is still right -- but `pos` is placed for LIGHTING.
            // rt_mzlflsh_u is -0.9 m, keeping the flash low so it washes the
            // room instead of blowing out the gun sprite, and rt_smoke_offset
            // then births the puff most of a metre BELOW eye level, around knee
            // height. Rising ~1 m over its life, it climbs up THROUGH the view
            // from underneath -- which reads as smoke appearing above you
            // rather than coming off a barrel, and is why backing away from it
            // looks better: from further off the same column reads as a column.
            //
            // So the smoke gets a vertical correction of its own. The flash
            // does not move, and the puff is still on the traced segment, so it
            // cannot be pushed inside the wall the trace just pulled it out of.
            const FVector3 smokePos =
                pos + gzvec3( up ) * float{ cvar::rt_smoke_muzzle_u };

            RT_SpawnSmokePuffs( gzvec3( basePosition ),
                                smokePos,
                                gzvec3( forward ),
                                inherit,
                                RT_SmokeProfileFor( viewactor ) );
        }
        s_wasFiring = true;
    }
    else
    {
        pos = s_lastPos;
    }

    auto sph = RgLightSphericalEXT{
        .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
        .pNext     = nullptr,
        .color     = s_color,
        .intensity = cvar::rt_mzlflsh_intensity * s_fade * s_intensityScale,
        .position  = { pos.X, pos.Y, pos.Z },
        .radius    = cvar::rt_mzlflsh_radius,
    };

    auto light = RgLightInfo{
        .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
        .pNext        = &sph,
        .uniqueID     = MuzzleFlashLightId,
        .isExportable = false,
    };

    RgResult r = rt.rgUploadLight( &light );
    RG_CHECK( r );
}

// Passive glow from the ready weapon's own lit element — the plasma rifle's electric
// core. See the rt_gunglow cvar block for why this cannot be a lightIntensity on the
// sprite's texture (it attaches the light to the rasterized quad it is meant to
// light, and that gun was the only one carrying such a light) nor an emissiveMult
// (a view weapon's emission is a screen-space overlay and illuminates nothing).
//
// Not uploading the light is how it turns off — same contract as the muzzle flash:
// RTGL1 tracks a light by uniqueID per frame, so an absent upload is an absent light.
void RTRenderState::RT_AddWeaponGlow( AActor*          camera,
                       const RgFloat3D& basePosition,
                       const RgFloat3D& forward,
                       const RgFloat3D& up )
{
    if( !cvar::rt_gunglow || !camera || !camera->player )
    {
        return;
    }
    AActor* ready = camera->player->ReadyWeapon;
    if( !ready )
    {
        return;
    }

    // Substring, not equality: the IWAD class is PlasmaRifle and Retribution
    // REPLACES it with 64PlasmaRifle. Both must light.
    const char* cls = ready->GetClass()->TypeName.GetChars();
    if( !cls || !strstr( cls, "PlasmaRifle" ) )
    {
        return;
    }

    FVector3 pos;

    if( m_haveGunAnchor )
    {
        // The gun's own quad, pulled a fraction of the way back toward the eye so the
        // light sits just IN FRONT of the sprite's visible face rather than inside or
        // behind it. Behind it lights nothing you can see — the quad faces the camera.
        //
        // m_gunAnchorView is view space with the eye at the origin, so "toward the
        // eye" is simply a scale down the same vector. Then camera-to-world, the same
        // matrix the quad itself was uploaded with, so the light cannot drift off the
        // gun no matter where you look.
        const float pull = 1.f - std::clamp( float( cvar::rt_gunglow_pullback ), 0.f, 0.95f );
        const FVector3 v = m_gunAnchorView * pull;

        const RgFloat4D w =
            ApplyMat44ToVec4( m_mainCameraView_Inverse, RgFloat4D{ v.X, v.Y, v.Z, 1.0f } );
        const RgFloat3D world = FromHomogeneous( w );
        pos                   = gzvec3( world );

        // Fine trim, in view axes, on top of the anchor.
        pos += gzvec3( up ) * float( cvar::rt_gunglow_u );
        pos += gzvec3( forward ) * float( cvar::rt_gunglow_f );
    }
    else
    {
        // No plasma quad uploaded yet this session — fall back to the view-relative
        // placement, and follow the psprite's bob by hand.
        pos = gzvec3( basePosition );
        pos += gzvec3( up ) * float( cvar::rt_gunglow_u );
        pos += gzvec3( forward ) * float( cvar::rt_gunglow_f );

        if( const DPSprite* psp = camera->player->FindPSprite( PSP_WEAPON ) )
        {
            const float bob = float( cvar::rt_gunglow_bob );
            if( bob > 0.f )
            {
                const auto right = gzvec3( forward ) ^ gzvec3( up );
                pos += right * ( float( psp->x ) * bob );
                pos -= gzvec3( up ) * ( float( psp->y - WEAPONTOP ) * bob );
            }
        }
    }

    // Electricity, not a bulb: two detuned sines so the beat never settles into a
    // visible loop. Deliberately smooth rather than per-frame random — the denoiser
    // resolves a moving light far better than a stuttering one, and white-noise
    // flicker on a 1-spp path tracer just reads as sparkle.
    float intensity = float( cvar::rt_gunglow_intensity );
    {
        const float depth = std::clamp( float( cvar::rt_gunglow_flicker ), 0.f, 1.f );
        if( depth > 0.f )
        {
            const float t = float( level.totaltime ) / float( TICRATE );
            const float n = 0.6f * std::sin( t * 37.0f ) + 0.4f * std::sin( t * 23.3f );
            intensity *= 1.f + depth * n;
        }
        // Kick on discharge. extralight is what the Flash state's A_Light1 raises,
        // so this tracks the actual shot rather than guessing from the frame.
        if( camera->player->extralight > 0 )
        {
            intensity *= std::max( 0.f, float( cvar::rt_gunglow_fire_boost ) );
        }
    }

    auto sph = RgLightSphericalEXT{
        .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
        .pNext     = nullptr,
        .color     = cvarcolor_to_rtcolor( cvar::rt_gunglow_color ),
        .intensity = std::max( 0.f, intensity ),
        .position  = { pos.X, pos.Y, pos.Z },
        .radius    = cvar::rt_gunglow_radius,
    };

    auto light = RgLightInfo{
        .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
        .pNext        = &sph,
        .uniqueID     = GunGlowLightId,
        .isExportable = false,
    };

    RgResult r = rt.rgUploadLight( &light );
    RG_CHECK( r );
}

