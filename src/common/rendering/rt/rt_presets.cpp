// Per-map lighting presets: moon aim, cloud deck, albedo tint and fog, keyed on
// the map name and re-applied on EVERY level load.
//
// They are engine-side and not in the sky pk3 because ZScript cannot set these
// cvars: _CVar.SetFloat throws "Attempt to change CVAR outside of menu code" for
// anything without CVAR_MOD, and every RT_CVAR is CVAR_GLOBALCONFIG|CVAR_ARCHIVE.
//
// Split out of rt_main.cpp. Behaviour unchanged; this is a move.

#include "rt_internal.h"

// fileSystem.GetMaxIwadNum(): which container a map lump came from, used by the
// IWAD guard in RT_OnLevelLoadPresets below.
#include "filesystem.h"

// The shared internals (RG_CHECK, ONEGAMEUNIT_IN_METERS, RT_SectorHue, the
// light-ID bases) come in unqualified, exactly as when this code lived inside
// rt_main.cpp's anonymous namespace.
using namespace rtx;

// Per-map moon aim.
//
// One moon, many maps, and each map's windows face wherever its author pointed
// them -- so a single global bearing lights one level well and rakes across the
// next one at a useless angle. This is that table.
//
// It is engine-side and not in the sky pk3 because ZScript cannot set these
// cvars: _CVar.SetFloat throws "Attempt to change CVAR outside of menu code" for
// anything without CVAR_MOD, and every RT_CVAR is CVAR_GLOBALCONFIG|CVAR_ARCHIVE.
//
// A NEGATIVE azimuth, altitude or intensity means "leave the launcher's value
// alone". Most entries only want to turn the moon, not rebalance the level's
// brightness; the VOIDSKY rows below want neither and only turn the disc off.
// Restating a value a row does not care about is how a table like this goes
// stale -- the copy stops tracking the launcher and nobody notices.
//
// `disc` is the MOONDISC quad, i.e. whether there is a moon to LOOK at, and it
// is separate from rt_sun, the light. The two are separable on purpose: the
// VOIDSKY maps want their windows to keep letting light in while having nothing
// visible in the sky to have emitted it.
//
// `sky` is rt_sky, the multiplier on the DOME's own radiance -- RT samples the
// rasterised sky cubemap on ray miss, so this is how much light the sky itself
// pours through an opening, as opposed to rt_sun which is the moon.
//
// It is here because the domes are no longer comparable. The launcher's rt_sky
// 25 was set when every map had the same near-black starfield; now that each map
// gets the sky its author drew, the mean LINEAR radiance of those domes spans
// three orders of magnitude (tools/gen_d64_skies.py's families, measured):
//
//     MOONSKY starfield   1.0x     <- what 25 was tuned against
//     SKYMTNA             12x
//     SKYCLD*/SKYMTNB/C   30-45x
//     VOIDSKY            186x
//     FRSKYNRM           953x
//
// So a fire map at the global 25 receives roughly a THOUSAND times the sky light
// a starfield map does. That is the reason the fire maps need no replacement
// light at all now the moon is off them: the burning sky already is one, and the
// entry below turns it DOWN, not up. The values are set for parity with the
// cloud families rather than measured in game -- start there and look.
//
// Parity, not preservation: the point is that no two maps sit a thousand times
// apart, NOT that the new skies deliver what the old starfield did. A burning
// sky should light a level more than a starfield; it should not light it 953x
// more.
//
// NOTE this writes rt_moon_geo at level load, after the command line is parsed,
// so on a listed map it overrides a +rt_moon_geo pin -- the same trap
// RT_CLOUD_PRESETS documents. rt_moon_presets 0 turns the whole table off.
//
// To add one: play the map, aim it with `moon <az> [alt]`, then type `moon` and
// paste the row it prints. Maps with no entry fall back to the launcher's
// rt_sun_a/b and rt_moon_geo, captured on the first level load (see
// g_moon_base_* below) so that a preset on one map cannot leak into the next.
namespace
{
struct MoonPreset
{
    const char* map;
    float       azimuth;
    float       altitude;
    float       intensity; // < 0: keep whatever the launcher pinned
    bool        disc;      // draw the MOONDISC quad at all
    float       sky;       // rt_sky, the dome's own radiance. < 0: keep
    const char* note;
};

constexpr MoonPreset RT_MOON_PRESETS[] = {
    // The three VOIDSKY maps -- 25, 26, 31 -- and they are ONE FAMILY, not three
    // decisions. Verified from the WAD rather than by eye, because the sky here
    // is a skybox ROOM and every map's MAPINFO sky1 says `ISUCK`: the VOIDSKY
    // texture appears in exactly these three TEXTMAPs of the main campaign, and
    // all three carry the same authored fog, `fade "00 56 56"` at
    // `fogdensity 200`. Identical inputs, so identical rows.
    //
    // The DISC goes because their skybox room is a plain box with a flat dark
    // teal on every surface -- no starfield, no cloud, no horizon, nothing that
    // implies a sky at all -- so a moon hanging in it is the one object on
    // screen with no reason to be there.
    //
    // The moon's LIGHT goes too (intensity 0) because all three are fogged
    // (RT_FOG_PRESETS) and a directional light is the one thing that wrecks a
    // fogged level. It rakes the froxel volume from a single bearing, so the fog
    // reads as a lit slab with a hard edge to it instead of as the even medium
    // the map was authored around; and none of these rooms have an opening the
    // moon could honestly be arriving through in the first place, so under
    // rt_sun_require_sky most of what it delivered was leak anyway. The fog's
    // light comes from the level's own lamps and lava (rt_fog_illum), which is
    // where it should have come from.
    //
    // The cost of taking the light away is real and was accepted knowingly: on a
    // map with F_SKY1 openings, rooms lit through them flatten. Here the fog's
    // own in-scattered floor (rt_fog_ambient 1) is what fills that space, which
    // is why the two tables have to move together -- an intensity 0 row here
    // WITHOUT a fog row there would leave a map genuinely darker, with nothing
    // put back. That is the pairing to preserve if a fourth map is ever added.
    { "map25", -1.f, -1.f, 0.f, false, 6.f,
      "VOIDSKY, moon OFF entirely -- disc and light. Cat and Mouse; fogged, see "
      "RT_FOG_PRESETS. Aim INHERITED: negative azimuth/altitude means `keep the "
      "launcher's`, so this row says only the things it is for. Without that "
      "convention a row like this would have to restate an aim it does not care "
      "about, and the restated copy would go stale the moment the launcher's "
      "changed. sky 6 because VOIDSKY's flat teal is 186x the starfield's mean "
      "radiance -- at the global 25 the void would glow like a lightbox." },
    { "map26", -1.f, -1.f, 0.f, false, 6.f,
      "VOIDSKY, as MAP25. Hardcore -- the map the fog was built for." },
    { "map31", -1.f, -1.f, 0.f, false, 6.f,
      "VOIDSKY, as MAP25. The secret map, and the last of the three: same "
      "skybox, same `fade` 00 56 56 at fogdensity 200, so same row." },

    // The five fire-sky maps (FRSKYNRM on 22/24/28, FRSKYGRN on 23/32).
    //
    // The disc goes for the VOIDSKY reason and more so: the sky is a wall of
    // burning cloud and a cold moon in front of it is the brightest wrong thing
    // on screen. The moonLIGHT goes with it (intensity 0) because nothing about
    // a fire sky is directional -- the art is black at the top and bright at the
    // bottom, so the fire is a RING at the horizon, arriving from every azimuth
    // at once. Any bearing picked for a directional would be arbitrary and would
    // rake shadows in a direction nothing on screen justifies.
    //
    // Nothing replaces it, because nothing has to: the burning dome is already
    // the brightest sky in the game by three orders of magnitude, and RT samples
    // it on ray miss through the map's real F_SKY1 openings with real occlusion.
    // The usual objection -- the sky is not importance-sampled, so a bright
    // thing in it lights nothing at 1 spp -- is an argument about SMALL sources.
    // The moon disc is half a degree, about 1e-5 of the hemisphere. A fire sky
    // is the whole hemisphere, which is the case where un-importance-sampled
    // environment light works, because a cosine-weighted diffuse ray hits it
    // constantly.
    //
    // sky 1.2 / 2.7 is parity with the cloud families at the global 25, off the
    // measured mean radiances (953x and 410x the starfield). Both are starting
    // points to look at, not settled numbers.
    { "map22", -1.f, -1.f, 0.f, false, 1.2f,
      "Fire sky. Moon off entirely -- disc and light -- and the dome does the "
      "lighting." },
    { "map24", -1.f, -1.f, 0.f, false, 1.2f, "Fire sky, as MAP22." },
    { "map28", -1.f, -1.f, 0.f, false, 1.2f, "Fire sky, as MAP22." },
    { "map23", -1.f, -1.f, 0.f, false, 2.7f,
      "Green fire sky -- FRSKYGRN is 2.3x dimmer than FRSKYNRM, hence the higher "
      "multiplier for the same delivered light." },
    { "map32", -1.f, -1.f, 0.f, false, 2.7f, "Green fire sky, as MAP23." },

    { "map01", 180.f, 90.f, -1.f, true, -1.f,
      "`moon 180 180` -- straight overhead, light pouring vertically. Chosen to "
      "match how the original game reads, and the vertical fall is the point: at "
      "altitude 90 the direction code clamps theta to 0, so the shafts come "
      "straight down through MAP01's roof slots the way they do in vanilla. "
      "This entry used to carry a caveat -- that the DISC could not follow the "
      "light above the sky dome's 60 degrees and the moon would not be where its "
      "own shafts came from. That was true of the painted moon and is not true "
      "now: the disc is geometry and draws over the dome's cap." },
    { "map18", 10.f, 70.f, -1.f, true, -1.f,
      "`moon 10 70` -- almost due north, high. Settled in play. Nothing to note "
      "about the altitude: 70 is above the sky dome's 60 degrees, which used to "
      "be a hard ceiling on where the DISC could go, but the moon is geometry "
      "now (RT_DrawSkyQuad) and draws over the dome and its cap alike, so it "
      "sits at its own light's bearing like any other." },
    { "map13", 90.f, 25.f, -1.f, true, -1.f,
      "Due north. Settled in play. The painted shafts this replaced implied two "
      "different suns -- the west hall's fans want light travelling +x, the north "
      "colonnade's want -y -- and 135 was the geometric compromise between them. "
      "90 reads better than the compromise did: it rakes hard through the north "
      "colonnade and still catches the west windows obliquely." },
    { "map02", 120.f, 40.f, -1.f, true, -1.f,
      "`moon 120 40` -- aimed in play and settled there (2026-08-22). Aim only: "
      "intensity and sky stay whatever the launcher pinned, so this row says the "
      "one thing it is for and cannot go stale against a later tuning pass." },
};

// The launcher's aim, captured once before any preset overwrites it, so a map
// with no entry gets the global default back instead of inheriting whatever the
// last map with an entry set. Without this the table would be sticky in one
// direction and the fallback would silently become "the last preset visited".
bool  g_moon_base_set  = false;
float g_moon_base_a    = 0.f;
float g_moon_base_b    = 0.f;
float g_moon_base_i    = 0.f;
bool  g_moon_base_disc = false;
float g_moon_base_sky  = 0.f;

const MoonPreset* RT_FindMoonPreset( const char* mapname )
{
    if( !mapname || mapname[ 0 ] == '\0' )
    {
        return nullptr;
    }
    for( const auto& p : RT_MOON_PRESETS )
    {
        if( stricmp( mapname, p.map ) == 0 )
        {
            return &p;
        }
    }
    return nullptr;
}

//-----------------------------------------------------------------------------
//
// Per-map cloud aim, the same shape as RT_MOON_PRESETS above and for the same
// reason: one global setting cannot serve 32 maps that were authored with
// different skies.
//
// OPT-IN. The deck is off unless a map has an entry here, because a cloud layer
// is not neutral scenery -- it changes what the moon does. MAP01's moon is
// straight overhead and its light falls vertically through roof slots; a deck
// over that would fight the one effect the map is built around.
//
// `tint` owns the hue outright (the slice art is near-achromatic on purpose --
// see rt_clouds_tint), so a map with a dark purple skybox gets purple clouds AND
// purple moonlight through them, rather than the default cool blue fighting the
// backdrop.
//
// Authoring loop: aim it in game, then type `clouds` and paste the row it prints.
//
//-----------------------------------------------------------------------------
// FColorCVarRef exposes no assignment operator -- it is commented out in
// c_cvars.h -- so colour cvars are written through FBaseCVar::SetGenericRep, the
// same way the engine sets any other (see d_netinfo.cpp's player colour).
// Templated only so it does not have to name the ref type, which differs between
// the defining TU and an EXTERN_CVAR one.
template< class TColorCVar >
void RT_SetColorCVar( TColorCVar& c, uint32_t rgb )
{
    UCVarValue v;
    v.Int = int( rgb );
    c->SetGenericRep( v, CVAR_Int );
}

struct CloudPreset
{
    const char* map;
    bool        clouds;   // deck on at all
    uint32_t    tint;     // 0 = keep the global rt_clouds_tint
    float       alpha;    // < 0 = keep
    float       wind;     // < 0 = keep
    int         shells;   // <= 0 = keep. Cost AND volume: see rt_clouds_shells
    float       thick;    // < 0 = keep
    float       transmit; // < 0 = keep. What a FULLY covered patch passes
    const char* note;
};

constexpr CloudPreset RT_CLOUD_PRESETS[] = {
    { "map01", false, 0, -1.f, -1.f, -1, -1.f, -1.f,
      "Explicitly OFF, and listed rather than left to the default so the decision "
      "is recorded. MAP01's moon preset is altitude 90 -- straight overhead, light "
      "falling vertically through the roof slots, which is the whole look of the "
      "map (see RT_MOON_PRESETS). A cloud deck sits directly across that path and "
      "would attenuate exactly the shafts the map is built around." },
    { "map11", true, 0x9AA6C8, 0.9f, 0.014f, -1, -1.f, -1.f,
      "The storm. This is the map the whole deck exists for: it carries the "
      "MAPINFO `lightning` keyword, and it is authored WITH clouds (a CLOUDPRP "
      "skybox room, ACS script 670) that RT never draws. Slightly desaturated "
      "cool grey rather than the default blue -- the level is lit green-grey and "
      "a strongly blue sky reads as a separate scene behind it." },
    { "map14", true, 0x8C7AB4, 0.85f, 0.010f, -1, -1.f, -1.f,
      "Purple. Thinner and slower than MAP11 -- this is weather, not a storm, so "
      "the deck should sit still enough to read as a backdrop." },
    { "map10", true, 0xC28153, 0.85f, 0.010f, -1, -1.f, -1.f,
      "Burnt orange. MAP10's skybox room is a CLOUDBRN overcast over a MOUNTB "
      "ridge, and the tint is that flat's own hue lifted to the luminance the "
      "other presets sit at -- the slice art is achromatic, so the tint is the "
      "cloud colour outright. Wind 0.010 because ACS script 670 scrolls the "
      "authored ceiling at 3, the same rate MAP14 does; MAP11's storm is 4." },
    { "map16", true, 0xC28153, 0.85f, 0.010f, -1, -1.f, -1.f,
      "The other CLOUDBRN map, same room and same scroll rate as MAP10, so the "
      "same orange. Kept as its own row rather than shared, because the table is "
      "keyed by map and a shared row would hide which maps are actually on." },
    { "map12", true, 0x6135A0, 1.0f, 0.010f, 8, 1.0f, 0.45f,
      "MAXED, and the one map that leans on the deck as a light rather than as "
      "scenery. Full alpha, all 8 shells, double the global thickness: the sky "
      "is solid cloud with no clear patches to speak of.\n"
      "  The tint matters more here than anywhere else because it is doing two "
      "jobs: the colour of the picture AND the colour of every bit of moonlight "
      "that reaches the level, since under total cover that is the only light "
      "the outdoors gets. The slice art is achromatic, so a cloud texel is "
      "LIT x tint x the shell ramp and the darkest is SHADOW x tint x that ramp "
      "-- both ends of the rendered range are arithmetic, which is what makes "
      "this tunable on paper at all.\n"
      "  Settled by sweeping, not by matching. It was 8660C0 first, which is the "
      "console game's own sky pixel for pixel (bright #7253AC against its "
      "#745BAD, dark #110D1E against its #100F1F -- screen/doom64clouds.png); "
      "that measured right and read WEAK in motion. Then 9C55DC and a run up "
      "into the neon range as far as 9A28FF, and the answer came back down: "
      "6135A0 is 9A28FF faded (saturation 0.84 -> 0.67) and deepened (Y 80 -> "
      "70). Bright cloud #522E8F, dark #0D0719, moonlight #F997FF.\n"
      "  NOTE it delivers about a THIRD less light than 9C55DC did -- moon "
      "intensity 11.1 against 17.4 at rt_sun_intensity 90 -- because tint "
      "luminance scales the moonlight as well as the picture and this tint is "
      "both darker and more saturated. Deliberately not compensated here: if "
      "MAP12's outdoors wants the light back, raise this row's transmit from "
      "0.45 to 0.60, which restores it without touching the colour. Sweep "
      "either without rebuilding: tools\\ab-clouds.cmd <arm> <map> <tint>.\n"
      "  transmit 0.45 against the global 0.22 is what makes that survivable. "
      "The deck's transmittance is now a slab (see hw_skyportal.cpp), so this is "
      "literally what a fully covered patch passes: 0.45 of the tint, which "
      "works out at about a sixth of the moon's luminance arriving strongly "
      "violet. At the global 0.22 a deck this thick would be a lid." },
    { "map30", true, 0x6135A0, 0.85f, 0.010f, -1, -1.f, -1.f,
      "Same purple as MAP12 -- same CLOUDPRP room, same MOUNTC ridge, same "
      "scroll rate -- but not MAP12's maxed shape: this map wants the deck as "
      "scenery, not as its light source." },
    { "map09", true, 0xE85062, 0.85f, 0.014f, -1, -1.f, -1.f,
      "Red-pink. The CLOUDPNK rooms are a lurid magenta-crimson (the flat's mean "
      "is 740317); this is that hue pushed off magenta towards red, which is "
      "what the level's own lighting sits under. Wind 0.014 rather than the 0.010 "
      "its neighbours get: MAP09's ACS scrolls the authored ceiling at 4, the "
      "storm's rate, not 3." },
    { "map15", true, 0xE85062, 0.85f, 0.010f, -1, -1.f, -1.f, "CLOUDPNK, as MAP09." },
    { "map18", true, 0xE85062, 0.85f, 0.010f, -1, -1.f, -1.f, "CLOUDPNK, as MAP09." },
    { "map19", true, 0xE85062, 0.85f, 0.010f, -1, -1.f, -1.f, "CLOUDPNK, as MAP09." },
    { "map20", true, 0xE85062, 0.85f, 0.010f, -1, -1.f, -1.f, "CLOUDPNK, as MAP09." },
    { "map17", true, 0xA67454, 0.85f, 0.010f, -1, -1.f, -1.f,
      "Brown. Same CLOUDBRN flat as MAP10/16 but deliberately duller and dimmer "
      "than their C28153 -- those two are a lit orange overcast over a ridge, "
      "these two are a flat brown sky with no ridge at all in the room, and the "
      "same orange over them reads as a sunset the level does not have." },
    { "map27", true, 0xA67454, 0.85f, 0.010f, -1, -1.f, -1.f, "CLOUDBRN, as MAP17." },
};

bool  g_cloud_base_set   = false;
bool  g_cloud_base_on    = false;
uint32_t g_cloud_base_tint = 0;
float g_cloud_base_alpha = 0.f;
float g_cloud_base_wind  = 0.f;
int   g_cloud_base_shells   = 0;
float g_cloud_base_thick    = 0.f;
float g_cloud_base_transmit = 0.f;

const CloudPreset* RT_FindCloudPreset( const char* mapname )
{
    if( !mapname || mapname[ 0 ] == '\0' )
    {
        return nullptr;
    }
    for( const auto& p : RT_CLOUD_PRESETS )
    {
        if( stricmp( mapname, p.map ) == 0 )
        {
            return &p;
        }
    }
    return nullptr;
}

void RT_ApplyCloudPreset( const char* mapname )
{
    // Capture the launcher's values once, before any preset overwrites them, so
    // a map with no entry gets the global back instead of inheriting whatever
    // the last map with an entry set. Without this the table would be sticky in
    // one direction -- the same trap g_moon_base_* exists for.
    if( !g_cloud_base_set )
    {
        g_cloud_base_set   = true;
        g_cloud_base_on    = bool{ cvar::rt_clouds };
        g_cloud_base_tint  = *( cvar::rt_clouds_tint );
        g_cloud_base_alpha = float{ cvar::rt_clouds_alpha };
        g_cloud_base_wind  = float{ cvar::rt_clouds_wind };
        g_cloud_base_shells   = int{ cvar::rt_clouds_shells };
        g_cloud_base_thick    = float{ cvar::rt_clouds_thick };
        g_cloud_base_transmit = float{ cvar::rt_clouds_transmit };
    }

    if( !bool{ cvar::rt_clouds_presets } )
    {
        return;
    }

    const CloudPreset* p = RT_FindCloudPreset( mapname );

    cvar::rt_clouds       = p ? p->clouds : g_cloud_base_on;
    cvar::rt_clouds_alpha = ( p && p->alpha >= 0.f ) ? p->alpha : g_cloud_base_alpha;
    cvar::rt_clouds_wind  = ( p && p->wind >= 0.f ) ? p->wind : g_cloud_base_wind;
    // Shape, for the maps that use the deck as a light source rather than as
    // scenery. shells is the cost knob as well as the volume knob, so raising it
    // is a per-map decision, not something to lift globally.
    cvar::rt_clouds_shells =
        ( p && p->shells > 0 ) ? p->shells : g_cloud_base_shells;
    cvar::rt_clouds_thick =
        ( p && p->thick >= 0.f ) ? p->thick : g_cloud_base_thick;
    cvar::rt_clouds_transmit =
        ( p && p->transmit >= 0.f ) ? p->transmit : g_cloud_base_transmit;
    RT_SetColorCVar( cvar::rt_clouds_tint,
                     ( p && p->tint != 0 ) ? p->tint : g_cloud_base_tint );
}

void RT_ApplyMoonPreset( const char* mapname )
{
    if( !g_moon_base_set )
    {
        g_moon_base_set = true;
        g_moon_base_a    = float{ cvar::rt_sun_a };
        g_moon_base_b    = float{ cvar::rt_sun_b };
        g_moon_base_i    = float{ cvar::rt_sun_intensity };
        g_moon_base_disc = bool{ cvar::rt_moon_geo };
        g_moon_base_sky  = float{ cvar::rt_sky };
    }

    if( !bool{ cvar::rt_moon_presets } )
    {
        return;
    }

    const MoonPreset* p = RT_FindMoonPreset( mapname );

    // Negative means "keep the launcher's", on every numeric field. A row that
    // only wants to turn the disc off says so and leaves the aim alone rather
    // than restating it -- see the VOIDSKY rows.
    cvar::rt_sun_a         = ( p && p->altitude >= 0.f ) ? p->altitude : g_moon_base_a;
    cvar::rt_sun_b         = ( p && p->azimuth >= 0.f ) ? p->azimuth : g_moon_base_b;
    cvar::rt_sun_intensity = ( p && p->intensity >= 0.f ) ? p->intensity : g_moon_base_i;
    cvar::rt_moon_geo      = p ? p->disc : g_moon_base_disc;
    cvar::rt_sky           = ( p && p->sky >= 0.f ) ? p->sky : g_moon_base_sky;

    // Say what was applied. The cloud and fog tables announce themselves and this
    // one did not, which makes a row impossible to confirm from a log: `moon` on
    // the command line runs BEFORE the level loads, so it prints the launcher's
    // pinned aim no matter what the table then does. Adding a row and checking it
    // took effect had no evidence behind it until this line existed.
    Printf( RT_DiagPrintLevel(),
            "RT moon: %s -> azimuth %.0f altitude %.0f intensity %.0f disc %s%s\n",
            mapname ? mapname : "(baseline capture)",
            float{ cvar::rt_sun_b },
            float{ cvar::rt_sun_a },
            float{ cvar::rt_sun_intensity },
            bool{ cvar::rt_moon_geo } ? "on" : "off",
            p ? " [RT_MOON_PRESETS row]" : " [no row -- launcher's values]" );
}

//-----------------------------------------------------------------------------
//
// Doom64-RT: per-map ALBEDO TINT strength.
//
// THE PROBLEM. rt_sector_tint_albedo multiplies each sector's peak-normalized
// Doom 64 colormap hue into the albedo of every world surface. A reflected beam
// is therefore `light x sector_hue x texture_albedo`, and because the hue is
// peak-normalized it can only ever REMOVE channels. On a cool sector that
// removal lands on red -- which is most of what a warm light is made of.
//
// The flashlight is ffbe82 = (1.00, 0.745, 0.51). Landing it on MAP01's
// #FFAA82 gives (1.00, 0.50, 0.26): saturated orange. Landing the SAME light on
// MAP13's #6AADFF gives (0.42, 0.51, 0.51) -- blue-dominant grey, 21% dimmer,
// saturation down 4x. That is the whole of the "the flashlight is yellow on
// MAP01 and white and weak on MAP13" report, and the muzzle flash (ff8c52,
// warmer still) loses proportionally more.
//
// WHY A TABLE AND NOT A GLOBAL. The obvious fix -- just lower the global -- is
// wrong, and measurably so. MAP02's switch-triggered blue room is
// `Sector_SetColor(21, 0, 80, 255)`, red retention 0.00: the MOST saturated
// colormap in the game, and the exact case rt_sector_tint_albedo's 1.0 default
// was chosen to match (see its description). Any monotonic global reduction hits
// it HARDER than it hits MAP13. At a global 0.6 the flashlight inside that room
// goes from (0.00, 0.23, 0.51) at saturation 1.00 to (0.40, 0.44, 0.51) at 0.22
// -- the blue room stops reading blue exactly where you point the light.
//
// So the split has to be per map, and it has to be explicit rather than derived:
// a heuristic that separates "ambient level character" from "deliberate effect"
// by colormap value alone cannot work, because MAP02's effect is the extreme end
// of the same axis MAP13's character sits in the middle of.
//
// THE NUMBERS. `redKeep` below is a map's mean R / max(R,G,B) over its authored
// sector colormaps -- literally the factor this multiplies the flashlight's red
// channel by at strength 1.0. Measured over every map's live TEXTMAP (base WAD
// plus the override wads the launcher loads, later-wins). Strength to lift a map
// to a target T solves 1 + (redKeep - 1) * s = T, i.e. s = (1 - T) / (1 - redKeep).
//
// The target here is 0.85, not MAP01's 0.93. "Whiter and weaker" is a comparison
// against MAP01, and it does not take full parity to stop reading as wrong --
// less movement means less risk to maps whose cool cast is the point. MAP31 is a
// green level and MAP26 a teal one; they are supposed to look like that.
//
// EVERY MAP AT redKeep >= 0.80 IS DELIBERATELY ABSENT, which is 24 of the 35 --
// they are already within a hair of MAP01. Absent means untouched, not defaulted:
// a map with no row gets the launcher's global back verbatim. MAP02 is absent for
// that reason and that reason alone.
struct TintPreset
{
    const char* map;
    float       albedo; // rt_sector_tint_albedo. < 0: keep the launcher's
    const char* note;
};

constexpr TintPreset RT_TINT_PRESETS[] = {
    { "map25", 0.27f, "redKeep 0.45, the coolest map in the game -- #5078C8 on 65 of 77 "
                      "sectors. Also a VOIDSKY map, so its rooms are lit by lamps and lava "
                      "rather than through the dome, which makes the flashlight most of what "
                      "you see and the tint most of what happens to it." },
    { "map26", 0.38f, "redKeep 0.60. #96FFDC on all 54 coloured sectors. This map is also "
                      "fogged and has its moon off entirely (RT_FOG_PRESETS, RT_MOON_PRESETS), "
                      "so again the flashlight is doing the work." },
    { "map30", 0.43f, "redKeep 0.65. #95DDFF x93." },
    { "map06", 0.43f, "redKeep 0.65 -- and only 43% of its sectors are cool, which is the "
                      "point: the cool ones are SATURATED greens (#AEFD97 x59, #9EFCCD x56). "
                      "Green kills a warm beam's red exactly as hard as blue does, so warm/cool "
                      "share counts are the wrong thing to read. redKeep is the right thing." },
    { "map31", 0.48f, "redKeep 0.69. #AAFFDF on 165 of 177 sectors -- a green level, and meant "
                      "to be one. 0.48 keeps that and stops it neutralizing the beam." },
    { "map10", 0.48f, "redKeep 0.69." },
    { "map04", 0.54f, "redKeep 0.72. #74A7FC / #C5BFFF / #BAB7FF." },
    { "map32", 0.54f, "redKeep 0.72, from saturated greens again (#6EFF6E x29) on a map that "
                      "is only 12% cool by sector count." },
    { "map03", 0.56f, "redKeep 0.73." },
    { "map13", 0.63f, "redKeep 0.76. The map the report came from: #6AADFF x46, #59A4FF x25, "
                      "#92C2FE x21. At 0.63 the beam lands warm again (R > G > B) instead of "
                      "blue-dominant grey, and about 25% brighter, while wide shots keep the "
                      "blue cast that makes the level look like itself." },
    { "map14", 0.71f, "redKeep 0.79, #C6B7FF x73. The mildest row -- included because 80% of "
                      "its sectors are cool, so the cast is everywhere even though it is soft." },
};

// The launcher's value, captured once before any row overwrites it, so a map
// with no row gets the global back instead of inheriting the last tinted map's
// strength. Same trap g_moon_base_* and g_cloud_base_* exist for -- and the one
// this table would have fallen into hardest, since most maps have no row.
bool  g_tint_base_set    = false;
float g_tint_base_albedo = 1.f;

const TintPreset* RT_FindTintPreset( const char* mapname )
{
    if( !mapname || mapname[ 0 ] == '\0' )
    {
        return nullptr;
    }
    for( const auto& p : RT_TINT_PRESETS )
    {
        if( stricmp( mapname, p.map ) == 0 )
        {
            return &p;
        }
    }
    return nullptr;
}

void RT_ApplyTintPreset( const char* mapname )
{
    if( !g_tint_base_set )
    {
        g_tint_base_set    = true;
        g_tint_base_albedo = float{ cvar::rt_sector_tint_albedo };
    }

    if( !bool{ cvar::rt_sector_tint_presets } )
    {
        cvar::rt_sector_tint_albedo = g_tint_base_albedo;
        return;
    }

    const TintPreset* p = RT_FindTintPreset( mapname );

    cvar::rt_sector_tint_albedo =
        ( p && p->albedo >= 0.f ) ? p->albedo : g_tint_base_albedo;
}

//-----------------------------------------------------------------------------
//
// Doom64-RT: per-map ILLUMINATED FOG.
//
// WHERE THE FOG COMES FROM. Not from here -- from the map. Doom 64 fogs whole
// levels and Retribution's MAPINFO carries it verbatim:
//
//     map MAP26 ... { fade = "00 56 56"  fogdensity = 200 }
//
// Nine maps have it (MAP12/21/25/26/27/29/30/31/33 by fade colour: cyan, brown,
// dark red). Under RT none of them showed it, because `fade` and `fogdensity`
// are consumed by the rasterizer's fog and the RT path never looks at them. The
// reference for what is missing is screen/doom64original_level26fog.png: the
// console game's MAP26, teal to the point that the far end of a corridor is
// gone.
//
// WHAT IS BUILT INSTEAD. Not that fog. Rasterizer fog is a per-pixel lerp
// toward a colour by distance -- it cannot be lit, so a lamp inside it gets no
// halo, and an unlit corridor fogs exactly as brightly as a lit hall. Under a
// path tracer the honest form of the same authored intent is the MEDIUM: a
// participating volume in RTGL1's froxel grid, which real lights scatter
// through. The map supplies the colour and the density; the renderer supplies
// what the fog does with the level's own light.
//
// Two things had to change in RTGL1 for that, both in RtVolumetric.rgen:
//
//   1. volumeMediaColor -- a scattering albedo. The froxel pass had no notion
//      of a coloured medium at all; the only colour available was the flat
//      ambient term, which tints the unlit fog and leaves everything the fog
//      does with LIGHT white. Now the tint multiplies the whole in-scattered
//      term, so cyan fog around a lamp is cyan.
//   2. volumeAllLights -- the pass scatters ONE light: whatever
//      LightManager::TryGetVolumetricLight picks, which is a
//      RG_LIGHT_ADDITIONAL_VOLUMETRIC light if one exists and otherwise the
//      sun. Nothing in this game sets that flag, so it is always the sun, and
//      on a map whose moon is deliberately off (MAP26 -- see RT_MOON_PRESETS)
//      that means the fog receives NOTHING and collapses to flat ambient.
//      With this it runs the full per-froxel direct estimate instead.
//
// Extinction stays monochrome on purpose: the transmittance channel is a single
// float all the way to CmPrepareFinal, so making it per-channel is a framebuffer
// change, not a shader one. The visible difference is that distance fades toward
// the fog colour rather than being filtered by it -- which is what the console
// game does anyway, since its fog is a lerp.
//
// OPT-IN, like RT_CLOUD_PRESETS and for the same reason: fog is not neutral
// scenery. It changes every distance judgement in a level and it costs a shadow
// ray per froxel cell. A map gets fog because someone looked at it.
//
// NOTE the table writes rt_fog_* at level load, after the command line is
// parsed, so on a listed map it overrides a +rt_fog_* pin. rt_fog_presets 0
// turns the table off -- which is what every tools/ab-fog.cmd arm except
// `preset` does.
//
// Authoring loop, same as `moon` and `clouds`: rt_fog_presets 0, tune with the
// `fog` CCMD, then type bare `fog` and paste the row it prints.
struct FogPreset
{
    const char* map;
    bool        fog;         // fog on at all
    uint32_t    color;       // 0 = use the map's own MAPINFO `fade`
    float       density;     // < 0 = derive from the map's own MAPINFO `fogdensity`
    float       far_m;       // < 0 = keep the launcher's rt_fog_far
    float       ambient;     // < 0 = keep
    int         illum;       // 1 = all-lights, 0 = single light, -1 = keep
    // The far end of the near->far ramp. Everything a map can leave alone, it
    // should: density_far < 0 means "same as near", i.e. a uniform medium, and
    // color_far 0 means "same colour". A row that fills these in is asking for
    // a ramp on purpose.
    float       density_far; // < 0 = same as density (uniform)
    uint32_t    color_far;   // 0 = same as color
    float       curve;       // < 0 = keep. 1 = linear
    const char* note;
};

// The MAP26 medium, named because more than one map ships exactly it. A second
// map that wants this look must not carry a SECOND COPY of the nine numbers --
// that copy is the one that goes stale the first time the profile is retuned,
// and the two maps then drift apart silently while both claim to match. A map
// that wants a DIFFERENT look writes its own row out in full; this is only for
// "the same medium", and it is a compile-time paste, so the `fog` CCMD still
// prints a plain paste-ready row.
#define RT_FOG_MEDIUM_MAP26 true, 0, 0.01f, 32.f, 1.f, 1, 10.f, 0, 2.4f

constexpr FogPreset RT_FOG_PRESETS[] = {
    // MAP25 -- Cat and Mouse. Same MAPINFO fog as MAP26 verbatim (`fade`
    // 00 56 56 at fogdensity 200), same VOIDSKY skybox, and now the same moon
    // treatment: disc AND light off (RT_MOON_PRESETS), which is what makes the
    // shared medium legitimate rather than a coincidence of colour. Only the
    // colour is inherited here, as on MAP26; the nine numbers come from the
    // shared medium above.
    { "map25", RT_FOG_MEDIUM_MAP26,
      "Cat and Mouse. The MAP26 medium verbatim -- see that row for what each "
      "number does and why. Legitimate because the two maps agree on everything "
      "the profile depends on: the same authored `fade` 00 56 56 at fogdensity "
      "200, the same flat-teal VOIDSKY, and (since this row) the same moon off "
      "entirely, so ILLUM 1 is as load-bearing here as it is there. If MAP25 "
      "ever wants a look of its own, replace RT_FOG_MEDIUM_MAP26 with its own "
      "nine numbers rather than editing the shared one." },
    // MAP31 -- the secret map, and the third and last VOIDSKY map. Everything
    // said of MAP25 holds verbatim: same skybox, same authored `fade` 00 56 56
    // at fogdensity 200, same moon off entirely. Checked from the WAD, not
    // assumed -- see the RT_MOON_PRESETS comment for how the three were found.
    { "map31", RT_FOG_MEDIUM_MAP26,
      "The MAP26 medium verbatim, as MAP25 -- see that row. This completes the "
      "VOIDSKY family: all three cyan-void maps now share one medium and one "
      "moon treatment, which is the point of naming the medium rather than "
      "pasting its nine numbers a third time. The maps still unlisted are the "
      "WARM ones (MAP17 3C140A, MAP27 80 1E 00, and the bonus episodes' RTR03 "
      "-- cyan but density 64 -- RTR04, ABS05, OUT10); none of them should be "
      "given this medium without being looked at first." },
    // Clear air around the player, a wall of teal at corridor distance. That
    // shape is the whole look of the reference shot and it is what the ramp
    // exists for -- see the transmittance ladder in the note.
    { "map26", RT_FOG_MEDIUM_MAP26,
      "Hardcore. The map this exists for, and the one with a reference shot "
      "(screen/doom64original_level26fog.png). "
      "The numbers are BAKED HERE, per map, the same way RT_MOON_PRESETS carries "
      "MAP13's azimuth 90 -- this is the map's authored look, not a global "
      "setting that happens to suit it, and it must not depend on a launcher pin "
      "or on whatever an ab-fog.cmd arm last left in the ini. Launch the game "
      "normally on MAP26 and this is what you get. "
      "A luminous VEIL rather than an occluder: density 0.01 at the camera to 10 "
      "at 32 m, so transmittance runs 1.00 / 0.98 / 0.93 / 0.83 at 256 / 512 / "
      "768 / 1024 map units and nothing is hidden. What makes it read is the "
      "in-scattered light, not extinction -- AMBIENT 1 is fifty times the floor "
      "the first thick version used, so the medium glows on its own and the "
      "level's lights modulate that glow rather than supply it. "
      "REACH 32 m is 1024 map units: past it everything is shaded with the far "
      "slice, so the fog stops deepening at corridor distance rather than a "
      "room-and-a-half further out. CURVE 2.4 holds the near value out to about "
      "half the volume before it climbs; at curve 1 the same ends thicken from "
      "the camera. "
      "COLOUR is the one thing still inherited: 0 falls through to the map's own "
      "MAPINFO `fade` 00 56 56, because that IS authored data and a copy of it "
      "here is the copy that goes stale. "
      "ILLUM 1 is not optional: MAP26's moon is off (RT_MOON_PRESETS), so the "
      "single-light path would leave this fog with no source at all." },
};

#undef RT_FOG_MEDIUM_MAP26

// The launcher's values, captured once before any preset overwrites them, so a
// map with no row gets the global back instead of inheriting the last fogged
// map's settings. Same reason g_moon_base_* exists.
bool     g_fog_base_set     = false;
uint32_t g_fog_base_color   = 0;
float    g_fog_base_density = -1.f;
float    g_fog_base_far     = 0.f;
float    g_fog_base_ambient = 0.f;
bool     g_fog_base_illum   = false;
float    g_fog_base_dfar    = -1.f;
uint32_t g_fog_base_cfar    = 0;
float    g_fog_base_curve   = 1.f;

// Resolution is DEFERRED to the first rendered frame, not done in
// RT_OnLevelLoad, because RT_OnLevelLoad runs from G_InitNew -- which is called
// BEFORE P_SetupLevel. At that point primaryLevel->fadeto and ->fogdensity
// still hold the PREVIOUS map's values, so reading them there gives every
// fogged map the fog of whatever was played before it.
FString g_fog_pending_map;
bool    g_fog_pending = false;
// Whether the map currently loaded has fog at all. Read every frame.
bool    g_fog_active = false;

const FogPreset* RT_FindFogPreset( const char* mapname )
{
    if( !mapname || mapname[ 0 ] == '\0' )
    {
        return nullptr;
    }
    for( const auto& p : RT_FOG_PRESETS )
    {
        if( stricmp( mapname, p.map ) == 0 )
        {
            return &p;
        }
    }
    return nullptr;
}

void RT_ApplyFogPreset( const char* mapname )
{
    if( !g_fog_base_set )
    {
        g_fog_base_set     = true;
        g_fog_base_color   = *( cvar::rt_fog_color );
        g_fog_base_density = float{ cvar::rt_fog_density };
        g_fog_base_far     = float{ cvar::rt_fog_far };
        g_fog_base_ambient = float{ cvar::rt_fog_ambient };
        g_fog_base_illum   = bool{ cvar::rt_fog_illum };
        g_fog_base_dfar    = float{ cvar::rt_fog_density_far };
        g_fog_base_cfar    = *( cvar::rt_fog_color_far );
        g_fog_base_curve   = float{ cvar::rt_fog_curve };
    }

    const FogPreset* p = bool{ cvar::rt_fog_presets } ? RT_FindFogPreset( mapname ) : nullptr;

    // With the table off, a map keeps whatever the cvars say and whether it is
    // fogged is then rt_fog_color's business alone -- that is what makes the
    // `fog` CCMD able to put fog on an unlisted map to look at it.
    if( !bool{ cvar::rt_fog_presets } )
    {
        g_fog_active = true;
        return;
    }

    g_fog_active = ( p != nullptr ) && p->fog;

    RT_SetColorCVar( cvar::rt_fog_color, p ? p->color : g_fog_base_color );
    cvar::rt_fog_density = ( p && p->density >= 0.f ) ? p->density : g_fog_base_density;
    cvar::rt_fog_far     = ( p && p->far_m >= 0.f ) ? p->far_m : g_fog_base_far;
    cvar::rt_fog_ambient = ( p && p->ambient >= 0.f ) ? p->ambient : g_fog_base_ambient;
    cvar::rt_fog_illum   = ( p && p->illum >= 0 ) ? ( p->illum != 0 ) : g_fog_base_illum;

    cvar::rt_fog_density_far = p ? p->density_far : g_fog_base_dfar;
    RT_SetColorCVar( cvar::rt_fog_color_far, p ? p->color_far : g_fog_base_cfar );
    cvar::rt_fog_curve = ( p && p->curve > 0.f ) ? p->curve : g_fog_base_curve;
}

} // anonymous namespace -- the preset TABLES stay file-local. Everything below
  // is the resolved fog that the frame loop reads, so it has to be visible;
  // ResolvedFog itself is declared in rt_internal.h.

