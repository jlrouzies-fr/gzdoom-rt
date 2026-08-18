// Doom64-RT: Options -> Quality presets.
//
// WHY A PRESET RATHER THAN JUST SLIDERS. The values below were not chosen; they
// were settled in play over many sessions, and the reasons are written into
// rt_cvars.inc and tools/d64rt-pins.cfg next to each one. A player who wants
// more frames should not have to rediscover which twenty of the seven hundred
// rt_* cvars cost frame time -- and should not be able to wreck the look by
// guessing. HIGH IS EXACTLY WHAT THE GAME SHIPPED WITH, so picking a preset can
// lower quality or raise it, but never silently redefines the intended look.
//
// WHAT IS IN HERE AND WHY, from docs/performance.md:
//   Persistence  -- the only cost centre that grows with how long you have been
//                   playing. Casings never despawn (their Death states are -1),
//                   spark debris and scorch decals are on infinite lives, and
//                   40 s of held trigger measured 490 -> 2009 primitives with
//                   the effect walk going 0.16 -> 1.23 ms and no plateau. This
//                   group is the one that answers "it gets heavy".
//   Effects      -- particle budgets: dust, smoke, arcs.
//   Sprite geom  -- the extra primitives every actor emits: shadow proxy planes
//                   and the contact-AO blob, which has no count cap at all.
//   Fixture      -- analytic lights inferred from lamp textures. Cheap on the
//     lights        CPU (those walks measured a flat 0.26 ms whatever the count)
//                   but each costs a ReSTIR reservoir slot, so it is a GPU knob.
//   Ray budget   -- the existing GPU knobs the menu already exposed.
//
// EVERY OWNED CVAR MUST BE UNPINNED. A pin in tools/d64rt-pins.cfg runs at
// launch and overrides both the compiled default AND anything a preset set, so
// a pinned cvar here would make the preset look broken in exactly the way that
// has cost this project two tuning passes already. tools/check_pins.py fails if
// one comes back.
//
// Cvars are resolved BY NAME rather than through the rt_cvars.inc externs,
// because three of them (d64_dropcasings, rt_gore_max, rt_gore_life) are
// CVARINFO cvars from the mod pk3s and have no C++ symbol at all. A name that
// does not resolve is skipped, so a preset still works with a pk3 unloaded.

#include "rt_internal.h"

#include "c_cvars.h"
#include "c_dispatch.h"
#include "printf.h"

namespace
{

enum RtQualityLevel
{
    RTQ_CUSTOM      = 0,
    RTQ_ULTRA       = 1,
    RTQ_HIGH        = 2, // ships; equals every value the game was tuned at
    RTQ_BALANCED    = 3,
    RTQ_PERFORMANCE = 4,
};

struct QualityEntry
{
    const char* name;
    float       ultra;
    float       high; // MUST equal the shipped value; see the note above
    float       balanced;
    float       performance;
};

// clang-format off
const QualityEntry g_quality[] = {
    // --- Persistence: the play-time cost centre -----------------------------
    // Performance is the only level that stops casings entirely. Balanced keeps
    // them and halves what piles up beside them instead, because the casings are
    // the loudest single contributor (201 of the 345 primitives a 13 s burst
    // added) and turning them off is a visible change to how the game plays.
    { "d64_dropcasings",         1,      1,      1,      0    },
    { "rt_spark_debris",         1,      1,      1,      0    },
    { "rt_spark_max",         2000,   2000,   1000,    400    },
    { "rt_arc_burn_max",        96,     96,     48,     16    },
    { "rt_gore_max",          1500,   1500,    600,    200    },
    // 0 = forever. Performance gives blood a 30 s life so a long level cannot
    // accumulate without bound; every other level keeps the authored behaviour.
    { "rt_gore_life",            0,      0,      0,     30    },

    // --- Effects ------------------------------------------------------------
    { "rt_dust_max",          1400,    900,    500,      0    },
    { "rt_smoke_budget",       120,    120,     80,     40    },
    { "rt_smoke_spp",            8,      8,      4,      2    },
    { "rt_arc_max",            160,    160,     96,     48    },
    { "rt_arc_glow_max",       100,    100,     64,     24    },

    // --- Per-sprite geometry ------------------------------------------------
    // The AO blob is one extra primitive, its own BLAS and its own TLAS instance
    // per thing with a floor under it, at rt_sprite_ao_scope 0. The distance is
    // what bounds the count; the segment count makes each one cheaper.
    { "rt_sprite_ao_segments",  32,     32,     20,     12    },
    { "rt_sprite_ao_dist",      30,     30,     20,     12    },
    { "rt_sprite_shadow_planes", 4,      4,      3,      2    },
    { "rt_sprite_shadow_dist",  40,     40,     28,     16    },

    // --- Inferred fixture lights -------------------------------------------
    { "rt_ceiling_edge_max",  1024,   1024,    512,    256    },
    { "rt_solo_lamp_max",      384,    384,    192,     96    },
    { "rt_faux_lamp_max",      256,    256,    128,     64    },
    { "rt_wall_strip_max",     128,    128,     96,     48    },

    // --- Ray budget (the knobs the menu already had) -------------------------
    // Ultra is the only level that raises anything, and rt_restir_initial is
    // where it spends first: it traces no rays at all, which is why rt_cvars.inc
    // says "Raise before reaching for rt_spp_direct".
    { "rt_shadow_samples",       2,      1,      1,      1    },
    { "rt_spp_direct",           2,      1,      1,      1    },
    { "rt_spp_indirect",         1,      1,      1,      1    },
    { "rt_restir_initial",      48,     32,     24,     12    },
    { "rt_restir_spatial",       8,      8,      6,      4    },
    { "rt_shadowrays",           4,      4,      2,      1    },
    { "rt_reflrefr_depth",       8,      8,      4,      2    },
};
// clang-format on

float ValueFor( const QualityEntry& e, int level )
{
    switch( level )
    {
        case RTQ_ULTRA: return e.ultra;
        case RTQ_HIGH: return e.high;
        case RTQ_BALANCED: return e.balanced;
        case RTQ_PERFORMANCE: return e.performance;
        default: return e.high;
    }
}

void ApplyQualityPreset( int level, bool verbose )
{
    if( level <= RTQ_CUSTOM || level > RTQ_PERFORMANCE )
    {
        return;
    }

    int applied = 0;
    int missing = 0;

    for( const QualityEntry& e : g_quality )
    {
        FBaseCVar* v = FindCVar( e.name, nullptr );
        if( v == nullptr )
        {
            // A CVARINFO cvar whose pk3 is not loaded. Not an error: the base
            // game and the Unseen Evil overlay do not ship the same set.
            missing++;
            continue;
        }

        const float f = ValueFor( e, level );

        UCVarValue val;
        switch( v->GetRealType() )
        {
            case CVAR_Bool:
                val.Bool = ( f != 0.0f );
                v->SetGenericRep( val, CVAR_Bool );
                break;
            case CVAR_Float:
                val.Float = f;
                v->SetGenericRep( val, CVAR_Float );
                break;
            default:
                val.Int = int( f );
                v->SetGenericRep( val, CVAR_Int );
                break;
        }
        applied++;
    }

    if( verbose )
    {
        static const char* const kNames[] = {
            "Custom", "Ultra", "High", "Balanced", "Performance"
        };
        Printf( "RT quality preset: %s -- %d cvar(s) set, %d not present\n",
                kNames[ level ],
                applied,
                missing );
    }
}

} // namespace