ResolvedFog RT_ResolveFog()
{
    auto out = ResolvedFog{ .on = false };

    if( !bool{ cvar::rt_fog } || !g_fog_active || !primaryLevel )
    {
        return out;
    }

    // Colour: the cvar, or -- and this is what every preset row asks for -- the
    // map's own `fade`. Black is the sentinel rather than a separate "use map"
    // bool because a black fog scatters nothing and would be invisible, so the
    // value is not one anybody can want.
    uint32_t rgb = *( cvar::rt_fog_color ) & 0xFFFFFF;
    if( rgb == 0 )
    {
        rgb = primaryLevel->fadeto & 0xFFFFFF;
    }
    if( rgb == 0 )
    {
        // No fade in MAPINFO either: the map is listed but says nothing about a
        // colour. Fogging it white would be a guess; showing nothing is honest.
        return out;
    }

    // Density: the cvar, or the map's own fogdensity scaled. Doom 64's density
    // is a rasterizer fog factor, so the scale is an eyeballed conversion and
    // lives in a cvar for that reason.
    float density = float{ cvar::rt_fog_density };
    if( density < 0.f )
    {
        density = float( primaryLevel->fogdensity ) * float{ cvar::rt_fog_density_mult };
    }
    if( density <= 0.f )
    {
        return out;
    }

    // The far end of the ramp. Both sentinels mean "same as near", so a medium
    // stays uniform unless something asked for a ramp -- and a map that
    // inherited its density from MAPINFO has nothing to say about a far value,
    // which is exactly the case that must not invent one.
    const float dfar = float{ cvar::rt_fog_density_far } >= 0.f
                           ? float{ cvar::rt_fog_density_far }
                           : density;
    uint32_t    cfar = *( cvar::rt_fog_color_far ) & 0xFFFFFF;
    if( cfar == 0 )
    {
        cfar = rgb;
    }

    out.on          = true;
    out.r           = RPART( rgb ) / 255.f;
    out.g           = GPART( rgb ) / 255.f;
    out.b           = BPART( rgb ) / 255.f;
    out.rf          = RPART( cfar ) / 255.f;
    out.gf          = GPART( cfar ) / 255.f;
    out.bf          = BPART( cfar ) / 255.f;
    out.density     = density;
    out.density_far = dfar;
    out.curve       = std::max( 0.01f, float{ cvar::rt_fog_curve } );
    out.far_m       = std::max( 1.f, float{ cvar::rt_fog_far } );
    out.ambient     = std::max( 0.f, float{ cvar::rt_fog_ambient } );
    out.illum       = bool{ cvar::rt_fog_illum };
    return out;
}

// Called from the frame path: the level is certainly loaded by then, which is
// the whole point (see g_fog_pending).
void RT_ResolveFogIfPending()
{
    if( !g_fog_pending || !primaryLevel || primaryLevel->info == nullptr )
    {
        return;
    }
    g_fog_pending = false;

    RT_ApplyFogPreset( g_fog_pending_map.GetChars() );

    if( bool{ cvar::rt_fog_debug } )
    {
        const uint32_t fade = primaryLevel->fadeto & 0xFFFFFF;
        const auto     f    = RT_ResolveFog();
        Printf( "rt_fog: %s -- MAPINFO fade %06X, fogdensity %d; preset %s\n",
                g_fog_pending_map.GetChars(),
                fade,
                primaryLevel->fogdensity,
                RT_FindFogPreset( g_fog_pending_map.GetChars() ) ? "YES" : "none" );
        if( f.on )
        {
            Printf( "        ACTIVE: color %02X%02X%02X  density %.1f  far %.0fm  "
                    "ambient %.3f  illum %s\n",
                    int( f.r * 255 ), int( f.g * 255 ), int( f.b * 255 ),
                    f.density, f.far_m, f.ambient, f.illum ? "ALL LIGHTS" : "single light" );
        }
        else
        {
            Printf( "        OFF (%s)\n",
                    !bool{ cvar::rt_fog } ? "rt_fog 0"
                    : !g_fog_active       ? "no RT_FOG_PRESETS row"
                                          : "no colour or zero density" );
        }
    }
}