// 2 = High = exactly the values the game shipped with, so a fresh config and a
// config that has never opened this menu render identically.
CUSTOM_CVAR( Int, rt_quality_preset, RTQ_HIGH, CVAR_ARCHIVE | CVAR_GLOBALCONFIG )
{
    if( self < RTQ_CUSTOM || self > RTQ_PERFORMANCE )
    {
        self = RTQ_HIGH;
        return;
    }
    ApplyQualityPreset( self, true );
}

void RT_ApplyQualityPresetOnce()
{
    // WHY THIS EXISTS, and why the CUSTOM_CVAR handler below is not enough.
    //
    // Three of the owned cvars -- d64_dropcasings, rt_gore_max, rt_gore_life --
    // are CVARINFO cvars declared by the mod pk3s, and those are created when
    // the WAD is loaded, which is AFTER the config is read. So at the moment
    // gzdoom applies rt_quality_preset from the ini, FindCVar cannot see them
    // and they are silently skipped. On a fresh config the handler does not run
    // at all, because taking a default is not a set.
    //
    // That is not theoretical. d64_dropcasings defaults to 0 in the WAD's
    // CVARINFO and the launcher pin was forcing it to 1; unpinning it in favour
    // of the preset turned shell casings off for everybody until this ran.
    //
    // So: apply once more when the level exists, which is the first moment every
    // cvar the preset owns is guaranteed to be registered.
    static bool s_done = false;
    if( s_done || !primaryLevel )
    {
        return;
    }
    s_done = true;

    // Verbose on purpose. This is the one apply nobody asked for, so the log has
    // to show it happened and that every name resolved -- "0 not present" is the
    // liveness check for the ordering problem described above.
    ApplyQualityPreset( rt_quality_preset, true );
}

// Re-apply without going through the archived cvar, for a launcher or an A/B arm
// that wants to state the preset rather than inherit whatever was saved.
CCMD( rt_quality_apply )
{
    if( argv.argc() < 2 )
    {
        Printf( "rt_quality_apply <1=Ultra|2=High|3=Balanced|4=Performance>\n" );
        return;
    }
    ApplyQualityPreset( atoi( argv[ 1 ] ), true );
}

// What the preset WOULD set, beside what is actually live. The honest way to
// catch a pin having quietly taken one of these back.
CCMD( rt_quality_show )
{
    const int level = rt_quality_preset;
    for( const QualityEntry& e : g_quality )
    {
        FBaseCVar* v = FindCVar( e.name, nullptr );
        Printf( "  %-26s preset=%-8g live=%s\n",
                e.name,
                ValueFor( e, level ),
                v ? v->GetHumanString() : "(not present)" );
    }
}