// The per-map lighting presets, re-applied on EVERY level load.
//
// RT_OnLevelLoad below runs from G_InitNew, which is reached by `map mapNN` and
// by starting a new game -- but NOT by ordinary map-to-map progression, which
// goes G_ChangeLevel -> G_DoCompleted -> G_DoWorldDone -> G_DoLoadLevel. So
// walking out of MAP12 into MAP13 left MAP12's moon aim, rt_sky, cloud deck, fog
// request and albedo tint in place, while `map map13` applied MAP13's. The same
// map was lit differently depending on how you arrived, which quietly
// invalidates any A/B whose arms do not all enter the same way.
//
// This is also why RT_TINT_PRESETS could not have been added before this was
// fixed: a per-map tint would have been correct via `map mapNN` and stale when
// you walked in, which is the hardest kind of lighting bug to see.
//
// Deliberately NOT the whole of RT_OnLevelLoad: titles, the cutscene music stop
// and the post-effect/fluid resets belong to the new-game path and are not
// idempotent enough to run again here. This is only the name-keyed preset state
// plus the two stale carry-overs that feed it.
//
// Safe to run twice -- the new-game path reaches both this and RT_OnLevelLoad.
// RT_Apply*Preset capture their g_*_base_* baseline behind a one-shot guard and
// then assign cvars from the table or that baseline, so a second call with the
// same map name is a no-op rather than a second capture of already-preset values.
void RT_OnLevelLoadPresets( const char* mapname )
{
    // A strike in flight when the level changes would keep flashing into the
    // new map, which has no storm at all unless its own MAPINFO says so.
    RT_StopLightning();
    // Same for smoke in the air: the puffs are world positions in the OLD map.
    // RT_UpdateSmokePuffs would also drop them on the maptime discontinuity,
    // but only on the next frame, and the first frame of a new level is exactly
    // where a stray puff would be seen.
    RT_ClearSmokePuffs();
    // No stale cover from the previous map's sky dimming this one's moon before
    // the first frame's RT_DrawCloudDeck gets to answer.
    RT_SetCloudSunTransmittance( 1.f, 1.f, 1.f );

    // IWAD MAPS TAKE NO PRESET. Every table here -- moon, cloud, tint, fog -- is
    // keyed on a BARE map name ("map25") and every row in them was measured on
    // Doom 64 Retribution. Retribution's maps arrive as "d64r-seqlight-fix_map25",
    // so the tables only ever matched because the lookup is handed the plain
    // level name.
    //
    // That means stock DOOM II's MAP25 matches Retribution's MAP25 row exactly --
    // and gets a fire-sky moon aim, a fog deck and an albedo tint authored for a
    // completely different level. Playing Unseen Evil (a DOOM 1/2 overhaul) is
    // what surfaced it: the moon leaks through rooms that have no opening it
    // could arrive by, because the row aiming it belongs to another game's map.
    //
    // ASK THE FILESYSTEM WHICH CONTAINER THE MAP LUMP CAME FROM. The first
    // attempt tested RT_GetMapName() for a wad prefix, and it was wrong in the
    // one way that matters: RT_MapName is not updated yet when this runs, so it
    // still holds the PREVIOUS level's name (or none at all on the first load).
    // Retribution's own MAP25 was therefore classified as an IWAD map and lost
    // its VOIDSKY row -- a preset system silently switching itself off, which is
    // exactly the class of bug this guard was added to prevent.
    //
    // GetMaxIwadNum() is the boundary the engine itself uses for "shipped with
    // the game" (see c_bind.cpp), and it is correct at this point in the load
    // because the file system is built long before any level.
    const int  maplump    = fileSystem.CheckNumForName( mapname );
    const bool is_pwadmap = maplump >= 0 &&
                            fileSystem.GetFileContainer( maplump ) >
                                fileSystem.GetMaxIwadNum();

    if( !is_pwadmap )
    {
        Printf( RT_DiagPrintLevel(),
                "RT presets: %s is an IWAD map -- skipping moon/cloud/tint/fog "
                "tables (they are keyed on Retribution map names)\n",
                mapname ? mapname : "?" );

        // The baselines still have to be captured, or the FIRST preset map of the
        // session would take an IWAD map's cvars as its "launcher's values".
        RT_ApplyMoonPreset( nullptr );
        g_fog_pending_map = "";
        g_fog_pending     = false;
        return;
    }

    RT_ApplyCloudPreset( mapname );
    RT_ApplyMoonPreset( mapname );
    RT_ApplyTintPreset( mapname );

    // Fog is only REQUESTED, for the reason given on g_fog_pending_map: the map's
    // own `fade`/`fogdensity` are not readable until P_SetupLevel has run, so
    // RT_ResolveFogIfPending picks this up on the first rendered frame instead.
    g_fog_pending_map = mapname ? mapname : "";
    g_fog_pending     = true;
    g_fog_active      = false;
}




// Aim the moon from the console: `moon <azimuth> [altitude] [intensity]`.
//
// The moon is two things -- an analytic directional light (rt_sun_*) that
// casts the shafts, and a disc you can see. The disc is GEOMETRY drawn along
// that same direction (RT_DrawMoonQuad, hw_skyportal.cpp), so the angles here
// move both at once and they cannot drift apart. There is nothing to
// calibrate: an earlier version painted the moon into the sky texture and
// needed four cvars plus a sky rotation to keep the two in step.
//
// Bare `moon` prints the current aim rather than changing it, because the
// first thing you want after walking into a room is to know where it thinks
// the moon is.
CCMD( moon )
{
    auto report = []() {
        Printf( "moon: azimuth %.1f, altitude %.1f, intensity %.0f, %s, disc %s\n",
                float{ cvar::rt_sun_b },
                float{ cvar::rt_sun_a },
                float{ cvar::rt_sun_intensity },
                bool{ cvar::rt_sun } ? "ON" : "OFF (set rt_sun 1)",
                bool{ cvar::rt_moon_geo } ? "ON" : "HIDDEN (rt_moon_geo 0)" );
        Printf( "  sky: rt_sky %.1f (the DOME's own light, separate from the "
                "moon above)\n", float{ cvar::rt_sky } );
        Printf( "  leak: require_sky %s, leak_debug %s\n        %s\n",
                bool{ cvar::rt_sun_require_sky } ? "ON" : "off",
                int{ cvar::rt_sun_leak_debug } == 2   ? "2 (COLOUR: red=shaft, green=leak)"
                : int{ cvar::rt_sun_leak_debug } == 1 ? "1 (ISOLATE: only the leak is lit)"
                                                      : "off",
                ( int{ cvar::rt_sun_leak_debug } == 2 && bool{ cvar::rt_sun_require_sky } )
                    ? "FIX + COLOUR: leaks are being dropped, survivors painted. "
                      "All RED and no GREEN = the fix is working."
                : int{ cvar::rt_sun_leak_debug } == 2
                    ? "RED = ray reached sky (real shaft). GREEN = ray escaped the map (leak)."
                : int{ cvar::rt_sun_leak_debug } == 1
                    ? "SHOWING ONLY THE LEAK - everything still lit escaped the map"
                    : ( bool{ cvar::rt_sun_require_sky }
                            ? "leaks suppressed - light must reach sky to count"
                            : "stock - a ray that hits nothing counts as lit" ) );
    };

    if( argv.argc() < 2 )
    {
        report();

        // The authoring loop: aim it here, then paste this row into
        // RT_MOON_PRESETS. Printing the row rather than writing a file keeps
        // the table a reviewable constant instead of runtime state nobody
        // can grep for.
        const char* mn = RT_GetMapName();
        if( mn && mn[ 0 ] )
        {
            const MoonPreset* have = RT_FindMoonPreset( mn );
            Printf( "  %s currently has %s. Row for RT_MOON_PRESETS:\n",
                    mn, have ? "a preset" : "NO preset (using the launcher aim)" );
            Printf( "    { \"%s\", %.0ff, %.0ff, -1.f, %s, %.1ff, \"...\" },\n",
                    mn, float{ cvar::rt_sun_b }, float{ cvar::rt_sun_a },
                    bool{ cvar::rt_moon_geo } ? "true" : "false",
                    float{ cvar::rt_sky } );
            Printf( "    (any of azimuth/altitude/intensity/sky as -1.f means "
                    "\"keep the launcher's\")\n" );
        }

        Printf( "  usage: moon <azimuth 0..360> [altitude -90..90] [intensity]\n" );
        return;
    }

    cvar::rt_sun_b = float( fmod( atof( argv[ 1 ] ), 360.0 ) );
    if( argv.argc() >= 3 )
    {
        // No altitude ceiling any more. This used to warn above 60 degrees,
        // because the sky dome spans only that much and a PAINTED moon above
        // it would have had to live in the flat sky cap, which carries no
        // texture -- so the light aimed high while the disc stayed put. The
        // moon is geometry now (RT_DrawSkyQuad) and draws over the dome and
        // the cap alike, so any altitude is honest. MAP18 ships at 70.
        cvar::rt_sun_a = std::clamp( float( atof( argv[ 2 ] ) ), -90.f, 90.f );
    }
    if( argv.argc() >= 4 )
    {
        cvar::rt_sun_intensity = std::max( 0.f, float( atof( argv[ 3 ] ) ) );
    }
    // Aiming a light that is switched off looks like the command did nothing,
    // so turn it on rather than making that a second thing to remember.
    if( !bool{ cvar::rt_sun } )
    {
        cvar::rt_sun = true;
        Printf( "moon: rt_sun was off, turned on\n" );
    }
    report();
}

// Tune the deck from the console: `clouds [on|off] [tint hex] [alpha] [wind]`.
//
// Bare `clouds` prints the current state AND the RT_CLOUD_PRESETS row to
// paste, which is the same authoring loop `moon` uses: settle it in game,
// then commit it as a reviewable constant rather than as runtime state
// nobody can grep for.
CCMD( clouds )
{
    auto report = []() {
        const char* mn = RT_GetMapName();
        Printf( "clouds: %s  tint %06X  alpha %.2f  wind %.3f  shells %d\n",
                bool{ cvar::rt_clouds } ? "ON" : "OFF",
                uint32_t( *( cvar::rt_clouds_tint ) ) & 0xFFFFFF,
                float{ cvar::rt_clouds_alpha },
                float{ cvar::rt_clouds_wind },
                int{ cvar::rt_clouds_shells } );
        Printf( "  occlude %.2f, transmit %.2f -> moonlight through solid cloud is "
                "%.0f%% and takes the tint\n",
                float{ cvar::rt_clouds_occlude },
                float{ cvar::rt_clouds_transmit },
                float{ cvar::rt_clouds_transmit } * 100.f );

        if( mn && mn[ 0 ] )
        {
            const CloudPreset* have = RT_FindCloudPreset( mn );
            Printf( "  %s currently has %s%s. Row for RT_CLOUD_PRESETS:\n",
                    mn,
                    have ? "a preset" : "NO preset (deck follows the global rt_clouds)",
                    bool{ cvar::rt_clouds_presets } ? "" : " -- but rt_clouds_presets is OFF" );
            Printf( "    { \"%s\", %s, 0x%06X, %.2ff, %.3ff, %d, %.2ff, %.2ff, \"...\" },\n",
                    mn,
                    bool{ cvar::rt_clouds } ? "true" : "false",
                    uint32_t( *( cvar::rt_clouds_tint ) ) & 0xFFFFFF,
                    float{ cvar::rt_clouds_alpha },
                    float{ cvar::rt_clouds_wind },
                    int{ cvar::rt_clouds_shells },
                    float{ cvar::rt_clouds_thick },
                    float{ cvar::rt_clouds_transmit } );
            Printf( "    (shells/thick/transmit as -1 mean \"keep the "
                    "launcher's\" -- most maps want that)\n" );
        }
    };

    if( argv.argc() < 2 )
    {
        report();
        Printf( "  usage: clouds <on|off> [tint hex] [alpha 0..1] [wind]\n" );
        return;
    }

    if( stricmp( argv[ 1 ], "on" ) == 0 )       cvar::rt_clouds = true;
    else if( stricmp( argv[ 1 ], "off" ) == 0 ) cvar::rt_clouds = false;
    else
    {
        Printf( "clouds: first argument must be on or off\n" );
        return;
    }

    if( argv.argc() >= 3 )
    {
        RT_SetColorCVar( cvar::rt_clouds_tint,
                         uint32_t( strtoul( argv[ 2 ], nullptr, 16 ) ) );
    }
    if( argv.argc() >= 4 )
    {
        cvar::rt_clouds_alpha = std::clamp( float( atof( argv[ 3 ] ) ), 0.f, 1.f );
    }
    if( argv.argc() >= 5 )
    {
        cvar::rt_clouds_wind = float( atof( argv[ 4 ] ) );
    }

    // Tuning a map that the table will immediately overwrite on the next
    // load looks like the command did nothing, so say so rather than making
    // it a second thing to remember.
    if( bool{ cvar::rt_clouds_presets } && RT_FindCloudPreset( RT_GetMapName() ) )
    {
        Printf( "clouds: NOTE this map has a RT_CLOUD_PRESETS entry -- these values "
                "last until the next level load. Paste the row below to keep them.\n" );
    }
    report();
}


// Tune the fog from the console: `fog [on|off] [colour hex] [density] [far]`.
//
// Bare `fog` prints what the medium currently is, where each number came
// from -- the map's MAPINFO or a cvar -- and the RT_FOG_PRESETS row to
// paste. Saying where a value came from matters more here than in `moon` or
// `clouds`, because two of them default to being read out of the map rather
// than set anywhere you could grep for.
CCMD( fog )
{
    auto report = []() {
        const char* mn = RT_GetMapName();
        const auto  f  = RT_ResolveFog();

        if( !f.on )
        {
            Printf( "fog: OFF (%s)\n",
                    !bool{ cvar::rt_fog } ? "rt_fog 0"
                    : !g_fog_active
                        ? "this map has no RT_FOG_PRESETS row -- `fog on` needs "
                          "rt_fog_presets 0 to stick"
                        : "no colour (rt_fog_color 0 and the map has no MAPINFO fade) "
                          "or zero density" );
        }
        else
        {
            Printf( "fog: ON  colour %02X%02X%02X (%s)  density %.1f (%s)  far %.0fm\n",
                    int( f.r * 255 ), int( f.g * 255 ), int( f.b * 255 ),
                    ( *( cvar::rt_fog_color ) & 0xFFFFFF ) ? "rt_fog_color"
                                                           : "the map's MAPINFO fade",
                    f.density,
                    float{ cvar::rt_fog_density } >= 0.f
                        ? "rt_fog_density"
                        : "the map's MAPINFO fogdensity x rt_fog_density_mult",
                    f.far_m );
            // The ramp, spelled out as near -> far rather than as two cvar
            // values, because the question actually being asked at the
            // console is "how much thicker does it get by the far end".
            const bool ramped = ( f.density_far != f.density ) || ( f.rf != f.r ) ||
                                ( f.gf != f.g ) || ( f.bf != f.b );
            if( ramped )
            {
                Printf( "  RAMP: density %.1f -> %.1f (x%.2f), colour %02X%02X%02X -> "
                        "%02X%02X%02X, curve %.2f%s\n",
                        f.density,
                        f.density_far,
                        f.density > 0.f ? f.density_far / f.density : 0.f,
                        int( f.r * 255 ), int( f.g * 255 ), int( f.b * 255 ),
                        int( f.rf * 255 ), int( f.gf * 255 ), int( f.bf * 255 ),
                        f.curve,
                        f.curve > 1.f    ? " (clear near, thickens late)"
                        : f.curve < 1.f  ? " (thickens immediately)"
                                         : " (linear)" );
            }
            else
            {
                Printf( "  RAMP: none -- uniform medium. `fog far <density>` thickens "
                        "the distance without touching the air around you.\n" );
            }
            Printf( "  ambient %.3f, lightmult %.2f, light near-fade %.1fm%s\n",
                    f.ambient,
                    float{ cvar::rt_fog_lightmult },
                    float{ cvar::rt_fog_light_near },
                    float{ cvar::rt_fog_light_near } > 0.01f
                        ? ""
                        : " (OFF -- a lit flashlight will white out the screen)" );
            Printf( "  lit by %s\n",
                    f.illum ? "ALL LIGHTS (per-froxel direct estimate)"
                            : "ONE light -- whatever TryGetVolumetricLight picked, "
                              "i.e. the sun, i.e. nothing if the moon is off" );
        }

        if( primaryLevel )
        {
            Printf( "  this map authored: fade %06X, fogdensity %d\n",
                    primaryLevel->fadeto & 0xFFFFFF,
                    primaryLevel->fogdensity );
        }

        if( mn && mn[ 0 ] )
        {
            Printf( "  %s currently has %s%s. Row for RT_FOG_PRESETS:\n",
                    mn,
                    RT_FindFogPreset( mn ) ? "a preset" : "NO preset (so no fog)",
                    bool{ cvar::rt_fog_presets } ? "" : " -- but rt_fog_presets is OFF" );
            Printf( "    { \"%s\", %s, 0x%06X, %.1ff, %.0ff, %.3ff, %d, %.1ff, "
                    "0x%06X, %.2ff, \"...\" },\n",
                    mn,
                    f.on ? "true" : "false",
                    *( cvar::rt_fog_color ) & 0xFFFFFF,
                    float{ cvar::rt_fog_density },
                    float{ cvar::rt_fog_far },
                    float{ cvar::rt_fog_ambient },
                    bool{ cvar::rt_fog_illum } ? 1 : 0,
                    float{ cvar::rt_fog_density_far },
                    *( cvar::rt_fog_color_far ) & 0xFFFFFF,
                    float{ cvar::rt_fog_curve } );
            Printf( "    (colour 0 = use the map's fade, density -1 = use its "
                    "fogdensity -- prefer both.\n"
                    "     density_far -1 and colour_far 0 mean `same as near`, i.e. "
                    "no ramp)\n" );
        }
    };

    if( argv.argc() < 2 )
    {
        report();
        Printf( "  usage: fog <on|off> [colour hex] [density] [far metres]\n"
                "         fog near  <density> [colour hex]   the air around you\n"
                "         fog far   <density> [colour hex]   at rt_fog_far\n"
                "         fog curve <k>                      the shape between them\n" );
        return;
    }

    // `fog near|far <density> [colour]` -- the two ends of the ramp, by
    // name. A 5th and 6th positional argument would have been shorter to
    // write and impossible to remember, and these are the two knobs that
    // get touched most while a map is being settled.
    const bool wantNear = stricmp( argv[ 1 ], "near" ) == 0;
    const bool wantFar  = stricmp( argv[ 1 ], "far" ) == 0;
    if( wantNear || wantFar )
    {
        if( argv.argc() < 3 )
        {
            Printf( "  usage: fog %s <density> [colour hex]   (on `far`: density -1 "
                    "and colour 0 mean `same as near`)\n",
                    wantNear ? "near" : "far" );
            return;
        }
        const float d = float( atof( argv[ 2 ] ) );
        if( wantNear )
        {
            cvar::rt_fog_density = d;
        }
        else
        {
            cvar::rt_fog_density_far = d;
        }
        if( argv.argc() >= 4 )
        {
            const uint32_t c = uint32_t( strtoul( argv[ 3 ], nullptr, 16 ) );
            RT_SetColorCVar( wantNear ? cvar::rt_fog_color : cvar::rt_fog_color_far, c );
        }
        report();
        return;
    }

    if( stricmp( argv[ 1 ], "curve" ) == 0 )
    {
        if( argv.argc() < 3 )
        {
            Printf( "  usage: fog curve <k>   (1 = linear, >1 clear near and thickens "
                    "late, <1 thickens immediately)\n" );
            return;
        }
        cvar::rt_fog_curve = std::max( 0.01f, float( atof( argv[ 2 ] ) ) );
        report();
        return;
    }

    if( stricmp( argv[ 1 ], "on" ) == 0 )
    {
        cvar::rt_fog = true;
        g_fog_active = true;
    }
    else if( stricmp( argv[ 1 ], "off" ) == 0 )
    {
        g_fog_active = false;
    }
    else
    {
        Printf( "fog: first argument must be on, off, near, far or curve\n" );
        return;
    }

    if( argv.argc() >= 3 )
    {
        RT_SetColorCVar( cvar::rt_fog_color,
                         uint32_t( strtoul( argv[ 2 ], nullptr, 16 ) ) );
    }
    if( argv.argc() >= 4 )
    {
        cvar::rt_fog_density = float( atof( argv[ 3 ] ) );
    }
    if( argv.argc() >= 5 )
    {
        cvar::rt_fog_far = std::max( 1.f, float( atof( argv[ 4 ] ) ) );
    }

    // Tuning a map the table will overwrite on the next load looks like the
    // command did nothing, so say so rather than making it a second thing
    // to remember. Same note `clouds` prints, same trap.
    if( bool{ cvar::rt_fog_presets } && RT_FindFogPreset( RT_GetMapName() ) )
    {
        Printf( "fog: NOTE this map has a RT_FOG_PRESETS entry -- these values last "
                "until the next level load. Paste the row below to keep them.\n" );
    }
    report();
}
