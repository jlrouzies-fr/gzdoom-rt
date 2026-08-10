#ifndef NOMINMAX
    #define NOMINMAX
#endif

#include "i_mainwindow.h"
#include "i_time.h"
#include "m_argv.h"
#include "win32rtvideo.h"

#include "base_sysfb.h"
#include "c_dispatch.h"
#include "hw_renderstate.h"
#include "g_levellocals.h"
#include "a_dynlight.h"
#include "r_utility.h"
#include "v_draw.h"
#include "flatvertices.h"
#include "hw_bonebuffer.h"
#include "hw_lightbuffer.h"
#include "hw_skydome.h"
#include "hw_viewpointbuffer.h"
#include "i_modelvertexbuffer.h"
#include "p_lnspec.h"
#include "image.h"
#include "texturemanager.h"
#include "actor.h"
#include "d_player.h" // player_t::ReadyWeapon, for RT_AddWeaponGlow
#include "r_state.h"

#include "rt_state.h"

#include <shellapi.h>

#include <filesystem>
#include <cmath>
#include <span>
#include <variant>
#include <ranges>
#include <unordered_map>
#include <unordered_set>


//
//
//
//
//
//

#define RG_USE_SURFACE_WIN32
#include <RTGL1/RTGL1.h>

// Generated fist offsets + colours for RT_UploadHandGlowLights.
#include "rt_hand_lights.h"

RgInterface rt      = {};
FRtState    rtstate = {};

bool g_isremix{ false };

//
//
//
//
//
//

// clang-format off
template< typename T >
using ValueToCVarRef = 
    std::conditional_t< std::is_same_v< T, bool  >, FBoolCVarRef,
    std::conditional_t< std::is_same_v< T, int   >, FIntCVarRef,
    std::conditional_t< std::is_same_v< T, float >, FFloatCVarRef,
    void > > >;

template< typename T >
constexpr ECVarType ValueToCVarType = 
    std::is_same_v< T, bool  > ? ECVarType::CVAR_Bool :
    std::is_same_v< T, int   > ? ECVarType::CVAR_Int :
    std::is_same_v< T, float > ? ECVarType::CVAR_Float :
                                 ECVarType::CVAR_Dummy;

#define RT_CVAR( name, default_value, description ) \
    ValueToCVarRef< decltype( default_value ) > name; \
    static FCVarDecl cvardecl_##name = { \
        &name, \
        ValueToCVarType< decltype( default_value ) >, \
        CVAR_GLOBALCONFIG | ( ( #name )[ 0 ] == '_' ? 0 : CVAR_ARCHIVE ), \
        #name, \
        CVarValue<ValueToCVarType< decltype( default_value ) >>( default_value ), \
        description, \
        nullptr, }; \
    extern FCVarDecl const *const cvardeclref_##name; \
    MSVC_VSEG FCVarDecl const *const cvardeclref_##name GCC_VSEG = &cvardecl_##name;

// Same as RT_CVAR but never written to the ini, so the cvar comes back at its
// default on every launch. For settings that are fixes or investigation knobs
// rather than preferences.
//
// This is not hypothetical tidiness. On 2026-08-07 a day was lost to a DLSS-RR
// "regression" that was really three cvars left at A/B values from earlier the
// same day -- rt_restir_tjitter 0, rt_shadow_samples 3, rt_rr_spechitdist 0.
// rt_restir_tjitter 0 removes the decorrelation from ReSTIR temporal reuse, so
// every pixel reprojects to exactly the same previous pixel and neighbours
// reuse in lockstep; the correlated residual noise is what RR's temporal pass
// smears into worm-like filaments. A git bisect blamed the commit that merely
// introduced the cvar, because every older build ignored it and used the old
// hardcoded constant instead.
//
// Two rules that follow, the hard way:
//   - A tuning knob must not outlive the session that set it.
//   - An A/B arm must set EVERY value explicitly. An arm that just leaves a
//     persisted cvar alone silently becomes a copy of whichever arm ran last.
#define RT_CVAR_NOARCH( name, default_value, description ) \
    ValueToCVarRef< decltype( default_value ) > name; \
    static FCVarDecl cvardecl_##name = { \
        &name, \
        ValueToCVarType< decltype( default_value ) >, \
        CVAR_GLOBALCONFIG, \
        #name, \
        CVarValue<ValueToCVarType< decltype( default_value ) >>( default_value ), \
        description, \
        nullptr, }; \
    extern FCVarDecl const *const cvardeclref_##name; \
    MSVC_VSEG FCVarDecl const *const cvardeclref_##name GCC_VSEG = &cvardecl_##name;

#define RT_CVAR_COLOR( name, default_value, description ) \
    CVARD( Color, ##name, default_value, CVAR_GLOBALCONFIG | CVAR_ARCHIVE, description )
// clang-format on


// clang-format off
namespace cvar
{
    // NOTE: if name start with '_' then the cvar won't be archived

    RT_CVAR( rt_cpu_cullmode,           0,      "[IMPACTS CPU PERFORMANCE HEAVILY] 0: BSP + all neighbor sectors of visible,  1 - original GZDoom's BSP/clip checks,  2: uploading whole map, no culling at all" )
    RT_CVAR( rt_cpu_nocullradius,       10.f,   "[IMPACTS CPU PERFORMANCE] Radius (in meters) in which culling must not be applied. Applicable with rt_cpu_cullmode=0" )

    RT_CVAR( rt_autoexport,             false,  "if true: if map's gltf doesn't exist on disk, export to gltf "
                                                "and process the map as if it's static (which improves performance / stability). "
                                                "Default false: auto-export freezes large UDMF mods (e.g. Doom 64 Retribution)." )
    RT_CVAR( rt_autoexport_light,       200.f,  "On auto export to gltf, apply this multiplier to the sector light intensities" ) 
    RT_CVAR( rt_sector_lights,          false,  "upload per-sector center point lights for ALL sectors (stock autoexport). "
                                                "Default false: floods mod maps with fake white wash" )
    RT_CVAR( rt_sector_flicker,         false,  "upload sector-center lights for flicker/strobe sector specials. "
                                                "Off by default — that blinks wall/monitor alcoves, not ceiling inset lamps" )

    RT_CVAR( rt_dynlight,               true,   "upload GZDoom map/GLDEFS dynamic lights (PointLight / Flicker / Pulse) into RT" )
    RT_CVAR( rt_dynlight_intensity,     40.0f,  "RT spherical intensity scale for GZDoom dynlights (× map radius)" )
    RT_CVAR( rt_dynlight_max,           500.0f, "hard cap on uploaded dynlight intensity after scale/stack (0 = no cap). "
                                                "Retribution key-door jambs stack 3× PointLights — uncapped blooms white" )
    RT_CVAR( rt_dynlight_rsoft,         40.f,   "map-unit radius above which dynlight energy rolls off (inv-square). "
                                                "MAP04 hall PointLights use r=88 and hit max as flat flood; door jambs "
                                                "stay at r=32 (unaffected). 0 = disable roll-off" )
    RT_CVAR( rt_dynlight_stack_atten,   true,   "divide intensity by co-located XY stack count (Doom64 door jamb strips)" )
    RT_CVAR( rt_dynlight_radius,        0.08f,  "RT light source radius in meters (smaller = harder shadows)" )
    RT_CVAR( rt_dynlight_minradius,     16.f,   "skip GZDoom dynlights whose MAP radius is below this. Retribution places bare "
                                                "stock PointLight things (white, r=12) next to switches and readables purely so "
                                                "they stay visible under the raster renderer; in PT they read as a fake hot spot "
                                                "floating on the wall. Real fixtures here are r>=32 (64BlueArmor 32, "
                                                "64TechPoleShort 48), so radius separates helper lights from fixtures without a "
                                                "class or position list. 0 = keep everything (2026-08-07)" )
    RT_CVAR( rt_dynlight_debug,         false,  "periodic console count of uploaded GZDoom dynlights + xy-stack histogram" )
    RT_CVAR( rt_dynlight_debug_marks,   false,  "also drop bright magenta marker spheres at each uploaded dynlight. Separate "
                                                "from rt_dynlight_debug because 60+ markers at intensity 400 flood the whole "
                                                "scene purple, which hides the very thing you are trying to localize" )
    RT_CVAR( rt_dynlight_flicker,       false,  "upload Flicker/RandomFlicker FDynamicLights (9802). Default false: MAP01 wall "
                                                "SMON alcoves use those; ceiling head lights are rt_ceiling_lamps instead" )

    RT_CVAR( rt_ceiling_lamps,          true,   "upload blinking shadow-casting lights under SFLATAS/SFLATAQ/SFLATAP/SPORT* ceilings "
                                                "(Doom 64 inset 'head lights') — only small sectors (see maxspan)" )
    RT_CVAR( rt_ceiling_lamp_intensity, 700.f,  "peak RT intensity for ceiling inset lamps (soft fade + RR boiling keep this usable)" )
    RT_CVAR( rt_ceiling_lamp_radius,    0.10f,  "RT source radius in meters for ceiling inset lamps" )
    RT_CVAR( rt_ceiling_lamp_zofs,      8.f,    "drop light this many map units below the ceiling plane" )
    RT_CVAR( rt_ceiling_lamp_off,       0.12f,  "intensity scale when blinking out. >0 keeps the light in ReSTIR/RR "
                                                "history (0 = hard extinguish — very noisy under DLSS-RR)" )
    RT_CVAR( rt_ceiling_lamp_fade,      40.f,   "tics to ease between on/off (0 = instant). Softens ReSTIR/RR history cuts. "
                                                "Was 8 -- too abrupt an intensity swing for ReSTIR's temporal reservoir "
                                                "reuse, producing salt localized right at the lamp (2026-08-07)." )
    RT_CVAR( rt_ceiling_lamp_maxspan,   128.f,  "skip analytic ceiling lamps if sector AABB width OR height exceeds this "
                                                "(map units). Large SFLATAQ halls only have edge texture blobs — a center "
                                                "sphere looks like a fake mid-ceiling light (MAP02)" )
    RT_CVAR( rt_ceiling_lamp_debug,     false,  "periodic console dump of ceiling inset lamp uploads + cyan marker spheres" )
    RT_CVAR( rt_wall_tex_debug,         false,  "periodic console dump of sidedef texture names + sector lightlevel near the "
                                                "camera. Used to name wall light-strip fixtures so they can be matched by "
                                                "texture instead of by map position. Covers sidedefs AND sector flats — Doom 64 "
                                                "puts fixtures on thin sector steps too, which a sidedef-only walk cannot see" )
    RT_CVAR( rt_wall_tex_debug_dist,    256.f,  "search radius in map units for rt_wall_tex_debug" )

    RT_CVAR( rt_wall_strips,            true,   "upload analytic lights along Doom 64 wall light strips (SPACEAR* bulb trim). "
                                                "RTGL1 emissive surfaces are NOT light sources — emission is only collected "
                                                "when an indirect bounce ray happens to hit them (HitInfo.inl / "
                                                "RtRaygenIndirect.inl), never through processDirectIllumination — so an "
                                                "emissive strip can never cast a pool of light or a shadow at any strength. "
                                                "Real analytic lights are the only way to make these fixtures light the room. "
                                                "Spherical, not polygonal: RgLightPolygonalEXT exists in RTGL1's header but "
                                                "LightManager.cpp compiles it out behind #if TRIANGLE_LIGHTS and hard-errors "
                                                "on upload (2026-08-07)" )
    RT_CVAR( rt_wall_strip_intensity,   180.f,  "RT intensity per strip segment. Was 500, chosen when 120 and 250 both read as no "
                                                "light at all — a strip sits flush against the wall it lights, so most of its "
                                                "sphere is occluded (§19). That premise did not survive: those readings were "
                                                "taken while ONE sector could consume the whole light budget, so only a handful "
                                                "of lights ever reached the scene. With the budget distributed by distance and "
                                                "both flat planes walked, 500 overexposed and fizzled. 180 confirmed by eye "
                                                "(2026-08-08)" )
    RT_CVAR( rt_wall_strip_minlight,    120.f,  "skip strips in sectors dimmer than this: the same trim texture is used in "
                                                "unlit maintenance areas where the bulbs are meant to be dead. Was 140, which "
                                                "only ever saw MAP03's SPACEAR strips at lightlevel 180 and silently dropped "
                                                "MAP02's STRAKR/STRAKX strips at 130 — dim, but nowhere near dead against that "
                                                "map's median of 160. Dead bulbs sit at 0-64 (2026-08-08)" )
    RT_CVAR( rt_wall_strip_seglen,      64.f,   "map units between strip lights. Keep at or below rt_wall_strip_radius in map "
                                                "units so neighbouring pools overlap — a chain of point lights spaced too far "
                                                "apart scallops along the wall instead of reading as one strip" )
    RT_CVAR( rt_wall_strip_radius,      0.35f,  "RT source radius in meters per strip light. Deliberately wider than ceiling "
                                                "lamps (0.10): a big soft source is what makes discrete spheres blend into a "
                                                "continuous strip" )
    RT_CVAR( rt_wall_strip_max,         128,    "hard cap on strip lights per frame, so a large open map cannot flood the light "
                                                "list and wreck ReSTIR/RR temporal reuse" )
    RT_CVAR( rt_wall_strip_debug,       false,  "periodic console dump of wall strip light uploads + rejection tally" )

    RT_CVAR( rt_ceiling_edge_lamps,     true,   "place lights around the PERIMETER of lamp ceilings (SFLATAS/SFLATAQ/SFLATAP/"
                                                "SPORT*) instead of one sphere at the centre. rt_ceiling_lamps skips any sector "
                                                "wider than rt_ceiling_lamp_maxspan precisely because a centre sphere in a big "
                                                "hall looks like a fake mid-ceiling light — but those halls carry their bulbs as "
                                                "blobs along the flat's EDGES, so skipping them left the bulbs casting nothing. "
                                                "Independent of rt_ceiling_lamps so the centre-sphere path can stay off "
                                                "(2026-08-07)" )
    RT_CVAR( rt_ceiling_edge_intensity, 180.f,  "RT intensity per flat bulb lamp. Kept equal to rt_wall_strip_intensity on "
                                                "purpose: the two walks light the SAME physical band where it turns a corner, so "
                                                "a mismatch shows up as a brightness step at every corner. Was 500; see that "
                                                "cvar for why the high value stopped applying (2026-08-08)" )
    RT_CVAR( rt_ceiling_edge_seglen,    64.f,   "map units between ceiling edge lamps" )
    RT_CVAR( rt_ceiling_edge_radius,    0.35f,  "RT source radius in meters for ceiling edge lamps" )
    RT_CVAR( rt_ceiling_edge_zofs,      10.f,   "drop edge lamps this many map units below the ceiling plane" )
    RT_CVAR( rt_ceiling_edge_inset,     10.f,   "pull edge lamps this many map units in from the wall, so they are not embedded "
                                                "in the geometry they light" )
    RT_CVAR( rt_ceiling_edge_max,       320,    "hard cap on flat bulb lamps per frame, counting BOTH planes. Demand is ~800 "
                                                "segments on MAP02 and MAP03, so this always binds and WHICH lights it drops is "
                                                "the whole behaviour — the walk collects everything and keeps the nearest, rather "
                                                "than stopping at the cap in sector-index order. Emitting in index order let "
                                                "MAP02's sector 16 (11,614u perimeter, 364 lights) take the entire budget and "
                                                "leave the rest of the level dark (2026-08-08)" )
    RT_CVAR( rt_ceiling_edge_maxdist,   3072.f, "skip flat bulb lamps further than this from the camera. One across the level "
                                                "contributes nothing visible but still costs a light slot and a ReSTIR reservoir. "
                                                "Doubled from 1536 -- that cut lamps (incl. faux/solo, which share this same "
                                                "distance check) at ~48m, visibly popping in as the camera approached (2026-08-09)" )
    RT_CVAR( rt_ceiling_edge_debug,     false,  "console dump of ceiling edge lamp uploads, split by ceiling vs floor" )
    RT_CVAR( rt_ceiling_edge_lattice,   true,   "place SFLATAS/SFLATAQ lights ON their painted bulbs (a per-64-unit-tile lattice, "
                                                "like rt_faux_lamps does for SFLATC) instead of around the sector perimeter. The "
                                                "perimeter walk lights a room's EDGES, but these two textures tile their bulbs "
                                                "across the whole flat, so every bulb away from a wall cast nothing and a wide "
                                                "panel stayed dark down its own middle — the exact objection the faux path was "
                                                "built to answer, which was never applied to the real arrays. Off = the old "
                                                "perimeter placement, for A/B. SPORT* keeps the perimeter walk either way: a "
                                                "teleporter pad is one fixture per sector, not a lattice (2026-08-10)" )

    RT_CVAR( rt_faux_lamps,             true,   "treat SFLATC (flat) and SPACECE (wall) as if they were bulb arrays, and light "
                                                "them with the same perimeter walks as the real ones. They are NOT lamp "
                                                "textures — no bulbs in the art, nothing in the original game lights them — so "
                                                "this is a deliberate invention to lift rooms that are simply too dark, such as "
                                                "MAP03's SFLATC-ceilinged stair hall. Kept behind its own cvar, colour and "
                                                "budget precisely because it is a lie: everything the real bulb walk does stays "
                                                "unchanged when this is off, and the two never share a light slot" )
    RT_CVAR_COLOR( rt_faux_lamp_color,  0x3C5078, "colour of the faux panel lights (hex): a dark blue-grey. Used RAW, not "
                                                "normalised to hue the way sector-tint lights are, so the darkness is real "
                                                "light output and not just a tint — 0x3C5078 emits (0.24,0.31,0.47), a "
                                                "2.0x blue:red channel spread. That is the point: these fixtures do not "
                                                "exist, so they should read as ambient fill the room happens to sit in "
                                                "rather than as a lamp the player will look for. Brightness lives in "
                                                "rt_faux_lamp_intensity — but so, partially, does hue: colour is RAW, so "
                                                "raising intensity scales all three channels together and pushes a weakly "
                                                "saturated colour toward the tonemapper's white clip point faster than a "
                                                "well-saturated one. 0x6E7F94 (1.35x spread) read as plain white once "
                                                "intensity went past ~110-200; 0x3C5078 (2.0x) was chosen to survive 500 "
                                                "(2026-08-08)" )
    RT_CVAR( rt_faux_lamp_intensity,    500.f,  "RT intensity per faux panel light. Above rt_ceiling_edge_intensity (180) on "
                                                "purpose, not below it as earlier reasoning here argued: playtest found 110 "
                                                "left the rooms this feature targets still reading as too dark, and the "
                                                "'real bulbs should out-light fake ones' worry did not survive contact with "
                                                "actually looking at it (2026-08-08)" )
    RT_CVAR( rt_faux_lamp_stride,       2,      "subsample the bulb lattice: place a faux light on every Nth socket, on both "
                                                "axes. 1 lights every bulb and is unaffordable — SFLATC's sockets sit 16 units "
                                                "apart, so a 512x512 room alone wants over a thousand lights. 2 gives 32-unit "
                                                "spacing. The stride counts ABSOLUTE lattice position, not a per-sector counter, "
                                                "so the chosen bulbs stay aligned across tile and sector seams" )
    RT_CVAR( rt_faux_lamp_max,          256,    "hard cap on faux panel lights per frame, budgeted SEPARATELY from "
                                                "rt_ceiling_edge_max and rt_wall_strip_max. Sharing a cap would be a silent "
                                                "regression: edge-lamp demand is already ~800 against a cap of 320, so adding "
                                                "SFLATC's 76 flats to the same pool would push real bulbs out of the nearest-N "
                                                "set and darken fixtures that do exist, to light ones that do not. Was 128, "
                                                "which left only 128 of ~215 wanted lit (nearest-N truncation reads as flats "
                                                "popping in as the camera approaches, not as a distance cutoff); raised to "
                                                "cover typical demand (2026-08-09)" )

    RT_CVAR( rt_solo_lamps,             true,   "light SFLATDE and SFLATCH: single-bulb ceiling flats that DO show a lit bulb "
                                                "in the art (unlike SFLATC/SPACECE's blank sockets) but that the original game "
                                                "never gave a light to. Not part of the faux/invented family — the bulb is "
                                                "real, so the light is white and placed exactly on it, one per texture tile, "
                                                "with its own cvars, colour and budget so it can never crowd rt_faux_lamps or "
                                                "the real ceiling-edge walk out of their light slots (2026-08-08)" )
    RT_CVAR_COLOR( rt_solo_lamp_color,  0xFFFFFF, "colour of the solo bulb lights (hex): plain white, used RAW like the faux "
                                                "colour. Unlike the faux panels, these fixtures genuinely show a lit bulb in "
                                                "the art, so there is no case for tinting them — white is not a placeholder "
                                                "here, it is the answer" )
    RT_CVAR( rt_solo_lamp_intensity,    45.f,   "RT intensity per solo bulb light. Deliberately modest — well under "
                                                "rt_ceiling_edge_intensity (180), rt_faux_lamp_intensity (500) and "
                                                "rt_ceiling_lamp_intensity (700): a single small ceiling bulb over a room "
                                                "should read as a normal fixture, not as the brightest thing in it. Was 90; "
                                                "halved on request after the first pass still read too strong (2026-08-08)" )
    RT_CVAR( rt_solo_lamp_radius,       0.06f,  "RT source radius in meters for solo bulb lights. Tighter than the faux/real "
                                                "lattice/edge lamps (0.35/0.10): a single visible bulb reads better with a "
                                                "harder, more precise source than a soft wide one" )
    RT_CVAR( rt_solo_lamp_zofs,         8.f,    "drop solo bulb lights this many map units below the ceiling plane" )
    RT_CVAR( rt_solo_lamp_max,          384,    "hard cap on solo bulb lights per frame, budgeted separately from "
                                                "rt_faux_lamp_max and rt_ceiling_edge_max for the same reason those two are "
                                                "split from each other — SFLATDE alone tiles across a 768x768 MAP03 room "
                                                "(144 positions at stride 1), which must not be able to starve the real or "
                                                "faux lattices of light slots. Was 64, which left only 64 of ~260 wanted lit "
                                                "(~24%) -- the nearest-N cap, not rt_ceiling_edge_maxdist, was why a solo bulb "
                                                "lit up only once it became one of the 64 closest, i.e. as the camera moved "
                                                "toward it, at any distance well inside the maxdist radius (2026-08-09)" )
    RT_CVAR( rt_solo_lamp_stride,       1,      "subsample the solo bulb lattice like rt_faux_lamp_stride. Default 1 (light "
                                                "every bulb) because these are a handful of genuine fixtures per map, not a "
                                                "dense invented grid — raise it if a large SFLATDE/SFLATCH room turns out to "
                                                "want fewer, stronger lights instead" )
    // There is deliberately no rt_eye_panels family here, and the absence is the finding
    // (2026-08-10). C23 looked like a lamp nobody had wired: a yellow speckled panel on
    // MAP09 that lit nothing. It was given its own light family on this walk before anyone
    // looked at the artwork. The artwork has no yellow in it at all -- its only saturated
    // pixels are six DARK red dots, albedo (112,0,0). The mod's own GLDEFS brightmap marks
    // exactly those six pixels, and gen_world_emissives.py had been compositing a loose
    // albedo-luma mask on top of it, painting 613 pixels of mortar highlight amber (194x
    // over-paint on C22, 102x on C23). The fixture was generated by our own toolchain, not
    // authored, so an engine light for it would have been a workaround for our own bug.
    // The mask is fixed at the source instead. Trust the brightmap.
    RT_CVAR( rt_light_mark_intensity,   25.f,   "intensity of every debug marker sphere. A marker is a real uploaded light, so N "
                                                "markers flood the scene N times over: 320 cyan marks at 400 turned a whole MAP02 "
                                                "room cyan and hid the fixtures being inspected. Section 10 recorded this for 67 "
                                                "magenta marks and fixed it by splitting stats from marks — but the flood scales "
                                                "with COUNT, so the limit has to be on the aggregate, not the single marker "
                                                "(2026-08-08)" )
    RT_CVAR( rt_light_mark_max,         24,     "mark only the N nearest lights of each path. Enough to read placement, few "
                                                "enough that the marks cannot become the lighting" )
    RT_CVAR( rt_ceiling_edge_debug_marks, false, "cyan marker spheres at each flat-mounted bulb lamp. The wall path had markers "
                                                "and this one did not, which read as 'only the wall bands are lit' when the "
                                                "flat bands were merely invisible to the debug view (2026-08-08). Cyan vs the "
                                                "wall strips' magenta so both can be on at once" )
    RT_CVAR( rt_wall_strip_debug_marks, false,  "bright magenta marker spheres at each strip light position. Separate from "
                                                "rt_wall_strip_debug: if the markers are visible the placement is fine and the "
                                                "problem is intensity; if they are not, the lights are occluded by geometry" )

    RT_CVAR( rt_hang_lamps,             true,   "upload warm shadow-casting lights at Doom 64 hanging tech lamps "
                                                "(LMP1/LMP2 sprites — MAP04 first room etc.). Map often has the props "
                                                "with no co-located PointLight things" )
    RT_CVAR( rt_hang_lamp_intensity,    220.f,  "RT intensity for hanging tech lamps (many per room — keep below ceiling lamps)" )
    RT_CVAR( rt_hang_lamp_radius,       0.09f,  "RT source radius in meters for hanging tech lamps" )
    RT_CVAR( rt_hang_lamp_zofs,         4.f,    "drop light this many map units below bulb estimate (SPAWNCEILING Z=bottom). "
                                                "Hanging lamps only — on a floor-standing pole this would walk the light down "
                                                "into the solid shaft" )
    RT_CVAR( rt_pole_lamp_intensity,    300.f,  "RT intensity for floor-standing tech pole lamps (64TechPoleLong/Short, sprites "
                                                "A035/A036). Separate from rt_hang_lamp_intensity because they are a different "
                                                "fixture in a different place: a pole lamp stands in the open with its head at "
                                                "eye level and is often the only light in a MAP01 corridor, where a hanging lamp "
                                                "comes in rows. These previously had no analytic light at all and were lit only "
                                                "by whatever PointLight the actor carries through rt_dynlight at intensity 40 "
                                                "(2026-08-08)" )
    RT_CVAR( rt_pole_lamp_zfrac,        0.88f,  "bulb height as a fraction of actor height for pole lamps. The head is at the "
                                                "TOP; the hanging lamps' 0.35 would put the light inside the shaft" )
    RT_CVAR( rt_hang_lamp_debug,        false,  "periodic console dump + yellow marker spheres at hanging lamp lights" )

    RT_CVAR( rt_hand_light_on,          true,   "upload one analytic light per FIST for the Baron family — BOS2 Hell Knight "
                                                "(green) and BOSS Baron of Hell (red) — at the fist's real body-relative "
                                                "position, instead of relying on the sprite's attached light. RTGL1 attaches "
                                                "a sprite light at the CENTRE of the billboard quad (VulkanDevice.cpp: "
                                                "center = average of the 4 quad verts), so the hand glow emitted from the "
                                                "torso, and the two fists averaged into a single point BETWEEN them. "
                                                "Emissive cannot substitute: RTGL1 emissive surfaces are not light sources "
                                                "(see rt_wall_strips), so the bright fists cannot light the forearm beside "
                                                "them at any emissiveMult. Offsets and per-monster colour are generated from "
                                                "the mod's authored brightmaps by tools/gen_hand_light_offsets.py" )
    RT_CVAR( rt_hand_light_intensity,   45.f,   "RT intensity per FIST, shared by both monsters. Two lights each, so the "
                                                "perceived total is roughly double this. Deliberately small — a passive "
                                                "knight should barely wash its own hands, not light the room; its BAL7 "
                                                "projectile (1000) is what lights a room during an attack" )
    RT_CVAR( rt_hand_light_radius,      0.06f,  "RT source radius in meters per fist. Small: a fist is a fist, and a wide "
                                                "source here washes the body flat instead of reading as a held glow" )
    RT_CVAR( rt_hand_light_maxdist,     2048.f, "cull fist lights beyond this many map units from the viewpoint" )
    RT_CVAR( rt_hand_light_max,         48,     "budget: max fist lights uploaded per frame, nearest first. 2 per monster, "
                                                "so this covers 24 simultaneously visible Hell Knights / Barons" )
    // NOARCH on purpose, unlike the older *_debug cvars next to it. This one uploads
    // magenta marker spheres at 350 intensity; archived, a single debug launch would
    // leave them burned into the ini and silently wreck every later visual judgement.
    // That is the "A/B cvars must not persist" failure this project has already paid for.
    RT_CVAR_NOARCH( rt_hand_light_debug, false, "periodic console dump + magenta marker spheres at Baron-family fist lights" )

    RT_CVAR( rt_flame_light_on,         true,   "upload one analytic, FLICKERING light per open flame — the standing torches "
                                                "(TL*/TS*), the wall torches (A030/A031/A032/GTCH), the loose fires "
                                                "(BFLM/GFLM/RFLM/YFLM) and the candle (CAND) — instead of the sprite's "
                                                "attached light. Two reasons this cannot stay in texture meta. (1) POSITION: "
                                                "RTGL1 attaches a sprite light at the CENTRE of the billboard quad, so a "
                                                "100-unit torch lit from its own midriff, ~30 units under the flame; the "
                                                "mod's GLDEFS lifts these 16..80 units and texture meta has no offset field. "
                                                "(2) FLICKER: texture meta is static per frame, and the sprite animations all "
                                                "start at map load, so any per-frame intensity ramp would pulse every torch "
                                                "in the level in lockstep. Table colours/offsets come from the mod's own "
                                                "GLDEFS (flickerlight TORCHLONG*/TORCHSHORT*/CANDLE/...); the matching "
                                                "sprites carry emissiveMult only, no lightIntensity, so this switch is the "
                                                "sole light source for every flame in the game (2026-08-10)" )
    RT_CVAR( rt_flame_light_scale,      1.0f,   "global multiplier on the per-flame table intensity. The table is already "
                                                "balanced against GLDEFS' relative sizes (candle 16, wall torch 28, standing "
                                                "torch 40), so scale the whole family here rather than retuning one entry" )
    RT_CVAR( rt_flame_light_radius,     0.09f,  "RT source radius in meters. A flame is a small, soft source; too wide and "
                                                "the shadows it throws across a corridor lose their edge entirely" )
    RT_CVAR( rt_flame_light_flicker,    0.15f,  "flicker depth, 0..1, as a fraction of base intensity (0 = steady). Three "
                                                "incommensurate sines per actor, so the pattern never visibly repeats. "
                                                "GLDEFS asks for a hard two-state switch (size/secondarySize at chance 0.5); "
                                                "that reads as strobing under a path tracer, where every flicker also moves "
                                                "the indirect bounce, so the same depth is delivered smoothly instead. "
                                                "Was 0.28 — too strong: a torch is the AMBIENT light of the room it stands "
                                                "in, so depth that would look right on a campfire in isolation swings the "
                                                "whole room's indirect bounce with it (2026-08-10)" )
    RT_CVAR( rt_flame_light_speed,      0.25f,  "flicker rate in radians per tic of the base sine (0.25 ~= 1.4 Hz). The two "
                                                "faster harmonics ride on top at 2.37x and 4.11x, so the fastest component "
                                                "is what sets the perceived rate — keep this well under 0.4 or the top "
                                                "harmonic starts to read as a strobe. Was 0.42" )
    RT_CVAR( rt_flame_light_wobble,     2.0f,   "how far the light drifts from its anchor, in MAP UNITS, on each axis. This "
                                                "is the 'moving' half of a real fire: a flame that only pulses reads as an "
                                                "electrical fault, one that also wanders reads as combustion. Kept small — "
                                                "past ~4 units the light visibly detaches from the sprite it belongs to" )
    RT_CVAR( rt_flame_light_maxdist,    3072.f, "cull flame lights beyond this many map units from the viewpoint. Larger "
                                                "than the fist cull: torches are static room lighting, and popping one in "
                                                "at range is far more visible than a monster's hand glow appearing" )
    RT_CVAR( rt_flame_light_max,        64,     "budget: max flame lights uploaded per frame, nearest first" )
    // NOARCH, for the same reason as rt_hand_light_debug: the markers are bright.
    RT_CVAR_NOARCH( rt_flame_light_debug, false, "periodic console dump + cyan marker spheres at flame lights" )

    RT_CVAR( rt_sector_emis,            0.35f,  "make bright world surfaces emit light themselves, scaled by sector lightlevel "
                                                "(0 = off). Doom 64 draws its light features as flat-shaded bright surfaces with "
                                                "no light actor anywhere — the red MAP02 corridor panels are lit purely by "
                                                "sector lightlevel. Since RT must discard lightlevel (it is baked shading), those "
                                                "rooms end up with no light source at all, and albedo tint cannot rescue them: "
                                                "zero incident light times red albedo is still black. Emitting from the surface "
                                                "is the right shape for this — a sector-center analytic sphere instead reads as a "
                                                "light bulb floating in the corner" )
    RT_CVAR( rt_sector_emis_minlight,   160.f,  "absolute floor for self-emission [0..255]. Only a floor — the effective threshold "
                                                "is max(this, map median + rt_sector_emis_margin), so a uniformly dim map does not "
                                                "start glowing just because its median is low" )
    RT_CVAR( rt_sector_emis_margin,     40.f,   "how far above the MAP'S OWN median sector lightlevel a surface must be to count as "
                                                "a light feature. This is what stops the flood: 180 means 'glowing panel' in a dark "
                                                "corridor and 'ordinary lit room' on a bright deck, so an absolute threshold either "
                                                "misses the panels or turns every wall in the level into a light (2026-08-07)" )
    RT_CVAR( rt_sector_emis_debug,      false,  "print the computed self-emission threshold and the map median it came from" )

    RT_CVAR( rt_sector_tint_lights,     0.85f,  "how much of a sector's Doom 64 colormap hue tints the analytic lights inside it "
                                                "(0 = hardcoded warm white everywhere, 1 = fully saturated sector hue). This is "
                                                "where the colored-room atmosphere comes from — emissive blobs, bloom, speculars "
                                                "and GI bounce all pick it up for free" )
    RT_CVAR( rt_sector_tint_albedo,     1.00f,  "surface tint from the sector colormap hue [0..1, values above 1 clamp]. 1.0 is "
                                                "the full normalized hue and is what matches the original game (confirmed on "
                                                "MAP02's blue armor room) — Doom 64's colored rooms are saturated, and because "
                                                "the hue is peak-normalized this only ever removes off-hue channels, never "
                                                "brightens. Lower it toward 0 for a more neutral, less stylized look" )

    RT_CVAR( rt_translucent_minalpha,   0.80f,  "floor vertex alpha for soft-blend sprites under rt_mod_compat "
                                                "(Retribution 64Spectre dips to 0.20 — pure ghost under PT alpha blend)" )
    RT_CVAR( rt_spectre_alpha,          0.20f,  "the single alpha a LIVING 64Spectre (SAR2) renders at, in every state "
                                                "[0 = disable, use whatever DECORATE's A_SetTranslucent last set]. 0.20 is the "
                                                "value the actor's own See/Melee/Pain states use, so this changes nothing "
                                                "about a spectre that is awake and chasing you — it only pulls the Spawn/Idle "
                                                "loop down to match. That loop calls A_SetTranslucent(1.0) once and then never "
                                                "lowers it again, so an idle spectre used to sit at alpha 1.0 (clipped to "
                                                "rt_translucent_minalpha 0.80) — 4x more opaque than the same monster after it "
                                                "wakes up, which is why it still read as lit in a pitch black room while a "
                                                "charging one looked correct. Cost: the Idle state's 0.25->1.0 alpha pulse is "
                                                "flattened; under path tracing that pulse reads as the ghost glowing on and "
                                                "off rather than as a shimmer (2026-08-09)" )
    RT_CVAR( rt_nightmareimp_alpha,     0.35f,  "the single alpha a LIVING 64NightmareImp (TRO2) renders at, in every state "
                                                "[0 = disable, use DECORATE's value]. The companion to rt_spectre_alpha, but "
                                                "note the two monsters had DIFFERENT bugs. The imp declares a flat "
                                                "\"Alpha 0.60\" and none of its states call A_SetTranslucent, so unlike the "
                                                "spectre its idle and chase alpha already agreed — it never had the "
                                                "Spawn-loop discrepancy. What it had instead was the opposite problem: the "
                                                "rt_translucent_minalpha FLOOR (max(a, 0.80)) applied to soft-blend sprites "
                                                "was raising it from the authored 0.60 to 0.80, i.e. making it MORE opaque "
                                                "than the actor asks for, which is the same too-visible-in-a-dark-room "
                                                "complaint by another route. Living ghosts now bypass that floor entirely. "
                                                "The default was briefly 0.60 (the authored value, once the floor was gone) "
                                                "and is now 0.35 by eye — deliberately BELOW what DECORATE asks for, on the "
                                                "same reasoning that settled the spectre: sector lightlevel is a poor proxy "
                                                "for real RT brightness in this mod, so a faint sprite is what actually makes "
                                                "imperfect light tracking stop being visible. Still well above the spectre's "
                                                "0.20 — a nightmare imp is a semi-transparent monster, not an invisible one "
                                                "(2026-08-09)" )
    RT_CVAR( rt_spectre_corpse_solid,   true,   "stop treating a 64Spectre as a spectre once it is dead (SAR2 frames I-N). "
                                                "A spectre carries RG_MESH_PRIMITIVE_TRANSLUCENT, so RTGL1 rasterizes it and "
                                                "RsWorld.inl draws vertexColor*texture with NO lighting term — the corpse "
                                                "takes light from nothing and reads as self-lit. The DECORATE Death sequence "
                                                "ends on A_SetTranslucent(1.0) anyway, so the corpse is meant to be solid: as "
                                                "an ordinary alpha-tested sprite it goes in the BLAS, gets lit and casts a "
                                                "shadow like every other corpse (2026-08-08)" )
    RT_CVAR( rt_ghost_solid,            false,  "render a LIVING soft-blend monster (64Spectre SAR2, 64NightmareImp TRO2) as an "
                                                "ordinary solid sprite instead of the rasterized translucent overlay. "
                                                "Superseded by rt_ghost_lightscale, which fixes the baked-lit look WITHOUT "
                                                "giving up transparency — leave this at 0. When on: alpha-tested at alpha 1, so the sprite is "
                                                "traced, lit and shadow-casting, and its _e eye mask emits as a real material — "
                                                "but the ghost look is gone; the body is a solid dark silhouette. Two other "
                                                "routes to solving the darkening problem were tried and rejected first: a "
                                                "vertex-colour dimmer (cannot work — RsWorld.inl derives its emissive from "
                                                "baseColor(), so darkening the body darkens the eyes by the same factor) and "
                                                "GLASS (traced and see-through, but a refractive billboard is the wrong look). "
                                                "Corpses are unaffected by this cvar — see "
                                                "rt_spectre_corpse_solid, which stays on: a dead spectre/imp is meant to read "
                                                "as a solid body, not a ghost (2026-08-08)" )
    RT_CVAR( rt_ghost_lightscale,       1.00f,  "how strongly a LIVING soft-blend monster (64Spectre SAR2, 64NightmareImp "
                                                "TRO2) fades out in a dark room [0 = never fades, always full-bright as "
                                                "before; 1 = fully tracks the sector light]. This is the fix for 'the ghost "
                                                "looks baked-lit / self-lit in a pitch black room'. "
                                                "\n"
                                                "It works by scaling the sprite's ALPHA, not its colour, which is what "
                                                "separates the body from the eyes — the thing the vertex-colour attempt could "
                                                "not do. RasterizerPipelines.cpp gives the two rasterizer outputs different "
                                                "blend factors: attachment 0 (the body) blends SRC_ALPHA, while attachment 1 "
                                                "(outScreenEmission — the _e eye mask) blends ONE/ONE and so ignores alpha "
                                                "completely. Fading alpha to 0 therefore dissolves the body while leaving the "
                                                "eyes at full strength: in pure dark a nightmare imp is a pair of floating "
                                                "eyes, and a spectre (whose _e is fully transparent) disappears outright. "
                                                "\n"
                                                "Unlike rt_illum_volume this is per-primitive, costs nothing, touches no "
                                                "other effect, and adds no temporal lag or froxel fuzz. It reads "
                                                "Sector->GetSpriteLight() via rtstate.m_lightlevel — static sector light only, "
                                                "so the flashlight and muzzle flashes do NOT brighten the ghost back up. That "
                                                "is deliberate: routing dynamic lights here is what made the illumination "
                                                "volume look foggy. Corpses are excluded — they are solid and genuinely traced "
                                                "and lit, see rt_spectre_corpse_solid (2026-08-09)" )
    RT_CVAR_NOARCH( rt_prim_debug,     false,   "Debug: list world textures RTGL1 will RASTERIZE rather than ray-trace. A "
                                                "rasterized primitive is never added to the acceleration structure, so it "
                                                "renders normally and can never block a shadow ray — which looks exactly like "
                                                "a lighting problem and is not one. Prints vertex alpha, the alpha actually "
                                                "sent, mAlphaThreshold and whether forcealpha1 fired, so a fix aimed at this "
                                                "can be confirmed live instead of judged by eye (2026-08-08)" )
    RT_CVAR( rt_force_mask_opaque,      true,   "force vertex alpha to 1 on alpha-TESTED world geometry (fences, grates, the "
                                                "MAP01 cage). RTGL1 rasterizes any primitive whose packed vertex alpha is below "
                                                "MESH_TRANSLUCENT_ALPHA_THRESHOLD (0.98), and a rasterized primitive is never "
                                                "added to the acceleration structure — so it renders perfectly and casts NO "
                                                "shadow from any light at any intensity. That is exactly what the MAP01 fence "
                                                "did while sprites in the same room cast fine. The cutout is unaffected: "
                                                "RtAlphaTest.rahit tests the TEXTURE's alpha per texel, which is a separate "
                                                "thing from the vertex alpha this controls. Only mAlphaThreshold>0 surfaces are "
                                                "touched, so water/glass/additive FX keep their alpha and stay rasterized "
                                                "(2026-08-08)" )

    RT_CVAR( rt_classic,                0.f,    "[0.0,1.0] what portion of the screen to render with a classic mode" )
    RT_CVAR( rt_classic_mus,            true,   "if true, apply high pass filter to music when classic mode is enabled" )
    RT_CVAR( rt_classic_white,          3.0f,   "white point for classic renderer" )
    RT_CVAR( rt_classic_llmin,          0.07f,  "min light level: remaps a gzdoom sector light level from [0.0,1.0] range to [rt_classic_llMIN,rt_classic_llMAX]" )
    RT_CVAR( rt_classic_llmax,          1.0f,   "max light level: remaps a gzdoom sector light level from [0.0,1.0] range to [rt_classic_llMIN,rt_classic_llMAX]" )
    RT_CVAR( rt_classic_llpow,          5.0f,   "power to apply to convert a gzdoom sector light level [0.0,1.0] to visible intensity" )

    RT_CVAR( rt_framegen,               0,      "enable frame generation via DirectX 12 and DXGI swapchain. DLSS3 if rt_upscale_dlss>0, FSR3 if rt_upscale_fsr2>0. "
                                                "Values:  0=off  1=on  -1=run frame generation logic, but skip presentation of the generated frame." )
    RT_CVAR( rt_dxgi,                   false,  "use DXGI (DirectX 12) swapchain to present to screen, better compatibility with Windows windowing system" )
    RT_CVAR( rt_vsync,                  false,  "vertical synchronization to prevent tearing" )
    RT_CVAR( rt_hdr,                    false,  "enable HDR output for display" )

    RT_CVAR( rt_fluid,                  true,   "enable fluid simulation (blood)" )
    RT_CVAR( rt_fluid_budget,         100000,   "(APPLIED ONLY after disabling rt_fluid) fluid simulation particle budget " )
    RT_CVAR( rt_fluid_pradius,          0.1f,   "(APPLIED ONLY after disabling rt_fluid) radis of one particle (in meters) for fluid simulation" )
    RT_CVAR( rt_fluid_gravity_x,        0.f,    "gravity vector for fluid (horizontal, X), in m/s^2" )
    RT_CVAR( rt_fluid_gravity_y,        0.f,    "gravity vector for fluid (horizontal, Y), in m/s^2" )
    RT_CVAR( rt_fluid_gravity_z,        -14.f,  "gravity vector for fluid (vertical), in m/s^2" )
    RT_CVAR( rt_blood_color_r,          0.4f,   "color for blood fluid (Red)" )
    RT_CVAR( rt_blood_color_g,          0.0f,   "color for blood fluid (Green)" )
    RT_CVAR( rt_blood_color_b,          0.0f,   "color for blood fluid (Blue)" )
    
    RT_CVAR( rt_renderscale,            0.f,    "[0.2, 1.0] resolution scale")
    RT_CVAR( rt_upscale_dlss,           0,      "0 - off, 1 - quality, 2 - balanced, 3 - perf, 4 - ultra perf, 5 - DLSS with rt_renderscale, 6 - DLAA. "
                                                "This controls the DLSS upscaling (Super Resolution) but not the Frame Generation" )
    RT_CVAR( rt_upscale_fsr2,           0,      "0 - off, 1 - quality, 2 - balanced, 3 - perf, 4 - ultra perf, 5 - FSR2 with rt_renderscale, 6 - native. "
                                                "This controls the FSR3 / FSR2 upscaling (Super Resolution), but not the Frame Generation.")
    RT_CVAR( rt_sharpen,                0,      "image sharpening; 0 - auto, 1 - naive, 2 - AMD CAS, 3 - force disable" )

    RT_CVAR( rt_rayreconstr,            false,  "native DLSS Ray Reconstruction (replaces A-SVGF + DLSS-SR). Requires rt_upscale_dlss>0 and nvngx_dlssd.dll. Disables frame generation." )
    RT_CVAR( rt_remix_rayreconstr,      false,  "[only for RTX Remix] DLSS Ray Reconstruction - denoise path tracing with AI" )
    RT_CVAR( rt_remix_reflex,           true,   "[only for RTX Remix] Reflex - reduce latency between inputs and visible results" )
    RT_CVAR( rt_remix_taa,              0,      "[only for RTX Remix] temporal anti aliasing. 0 - off, 1 - quality, 2 - balanced, 3 - perf, 4 - ultra perf, 5 - FSR2 with rt_renderscale, 6 - native" )

    RT_CVAR( rt_shadowrays,             4,      "max depth of shadow ray casts" )
    RT_CVAR( rt_withplayer,             true,   "enable player model for shadows, reflections etc" )
    RT_CVAR( rt_lerpmdlangle,           true,   "interpolate subtick rotation for replacements" )
    RT_CVAR( rt_spectre,                0,      "[deprecated] spectres now use alpha-tested PT (opaque sprite shape)" )
    RT_CVAR( rt_spectre_invis1,         0,      "[deprecated] invisibility uses alpha-tested PT (opaque sprite shape)" )
    RT_CVAR( rt_znear,                  0.07f,  "camera near plane (in meters); precision problems occur on a first-person weapons if too small (<=0.05)" )
    RT_CVAR( rt_zfar,                   2048.f, "camera far plane (in meters); precision problems occur on a first-person weapons if too large" )

    RT_CVAR( rt_normalmap_stren,        1.f,    "normal map influence" )
    RT_CVAR( rt_heightmap_stren,        1.f,    "height map influence" )
    RT_CVAR( rt_emis_mapboost,          200.f,  "indirect illumination emissiveness" )
    RT_CVAR( rt_emis_maxscrcolor,       8.f,    "burn on-screen emissive colors" )
    RT_CVAR( rt_emis_additive_dflt,     0.5f,   "emission value for objects with additive blending" )
    RT_CVAR( rt_smoothtextures,         false,  "enable linear texture filtering" )

    RT_CVAR( rt_tnmp_ev100_min,         2.f,    "min brightness for auto-exposure" )
    RT_CVAR( rt_tnmp_ev100_max,         7.7f,   "max brightness for auto-exposure" )
    RT_CVAR( rt_tnmp_saturation_r,      0.f,    "-1 desaturate, +1 over saturate" )
    RT_CVAR( rt_tnmp_saturation_g,      0.f,    "-1 desaturate, +1 over saturate" )
    RT_CVAR( rt_tnmp_saturation_b,      0.f,    "-1 desaturate, +1 over saturate" )
    RT_CVAR( rt_tnmp_crosstalk_r,       1.0f,   "how much to shift Red, when Green or Blue are intense; set one channel to 1.0, others to <= 1.0" )
    RT_CVAR( rt_tnmp_crosstalk_g,       0.7f,   "how much to shift Green, when Red or Blue are intense; set one channel to 1.0, others to <= 1.0" )
    RT_CVAR( rt_tnmp_crosstalk_b,       0.8f,   "how much to shift Blue, when Red or Green are intense; set one channel to 1.0, others to <= 1.0" )
    RT_CVAR( rt_tnmp_contrast,          0.1f,   "(only if rt_hdr is OFF) LDR contrast" )
    RT_CVAR( rt_hdr_contrast,           0.15f,  "(only if rt_hdr is ON) HDR contrast" )
    RT_CVAR( rt_hdr_saturation,         0.15f,  "(only if rt_hdr is ON) HDR saturation: -1 desaturate, +1 over saturate" )
    RT_CVAR( rt_hdr_brightness,         1.0f,   "(only if rt_hdr is ON) HDR brightess multiplier" )

    RT_CVAR( rt_sky,                    100.f,  "sky intensity")
    RT_CVAR( rt_sky_saturation,         1.f,    "sky saturation")
    RT_CVAR( rt_sky_stretch,            1.2f,   "how much to stretch the sky sphere along the vertical axis")
    RT_CVAR( rt_sky_always,             true,   "always submit sky geometry (even if it's not visible in primary view)")
    RT_CVAR( rt_sky_nowalls,            false,  "suppress the sky WALL band on two-sided lines -- the same thing "
                                                "ML_NOSKYWALLS does per line, applied to every line at once. Sky "
                                                "ceilings and one-sided sky curtains are untouched, so you still see "
                                                "the sky by looking up and outdoor areas stay enclosed; what goes "
                                                "away is the sideways band at wall tops. That band is real "
                                                "SKY_VISIBILITY geometry under RT -- rays hitting it get sky radiance "
                                                "-- so it is where sky light gets into rooms that look sealed. Blunt: "
                                                "it removes good bands with the bad. Use it to find out WHETHER a "
                                                "leak is wall-class before spending time on a targeted fix." )

    RT_CVAR( rt_decals,                 true,   "draw decals. NOTE: impacts CPU performance, as gzdoom requires a doom-wall to be fullyparsed to submit its decals :(")

    RT_CVAR( rt_lightlevel_min,            80,  "[replacements lights] min bound for translating gzdoom lightlevel to light intensity: if lightlevel below this, lights are multiplied by 0.0; must be >= 0" )
    RT_CVAR( rt_lightlevel_max,           230,  "[replacements lights] max bound for translating gzdoom lightlevel to light intensity: if lightlevel above this, lights are multiplied by 1.0; must be <= 255" )
    RT_CVAR( rt_lightlevel_exp,          2.0f,  "[replacements lights] exponent to apply when converting gzdoom lightlevel to light intensity" )

    RT_CVAR( rt_flsh,                   false,  "flashlight enable")
    RT_CVAR( rt_flsh_intensity,         90.f,   "flashlight intensity (dimmer = more horror)" )
    RT_CVAR( rt_flsh_radius,            0.02f,  "flashlight source disk radius in meters")
    RT_CVAR( rt_flsh_angle,             42.f,   "flashlight width in degrees")
    RT_CVAR( rt_flsh_pitch,             22.f,   "degrees to tip flashlight aim toward the ground" )
    RT_CVAR( rt_flsh_r,                 -0.3f,  "flashlight position offset - right (in meteres)")
    RT_CVAR( rt_flsh_u,                 -0.7f,  "flashlight position offset - up (in meteres)")
    RT_CVAR( rt_flsh_f,                 0.0f,   "flashlight position offset - forward (in meteres)")
    RT_CVAR_COLOR( rt_flsh_color,     0xFFBE82, "flashlight color (hex); warm horror tint default" )

    RT_CVAR( rt_flsh_battery,           true,   "horror battery cycle: on → dying flicker → recharge → repeat" )
    RT_CVAR( rt_flsh_on_secs,           30.f,   "seconds the flashlight stays lit before dying (before jitter)" )
    RT_CVAR( rt_flsh_die_secs,          4.f,    "seconds of irregular blackouts at the end of the on-phase" )
    RT_CVAR( rt_flsh_off_secs,          5.f,    "seconds of full recharge (light off)" )
    RT_CVAR( rt_flsh_jitter,            0.15f,  "0..1 randomness scale on on/off durations" )
    RT_CVAR( rt_flsh_idle_recharge,     0.25f,  "recharge rate while the flashlight is switched off, as a fraction of the post-burnout rate (0 = never)" )
    // HUD readouts (written each frame by RT_AddFlashlight; ZScript pk3 displays them)
    RT_CVAR( rt_flsh_charge,            0.f,    "0..1 battery charge readout for HUD" )
    RT_CVAR( rt_flsh_battstate,         0,      "0=off 1=on 2=dying 3=recharge (HUD readout)" )
    RT_CVAR( rt_flsh_flicker,           0,      "counter, ++ on each beam fade-out (sound cue readout)" )

    RT_CVAR( rt_sun,                    false,  "enable sun for debugging")
    RT_CVAR( rt_sun_intensity,          1000.f, "sun intensity")
    RT_CVAR( rt_sun_a,                  45.f,   "[-90, 90] sun altitude angle; how high it is from the horizon")
    RT_CVAR( rt_sun_b,                  0.f,    "[0, 360] sun azimuth angle; hotizontal angle, counter-clockwise")
    RT_CVAR_COLOR( rt_sun_color,      0xFFFFFF, "sun color (hex)")
    RT_CVAR( rt_sun_angdiam,            0.5f,   "apparent diameter of the sun/moon disc in degrees, and the global "
                                                "size gate for sky leaks. 0.5 is the real moon and makes this a POINT "
                                                "light: one shadow ray, so a crack delivers as much light as a "
                                                "doorway. Widening it makes the shadow test proportional instead -- an "
                                                "opening admits light in proportion to how much of the disc it "
                                                "reveals, so narrow gaps dim smoothly and, because an opening of size "
                                                "d seen from L away subtends d/L, distant spill dies while light "
                                                "beside the opening survives. Softens the wanted shafts by the same "
                                                "amount: one knob, both effects. Try 6-15 for leaky maps." )

    // Doom64-RT: aiming the moon from the console.
    //
    // rt_sun_* moves the LIGHT. The moon you can see is a disc painted into the
    // sky texture (tools/gen_moon_sky.py), so on its own rt_sun_b would swing the
    // shafts around while the disc stayed put. These rotate the sky dome to match,
    // which works because the sky's horizontal offset (LevelLocals::hw_sky1pos,
    // degrees) is applied as a rotation about the up axis in
    // FSkyVertexBuffer::SetupMatrices -- the same knob a scrolling sky uses.
    //
    // Rotating the whole sky rather than the disc alone is the point: the
    // starfield is uniform, so nothing else in it reads as having moved, and
    // under RT the dome is rasterised into the sky cubemap, so the disc lands at
    // the new bearing in the environment map too, not just on screen.
    //
    // Use the `moon` CCMD rather than setting these by hand.
    RT_CVAR( rt_moon_track,             true,   "rotate the sky so the painted moon disc follows rt_sun_b. Off = the "
                                                "sky stays where the texture put it and only the light moves, which "
                                                "is what you want while A/B-ing shaft direction alone." )
    RT_CVAR( rt_moon_tex_b,             135.f,  "azimuth the moon is PAINTED at in the sky texture. Must match "
                                                "tools/gen_moon_sky.py --azimuth; it is the reference point the "
                                                "tracking rotates away from, not a position in itself." )
    RT_CVAR( rt_moon_yawsign,           1.f,    "+1 or -1: which way the sky turns per degree of rt_sun_b. The sky "
                                                "dome mirrors in x AND negates u, so this sign cancels out of any "
                                                "derivation and has to be settled by looking. If moving the moon "
                                                "sends it the wrong way, flip this. 0 pins the sky." )
    RT_CVAR( rt_sky_yaw,                0.f,    "extra sky rotation in degrees, added on top of the tracking above. "
                                                "This is the one-time calibration: with rt_sun_b and the painted "
                                                "azimuth agreeing, dial this until the disc sits where the shafts "
                                                "come from, then keep the number." )
    RT_CVAR( rt_moon_presets,           true,   "apply the per-map moon aim table (RT_MOON_PRESETS) at level load. "
                                                "Off = every map uses the launcher's rt_sun_a/b, which is what you "
                                                "want while hunting a bearing for a map that has no entry yet." )

    RT_CVAR( rt_reflrefr_depth,         8,      "max depth of reflect/refract") 
    RT_CVAR( rt_refr_glass,             1.52f,  "glass index of refraction") 
    RT_CVAR( rt_refr_water,             1.33f,  "water index of refraction") 
    RT_CVAR( rt_refr_thinwidth,         0.0f,   "approx. width of thin media, e.g. thin glass (in meters)") 
    RT_CVAR( rt_refl_thresh,            0.0f,   "assume mirror if roughness is less than this value") 

    RT_CVAR( rt_mzlflsh,                true,   "enable muzzle flash light source (activated on extralight)" )
    RT_CVAR( rt_mzlflsh_intensity,      100.f,  "muzzle flash intensity" )
    RT_CVAR_COLOR( rt_mzlflsh_color,  0xFF8C52, "muzzle flash color (hex)" )
    RT_CVAR( rt_mzlflsh_radius,         0.02f,  "muzzle flash light sphere radius (in meters)")
    RT_CVAR( rt_mzlflsh_offset,         0.6f,   "[0.0, 1.0] muzzle flash offset from the hit point, so the light would not be in a wall")
    RT_CVAR( rt_mzlflsh_f,              3.0f,   "muzzle flash light offset - forward (in meteres)" )
    RT_CVAR( rt_mzlflsh_u,              -0.9f,  "muzzle flash light offset - up (in meteres)" )
    RT_CVAR( rt_mzlflsh_fade,           5.f,    "soft fade-out duration in tics when extralight ends (0 = hard cut; reduces RR residual sparkle)" )

    // Per-weapon muzzle flash colour. rt_mzlflsh_color above stays the default for
    // everything not listed here (pistol / shotgun / SSG / rocket: their flash art means
    // out warm, #ffd5ae, which is what that default already is).
    //
    // These four are NOT invented — each is the mean of the bright texels of that
    // weapon's own flash sprite in D64RTR_v15.WAD, peak-normalised:
    //   CHGF (chaingun) #9677ff   BFGF (BFG) #a0ffa0
    //   UNMF (unmaker)  #ff1111   PLSF (plasma) #7dbfff
    // Plasma is pinned to the same blue as the bolt and the gun's core rather than its
    // sampled #7dbfff, which reads cyan next to them.
    RT_CVAR( rt_mzlflsh_perweapon,      true,   "tint the muzzle flash per weapon (plasma blue, BFG green, unmaker red, "
                                                "chaingun blue-purple); 0 = always rt_mzlflsh_color" )
    RT_CVAR_COLOR( rt_mzlflsh_color_plasma,   0x3355FF, "muzzle flash color for the plasma rifle (hex)" )
    RT_CVAR_COLOR( rt_mzlflsh_color_bfg,      0xA0FFA0, "muzzle flash color for the BFG (hex)" )
    RT_CVAR_COLOR( rt_mzlflsh_color_unmaker,  0xFF1111, "muzzle flash color for the unmaker / laser (hex)" )
    RT_CVAR_COLOR( rt_mzlflsh_color_chaingun, 0x9677FF, "muzzle flash color for the chaingun (hex)" )
    RT_CVAR( rt_mzlflsh_luma_compensate, true,  "scale muzzle flash intensity by the tint's relative luminance, so a "
                                                "saturated colour is not dimmer than the warm default at the same "
                                                "rt_mzlflsh_intensity" )

    // Passive glow from a weapon that has a lit element in its art (the plasma rifle's
    // electric core). This CANNOT be done with a lightIntensity on the sprite's own
    // texture: a first-person prim is rasterized, so RTGL1 attaches the light to the
    // very quad it is meant to illuminate, and the plasma rifle was the only weapon
    // carrying one — which is exactly why the artefact showed up on that gun and on no
    // other. Emission cannot do it either: RsWorld.inl writes a view weapon's emission
    // to outScreenEmission, a screen-space additive overlay that lights nothing.
    // So the light lives here, at the viewer, like the flashlight and the muzzle flash.
    RT_CVAR( rt_gunglow,                true,   "enable the ready weapon's passive core glow as a real light source" )
    RT_CVAR( rt_gunglow_intensity,      90.f,   "weapon core glow intensity. Lower than it looks like it should be: the "
                                                "light is anchored centimetres off the sprite, so inverse-square makes it "
                                                "land far harder on the gun than on the room" )
    RT_CVAR_COLOR( rt_gunglow_color,  0x3355FF, "weapon core glow color (hex); plasma rifle blue" )
    RT_CVAR( rt_gunglow_radius,         0.07f,  "weapon core glow light sphere radius (in meters). A wider source softens "
                                                "the falloff across the chassis instead of hot-spotting one panel" )
    RT_CVAR( rt_gunglow_f,              0.0f,   "weapon core glow trim - forward (in meters), on top of the gun anchor" )
    RT_CVAR( rt_gunglow_u,              0.0f,   "weapon core glow trim - up (in meters), on top of the gun anchor" )
    RT_CVAR( rt_gunglow_pullback,       0.5f,   "[0,0.95] how far from the gun's quad toward the eye to place the core "
                                                "light. This is the knob for the SPRITE's exposure specifically: the room "
                                                "is metres away so moving the light a few cm barely changes it, but the "
                                                "gun is right there and takes the inverse square. Raise it if the chassis "
                                                "blows out, lower it if the gun goes flat" )
    RT_CVAR( rt_gunglow_bob,            0.012f, "how much the core glow follows the weapon's bob, in meters per psprite "
                                                "unit. 0 pins the light to the view instead, which reads as a lamp on the "
                                                "camera rather than a light on the gun" )
    RT_CVAR( rt_gunglow_flicker,        0.22f,  "[0,1] depth of the core glow's flicker. Electricity is not a steady lamp; "
                                                "two detuned sines beat against each other so it never loops audibly. "
                                                "0 = steady" )
    RT_CVAR( rt_gunglow_fire_boost,     2.2f,   "core glow multiplier while the weapon is firing (extralight up)" )

    RT_CVAR( rt_wpn_solid_bright,       true,   "draw FULLBRIGHT view-weapon frames as alpha-tested cutouts instead of "
                                                "translucent UI overlays. 0 restores the old behaviour, where a BRIGHT "
                                                "weapon frame (Retribution's plasma rifle fire frames) went see-through." )
    RT_CVAR( rt_wpn_debug,              false,  "log every first-person weapon primitive: texture, prim flags, alpha chain. "
                                                "One line per distinct texture+flags combination, so shooting prints a few "
                                                "lines and then goes quiet. For diagnosing view-weapon blending." )

    RT_CVAR( rt_illum_sens_direct,      1.0f,   "[0,1] lighting-change sensitivity for direct diffuse (higher = faster history invalidation)" )
    RT_CVAR( rt_illum_sens_indirect,    0.75f,  "[0,1] lighting-change sensitivity for indirect diffuse (stock RTGL default bias)" )
    RT_CVAR( rt_illum_sens_spec,        1.0f,   "[0,1] lighting-change sensitivity for specular" )
    RT_CVAR( rt_rr_temporal,            false,  "DLSS-RR: A-SVGF temporal before ComposeNoisy. Default OFF — "
                                                "caused faded duplicate/ghost depth view (double reprojection + "
                                                "checkerboard coord mismatch). Keep soft lamp fades instead." )

    RT_CVAR( rt_rr_disocc,              true,   "DLSS-RR: disocclusion mask — force RR to drop temporal history where "
                                                "scene luminance changed sharply vs previous frame (motion-reprojected, "
                                                "per 16x16 tile). Fixes barrel/muzzle flash linger and occluded-glow ghosting." )
    RT_CVAR( rt_rr_disocc_ratio,        3.0f,   "DLSS-RR disocclusion: tile luminance ratio (cur vs prev, symmetric) above "
                                                "which history is discarded. Lower = more responsive, noisier. [1.0, +inf)" )
    RT_CVAR( rt_rr_disocc_mindelta,     0.01f,  "DLSS-RR disocclusion: minimum absolute tile luminance delta to fire "
                                                "(guards against false positives in near-black areas)" )
    RT_CVAR( rt_rr_disocc_show,         false,  "DLSS-RR disocclusion: debug — tint fired tiles red in final image" )

    RT_CVAR_NOARCH( rt_rr_firefly,             0.0f,   "DLSS-RR: neighbourhood firefly clamp on the noisy lighting before RR. "
                                                "A pixel is scaled down only if it out-shines the brightest of its 4 "
                                                "neighbours by this factor. Lower = more aggressive. 0 = off (default). "
                                                "A/B'd 2026-08-07 at 4.0/2.0: barely reduced motion noise and ADDED a "
                                                "trail behind the weapon sprite — suppressing outliers removes the local "
                                                "contrast RR uses to detect change, so it over-trusts history. Kept as a "
                                                "knob, off by default; it trades noise for ghosting rather than fixing it." )
    RT_CVAR_NOARCH( rt_rr_firefly_minlum,      0.01f,  "DLSS-RR firefly clamp: absolute luminance floor below which the clamp "
                                                "is skipped (ratios are meaningless in near-black areas)" )

    RT_CVAR_NOARCH( rt_restir_bluenoise,       true,   "ReSTIR: place temporal/spatial reuse taps with tiled blue noise instead "
                                                "of hash white noise. White noise makes the 8 spatial taps clump and "
                                                "neighbouring pixels reuse overlapping neighbourhoods, correlating their "
                                                "estimates into low-frequency blotching no denoiser can separate from "
                                                "signal. Reduces variance at the SOURCE, so it helps A-SVGF and DLSS-RR "
                                                "alike (RR guide 3.5 requires decorrelated reservoirs). Judge with the "
                                                "Dev 'Unfiltered diffuse direct' view, not the final image." )

    RT_CVAR_NOARCH( rt_shadow_samples,            1,   "Shadow rays per pixel for DIRECT lighting [1..8]. Direct illumination "
                                                "multiplies by a single binary visibility ray, so at 1 spp a pixel is "
                                                "fully lit or fully black — that 0/1 term dominates the raw noise and is "
                                                "untouched by ReSTIR or by any denoiser tuning. Averaging N points on the "
                                                "chosen light makes it a soft fraction; noise falls roughly as 1/sqrt(N). "
                                                "Costs N-1 extra rays per pixel. Judge in the Dev 'Unfiltered diffuse "
                                                "direct' view — the effect should be obvious there if anywhere." )

    RT_CVAR_NOARCH( rt_debug_visibility,         0,    "Debug: show the shadow-ray visibility term instead of radiance. "
                                                "1 = greyscale, BLACK where the shadow ray was blocked. 2 = normal "
                                                "shading with shadowed pixels tinted red, to locate an umbra against "
                                                "the geometry casting it. "
                                                "Exists because the final image cannot separate \"the occluder never "
                                                "blocked the ray\" from \"the shadow is cast but drowned in fill light "
                                                "or smeared away by the denoiser\" — both read as no shadow, and four "
                                                "A/B ladders failed to tell them apart (2026-08-08). If an occluder "
                                                "shows black here, shadow casting works and the problem is downstream; "
                                                "if it does not, the ray is not being blocked and the cause is in the "
                                                "geometry upload or the shadow cull mask." )

    RT_CVAR_NOARCH( rt_debug_restir_m,       false,    "Debug: show ReSTIR reservoir M (accumulated sample count) instead of "
                                                "radiance, as a green ramp (M/32; black = M=1, the worst case). ReSTIR at "
                                                "1 spp only converges because temporal reuse grows M. Stand still and "
                                                "watch it brighten, then move: if it goes dark, history is being rejected "
                                                "and the RAW signal really is noisier in motion — upstream of every "
                                                "denoiser, so no denoiser tuning can fix it." )

    RT_CVAR_NOARCH( rt_restir_tjitter,        2.0f,   "ReSTIR temporal reuse tap jitter radius, in pixels (stock 2). The jitter "
                                                "decorrelates the tap, but on grazing surfaces a 2px offset moves depth "
                                                "well past the flat 10%% reuse threshold, so the tap is rejected and M "
                                                "collapses to 1 exactly where variance is already worst — visible as the "
                                                "sides going dark under rt_debug_restir_m 1 while moving. 0 = reproject "
                                                "exactly (what RTXDI does)." )

    RT_CVAR_NOARCH( rt_rr_spechitdist,         true,   "DLSS-RR: feed pInSpecularHitDistance (world distance from the shading "
                                                "point to whatever produced the highlight). Specular does not live ON the "
                                                "surface, so without it RR reprojects highlights as if it did and glossy "
                                                "surfaces smear/fizzle in motion — the exact symptom, on a full PBR/ORM "
                                                "texture set. Was nullptr because the old binding (FB_DEPTH_WORLD) was the "
                                                "primary-hit CAMERA distance, the wrong signal; this is the right one. "
                                                "Shipping RR integrations all provide it." )

    // --- Samples per pixel ---------------------------------------------------
    RT_CVAR_NOARCH( rt_spp_direct,                1,   "Direct-lighting samples per pixel [1..8]. The path tracer is 1 spp and "
                                                "only converges via temporal accumulation, which camera motion destroys — "
                                                "so the raw signal is what you see while moving. N independent estimates "
                                                "averaged cut that noise ~1/sqrt(N) at the SOURCE, which is upstream of the "
                                                "denoiser and so helps A-SVGF and DLSS-RR equally. Costs 1 extra shadow ray "
                                                "per extra sample. 1 = stock (bit-identical)." )
    RT_CVAR_NOARCH( rt_spp_indirect,              1,   "Indirect/GI samples per pixel [1..8]. N independent paths RIS-combined "
                                                "into the initial reservoir. Costs ~4 extra rays per extra sample — the "
                                                "expensive one. 1 = stock (bit-identical)." )

    // --- ReSTIR quality (previously hardcoded) -------------------------------
    RT_CVAR_NOARCH( rt_restir_initial,            8,   "ReSTIR RIS candidate lights per pixel [1..32] (was hardcoded 8). Traces "
                                                "NO rays — pure light importance-sampling quality, so this is close to "
                                                "free. Raise before reaching for rt_spp_direct." )
    RT_CVAR_NOARCH( rt_restir_spatial,            8,   "ReSTIR spatial reuse taps in the direct pass [0..16] (was hardcoded 8). "
                                                "Image reads, no rays." )
    RT_CVAR_NOARCH( rt_restir_spatial_radius,  30.f,   "Radius in pixels of the ReSTIR spatial reuse taps [1..64] (was hardcoded "
                                                "30). Wider samples a broader neighbourhood but gets more taps rejected by "
                                                "the depth/normal reuse test." )
    RT_CVAR_NOARCH( rt_restir_mcap,              20,   "Cap on accumulated ReSTIR temporal M, as a multiple of the initial "
                                                "reservoir's M [1..64] (was hardcoded 20). Higher = longer history = "
                                                "smoother when still, slower to react to change. Watch it with "
                                                "rt_debug_restir_m 1." )

    // NOT archived: this is a bisect handle for the worm-artifact regression.
    // A value left over from an A/B would silently decide image quality later.
    RT_CVAR_NOARCH( rt_restir_indir_antilag, 1, "Apply the A-SVGF antilag gate to INDIRECT ReSTIR "
                                                "temporal reuse [0/1]. The gradient buffer that gate reads "
                                                "(framebufDISGradientHistory) is written ONLY by "
                                                "CmASVGFGradientAtrous, which runs only inside "
                                                "Denoiser::Denoise() — and DLSS-RR skips Denoise() in favour of "
                                                "ComposeNoisy(). So under RR the gate tests a buffer nothing "
                                                "updates: either it is a dead no-op, or it rejects GI temporal "
                                                "reuse every frame, leaving indirect lighting at 1 spp with no "
                                                "accumulation (surfaces fizzle; only the denoiser's own history "
                                                "hides it, which motion removes). 0 = ignore the gate." )

    RT_CVAR_NOARCH( rt_rr_guide_mode,      1,   "DLSS-RR: what the albedo guides contain. RR uses them for edge detection "
                                                "and reprojection, not just demodulation, so they want to be SMOOTH "
                                                "material properties. 0 = raw albedo/F0 (what shipped before 2026-08-05, "
                                                "when the worm artifact was reportedly absent). 1 = ro_d/envBRDF * "
                                                "throughput * ambient (the 2026-08-06 rework; folds in throughput, which "
                                                "is fetched in CHECKERBOARD space so neighbouring output pixels read "
                                                "non-adjacent texels, and getMaterialAmbient(), a thresholded quadratic "
                                                "that hits exactly 0 at black albedo — a derivative kink along dark "
                                                "texture contours that no guide FLOOR can remove). 2 = ro_d/envBRDF only, "
                                                "no throughput/ambient, to separate the two changes. Does not affect the "
                                                "composed colour, only what RR is told the albedo is." )

    RT_CVAR_NOARCH( rt_rr_guide_min,          0.01f,   "DLSS-RR: floor for the diffuse/specular albedo guides. RR demodulates "
                                                "colour by these (lighting ~ colour/guide) and remodulates with the same "
                                                "values, so a small floor costs almost nothing — but without it the "
                                                "division explodes wherever albedo*throughput approaches 0 (dark rooms, "
                                                "and exactly 0 on metallic), producing correlated dark worms in dim "
                                                "distant areas while lit near surfaces stay clean. A-SVGF never reads "
                                                "these guides. 0 = no floor. A/B'd: does NOT fix the worm artifact (0 vs "
                                                "0.01 is a wash, higher is worse), but 0.01 is kept because ro_d is "
                                                "exactly 0 on metallic surfaces, which is a genuine divide-by-zero." )

    RT_CVAR_NOARCH( rt_mip_bias,               0.0f,   "Offset added to the texture mip LOD bias. RTGL uses the DLSS-SR formula "
                                                "log2(render/output) - 1.0, which at Balanced is ~-1.77 mips: textures are "
                                                "sampled far sharper than the render resolution can represent. DLSS-SR "
                                                "turns that aliasing into detail from a CLEAN image; DLSS-RR gets the same "
                                                "over-sharp texture inside a NOISY albedo guide. Positive = softer (+1.0 "
                                                "cancels the -1.0 term). A/B'd at +1.0 and +1.8: does NOT fix the RR worm "
                                                "artifact on distant textures. Render resolution only makes it LESS "
                                                "VISIBLE (Balanced worst, DLAA best) — it is still present at DLAA, where "
                                                "A-SVGF is clean, so resolution is not the cause. Still unexplained." )

    // NOT archived (RT_CVAR_NOARCH): this is a fix, not a preference, and it must
    // come up enabled on every launch. It stays a cvar only so it can be flipped
    // off for an in-session A/B -- a stuck 0 in the ini silently reinstates the
    // ~3-7s flashlight linger, and this project has lost sessions to exactly that.
    RT_CVAR_NOARCH( rt_rr_reset_on_lightcut, true,
                                                "DLSS-RR: flush temporal history (InReset) on an abrupt light "
                                                "cut — flashlight on/off. Fixes ~3-7s linger under RR's "
                                                "stabilized history. Always on at launch (not saved to the ini); "
                                                "set to 0 only for a temporary A/B. See rt_rr_reset_on_dynlight." )
    RT_CVAR( rt_rr_reset_delta,         0.5f,   "DLSS-RR reset: min abrupt change in emitted flashlight scale "
                                                "(0..1) that counts as a light cut" )
    RT_CVAR( rt_rr_reset_on_dynlight,   true,   "DLSS-RR: also flush temporal history when a GZDoom dynamic "
                                                "light (barrel/rocket explosion flash, pickup glow, etc.) newly "
                                                "appears or disappears from the uploaded light list. Steady "
                                                "flicker/pulse lights don't count (they never leave the list). "
                                                "Muzzle flash is intentionally excluded — too frequent, already "
                                                "soft-faded via rt_mzlflsh_fade." )
    RT_CVAR( rt_rr_reset_min_ms,        250,    "DLSS-RR reset: minimum milliseconds between history flushes "
                                                "(rate limit, avoids back-to-back flushes during rapid triggers)" )
    RT_CVAR( rt_rr_reset_hold,          false,  "DLSS-RR reset: diagnostic — force InReset every frame "
                                                "(image should go visibly noisy if this reaches NGX)" )
    RT_CVAR( rt_rr_reset_now,           false,  "DLSS-RR reset: diagnostic — fire a single history flush, "
                                                "then self-clears" )
    RT_CVAR( rt_rr_reset_debug,         false,  "DLSS-RR reset: diagnostic — log every history flush (with its "
                                                "cause) plus a once-a-second fired/suppressed tally. Use this to "
                                                "check whether a trigger is over-firing." )

    RT_CVAR( rt_volume_type,            1,      "0 - none, 1 - volumetric, 2 - distance based" )
    RT_CVAR( rt_volume_far,             30.f,   "max distance of scattering volume (in meteres)" )
    RT_CVAR( rt_volume_scatter,         1.f,    "density of media" )
    RT_CVAR( rt_volume_ambient,         0.2f,   "ambient term" )
    RT_CVAR( rt_volume_lintensity,      1.f,    "intensity of lights for scattering" )
    RT_CVAR( rt_volume_lassymetry,      0.5f,   "scaterring phase function assymetry" )
    RT_CVAR( rt_volume_history,         8.f,    "max history length for scaterring accumulation (in frames)" )
    RT_CVAR( rt_illum_volume,           false,  "sample the traced illumination volume (RtVolumetric.rgen output) when shading "
                                                "RASTERIZED translucent primitives, instead of the RTGL1 default "
                                                "max(1, avgLuminance). OFF: tried as a way to dim ghost sprites in dark rooms "
                                                "and rejected, because the thing it samples is NOT surface irradiance -- "
                                                "RtVolumetric.rgen writes g_illuminationVolume from the same froxel pass that "
                                                "feeds volumetric FOG: coarse 3D grid, temporally blended at 0.05 (~20 frame "
                                                "lag), storing absolute unnormalized radiance. Multiplying a sprite by it gave "
                                                "exactly the artifacts you would predict from that: a muzzle flash or the "
                                                "flashlight lights up whole froxel cells, so the sprite reads foggy/fuzzy and "
                                                "haloed, and because the radiance is unnormalized (>1 under a light) the "
                                                "additive-blended body brightens until it stops looking see-through. It is "
                                                "also a GLOBAL switch in RsWorld.inl, hitting every particle and additive FX, "
                                                "not just the ghosts. Superseded by rt_ghost_lightscale, which is per-sprite "
                                                "and needs no shader support. Left in (and RTGL1 still built with "
                                                "ILLUMINATION_VOLUME=1) only so the path can be re-tested (2026-08-09)" )

    RT_CVAR( rt_water_r,                255,    "water color Red [0,255]" )
    RT_CVAR( rt_water_g,                255,    "water color Green [0,255]" )
    RT_CVAR( rt_water_b,                255,    "water color Blue [0,255]" )
    RT_CVAR( rt_water_wavestren,        0.4f,   "normal map strength for water. Also drives the partial-invisibility warp. 3.0 was the old default: it shattered the reflected image into sparkle, which reads as no reflection at all." )
    // These two were hardcoded (0.05 / 1.0) with the comment "for
    // partial_invisibility" -- the invisibility warp uses the same water normal
    // field. At 0.05 the waves crawl: one full cycle of the normal texture takes
    // minutes, so the stylized water's caustics would not shimmer at all. Now
    // exposed, at RTGL's own default speed and a tighter tile scale. They still
    // also drive the partial-invisibility warp, which is only cosmetic there.
    RT_CVAR( rt_water_wavespeed,        0.2f,   "water wave scroll speed (also the partial-invisibility warp). "
                                                "0.05 was the old hardcoded value — effectively static." )
    RT_CVAR( rt_water_areascale,        0.35f,  "world area one tile of the water normal texture covers. Larger "
                                                "= broader, slower swells; smaller = tighter ripples." )

    // Doom64-RT stylized water.
    //
    // RTGL's stock water is physical: refract into the media, absorb with
    // Beer-Lambert, mirror-reflect the rest. That reads far too real next to
    // Doom 64's art, and for these maps it is also plain wrong — D64W2_01 /
    // D64W1_01 are opaque FLOOR flats, there is no sector under them to
    // refract into, so the refraction half of the split has nothing to show.
    //
    // Stylized mode drops refraction and spends the checkerboard split on
    // "keep the lit water surface" vs "mirror reflection", resolved by
    // CmCheckerboard into  F*reflection + (1-F)*surface. The surface itself is
    // rebuilt from the flat: deep blue body + the texture's own caustic veins,
    // shimmering with the animated wave normal. Implemented in
    // deps/RTGL Shaders/RaygenPrimary.inl (getStylizedWaterAlbedo).
    //
    // Only applies to surfaces RTGL already considers water, i.e. textures
    // tagged "isWater" in rt/data/textures.json (tools/set_water_meta.py).
    RT_CVAR( rt_water_style,            true,   "stylized (Doom 64) water instead of physical refract+absorb. "
                                                "Deep blue opaque body with the flat's own caustics, plus a "
                                                "Fresnel-weighted reflection. 0 = stock RTGL water." )
    RT_CVAR( rt_water_tint_r,           5,      "stylized water: body colour Red [0,255]" )
    RT_CVAR( rt_water_tint_g,          23,      "stylized water: body colour Green [0,255]" )
    RT_CVAR( rt_water_tint_b,          61,      "stylized water: body colour Blue [0,255]" )
    RT_CVAR( rt_water_caustic,          1.5f,   "stylized water: how hard the wave crests brighten the "
                                                "texture's caustic veins. 0 = static veins." )
    // Reflection strength is deliberately NOT physical. Real water has F0=0.02,
    // so a correct Schlick term is ~2% looking straight down -- the reflection
    // is there, and it reflects sprites and geometry correctly, but at a
    // strength you cannot see. The shader keeps the SHAPE of the Schlick curve
    // and remaps its range onto [reflmin, reflmax].
    RT_CVAR( rt_water_reflmin,          0.1f,   "stylized water: reflection strength looking straight DOWN at "
                                                "the surface. Physical water is 0.02 (invisible); this is the "
                                                "artistic floor that makes it read as a reflective pool. 0 = "
                                                "physically correct and effectively no reflection from above." )
    RT_CVAR( rt_water_reflmax,          0.75f,  "stylized water: reflection strength at GRAZING angles. 1 = a "
                                                "true mirror at the horizon, lower keeps it a sheen." )
    RT_CVAR( rt_water_rough,            0.1f,   "stylized water: roughness of the surface half (specular "
                                                "highlight width from lights)" )
    RT_CVAR( rt_water_glow,             0.15f,  "stylized water: unlit on-screen sheen on the caustic veins, "
                                                "so the pattern still reads in near-black rooms. Casts no "
                                                "light. 0 = fully lighting-dependent." )
    RT_CVAR( rt_water_veinref,          0.1f,   "stylized water: LINEAR luminance that saturates the caustic "
                                                "vein mask. Measured on D64W2_01: median texel 0.0024, p90 "
                                                "0.0064, p95 0.0113, p99 0.0161, max 0.0327 — the flat is far "
                                                "darker in linear space than its sRGB thumbnail suggests, so a "
                                                "value near the p99 is what makes the vein cores read while the "
                                                "body stays deep. Lower = wider, brighter veins." )
    // Caustics cast BY the water ONTO the walls and floors around it. A 1-spp
    // path tracer cannot find these on its own -- a caustic is a specular-to-
    // diffuse path, and a random diffuse bounce has effectively no chance of
    // landing on the water and then scattering into a light -- so they are
    // projected: one probe ray straight down per shading point, and if it lands
    // on water within rt_water_caustic_dist the point's DIRECT lighting is
    // modulated by an animated caustic field. Multiplicative, never additive:
    // caustics are focused light, not a light source, so a dark room stays dark.
    RT_CVAR( rt_water_caustics,         1.2f,   "strength of the caustics the water casts onto surrounding "
                                                "geometry. 0 = off, and no probe ray is traced at all (this is "
                                                "the perf switch: it costs one ray per pixel)." )
    RT_CVAR( rt_water_caustic_scale,    0.8f,   "caustic field frequency, UV per METRE (RTGL world space is "
                                                "metres; 1 map unit = 1/32 m). 0.8 tiles the field every "
                                                "~1.25 m = 40 map units. Higher = smaller, busier "
                                                "filaments." )
    RT_CVAR( rt_water_caustic_speed,    0.35f,  "caustic field scroll speed" )
    RT_CVAR( rt_water_caustic_dist,     192.f,  "how far below a surface the water may be and still light it, "
                                                "in map units (192 = 3 player heights). Larger reaches higher up "
                                                "walls but also lets water light things it should not." )
    // NOT archived: a diagnostic left at 1 in the ini would paint every water
    // surface magenta on every later launch, and this project has lost time to
    // exactly that class of stuck cvar.
    RT_CVAR_NOARCH( rt_water_debug,     false,  "diagnostic: paint water surfaces MAGENTA where the stylized "
                                                "branch runs, GREEN where RTGL flagged the surface as water but "
                                                "the stylized gate rejected it. No colour at all means the "
                                                "primitive never got RG_MESH_PRIMITIVE_WATER — i.e. the JSON "
                                                "meta never reached it (run tools/set_water_meta.py --apply)." )

    RT_CVAR( rt_bloom,                  true,   "enable bloom" )
    RT_CVAR( rt_bloom_scale,            1.f,    "multiplier for a calculated bloom" )
    RT_CVAR( rt_bloom_ev,               6.f,    "EV offset for bloom calculation input" )
    RT_CVAR( rt_bloom_threshold,        16.f,   "brightness threshold for bloom calculation input" )
    RT_CVAR( rt_bloom_dirt,             true,   "lens dirt enable" )
    RT_CVAR( rt_bloom_dirt_scale,       1.5f,   "lens dirt multiplier" )
    
    RT_CVAR( rt_ef_crt,                 false,  "CRT-monitor filter" )
    RT_CVAR( rt_ef_chraber,             0.15f,  "chromatic aberration intensity" )
    RT_CVAR( rt_ef_vhs,                 0.f,    "VHS filter intensity" )
    RT_CVAR( rt_ef_dither,              0.f,    "dithering filter intensity" )
    RT_CVAR( rt_ef_vintage,             0,      "[0, 7] vintage effects, disabled if rt_renderscale>0" ) // look RT_VINTAGE_* enum
    RT_CVAR( rt_ef_water,               true,   "warp screen while under water" )

    RT_CVAR( rt_pw_lightamp,            0,      "light amplification powerup type: 0 - night vision, 1 - thermal camera, 2 - flashlight" )

    RT_CVAR( rt_melt_duration,          1.5f,   "screen melt effect duration" )

    RT_CVAR( rt_wall_nomv,              1,      "0: motion vectors always,  1: use pegging flags to determine wall motion vectors,  2: always force no motion vectors on walls. "
                                                "This option is needed to fix illumination motion artifacts on lifts / crashers" )

    RT_CVAR( hack_initialframesskip,    true,   "skip initial a couple of frames on game launch; if not skipped, there might be a distracting flashing of the main window" )

    RT_CVAR( _rt_showexportable,        false,  "internal variable; only in debug" )

	// default, so when user launches a game with CRT/Vintage,
	// and after that changes to dlss/fsr2, then this value will be set to the cvars;
	// 2 = balanced; non-archived
    RT_CVAR( _rt_cachedpreset,          2,      "internal variable for menu UX" )

    bool rt_available_dlss2   = false;
    bool rt_available_dlss3fg = false;
    bool rt_available_fsr2    = false;
    bool rt_available_fsr3fg  = false;
    bool rt_available_dxgi    = false;

    const char* rt_failreason_dlss2   = nullptr;
    const char* rt_failreason_dlss3fg = nullptr;
    const char* rt_failreason_fsr2    = nullptr;
    const char* rt_failreason_fsr3fg  = nullptr;
    const char* rt_failreason_dxgi    = nullptr;

    bool rt_hdr_available = false;
    bool rt_fluid_available = false;

    bool rt_firststart = false;
}

RT_CVAR( rt_mod_compat, 3, "mod compatibility level bit mask: < bit 1: brightmap fallback | bit 0: general >" )

// clang-format on

EXTERN_CVAR( Float, blood_fade_scalar );
EXTERN_CVAR( Float, pickup_fade_scalar );

//
//
//
//
//
//

const char* g_rt_cutscenename        = nullptr;
bool        g_rt_showfirststartscene = false;
int         g_rt_skipinitframes      = -10; // to prevent flashing when starting the game
bool        g_rt_forcenofocuschange  = true;
int         rt_cullmode              = 2; // 0 -- balanced,  1 -- original gzdoom,  2 -- none

extern float RT_CutsceneTime();
extern void  RT_ForceIntroCutsceneMusicStop();

extern void RT_CloseLauncherWindow();

auto RT_MakeUpRightForwardVectors( const DRotator& rotation ) -> std::tuple< RgFloat3D, RgFloat3D, RgFloat3D >;

namespace
{

void RG_CHECK( RgResult r )
{
    assert( ( r ) == RG_RESULT_SUCCESS );
}

#define RG_TRANSFORM_IDENTITY              \
    {                                      \
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 \
    }

constexpr auto ORIGINAL_DOOM_RESOLUTION_HEIGHT = 200;
constexpr auto ONEGAMEUNIT_IN_METERS           = 1.0f / 32.0f; // https://doomwiki.org/wiki/Map_unit

constexpr auto RG_PACKED_COLOR_WHITE = RgColor4DPacked32{ 0xFFFFFFFF };


enum
{
    RT_VINTAGE_OFF,
    RT_VINTAGE_CRT,
    RT_VINTAGE_VHS,
    RT_VINTAGE_VHS_CRT,
    RT_VINTAGE_200,
    RT_VINTAGE_200_DITHER,
    RT_VINTAGE_480,
    RT_VINTAGE_480_DITHER,
};


constexpr uint64_t FlashlightLightId  = 0xFFFFFFF + 0;
constexpr uint64_t SunLightId         = 0xFFFFFFF + 1;
constexpr uint64_t MuzzleFlashLightId = 0xFFFFFFF + 2;
// NOT 0xFFFFFFF + n: SectorLightId_Base starts at +3 and runs to +sectorCount, so any
// small offset here would collide with a sector's light. Own decade, like the lattices.
// Bit 50, far above SoloLatticeId_Base's (1<<40) sector-derived range.
constexpr uint64_t GunGlowLightId     = 1ull << 50;
constexpr uint64_t SectorLightId_Base = 0xFFFFFFF + 3;
// Keep clear of sector-light IDs (base + sectorIndex).
constexpr uint64_t DynLightId_Base    = 0xA0000000ull;

// DLSS-RR: any transient-light source that wants a full temporal-history
// flush (flashlight on/off, a dynlight appearing/disappearing) sets this.
// Consumed (and cleared) once per frame at RgDrawFrameInfo.resetHistory,
// rate-limited by rt_rr_reset_min_ms via g_rt_lastresetat. Declared here
// (rather than beside the similar g_resetposteffects further down) because
// RT_AddFlashlight() is an inline method of a class nested in this same
// anonymous namespace and needs ordinary forward-visible lookup.
static bool   g_rt_lightcut    = false;
static double g_rt_lastresetat = -1e9;
// Which setter raised g_rt_lightcut, for rt_rr_reset_debug. Static string only.
static const char* g_rt_lightcut_why = "?";
// Self-emission threshold for rt_sector_emis, derived from THIS map's own lightlevel
// distribution rather than an absolute number. 180 means "glowing panel" in a dark
// corridor and "ordinary lit room" in a bright engineering deck, so a fixed global
// threshold cannot mean the right thing in both — it either misses the panels or makes
// every wall in the level a light. Recomputed on map change; starts closed (nothing
// emits) so a frame before the first update cannot flash the whole map bright.
static float g_sectorEmisThreshold = 255.f;

constexpr uint64_t CeilingLampId_Base = 0xB0000000ull;
constexpr uint64_t HangLampId_Base    = 0xC0000000ull;
constexpr uint64_t WallStripId_Base   = 0xD0000000ull;
constexpr uint64_t CeilingEdgeId_Base = 0xE0000000ull;
// Faux panel lattice lights. Own range because their IDs are derived from a sector index
// and a lattice cell rather than from a linedef, so they cannot share the edge encoding.
constexpr uint64_t FauxLatticeId_Base = 0xF0000000ull;
// Solo bulb lattice lights. NOT the next round number after FauxLatticeId_Base: that
// formula is secIndex<<20 + ..., and secIndex alone can push the faux range past 32 bits
// on a map with thousands of sectors, so "the next range" is not a safe assumption here.
// Bit 40 is comfortably above anything that formula can produce.
constexpr uint64_t SoloLatticeId_Base = 1ull << 40;
// Hell Knight fist lights. Bit 42, not "the next value after SoloLatticeId_Base": that
// base is 1<<40 and its IDs add secIndex<<20 + cell, which can climb well past 1<<40
// itself. Bit 42 clears the whole reachable solo range with room to spare. Two lights per
// actor, so the low bit of the id is the hand index.
constexpr uint64_t HandLightId_Base      = 1ull << 42;
// Torch / fire / candle lights. Bit 43, one clear bit above the fist range: hand IDs are
// HandLightId_Base + (ptr & 0xFFFFFFFF) << 1 + hand, which spans 33 bits above 1<<42 and
// so climbs into 1<<43 territory on its own... it does NOT, because 1<<42 + 2^33 < 1<<43,
// but the margin is only there by arithmetic. Flame IDs use the same ptr-derived low bits
// with NO shift, so they occupy 1<<43 + 32 bits and cannot reach 1<<44.
constexpr uint64_t FlameLightId_Base     = 1ull << 43;
// Real bulb-array lattice lights (SFLATAS/SFLATAQ). Bit 44, one clear bit above the flame
// range, and NOT sharing CeilingEdgeId_Base even though these come from the same walk and
// the same budget: that base's IDs are line-derived (line->Index() * 2 * segsPerLine + sg)
// while a lattice ID is secIndex<<20 + cell, so the two encodings would overlap
// unpredictably inside one range. Bit 41 is technically free but the solo range below it
// is secIndex-derived and its headroom is only there by arithmetic — see SoloLatticeId_Base.
constexpr uint64_t CeilingLatticeId_Base = 1ull << 44;
// Segments per line, so a line's light IDs never collide with the next line's.
constexpr uint64_t WallStripSegsPerLine = 16;
// Bounds the id packing in RT_UploadCeilingEdgeLamps: line->Index() * 2 * this stays well
// under the 0x02000000 offset the debug markers add, so the two can never collide.
constexpr uint64_t CeilingEdgeSegsPerLine = 32;



// Doom 64 stores room atmosphere as a per-sector colormap (MAP02's blue armor room
// is lightcolor 0x0050FF). That is a post-shade filter from the original renderer,
// not a light: it has no position and no falloff, and GZDoom bakes it into vertex
// color together with lightlevel. Handing that to the path tracer as albedo
// double-counts shading — it is what produced the yellow key-door neon wash and the
// black light-absorbing rooms that rt_mod_compat's force-white works around.
//
// So: throw away the baked shading, keep the art intent. Normalize to hue only (peak
// channel == 1, so this can never darken a surface the way raw lightcolor did), then
// lerp toward white by `strength`. Feed most of it to light color — then emissive
// blobs, bloom, speculars and GI bounce all inherit the tint physically — and only a
// little to albedo, as a fallback for sectors that have no analytic light at all.
static FVector3 RT_SectorHue( float r, float g, float b, float strength )
{
    const float peak = std::max( { r, g, b } );
    if( peak <= 0.001f )
    {
        return FVector3{ 1.0f, 1.0f, 1.0f };
    }

    const float s = std::clamp( strength, 0.0f, 1.0f );
    return FVector3{ 1.0f + ( r / peak - 1.0f ) * s,
                     1.0f + ( g / peak - 1.0f ) * s,
                     1.0f + ( b / peak - 1.0f ) * s };
}

static FVector3 RT_SectorHue( const PalEntry& lightcolor, float strength )
{
    return RT_SectorHue( lightcolor.r / 255.0f,
                         lightcolor.g / 255.0f,
                         lightcolor.b / 255.0f,
                         strength );
}

const char* RT_GetMapName()
{
    if( g_rt_cutscenename && g_rt_cutscenename[ 0 ] != '\0' )
    {
        return g_rt_cutscenename;
    }

    if( primaryLevel && !primaryLevel->RT_MapName.IsEmpty() )
    {
        // Official modcompat: RT_MapName is set in p_openmap for PWAD maps
        // so Doom II rt/scenes/map## do not collide with mod MAP01 etc.
        return primaryLevel->RT_MapName.GetChars();
    }

    if( g_rt_showfirststartscene )
    {
        // HACKHACK: do not show scene at the first frame: cutscene's firststart::draw is not called at that time :(
        static bool HACKHACK_firstframeskipped = false;
        if( !HACKHACK_firstframeskipped )
        {
            HACKHACK_firstframeskipped = true;
            return nullptr;
        }

        return "mainmenu";
    }

    return nullptr;
}

bool RT_ForceNoClassicMode()
{
    if( g_rt_cutscenename && g_rt_cutscenename[ 0 ] != '\0' )
    {
        return true;
    }
    if( g_rt_showfirststartscene )
    {
        return true;
    }
    return false;
}



constexpr auto RT_BIT( uint32_t b )
{
    return 1u << b;
}
enum rt_powerupflag_t
{
    RT_POWERUP_FLAG_BONUS_BIT          = RT_BIT( 1 ),
    RT_POWERUP_FLAG_BERSERK_BIT        = RT_BIT( 2 ),
    RT_POWERUP_FLAG_RADIATIONSUIT_BIT  = RT_BIT( 3 ),
    RT_POWERUP_FLAG_INVUNERABILITY_BIT = RT_BIT( 4 ),
    RT_POWERUP_FLAG_INVISIBILITY_BIT   = RT_BIT( 5 ),
    RT_POWERUP_FLAG_NIGHTVISION_BIT    = RT_BIT( 6 ),
    RT_POWERUP_FLAG_THERMALVISION_BIT  = RT_BIT( 7 ),
    RT_POWERUP_FLAG_FLASHLIGHT_BIT     = RT_BIT( 8 ),
};
uint32_t RT_CalcPowerupFlags();



constexpr float pi()
{
    return pi::pif();
}

constexpr float to_rad( float degrees )
{
    return degrees * ( pi() / 180.0f );
}

constexpr FVector3 gzvec3(const RgFloat3D &v)
{
    return { v.data[ 0 ], v.data[ 1 ], v.data[ 2 ] };
}

constexpr DVector3 gzvec3d(const RgFloat3D &v)
{
    return { v.data[ 0 ], v.data[ 1 ], v.data[ 2 ] };
}

template< typename T >
auto applygamma( T x ) = delete;
template<>
auto applygamma( float x )
{
    return std::clamp( x * x, 0.f, 1.f );
}
template<>
auto applygamma( uint8_t x )
{
    return static_cast< uint8_t >( applygamma( float( x ) / 255.f ) * 255.f );
}

auto rtcolor( const PalEntry& e ) -> RgColor4DPacked32
{
    return rt.rgUtilPackColorByte4D( e.r, e.g, e.b, e.a );
}

auto rtcolor( const FVector4PalEntry& e ) -> RgColor4DPacked32
{
    return rt.rgUtilPackColorFloat4D( e.r, e.g, e.b, e.a );
}

auto cvarcolor_to_rtcolor( const FColorCVarRef& cvarcolor ) -> RgColor4DPacked32
{
    uint32_t ba = *( cvarcolor );

    int r = RPART( ba );
    int g = GPART( ba );
    int b = BPART( ba );

    return rt.rgUtilPackColorByte4D( r, g, b, 255 );
}

float lightlevel_to_classic( bool isui, float lightlevel )
{
    if( isui )
    {
        return 1.0f;
    }

    if( lightlevel < 0.0f )
    {
        return 1.0f;
    }

    float lmin = std::max( float( cvar::rt_classic_llmin ), 0.0f );
    float lmax = std::min( float( cvar::rt_classic_llmax ), 1.0f );

    float lrange = std::max( lmax - lmin, 0.0f );
    if( lrange < 0.001f )
    {
        lmin   = 0.0f;
        lmax   = 1.0f;
        lrange = 1.0f;
    }

    float t01 = std::clamp( lightlevel, 0.0f, 1.0f );
    t01       = std::pow( t01, float( cvar::rt_classic_llpow ) );
    
    return lmin + t01 * lrange;
}

auto rtcolor_multiply( const FVector4PalEntry& e, const FVector4& b, bool forcealpha1 ) -> RgColor4DPacked32
{
    return rt.rgUtilPackColorFloat4D( e.r * b[ 0 ], //
                                      e.g * b[ 1 ],
                                      e.b * b[ 2 ],
                                      forcealpha1 ? 1.0f : e.a * b[ 3 ] );
}

auto rtcolor_bgr_alphagamma( const PalEntry& e ) -> RgColor4DPacked32
{
    return rt.rgUtilPackColorByte4D( e.b, e.g, e.r, applygamma( e.a ) );
}



class RTRenderState;

class RTFrameBuffer : public SystemBaseFrameBuffer
{
    using Super = SystemBaseFrameBuffer;

public:
    RTFrameBuffer( void* hMonitor, bool fullscreen );
    ~RTFrameBuffer() override;
    void InitializeState() override;
    void BeginFrame() override
    {
        SetViewportRects( nullptr );
        RT_BeginFrame();
        Super::BeginFrame();
    }
    void Update() override
    {
        this->Draw2D();
        twod->Clear();
        RT_DrawFrame();
        Super::Update();
    }
    void FirstEye() override;

    FRenderState*     RenderState() override;
    IVertexBuffer*    CreateVertexBuffer() override;
    IIndexBuffer*     CreateIndexBuffer() override;
    IDataBuffer*      CreateDataBuffer( int bindingpoint, bool ssbo, bool needsresize ) override;
    IHardwareTexture* CreateHardwareTexture( int numchannels ) override;

    void SetVSync( bool vsync ) override { m_vsync = vsync; }
    void SetTextureFilterMode() override {}
    void SetLevelMesh( hwrenderer::LevelMesh* mesh ) override {}

    void Draw2D() override;

    // RTGL1 has no frame-readback API; grab the presented HWND contents.
    TArray< uint8_t > GetScreenshotBuffer( int& pitch, ESSType& color_type, float& gamma ) override;

public:
    void RT_MarkWasSky() { m_wassky = true; }

private:
    void RT_BeginFrame();
    void RT_DrawFrame();

private:
    RTRenderState* m_state{ nullptr };
    bool           m_vsync{ false };
    bool           m_wassky{ false };
};



class VectorAsBuffer : virtual public IBuffer
{
public:
    ~VectorAsBuffer() override = default;

    void SetSubData( size_t offset, size_t size, const void* data ) override
    {
        if( offset + size > m_buffer.size() )
        {
            m_buffer.resize( offset + size );
        }

        if( data )
        {
            memcpy( &m_buffer[ offset ], data, size );
        }

        buffersize = m_buffer.size();
        if( map )
        {
            map = m_buffer.data();
        }
    }
    void SetData( size_t size, const void* data, BufferUsageType type ) override
    {
        SetSubData( 0, size, data );
    }
    void* Lock( unsigned size ) override
    {
        SetSubData( 0, size, nullptr );
        return m_buffer.data();
    }
    void Unlock() override {}
    void Resize( size_t newsize ) override { m_buffer.resize( newsize ); }
    void Upload( size_t start, size_t size ) override {}
    void Map() override { map = m_buffer.data(); }
    void Unmap() override { map = nullptr; }
    void GPUDropSync() override {}
    void GPUWaitSync() override {}

protected:
    auto AccessBuffer() const { return std::span{ m_buffer }; }

private:
    std::vector< uint8_t > m_buffer;
};

class RTVertexBuffer
    : public IVertexBuffer
    , public VectorAsBuffer
{
    using Super            = VectorAsBuffer;
    using VertexTypeHolder = std::
        variant< std::monostate, FSkyVertex, FModelVertex, FFlatVertex, F2DDrawer::TwoDVertex >;

public:
    void SetFormat( int                           numBindingPoints,
                    int                           numAttributes,
                    size_t                        stride,
                    const FVertexBufferAttribute* attrs ) override
    {
        static_assert( sizeof( FSkyVertex ) != sizeof( FModelVertex ) );
        static_assert( sizeof( FSkyVertex ) != sizeof( FFlatVertex ) );
        static_assert( sizeof( FSkyVertex ) != sizeof( F2DDrawer::TwoDVertex ) );
        static_assert( sizeof( FModelVertex ) != sizeof( FFlatVertex ) );
        static_assert( sizeof( FModelVertex ) != sizeof( F2DDrawer::TwoDVertex ) );
        static_assert( sizeof( FFlatVertex ) != sizeof( F2DDrawer::TwoDVertex ) );

        if( numBindingPoints == 1 && numAttributes == 4 && stride == sizeof( FSkyVertex ) )
        {
            m_vertextype = FSkyVertex{};
        }
        else if( numBindingPoints == 2 && numAttributes == 8 && stride == sizeof( FModelVertex ) )
        {
            m_vertextype = FModelVertex{};
        }
        else if( numBindingPoints == 1 && numAttributes == 3 && stride == sizeof( FFlatVertex ) )
        {
            m_vertextype = FFlatVertex{};
        }
        else if( numBindingPoints == 1 && numAttributes == 3 &&
                 stride == sizeof( F2DDrawer::TwoDVertex ) )
        {
            m_vertextype = F2DDrawer::TwoDVertex{};
        }
        else
        {
            assert( 0 );
            m_vertextype = std::monostate{};
        }
        m_formatted.clear();
    }

    static void MakeFormatted( std::vector< RgPrimitiveVertex >& dst,
                               size_t                            targetCount,
                               std::span< const uint8_t >        srcbuf,
                               const VertexTypeHolder&           vertextype )
    {
        // TODO: mStreamData.uVertexColor for lightstyled?


        static auto gz_unpacknormal_x = []( uint32_t packedNormal ) -> float {
            int inx = ( packedNormal & 1023 );
            return float( inx ) / 512.0f;
        };
        static auto gz_unpacknormal_y = []( uint32_t packedNormal ) -> float {
            int iny = ( ( packedNormal >> 10 ) & 1023 );
            return float( iny ) / 512.0f;
        };
        static auto gz_unpacknormal_z = []( uint32_t packedNormal ) -> float {
            int inz = ( ( packedNormal >> 20 ) & 1023 );
            return float( inz ) / 512.0f;
        };

        static auto rg_packednormal_fallback = rt.rgUtilPackNormal( 0, 1, 0 );

        // make by type
        std::visit(
            [ & ]< typename T >( const T& ) {
                assert( srcbuf.size_bytes() % sizeof( T ) == 0 );

                dst.reserve( targetCount );
                for( size_t i = dst.size(); i < targetCount; i++ )
                {
                    static_assert( sizeof( decltype( srcbuf )::value_type ) == 1 );
                    const auto* ptr = &srcbuf[ i * sizeof( T ) ];

                    if constexpr( std::is_same_v< T, FSkyVertex > )
                    {
                        auto src = reinterpret_cast< const FSkyVertex* >( ptr );

                        dst.push_back( RgPrimitiveVertex{
                            .position     = { src->x * ONEGAMEUNIT_IN_METERS,
                                              src->y * ONEGAMEUNIT_IN_METERS,
                                              src->z * ONEGAMEUNIT_IN_METERS },
                            .normalPacked = rg_packednormal_fallback,
                            .texCoord     = { src->u, src->v },
                            .color        = rtcolor( src->color ),
                        } );
                    }
                    else if constexpr( std::is_same_v< T, FModelVertex > )
                    {
                        auto src = reinterpret_cast< const FModelVertex* >( ptr );

                        dst.push_back( RgPrimitiveVertex{
                            .position = { src->x * ONEGAMEUNIT_IN_METERS,
                                          src->y * ONEGAMEUNIT_IN_METERS,
                                          src->z * ONEGAMEUNIT_IN_METERS },
                            .normalPacked =
                                rt.rgUtilPackNormal( gz_unpacknormal_x( src->packedNormal ),
                                                     gz_unpacknormal_y( src->packedNormal ),
                                                     gz_unpacknormal_z( src->packedNormal ) ),
                            .texCoord = { src->u, src->v },
                            .color    = RG_PACKED_COLOR_WHITE,
                        } );
                    }
                    else if constexpr( std::is_same_v< T, FFlatVertex > )
                    {
                        auto src = reinterpret_cast< const FFlatVertex* >( ptr );

                        dst.push_back( RgPrimitiveVertex{
                            .position     = { src->x * ONEGAMEUNIT_IN_METERS,
                                              src->y * ONEGAMEUNIT_IN_METERS,
                                              src->z * ONEGAMEUNIT_IN_METERS },
                            .normalPacked = rg_packednormal_fallback,
                            .texCoord     = { src->u, src->v },
                            .color        = RG_PACKED_COLOR_WHITE,
                        } );
                    }
                    else if constexpr( std::is_same_v< T, F2DDrawer::TwoDVertex > )
                    {
                        auto src = reinterpret_cast< const F2DDrawer::TwoDVertex* >( ptr );

                        dst.push_back( RgPrimitiveVertex{
                            .position     = { src->x, src->y, src->z },
                            .normalPacked = rg_packednormal_fallback,
                            .texCoord     = { src->u, src->v },
                            .color        = rtcolor_bgr_alphagamma( src->color0 ),
                        } );
                    }
                    else
                    {
                        assert( 0 );
                    }
                }
            },
            vertextype );
    }

    auto AccessFormatted( uint32_t first, uint32_t count ) -> std::span< const RgPrimitiveVertex >
    {
        if( std::holds_alternative< std::monostate >( m_vertextype ) )
        {
            return {};
        }

        if( first + count > m_formatted.size() )
        {
            MakeFormatted( m_formatted, first + count, AccessBuffer(), m_vertextype );
        }

        assert( first + count <= m_formatted.size() );

        return std::span{
            &m_formatted[ first ],
            count,
        };
    }

    void SetData( size_t size, const void* data, BufferUsageType type ) override
    {
        m_formatted.clear();
        Super::SetData( size, data, type );
    }

    void SetSubData( size_t offset, size_t size, const void* data ) override
    {
        m_formatted.clear();
        Super::SetSubData( offset, size, data );
    }

    void Unmap() override
    {
        m_formatted.clear();
        Super::Unmap();
    }

    bool IsSky() const { return std::holds_alternative< FSkyVertex >( m_vertextype ); }
    bool IsUI() const { return std::holds_alternative< F2DDrawer::TwoDVertex >( m_vertextype ); }

private:
    VertexTypeHolder m_vertextype;

    std::vector< RgPrimitiveVertex > m_formatted;
};

class RTIndexBuffer
    : public IIndexBuffer
    , public VectorAsBuffer
{
    using IndexType = uint32_t;

public:
    auto AccessFormatted( uint32_t first, uint32_t count )
    {
        const auto rawbuf = AccessBuffer();
        // loose type check
        assert( rawbuf.size_bytes() % sizeof( IndexType ) == 0 );
        // alignment
        assert( uint64_t( rawbuf.data() ) % sizeof( IndexType ) == 0 );
        // overflow
        assert( sizeof( IndexType ) * ( first + count ) <= rawbuf.size_bytes() );

        return std::span{
            reinterpret_cast< const IndexType* >( rawbuf.data() ) + first,
            count,
        };
    }

    static auto CalcFirstVertexAndVertexCount( std::span< const IndexType > indices )
    {
        uint32_t imin = std::numeric_limits< uint32_t >::max();
        uint32_t imax = std::numeric_limits< uint32_t >::lowest();
        for( const auto& i : indices )
        {
            imin = std::min( imin, i );
            imax = std::max( imax, i );
        }
        return std::pair{
            imax > imin ? imin : 0,
            imax > imin ? imax - imin + 1 : 0,
        };
    }

    auto MakeWithNewFirstIndex( std::span< const IndexType > indices, IndexType newFirst )
    {
        m_cache.clear();
        m_cache.reserve( indices.size() );

        for( const auto& i : indices )
        {
            assert( i >= newFirst );
            m_cache.push_back( i - newFirst );
        }

        return m_cache;
    }

private:
    std::vector< IndexType > m_cache;
};



class RTHardwareTexture : public IHardwareTexture
{
public:
    // Empty, as it's only used for software renderer
    uint32_t CreateTexture( uint8_t*, int, int, int, bool, const char* ) override { return 0; }
    void     AllocateBuffer( int, int, int ) override {}
    uint8_t* MapBuffer() override { return nullptr; }

    void CreateIfWasnt( FGameTexture&       src,
                        int                 clampmode,
                        int                 translation,
                        int                 flags,
                        const FRenderStyle& renderStyle )
    {
        auto rtclamp_x = []( int clampmode ) {
            switch( clampmode )
            {
                case CLAMP_X:
                case CLAMP_XY:
                case CLAMP_XY_NOMIP:
                case CLAMP_NOFILTER_X:
                case CLAMP_NOFILTER_XY:
                case CLAMP_CAMTEX: return RG_SAMPLER_ADDRESS_MODE_CLAMP;
                default: return RG_SAMPLER_ADDRESS_MODE_REPEAT;
            }
        };
        auto rtclamp_y = []( int clampmode ) {
            switch( clampmode )
            {
                case CLAMP_Y:
                case CLAMP_XY:
                case CLAMP_XY_NOMIP:
                case CLAMP_NOFILTER_Y:
                case CLAMP_NOFILTER_XY:
                case CLAMP_CAMTEX: return RG_SAMPLER_ADDRESS_MODE_CLAMP;
                default: return RG_SAMPLER_ADDRESS_MODE_REPEAT;
            }
        };
        auto desaturateIfNeed = []( FTextureBuffer& data, int flags, const char* lumpname ) {
            // special case for the SmallFont...
            const bool isSTCFNFont = !( flags & CTF_Indexed ) && lumpname &&
                                     strlen( lumpname ) == 8 &&
                                     strncmp( lumpname, "STCFN", 5 ) == 0;
            if( isSTCFNFont )
            {
                for( int i = 0; i < data.mWidth; i++ )
                {
                    for( int j = 0; j < data.mHeight; j++ )
                    {
                        uint8_t* pix =
                            &data.mBuffer[ 4 *
                                           ( i * static_cast< uint64_t >( data.mHeight ) + j ) ];
                        const uint8_t gray = std::max( pix[ 0 ], std::max( pix[ 1 ], pix[ 2 ] ) );
                        pix[ 0 ] = pix[ 1 ] = pix[ 2 ] = gray;
                    }
                }
            }
        };
        auto calculateAlphaIfNeed = []( FTextureBuffer& data, bool redIsAlpha ) {
            if( redIsAlpha )
            {
                for( int i = 0; i < data.mWidth; i++ )
                {
                    for( int j = 0; j < data.mHeight; j++ )
                    {
                        uint8_t* pix =
                            &data.mBuffer[ 4 *
                                           ( i * static_cast< uint64_t >( data.mHeight ) + j ) ];

                        // alpha = red
                        pix[ 3 ] = pix[ 0 ];
                    }
                }
            }
        };
        // Doom64-RT: RGBA PNGs are always Masked. Soft garbage alpha hole-punches solid
        // walls; real fences have many low-alpha pixels. Heuristic: if <8% of pixels are
        // "see-through" (A<32), force opaque; otherwise keep alpha for fences/grates.
        auto forceOpaqueAlphaIfNeed = []( FTextureBuffer& data, bool forceOpaque ) {
            if( !forceOpaque || !data.mBuffer || data.mWidth <= 0 || data.mHeight <= 0 )
            {
                return;
            }
            const size_t n =
                size_t( data.mWidth ) * size_t( data.mHeight );
            for( size_t i = 0; i < n; i++ )
            {
                data.mBuffer[ 4 * i + 3 ] = 255;
            }
        };
        auto looksLikeRealMask = []( FTextureBuffer& data ) -> bool {
            if( !data.mBuffer || data.mWidth <= 0 || data.mHeight <= 0 )
            {
                return false;
            }
            const size_t n = size_t( data.mWidth ) * size_t( data.mHeight );
            size_t       holes = 0;
            for( size_t i = 0; i < n; i++ )
            {
                if( data.mBuffer[ 4 * i + 3 ] < 32 )
                {
                    holes++;
                }
            }
            return holes * 100 >= n * 8; // >= 8% transparent-ish pixels
        };

        if( m_created )
        {
            return;
        }

        m_created = true;
        m_name    = MakeTextureName( src );

        if( m_name.empty() || !src.GetTexture() )
        {
            assert( 0 );
            return;
        }

        auto texbuffer = src.GetTexture()->CreateTexBuffer( translation, flags | CTF_ProcessData );
        desaturateIfNeed( texbuffer, flags, fileSystem.GetFileShortName( src.GetSourceLump() ) );
        calculateAlphaIfNeed( texbuffer, renderStyle.Flags & STYLEF_RedIsAlpha );
        {
            const auto use = src.GetUseType();
            const bool keepAlpha = ( renderStyle.Flags & STYLEF_RedIsAlpha ) ||
                                   use == ETextureType::Sprite ||
                                   use == ETextureType::FontChar ||
                                   use == ETextureType::SkinSprite ||
                                   looksLikeRealMask( texbuffer );
            forceOpaqueAlphaIfNeed( texbuffer, rt_mod_compat != 0 && !keepAlpha );
        }

        if( texbuffer.mWidth <= 0 || texbuffer.mHeight <= 0 )
        {
            assert( 0 );
            return;
        }

        const bool exportseparately = m_name.starts_with( "vx_" );

        auto details = RgOriginalTextureDetailsEXT{
            .sType  = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_DETAILS_EXT,
            .pNext  = nullptr,
            .flags  = exportseparately ? RG_ORIGINAL_TEXTURE_INFO_FORCE_EXPORT_AS_EXTERNAL : 0u,
            .format = flags & CTF_Indexed ? RG_FORMAT_R8_SRGB : RG_FORMAT_B8G8R8A8_SRGB,
        };

        auto info = RgOriginalTextureInfo{
            .sType        = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO,
            .pNext        = &details,
            .pTextureName = m_name.c_str(),
            .pPixels      = texbuffer.mBuffer,
            .size         = { static_cast< uint32_t >( texbuffer.mWidth ),
                              static_cast< uint32_t >( texbuffer.mHeight ) },
            .filter       = RG_SAMPLER_FILTER_AUTO,
            .addressModeU = RG_SAMPLER_ADDRESS_MODE_REPEAT, //  rtclamp_x( clampmode ),
            .addressModeV = RG_SAMPLER_ADDRESS_MODE_REPEAT, //  rtclamp_y( clampmode ),
        };

        RgResult r = rt.rgProvideOriginalTexture( &info );
        RG_CHECK( r );
    }

    ~RTHardwareTexture() override
    {
// HACKHACK: TODO: why this is being called only on Release? (and destroying actually used textures)
#if 0
        RgResult r = rt.rgMarkOriginalTextureAsDeleted( m_name.c_str() );
        RG_CHECK( r );
#endif
    }

    auto GetRTName() const -> const char*
    {
        return m_created && !m_name.empty() ? m_name.c_str() : nullptr;
    }

private:
    static auto MakeTextureName( FGameTexture& fgametex ) -> std::string
    {
        // highest priority: FGameTexture name
        if( !fgametex.GetName().IsEmpty() )
        {
            return fgametex.GetName().GetChars();
        }

        // if no lump name, stringify the image ID;
        // this is undesirable for textures that require a replacement
        // (which are found by texname; and because ID is assigned at runtime,
        // replacements can't be found correctly)
        if( FTexture* ftex = fgametex.GetTexture() )
        {
            if( FImageSource* imgsrc = ftex->GetImage() )
            {
                // MSVC's std::string has 16 chars inlined,
                // so no allocation should happen
                return std::to_string( imgsrc->GetId() );
            }
        }

        assert( 0 );
        return {};
    }

private:
    bool        m_created{ false };
    std::string m_name{};
};


template< typename T >
void ApplyMat33ToVec3_row( const T row_mat[ 3 ][ 3 ], float ( &v )[ 3 ] )
{
    RgFloat3D r;
    for( int i = 0; i < 3; i++ )
    {
        r.data[ i ] = row_mat[ i ][ 0 ] * T( v[ 0 ] ) + row_mat[ i ][ 1 ] * T( v[ 1 ] ) +
                      row_mat[ i ][ 2 ] * T( v[ 2 ] );
    }
    v[ 0 ] = r.data[ 0 ];
    v[ 1 ] = r.data[ 1 ];
    v[ 2 ] = r.data[ 2 ];
}

template< typename T >
RgFloat4D ApplyMat44ToVec4( const T column_mat[ 4 ][ 4 ], const RgFloat4D& vs )
{
    const auto* v = vs.data;
    RgFloat4D   r;
    for( int i = 0; i < 4; i++ )
    {
        r.data[ i ] = column_mat[ 0 ][ i ] * T( v[ 0 ] ) + column_mat[ 1 ][ i ] * T( v[ 1 ] ) +
                      column_mat[ 2 ][ i ] * T( v[ 2 ] ) + column_mat[ 3 ][ i ] * T( v[ 3 ] );
    }
    return r;
}

template< typename T >
RgFloat4D ApplyMat44ToVec4( const T* column_mat, const RgFloat4D& vs )
{
    return ApplyMat44ToVec4< T >( reinterpret_cast< const T( * )[ 4 ] >( column_mat ), vs );
}

RgFloat3D FromHomogeneous( const RgFloat4D& v )
{
    return RgFloat3D{ v.data[ 0 ] / v.data[ 3 ],
                      v.data[ 1 ] / v.data[ 3 ],
                      v.data[ 2 ] / v.data[ 3 ] };
}

class RTRenderState : public FRenderState
{
public:
    explicit RTRenderState( RTFrameBuffer* parent ) : m_fb( parent ) {}
    virtual ~RTRenderState() = default;

    void RT_BeginFrame()
    {
        rtstate.reset();
        m_weaponDrawCallIndex = 0;
    }

    bool IsCurrentDrawIgnored() const
    {
        return rtstate.is< RtPrim::Ignored >() || mTextureMode == TM_FOGLAYER;
    }

    // Retribution's two soft-blend monsters — both DECORATE RenderStyle Translucent
    // (64Spectre at a pulsing alpha, 64NightmareImp at a flat 0.60), so both land in the
    // same RTGL1 hole: below MESH_TRANSLUCENT_ALPHA_THRESHOLD they are rasterized rather
    // than traced, and the rasterizer lights nothing. Keyed off the sprite prefix —
    // there is no actor pointer down here. n[4] is the animation frame letter
    // (rt_state.h: 'A' + animframe).
    enum class GhostActor
    {
        None,
        Spectre,       // SAR2 — living A..H, corpse I..N
        NightmareImp,  // TRO2 — living A..K, corpse/gib L..X
    };

    auto GhostSprite( bool* outIsCorpse = nullptr ) const -> GhostActor
    {
        if( outIsCorpse )
        {
            *outIsCorpse = false;
        }
        if( !rt_mod_compat || !rtstate.is< RtPrim::ExportInstance >() )
        {
            return GhostActor::None;
        }
        const char* n = rtstate.get_exportinstance_name();
        if( !n || !n[ 0 ] || !n[ 4 ] )
        {
            return GhostActor::None;
        }
        // SARG is the regular pinky and TROO the regular imp — neither is soft-blend.
        if( n[ 0 ] == 'S' && n[ 1 ] == 'A' && n[ 2 ] == 'R' && n[ 3 ] == '2' )
        {
            if( outIsCorpse )
            {
                *outIsCorpse = ( n[ 4 ] >= 'I' );
            }
            return GhostActor::Spectre;
        }
        if( n[ 0 ] == 'T' && n[ 1 ] == 'R' && n[ 2 ] == 'O' && n[ 3 ] == '2' )
        {
            if( outIsCorpse )
            {
                *outIsCorpse = ( n[ 4 ] >= 'L' );
            }
            return GhostActor::NightmareImp;
        }
        return GhostActor::None;
    }

    // Should this sprite be an ordinary solid, path-traced, lit sprite? Alive and dead
    // are separate cvars because the corpse fix landed first and is settled.
    bool IsSolidGhost() const
    {
        bool corpse = false;
        if( GhostSprite( &corpse ) == GhostActor::None )
        {
            return false;
        }
        return corpse ? bool( cvar::rt_spectre_corpse_solid ) : bool( cvar::rt_ghost_solid );
    }

    // Multiplier for a LIVING ghost's vertex alpha, so the body fades out in a dark room
    // while its eyes do not. See rt_ghost_lightscale for why alpha (and not colour) is
    // the channel that separates the two: RasterizerPipelines.cpp blends attachment 0
    // (body) with SRC_ALPHA and attachment 1 (outScreenEmission / the _e eye mask) with
    // ONE,ONE — the emission output never sees alpha at all.
    float GhostLightScale() const
    {
        bool corpse = false;
        if( GhostSprite( &corpse ) == GhostActor::None || corpse )
        {
            return 1.f;
        }

        const float amount = std::clamp( float( cvar::rt_ghost_lightscale ), 0.f, 1.f );
        if( amount <= 0.f )
        {
            return 1.f;
        }

        // m_lightlevel is the sprite-only field (hw_sprites.cpp sets it from
        // actor->Sector->GetSpriteLight(), defaulting to 255). NOT m_sectorLightLevel —
        // that one is only ever pushed for walls and flats, so on a sprite it is stale
        // and would have read as a permanently pitch-black room.
        const float ll = std::clamp( float( rtstate.m_lightlevel ) / 255.f, 0.f, 1.f );

        // sqrt, not linear: Doom lightlevels read far brighter than their numeric value,
        // so a linear curve crushes an ordinary dim-but-lit corridor down to nearly
        // invisible. Only genuinely unlit rooms should erase the body.
        const float lit = std::sqrt( ll );

        return 1.f - amount * ( 1.f - lit );
    }

    // A soft-blend monster that is alive and is NOT being forced solid. These must be
    // rasterized TRANSLUCENT overlays, never ALPHA_TESTED cutouts — see makePrimFlags.
    bool IsLivingGhost() const
    {
        bool corpse = false;
        if( GhostSprite( &corpse ) == GhostActor::None || corpse )
        {
            return false;
        }
        return !IsSolidGhost();
    }

    bool IsSpectre() const
    {
        switch( mRenderStyle.BlendOp )
        {
            case STYLEOP_Fuzz:
            case STYLEOP_FuzzOrAdd:
            case STYLEOP_FuzzOrSub:
            case STYLEOP_FuzzOrRevSub:
            case STYLEOP_Shadow: return true;
            default: break;
        }
        // Retribution 64Spectre is STYLE_Translucent + SAR2, not classic Fuzz.
        // Uses rasterized TRANSLUCENT + minalpha cap for see-through ghostly look —
        // unless it is being rendered solid, in which case it must NOT carry
        // RG_MESH_PRIMITIVE_TRANSLUCENT, because that flag forces rasterization on its
        // own regardless of alpha (VulkanDevice.cpp IsRasterized).
        if( IsSolidGhost() )
        {
            return false;
        }
        if( rt_mod_compat && rtstate.is< RtPrim::ExportInstance >() )
        {
            const char* n = rtstate.get_exportinstance_name();
            if( n && n[ 0 ] == 'S' && n[ 1 ] == 'A' && n[ 2 ] == 'R' &&
                n[ 3 ] == '2' )  // SAR2 = 64Spectre sprite prefix
            {
                // n[4] is the animation frame letter (rt_state.h: 'A' + animframe).
                // SAR2 I..N are the death frames — the WAD stores them as I0..N0, and
                // the DECORATE Death sequence fades in to A_SetTranslucent(1.0), so the
                // corpse is authored as a SOLID body, not a ghost.
                //
                // Keeping the spectre treatment on them is what made a dead spectre take
                // no light: spectres are flagged RG_MESH_PRIMITIVE_TRANSLUCENT, RTGL1
                // rasterizes any translucent primitive instead of tracing it
                // (VulkanDevice.cpp IsRasterized), and the rasterizer shader
                // (RsWorld.inl) outputs vertexColor * texture with no lighting term
                // whatsoever. So the corpse received light from nothing — not the
                // flashlight, not a lamp, not the sun — and sat at full texture
                // brightness on a dark floor. Dropping it out of IsSpectre() makes it an
                // ordinary alpha-tested sprite: alpha 1.0 clears
                // MESH_TRANSLUCENT_ALPHA_THRESHOLD, it enters the BLAS, and it is lit
                // and casts a shadow like every other corpse (2026-08-08).
                if( cvar::rt_spectre_corpse_solid && n[ 4 ] >= 'I' && n[ 4 ] <= 'N' )
                {
                    return false;
                }
                return true;
            }
        }
        return false;
    }

    void Draw( int dt, int index, int count, bool apply = true ) override
    {
        if( IsCurrentDrawIgnored() )
        {
            return;
        }

        assert( count > 0 );

        const uint32_t* pIndices   = nullptr;
        uint32_t        indexCount = 0;

        bool islines = false;

        switch( dt )
        {
            case DT_Points: assert( 0 ); return;
            case DT_Lines: islines = true; break;
            case DT_Triangles:
                // indices are sequential, just use vertex array
                break;
            case DT_TriangleFan:
                rt.rgUtilScratchGetIndices(
                    RG_UTIL_IM_SCRATCH_TOPOLOGY_TRIANGLE_FAN, count, &pIndices, &indexCount );
                break;
            case DT_TriangleStrip:
                rt.rgUtilScratchGetIndices(
                    RG_UTIL_IM_SCRATCH_TOPOLOGY_TRIANGLE_STRIP, count, &pIndices, &indexCount );
                break;
            default: break;
        }

        auto vb = static_cast< RTVertexBuffer* >( mVertexBuffer );
        if( !vb )
        {
            assert( 0 );
            return;
        }
        assert( rtstate.is< RtPrim::Sky >() == vb->IsSky() );

        InternalDraw( vb->AccessFormatted( mVertexOffsets[ 0 ] + index, count ),
                      std::span{ pIndices, indexCount },
                      vb->IsUI(),
                      islines );
    }

    void DrawIndexed( int dt, int index, int count, bool apply = true ) override
    {
        if( IsCurrentDrawIgnored() )
        {
            return;
        }

        assert( dt == DT_Triangles );
        if( count <= 0 )
        {
            // E3M2 fails
            return;
        }

        auto vb = static_cast< RTVertexBuffer* >( mVertexBuffer );
        if( !vb )
        {
            assert( 0 );
            return;
        }
        assert( rtstate.is< RtPrim::Sky >() == vb->IsSky() );

        auto ib = static_cast< RTIndexBuffer* >( mIndexBuffer );
        if( !ib )
        {
            assert( 0 );
            return;
        }

        auto indices = ib->AccessFormatted( index, count );

        auto [ vertFirst, vertCount ] = RTIndexBuffer::CalcFirstVertexAndVertexCount( indices );

        InternalDraw( vb->AccessFormatted( mVertexOffsets[ 0 ] + vertFirst, vertCount ),
                      ib->MakeWithNewFirstIndex( indices, vertFirst ),
                      vb->IsUI() );
    }

    void ClearScreen() override {}
    bool SetDepthClamp( bool on ) override { return on; }
    void SetDepthMask( bool on ) override {}
    void SetDepthFunc( int func ) override {}
    void SetDepthRange( float min, float max ) override {}
    void SetColorMask( bool r, bool g, bool b, bool a ) override {}
    void SetStencil( int offs, int op, int flags = -1 ) override {}
    void SetCulling( int mode ) override {}
    void EnableClipDistance( int num, bool state ) override {}
    void Clear( int targets ) override {}
    void EnableStencil( bool on ) override {}
    void SetScissor( int x, int y, int w, int h ) override {}
    void SetViewport( int x, int y, int w, int h ) override
    {
        m_viewport = RgViewport{
            .x        = float( x ),
            .y        = float( y ),
            .width    = float( w ),
            .height   = float( h ),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
    }
    void EnableDepthTest( bool on ) override {}
    void EnableMultisampling( bool on ) override {}
    void EnableLineSmooth( bool on ) override {}
    void EnableDrawBuffers( int count, bool apply ) override {}

private:
    static bool IsPerspectiveMatrix( const float* m );
    static bool IsLikeIdentity( const float* m );
    static bool IsLikeIdentity( const double* m );

    // If need to calculate a transform at the sprite's bottom.
    bool RequiresTrueTransform() const
    {
        if( rtstate.is< RtPrim::ExportInstance >() )
        {
            // need to make a true one, since gzdoom doesn't provide a world transform
            return !mModelMatrixEnabled;
        }
        return false;
    }

    auto CalculateTrueTransformAndItsVerts( std::span< const RgPrimitiveVertex > originalVerts )
        -> std::pair< RgTransform, std::span< const RgPrimitiveVertex > >
    {
        assert( RequiresTrueTransform() );
        assert( originalVerts.size() == 4 ); // to find a non-sprite without model matrix
        assert( !mModelMatrixEnabled );      // means that vert positions are in a metric space

        // need to offset a bit, to prevent clipping with floor (for glass spectres)
        constexpr float CLIP_FIX_OFFSET = 0.005f;

        const float pivot[] = {
            rtstate.m_lastthingposition.X * ONEGAMEUNIT_IN_METERS,
            rtstate.m_lastthingposition.Y * ONEGAMEUNIT_IN_METERS,
            rtstate.m_lastthingposition.Z * ONEGAMEUNIT_IN_METERS + CLIP_FIX_OFFSET,
        };

        m_tempverts.clear();
        m_tempverts.assign( originalVerts.begin(), originalVerts.end() );

        // make relative to pivot
        for( uint32_t v = 0; v < originalVerts.size(); v++ )
        {
            m_tempverts[ v ].position[ 0 ] -= pivot[ 0 ];
            m_tempverts[ v ].position[ 1 ] -= pivot[ 1 ];
            m_tempverts[ v ].position[ 2 ] -= pivot[ 2 ];
        }

        // un-rotate the angle
        const auto [ pitch, yaw ] = rtstate.get_spriterotation();
        
#if 0 // reference
        Matrix3x4 m;
        m.MakeIdentity();
        m.Rotate( 0, 0, 1, to_deg( yaw ) );
        m.Rotate( 0, 1, 0, to_deg( pitch ) );
#else
        const float cos_pitch = std::cos( pitch );
        const float sin_pitch = std::sin( pitch );
        const float cos_yaw   = std::cos( yaw );
        const float sin_yaw   = std::sin( yaw );

        //     |  cos_pitch, 0, sin_pitch |   | cos_yaw, -sin_yaw, 0 |
        // m = |          0, 1,         0 | x | sin_yaw,  cos_yaw, 0 |
        //     | -sin_pitch, 0, cos_pitch |   |       0,       0,  1 |

        float m[ 3 ][ 3 ] = {
            { cos_yaw * cos_pitch, -sin_yaw, cos_yaw * sin_pitch },
            { sin_yaw * cos_pitch, cos_yaw, sin_yaw * sin_pitch },
            { -sin_pitch, 0, cos_pitch },
        };
#endif
        const float m_inv[ 3 ][ 3 ] = {
            { m[ 0 ][ 0 ], m[ 1 ][ 0 ], m[ 2 ][ 0 ] },
            { m[ 0 ][ 1 ], m[ 1 ][ 1 ], m[ 2 ][ 1 ] },
            { m[ 0 ][ 2 ], m[ 1 ][ 2 ], m[ 2 ][ 2 ] },
        };
        for( auto& v : m_tempverts )
        {
            ApplyMat33ToVec3_row( m_inv, v.position );
        }

        return {
            RgTransform{ {
                { m[ 0 ][ 0 ], m[ 0 ][ 1 ], m[ 0 ][ 2 ], pivot[ 0 ] },
                { m[ 1 ][ 0 ], m[ 1 ][ 1 ], m[ 1 ][ 2 ], pivot[ 1 ] },
                { m[ 2 ][ 0 ], m[ 2 ][ 1 ], m[ 2 ][ 2 ], pivot[ 2 ] },
            } },
            std::span{ m_tempverts },
        };
    }

    auto MakeTransform( bool isSky ) const -> RgTransform
    {
        assert( !RequiresTrueTransform() );

        // also converts to metric
        auto fromGzMatrix = []( const float* m ) {
            return RgTransform{ {
                { m[ 0 ], m[ 4 ], m[ 8 ], m[ 12 ] * ONEGAMEUNIT_IN_METERS },
                { m[ 1 ], m[ 5 ], m[ 9 ], m[ 13 ] * ONEGAMEUNIT_IN_METERS },
                { m[ 2 ], m[ 6 ], m[ 10 ], m[ 14 ] * ONEGAMEUNIT_IN_METERS },
            } };
        };

        // sky has view matrix that is different from main camera, apply it
        if( isSky )
        {
            auto l_unit = []( float f ) {
                return f > +0.5f   ? +1.0f //
                       : f < -0.5f ? -1.0f //
                                   : 0.0f;
            };

            auto skyToMainCameraIrregular =
                VSMatrix::smultMatrix( m_mainCameraView_Inverse, m_view );

            const float* irr = skyToMainCameraIrregular.get();

            const float skyToMainCamera[ 16 ] = {
                l_unit( irr[ 0 ] ), l_unit( irr[ 1 ] ), l_unit( irr[ 2 ] ),  0,
                l_unit( irr[ 4 ] ), l_unit( irr[ 5 ] ), l_unit( irr[ 6 ] ),  0,
                l_unit( irr[ 8 ] ), l_unit( irr[ 9 ] ), l_unit( irr[ 10 ] ), 0,
                irr[ 12 ],          irr[ 13 ],          irr[ 14 ],           1,
            };

            auto skyTransform = mModelMatrix;
            skyTransform.scale( 1, cvar::rt_sky_stretch, 1 );

            auto t = VSMatrix::smultMatrix( skyToMainCamera, skyTransform.get() );
            return fromGzMatrix( t.get() );
        }

        if( mModelMatrixEnabled )
        {
            return fromGzMatrix( mModelMatrix.get() );
        }

        return RG_TRANSFORM_IDENTITY;
    }

    auto MapLightLevel( int lightlevel ) -> float
    {
        assert( lightlevel <= 255 );
        int lmin = std::max< int >( cvar::rt_lightlevel_min, 0 );
        int lmax = std::min< int >( cvar::rt_lightlevel_max, 255 );

        if( lmin >= lmax )
        {
            return 0.0f;
        }
        if( lightlevel <= lmin )
        {
            return 0.0f;
        }
        if( lightlevel >= lmax )
        {
            return 1.0f;
        }
        float t = float( lightlevel - lmin ) / float( lmax - lmin );

        if( std::abs( cvar::rt_lightlevel_exp - 2.f ) < 0.01f )
        {
            return t * t;
        }
        if( std::abs( cvar::rt_lightlevel_exp - 1.f ) < 0.01f )
        {
            return t;
        }
        return std::powf( t, cvar::rt_lightlevel_exp );
    }

    auto MakeFirstPersonQuadInWorldSpace( std::span< const RgPrimitiveVertex > verts )
        -> std::pair< RgTransform, std::span< const RgPrimitiveVertex > >
    {
        if( verts.size() != 4 )
        {
            // assert( 0 );
            return { RgTransform{ RG_TRANSFORM_IDENTITY }, verts };
        }

        const auto  priority = m_weaponDrawCallIndex++;
        const float z        = 0.1f / float( 1 + priority );

        auto toPix = []( const RgPrimitiveVertex& vert ) {
            // because of MakeFormatted...
            return RgFloat2D{
                vert.position[ 0 ] / ONEGAMEUNIT_IN_METERS,
                vert.position[ 2 ] / ONEGAMEUNIT_IN_METERS,
            };
        };

        auto applyViewport = []( const RgViewport& vp, const RgFloat2D& vert ) {
            return RgFloat2D{
                vert.data[ 0 ] / float( vp.width ),
                vert.data[ 1 ] / float( vp.height ),
            };
        };

        // screen space [0,1]
        RgFloat2D scr01[] = {
            applyViewport( m_viewport, toPix( verts[ 0 ] ) ),
            applyViewport( m_viewport, toPix( verts[ 1 ] ) ),
            applyViewport( m_viewport, toPix( verts[ 2 ] ) ),
            applyViewport( m_viewport, toPix( verts[ 3 ] ) ),
        };

        // remap [0,1] to [-1,1] clip space
        RgFloat4D clipspace[] = {
            RgFloat4D{ scr01[ 0 ].data[ 0 ] * 2 - 1, scr01[ 0 ].data[ 1 ] * 2 - 1, z, 1.0f },
            RgFloat4D{ scr01[ 1 ].data[ 0 ] * 2 - 1, scr01[ 1 ].data[ 1 ] * 2 - 1, z, 1.0f },
            RgFloat4D{ scr01[ 2 ].data[ 0 ] * 2 - 1, scr01[ 2 ].data[ 1 ] * 2 - 1, z, 1.0f },
            RgFloat4D{ scr01[ 3 ].data[ 0 ] * 2 - 1, scr01[ 3 ].data[ 1 ] * 2 - 1, z, 1.0f },
        };

        // inverse projection to transform clip space -> view space
        RgFloat4D viewspace[] = {
            ApplyMat44ToVec4( m_mainCameraProjection_Inverse, clipspace[ 0 ] ),
            ApplyMat44ToVec4( m_mainCameraProjection_Inverse, clipspace[ 1 ] ),
            ApplyMat44ToVec4( m_mainCameraProjection_Inverse, clipspace[ 2 ] ),
            ApplyMat44ToVec4( m_mainCameraProjection_Inverse, clipspace[ 3 ] ),
        };

#if 0
        // inverse view to transform view space -> world space
        RgFloat3D worldspace[] = {
            FromHomogeneous( ApplyMat44ToVec4( m_mainCameraView_Inverse, viewspace[ 0 ] ) ),
            FromHomogeneous( ApplyMat44ToVec4( m_mainCameraView_Inverse, viewspace[ 1 ] ) ),
            FromHomogeneous( ApplyMat44ToVec4( m_mainCameraView_Inverse, viewspace[ 2 ] ) ),
            FromHomogeneous( ApplyMat44ToVec4( m_mainCameraView_Inverse, viewspace[ 3 ] ) ),
        };

        m_tempverts.clear();
        m_tempverts.assign( verts.begin(), verts.end() );
        for( uint32_t i = 0; i < std::size( worldspace ); i++ )
        {
            // because of m_mainCameraView_Inverse, m_mainCameraProjection_Inverse,
            // vi_world already have ONEGAMEUNIT_IN_METERS applied
            m_tempverts[ i ].position[ 0 ] = worldspace[ i ].data[ 0 ];
            m_tempverts[ i ].position[ 1 ] = worldspace[ i ].data[ 1 ];
            m_tempverts[ i ].position[ 2 ] = worldspace[ i ].data[ 2 ];
        }
        return m_tempverts;
#else

        // treat m_mainCameraView_Inverse as the transform
        const float* t = m_mainCameraView_Inverse;
        
        auto transform = RgTransform{ {
            { t[ 0 ], t[ 4 ], t[ 8 ], t[ 12 ] },
            { t[ 1 ], t[ 5 ], t[ 9 ], t[ 13 ] },
            { t[ 2 ], t[ 6 ], t[ 10 ], t[ 14 ] },
        } };

        m_tempverts.clear();
        m_tempverts.assign( verts.begin(), verts.end() );
        for( uint32_t i = 0; i < std::size( viewspace ); i++ )
        {
            double w = viewspace[ i ].data[ 3 ];
            w        = std::max( w, 0.00000001 );

            // because of m_mainCameraView_Inverse, m_mainCameraProjection_Inverse,
            // vi_world already have ONEGAMEUNIT_IN_METERS applied
            m_tempverts[ i ].position[ 0 ] = float( viewspace[ i ].data[ 0 ] / w );
            m_tempverts[ i ].position[ 1 ] = float( viewspace[ i ].data[ 1 ] / w );
            m_tempverts[ i ].position[ 2 ] = float( viewspace[ i ].data[ 2 ] / w );
        }
        return { transform, m_tempverts };
#endif
    }

    void InternalDraw( std::span< const RgPrimitiveVertex > verts,
                       std::span< const uint32_t >          indices,
                       const bool                           isUI,
                       const bool                           islines = false )
    {
        assert( RG_PACKED_COLOR_WHITE == rt.rgUtilPackColorByte4D( 255, 255, 255, 255 ) );

        if( islines && !isUI )
        {
            assert( 0 );
            return;
        }

        if( verts.empty() )
        {
            assert( 0 );
            return;
        }

        const char* texname = nullptr;
        if( mTextureEnabled && mMaterial.mMaterial )
        {
            if( FGameTexture* gametex = mMaterial.mMaterial->sourcetex )
            {
                if( FTexture* base = gametex->GetTexture() )
                {
                    if( auto hwtex = static_cast< RTHardwareTexture* >( base->GetHardwareTexture(
                            mMaterial.mTranslation, mMaterial.mMaterial->GetScaleFlags() ) ) )
                    {
                        hwtex->CreateIfWasnt( *gametex,
                                              mMaterial.mClampMode,
                                              mMaterial.mTranslation,
                                              mMaterial.mMaterial->GetScaleFlags(),
                                              mRenderStyle );
                        texname = hwtex->GetRTName();
                    }
                }
            }
        }

        if( !texname && !isUI && !rtstate.is< RtPrim::Sky >() &&
            !rtstate.is< RtPrim::SkyVisibility >() )
        {
            // assert( 0 );
        }

        // TODO: apply texture matrix on gpu
        if( mTextureMatrixEnabled )
        {
            m_tempverts.clear();
            m_tempverts.assign( verts.begin(), verts.end() );

            auto applyTexMatrix = [ & ]( float u, float v ) {
                auto m = [ & ]( int i, int j ) {
                    return mTextureMatrix.get()[ i + j * 4 ];
                };

                return std::pair{
                    m( 0, 0 ) * u + m( 1, 0 ) * v,
                    m( 0, 1 ) * u + m( 1, 1 ) * v,
                };
            };

            for( RgPrimitiveVertex& v : m_tempverts )
            {
                std::tie( v.texCoord[ 0 ], v.texCoord[ 1 ] ) =
                    applyTexMatrix( v.texCoord[ 0 ], v.texCoord[ 1 ] );
            }

            verts = m_tempverts;
        }

        if( rtstate.is< RtPrim::Sky >() && texname )
        {
            m_fb->RT_MarkWasSky();
        }

        RgTransform transform;
        if( rtstate.is< RtPrim::FirstPerson >() )
        {
            std::tie( transform, verts ) = MakeFirstPersonQuadInWorldSpace( verts );

            // Anchor for rt_gunglow. Only the plasma frames — this is the weapon whose
            // art has a lit core; anchoring on any weapon would move the light onto guns
            // that are not supposed to emit.
            if( texname && verts.size() == 4 &&
                ( strncmp( texname, "PLSG", 4 ) == 0 || strncmp( texname, "PLSF", 4 ) == 0 ) )
            {
                FVector3 c{ 0, 0, 0 };
                for( const auto& v : verts )
                {
                    c += FVector3{ v.position[ 0 ], v.position[ 1 ], v.position[ 2 ] };
                }
                m_gunAnchorView = c / float( verts.size() );
                m_haveGunAnchor = true;
            }
        }
        else if( RequiresTrueTransform() )
        {
            std::tie( transform, verts ) = CalculateTrueTransformAndItsVerts( verts );
        }
        else
        {
            transform = MakeTransform( rtstate.is< RtPrim::Sky >() );
        }

        auto ui = RgMeshPrimitiveSwapchainedEXT{
            .sType       = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_SWAPCHAINED_EXT,
            .pNext       = nullptr,
            .flags       = islines ? uint32_t{ RG_MESH_PRIMITIVE_SWAPCHAINED_DRAW_AS_LINES } : 0,
            .pViewport   = &m_viewport,
            .pView       = m_view,
            .pProjection = m_projection,
            .pViewProjection = nullptr,
        };

        auto l_makeInstanceFlags = [ & ]() -> RgMeshInfoFlags {
            if( rtstate.is< RtPrim::FirstPersonViewer >() )
            {
                return RG_MESH_FIRST_PERSON_VIEWER;
            }
            if( rtstate.is< RtPrim::FirstPerson >() )
            {
                return RG_MESH_FIRST_PERSON;
            }
            return 0;
        };

        auto l_makeSpectreFlags = [ & ]() -> RgMeshInfoFlags {
            if( IsSpectre() )
            {
                // suppress inter-reflection on spectres
                return RG_MESH_FORCE_IGNORE_REFRACT_AFTER;
            }
            return 0;
        };

        auto mesh = RgMeshInfo{
            .sType = RG_STRUCTURE_TYPE_MESH_INFO,
            .pNext = nullptr,
            .flags =
                l_makeInstanceFlags() | l_makeSpectreFlags() |
                ( rtstate.is< RtPrim::ExportInstance >() ? RG_MESH_EXPORT_AS_SEPARATE_FILE : 0 ),
            .uniqueObjectID = rtstate.get_uniqueid(),
            .pMeshName      = rtstate.is< RtPrim::ExportMap >() ? RT_GetMapName()
                              : rtstate.is< RtPrim::ExportInstance >()
                                  ? rtstate.get_exportinstance_name()
                                  : nullptr,
            .transform      = transform,
            .isExportable =
                rtstate.is< RtPrim::ExportMap >() || rtstate.is< RtPrim::ExportInstance >(),
            .animationTime        = 0.0f,
            .localLightsIntensity = MapLightLevel( rtstate.m_lightlevel ),
        };

        auto makePrimFlags = [ this, &verts ]( bool isUI ) -> RgMeshPrimitiveFlags {
            if( isUI )
            {
                return RG_MESH_PRIMITIVE_TRANSLUCENT;
            }
            if( rtstate.is< RtPrim::Decal >() )
            {
                assert( verts.size() == 4 );
                return RG_MESH_PRIMITIVE_DECAL;
            }
            if( rtstate.is< RtPrim::SkyVisibility >() )
            {
                return RG_MESH_PRIMITIVE_SKY_VISIBILITY;
            }
            if( rtstate.is< RtPrim::Sky >() )
            {
                return RG_MESH_PRIMITIVE_SKY | RG_MESH_PRIMITIVE_TRANSLUCENT;
            }
            if( rtstate.is< RtPrim::Particle >() )
            {
                return RG_MESH_PRIMITIVE_TRANSLUCENT;
            }
            if( rtstate.is< RtPrim::Mirror >() )
            {
                return RG_MESH_PRIMITIVE_MIRROR;
            }
            if( rtstate.is< RtPrim::Glass >() )
            {
                return RG_MESH_PRIMITIVE_GLASS;
            }

            RgMeshPrimitiveFlags add;
            switch( int( cvar::rt_wall_nomv ) )
            {
                case 0: add = 0; break;
                case 2: add = RG_MESH_PRIMITIVE_NO_MOTION_VECTORS; break;
                default:
                    add = rtstate.is< RtPrim::NoMotionVectors >()
                              ? RG_MESH_PRIMITIVE_NO_MOTION_VECTORS
                              : 0;
                    break;
            }
            
            bool alphaTest = mAlphaThreshold > 0;
            if( rt_mod_compat )
            {
                if( rtstate.is< RtPrim::ExportInstance >() )
                {
                    // Soft blends under RT:
                    //  - SAR2 / classic Fuzz spectre → IsSpectre() → rasterized
                    //    TRANSLUCENT overlay (the see-through ghost look).
                    //  - Any other LIVING soft-blend monster (64NightmareImp / TRO2) →
                    //    same treatment, see IsLivingGhost() below.
                    //  - Additive (DestAlpha One): fire/muzzle — TRANSLUCENT.
                    //  - Other sprites (and the spectre/imp CORPSE): ALPHA_TESTED cutout.
                    const bool additiveBlend =
                        mRenderStyle.BlendOp == STYLEOP_Add &&
                        mRenderStyle.DestAlpha == STYLEALPHA_One;

                    if( additiveBlend )
                    {
                        add |= RG_MESH_PRIMITIVE_TRANSLUCENT;
                        alphaTest = false;
                    }
                    else if( IsSpectre() || IsLivingGhost() )
                    {
                        // Spectre: rasterized TRANSLUCENT overlay with alpha floor.
                        // Gives the purple-dark see-through look (like classic alpha blend).
                        //
                        // IsLivingGhost() extends this to 64NightmareImp, which used to
                        // fall through to the ALPHA_TESTED branch below. That was wrong for
                        // a "RenderStyle Translucent, Alpha 0.60" monster and it actively
                        // broke it: RsWorld.inl ends with
                        //
                        //     if( alphaTest != 0 ) { if( outColor.a < 0.5 ) discard; }
                        //
                        // and `discard` kills the WHOLE fragment — including
                        // outScreenEmission, so the emissive eyes died with the body. The
                        // imp only ever survived because the old max(a, 0.80) floor happened
                        // to hold it above 0.5; that floor's real job was clearing this
                        // threshold, not looks. Dropping the floor to the authored 0.60 left
                        // almost no margin, and once rt_ghost_lightscale dimmed a room past
                        // ~0.83x the imp did not fade, it POPPED OUT ENTIRELY. At
                        // rt_nightmareimp_alpha 0.35 it was invisible everywhere.
                        //
                        // As a TRANSLUCENT overlay there is no threshold to fall off: the
                        // body fades smoothly to nothing and the eyes, on an ONE/ONE
                        // attachment, are never discarded at any alpha (2026-08-09).
                        add |= RG_MESH_PRIMITIVE_TRANSLUCENT;
                        alphaTest = false;
                    }
                    else
                    {
                        add |= RG_MESH_PRIMITIVE_ALPHA_TESTED;
                        alphaTest = true;
                    }
                }
                // World: keep alpha-test for real masks (fences). Soft-alpha solids were
                // forced opaque at upload time via looksLikeRealMask heuristic.
            }

            return ( alphaTest ? RG_MESH_PRIMITIVE_ALPHA_TESTED : 0 ) | add;
        };

        auto l_isemis = [ & ]() {
            if( mRenderStyle.BlendOp == STYLEOP_Add && mRenderStyle.DestAlpha == STYLEALPHA_One )
            {
                return true;
            }
            if( rt_mod_compat & 2 )
            {
                // Auto: brightmaps/glowmaps on sprites AND world geometry -> RT emissive
                if( rtstate.is< RtPrim::ExportInstance >() ||
                    rtstate.is< RtPrim::ExportMap >() )
                {
                    if( mBrightmapEnabled )
                    {
                        if( mTextureModeFlags & TEXF_Glowmap )
                        {
                            if( mMaterial.mMaterial && mMaterial.mMaterial->sourcetex &&
                                mMaterial.mMaterial->sourcetex->Layers.get() &&
                                mMaterial.mMaterial->sourcetex->Layers->Glowmap.get() &&
                                mMaterial.mMaterial->sourcetex->Layers->Glowmap->GetSourceLump() >= 0 &&
                                mMaterial.mMaterial->sourcetex->Layers->Glowmap->GetWidth() > 0 &&
                                mMaterial.mMaterial->sourcetex->Layers->Glowmap->GetHeight() > 0 )
                            {
                                return true;
                            }
                        }

                        if( mTextureModeFlags & TEXF_Brightmap )
                        {
                            if( mMaterial.mMaterial && mMaterial.mMaterial->sourcetex &&
                                mMaterial.mMaterial->sourcetex->Brightmap.get() &&
                                mMaterial.mMaterial->sourcetex->Brightmap->GetSourceLump() >= 0 &&
                                mMaterial.mMaterial->sourcetex->Brightmap->GetWidth() > 0 &&
                                mMaterial.mMaterial->sourcetex->Brightmap->GetHeight() > 0 )
                            {
                                return true;
                            }
                        }
                    }
                }
            }
            return false;
        };

        // Masked world geometry — fences, grates, the MAP01 cage — is alpha-TESTED,
        // not translucent: the texture's alpha cuts the holes, and the surface between
        // the holes is fully solid. But it arrives carrying a vertex alpha below 1, and
        // RTGL1 rasterizes any primitive whose packed vertex alpha is under
        // MESH_TRANSLUCENT_ALPHA_THRESHOLD (0.98, Const.h). A rasterized primitive is
        // never added to the acceleration structure at all, so it renders perfectly and
        // casts NOTHING — no shadow, at any light intensity, from any light.
        //
        // That is why the MAP01 fence cast no shadow while sprites and props in the same
        // room did: it was not in the BLAS to be hit. Forcing vertex alpha to 1 puts it
        // back in; the cutout still works, because RtAlphaTest.rahit tests the TEXTURE's
        // alpha per-texel and ignores intersections through the holes.
        //
        // Gated on mAlphaThreshold > 0 so only genuinely alpha-tested surfaces are
        // affected — real translucents (water, glass, additive FX) keep their alpha and
        // stay rasterized, which is correct for them (2026-08-08).
        const bool worldMaskedCutout =
            cvar::rt_force_mask_opaque && rt_mod_compat && !isUI && mAlphaThreshold > 0 &&
            !rtstate.is< RtPrim::ExportInstance >() && !rtstate.is< RtPrim::FirstPerson >() &&
            !rtstate.is< RtPrim::FirstPersonViewer >() && !rtstate.is< RtPrim::Sky >();

        // HACKHACK: replacements are ignored if a prim is rasterized, force alpha=1.0
        // Doom64-RT: always force opaque vertex color on world geometry under mod_compat.
        const bool forcealpha1 = ( mesh.flags & RG_MESH_FORCE_GLASS ) ||
                                 ( mesh.flags & RG_MESH_FORCE_MIRROR ) ||
                                 ( mesh.flags & RG_MESH_FORCE_WATER ) ||
                                 ( rt_mod_compat && rtstate.is< RtPrim::ExportMap >() ) ||
                                 worldMaskedCutout;

        // Doom64-RT: sector lightlevel / lightcolor must NOT bake into PT albedo.
        // Otherwise: yellow key-door sectors look neon-emissive, and lightlevel-0 rooms
        // get black vertex color so flashlight / ceiling lamps are absorbed.
        // IMPORTANT: doors/lifts are NOT ExportMap (movable) — still force white on them.
        const bool forceWorldWhiteRgb =
            rt_mod_compat && !isUI &&
            !rtstate.is< RtPrim::ExportInstance >() &&
            !rtstate.is< RtPrim::FirstPerson >() &&
            !rtstate.is< RtPrim::FirstPersonViewer >() &&
            !rtstate.is< RtPrim::Sky >() &&
            !rtstate.is< RtPrim::SkyVisibility >() &&
            !rtstate.is< RtPrim::Particle >() &&
            !rtstate.is< RtPrim::Decal >();

        // Same bake issue on sprites / weapon: lightlevel-0 → black uVertexColor →
        // silhouette even after world white fix. Keep uObjectColor (ThingColor / weapon
        // ObjectColor / sector sprite tint); drop lightlevel from uVertexColor RGB.
        const bool forceSpriteUnlitAlbedo =
            rt_mod_compat && !isUI &&
            ( rtstate.is< RtPrim::ExportInstance >() ||
              rtstate.is< RtPrim::FirstPerson >() ||
              rtstate.is< RtPrim::FirstPersonViewer >() );

        auto l_spriteAlpha = [ &, this ]() -> float {
            if( forcealpha1 )
            {
                return 1.0f;
            }
            float a = mStreamData.uObjectColor.a * mStreamData.uVertexColor[ 3 ];
            // Dropping the TRANSLUCENT flag is only half of going solid: RTGL1 also
            // rasterizes anything whose packed vertex alpha is under
            // MESH_TRANSLUCENT_ALPHA_THRESHOLD (0.98). The spectre pulses down to 0.20
            // and the nightmare imp sits at a flat 0.60, and rt_translucent_minalpha
            // floors both around 0.72 — all well under the bar, so without this they
            // would still miss the BLAS and still take no light.
            if( IsSolidGhost() )
            {
                return 1.0f;
            }

            bool             ghostCorpse = false;
            const GhostActor ghost       = GhostSprite( &ghostCorpse );
            const bool       livingGhost = ( ghost != GhostActor::None ) && !ghostCorpse;

            if( livingGhost && ghost == GhostActor::Spectre )
            {
                // FORCE, don't cap. 64Spectre's DECORATE only lowers alpha in states that
                // call A_SetTranslucent, and its Spawn/Idle loop never does:
                //
                //     Spawn:  SAR2 A 0 A_SetTranslucent(1.0, 0)
                //             SAR2 BD 10 A_Look
                //             Goto Spawn+1          <- loops here, alpha stays 1.0
                //     See:    ... A_SetTranslucent(0.75) 0.50 0.25 0.20
                //
                // so an idle spectre sits at 1.0 while a chasing one sits at 0.20. The old
                // min(a, minalpha) only clipped the top, leaving idle at 0.80 — 4x more
                // opaque than the same monster once it wakes up. That is why an idle
                // spectre still read as lit in a pitch black room while a charging one
                // looked right: rt_ghost_lightscale was working, it was just scaling a body
                // 4x more opaque to begin with (2026-08-09).
                //
                // Forcing one value is what the old comment here already claimed to do
                // ("uniformly semi-transparent") but min() never actually did. Cost: the
                // Idle state's 0.25->1.0 alpha pulse is flattened out. That pulse is what
                // reads as "glowing on and off" under PT — set rt_spectre_alpha 0 to hand
                // control back to DECORATE if the shimmer is wanted.
                const float forced = float( cvar::rt_spectre_alpha );
                if( forced > 0.f )
                {
                    a = std::min( forced, 1.f );
                }
            }
            else if( livingGhost )
            {
                // 64NightmareImp. Same treatment as the spectre above — one forced alpha,
                // and critically NOT run through the rt_translucent_minalpha floor further
                // down, because max(0.60, 0.80) would push it MORE opaque than DECORATE
                // asks for: the same too-visible-in-a-dark-room complaint by another route.
                //
                // Unlike the spectre there is no idle-vs-active discrepancy to repair here:
                // 64NightmareImp declares a flat "Alpha 0.60" and none of its states ever
                // call A_SetTranslucent, so Spawn and See already agree. The default is 0.35
                // — tuned by eye, below the authored 0.60, for the same reason the spectre
                // settled at 0.20: a faint enough sprite makes the imperfect light tracking
                // stop being visible (2026-08-09).
                const float forced = float( cvar::rt_nightmareimp_alpha );
                if( forced > 0.f )
                {
                    a = std::min( forced, 1.f );
                }
            }
            else if( IsSpectre() )
            {
                // Classic Fuzz spectres (not SAR2): cap only, no authored alpha to respect.
                a = std::min( a, float( cvar::rt_translucent_minalpha ) );
            }
            else if( rt_mod_compat && rtstate.is< RtPrim::ExportInstance >() )
            {
                // Other soft-blend sprites: floor so they're not ghostly-clear.
                a = std::max( a, float( cvar::rt_translucent_minalpha ) );
            }
            // Applied AFTER the minalpha floor/cap on purpose: those pin how see-through
            // the ghost is at full light, this then fades that whole look out with the
            // room. Folding it in before would let minalpha clamp the darkness back off.
            return a * GhostLightScale();
        };

        // The map's bright surfaces become the emitters. primColor below already carries
        // the sector hue at full strength, so a red corridor panel emits red without any
        // extra colour plumbing here.
        auto l_worldemissive = [ & ]() -> float {
            if( l_isemis() )
            {
                return float{ cvar::rt_emis_additive_dflt };
            }
            if( !forceWorldWhiteRgb )
            {
                return 0.f;
            }

            const float strength = float{ cvar::rt_sector_emis };
            // Map-relative, not absolute — see RT_UpdateSectorEmisThreshold.
            const float minLight = g_sectorEmisThreshold;
            if( strength <= 0.f )
            {
                return 0.f;
            }

            // A threshold at or above 255 means THIS map has no light features: its own
            // median is already so high that nothing stands out above it. Bail out.
            //
            // This used to clamp minLight to 254 instead, which inverted the whole
            // feature on exactly the maps it was meant to protect. MAP09 and MAP31 are
            // authored fullbright (median lightlevel 255), so the threshold comes out at
            // 295 — "nothing emits" — but the clamp pulled it back to 254 and the ramp
            // below then evaluated (255-254)/(255-254) = 1.0, i.e. FULL strength, on
            // every one of MAP09's 74 lightlevel-255 sectors out of 81. The result was a
            // night courtyard where every wall, pillar and floor self-glowed evenly with
            // no shadow anywhere, while the imps and barrels standing in it stayed pitch
            // black — RTGL1 emissive is not a light source (see rt_wall_strips), so the
            // glow lit the surface it was on and nothing else. MAP07 (threshold 260) was
            // the same bug in milder form, 119 sectors of 429.
            if( minLight >= 255.f )
            {
                return 0.f;
            }

            const float ll = float( rtstate.m_sectorLightLevel );
            if( ll <= minLight )
            {
                return 0.f;
            }

            // Ramp from the threshold to full bright so a lightlevel-200 panel glows
            // less than a lightlevel-255 one, instead of a hard on/off step.
            return ( ( ll - minLight ) / ( 255.f - minLight ) ) * strength;
        };

        RgColor4DPacked32 primColor;
        if( forceWorldWhiteRgb )
        {
            // Not white but hue-only: lightlevel stays dropped (that is what fixed the
            // black rooms), while the sector's colormap hue survives — see RT_SectorHue.
            //
            // Emissive world surfaces get the full light-strength hue, because under
            // this mod's launch config they ARE the room's light source: the launcher
            // runs rt_ceiling_lamps 0 / rt_sector_lights 0, so the glow in a MAP02
            // ceiling recess is texture emissive under rt_emis_mapboost, not an
            // analytic sphere. Tinting only the analytic lamps would have missed it.
            // Everything else gets the weak albedo strength — that is paint, not light.
            const float tintStrength = l_isemis() ? float{ cvar::rt_sector_tint_lights }
                                                  : float{ cvar::rt_sector_tint_albedo };

            const FVector3 tint = RT_SectorHue( rtstate.m_sectorLightColor.X,
                                                rtstate.m_sectorLightColor.Y,
                                                rtstate.m_sectorLightColor.Z,
                                                tintStrength );
            primColor =
                rt.rgUtilPackColorFloat4D( tint.X, tint.Y, tint.Z, l_spriteAlpha() );
        }
        else if( forceSpriteUnlitAlbedo )
        {
            // Do NOT try to dim a spectre via the RGB here to stop it reading as self-lit
            // in a dark room. It was tried and it cannot work: RsWorld.inl builds its
            // emissive out of baseColor() too —
            //
            //     ldrEmis = baseColor().rgb * emisTex.rgb;   // then *= emissiveMult
            //
            // so scaling this colour down to darken the body scales the eye mask down by
            // exactly the same factor. Body and eyes are inseparable *in this channel*.
            //
            // The channel that does separate them is ALPHA, one stage later: the body
            // (attachment 0) blends SRC_ALPHA while outScreenEmission (attachment 1)
            // blends ONE,ONE and never sees alpha. That is what GhostLightScale() in
            // l_spriteAlpha() uses. The living ghost stays rasterized on purpose.
            primColor = rt.rgUtilPackColorFloat4D( mStreamData.uObjectColor.r,
                                                  mStreamData.uObjectColor.g,
                                                  mStreamData.uObjectColor.b,
                                                  l_spriteAlpha() );
        }
        else
        {
            primColor = rtcolor_multiply(
                mStreamData.uObjectColor, mStreamData.uVertexColor, forcealpha1 );
        }

        // Reports what RTGL1 will actually DO with each world primitive, not what we
        // hoped it would do. Two prior fixes were judged by eye from the final image and
        // both nulls were worthless, because "renders but casts nothing" and "casts but
        // the shadow is washed out" look identical there.
        //
        // RASTERIZED is the field that matters: RTGL1 keeps a primitive out of the
        // acceleration structure entirely when its vertex alpha is below 0.98 or it
        // carries TRANSLUCENT (VulkanDevice.cpp IsRasterized, Const.h:54), and geometry
        // outside the BLAS can never block a shadow ray. Printing alphaThr alongside it
        // shows whether rt_force_mask_opaque's gate even fired on this surface
        // (2026-08-08).
        if( cvar::rt_prim_debug && !isUI && texname )
        {
            const float vAlpha = mStreamData.uObjectColor.a * mStreamData.uVertexColor[ 3 ];
            const float sent   = forcealpha1 ? 1.0f : vAlpha;
            const auto  pf     = makePrimFlags( isUI );
            const bool  raster = ( pf & RG_MESH_PRIMITIVE_TRANSLUCENT ) || sent < 0.98f;

            struct PrimStat
            {
                float vAlpha, sent, alphaThr;
                uint32_t flags;
                bool raster, forced;
                int count;
            };
            static std::unordered_map< std::string, PrimStat > s_stats;
            static int                                         s_tick;

            auto& st = s_stats[ texname ];
            st = PrimStat{ vAlpha, sent, mAlphaThreshold, uint32_t( pf ),
                           raster, forcealpha1, st.count + 1 };

            if( ( ++s_tick % 600 ) == 0 )
            {
                Printf( "rt_prim_debug: %zu distinct world texture(s) this frame\n",
                        s_stats.size() );
                // Print EVERY world texture, not just the rasterized ones. The first
                // version filtered to `raster` only -- and the answer turned out to be
                // that the surface in question was NOT in that list, which the filter
                // made indistinguishable from it not being drawn at all. Same mistake
                // as the truncated nearby-texture dump (§14): a filtered instrument can
                // only ever confirm the theory it was built around.
                std::vector< std::pair< std::string, PrimStat > > rows( s_stats.begin(),
                                                                        s_stats.end() );
                std::sort( rows.begin(), rows.end(), []( const auto& a, const auto& b ) {
                    return a.second.count > b.second.count;
                } );
                for( const auto& [ nm, s ] : rows )
                {
                    Printf( "  %-10s x%-4d a=%.2f sent=%.2f thr=%.3f flags=0x%-5X "
                            "f1=%d  %s%s\n",
                            nm.c_str(), s.count, s.vAlpha, s.sent, s.alphaThr, s.flags,
                            int( s.forced ),
                            s.raster ? "RASTERIZED(no shadow)" : "in BLAS",
                            ( s.flags & RG_MESH_PRIMITIVE_ALPHA_TESTED ) ? " ALPHATESTED"
                                                                        : "" );
                }
                s_stats.clear();
            }
        }

        // Doom64-RT: tag the water flats engine-side rather than through the JSON
        // meta. RTGL only runs its water path on primitives carrying
        // RG_MESH_PRIMITIVE_WATER, and the JSON route (isWater in
        // rt/data/textures.json) proved impossible to verify from here: RTGL's
        // own messages are gated behind BOTH -rtdebug and its private
        // g_printSeverity, so a meta that silently failed to apply looks exactly
        // like a meta that applied and did nothing.
        //
        // Tagging here is also more durable. rt/data/textures.json lives under
        // build/ (gitignored) and is rewritten wholesale by the PBR tooling, so
        // the tag had to be re-applied by hand after every regen. A texture-name
        // match in the engine is under launcher control like rt_faux_lamps and
        // rt_solo_lamps already are, and survives everything.
        auto l_waterflag = [ & ]() -> RgMeshPrimitiveFlags {
            if( !cvar::rt_water_style || isUI || !texname )
            {
                return RgMeshPrimitiveFlags( 0 );
            }
            // PREFIX match, and that is the whole point. D64W2_01 is not a
            // texture, it is frame 1 of a 64-frame ANIMDEFS sequence
            // (D64W2_01..D64W2_64, 2 tics each); the map's sector names frame 1
            // but GZDoom swaps in a different frame every 2 tics, so the name
            // that actually reaches RTGL is almost never "D64W2_01".
            //
            // An exact match therefore tagged the water for 2 tics out of ~128 —
            // the water became water for one frame per ~3.7s cycle and reverted,
            // which is exactly the "regular bright flash" this was reported as.
            // RTGL's own GeomInfoManager says the same thing: "can't use texture
            // / mesh name, as texture can be just 1 frame of animation sequence".
            //
            // D64WATR1/2 are the 192x192 source patches (warp2 in ANIMDEFS), kept
            // in case a map places one directly.
            static const char* const kWaterPrefix[] = { "D64W1_", "D64W2_" };
            static const char* const kWaterExact[]  = { "D64WATR1", "D64WATR2" };

            const auto tagged = [ & ]( const char* nm ) {
                // One line per distinct frame name per session. Printf is
                // gzdoom's own, so it is NOT subject to RTGL's message gates.
                static std::unordered_set< std::string > s_seen;
                if( s_seen.insert( nm ).second )
                {
                    Printf( "RT water: tagging \"%s\" as RG_MESH_PRIMITIVE_WATER "
                            "(rt_water_style %d)\n",
                            nm,
                            int( cvar::rt_water_style ) );
                }
                return RG_MESH_PRIMITIVE_WATER;
            };

            for( const char* w : kWaterPrefix )
            {
                if( strncmp( texname, w, strlen( w ) ) == 0 )
                {
                    return tagged( texname );
                }
            }
            for( const char* w : kWaterExact )
            {
                if( strcmp( texname, w ) == 0 )
                {
                    return tagged( texname );
                }
            }
            return RgMeshPrimitiveFlags( 0 );
        };

        auto prim = RgMeshPrimitiveInfo{
            .sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
            .pNext = isUI ? &ui : nullptr,
            .flags = makePrimFlags( isUI ) | l_waterflag() | RG_MESH_PRIMITIVE_FORCE_EXACT_NORMALS |
                     ( rtstate.is< RtPrim::ExportInvertNormals >()
                           ? RG_MESH_PRIMITIVE_EXPORT_INVERT_NORMALS
                           : 0 ),
            .primitiveIndexInMesh = rtstate.next_primitiveindex(),
            .pVertices            = verts.data(),
            .vertexCount          = static_cast< uint32_t >( verts.size() ),
            .pIndices             = indices.empty() ? nullptr : indices.data(),
            .indexCount           = static_cast< uint32_t >( indices.size() ),
            .pTextureName         = texname,
            .textureFrame         = 0,
            .color        = primColor,
            .emissive     = l_worldemissive(),
            .classicLight = lightlevel_to_classic( isUI, mLightParms[ 3 ] ),
        };

#ifndef NDEBUG
        if( cvar::_rt_showexportable )
        {
            if( !rtstate.is< RtPrim::ExportMap >() && !isUI )
            {
                return;
            }
        }
#endif

        // Why this exists: the plasma rifle goes half see-through while firing and no
        // other weapon does. The sprite's own alpha is binary (0/255, measured on every
        // frame) and RsWorld.inl's alpha test is a discard, so neither can produce a
        // PARTIAL fade — that needs alpha BLENDING, which means the quad is being
        // rasterized as translucent. RTGL1 rasterizes anything whose packed vertex alpha
        // is under MESH_TRANSLUCENT_ALPHA_THRESHOLD (0.98), so the question is simply
        // what the alpha chain below evaluates to on the fire frames. Print it.
        if( cvar::rt_wpn_debug &&
            ( rtstate.is< RtPrim::FirstPerson >() || rtstate.is< RtPrim::FirstPersonViewer >() ) )
        {
            // Dedup on texture+flags+quantised alpha so a held trigger does not spam.
            static std::unordered_set< uint64_t > s_seen;
            const uint32_t                        a8 = uint32_t( primColor >> 24 );
            const uint64_t key = ( uint64_t( prim.flags ) << 32 ) ^
                                 ( uint64_t( a8 ) << 24 ) ^
                                 std::hash< std::string_view >{}( texname ? texname : "" );
            if( s_seen.insert( key ).second )
            {
                Printf( "RTWPN %s flags=0x%X alphaThresh=%.2f objA=%.3f vertA=%.3f "
                        "packedA=%u emis=%.3f blendop=%d destalpha=%d\n",
                        texname ? texname : "?",
                        unsigned( prim.flags ),
                        mAlphaThreshold,
                        mStreamData.uObjectColor.a,
                        mStreamData.uVertexColor[ 3 ],
                        a8,
                        prim.emissive,
                        int( mRenderStyle.BlendOp ),
                        int( mRenderStyle.DestAlpha ) );
            }
        }

        RgResult r = rt.rgUploadMeshPrimitive( &mesh, &prim );
        RG_CHECK( r );
    }

public:
    void RT_SetMatrices( const VSMatrix& view, const VSMatrix& proj )
    {
        // TODO: only calculate when UI mode;
        //       can those UI elements be with perspective matrix?

        // clang-format off
        constexpr static float vkcorrection[] = {
            1,  0,    0, 0,
            0, -1,    0, 0,
            0,  0, 0.5f, 0,
            0,  0, 0.5f, 1,
        };
        // clang-format on

        auto correctedProj = VSMatrix::smultMatrix( vkcorrection, proj.get() );
        memcpy( m_projection, correctedProj.get(), sizeof( float ) * 16 );
        memcpy( m_view, view.get(), sizeof( float ) * 16 );
    }

    void RT_AddMainCamera( const FRenderViewpoint& viewpoint )
    {
        const auto [ up, right, forward ] = RT_MakeUpRightForwardVectors( viewpoint.Angles );

        const float pixelstretch =
            viewpoint.ViewLevel ? viewpoint.ViewLevel->info->pixelstretch : 1.0f;

        const auto aspectRatio = r_viewwindow.WidescreenRatio;
        const auto fovRatio    = r_viewwindow.WidescreenRatio >= 1.3f ? 1.333333f : aspectRatio;

        const auto fovy = static_cast< float >(
            2.0 * std::atan( std::tan( viewpoint.FieldOfView.Radians() / 2.0 ) /
                             static_cast< double >( fovRatio ) ) );


        auto readback = RgCameraInfoReadbackEXT{
            .sType = RG_STRUCTURE_TYPE_CAMERA_INFO_READ_BACK_EXT,
        };

        auto info = RgCameraInfo{
            .sType       = RG_STRUCTURE_TYPE_CAMERA_INFO,
            .pNext       = &readback,
            .flags       = 0,
            .position    = { float( viewpoint.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                             float( viewpoint.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                             float( viewpoint.Pos.Z ) * ONEGAMEUNIT_IN_METERS },
            .up          = up,
            .right       = right,
            .fovYRadians = fovy,
            .aspect      = aspectRatio * pixelstretch,
            .cameraNear  = cvar::rt_znear,
            .cameraFar   = cvar::rt_zfar,
        };

        RgResult r = rt.rgUploadCamera( &info );
        RG_CHECK( r );


        // for first-person weapons
        memcpy( m_mainCameraView_Inverse, readback.viewInverse, 16 * sizeof( float ) );
        memcpy( m_mainCameraProjection_Inverse, readback.projectionInverse, 16 * sizeof( float ) );
        static_assert( sizeof m_mainCameraView_Inverse == sizeof readback.viewInverse );
        static_assert( sizeof m_mainCameraProjection_Inverse == sizeof readback.projectionInverse );


        RT_AddFlashlight( info.position, forward, up, right );
        RT_AddMuzzleFlash( viewpoint.ViewActor, viewpoint.extralight, info.position, forward, up );
        RT_AddWeaponGlow( viewpoint.camera, info.position, forward, up );
    }

    void RT_AddFlashlight( const RgFloat3D& basePosition,
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
            return 1.f - std::sin( u * pi() );
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

        auto target = gzvec3( basePosition ) + 20 * aim;
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
    static float cvarcolor_luma( const FColorCVarRef& c )
    {
        const uint32_t ba = *( c );
        return 0.2126f * float( RPART( ba ) ) + 0.7152f * float( GPART( ba ) ) +
               0.0722f * float( BPART( ba ) );
    }

    struct MuzzleTint
    {
        RgColor4DPacked32 color;
        // rt_mzlflsh_intensity is a multiplier on the COLOUR, so a saturated hue carries
        // less light than the warm default at the same number: measured against
        // ff8c52 (luma 160), plasma 3355ff is 0.56x, unmaker ff1111 only 0.42x. Left
        // uncompensated, retinting the flash silently dimmed it — "the muzzle flash is
        // barely visible anymore". This scales intensity back so changing the hue does
        // not change how bright the flash reads. Clamped so an extreme colour cannot
        // blow the exposure.
        float intensityScale;
    };

    MuzzleTint MuzzleFlashTintFor( AActor* viewactor ) const
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

    void RT_AddMuzzleFlash( AActor*          viewactor,
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

        const bool wantFlash =
            extralight > 0 && cvar::rt_mzlflsh && viewactor && viewactor->Sector;

        if( wantFlash )
        {
            s_fade = 1.f;
        }
        else
        {
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
    void RT_AddWeaponGlow( AActor*          camera,
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

private:
    RgViewport m_viewport{};
    float      m_view[ 16 ]{};
    float      m_projection[ 16 ]{};

    float m_mainCameraView_Inverse[ 16 ]{};
    float m_mainCameraProjection_Inverse[ 16 ]{};

    uint32_t m_weaponDrawCallIndex{ 0 }; // to z-sort weapon sprites

    // Where the plasma rifle's quad actually is, in VIEW space metres (origin = eye),
    // captured as it is uploaded. RT_AddWeaponGlow anchors the core light to this
    // instead of a guessed offset from the camera: the quad sits centimetres from the
    // eye, so the old 0.7m-forward placement put the light PAST the gun and lit its back
    // face, which is why it lit the room but never the sprite. Anchoring also gives the
    // weapon bob for free — it is the real geometry, not an approximation of it.
    FVector3 m_gunAnchorView{ 0, 0, 0 };
    bool     m_haveGunAnchor{ false };

    std::vector< RgPrimitiveVertex > m_tempverts{};

public:
    RTFrameBuffer* m_fb{ nullptr };
};



class RTDataBuffer
    : public IDataBuffer
    , public VectorAsBuffer
{
    void BindRange( FRenderState* state, size_t start, size_t length ) override
    {
        auto hwstate = static_cast< RTRenderState* >( state );

        // ugly way to fetch viewpoint info
        if( this == hwstate->m_fb->mViewpoints->DataBuffer() )
        {
            const HWViewpointUniforms& vp = hwstate->m_fb->mViewpoints->FetchViewpoint( start );
            hwstate->RT_SetMatrices( vp.mViewMatrix, vp.mProjectionMatrix );
        }
    }
};



void RT_Print( const char* pMessage, RgMessageSeverityFlags flags, void* pUserData )
{
    if( !pMessage )
    {
        DPrintf( DMSG_ERROR, "RT_Print: pMessage is NULL\n" );
        return;
    }

    if( flags & RG_MESSAGE_SEVERITY_ERROR )
    {
        DPrintf( DMSG_ERROR, "%s\n", pMessage );

#ifdef WIN32
        static bool g_breakOnError = true;
        if( g_breakOnError )
        {
            auto msg = std::string_view{ pMessage };
            auto str = std::format( "{}{}\n"
                                    "\n\'Abort\' to exit the game."
                                    "\n\'Retry\' to skip only this error message."
                                    "\n\'Ignore\' to ignore all such error messages.",
                                    msg,
                                    msg.ends_with( '.' ) ? "" : "." );

            int ok = MessageBoxA( nullptr,
                                  str.c_str(), // null-terminated
                                  "Renderer Error",
                                  MB_ABORTRETRYIGNORE | MB_DEFBUTTON2 | MB_ICONERROR );
            switch( ok )
            {
                case IDIGNORE: g_breakOnError = false; break;
                case IDRETRY: break;
                case IDABORT:
                default: exit( -1 );
            }
        }
#endif
    }
    else if( flags & RG_MESSAGE_SEVERITY_WARNING )
    {
        // Printf, not DPrintf: DPrintf( DMSG_WARNING, ... ) is additionally
        // gated behind gzdoom's `developer` cvar being >= 2, which was a third
        // independent layer of silence on top of RgInstanceCreateInfo::
        // allowedMessages and RTGL's own g_printSeverity. Renderer warnings
        // (DLSS-RR failing to initialise, denoiser path changing) must reach
        // the console and the logfile unconditionally -- muting them by
        // default is what hid the compiled-out-RR bug for an entire
        // investigation.
        Printf( "%s\n", pMessage );
    }
    else if( flags & RG_MESSAGE_SEVERITY_INFO )
    {
        DPrintf( DMSG_NOTIFY, "%s\n", pMessage );
    }
    else
    {
        DPrintf( DMSG_SPAMMY, "%s\n", pMessage );
    }
}

} // anonymous namespace

bool RT_ModMapNeedsLiveGeometryUpload()
{
    // PWAD maps use RT_MapName like "d64rtr_v15_map01" and have no baked rt/scenes/*.
    // Stock Doom II maps are plain "map01" and rely on static gltf — those can omit uploads.
    const char* mapname = RT_GetMapName();
    return mapname != nullptr && strchr( mapname, '_' ) != nullptr;
}

#ifdef _WIN32
std::atomic< HWND > g_msgbox_parent{};
#endif



//
//
//
//
//
//



RG_D3D12CORE_HELPER( "rt/" )

Win32RTVideo::Win32RTVideo()
{
    extern std::atomic_bool g_continueMain;
    extern std::atomic_bool g_forceLnchThreadStop;
    while( !g_continueMain )
    {
    }
    if( g_forceLnchThreadStop.load() )
    {
        exit( 1 );
    }

    // warn if no needed dll-s
    if( !Args->CheckParm( "-nodllcheck" ) )
    {
        enum rt_feature_flag_t
        {
            RT_FEATURE_FSR2     = 1,
            RT_FEATURE_FSR3_FG  = 2,
            RT_FEATURE_DLSS2    = 4,
            RT_FEATURE_DLSS3_FG = 8,
            RT_FEATURE_DLSS_RR  = 16,
        };

        const std::pair< std::filesystem::path, int > dlls[] = {
            { "rt/bin/D3D12Core.dll", RT_FEATURE_FSR3_FG | RT_FEATURE_DLSS3_FG },
            { "rt/bin/nvngx_dlss.dll", RT_FEATURE_DLSS2 },
            { "rt/bin/nvngx_dlssd.dll", RT_FEATURE_DLSS_RR },
            { "rt/bin/nvngx_dlssg.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/NvLowLatencyVk.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/sl.dlss.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/sl.dlss_g.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/sl.reflex.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/sl.pcl.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/sl.common.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/sl.interposer.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/ffx_fsr2_x64.dll", RT_FEATURE_FSR2 },
            { "rt/bin/ffx_fsr3_x64.dll", RT_FEATURE_FSR3_FG },
            { "rt/bin/ffx_fsr3upscaler_x64.dll", RT_FEATURE_FSR3_FG },
            { "rt/bin/ffx_frameinterpolation_x64.dll", RT_FEATURE_FSR3_FG },
            { "rt/bin/ffx_opticalflow_x64.dll", RT_FEATURE_FSR3_FG },
            { "rt/bin/ffx_backend_dx12_x64.dll", RT_FEATURE_FSR3_FG },
            { "rt/bin/ffx_backend_vk_x64.dll", RT_FEATURE_FSR2 | RT_FEATURE_FSR3_FG },
        };

        auto failedPaths    = std::string{};
        int  failedFeatures = 0;
        for( const auto& [ dll, feature ] : dlls )
        {
            if( !exists( dll ) )
            {
                failedPaths += "    " + dll.filename().string() + '\n';
                failedFeatures |= feature;
            }
        }

        if( !failedPaths.empty() )
        {
            auto msg = std::string{};

            if( failedFeatures == 0 )
            {
                msg = "Some features will NOT be available!";
            }
            else
            {
                // clang-format off
                if( failedFeatures & RT_FEATURE_DLSS3_FG) msg += "NVIDIA DLSS3 (AI Frame Generation)\n";
                if( failedFeatures & RT_FEATURE_DLSS2   ) msg += "NVIDIA DLSS2 (AI Upscaling)\n";
                if( failedFeatures & RT_FEATURE_DLSS_RR ) msg += "NVIDIA DLSS Ray Reconstruction\n";
                if( failedFeatures & RT_FEATURE_FSR3_FG ) msg += "AMD FSR 3 (Frame Generation)\n";
                if( failedFeatures & RT_FEATURE_FSR2    ) msg += "AMD FSR 2 (Upscaling)\n";
                // clang-format on
                msg += "                                   will NOT be available!\n";
            }

            msg += "Reason: \'rt/bin/\' folder doesn't contain:\n";
            msg += failedPaths;
            // msg += "\n(To suppress this warning, use \'-nodllcheck\' argument)";
            msg += "\n\nDo you want to download the missing files?\n";
            msg += "\nYES - open renderer's Download page";
            msg += "\nNO  - proceed with a limited feature set";
            
            int l = MessageBoxA( g_msgbox_parent.load(),
                                 msg.c_str(),
                                 "DLL check failure",
                                 MB_ICONEXCLAMATION | MB_YESNO );
            if( l == IDYES )
            {
                ShellExecute(
                    nullptr, 0, L"https://github.com/vs-shirokii/RTGL/releases", 0, 0, SW_SHOW );
                exit( -1 );
            }
        }
    }

    rt = RgInterface{};

#ifdef WIN32
    auto win32Info = RgWin32SurfaceCreateInfo{
        .hinstance = GetModuleHandle( NULL ),
        .hwnd      = mainwindow.GetHandle(),
    };
#else
    RgXlibSurfaceCreateInfo x11Info = { .dpy    = wmInfo.info.x11.display,
                                        .window = wmInfo.info.x11.window };
#endif

    auto info = RgInstanceCreateInfo
    {
        .sType = RG_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = NULL,

        .version = RG_RTGL_VERSION_API, .sizeOfRgInterface = sizeof( RgInterface ),

        .pAppName = "GZDoom", .pAppGUID = "8cbd354f-38d3-4173-92b9-c16b5a210b37",

#if WIN32
        .pWin32SurfaceInfo = &win32Info,
#else
        .pXlibSurfaceCreateInfo  = &x11Info,
#endif

        .pOverrideFolderPath = "rt/",

        .pfnPrint = RT_Print, .pUserPrintData = nullptr,
        // WARNING and ERROR are ALWAYS allowed; -rtdebug only adds the chatty
        // VERBOSE/INFO stream. This used to be `0` without -rtdebug, i.e. RTGL
        // failures were muted by default -- which is precisely how "DLSS-RR was
        // compiled out of RTGL1.dll" survived undetected (every DLSSRR: failure
        // string was suppressed), and how a null nvDlssRr still silently falls
        // back to A-SVGF today. A renderer must never swallow its own errors.
        .allowedMessages =
            Args->CheckParm( "-rtdebug" )
                ? RgMessageSeverityFlags{ RG_MESSAGE_SEVERITY_VERBOSE | RG_MESSAGE_SEVERITY_INFO |
                                          RG_MESSAGE_SEVERITY_WARNING | RG_MESSAGE_SEVERITY_ERROR }
                : RgMessageSeverityFlags{ RG_MESSAGE_SEVERITY_WARNING |
                                          RG_MESSAGE_SEVERITY_ERROR },

        .primaryRaysMaxAlbedoLayers = 1, .indirectIlluminationMaxAlbedoLayers = 1,

        .replacementsMaxVertexCount = 32 * 1024 * 1024, .dynamicMaxVertexCount = 2 * 1024 * 1024,

        .rayCullBackFacingTriangles = 0,
        .allowTexCoordLayer1 = false, .allowTexCoordLayer2 = false, .allowTexCoordLayer3 = false,

        .lightmapTexCoordLayerIndex = 1,

        .rasterizedMaxVertexCount = 1 << 20, .rasterizedMaxIndexCount = 1 << 21,
        .rasterizedVertexColorGamma = true,

        .rasterizedSkyCubemapSize = 256,

        .textureSamplerForceMinificationFilterLinear = true,
        .textureSamplerForceNormalMapFilterLinear    = true,

        .pbrTextureSwizzling = RG_TEXTURE_SWIZZLING_NULL_ROUGHNESS_METALLIC,

        .effectWipeIsUsed = true,

        .worldUp = { 0, 0, 1 }, .worldForward = { 0, 1, 0 }, .worldScale = 1.0f,

        .importedLightIntensityScaleDirectional = 1.0f / 50,
        .importedLightIntensityScaleSphere      = 1.0f / 500,
        .importedLightIntensityScaleSpot        = 1.0f / 500,
    };

#ifndef NDEBUG
    constexpr bool isdebug = true;
#else
    constexpr bool isdebug = false;
#endif

    const char* remixdll = g_isremix ? "\\bin_remix\\RTGL1.dll" : nullptr;

    RgResult r = rgLoadLibraryAndCreate( &info, isdebug, remixdll, &rt, nullptr );
    if( r != RG_RESULT_SUCCESS )
    {
        auto msg = std::string{ "RgResult code: " };

        switch( r )
        {
            case RG_RESULT_CANT_FIND_DYNAMIC_LIBRARY:
                msg = remixdll  ? "Can't load Remix Renderer DLLs"
                      : isdebug ? "Can't find \'rt/bin/debug/RTGL1.dll\' file"
                                : "Can't find \'rt/bin/RTGL1.dll\' file";
                break;
            case RG_RESULT_CANT_FIND_ENTRY_FUNCTION_IN_DYNAMIC_LIBRARY:
                msg =
                    remixdll  ? "Can't find rgCreateInstance function in Remix Renderer wrapper DLL"
                    : isdebug ? "Can't find rgCreateInstance function in \'rt/bin/debug/RTGL1.dll\'"
                              : "Can't find rgCreateInstance function in \'rt/bin/RTGL1.dll\'";
                break;

            // clang-format off
            case RG_RESULT_NOT_INITIALIZED:                     msg += "RG_RESULT_NOT_INITIALIZED";                     break;
            case RG_RESULT_ALREADY_INITIALIZED:                 msg += "RG_RESULT_ALREADY_INITIALIZED";                 break;
            case RG_RESULT_GRAPHICS_API_ERROR:                  msg += "RG_RESULT_GRAPHICS_API_ERROR";                  break;
            case RG_RESULT_INTERNAL_ERROR:                      msg += "RG_RESULT_INTERNAL_ERROR";                      break;
            case RG_RESULT_CANT_FIND_SUPPORTED_PHYSICAL_DEVICE: msg += "RG_RESULT_CANT_FIND_SUPPORTED_PHYSICAL_DEVICE"; break;
            case RG_RESULT_FRAME_WASNT_STARTED:                 msg += "RG_RESULT_FRAME_WASNT_STARTED";                 break;
            case RG_RESULT_FRAME_WASNT_ENDED:                   msg += "RG_RESULT_FRAME_WASNT_ENDED";                   break;
            case RG_RESULT_WRONG_FUNCTION_CALL:                 msg += "RG_RESULT_WRONG_FUNCTION_CALL";                 break;
            case RG_RESULT_WRONG_FUNCTION_ARGUMENT:             msg += "RG_RESULT_WRONG_FUNCTION_ARGUMENT";             break;
            case RG_RESULT_WRONG_STRUCTURE_TYPE:                msg += "RG_RESULT_WRONG_STRUCTURE_TYPE";                break;
            case RG_RESULT_ERROR_CANT_FIND_HARDCODED_RESOURCES: msg += "RG_RESULT_ERROR_CANT_FIND_HARDCODED_RESOURCES"; break;
            case RG_RESULT_ERROR_CANT_FIND_SHADER:              msg += "RG_RESULT_ERROR_CANT_FIND_SHADER";              break;
            case RG_RESULT_ERROR_MEMORY_ALIGNMENT:              msg += "RG_RESULT_ERROR_MEMORY_ALIGNMENT";              break;
            case RG_RESULT_ERROR_NO_VULKAN_EXTENSION:           msg += "RG_RESULT_ERROR_NO_VULKAN_EXTENSION";           break;
                // clang-format on

            default: msg += std::to_string( r ); break;
        }

        MessageBoxA(
            nullptr, msg.c_str(), "Failed to initialize RT renderer", MB_ICONEXCLAMATION | MB_OK );
        exit( -1 );
    }

    // on first start, try to set DLSS, if available
    if( cvar::rt_firststart )
    {
        if( rt.rgUtilIsUpscaleTechniqueAvailable( RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS, //
                                                  RG_FRAME_GENERATION_MODE_OFF,
                                                  nullptr ) )
        {
            cvar::rt_upscale_dlss = 2;
            cvar::rt_upscale_fsr2 = 0;
            cvar::rt_remix_taa    = 0;
            cvar::rt_ef_vintage   = 0;
        }
        else if( rt.rgUtilIsUpscaleTechniqueAvailable( RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2, //
                                                       RG_FRAME_GENERATION_MODE_OFF,
                                                       nullptr ) )
        {
            cvar::rt_upscale_dlss = 0;
            cvar::rt_upscale_fsr2 = 2;
            cvar::rt_remix_taa    = 0;
            cvar::rt_ef_vintage   = 0;
        }
        else
        {
            cvar::rt_upscale_dlss = 0;
            cvar::rt_upscale_fsr2 = 0;
            cvar::rt_remix_taa    = g_isremix ? 2 : 0;
            cvar::rt_ef_vintage   = g_isremix ? 0 : RT_VINTAGE_480_DITHER;
        }
    }
    else
    {
        if( g_isremix )
        {
            if( cvar::rt_upscale_dlss == 0 && //
                cvar::rt_upscale_fsr2 > 0 &&  //
                cvar::rt_remix_taa == 0 &&    //
                cvar::rt_ef_vintage == 0 )
            {
                cvar::rt_remix_taa = cvar::rt_upscale_fsr2;
            }
            cvar::rt_upscale_fsr2 = 0;
            cvar::rt_ef_vintage   = 0;
        }
        else
        {
            if( cvar::rt_upscale_dlss == 0 && //
                cvar::rt_upscale_fsr2 == 0 &&  //
                cvar::rt_remix_taa > 0 &&    //
                cvar::rt_ef_vintage == 0 )
            {
                cvar::rt_upscale_fsr2 = cvar::rt_remix_taa;
            }
            cvar::rt_remix_taa = 0;
        }
    }
}

DFrameBuffer* Win32RTVideo::CreateFrameBuffer()
{
    return new RTFrameBuffer{ m_hMonitor, vid_fullscreen };
}

TArray< uint8_t > RTFrameBuffer::GetScreenshotBuffer( int& pitch, ESSType& color_type, float& gamma )
{
    // RTGL1 exposes no GPU readback for the presented frame. Capture the HWND
    // after present so `screenshot` / Level.MakeScreenShot work (and don't need focus).
    const int w = GetClientWidth() > 0 ? GetClientWidth() : GetWidth();
    const int h = GetClientHeight() > 0 ? GetClientHeight() : GetHeight();
    if( w <= 0 || h <= 0 )
    {
        return {};
    }

    HWND hwnd = mainwindow.GetHandle();
    if( !hwnd )
    {
        return {};
    }

    HDC hdcWin = GetDC( hwnd );
    if( !hdcWin )
    {
        return {};
    }

    HDC     hdcMem = CreateCompatibleDC( hdcWin );
    HBITMAP hbm    = CreateCompatibleBitmap( hdcWin, w, h );
    HGDIOBJ old    = SelectObject( hdcMem, hbm );

    // PW_RENDERFULLCONTENT: ask DWM for the redirected surface (Vulkan/DXGI).
    BOOL ok = PrintWindow( hwnd, hdcMem, 0x00000002 );
    if( !ok )
    {
        ok = BitBlt( hdcMem, 0, 0, w, h, hdcWin, 0, 0, SRCCOPY );
    }

    TArray< uint8_t > out;
    if( ok )
    {
        BITMAPINFOHEADER bi{};
        bi.biSize        = sizeof( bi );
        bi.biWidth       = w;
        bi.biHeight      = h; // bottom-up DIB
        bi.biPlanes      = 1;
        bi.biBitCount    = 32;
        bi.biCompression = BI_RGB;

        TArray< uint8_t > bgra( size_t( w ) * size_t( h ) * 4u, true );
        if( GetDIBits( hdcMem,
                       hbm,
                       0,
                       UINT( h ),
                       bgra.Data(),
                       reinterpret_cast< BITMAPINFO* >( &bi ),
                       DIB_RGB_COLORS ) )
        {
            // Reject obviously empty / failed captures (all black).
            uint64_t sum = 0;
            for( int i = 0; i < w * h; ++i )
            {
                sum += bgra[ size_t( i ) * 4u + 0u ];
                sum += bgra[ size_t( i ) * 4u + 1u ];
                sum += bgra[ size_t( i ) * 4u + 2u ];
            }
            if( sum > 0 )
            {
                out.Resize( size_t( w ) * size_t( h ) * 3u );
                for( int y = 0; y < h; ++y )
                {
                    const uint8_t* src = bgra.Data() + size_t( y ) * size_t( w ) * 4u;
                    uint8_t*       dst = out.Data() + size_t( h - 1 - y ) * size_t( w ) * 3u;
                    for( int x = 0; x < w; ++x )
                    {
                        dst[ x * 3 + 0 ] = src[ x * 4 + 2 ];
                        dst[ x * 3 + 1 ] = src[ x * 4 + 1 ];
                        dst[ x * 3 + 2 ] = src[ x * 4 + 0 ];
                    }
                }
                pitch      = w * 3;
                color_type = SS_RGB;
                gamma      = 1.0f;
            }
        }
    }

    SelectObject( hdcMem, old );
    DeleteObject( hbm );
    DeleteDC( hdcMem );
    ReleaseDC( hwnd, hdcWin );
    return out;
}

void Win32RTVideo::Shutdown()
{
    if( !rt.rgDestroyInstance )
    {
        return;
    }

    RgResult r = rt.rgDestroyInstance();
    if( r != RG_RESULT_SUCCESS )
    {
        MessageBoxA(
            nullptr, "rgDestroyAndUnloadLibrary has failed", "Fail", MB_ICONEXCLAMATION | MB_OK );
        exit( -1 );
    }

    rt = {};
}

void RT_ShowWarningMessageBox( const char* msg )
{
#ifdef _WIN32
    MessageBoxA( g_msgbox_parent.load(), msg, "Warning - Ray Tracing", MB_ICONEXCLAMATION | MB_OK );
#else
    assert( 0 );
#endif
}

bool RT_AskToOpenUrl( const char* heading, const char* msg, const wchar_t* url )
{
    int l = MessageBoxA( g_msgbox_parent.load(), msg, heading, MB_ICONEXCLAMATION | MB_YESNO );
    if( l == IDYES )
    {
        ShellExecute( nullptr, 0, url, 0, 0, SW_SHOW );
        return true;
    }
    return false;
}

//
//
//

auto RT_GetCurrentTime() -> double
{
    auto ns = []() {
        using namespace std::chrono;
        return duration_cast< nanoseconds >( steady_clock::now().time_since_epoch() ).count();
    };

    static int64_t startupTimeNS = ns();
    return static_cast< double >( ns() - startupTimeNS ) / 1000000000.0;
}

auto RT_GetVramUsage( bool* ok ) -> const char*
{
    const RgUtilMemoryUsage vram = rt.rgUtilRequestMemoryUsage();

    if( ok )
    {
        // < 80% is ok
        *ok = ( vram.vramUsed <= 0.8 * vram.vramTotal );
    }

    static char buf[ 64 ];
    snprintf( buf,
              std::size( buf ),
              "%d / %d MB",
              int( std::round( double( vram.vramUsed ) / 1024 / 1024 ) ),
              int( std::round( double( vram.vramTotal ) / 1024 / 1024 ) ) );

    buf[ std::size( buf ) - 1 ] = '\0';
    return buf;
}

namespace
{

RgExtent2D RT_GetCurrentWindowSize()
{
    return {
        static_cast< uint32_t >( screen->GetWidth() ),
        static_cast< uint32_t >( screen->GetHeight() ),
    };
}

void RT_ResolutionToRtgl( RgStartFrameRenderResolutionParams* dst, const RgExtent2D winsize )
{
    const auto aspect =
        static_cast< double >( winsize.width ) / static_cast< double >( winsize.height );

    if( cvar::rt_renderscale > 0.2f )
    {
        auto scale = std::clamp( double( *cvar::rt_renderscale ), 0.2, 1.0 );

        dst->customRenderSize.width    = static_cast< uint32_t >( winsize.width * scale );
        dst->customRenderSize.height   = static_cast< uint32_t >( winsize.height * scale );
        dst->pixelizedRenderSizeEnable = false;

        return;
    }
    else
    {
        if( int{ cvar::rt_ef_vintage } != RT_VINTAGE_OFF )
        {
            uint32_t h_pixelized = 0;
            uint32_t h_render    = 0;

            switch( int{ cvar::rt_ef_vintage } )
            {
                case RT_VINTAGE_200:
                case RT_VINTAGE_200_DITHER:
                    h_pixelized = 200;
                    h_render    = 400;
                    break;

                case RT_VINTAGE_480:
                case RT_VINTAGE_480_DITHER:
                    h_pixelized = 480;
                    h_render    = 600;
                    break;

                case RT_VINTAGE_CRT:
                case RT_VINTAGE_VHS:
                case RT_VINTAGE_VHS_CRT:
                    h_pixelized = 480;
                    h_render    = 480;
                    break;

                default:
                    cvar::rt_ef_vintage            = 0;
                    dst->customRenderSize          = winsize;
                    dst->pixelizedRenderSizeEnable = false;
                    return;
            }

            assert( h_render > 0 && h_pixelized > 0 );

            dst->pixelizedRenderSize.height = h_pixelized;
            dst->pixelizedRenderSize.width  = static_cast< uint32_t >( h_pixelized * aspect );
            dst->pixelizedRenderSizeEnable  = true;
            dst->customRenderSize.height    = h_render;
            dst->customRenderSize.width     = static_cast< uint32_t >( h_render * aspect );

            return;
        }
    }

    dst->customRenderSize          = winsize;
    dst->pixelizedRenderSizeEnable = false;
}

auto RT_GetSharpenTechniqueFromCvar( bool dlssOrFsr2 ) -> RgRenderSharpenTechnique
{
    switch( cvar::rt_sharpen )
    {
        case 3: return RG_RENDER_SHARPEN_TECHNIQUE_NONE;
        case 2: return RG_RENDER_SHARPEN_TECHNIQUE_AMD_CAS;
        case 1: return RG_RENDER_SHARPEN_TECHNIQUE_NAIVE;
        default: {
            if( dlssOrFsr2 )
            {
                return RG_RENDER_SHARPEN_TECHNIQUE_AMD_CAS;
            }
            // to accentuate a chunky look, because of the linear (not nearest) downscale mode
            switch( cvar::rt_ef_vintage )
            {
                case RT_VINTAGE_CRT:
                case RT_VINTAGE_VHS:
                case RT_VINTAGE_VHS_CRT: return RG_RENDER_SHARPEN_TECHNIQUE_NAIVE;
                case RT_VINTAGE_200:
                case RT_VINTAGE_200_DITHER:
                case RT_VINTAGE_480:
                case RT_VINTAGE_480_DITHER: return RG_RENDER_SHARPEN_TECHNIQUE_AMD_CAS;
                default: return RG_RENDER_SHARPEN_TECHNIQUE_NONE;
            }
        }
    }
}

// Snapshot of the last RT_UpscaleCvarsToRtgl() decision, for rt_rr_status.
static bool g_rr_dbg_isremix     = false;
static bool g_rr_dbg_wantNative  = false;
static int  g_rr_dbg_nvDlss      = 0;
static bool g_rr_dbg_rrRequested = false;

void RT_UpscaleCvarsToRtgl( RgStartFrameRenderResolutionParams* pDst )
{
    cvar::rt_available_dlss2 =
        rt.rgUtilIsUpscaleTechniqueAvailable( RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS,
                                              RG_FRAME_GENERATION_MODE_OFF,
                                              &cvar::rt_failreason_dlss2 );
    cvar::rt_available_dlss3fg =
        rt.rgUtilIsUpscaleTechniqueAvailable( RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS,
                                              RG_FRAME_GENERATION_MODE_ON,
                                              &cvar::rt_failreason_dlss3fg );
    cvar::rt_available_fsr2 =
        rt.rgUtilIsUpscaleTechniqueAvailable( RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2,
                                              RG_FRAME_GENERATION_MODE_OFF,
                                              &cvar::rt_failreason_fsr2 );
    cvar::rt_available_fsr3fg =
        rt.rgUtilIsUpscaleTechniqueAvailable( RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2,
                                              RG_FRAME_GENERATION_MODE_ON,
                                              &cvar::rt_failreason_fsr3fg );
    cvar::rt_available_dxgi = rt.rgUtilDXGIAvailable( &cvar::rt_failreason_dxgi );

    const RgFeatureFlags features = rt.rgUtilGetSupportedFeatures();

    cvar::rt_hdr_available   = ( features & RG_FEATURE_HDR );
    cvar::rt_fluid_available = ( features & RG_FEATURE_FLUID );

    int nvDlss = cvar::rt_available_dlss2 || cvar::rt_available_dlss3fg //
                     ? int( cvar::rt_upscale_dlss )
                     : 0;
    int amdFsr = cvar::rt_available_fsr2 || cvar::rt_available_fsr3fg //
                     ? int( cvar::rt_upscale_fsr2 )
                     : 0;

    // Native Ray Reconstruction needs a DLSS quality mode; default to Balanced.
    const bool wantNativeRr = !g_isremix && bool( cvar::rt_rayreconstr );
    if( wantNativeRr && nvDlss == 0 && ( cvar::rt_available_dlss2 || cvar::rt_available_dlss3fg ) )
    {
        nvDlss                = 2;
        cvar::rt_upscale_dlss = 2;
    }

    // DLSS and FSR2 both write pDst->upscaleTechnique and the FSR switch below
    // runs *second*, so a non-zero rt_upscale_fsr2 silently overwrites the DLSS
    // choice. rayReconstruction is still set afterwards (it only tests
    // nvDlss != 0), so gzdoom would hand RTGL "upscaler=FSR2 + RR=on" -- a
    // contradiction RTGL resolves by quietly dropping RR and running A-SVGF.
    //
    // rt_upscale_fsr2 is CVAR_ARCHIVE like every RT_CVAR, so a stale 2 in the
    // ini disabled Ray Reconstruction across every launch while rt_rayreconstr
    // still read 1 (2026-08-07). DLSS wins when both are set; RR depends on it.
    if( nvDlss != 0 && amdFsr != 0 )
    {
        static bool s_warned = false;
        if( !s_warned )
        {
            s_warned = true;
            Printf( "RT: both rt_upscale_dlss (%d) and rt_upscale_fsr2 (%d) are set; "
                    "they share one upscaler slot. Using DLSS and ignoring FSR2 "
                    "(Ray Reconstruction requires DLSS). Set rt_upscale_fsr2 0 to silence.\n",
                    nvDlss,
                    amdFsr );
        }
        amdFsr = 0;
    }

    switch( nvDlss )
    {
        case 1:
            // start with Quality
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_QUALITY;
            break;
        case 2:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_BALANCED;
            break;
        case 3:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_PERFORMANCE;
            break;
        case 4:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE;
            break;

        case 5:
            // use DLSS with rt_renderscale
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_CUSTOM;
            break;

        case 6:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_NATIVE_AA;
            break;

        default: nvDlss = 0; break;
    }

    switch( amdFsr )
    {
        case 1:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_QUALITY;
            break;
        case 2:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_BALANCED;
            break;
        case 3:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_PERFORMANCE;
            break;
        case 4:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE;
            break;

        case 5:
            // use FSR2 with rt_renderscale
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_CUSTOM;
            break;

        case 6:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_NATIVE_AA;
            break;

        default: amdFsr = 0; break;
    }

    // both disabled
    if( nvDlss == 0 && amdFsr == 0 )
    {
        pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NEAREST;
        pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_CUSTOM;
        pDst->frameGeneration  = RG_FRAME_GENERATION_MODE_OFF;
    }
    else
    {
        if( ( nvDlss != 0 && cvar::rt_available_dlss3fg ) ||
            ( amdFsr != 0 && cvar::rt_available_fsr3fg ) )
        {
            switch( cvar::rt_framegen )
            {
                case -1: pDst->frameGeneration = RG_FRAME_GENERATION_MODE_WITHOUT_GENERATED; break;
                case 1: pDst->frameGeneration = RG_FRAME_GENERATION_MODE_ON; break;
                default: pDst->frameGeneration = RG_FRAME_GENERATION_MODE_OFF; break;
            }
        }
        else
        {
            pDst->frameGeneration = RG_FRAME_GENERATION_MODE_OFF;
        }
    }

    // Native RR replaces A-SVGF + DLSS-SR; Frame Gen is out of scope for MVP.
    // Gate on the technique that actually survived both switches above, not on
    // nvDlss alone: RTGL drops rayReconstruction whenever the upscaler isn't
    // DLSS (RenderResolutionHelper::Setup), so requesting RR alongside any
    // other upscaler is a contradiction that silently costs the denoiser.
    pDst->rayReconstruction = 0;
    if( wantNativeRr && nvDlss != 0 &&
        pDst->upscaleTechnique == RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS )
    {
        pDst->rayReconstruction = 1;
        pDst->frameGeneration   = RG_FRAME_GENERATION_MODE_OFF;
        if( int( cvar::rt_framegen ) != 0 )
        {
            cvar::rt_framegen = 0;
        }
    }

    // Cached for the rt_rr_status CCMD (RTGL's own DLSSRR messages are muted
    // unless -rtdebug, so this is the only in-game view of the decision chain).
    g_rr_dbg_isremix     = g_isremix;
    g_rr_dbg_wantNative  = wantNativeRr;
    g_rr_dbg_nvDlss      = nvDlss;
    g_rr_dbg_rrRequested = ( pDst->rayReconstruction != 0 );

    // Report the decision the FIRST time it is actually computed, and on every
    // later change. Running `rt_rr_status` from the command line reads the
    // cached globals above before this function has ever run, so it reports
    // startup defaults (DLSS2 available = NO) that look like a real negative --
    // another way this decision chain lied. The failure reason from
    // rgUtilIsUpscaleTechniqueAvailable is printed here because nothing else
    // ever surfaced it at frame time.
    {
        static bool s_have = false;
        static int  s_prev = -1;

        const int state = ( int( bool( cvar::rt_available_dlss2 ) ) << 0 ) |
                          ( int( bool( cvar::rt_available_dlss3fg ) ) << 1 ) |
                          ( int( wantNativeRr ) << 2 ) |
                          ( int( pDst->rayReconstruction != 0 ) << 3 ) | ( nvDlss << 4 );

        if( !s_have || s_prev != state )
        {
            s_have = true;
            s_prev = state;

            Printf( "RT upscale/RR decision: DLSS2=%s DLSS3FG=%s nvDlss=%d "
                    "wantNativeRr=%s -> rayReconstruction=%s\n",
                    cvar::rt_available_dlss2 ? "yes" : "NO",
                    cvar::rt_available_dlss3fg ? "yes" : "NO",
                    nvDlss,
                    wantNativeRr ? "yes" : "no",
                    pDst->rayReconstruction ? "ON" : "OFF" );

            if( !cvar::rt_available_dlss2 && cvar::rt_failreason_dlss2 )
            {
                Printf( "  DLSS2 unavailable, reason: %s\n",
                        static_cast< const char* >( cvar::rt_failreason_dlss2 ) );
            }
        }
    }

    pDst->sharpenTechnique = RT_GetSharpenTechniqueFromCvar( amdFsr || nvDlss );
}

template< typename T >
    requires( std::is_same_v< T, int > )
uint32_t safe_uint( T x )
{
    return static_cast< uint32_t >( std::max< int >( x, 0 ) );
}

} // anonymous namespace

//
//
//

RTFrameBuffer::RTFrameBuffer( void* hMonitor, bool fullscreen )
    : SystemBaseFrameBuffer( hMonitor, fullscreen ), m_state{ new RTRenderState{ this } }
{
}
RTFrameBuffer::~RTFrameBuffer()
{
    delete m_state;
    delete mVertexData;
    delete mSkyData;
    delete mViewpoints;
    delete mLights;
    delete mBones;
}
void RTFrameBuffer::InitializeState()
{
    m_state      = new RTRenderState{ this };
    vendorstring = "RT";
    mVertexData  = new FFlatVertexBuffer( GetWidth(), GetHeight(), screen->mPipelineNbr );
    mSkyData     = new FSkyVertexBuffer;
    mViewpoints  = new HWViewpointBuffer( screen->mPipelineNbr );
    mLights      = new FLightBuffer( screen->mPipelineNbr );
    mBones       = new BoneBuffer( screen->mPipelineNbr );
}

void RTFrameBuffer::FirstEye()
{
    m_state->RT_AddMainCamera( r_viewpoint );
    Super::FirstEye();
}

FRenderState* RTFrameBuffer::RenderState()
{
    return m_state;
}
IVertexBuffer* RTFrameBuffer::CreateVertexBuffer()
{
    return new RTVertexBuffer{};
}
IIndexBuffer* RTFrameBuffer::CreateIndexBuffer()
{
    return new RTIndexBuffer{};
}
IDataBuffer* RTFrameBuffer::CreateDataBuffer( int bindingpoint, bool ssbo, bool needsresize )
{
    return new RTDataBuffer{};
}
IHardwareTexture* RTFrameBuffer::CreateHardwareTexture( int numchannels )
{
    return new RTHardwareTexture{};
}
void RTFrameBuffer::Draw2D()
{
    ::Draw2D( twod, *m_state );
}

//
//
//

namespace
{
constexpr auto remap01( float v, float newmin, float newmax )
{
    assert( newmax > newmin );
    return newmin + std::clamp( v, 0.f, 1.f ) * ( newmax - newmin );
}

auto RT_GetPlayer() -> player_t*
{
    return players[ consoleplayer ].camera ? players[ consoleplayer ].camera->player : nullptr;
}

auto RT_DamageIntensity() -> std::optional< float >
{
    // for reference https://doom.fandom.com/wiki/Comparison_of_Doom_monsters
    constexpr float maxdmg = 100.f;

    if( auto player = RT_GetPlayer() )
    {
        if( player->damagecount > 0 )
        {
            float dmg01 =
                std::clamp( static_cast< float >( player->damagecount ) / maxdmg, 0.f, 1.f );

            // smaller damage should also have effect
            dmg01 = sqrt( dmg01 );

            assert( dmg01 > 0.005f );
            return dmg01;
        }
    }
    return {};
}

uint32_t RT_CalcPowerupFlags()
{
    auto player = RT_GetPlayer();
    if( !player )
    {
        return 0;
    }

    uint32_t powerups = 0;

    for( AActor* in = player->mo->Inventory; in; in = in->Inventory )
    {
        if( in->IsKindOf( NAME_PowerStrength ) )
        {
            if( rtstate.m_berserkBlend > 10 )
            {
                powerups |= RT_POWERUP_FLAG_BERSERK_BIT;
            }
        }
        else if( in->IsKindOf( NAME_PowerIronFeet ) )
        {
            powerups |= RT_POWERUP_FLAG_RADIATIONSUIT_BIT;
        }
        else if( in->IsKindOf( NAME_PowerInvulnerable ) )
        {
            powerups |= RT_POWERUP_FLAG_INVUNERABILITY_BIT;
        }
        else if( in->IsKindOf( NAME_PowerLightAmp ) )
        {
            switch( *cvar::rt_pw_lightamp )
            {
                case 1: powerups |= RT_POWERUP_FLAG_THERMALVISION_BIT; break;
                case 2: powerups |= RT_POWERUP_FLAG_FLASHLIGHT_BIT; break;
                default: powerups |= RT_POWERUP_FLAG_NIGHTVISION_BIT; break;
            }
        }
        else if( in->IsKindOf( NAME_PowerInvisibility ) )
        {
            powerups |= RT_POWERUP_FLAG_INVISIBILITY_BIT;
        }

        // NAME_PowerTargeter
        // NAME_PowerWeaponLevel2
        // NAME_PowerFlight
        // NAME_PowerSpeed
        // NAME_PowerTorch
        // NAME_PowerHighJump
        // NAME_PowerReflection
        // NAME_PowerDrain
        // NAME_PowerScanner
        // NAME_PowerDoubleFiringSpeed
        // NAME_PowerInfiniteAmmo
        // NAME_PowerBuddha
    }

    if( player->bonuscount > 0 )
    {
        powerups |= RT_POWERUP_FLAG_BONUS_BIT;
    }

    return powerups;
}
} // anonymous namespace

//
//
//

static bool   g_resetposteffects = false;
static bool   g_resetfluid       = false;
static bool   g_melt_requested   = false;
static double g_melt_endtime     = -1;
bool          g_noinput_onstart  = true;

bool   g_cpu_latency_get = false;
double g_cpu_latency     = 0;

static void RT_DrawTitle();
static void RT_ClearTitles();
static void RT_InjectTitleIntoDoomMap( const char* mapname );

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
// intensity < 0 means "leave the launcher's value alone" -- most entries only
// want to turn the moon, not rebalance the level's brightness.
//
// To add one: play the map, aim it with `moon <az> [alt]`, then type `moon` and
// paste the row it prints. Maps with no entry fall back to the launcher's
// rt_sun_a/b, captured on the first level load (see g_moon_base_* below) so that
// a preset on one map cannot leak into the next.
namespace
{
struct MoonPreset
{
    const char* map;
    float       azimuth;
    float       altitude;
    float       intensity; // < 0: keep whatever the launcher pinned
    const char* note;
};

constexpr MoonPreset RT_MOON_PRESETS[] = {
    { "map13", 90.f, 25.f, -1.f,
      "Due north. Settled in play. The painted shafts this replaced implied two "
      "different suns -- the west hall's fans want light travelling +x, the north "
      "colonnade's want -y -- and 135 was the geometric compromise between them. "
      "90 reads better than the compromise did: it rakes hard through the north "
      "colonnade and still catches the west windows obliquely." },
};

// The launcher's aim, captured once before any preset overwrites it, so a map
// with no entry gets the global default back instead of inheriting whatever the
// last map with an entry set. Without this the table would be sticky in one
// direction and the fallback would silently become "the last preset visited".
bool  g_moon_base_set = false;
float g_moon_base_a   = 0.f;
float g_moon_base_b   = 0.f;
float g_moon_base_i   = 0.f;

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

void RT_ApplyMoonPreset( const char* mapname )
{
    if( !g_moon_base_set )
    {
        g_moon_base_set = true;
        g_moon_base_a   = float{ cvar::rt_sun_a };
        g_moon_base_b   = float{ cvar::rt_sun_b };
        g_moon_base_i   = float{ cvar::rt_sun_intensity };
    }

    if( !bool{ cvar::rt_moon_presets } )
    {
        return;
    }

    const MoonPreset* p = RT_FindMoonPreset( mapname );

    cvar::rt_sun_a         = p ? p->altitude : g_moon_base_a;
    cvar::rt_sun_b         = p ? p->azimuth : g_moon_base_b;
    cvar::rt_sun_intensity = ( p && p->intensity >= 0.f ) ? p->intensity : g_moon_base_i;
}
} // namespace

void RT_OnLevelLoad( const char* mapname )
{
    RT_ApplyMoonPreset( mapname );
    g_resetposteffects = true;
    g_resetfluid       = true;
    g_rt_lightcut      = true; // DLSS-RR: new scene, flush temporal history unconditionally
    g_rt_lightcut_why  = "levelload";
    RT_ClearTitles();
    RT_InjectTitleIntoDoomMap( mapname );
    RT_ForceIntroCutsceneMusicStop();
}

void RT_RequestMelt()
{
    // HACKHACK: suppress melting when getting into the first start
    {
        static bool first = true;
        if( first )
        {
            first = false; 
            return;
        }
    }
    g_melt_requested = true;
}

bool RT_IsMeltActive()
{
    return g_melt_endtime > 0 && RT_GetCurrentTime() < g_melt_endtime;
}
bool RT_IgnoreUserInput()
{
    return RT_IsMeltActive() || g_noinput_onstart;
}

static double CalcCpuLatency()
{
    static double   g_lprevtime            = RT_GetCurrentTime();
    static double   g_lprevlatencies[ 30 ] = {};
    static uint32_t g_lprevi               = 0;

    double lcurtime = RT_GetCurrentTime();

    g_lprevlatencies[ g_lprevi ] = lcurtime - g_lprevtime;

    g_lprevi    = ( g_lprevi + 1 ) % std::size( g_lprevlatencies );
    g_lprevtime = lcurtime;

    double sum = 0;
    int    cnt = 0;
    for( double t : g_lprevlatencies )
    {
        if( t > 0 )
        {
            sum += t;
            cnt++;
        }
    }

    return cnt > 0 ? sum / cnt : 0;
}

namespace
{
template< typename T >
T smoothstep( T edge0, T edge1, T x )
{
    T t = std::clamp( ( x - edge0 ) / ( edge1 - edge0 ), T( 0 ), T( 1 ) );
    return t * t * ( T( 3 ) - T( 2 ) * t );
}

namespace classic_toggle
{
    constexpr double Duration  = 0.75;
    double           g_timeend = 0.0;

    float                  g_source = 0.0f;
    std::optional< float > g_target = {};

    // Why is DLSS Ray Reconstruction on/off? RTGL's own DLSSRR messages are
    // suppressed unless gzdoom is launched with -rtdebug, so this prints the
    // whole gzdoom-side decision chain that feeds
    // RgStartFrameRenderResolutionParams::rayReconstruction.
    CCMD( rt_rr_status )
    {
        Printf( "--- DLSS Ray Reconstruction status ---\n" );
        Printf( "  rt_rayreconstr        = %d  (user request)\n", int( bool( cvar::rt_rayreconstr ) ) );
        Printf( "  rt_upscale_dlss       = %d  (0 = off; RR needs != 0)\n", int( cvar::rt_upscale_dlss ) );
        Printf( "  remix mode            = %s  (RR is native-only, disabled under Remix)\n",
                g_rr_dbg_isremix ? "YES" : "no" );
        Printf( "  DLSS2 available       = %s%s%s\n",
                cvar::rt_available_dlss2 ? "YES" : "NO",
                ( !cvar::rt_available_dlss2 && cvar::rt_failreason_dlss2 ) ? "  reason: " : "",
                ( !cvar::rt_available_dlss2 && cvar::rt_failreason_dlss2 ) ? cvar::rt_failreason_dlss2
                                                                          : "" );
        Printf( "  DLSS3-FG available    = %s%s%s\n",
                cvar::rt_available_dlss3fg ? "YES" : "NO",
                ( !cvar::rt_available_dlss3fg && cvar::rt_failreason_dlss3fg ) ? "  reason: " : "",
                ( !cvar::rt_available_dlss3fg && cvar::rt_failreason_dlss3fg )
                    ? cvar::rt_failreason_dlss3fg
                    : "" );
        Printf( "  -> wantNativeRr       = %s\n", g_rr_dbg_wantNative ? "YES" : "no" );
        Printf( "  -> nvDlss (mode)      = %d\n", g_rr_dbg_nvDlss );
        Printf( "  -> RR REQUESTED       = %s\n", g_rr_dbg_rrRequested ? "YES" : "NO" );
        Printf( "\n" );
        // Everything above is what gzdoom ASKS FOR. RTGL's Dev UI can silently
        // replace it afterwards, so none of it proves what actually ran.
        Printf( "  NOTE: this is gzdoom's REQUEST, not the applied state. RTGL's Dev UI\n"
                "  can override it -- a sticky \"DLSS Ray Reconstruction\" checkbox wins\n"
                "  even with the Override master switch OFF, and used to persist across\n"
                "  launches in rt/devmode_settings.json. That made this command report\n"
                "  \"RR REQUESTED = YES\" through several sessions that actually ran A-SVGF\n"
                "  (2026-08-07). RTGL now resets sticky flags on load and warns (-rtdebug)\n"
                "  whenever it overrides this request. To be certain: launch with -rtdebug\n"
                "  and check for a \"Dev override: DLSS Ray Reconstruction forced ...\" line,\n"
                "  or delete rt/devmode_settings.json.\n" );
        Printf( "\n" );
        if( !g_rr_dbg_rrRequested )
        {
            Printf( "  RR is NOT requested -> A-SVGF denoiser runs (image should be smooth).\n" );
        }
        else
        {
            Printf( "  RR IS requested. If the image is still raw/noisy, RTGL accepted the\n"
                    "  request but DLSSRR::Apply() bailed (VulkanDevice.cpp skips A-SVGF\n"
                    "  whenever the nvDlssRr object merely exists) -> no denoiser at all.\n"
                    "  Relaunch with -rtdebug to see the DLSSRR: lines from RTGL.\n" );
        }
    }

    // Aim the moon from the console: `moon <azimuth> [altitude] [intensity]`.
    //
    // The moon is two things that have to agree -- an analytic directional light
    // (rt_sun_*) that casts the shafts, and a disc painted into the sky texture
    // that you can actually see. Setting rt_sun_b alone swings the shafts away
    // from the disc. This moves both: the angles here, and the sky rotation that
    // carries the disc, via rt_moon_track in R_UpdateSky.
    //
    // Bare `moon` prints the current aim rather than changing it, because the
    // first thing you want after walking into a room is to know where it thinks
    // the moon is.
    CCMD( moon )
    {
        auto report = []() {
            Printf( "moon: azimuth %.1f, altitude %.1f, intensity %.0f, %s\n",
                    float{ cvar::rt_sun_b },
                    float{ cvar::rt_sun_a },
                    float{ cvar::rt_sun_intensity },
                    bool{ cvar::rt_sun } ? "ON" : "OFF (set rt_sun 1)" );
            Printf( "  sky: painted at %.1f, tracking %s, yawsign %+.0f, extra yaw %.1f\n",
                    float{ cvar::rt_moon_tex_b },
                    bool{ cvar::rt_moon_track } ? "ON" : "OFF (disc will not follow)",
                    float{ cvar::rt_moon_yawsign },
                    float{ cvar::rt_sky_yaw } );
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
                Printf( "    { \"%s\", %.0ff, %.0ff, -1.f, \"...\" },\n",
                        mn, float{ cvar::rt_sun_b }, float{ cvar::rt_sun_a } );
            }

            Printf( "  usage: moon <azimuth 0..360> [altitude -90..90] [intensity]\n"
                    "         moon flip     - disc moves the wrong way? reverse it\n"
                    "         moon nudge <deg> - shift the disc alone, to calibrate\n" );
            return;
        }

        // The sign of the sky rotation cancels out of any derivation off the dome
        // vertices (it mirrors in x AND negates u), so it is settled by looking at
        // it, not by reasoning. One word, then look again.
        if( stricmp( argv[ 1 ], "flip" ) == 0 )
        {
            cvar::rt_moon_yawsign = -float{ cvar::rt_moon_yawsign };
            Printf( "moon: yawsign now %+.0f\n", float{ cvar::rt_moon_yawsign } );
            return;
        }

        // Calibration: with the light where you want it, walk the disc until the
        // two line up, then keep the number in the launcher as +rt_sky_yaw.
        if( stricmp( argv[ 1 ], "nudge" ) == 0 )
        {
            if( argv.argc() < 3 )
            {
                Printf( "moon nudge: needs degrees, e.g. `moon nudge 15`\n" );
                return;
            }
            cvar::rt_sky_yaw = float{ cvar::rt_sky_yaw } + float( atof( argv[ 2 ] ) );
            Printf( "moon: sky yaw %.1f (pin it with +rt_sky_yaw %.1f)\n",
                    float{ cvar::rt_sky_yaw }, float{ cvar::rt_sky_yaw } );
            return;
        }

        cvar::rt_sun_b = float( fmod( atof( argv[ 1 ] ), 360.0 ) );
        if( argv.argc() >= 3 )
        {
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

    CCMD( rt_dump_dynlights )
    {
        if( !primaryLevel || !primaryLevel->lights )
        {
            Printf( "rt_dump_dynlights: no level / no light list\n" );
            return;
        }
        unsigned n = 0;
        for( FDynamicLight* light = primaryLevel->lights; light != nullptr; light = light->next )
        {
            if( !light->IsActive() || light->X() < -1.0e6 )
            {
                continue;
            }
            Printf(
                "  [%u] pos=(%.0f,%.0f,%.0f) rgb=(%d,%d,%d) radius=%.1f active=%d\n",
                n,
                light->X(),
                light->Y(),
                light->Z(),
                light->GetRed(),
                light->GetGreen(),
                light->GetBlue(),
                light->m_currentRadius,
                light->IsActive() ? 1 : 0 );
            ++n;
        }
        Printf( "rt_dump_dynlights: %u listed (GZDoom FDynamicLight chain)\n", n );
    }

    CCMD( rt_classic_toggle )
    {
        if( g_isremix )
        {
            cvar::rt_classic = 0; 
            return;
        }

        g_timeend = RT_GetCurrentTime() + Duration;
        g_source  = std::clamp< float >( cvar::rt_classic, 0, 1 );

        if( g_target )
        {
            g_target = g_target.value() > 0 ? 0.f : 1.f;
        }
        else
        {
            g_target = cvar::rt_classic > 0 ? 0.f : 1.f;
        }
    }

    void Animate()
    {
        if( g_isremix )
        {
            cvar::rt_classic = 0;
            return;
        }

        if( g_target )
        {
            double dt = g_timeend - RT_GetCurrentTime();
            if( dt <= 0 )
            {
                cvar::rt_classic = *g_target;
                g_target         = {};
                return;
            }

            double ratio = 1 - std::clamp( dt / Duration, 0.0, 1.0 );
            ratio        = smoothstep( 0.0, 1.0, ratio );

            cvar::rt_classic = std::lerp( g_source, *g_target, static_cast< float >( ratio ) );
        }
    }
} // namespace classic_toggle

auto g_sectorlightlevels = std::vector< uint8_t >{};

void RT_MakeLightstyles()
{
    if( !primaryLevel || primaryLevel->sectors.Size() == 0 )
    {
        g_sectorlightlevels.clear();
        return;
    }
    g_sectorlightlevels.resize( primaryLevel->sectors.Size() );

    for( uint32_t i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        g_sectorlightlevels[ i ] = uint8_t( std::clamp( //
            primaryLevel->sectors[ i ].GetLightLevel(),
            0,
            255 ) );
    }
}

static bool RT_IsSectorLightChangingSpecial( int special )
{
    return ( special == Light_Phased ) || ( special == LightSequenceStart ) ||
           ( special == LightSequenceSpecial1 ) || ( special == LightSequenceSpecial2 ) ||
           ( special == dLight_Flicker ) || ( special == dLight_StrobeFast ) ||
           ( special == dLight_StrobeSlow ) || ( special == dLight_Strobe_Hurt ) ||
           ( special == dLight_Glow ) || ( special == dLight_StrobeSlowSync ) ||
           ( special == dLight_StrobeFastSync ) || ( special == dLight_FireFlicker ) ||
           ( special == sLight_Strobe_Hurt ) || ( special == Light_OutdoorLightning ) ||
           ( special == Light_IndoorLightning1 ) || ( special == Light_IndoorLightning2 );
}

void RT_UploadExportableSectorLights()
{
    // Stock path uploads a white sphere at every sector center. On Retribution that
    // was a lingering wash. Two modes:
    //   rt_sector_lights  = all sectors (stock / Doom II export)
    //   rt_sector_flicker = only sectors with light-changing specials (blink without wash)
    //
    // MAP01 note: spawn booth ceilings (SFLATAS) have special 0 / steady lightlevel —
    // sector lights do NOT blink them. The wall SMON alcoves are dLight_Flicker (65)
    // and DO blink with this path — that is the wrong target for "head lights".
    const bool allSectors   = bool{ cvar::rt_sector_lights };
    const bool flickerOnly  = bool{ cvar::rt_sector_flicker };
    if( !allSectors && !flickerOnly )
    {
        return;
    }
    if( !primaryLevel )
    {
        return;
    }
    // BeginFrame normally fills this; rebuild if DrawFrame somehow races ahead.
    if( g_sectorlightlevels.size() != primaryLevel->sectors.Size() )
    {
        RT_MakeLightstyles();
    }
    if( g_sectorlightlevels.size() != primaryLevel->sectors.Size() )
    {
        return;
    }

    assert( g_sectorlightlevels.size() == primaryLevel->sectors.Size() );

    for( uint32_t i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        const sector_t& sector = primaryLevel->sectors[ i ];

        if( !allSectors && !RT_IsSectorLightChangingSpecial( sector.special ) )
        {
            continue;
        }

        float z;
        {
            auto zfloor   = float( sector.floorplane.ZatPoint( sector.centerspot ) );
            auto zceiling = float( sector.ceilingplane.ZatPoint( sector.centerspot ) );

            // if too thin
            if( std::abs( zfloor - zceiling ) < 0.1f )
            {
                if( !RT_IsSectorLightChangingSpecial( sector.special ) )
                {
                    continue;
                }
            }

            z = ( zfloor + zceiling ) / 2;
        }

        // Flicker-only path uses a milder intensity than stock autoexport (200)
        // so we get blink + cast without room-wide wash.
        const float intensity =
            allSectors ? float{ cvar::rt_autoexport_light }
                       : std::min( 80.f, float{ cvar::rt_autoexport_light } * 0.35f );

        const auto center = FVector3{
            float( sector.centerspot.X ),
            float( sector.centerspot.Y ),
            z,
        };

        auto adt = RgLightAdditionalEXT{
            .sType      = RG_STRUCTURE_TYPE_LIGHT_ADDITIONAL_EXT,
            .pNext      = nullptr,
            .flags      = RG_LIGHT_ADDITIONAL_LIGHTSTYLE,
            .lightstyle = int( i ), // references g_sectorlightlevels
            .hashName   = "",
        };

        // Was RG_PACKED_COLOR_WHITE — that is the "fake white wash" this cvar's own
        // description warns about. A Doom 64 sector's light IS its colormap color, so
        // carry the hue: a red corridor gets a red sector light, not a white one.
        const FVector3 hue =
            RT_SectorHue( sector.Colormap.LightColor, float{ cvar::rt_sector_tint_lights } );

        auto lsph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = &adt,
            .color     = rt.rgUtilPackColorFloat4D( hue.X, hue.Y, hue.Z, 1.0f ),
            .intensity = intensity,
            .position  = { center.X * ONEGAMEUNIT_IN_METERS,
                           center.Y * ONEGAMEUNIT_IN_METERS,
                           center.Z * ONEGAMEUNIT_IN_METERS },
            .radius    = 0.05f,
        };

        auto linfo = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &lsph,
            .uniqueID     = SectorLightId_Base + i,
            .isExportable = allSectors, // flicker-only lights are runtime-only
        };

        RgResult r = rt.rgUploadLight( &linfo );
        RG_CHECK( r );
    }
}

void RT_UploadGzDoomDynamicLights()
{
    // DLSS-RR: previous frame's set of uploaded light IDs, for the
    // appear/disappear diff at the bottom of this function.
    static std::unordered_set< uint64_t > s_prevDynIds;

    // Stock gzdoom-rt never forwarded FDynamicLight (map things 9800/9802, GLDEFS
    // attached lights) into RTGL. Retribution MAP01 spawn blink lamps are
    // PointLightFlicker (9802) beside SMONAA — without this they are invisible in PT.
    //
    // Note: deliberately NOT bailing out when primaryLevel->lights is null (list
    // went empty) — the diff below still needs to run to catch "last light
    // just disappeared"; the loops below simply do zero iterations in that case.
    if( !cvar::rt_dynlight || !primaryLevel )
    {
        s_prevDynIds.clear();
        return;
    }

    std::unordered_set< uint64_t > curDynIds;

    const float intensityScale = std::max( 0.f, float{ cvar::rt_dynlight_intensity } );
    const float intensityMax   = std::max( 0.f, float{ cvar::rt_dynlight_max } );
    const float srcRadius      = std::max( 0.01f, float{ cvar::rt_dynlight_radius } );
    const bool  stackAtten     = bool{ cvar::rt_dynlight_stack_atten };

    // Doom64 key doors place 3 PointLights on the same XY at different heights so the
    // classic HW path lights a tall jamb strip. In PT those spheres add, so bloom goes
    // nuclear-white. Count co-located XY first, then divide each upload by the stack size.
    auto xyKey = []( double x, double y ) -> uint64_t {
        const int qx = int( std::lround( x / 4.0 ) );
        const int qy = int( std::lround( y / 4.0 ) );
        return ( uint64_t( uint32_t( qx ) ) << 32 ) | uint32_t( qy );
    };

    // Also count when only debugging: the histogram below is how you tell whether
    // stack attenuation is doing anything at all.
    std::unordered_map< uint64_t, int > stackCount;
    if( stackAtten || bool{ cvar::rt_dynlight_debug } )
    {
        for( FDynamicLight* light = primaryLevel->lights; light != nullptr; light = light->next )
        {
            if( !light->IsActive() || light->IsSubtractive() || light->DontLightMap() )
            {
                continue;
            }
            if( light->X() < -1.0e6 )
            {
                continue;
            }
            if( !cvar::rt_dynlight_flicker &&
                ( light->lighttype == FlickerLight || light->lighttype == RandomFlickerLight ) )
            {
                continue;
            }
            if( light->m_currentRadius <= 0.01f )
            {
                continue;
            }
            if( light->m_currentRadius < float{ cvar::rt_dynlight_minradius } )
            {
                continue;
            }
            if( light->GetRed() + light->GetGreen() + light->GetBlue() <= 0 )
            {
                continue;
            }
            stackCount[ xyKey( light->X(), light->Y() ) ]++;
        }
    }

    uint32_t index = 0;
    for( FDynamicLight* light = primaryLevel->lights; light != nullptr; light = light->next )
    {
        if( !light->IsActive() || light->IsSubtractive() || light->DontLightMap() )
        {
            continue;
        }

        // Skip uninitialized lights (GetLight seeds Pos.X = -1e7 until UpdateLocation).
        if( light->X() < -1.0e6 )
        {
            continue;
        }

        // MAP01 wall SMON alcoves use PointLightFlicker (9802). Those are NOT the ceiling
        // head lights — skip unless explicitly re-enabled.
        if( !cvar::rt_dynlight_flicker &&
            ( light->lighttype == FlickerLight || light->lighttype == RandomFlickerLight ) )
        {
            continue;
        }

        // Stable ID across frames (index shifts when other lights activate/deactivate
        // and breaks ReSTIR temporal matching — flicker gets smoothed away).
        const uint64_t stableId =
            DynLightId_Base + ( uint64_t{ reinterpret_cast< uintptr_t >( light ) } & 0xFFFFFFFFull );

        // DLSS-RR: track which lights are present this frame so a newly appeared/
        // disappeared one (barrel/rocket explosion flash, pickup glow, etc.) can
        // flush RR history below.
        //
        // Recorded HERE, before the brightness cutoffs below, deliberately: a pulse
        // light whose m_currentRadius (or scaled intensity) dips under 0.01 for a few
        // tics is still the same light, and must not read as disappear-then-reappear.
        // Doing this after those cutoffs made steady flicker/pulse lights churn the
        // set — membership changing while the *count* stayed flat, so the
        // rt_dynlight_debug count check never caught it — and fired a history flush
        // almost every frame, i.e. permanent RR noise. Presence here means "this
        // FDynamicLight exists and is active", which is what actually maps to the
        // scene-lighting cut we care about.
        curDynIds.insert( stableId );

        // GZDoom stores intensity as light radius in map units; flicker/pulse update
        // m_currentRadius each tic. MAP01 9802 uses 24/20 — only ~17% HW delta, invisible
        // under RR. Remap [lo,hi] → [0.15,1.0] * peak so blink reads as on/off.
        const float mapRadius = light->m_currentRadius;
        if( mapRadius <= 0.01f )
        {
            continue;
        }
        // Below the fixture threshold: a raster-era helper light, not a real source.
        // Filtered after curDynIds.insert above on purpose, so skipping it does not
        // register as a light appearing/disappearing and flush RR temporal history.
        if( mapRadius < float{ cvar::rt_dynlight_minradius } )
        {
            continue;
        }

        const float lo = float( std::min( light->GetIntensity(), light->GetSecondaryIntensity() ) );
        const float hi = float( std::max( light->GetIntensity(), light->GetSecondaryIntensity() ) );
        float       blink = 1.f;
        if( hi > lo + 0.5f &&
            ( light->lighttype == FlickerLight || light->lighttype == RandomFlickerLight ||
              light->lighttype == PulseLight ) )
        {
            const float t = std::clamp( ( mapRadius - lo ) / ( hi - lo ), 0.f, 1.f );
            blink         = 0.15f + 0.85f * t;
        }

        float intensity = hi * intensityScale * blink;
        if( stackAtten )
        {
            const int n = std::max( 1, stackCount[ xyKey( light->X(), light->Y() ) ] );
            intensity /= float( n );
        }
        if( intensityMax > 0.f )
        {
            intensity = std::min( intensity, intensityMax );
        }
        // Large map-radius PointLights (MAP04 yellow hall r=88) otherwise all sit at
        // rt_dynlight_max and read as flat sector fill, drowning hanging-lamp pools.
        // Inv-square roll-off above rsoft keeps jamb-sized lights (r~32) unchanged.
        {
            const float rSoft = float{ cvar::rt_dynlight_rsoft };
            if( rSoft > 1.f && mapRadius > rSoft )
            {
                const float t = rSoft / mapRadius;
                intensity *= t * t;
            }
        }
        if( intensity <= 0.01f )
        {
            continue;
        }

        const int cr = light->GetRed();
        const int cg = light->GetGreen();
        const int cb = light->GetBlue();
        if( cr + cg + cb <= 0 )
        {
            continue;
        }

        const auto color = rt.rgUtilPackColorByte4D( cr, cg, cb, 255 );

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = color,
            .intensity = intensity,
            .position  = { float( light->X() ) * ONEGAMEUNIT_IN_METERS,
                           float( light->Y() ) * ONEGAMEUNIT_IN_METERS,
                           float( light->Z() ) * ONEGAMEUNIT_IN_METERS },
            .radius    = srcRadius,
        };

        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sph,
            .uniqueID     = stableId,
            .isExportable = false,
        };

        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        // Magenta hotspot markers so light sources are visible as blobs in-world
        // (no RTGL "draw all lights" overlay exists; this is the closest engine-side debug).
        if( cvar::rt_dynlight_debug_marks )
        {
            auto markSph = RgLightSphericalEXT{
                .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                .pNext     = nullptr,
                .color     = rt.rgUtilPackColorByte4D( 255, 0, 255, 255 ),
                .intensity = std::max( 0.f, float{ cvar::rt_light_mark_intensity } ),
                .position  = sph.position,
                .radius    = 0.05f,
            };
            auto markInfo = RgLightInfo{
                .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                .pNext        = &markSph,
                .uniqueID     = stableId + 0x50000000ull,
                .isExportable = false,
            };
            r = rt.rgUploadLight( &markInfo );
            RG_CHECK( r );
        }

        ++index;
    }

    if( cvar::rt_dynlight_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            // stack_atten only divides when co-located lights land in the SAME 4-unit
            // xyKey bucket. If maxStack is 1 the attenuation is a no-op by construction
            // — which is exactly what "toggling stack_atten changes nothing" looks like
            // — and Doom 64's triple-PointLight key-door jambs upload at full 3x.
            int stackedBuckets = 0;
            int maxStack       = 0;
            for( const auto& [ bucket, n ] : stackCount )
            {
                stackedBuckets += ( n > 1 );
                maxStack = std::max( maxStack, n );
            }
            Printf( "rt_dynlight_debug: %u active GZDoom light(s) uploaded this frame; "
                    "xy-buckets with >1 light: %d, max stack: %d\n",
                    index,
                    stackedBuckets,
                    maxStack );

            // Identify one specific offending light (the white one on the MAP02 blue-room
            // switch) by walking up to it and reading this. Owner class + color is what
            // lets it be filtered by something meaningful; filtering by map position would
            // be the same one-room hack the sector-tint work already had to undo.
            struct NearLight
            {
                double         dist2;
                FDynamicLight* light;
            };
            std::vector< NearLight > nearest;
            const DVector3           vp = r_viewpoint.Pos;

            for( FDynamicLight* light = primaryLevel->lights; light != nullptr; light = light->next )
            {
                if( !light->IsActive() || light->X() < -1.0e6 )
                {
                    continue;
                }
                const double dx = light->X() - vp.X;
                const double dy = light->Y() - vp.Y;
                const double dz = light->Z() - vp.Z;
                nearest.push_back( { dx * dx + dy * dy + dz * dz, light } );
            }

            std::sort( nearest.begin(),
                       nearest.end(),
                       []( const NearLight& a, const NearLight& b ) { return a.dist2 < b.dist2; } );

            for( size_t n = 0; n < std::min< size_t >( 5, nearest.size() ); n++ )
            {
                FDynamicLight* l     = nearest[ n ].light;
                AActor*        owner = l->target.Get();
                Printf( "  near[%zu] dist=%.0f owner='%s' rgb=(%d,%d,%d) r=%.0f type=%d "
                        "xyz=(%.0f,%.0f,%.0f)\n",
                        n,
                        std::sqrt( nearest[ n ].dist2 ),
                        ( owner && owner->GetClass() ) ? owner->GetClass()->TypeName.GetChars()
                                                       : "?",
                        l->GetRed(),
                        l->GetGreen(),
                        l->GetBlue(),
                        l->m_currentRadius,
                        int( l->lighttype ),
                        float( l->X() ),
                        float( l->Y() ),
                        float( l->Z() ) );
            }
        }
    }

    // DLSS-RR: any ID present in exactly one of the two sets means a light
    // appeared or disappeared this frame -- flush temporal history.
    if( bool{ cvar::rt_rr_reset_on_dynlight } && curDynIds != s_prevDynIds )
    {
        // Detail line, throttled: if this trigger is over-firing it can hit every
        // single frame, and 60 console lines/s would drown everything else. Print
        // at most one line per 15 changes and carry the skipped count on it.
        static uint32_t s_dbgSince = 0;
        if( cvar::rt_rr_reset_debug && ( s_dbgSince++ % 15 ) == 0 )
        {
            uint32_t appeared = 0;
            uint32_t vanished = 0;
            for( uint64_t id : curDynIds )
            {
                appeared += ( s_prevDynIds.count( id ) == 0 );
            }
            for( uint64_t id : s_prevDynIds )
            {
                vanished += ( curDynIds.count( id ) == 0 );
            }
            Printf( "rt_rr_reset: dynlight set changed +%u/-%u (present %u, was %u) "
                    "[change #%u]\n",
                    appeared,
                    vanished,
                    uint32_t( curDynIds.size() ),
                    uint32_t( s_prevDynIds.size() ),
                    s_dbgSince );
        }
        g_rt_lightcut     = true;
        g_rt_lightcut_why = "dynlight";
    }
    s_prevDynIds = std::move( curDynIds );
}

static bool RT_IsCeilingInsetLampTexture( const char* name )
{
    if( !name || !*name )
    {
        return false;
    }
    // Doom 64 inset ceiling lamps: round bright blobs on dark flats (MAP01 spawn
    // booths over the first zombies use SFLATAS).
    //
    // NOT SFLATAP: it is a recessed grille/vent panel with slats, and the original game
    // does not light it. It was in this list from the start and only became visible once
    // the flat walk covered floors and stopped letting one sector eat the budget — a
    // false positive can sit unnoticed for as long as the path around it is broken
    // (2026-08-08).
    if( strncmp( name, "SFLATAS", 7 ) == 0 || strncmp( name, "SFLATAQ", 7 ) == 0 )
    {
        return true;
    }
    if( strncmp( name, "SPORT", 5 ) == 0 )
    {
        return true;
    }
    return false;
}

// The faux pair, kept strictly apart from the real bulb classifiers above.
//
// SFLATC and SPACECE are not lamps. There are no bulbs in the art and the original game
// never lights them; MAP03's stair hall is ceilinged in SFLATC and is simply dark. This
// treats them as bulb arrays anyway, to lift rooms that read as too dark under RT.
//
// The usage split is why there are two predicates rather than one, and it mirrors the
// real pair exactly. Across the game SFLATC appears 76 times and is a FLAT (33 floor,
// 43 ceiling) like SFLATAQ, so it belongs to the flat perimeter walk. SPACECE appears 61
// times and is a WALL texture (60 on sidedefs, 1 stray floor) like SPACEAZ, so it belongs
// to the wall strip walk. Feeding either to the wrong walk would match almost nothing.
static bool RT_IsFauxLampFlat( const char* name )
{
    return cvar::rt_faux_lamps && name && *name && strcmp( name, "SFLATC" ) == 0;
}

static bool RT_IsFauxLampWall( const char* name )
{
    return cvar::rt_faux_lamps && name && *name && strcmp( name, "SPACECE" ) == 0;
}

// Raw, not hue-normalised. RT_SectorHue forces the peak channel to 1 so a tint can never
// darken a light; here darkness is the requested behaviour, so the colour is used as it
// is written and rt_faux_lamp_intensity carries the brightness.
static FVector3 RT_FauxLampHue()
{
    const uint32_t c = uint32_t( cvar::rt_faux_lamp_color );
    return FVector3{ float( ( c >> 16 ) & 0xFF ) / 255.0f,
                     float( ( c >> 8 ) & 0xFF ) / 255.0f,
                     float( c & 0xFF ) / 255.0f };
}

// The solo pair: SFLATDE and SFLATCH. Different from the faux pair in the one way that
// matters — these textures DO show a lit bulb baked into the art (a bright white blob
// dead centre in an X-shaped or ringed housing), the base game simply never wired a light
// to it. So this is not an invention like rt_faux_lamps; it is the same "texture implies a
// fixture" reasoning as the real bulb arrays (SFLATAS/SFLATAQ/SPORT*), just for two names
// that classifier does not cover. Kept off that classifier and given its own cvars/budget
// rather than folded in, because RT_UploadCeilingInsetLamps' shared intensity (700, and
// currently switched off entirely via rt_ceiling_lamps 0 in the launcher) is tuned for a
// different fixture family and reusing it would either relight nothing (feature off) or
// retune those fixtures as a side effect of this one.
//
// Each texture is single-bulb, not a lattice — the geometry is one offset per 64-unit
// tile, not a grid within it — so unlike SFLATC's shared 4-value array, the two textures
// carry their OWN centre, detected the same way (flood-fill centroid of the bright blob):
//   SFLATDE  centre (31.5, 30.5)
//   SFLATCH  centre (32.0, 32.0)
struct SoloBulbTex
{
    const char* name;
    double      ox, oy;
};
static constexpr SoloBulbTex SoloBulbTextures[] = {
    { "SFLATDE", 31.5, 30.5 },
    { "SFLATCH", 32.0, 32.0 },
};

static bool RT_FindSoloBulbOffset( const char* name, double& ox, double& oy )
{
    if( !cvar::rt_solo_lamps || !name || !*name )
    {
        return false;
    }
    for( const SoloBulbTex& t : SoloBulbTextures )
    {
        if( strcmp( name, t.name ) == 0 )
        {
            ox = t.ox;
            oy = t.oy;
            return true;
        }
    }
    return false;
}

// Plain white, used raw like RT_FauxLampHue — but unlike the faux colour, there is no
// darkening intent here, so this exists mainly so the colour is a cvar rather than a
// hardcoded constant, in case a texture is added later whose bulb is not white.
static FVector3 RT_SoloLampHue()
{
    const uint32_t c = uint32_t( cvar::rt_solo_lamp_color );
    return FVector3{ float( ( c >> 16 ) & 0xFF ) / 255.0f,
                     float( ( c >> 8 ) & 0xFF ) / 255.0f,
                     float( c & 0xFF ) / 255.0f };
}

void RT_UploadCeilingInsetLamps()
{
    // Surface _e provides fixture albedo. These analytic lights blink + cast under
    // ceiling flats only (MAP01 spawn "head lights"). Floor lamp panels use texture
    // emissiveMult GI instead — do not upload floor analytic spheres.
    //
    // DLSS-RR skips A-SVGF, so hard on/off + dropping lights from the list
    // nukes ReSTIR temporal reservoirs and shows up as unfiltered-direct sparkle
    // in the final image. Always upload a stable uniqueID and ease intensity.
    if( !cvar::rt_ceiling_lamps || !primaryLevel )
    {
        return;
    }

    const float peak      = std::max( 0.f, float{ cvar::rt_ceiling_lamp_intensity } );
    const float srcRadius = std::max( 0.01f, float{ cvar::rt_ceiling_lamp_radius } );
    const float zOfs      = float{ cvar::rt_ceiling_lamp_zofs };
    const float offScale  = std::clamp( float{ cvar::rt_ceiling_lamp_off }, 0.f, 1.f );
    const float fadeTics  = std::max( 0.f, float{ cvar::rt_ceiling_lamp_fade } );
    if( peak <= 0.01f )
    {
        return;
    }

    // Per-sector eased blink level (survives across frames; resized on map change).
    static TArray<float> s_lampLevel;
    if( s_lampLevel.Size() != primaryLevel->sectors.Size() )
    {
        s_lampLevel.Resize( primaryLevel->sectors.Size() );
        for( unsigned n = 0; n < s_lampLevel.Size(); n++ )
        {
            s_lampLevel[ n ] = 1.f;
        }
    }

    const int maptime = primaryLevel->maptime;
    uint32_t  uploaded = 0;

    for( unsigned i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        sector_t& sector = primaryLevel->sectors[ i ];
        auto*     gtex =
            TexMan.GetGameTexture( sector.GetTexture( sector_t::ceiling ), true );
        if( !gtex )
        {
            continue;
        }
        const char* tname = gtex->GetName().GetChars();
        if( !RT_IsCeilingInsetLampTexture( tname ) )
        {
            continue;
        }

        const float zfloor =
            float( sector.floorplane.ZatPoint( sector.centerspot ) );
        const float zceiling =
            float( sector.ceilingplane.ZatPoint( sector.centerspot ) );
        if( zceiling - zfloor < 8.f )
        {
            continue;
        }

        // Large halls (MAP02 SFLATAQ corridors) only have lamp blobs on the texture
        // edges. A single analytic sphere at centerspot makes a blinking white patch
        // in empty mid-ceiling. Keep analytics for small booths (MAP01 ~96×96).
        const float maxSpan = std::max( 0.f, float{ cvar::rt_ceiling_lamp_maxspan } );
        if( maxSpan > 0.f )
        {
            float minx = 1.e9f, miny = 1.e9f, maxx = -1.e9f, maxy = -1.e9f;
            bool  any  = false;
            for( unsigned li = 0; li < sector.Lines.Size(); li++ )
            {
                const line_t* line = sector.Lines[ li ];
                if( !line )
                {
                    continue;
                }
                for( vertex_t* v : { line->v1, line->v2 } )
                {
                    if( !v )
                    {
                        continue;
                    }
                    any  = true;
                    minx = std::min( minx, float( v->fX() ) );
                    miny = std::min( miny, float( v->fY() ) );
                    maxx = std::max( maxx, float( v->fX() ) );
                    maxy = std::max( maxy, float( v->fY() ) );
                }
            }
            if( any && ( maxx - minx > maxSpan || maxy - miny > maxSpan ) )
            {
                continue;
            }
        }

        // Mostly-on cycle with short dips (same timing as before), but target
        // stays >= offScale so the light never leaves the ReSTIR list.
        const int phase = int( ( maptime * 4 + int( i ) * 23 ) % 256 );
        const bool blackout =
            ( phase < 40 ) || ( phase >= 110 && phase < 122 ) || ( phase >= 200 && phase < 208 );
        const float target = blackout ? offScale : 1.f;

        float& level = s_lampLevel[ i ];
        if( fadeTics <= 0.f )
        {
            level = target;
        }
        else
        {
            const float step = 1.f / fadeTics;
            if( level < target )
            {
                level = std::min( target, level + step );
            }
            else if( level > target )
            {
                level = std::max( target, level - step );
            }
        }

        // Always upload (even when dim) — hard delete was the RR noise source.
        const float intensity = std::max( peak * level, peak * offScale * 0.25f );

        const float z = zceiling - zOfs;
        const float px = float( sector.centerspot.X ) * ONEGAMEUNIT_IN_METERS;
        const float py = float( sector.centerspot.Y ) * ONEGAMEUNIT_IN_METERS;
        const float pz = z * ONEGAMEUNIT_IN_METERS;

        // Warm white base — matches inset lamp blobs better than SMON green 9802s —
        // modulated by the sector's own Doom 64 colormap hue, so colored rooms (MAP02's
        // 0x0050FF blue armor room) light up in their intended color instead of being
        // washed neutral by a hardcoded lamp.
        const FVector3 hue =
            RT_SectorHue( sector.Colormap.LightColor, float{ cvar::rt_sector_tint_lights } );

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( 1.000f * hue.X,
                                                    0.902f * hue.Y,
                                                    0.745f * hue.Z,
                                                    1.0f ),
            .intensity = intensity,
            .position  = { px, py, pz },
            .radius    = srcRadius,
        };

        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sph,
            .uniqueID     = CeilingLampId_Base + i,
            .isExportable = false,
        };

        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        if( cvar::rt_ceiling_lamp_debug )
        {
            auto markSph = RgLightSphericalEXT{
                .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                .pNext     = nullptr,
                .color     = rt.rgUtilPackColorByte4D( 0, 255, 255, 255 ),
                .intensity = 400.f,
                .position  = { px, py, pz },
                .radius    = 0.05f,
            };
            auto markInfo = RgLightInfo{
                .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                .pNext        = &markSph,
                .uniqueID     = CeilingLampId_Base + 0x08000000ull + i,
                .isExportable = false,
            };
            r = rt.rgUploadLight( &markInfo );
            RG_CHECK( r );

            if( ( maptime % 35 ) == 0 && uploaded < 8 )
            {
                Printf( "rt_ceiling_lamp: sec %u '%s' xyz=(%.0f,%.0f,%.0f) I=%.0f\n",
                        i,
                        tname,
                        float( sector.centerspot.X ),
                        float( sector.centerspot.Y ),
                        z,
                        intensity );
            }
        }

        ++uploaded;
    }

    if( cvar::rt_ceiling_lamp_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            Printf( "rt_ceiling_lamp_debug: %u ceiling inset lamp(s) uploaded\n", uploaded );
        }
    }
}

// Two shapes of tech lamp, and the difference that matters is where the bulb sits.
//
//   Hang  64LampTechLongHang (1015/LMP1), 64LampTechShortHang (1016/LMP2).
//         +SPAWNCEILING, so mo->Z() is the BOTTOM of the bbox and the bulb hangs in the
//         lower part of the fixture.
//   Pole  64TechPoleLong (1031/A035, height 80), 64TechPoleShort (1032/A036, height 60).
//         Floor-standing, bulb in the head at the TOP.
//
// Placing a pole lamp's light with the hanging fraction would bury it in the pole's
// shaft, which is solid — fully occluded, and indistinguishable from no light at all
// (§13). One enum, two height fractions (2026-08-08).
enum class RtTechLamp
{
    None,
    Hang,
    Pole,
};

static RtTechLamp RT_TechLampKind( AActor* mo )
{
    if( !mo )
    {
        return RtTechLamp::None;
    }
    if( mo->sprite >= 0 && mo->sprite < sprites.Size() )
    {
        const char* sn = sprites[ mo->sprite ].name;
        if( sn && sn[ 0 ] == 'L' && sn[ 1 ] == 'M' && sn[ 2 ] == 'P' &&
            ( sn[ 3 ] == '1' || sn[ 3 ] == '2' ) )
        {
            return RtTechLamp::Hang;
        }
        // A035 / A036 are the pole lamps' only sprite. Matched in full, not by an 'A'
        // prefix, which would swallow a large slice of the sprite table.
        if( sn && strnicmp( sn, "A035", 4 ) == 0 )
        {
            return RtTechLamp::Pole;
        }
        if( sn && strnicmp( sn, "A036", 4 ) == 0 )
        {
            return RtTechLamp::Pole;
        }
    }
    // Class-name fallback (sprite table glitches / replacements).
    if( mo->GetClass() && mo->GetClass()->TypeName.IsValidName() )
    {
        const char* cn = mo->GetClass()->TypeName.GetChars();
        if( cn )
        {
            if( stricmp( cn, "64LampTechLongHang" ) == 0 ||
                stricmp( cn, "64LampTechShortHang" ) == 0 )
            {
                return RtTechLamp::Hang;
            }
            if( stricmp( cn, "64TechPoleLong" ) == 0 ||
                stricmp( cn, "64TechPoleShort" ) == 0 )
            {
                return RtTechLamp::Pole;
            }
        }
    }
    return RtTechLamp::None;
}

// A light feature is a surface much brighter than the rest of ITS map, not one over a
// fixed number. Take the map's median sector lightlevel and require a margin above it,
// with the absolute floor still applied so a uniformly dim map does not start glowing.
void RT_UpdateSectorEmisThreshold()
{
    static const void* s_cachedLevel   = nullptr;
    static unsigned    s_cachedSectors = 0;
    static float       s_cachedMargin  = -1.f;
    static float       s_cachedFloor   = -1.f;

    if( !primaryLevel || primaryLevel->sectors.Size() == 0 )
    {
        g_sectorEmisThreshold = 255.f;
        return;
    }

    const float margin   = float{ cvar::rt_sector_emis_margin };
    const float absFloor = float{ cvar::rt_sector_emis_minlight };

    // Recompute only on map change or when the tuning cvars move.
    if( s_cachedLevel == primaryLevel && s_cachedSectors == primaryLevel->sectors.Size() &&
        s_cachedMargin == margin && s_cachedFloor == absFloor )
    {
        return;
    }

    std::vector< int > levels;
    levels.reserve( primaryLevel->sectors.Size() );
    for( unsigned i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        levels.push_back( primaryLevel->sectors[ i ].lightlevel );
    }

    const size_t mid = levels.size() / 2;
    std::nth_element( levels.begin(), levels.begin() + mid, levels.end() );
    const float median = float( levels[ mid ] );

    g_sectorEmisThreshold = std::max( absFloor, median + margin );

    s_cachedLevel   = primaryLevel;
    s_cachedSectors = primaryLevel->sectors.Size();
    s_cachedMargin  = margin;
    s_cachedFloor   = absFloor;

    if( cvar::rt_sector_emis_debug )
    {
        Printf( "rt_sector_emis: map median lightlevel=%.0f margin=%.0f floor=%.0f "
                "-> only sectors above %.0f self-emit\n",
                median,
                margin,
                absFloor,
                g_sectorEmisThreshold );
    }
}

static bool RT_IsWallStripLampTexture( const char* name )
{
    if( !name || !*name )
    {
        return false;
    }
    // The bulb arrays themselves: a regular grid of round lamps.
    //
    //   SPACEAZ  4x4 bulbs, authored as a wall texture
    //   SFLATAQ  4x4 bulbs, authored as a flat but ALSO hung on wall faces
    //   SFLATAS  2x2 large bulbs
    //
    // SFLATAP is deliberately absent despite the matching name: it is a recessed grille,
    // not a lamp, and the original game does not light it.
    //
    // This used to match SPACEAR instead, which is a mistake worth recording. SPACEAR is
    // the plain trim panel that sits on the same thin step the bulb flat caps, so on
    // MAP03 it is adjacent to an SFLATAQ bulb flat on 54 of its 57 sidedefs (95%) — the
    // light landed a few units from the real fixture and looked correct. On MAP02 that
    // adjacency is 4 of 41 (10%): the same rule lights blank wall and misses every actual
    // lamp. A rule that is right by proximity on the map you tested is not a rule.
    //
    // Flats named SFLAT* reach this walk because Doom 64 hangs them on sidedefs too —
    // MAP02 carries SFLATAQ as `bottom` 26 times and `middle` 4 times. The flat-side
    // coverage is separate: see RT_UploadCeilingInsetLamps / RT_UploadCeilingEdgeLamps
    // (2026-08-08).
    return strcmp( name, "SPACEAZ" ) == 0 || strcmp( name, "SFLATAQ" ) == 0 ||
           strcmp( name, "SFLATAS" ) == 0;
}

// Doom 64 wall light strips carry their light in the texture only. Under RTGL1 an
// emissive surface is not a light source (see rt_wall_strips), so the strip glows but
// lights nothing — the corridor reads flat and shadowless. Place real area lights along
// the fixture instead.
//
// Polygonal rather than spherical on purpose: a strip is a long thin emitter, and a
// chain of point lights gives scalloped hotspots along the wall instead of an even wash.
void RT_UploadWallStripLights()
{
    if( !cvar::rt_wall_strips || !primaryLevel )
    {
        return;
    }

    const float peak     = std::max( 0.f, float{ cvar::rt_wall_strip_intensity } );
    const float minLight = float{ cvar::rt_wall_strip_minlight };
    const float segLen   = std::max( 16.f, float{ cvar::rt_wall_strip_seglen } );
    const int   maxLights = std::max( 0, int{ cvar::rt_wall_strip_max } );
    // Faux panels have their own intensity and cap, so zeroing the real strips must not
    // switch them off with it -- turning the real fixtures down to judge the fake ones is
    // exactly the comparison someone will want to run.
    const bool  fauxOn   = bool{ cvar::rt_faux_lamps } &&
                          float{ cvar::rt_faux_lamp_intensity } > 0.01f &&
                          int{ cvar::rt_faux_lamp_max } > 0;
    if( ( peak <= 0.01f || maxLights <= 0 ) && !fauxOn )
    {
        return;
    }

    // Rejection tally, not just a success count: "0 uploaded" is ambiguous on its own,
    // and the stack-attenuation hunt already showed how expensive that ambiguity is.
    int uploaded    = 0;
    int matchedTex  = 0;
    int rejLight    = 0;
    int rejBand     = 0;
    int rejShort    = 0;
    int marked      = 0;

    // Faux panels are budgeted apart from the real strips, for the same reason the flat
    // walk splits its cap: an invented fixture must never push a real one out.
    const int fauxMax      = std::max( 0, int{ cvar::rt_faux_lamp_max } );
    int       fauxWalls    = 0;
    int       fauxUploaded = 0;
    // The walk stops only when BOTH budgets are spent -- gating the loops on the real
    // count alone would let a run of real strips end the walk before any faux panel was
    // even looked at.
    auto budgetLeft = [ & ] { return uploaded < maxLights || fauxUploaded < fauxMax; };

    // Placement of the lights actually near the camera, not one arbitrary sample:
    // "the fixture matched" and "the light is where the bulbs are" are different claims,
    // and only the second explains a strip that is found but still looks unlit.
    struct Placed
    {
        double dist;
        double x, y, z;
        double bandLow, bandHigh;
        int    part;
        FString tex; // two families match now, so "which one" is part of the answer
    };
    std::vector< Placed > placed;
    const DVector3        vpos = r_viewpoint.Pos;

    for( unsigned i = 0; i < primaryLevel->lines.Size() && uploaded < maxLights; i++ )
    {
        const line_t& line = primaryLevel->lines[ i ];
        if( !line.v1 || !line.v2 )
        {
            continue;
        }

        for( int s = 0; s < 2 && budgetLeft(); s++ )
        {
            const side_t* side = line.sidedef[ s ];
            if( !side || !side->sector )
            {
                continue;
            }

            const sector_t* thisSec  = side->sector;
            const side_t*   otherSide = line.sidedef[ 1 - s ];
            const sector_t* otherSec = otherSide ? otherSide->sector : nullptr;

            for( int part = 0; part < 3 && budgetLeft(); part++ )
            {
                auto* gtex = TexMan.GetGameTexture( side->GetTexture( part ), true );
                if( !gtex )
                {
                    continue;
                }
                const char* wtname = gtex->GetName().GetChars();
                const bool  isFaux = RT_IsFauxLampWall( wtname );
                if( !isFaux && !RT_IsWallStripLampTexture( wtname ) )
                {
                    continue;
                }
                matchedTex++;
                if( isFaux )
                {
                    fauxWalls++;
                }

                // Checked after the texture match so the tally can tell "no strips in
                // this map" apart from "strips found but every one was rejected".
                //
                // Faux panels are exempt, and this is the whole point of them. The
                // minlight gate exists so a real strip in an already-bright room does not
                // double-light it; but a faux panel's only job is to lift a room that is
                // too dark, so applying the gate would reject precisely the sectors the
                // feature was asked for and leave it looking like it does nothing.
                if( !isFaux && float( thisSec->lightlevel ) < minLight )
                {
                    rejLight++;
                    continue;
                }

                const double x1 = line.v1->fX();
                const double y1 = line.v1->fY();
                const double x2 = line.v2->fX();
                const double y2 = line.v2->fY();

                const double lineLen = std::hypot( x2 - x1, y2 - y1 );
                if( lineLen < 1.0 )
                {
                    rejShort++;
                    continue;
                }

                const int segs = std::clamp(
                    int( std::ceil( lineLen / segLen ) ), 1, int( WallStripSegsPerLine ) );

                for( int sg = 0; sg < segs && budgetLeft(); sg++ )
                {
                    const double t0 = double( sg ) / segs;
                    const double t1 = double( sg + 1 ) / segs;

                    const double ax = x1 + ( x2 - x1 ) * t0;
                    const double ay = y1 + ( y2 - y1 ) * t0;
                    const double bx = x1 + ( x2 - x1 ) * t1;
                    const double by = y1 + ( y2 - y1 ) * t1;
                    const double mx = ( ax + bx ) * 0.5;
                    const double my = ( ay + by ) * 0.5;

                    const DVector2 mid{ mx, my };

                    // Which vertical band this sidedef part actually covers.
                    double zLow  = 0.0;
                    double zHigh = 0.0;
                    if( part == side_t::top && otherSec )
                    {
                        zLow  = otherSec->ceilingplane.ZatPoint( mid );
                        zHigh = thisSec->ceilingplane.ZatPoint( mid );
                    }
                    else if( part == side_t::bottom && otherSec )
                    {
                        zLow  = thisSec->floorplane.ZatPoint( mid );
                        zHigh = otherSec->floorplane.ZatPoint( mid );
                    }
                    else if( part == side_t::mid && otherSec )
                    {
                        // A middle texture on a two-sided line only covers the OPENING,
                        // not this sector's full height. Handing it floor..ceiling put
                        // MAP02's blue-room strips at mid-room height, floating in front
                        // of the fixture instead of on it (2026-08-08).
                        zLow  = std::max( thisSec->floorplane.ZatPoint( mid ),
                                         otherSec->floorplane.ZatPoint( mid ) );
                        zHigh = std::min( thisSec->ceilingplane.ZatPoint( mid ),
                                          otherSec->ceilingplane.ZatPoint( mid ) );
                    }
                    else
                    {
                        zLow  = thisSec->floorplane.ZatPoint( mid );
                        zHigh = thisSec->ceilingplane.ZatPoint( mid );
                    }

                    if( zHigh < zLow )
                    {
                        std::swap( zLow, zHigh );
                    }
                    if( zHigh - zLow < 1.0 )
                    {
                        rejBand++;
                        continue;
                    }

                    // The fixture is the bulb row, not the whole band: pull the emitter to
                    // the middle of the band and keep it thin, so a tall step does not turn
                    // into a wall-height slab of light.
                    const double zMid  = ( zLow + zHigh ) * 0.5;
                    const double zHalf = std::min( 6.0, ( zHigh - zLow ) * 0.5 );

                    // Nudge off the wall so the emitter is not coplanar with the geometry
                    // it is meant to light (self-shadowing / acne at grazing angles).
                    //
                    // Which way is "off the wall" is decided by testing against the sector's
                    // own centre rather than by Doom's front/back winding convention. Getting
                    // that convention backwards buries every light 2 units inside solid
                    // geometry, where it is fully occluded and emits nothing visible — which
                    // is exactly what happened here, and it looks identical to the lights
                    // never being uploaded at all.
                    const double segDx = bx - ax;
                    const double segDy = by - ay;
                    const double segLenXY = std::hypot( segDx, segDy ) + 1e-6;
                    const double nx = -segDy / segLenXY;
                    const double ny = segDx / segLenXY;

                    const double towardX = double( thisSec->centerspot.X ) - mx;
                    const double towardY = double( thisSec->centerspot.Y ) - my;
                    const double ofs =
                        ( nx * towardX + ny * towardY ) >= 0.0 ? 2.0 : -2.0;

                    // Per-class budget, checked here rather than in the loop guard: the
                    // guard only knows whether SOME budget remains, not whether this
                    // particular light's budget does.
                    if( isFaux ? ( fauxUploaded >= fauxMax ) : ( uploaded >= maxLights ) )
                    {
                        continue;
                    }

                    const FVector3 hue =
                        isFaux ? RT_FauxLampHue()
                               : RT_SectorHue( thisSec->Colormap.LightColor,
                                               float{ cvar::rt_sector_tint_lights } );

                    auto toM = [ & ]( double x, double y, double z ) -> RgFloat3D {
                        return { float( x + nx * ofs ) * ONEGAMEUNIT_IN_METERS,
                                 float( y + ny * ofs ) * ONEGAMEUNIT_IN_METERS,
                                 float( z ) * ONEGAMEUNIT_IN_METERS };
                    };

                    // Spherical, not polygonal: RTGL1 declares RgLightPolygonalEXT in the
                    // public header but LightManager.cpp compiles it out behind
                    // #if TRIANGLE_LIGHTS and hard-errors on upload. Emulate the strip with
                    // overlapping spheres instead — a generous source radius plus a segment
                    // length below it keeps the pools blended rather than scalloped.
                    ( void )zHalf;

                    auto sph = RgLightSphericalEXT{
                        .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                        .pNext     = nullptr,
                        .color     = rt.rgUtilPackColorFloat4D( hue.X, hue.Y, hue.Z, 1.0f ),
                        .intensity = isFaux
                                         ? std::max( 0.f,
                                                     float{ cvar::rt_faux_lamp_intensity } )
                                         : peak,
                        .position  = toM( mx, my, zMid ),
                        .radius = std::max( 0.01f, float{ cvar::rt_wall_strip_radius } ),
                    };

                    auto info = RgLightInfo{
                        .sType    = RG_STRUCTURE_TYPE_LIGHT_INFO,
                        .pNext    = &sph,
                        .uniqueID = WallStripId_Base +
                                    ( uint64_t( i ) * WallStripSegsPerLine * 8 ) +
                                    ( uint64_t( s ) * WallStripSegsPerLine * 4 ) +
                                    ( uint64_t( part ) * WallStripSegsPerLine ) + uint64_t( sg ),
                        .isExportable = false,
                    };

                    RgResult r = rt.rgUploadLight( &info );
                    RG_CHECK( r );

                    // Same aggregate limit as the flat lamps: N markers are N real lights.
                    if( cvar::rt_wall_strip_debug_marks &&
                        marked < std::max( 0, int{ cvar::rt_light_mark_max } ) )
                    {
                        marked++;
                        auto markSph = RgLightSphericalEXT{
                            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                            .pNext     = nullptr,
                            .color     = rt.rgUtilPackColorByte4D( 255, 0, 255, 255 ),
                            .intensity = std::max( 0.f, float{ cvar::rt_light_mark_intensity } ),
                            .position  = toM( mx, my, zMid ),
                            .radius    = 0.05f,
                        };
                        auto markInfo = RgLightInfo{
                            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                            .pNext        = &markSph,
                            .uniqueID     = info.uniqueID + 0x04000000ull,
                            .isExportable = false,
                        };
                        RgResult mr = rt.rgUploadLight( &markInfo );
                        RG_CHECK( mr );
                    }

                    if( cvar::rt_wall_strip_debug )
                    {
                        const double lx = mx + nx * ofs;
                        const double ly = my + ny * ofs;
                        placed.push_back( { std::hypot( lx - vpos.X, ly - vpos.Y ),
                                            lx,
                                            ly,
                                            zMid,
                                            zLow,
                                            zHigh,
                                            part } );
                    }
                    if( isFaux )
                    {
                        fauxUploaded++;
                    }
                    else
                    {
                        uploaded++;
                    }
                }
            }
        }
    }

    if( cvar::rt_wall_strip_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            Printf( "rt_wall_strip: uploaded=%d (cap %d) | matchedTex=%d rejected: "
                    "lightlevel=%d band=%d shortline=%d | I=%.0f radius=%.2f | "
                    "faux %d sidedef(s), uploaded=%d (cap %d) I=%.0f\n",
                    uploaded,
                    maxLights,
                    matchedTex,
                    rejLight,
                    rejBand,
                    rejShort,
                    peak,
                    float{ cvar::rt_wall_strip_radius },
                    fauxWalls,
                    fauxUploaded,
                    fauxMax,
                    float{ cvar::rt_faux_lamp_intensity } );
            Printf( "  viewer z=%.0f — strip lights nearest the camera:\n", vpos.Z );

            std::sort( placed.begin(), placed.end(), []( const Placed& a, const Placed& b ) {
                return a.dist < b.dist;
            } );

            static const char* partName[ 3 ] = { "top", "mid", "bot" };
            for( size_t n = 0; n < std::min< size_t >( 6, placed.size() ); n++ )
            {
                const Placed& p = placed[ n ];
                Printf( "    d=%.0f %s xyz=(%.0f,%.0f,%.0f) band=%.0f..%.0f (%.0f tall)\n",
                        p.dist,
                        partName[ p.part ],
                        p.x,
                        p.y,
                        p.z,
                        p.bandLow,
                        p.bandHigh,
                        p.bandHigh - p.bandLow );
            }
        }
    }
}

// Doom 64 lamp ceilings put their bulbs as blobs around the EDGE of the flat, not in the
// middle. RT_UploadCeilingInsetLamps answers that by putting one sphere at the sector
// centre, which only reads correctly in a small booth — so it skips anything wider than
// rt_ceiling_lamp_maxspan, and every large hall's bulbs end up casting nothing at all.
//
// Trace the sector perimeter instead. Works for both shapes: a ring of lights around a
// small square ceiling panel, and a run of lights along a long corridor's edge.
void RT_UploadCeilingEdgeLamps()
{
    if( !cvar::rt_ceiling_edge_lamps || !primaryLevel )
    {
        return;
    }

    const float peak      = std::max( 0.f, float{ cvar::rt_ceiling_edge_intensity } );
    const float segLen    = std::max( 16.f, float{ cvar::rt_ceiling_edge_seglen } );
    const float srcRadius = std::max( 0.01f, float{ cvar::rt_ceiling_edge_radius } );
    const float zOfs      = float{ cvar::rt_ceiling_edge_zofs };
    const float inset     = float{ cvar::rt_ceiling_edge_inset };
    const int   maxLights = std::max( 0, int{ cvar::rt_ceiling_edge_max } );
    // See RT_UploadWallStripLights: the faux and solo budgets are independent, so the
    // real lamps being off must not take either of them with it.
    const bool  fauxOn    = bool{ cvar::rt_faux_lamps } &&
                        float{ cvar::rt_faux_lamp_intensity } > 0.01f &&
                        int{ cvar::rt_faux_lamp_max } > 0;
    const bool  soloOn    = bool{ cvar::rt_solo_lamps } &&
                        float{ cvar::rt_solo_lamp_intensity } > 0.01f &&
                        int{ cvar::rt_solo_lamp_max } > 0;
    if( ( peak <= 0.01f || maxLights <= 0 ) && !fauxOn && !soloOn )
    {
        return;
    }

    int uploaded   = 0;
    int lampCeils  = 0;
    int lampFloors = 0;
    int fauxFlats  = 0;
    int soloFlats  = 0;
    // Counted separately from lampCeils/lampFloors, which now mean "flats that took the
    // perimeter walk" — without the split, a level whose bulb flats all moved onto the
    // lattice would report an unchanged lamp count while placing lights somewhere else
    // entirely, which is precisely the kind of silent move this debug line exists to catch.
    int bulbLattices = 0;

    // Collect first, then keep the nearest maxLights — do NOT stop the walk at the cap.
    //
    // Emitting in sector-index order and breaking at the cap lets one sector take the
    // whole budget: MAP02's sector 16 has an 11,614-unit perimeter and alone wants 364
    // lights against a cap of 320, so every other bulb sector in the level got nothing
    // and the debug line read "1 lamp ceiling + 1 lamp floor". Demand is ~800 segments on
    // both MAP02 and MAP03, so the cap always binds and *which* lights it drops is the
    // entire behaviour. Nearest-first also puts the budget where it is visible
    // (2026-08-08).
    struct Cand
    {
        double   dist2;
        double   x, y, z;
        uint64_t id;
        FVector3 hue;
        float    intensity;
        float    radius;
    };
    std::vector< Cand > cand;
    // Faux panels and solo bulbs each collect into their own list and get their own cap,
    // then all three are merged. Appending them to `cand` would let invented/solo fixtures
    // compete with real bulbs for a budget that already binds hard (~800 demand vs 320),
    // so they would darken the real ones — a regression no debug counter would obviously
    // show.
    std::vector< Cand > fauxCand;
    std::vector< Cand > soloCand;

    const DVector3 vpos    = r_viewpoint.Pos;
    const double   maxDist = std::max( 64.0, double( float{ cvar::rt_ceiling_edge_maxdist } ) );
    const double   maxDist2 = maxDist * maxDist;

    // SFLATC's bulb lattice, in texture pixels within the 64x64 tile, detected from the
    // art by tools/make_bulb_textures.py rather than assumed: 4x4 sockets at 7.5, 23.5,
    // 39.5, 55.5 on both axes. Flats are mapped 1:1 to world units from the world origin,
    // so these are also world offsets modulo 64.
    static constexpr double FauxFlatLattice[] = { 7.5, 23.5, 39.5, 55.5 };
    constexpr double        TileSize          = 64.0;

    // The REAL bulb arrays' lattices, detected the same way as SFLATC's and just as much
    // NOT assumed: these are the blob centroids of the authored `_e` masks
    // (tools/gen_bulb_flat_masks.py). SFLATAS is 2x2 at 32-unit spacing, SFLATAQ 4x4 at 16.
    static constexpr double BulbLatticeAS[] = { 15.5, 47.5 };
    static constexpr double BulbLatticeAQ[] = { 7.5, 23.5, 39.5, 55.5 };
    const bool              latticeOn       = bool{ cvar::rt_ceiling_edge_lattice };

    // Stride lives in this table rather than in a cvar, and is per texture rather than
    // shared, because the two lattices are different densities and one number cannot mean
    // the same thing on both. Stride 1 on SFLATAS (bulbs already 32 units apart) and 2 on
    // SFLATAQ (16 -> 32) lands a light every ~32 units on either texture. That matters for
    // the same reason rt_ceiling_edge_intensity is pinned equal to rt_wall_strip_intensity:
    // one physical bulb band crosses between these textures, and a density step reads as a
    // brightness step at the seam.
    auto bulbLatticeFor = []( const char* n, const double*& off, int& nOff, int& stride ) {
        if( strncmp( n, "SFLATAS", 7 ) == 0 )
        {
            off = BulbLatticeAS, nOff = 2, stride = 1;
            return true;
        }
        if( strncmp( n, "SFLATAQ", 7 ) == 0 )
        {
            off = BulbLatticeAQ, nOff = 4, stride = 2;
            return true;
        }
        return false;
    };

    // One light per bulb is unaffordable: at 16-unit spacing a 512x512 room wants over a
    // thousand. The stride subsamples the lattice, so lights stay ON bulbs (which is the
    // whole point) but not on every one. Faux and solo get independent strides because
    // they are different densities of invention: SFLATC is a dense invented grid, the
    // solo textures are a handful of genuine fixtures.
    const int fauxStrideN = std::max( 1, int{ cvar::rt_faux_lamp_stride } );
    const int soloStrideN = std::max( 1, int{ cvar::rt_solo_lamp_stride } );
    const float fauxIntensity = std::max( 0.f, float{ cvar::rt_faux_lamp_intensity } );
    const float soloIntensity = std::max( 0.f, float{ cvar::rt_solo_lamp_intensity } );
    const float soloRadius    = std::max( 0.01f, float{ cvar::rt_solo_lamp_radius } );
    const float soloZofs      = float{ cvar::rt_solo_lamp_zofs };

    // Shared by both the faux (4x4 grid) and solo (single bulb) placements: walk whole
    // 64-unit tiles across a sector's bounding box, and within each tile drop a light at
    // every (offX[ox], offY[oy]) pair — a 4x4 cross product for SFLATC's grid, or a single
    // point for a solo texture's one bulb. offX/offY are separate arrays (not one shared
    // array reused for both axes) because a solo bulb's centre need not be exactly square
    // — SFLATDE's detected centre is (31.5, 30.5), not (31.5, 31.5).
    auto addLattice = [ & ]( const sector_t& sector, unsigned secIndex, bool isCeiling,
                             const double* offX, const double* offY, int nOff, int stride,
                             FVector3 hue, float intensity, float radius, float zofs,
                             uint64_t idBase, std::vector< Cand >& out ) {
        double minx = 1.e9, miny = 1.e9, maxx = -1.e9, maxy = -1.e9;
        for( unsigned li = 0; li < sector.Lines.Size(); li++ )
        {
            const line_t* line = sector.Lines[ li ];
            if( !line )
            {
                continue;
            }
            for( const vertex_t* v : { line->v1, line->v2 } )
            {
                if( !v )
                {
                    continue;
                }
                minx = std::min( minx, v->fX() );
                maxx = std::max( maxx, v->fX() );
                miny = std::min( miny, v->fY() );
                maxy = std::max( maxy, v->fY() );
            }
        }
        if( maxx < minx || maxy < miny )
        {
            return;
        }

        // Walk whole tiles across the sector's bounding box, then the lattice within each.
        const long tile0x = long( std::floor( minx / TileSize ) );
        const long tile1x = long( std::floor( maxx / TileSize ) );
        const long tile0y = long( std::floor( miny / TileSize ) );
        const long tile1y = long( std::floor( maxy / TileSize ) );

        for( long ty = tile0y; ty <= tile1y; ty++ )
        {
            for( long tx = tile0x; tx <= tile1x; tx++ )
            {
                for( int oy = 0; oy < nOff; oy++ )
                {
                    for( int ox = 0; ox < nOff; ox++ )
                    {
                        // Stride against the ABSOLUTE lattice index, not a per-sector
                        // counter, so the chosen bulbs line up across tile and sector
                        // boundaries instead of jumping at every seam.
                        const long gx = tx * nOff + ox;
                        const long gy = ty * nOff + oy;
                        if( ( ( gx % stride ) + stride ) % stride != 0 ||
                            ( ( gy % stride ) + stride ) % stride != 0 )
                        {
                            continue;
                        }

                        const double px = double( tx ) * TileSize + offX[ ox ];
                        const double py = double( ty ) * TileSize + offY[ oy ];
                        if( px < minx || px > maxx || py < miny || py > maxy )
                        {
                            continue;
                        }

                        const double dx = px - vpos.X;
                        const double dy = py - vpos.Y;
                        const double d2 = dx * dx + dy * dy;
                        if( d2 > maxDist2 )
                        {
                            continue;
                        }

                        // The bounding box is not the sector: an L-shaped room would
                        // otherwise get lights hanging in the neighbouring one.
                        if( primaryLevel->PointInSector( DVector2( px, py ) ) != &sector )
                        {
                            continue;
                        }

                        const DVector2 at{ px, py };
                        const double   pz = isCeiling
                                                ? sector.ceilingplane.ZatPoint( at ) - zofs
                                                : sector.floorplane.ZatPoint( at ) + zofs;

                        // Stable ID from position, not from an emit counter: the nearest-N
                        // set changes as the camera moves, and a counter-derived ID would
                        // renumber every light and flush RR temporal history each frame.
                        const uint64_t id =
                            idBase +
                            ( uint64_t( secIndex ) << 20 ) +
                            ( uint64_t( ( gy & 0x3FF ) ) << 10 ) +
                            uint64_t( gx & 0x3FF ) +
                            ( isCeiling ? 0ull : 0x80000ull );

                        out.push_back( Cand{ d2, px, py, pz, id, hue, intensity, radius } );
                    }
                }
            }
        }
    };

    auto addFauxLattice = [ & ]( const sector_t& sector, unsigned secIndex, bool isCeiling ) {
        addLattice( sector, secIndex, isCeiling, FauxFlatLattice, FauxFlatLattice,
                    int( std::size( FauxFlatLattice ) ), fauxStrideN, RT_FauxLampHue(),
                    fauxIntensity, srcRadius, zOfs, FauxLatticeId_Base, fauxCand );
    };

    auto addSoloLattice = [ & ]( const sector_t& sector, unsigned secIndex, bool isCeiling,
                                 double ox, double oy ) {
        const double offX[ 1 ] = { ox };
        const double offY[ 1 ] = { oy };
        addLattice( sector, secIndex, isCeiling, offX, offY, 1, soloStrideN, RT_SoloLampHue(),
                    soloIntensity, soloRadius, soloZofs, SoloLatticeId_Base, soloCand );
    };


    // Both planes, not just the ceiling. Doom 64 runs one continuous bulb band along a
    // wall and then across whichever flat it meets — the band does not care which way it
    // is facing, and neither should this. Reading only sector_t::ceiling left 19 bulb
    // floors unlit on MAP02 and 46 on MAP03: the band visibly stopped at the corner
    // where it turned onto the floor (2026-08-08).
    for( unsigned i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        const sector_t& sector = primaryLevel->sectors[ i ];

        for( int plane = 0; plane < 2; plane++ )
        {
        const bool isCeiling = ( plane == 0 );
        auto*      gtex      = TexMan.GetGameTexture(
            sector.GetTexture( isCeiling ? sector_t::ceiling : sector_t::floor ), true );
        if( !gtex )
        {
            continue;
        }
        const char* ftname = gtex->GetName().GetChars();
        const bool  isFaux = RT_IsFauxLampFlat( ftname );
        double      soloOx = 0.0, soloOy = 0.0;
        const bool  isSolo = !isFaux && RT_FindSoloBulbOffset( ftname, soloOx, soloOy );
        if( !isFaux && !isSolo && !RT_IsCeilingInsetLampTexture( ftname ) )
        {
            continue;
        }
        if( isFaux )
        {
            fauxFlats++;

            // Faux flats do NOT use the perimeter walk below, and that is the whole
            // point of this branch. The perimeter walk drops a light every
            // rt_ceiling_edge_seglen units around the sector edge, which has no relation
            // to where the art puts its bulbs — on SFLATC the sockets are a 4x4 lattice
            // at 16-unit spacing, so perimeter lights land between bulbs, in the middle
            // of blank plate, and read as light coming from nowhere. Placing them on the
            // lattice instead means every faux light sits inside a painted socket.
            //
            // Doom flats are mapped 1:1 to world units and anchored at the world origin,
            // so a socket at texture u appears at every world x with x mod 64 == u. The
            // lattice below is expressed as offsets within that 64-unit tile.
            addFauxLattice( sector, i, isCeiling );
            continue;
        }
        if( isSolo )
        {
            soloFlats++;
            // Same lattice mechanism as faux, same reason (the perimeter walk has no
            // relation to where the art puts its one bulb per tile), just one point per
            // tile instead of sixteen.
            addSoloLattice( sector, i, isCeiling, soloOx, soloOy );
            continue;
        }
        ( isCeiling ? lampCeils : lampFloors )++;

        // Lattice placement for the real bulb arrays, for exactly the reason the isFaux
        // branch above gives — it was simply never applied to them. SFLATAS/SFLATAQ tile
        // their bulbs across the WHOLE flat, so a perimeter walk lights the room's edges
        // and leaves every interior bulb casting nothing: a wide panel stayed dark down
        // its own middle while its art showed lit bulbs there (open-issues 1.6g). Feeds
        // the SAME `cand` list, budget and intensity as the perimeter path, because this
        // changes only WHERE the lights go, not how many or how bright.
        //
        // SPORT* deliberately has no entry and falls through: a teleporter pad is one
        // fixture filling its sector, not a tiled lattice, so the perimeter walk is right
        // for it.
        const double* bulbOff    = nullptr;
        int           bulbN      = 0;
        int           bulbStride = 1;
        if( latticeOn && bulbLatticeFor( ftname, bulbOff, bulbN, bulbStride ) )
        {
            bulbLattices++;
            addLattice( sector,
                        i,
                        isCeiling,
                        bulbOff,
                        bulbOff,
                        bulbN,
                        bulbStride,
                        RT_SectorHue( sector.Colormap.LightColor,
                                      float{ cvar::rt_sector_tint_lights } ),
                        peak,
                        srcRadius,
                        zOfs,
                        CeilingLatticeId_Base,
                        cand );
            continue;
        }

        for( unsigned li = 0; li < sector.Lines.Size(); li++ )
        {
            const line_t* line = sector.Lines[ li ];
            if( !line || !line->v1 || !line->v2 )
            {
                continue;
            }

            const double x1 = line->v1->fX();
            const double y1 = line->v1->fY();
            const double x2 = line->v2->fX();
            const double y2 = line->v2->fY();

            const double len = std::hypot( x2 - x1, y2 - y1 );
            if( len < 1.0 )
            {
                continue;
            }

            // Clamped so one very long line cannot dominate, and so the id packing below
            // stays collision-free.
            const int segs = std::clamp(
                int( std::ceil( len / segLen ) ), 1, int( CeilingEdgeSegsPerLine ) );

            for( int sg = 0; sg < segs; sg++ )
            {
                const double t  = ( double( sg ) + 0.5 ) / segs;
                const double px = x1 + ( x2 - x1 ) * t;
                const double py = y1 + ( y2 - y1 ) * t;

                // Pull inward toward the sector centre so the lamp is not embedded in the
                // wall. Same lesson as the wall strips: a winding-convention normal put
                // every light inside solid geometry, where it lit nothing.
                double towardX = double( sector.centerspot.X ) - px;
                double towardY = double( sector.centerspot.Y ) - py;
                const double tlen = std::hypot( towardX, towardY );
                if( tlen > 0.001 )
                {
                    towardX /= tlen;
                    towardY /= tlen;
                }

                const double lx = px + towardX * inset;
                const double ly = py + towardY * inset;
                // zOfs pulls the lamp away from its own plane, so it flips sign with the
                // plane: down from a ceiling, up from a floor. Sharing one sign would
                // bury every floor lamp below the floor, fully occluded — the same
                // failure as the wall strips' inverted normal (§13).
                const DVector2 lpos{ lx, ly };
                const double   lz = isCeiling
                                        ? sector.ceilingplane.ZatPoint( lpos ) - zOfs
                                        : sector.floorplane.ZatPoint( lpos ) + zOfs;

                const double dx = lx - vpos.X;
                const double dy = ly - vpos.Y;
                const double dz = lz - vpos.Z;
                const double d2 = dx * dx + dy * dy + dz * dz;
                if( d2 > maxDist2 )
                {
                    continue;
                }

                // Derived from map indices, not from a running counter. An idSeed++ makes
                // every light's ID depend on how many lights happened to be emitted before
                // it, so the moment the camera moves and the nearest-N set changes, every
                // ID shifts and RTGL1 sees the entire set vanish and reappear — which
                // flushes RR temporal history every frame. Map indices are stable.
                const uint64_t id =
                    CeilingEdgeId_Base +
                    ( ( uint64_t( line->Index() ) * 2 + uint64_t( plane ) ) *
                      CeilingEdgeSegsPerLine ) +
                    uint64_t( sg );

                // isFaux and isSolo are always false down here: both branch to their own
                // lattice function and `continue` before reaching this perimeter walk, so
                // this path only ever runs for the real RT_IsCeilingInsetLampTexture case.
                cand.push_back( Cand{
                    d2,
                    lx,
                    ly,
                    lz,
                    id,
                    RT_SectorHue( sector.Colormap.LightColor, float{ cvar::rt_sector_tint_lights } ),
                    peak,
                    srcRadius } );
            }
        }
        }
    }

    const int wanted     = int( cand.size() );
    const int fauxWanted = int( fauxCand.size() );
    const int soloWanted = int( soloCand.size() );
    if( cand.size() > size_t( maxLights ) )
    {
        std::nth_element( cand.begin(),
                          cand.begin() + maxLights,
                          cand.end(),
                          []( const Cand& a, const Cand& b ) { return a.dist2 < b.dist2; } );
        cand.resize( size_t( maxLights ) );
    }

    // Same nearest-N trim, applied to the faux and solo lists against their OWN caps,
    // before all three are merged. Trimming after the merge would defeat the point of the
    // split budgets.
    const int fauxMax = std::max( 0, int{ cvar::rt_faux_lamp_max } );
    if( fauxCand.size() > size_t( fauxMax ) )
    {
        std::nth_element( fauxCand.begin(),
                          fauxCand.begin() + fauxMax,
                          fauxCand.end(),
                          []( const Cand& a, const Cand& b ) { return a.dist2 < b.dist2; } );
        fauxCand.resize( size_t( fauxMax ) );
    }
    const int soloMax = std::max( 0, int{ cvar::rt_solo_lamp_max } );
    if( soloCand.size() > size_t( soloMax ) )
    {
        std::nth_element( soloCand.begin(),
                          soloCand.begin() + soloMax,
                          soloCand.end(),
                          []( const Cand& a, const Cand& b ) { return a.dist2 < b.dist2; } );
        soloCand.resize( size_t( soloMax ) );
    }
    cand.insert( cand.end(), fauxCand.begin(), fauxCand.end() );
    cand.insert( cand.end(), soloCand.begin(), soloCand.end() );
    // Nearest-first ordering, so the marker budget below lands on the lights actually in
    // front of the camera. Cheap at this size, and it makes the upload order stable.
    std::sort( cand.begin(), cand.end(), []( const Cand& a, const Cand& b ) {
        return a.dist2 < b.dist2;
    } );

    const int markMax = std::max( 0, int{ cvar::rt_light_mark_max } );
    int       marked  = 0;

    for( const Cand& c : cand )
    {
        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( c.hue.X, c.hue.Y, c.hue.Z, 1.0f ),
            .intensity = c.intensity,
            .position  = { float( c.x ) * ONEGAMEUNIT_IN_METERS,
                           float( c.y ) * ONEGAMEUNIT_IN_METERS,
                           float( c.z ) * ONEGAMEUNIT_IN_METERS },
            .radius    = c.radius,
        };

        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sph,
            .uniqueID     = c.id,
            .isExportable = false,
        };

        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );
        uploaded++;

        // Cyan, not the wall strips' magenta: with both paths marked at once the only
        // useful question is which one owns a given light, and two colours answer it
        // without a second toggle.
        if( cvar::rt_ceiling_edge_debug_marks && marked < markMax )
        {
            marked++;
            auto markSph = RgLightSphericalEXT{
                .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                .pNext     = nullptr,
                .color     = rt.rgUtilPackColorByte4D( 0, 255, 255, 255 ),
                .intensity = std::max( 0.f, float{ cvar::rt_light_mark_intensity } ),
                .position  = sph.position,
                .radius    = 0.05f,
            };
            auto markInfo = RgLightInfo{
                .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                .pNext        = &markSph,
                .uniqueID     = info.uniqueID + 0x02000000ull,
                .isExportable = false,
            };
            RgResult mr = rt.rgUploadLight( &markInfo );
            RG_CHECK( mr );
        }
    }

    if( cvar::rt_ceiling_edge_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            // `wanted` vs `uploaded` is the point of this line: they were equal only
            // because the walk stopped at the cap, which hid that one sector was taking
            // the entire budget.
            // Faux and solo counted separately on purpose: the whole reason the budgets
            // are split is so a glance can tell whether invented or solo fixtures are
            // crowding real ones.
            Printf( "rt_ceiling_edge: uploaded=%d of %d wanted (cap %d, within %.0fu) "
                    "from %d lamp ceiling(s) + %d lamp floor(s) + %d bulb lattice(s) | I=%.0f | "
                    "faux %d flat(s), %d of %d wanted (cap %d) I=%.0f | "
                    "solo %d flat(s), %d of %d wanted (cap %d) I=%.0f\n",
                    uploaded,
                    wanted,
                    maxLights,
                    maxDist,
                    lampCeils,
                    lampFloors,
                    bulbLattices,
                    peak,
                    fauxFlats,
                    int( fauxCand.size() ),
                    fauxWanted,
                    fauxMax,
                    fauxIntensity,
                    soloFlats,
                    int( soloCand.size() ),
                    soloWanted,
                    soloMax,
                    soloIntensity );
        }
    }
}

// Names the wall fixtures near the camera, so a light-strip matcher can be written
// against real texture names instead of guesses. Prints sector lightlevel too, since
// that is the other half of the "is this a light fixture" test.
void RT_DebugNearbyWallTextures()
{
    if( !cvar::rt_wall_tex_debug || !primaryLevel )
    {
        return;
    }

    static int s_tick;
    if( ( ++s_tick % 60 ) != 0 )
    {
        return;
    }

    const DVector3 vp = r_viewpoint.Pos;
    const double   maxDist =
        std::max( 32.0, double( float{ cvar::rt_wall_tex_debug_dist } ) );

    struct Hit
    {
        double      dist;
        const char* tex;
        int         lightlevel;
        int         part;
        double      x, y;
        double      zLow, zHigh;
    };
    std::vector< Hit > hits;

    // Flats too: Doom 64 puts light fixtures on thin sector steps whose floor/ceiling
    // carry the lamp texture, and a sidedef-only dump cannot see those at all.
    for( unsigned i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        const sector_t& sec = primaryLevel->sectors[ i ];

        const double cx = double( sec.centerspot.X );
        const double cy = double( sec.centerspot.Y );
        const double d  = std::hypot( cx - vp.X, cy - vp.Y );
        if( d > maxDist )
        {
            continue;
        }

        for( int pl = 0; pl < 2; pl++ )
        {
            auto* gtex = TexMan.GetGameTexture(
                sec.GetTexture( pl == 0 ? sector_t::floor : sector_t::ceiling ), true );
            if( !gtex )
            {
                continue;
            }
            const char* nm = gtex->GetName().GetChars();
            if( !nm || !*nm )
            {
                continue;
            }
            const double zPlane = pl == 0 ? sec.floorplane.ZatPoint( sec.centerspot )
                                          : sec.ceilingplane.ZatPoint( sec.centerspot );
            hits.push_back(
                { d, nm, sec.lightlevel, pl == 0 ? 3 : 4, cx, cy, zPlane, zPlane } );
        }
    }

    for( unsigned i = 0; i < primaryLevel->lines.Size(); i++ )
    {
        const line_t& line = primaryLevel->lines[ i ];
        if( !line.v1 || !line.v2 )
        {
            continue;
        }

        const double mx = ( line.v1->fX() + line.v2->fX() ) * 0.5;
        const double my = ( line.v1->fY() + line.v2->fY() ) * 0.5;
        const double d  = std::hypot( mx - vp.X, my - vp.Y );
        if( d > maxDist )
        {
            continue;
        }

        for( int s = 0; s < 2; s++ )
        {
            const side_t* side = line.sidedef[ s ];
            if( !side )
            {
                continue;
            }
            const sector_t* sec = side->sector;

            const side_t*   otherSide = line.sidedef[ 1 - s ];
            const sector_t* otherSec  = otherSide ? otherSide->sector : nullptr;
            const DVector2  mid{ mx, my };

            for( int part = 0; part < 3; part++ )
            {
                auto* gtex = TexMan.GetGameTexture( side->GetTexture( part ), true );
                if( !gtex )
                {
                    continue;
                }
                const char* nm = gtex->GetName().GetChars();
                if( !nm || !*nm )
                {
                    continue;
                }

                // Height of the band this part occupies. This is what identifies a
                // ceiling-level fixture: the name alone does not say where it sits.
                double zLow = 0.0, zHigh = 0.0;
                if( sec )
                {
                    if( part == side_t::top && otherSec )
                    {
                        zLow  = otherSec->ceilingplane.ZatPoint( mid );
                        zHigh = sec->ceilingplane.ZatPoint( mid );
                    }
                    else if( part == side_t::bottom && otherSec )
                    {
                        zLow  = sec->floorplane.ZatPoint( mid );
                        zHigh = otherSec->floorplane.ZatPoint( mid );
                    }
                    else
                    {
                        zLow  = sec->floorplane.ZatPoint( mid );
                        zHigh = sec->ceilingplane.ZatPoint( mid );
                    }
                    if( zHigh < zLow )
                    {
                        std::swap( zLow, zHigh );
                    }
                }

                hits.push_back(
                    { d, nm, sec ? sec->lightlevel : -1, part, mx, my, zLow, zHigh } );
            }
        }
    }

    // Aggregate by texture name. The first version printed the 12 nearest rows, which was
    // dominated by a handful of repeated wall panels and hid the rarer fixture textures
    // entirely -- the upper light strips never appeared in the list at all.
    struct Agg
    {
        double nearest;
        int    count;
        bool   parts[ 5 ]; // top, mid, bot, floor, ceil
        int    minLight;
        int    maxLight;
        double zLow;
        double zHigh;
    };
    std::unordered_map< std::string, Agg > byTex;

    for( const Hit& h : hits )
    {
        auto it = byTex.find( h.tex );
        if( it == byTex.end() )
        {
            Agg a{ h.dist,      1,           { false, false, false, false, false },
                   h.lightlevel, h.lightlevel, h.zLow,
                   h.zHigh };
            a.parts[ h.part ] = true;
            byTex.emplace( h.tex, a );
        }
        else
        {
            Agg& a      = it->second;
            a.nearest   = std::min( a.nearest, h.dist );
            a.count++;
            a.parts[ h.part ] = true;
            a.minLight        = std::min( a.minLight, h.lightlevel );
            a.maxLight        = std::max( a.maxLight, h.lightlevel );
            a.zLow            = std::min( a.zLow, h.zLow );
            a.zHigh           = std::max( a.zHigh, h.zHigh );
        }
    }

    std::vector< std::pair< std::string, Agg > > sorted( byTex.begin(), byTex.end() );
    std::sort( sorted.begin(), sorted.end(), []( const auto& a, const auto& b ) {
        return a.second.nearest < b.second.nearest;
    } );

    Printf( "rt_wall_tex_debug: %zu sidedef texture(s), %zu distinct, within %.0fu\n",
            hits.size(),
            sorted.size(),
            maxDist );
    // Print every distinct name, not a nearest-N slice. The list is already bounded by
    // rt_wall_tex_debug_dist, and the old cap of 24 silently dropped 6 of MAP02's 30 --
    // sorted by distance, so the ones cut were the far ones, which is exactly where a
    // strip on the far side of a room lands. It printed "30 distinct" and then listed 24
    // with no truncation notice, which is the &sect;14 failure over again (2026-08-08).
    constexpr size_t MaxRows = 96;
    for( size_t n = 0; n < std::min( MaxRows, sorted.size() ); n++ )
    {
        const Agg& a = sorted[ n ].second;
        Printf( "  '%s' nearest=%.0f uses=%d parts=%s%s%s%s%s z=%.0f..%.0f lightlevel=%d..%d%s\n",
                sorted[ n ].first.c_str(),
                a.nearest,
                a.count,
                a.parts[ 0 ] ? "top " : "",
                a.parts[ 1 ] ? "mid " : "",
                a.parts[ 2 ] ? "bot " : "",
                a.parts[ 3 ] ? "FLOOR " : "",
                a.parts[ 4 ] ? "CEIL" : "",
                a.zLow,
                a.zHigh,
                a.minLight,
                a.maxLight,
                RT_IsWallStripLampTexture( sorted[ n ].first.c_str() ) ? "  <-- MATCHED as strip"
                                                                      : "" );
    }
    if( sorted.size() > MaxRows )
    {
        Printf( "  ... %zu more not shown -- lower rt_wall_tex_debug_dist to see them\n",
                sorted.size() - MaxRows );
    }
}

void RT_UploadHangingTechLamps()
{
    // MAP04 first room (and many D64 halls) hang LMP1/LMP2 props with BRIGHT sprites
    // but no co-located PointLight things — bulbs look lit, room stays flat ambient.
    // Place a warm analytic sphere at each hanging lamp actor so PT casts pools/shadows.
    if( !cvar::rt_hang_lamps || !primaryLevel )
    {
        return;
    }

    const float hangPeak  = std::max( 0.f, float{ cvar::rt_hang_lamp_intensity } );
    const float polePeak  = std::max( 0.f, float{ cvar::rt_pole_lamp_intensity } );
    const float srcRadius = std::max( 0.01f, float{ cvar::rt_hang_lamp_radius } );
    const float zOfs      = float{ cvar::rt_hang_lamp_zofs };
    if( hangPeak <= 0.01f && polePeak <= 0.01f )
    {
        return;
    }

    const int maptime  = primaryLevel->maptime;
    uint32_t  uploaded = 0;

    auto it = primaryLevel->GetThinkerIterator< AActor >();
    AActor* mo = nullptr;
    while( ( mo = it.Next() ) != nullptr )
    {
        const RtTechLamp kind = RT_TechLampKind( mo );
        if( kind == RtTechLamp::None )
        {
            continue;
        }
        const float peak = ( kind == RtTechLamp::Pole ) ? polePeak : hangPeak;
        if( peak <= 0.01f )
        {
            continue;
        }
        // Skip fully faded / non-rendered.
        if( mo->renderflags & RF_INVISIBLE )
        {
            continue;
        }
        if( mo->Alpha <= 0.01 )
        {
            continue;
        }

        // Hang: SPAWNCEILING, so actor Z is the bottom of the bbox (Top() touches the
        //       ceiling) and the bulb sits in the lower part of the fixture; zofs nudges
        //       further down.
        // Pole: floor-standing, bulb in the head at the top. zofs is NOT applied — it
        //       means "down from the bulb estimate", which on a pole walks the light
        //       into the shaft.
        const float px = float( mo->X() ) * ONEGAMEUNIT_IN_METERS;
        const float py = float( mo->Y() ) * ONEGAMEUNIT_IN_METERS;
        const float zBulb =
            ( kind == RtTechLamp::Pole )
                ? float( mo->Z() ) + float( mo->Height ) * float( cvar::rt_pole_lamp_zfrac )
                : float( mo->Z() ) + float( mo->Height ) * 0.35f - zOfs;
        const float pz = zBulb * ONEGAMEUNIT_IN_METERS;

        // Mild per-lamp phase so a dense hall doesn't hard-sync (no full blackout —
        // RR hates lights leaving the list; keep >= ~0.85).
        const int   phase = int( ( maptime * 3 + int( mo->X() ) + int( mo->Y() ) * 7 ) % 256 );
        const float flicker =
            ( phase < 6 ) ? 0.88f : ( phase >= 130 && phase < 134 ) ? 0.92f : 1.f;
        const float intensity = peak * flicker;

        // Stable ID from actor pointer (same pattern as dynlights).
        const uint64_t stableId =
            HangLampId_Base + ( uint64_t{ reinterpret_cast< uintptr_t >( mo ) } & 0xFFFFFFFFull );

        // Warm amber base — matches LMP bulb albedo better than pure white — tinted by
        // the hue of the sector the lamp actually hangs in (see RT_SectorHue).
        const sector_t* lampSector = mo->Sector;
        const FVector3  hue =
            lampSector ? RT_SectorHue( lampSector->Colormap.LightColor,
                                       float{ cvar::rt_sector_tint_lights } )
                       : FVector3{ 1.0f, 1.0f, 1.0f };

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( 1.000f * hue.X,
                                                    0.784f * hue.Y,
                                                    0.471f * hue.Z,
                                                    1.0f ),
            .intensity = intensity,
            .position  = { px, py, pz },
            .radius    = srcRadius,
        };

        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sph,
            .uniqueID     = stableId,
            .isExportable = false,
        };

        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        if( cvar::rt_hang_lamp_debug )
        {
            auto markSph = RgLightSphericalEXT{
                .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                .pNext     = nullptr,
                .color     = rt.rgUtilPackColorByte4D( 255, 255, 0, 255 ),
                .intensity = 350.f,
                .position  = { px, py, pz },
                .radius    = 0.05f,
            };
            auto markInfo = RgLightInfo{
                .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                .pNext        = &markSph,
                .uniqueID     = stableId + 0x08000000ull,
                .isExportable = false,
            };
            r = rt.rgUploadLight( &markInfo );
            RG_CHECK( r );

            if( ( maptime % 35 ) == 0 && uploaded < 8 )
            {
                const char* sn = ( mo->sprite >= 0 && mo->sprite < sprites.Size() )
                                     ? sprites[ mo->sprite ].name
                                     : "?";
                Printf( "rt_hang_lamp: '%s'/%s xyz=(%.0f,%.0f,%.0f) I=%.0f\n",
                        sn,
                        mo->GetClass() ? mo->GetClass()->TypeName.GetChars() : "?",
                        float( mo->X() ),
                        float( mo->Y() ),
                        zBulb,
                        intensity );
            }
        }

        ++uploaded;
    }

    if( cvar::rt_hang_lamp_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            Printf( "rt_hang_lamp_debug: %u hanging tech lamp(s) uploaded\n", uploaded );
        }
    }
}

// Which Baron-family monster is this, if any? Both carry a magic glow on their fists:
// BOS2 the Hell Knight (green), BOSS the Baron of Hell (red). Sprite first, class name as
// the fallback — same order as RT_TechLampKind, because a sprite replacement can desync
// the two.
//
// Matched in FULL, never on a 'BOS' prefix: the two differ only in the 4th character and
// they need different colours, so a prefix match would silently give the Baron the Hell
// Knight's green.
static int RT_HandGlowMonster( AActor* mo )
{
    if( !mo )
    {
        return -1;
    }
    if( mo->sprite >= 0 && mo->sprite < int( sprites.Size() ) )
    {
        const char* sn = sprites[ mo->sprite ].name;
        if( sn && strnicmp( sn, "BOS2", 4 ) == 0 )
        {
            return RT_HAND_HELLKNIGHT;
        }
        if( sn && strnicmp( sn, "BOSS", 4 ) == 0 )
        {
            return RT_HAND_BARON;
        }
    }
    if( mo->GetClass() && mo->GetClass()->TypeName.IsValidName() )
    {
        const char* cn = mo->GetClass()->TypeName.GetChars();
        if( cn && strnicmp( cn, "64HellKnight", 12 ) == 0 )
        {
            return RT_HAND_HELLKNIGHT;
        }
        // "64BaronOfHell" — not a prefix of the Hell Knight's name, so order is safe here.
        if( cn && strnicmp( cn, "64BaronOfHell", 13 ) == 0 )
        {
            return RT_HAND_BARON;
        }
    }
    return -1;
}

void RT_UploadHandGlowLights()
{
    // The Hell Knight carries a green magic glow on its fists. That glow is texture
    // emissive, and RTGL1 emissive is never a light source (rt_wall_strips explains why),
    // so the only illumination was the sprite's attached light — which RTGL1 pins to the
    // CENTRE of the billboard quad. Result: light out of the torso, and the two fists
    // collapsed to one point between them. Here each fist gets its own analytic sphere at
    // its real body-relative position, taken from the authored brightmaps.
    if( !cvar::rt_hand_light_on || !primaryLevel )
    {
        return;
    }

    const float intensity = std::max( 0.f, float{ cvar::rt_hand_light_intensity } );
    if( intensity <= 0.01f )
    {
        return;
    }
    const float  srcRadius = std::max( 0.01f, float{ cvar::rt_hand_light_radius } );
    const double maxDist   = std::max( 64.0, double( float{ cvar::rt_hand_light_maxdist } ) );
    const double maxDist2  = maxDist * maxDist;
    const int    budget    = std::max( 0, int{ cvar::rt_hand_light_max } );
    if( budget == 0 )
    {
        return;
    }

    const DVector3 vpos = r_viewpoint.Pos;

    // Collect then trim nearest-first. Same shape as the ceiling-edge/solo systems: a
    // distance filter alone does not bound the count, and letting the far half win by
    // iteration order is what made distant lamps pop in there.
    struct HandCand
    {
        double   d2;
        float    px, py, pz;
        uint64_t id;
        int      monster; // index into RT_HAND_COLOR — green knight vs red baron
    };
    std::vector< HandCand > cand;

    auto    it = primaryLevel->GetThinkerIterator< AActor >();
    AActor* mo = nullptr;
    while( ( mo = it.Next() ) != nullptr )
    {
        const int monster = RT_HandGlowMonster( mo );
        if( monster < 0 )
        {
            continue;
        }
        if( mo->renderflags & RF_INVISIBLE )
        {
            continue;
        }
        if( mo->Alpha <= 0.01 )
        {
            continue;
        }
        // Frames I..N are death/gib. They have no entry in the table and must not light:
        // the gore reuses the hand glow's own palette ramp, so a lit corpse would flare.
        const int frame = mo->frame;
        if( frame < 0 || frame >= RT_HAND_FRAME_COUNT )
        {
            continue;
        }
        const RtHandFrame& hk = RT_HAND_FRAMES[ monster ][ frame ];
        if( hk.count <= 0 )
        {
            continue;
        }

        // Offsets are body-relative, so rotate them by the actor's facing. Doom angle 0
        // is +X; "right of facing" is yaw - 90 degrees.
        const double yaw = mo->Angles.Yaw.Radians();
        const double fx = std::cos( yaw ), fy = std::sin( yaw );
        const double rx = std::sin( yaw ), ry = -std::cos( yaw );

        for( int h = 0; h < hk.count && h < 2; ++h )
        {
            const RtHandPos& hand = hk.hands[ h ];

            const double wx = double( mo->X() ) + rx * hand.lateral + fx * hand.fwd;
            const double wy = double( mo->Y() ) + ry * hand.lateral + fy * hand.fwd;
            const double wz = double( mo->Z() ) + hand.up;

            const double dx = wx - vpos.X, dy = wy - vpos.Y, dz = wz - vpos.Z;
            const double d2 = dx * dx + dy * dy + dz * dz;
            if( d2 > maxDist2 )
            {
                continue;
            }

            // Stable across frames: actor identity + hand index. An id that shifted per
            // tick would make RTGL1 see the set vanish and reappear.
            const uint64_t id = HandLightId_Base +
                                ( ( uint64_t{ reinterpret_cast< uintptr_t >( mo ) } &
                                    0xFFFFFFFFull )
                                  << 1 ) +
                                uint64_t( h );

            cand.push_back( HandCand{
                d2,
                float( wx ) * ONEGAMEUNIT_IN_METERS,
                float( wy ) * ONEGAMEUNIT_IN_METERS,
                float( wz ) * ONEGAMEUNIT_IN_METERS,
                id,
                monster,
            } );
        }
    }

    const size_t wanted = cand.size();
    if( wanted > size_t( budget ) )
    {
        std::partial_sort( cand.begin(),
                           cand.begin() + budget,
                           cand.end(),
                           []( const HandCand& a, const HandCand& b ) { return a.d2 < b.d2; } );
        cand.resize( size_t( budget ) );
    }

    for( const HandCand& c : cand )
    {
        // Colour comes from the generated table, never a literal here: it is the same
        // value the mask generator tints with, and a hardcoded copy would drift the cast
        // light away from the glow the moment either was retuned.
        const unsigned rgb = RT_HAND_COLOR[ c.monster ];
        const float    kR  = ( ( rgb >> 16 ) & 0xFF ) / 255.0f;
        const float    kG  = ( ( rgb >> 8 ) & 0xFF ) / 255.0f;
        const float    kB  = ( rgb & 0xFF ) / 255.0f;

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( kR, kG, kB, 1.0f ),
            .intensity = intensity,
            .position  = { c.px, c.py, c.pz },
            .radius    = srcRadius,
        };
        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sph,
            .uniqueID     = c.id,
            .isExportable = false,
        };
        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        if( cvar::rt_hand_light_debug )
        {
            auto markSph = RgLightSphericalEXT{
                .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                .pNext     = nullptr,
                .color     = rt.rgUtilPackColorByte4D( 255, 0, 255, 255 ),
                .intensity = 350.f,
                .position  = { c.px, c.py, c.pz },
                .radius    = 0.05f,
            };
            auto markInfo = RgLightInfo{
                .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                .pNext        = &markSph,
                .uniqueID     = c.id + ( 1ull << 41 ),
                .isExportable = false,
            };
            r = rt.rgUploadLight( &markInfo );
            RG_CHECK( r );
        }
    }

    if( cvar::rt_hand_light_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            Printf( "rt_hand_light: uploaded=%zu of %zu wanted (cap %d, within %.0fu) I=%.0f\n",
                    cand.size(),
                    wanted,
                    budget,
                    maxDist,
                    intensity );
        }
    }
}

// Every open flame in the game, keyed by sprite name.
//
// `up` and the relative brightness are the mod's OWN authored intent, lifted from its
// GLDEFS `flickerlight` blocks (offset 0 N 0 / size N), so nothing here is invented:
//
//   CANDLE          size 16  offset 0 16 0     REDFIRE etc.     size 32  offset 0  8 0
//   *TORCH (wall)   size 28  offset 0 24 0     TORCHSHORT*      size 40  offset 0 64 0
//                                              TORCHLONG*       size 40  offset 0 80 0
//
// Colours do NOT come from GLDEFS, which asks for fully-primary hues (0.0 1.0 0.0 green,
// 1.0 0.1 0.1 red). Those are the shared flame palette instead — the same four hexes the
// _e.png mask generators tint with (tools/gen_torch_emissives.py, gen_fx_emissives.py).
// Keeping cast light and on-screen glow on one palette is the whole point: they drifted
// apart once already (the LPUF regression), and a literal here would let it happen again.
struct RtFlameKind
{
    char     sprite[ 5 ];
    unsigned rgb;
    float    up;        // map units above the actor origin, from GLDEFS `offset 0 N 0`
    float    intensity; // RT intensity, scaled by GLDEFS `size` relative to the others
};

constexpr unsigned RT_FLAME_BLUE   = 0x4488FF;
constexpr unsigned RT_FLAME_GREEN  = 0x44FF66;
constexpr unsigned RT_FLAME_RED    = 0xFF4020;
constexpr unsigned RT_FLAME_YELLOW = 0xFFCC33;
// The candle is deliberately NOT FLAME_YELLOW. A candle is a single wick, not a pitch
// torch: it should read as a dim red ember at the edge of a dark room, so it takes a
// warm red of its own at a fraction of the intensity.
constexpr unsigned RT_FLAME_CANDLE = 0xFF4A14;

constexpr RtFlameKind RT_FLAME_KINDS[] = {
    // standing torches, long (27x100) — GLDEFS TORCHLONG*
    { "TLBL", RT_FLAME_BLUE, 80.f, 900.f },
    { "TLGR", RT_FLAME_GREEN, 80.f, 900.f },
    { "TLRD", RT_FLAME_RED, 80.f, 900.f },
    { "TLYL", RT_FLAME_YELLOW, 80.f, 900.f },
    // standing torches, short (18x85) — GLDEFS TORCHSHORT*, same size, lower offset
    { "TSBL", RT_FLAME_BLUE, 64.f, 900.f },
    { "TSGR", RT_FLAME_GREEN, 64.f, 900.f },
    { "TSRD", RT_FLAME_RED, 64.f, 900.f },
    { "TSYL", RT_FLAME_YELLOW, 64.f, 900.f },
    // wall sconces — GLDEFS size 28, so below the standing torches
    { "A030", RT_FLAME_YELLOW, 24.f, 700.f },
    { "A031", RT_FLAME_BLUE, 24.f, 700.f },
    { "A032", RT_FLAME_RED, 24.f, 700.f },
    { "GTCH", RT_FLAME_GREEN, 24.f, 700.f },
    // loose fires burning on the floor — GLDEFS size 32, offset only 8 up
    { "BFLM", RT_FLAME_BLUE, 8.f, 650.f },
    { "GFLM", RT_FLAME_GREEN, 8.f, 650.f },
    { "RFLM", RT_FLAME_RED, 8.f, 650.f },
    { "YFLM", RT_FLAME_YELLOW, 8.f, 650.f },
    // candle — GLDEFS size 16, the smallest flame in the game
    { "CAND", RT_FLAME_CANDLE, 16.f, 260.f },
};

static const RtFlameKind* RT_FlameKindOf( AActor* mo )
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
    // Full 4-character match, never a prefix. TL/TS families differ only in characters
    // 3-4, and A030/A031/A032 only in the last, so a prefix match would hand three wall
    // torches the same colour.
    for( const RtFlameKind& k : RT_FLAME_KINDS )
    {
        if( strnicmp( sn, k.sprite, 4 ) == 0 )
        {
            return &k;
        }
    }
    return nullptr;
}

void RT_UploadFlameLights()
{
    // Fire is the one light source in this game that must not hold still. See the
    // rt_flame_light_on cvar text for why neither half of that (the flicker, and the
    // offset up onto the flame) can be expressed in RTGL1 texture meta.
    if( !cvar::rt_flame_light_on || !primaryLevel )
    {
        return;
    }

    const float scale = std::max( 0.f, float{ cvar::rt_flame_light_scale } );
    if( scale <= 0.001f )
    {
        return;
    }
    const float  srcRadius = std::max( 0.01f, float{ cvar::rt_flame_light_radius } );
    const float  flicker   = std::clamp( float{ cvar::rt_flame_light_flicker }, 0.f, 1.f );
    const float  speed     = std::max( 0.f, float{ cvar::rt_flame_light_speed } );
    const float  wobble    = std::max( 0.f, float{ cvar::rt_flame_light_wobble } );
    const double maxDist   = std::max( 64.0, double( float{ cvar::rt_flame_light_maxdist } ) );
    const double maxDist2  = maxDist * maxDist;
    const int    budget    = std::max( 0, int{ cvar::rt_flame_light_max } );
    if( budget == 0 )
    {
        return;
    }

    // maptime, not wall clock: a paused or console-open game must freeze the fire with
    // everything else. 35 Hz stepping is not a limitation — GLDEFS' own flickerlight
    // re-rolls once per tic, so this is already the smoother of the two.
    const float t = float( primaryLevel->maptime ) * speed;

    const DVector3 vpos = r_viewpoint.Pos;

    struct FlameCand
    {
        double             d2;
        float              px, py, pz;
        float              intensity;
        uint64_t           id;
        const RtFlameKind* kind;
    };
    std::vector< FlameCand > cand;

    auto    it = primaryLevel->GetThinkerIterator< AActor >();
    AActor* mo = nullptr;
    while( ( mo = it.Next() ) != nullptr )
    {
        const RtFlameKind* kind = RT_FlameKindOf( mo );
        if( !kind )
        {
            continue;
        }
        if( ( mo->renderflags & RF_INVISIBLE ) || mo->Alpha <= 0.01 )
        {
            continue;
        }

        // Per-actor phase. Without it every torch in the level flickers in unison, which
        // is exactly the failure that rules out doing this from the sprite animation:
        // the props all spawn at map load, so their frame counters are already in lockstep.
        const uint64_t h = uint64_t( reinterpret_cast< uintptr_t >( mo ) ) >> 4;
        const float phase = float( h & 0xFFFF ) * ( 6.2831853f / 65536.0f );

        // Three incommensurate harmonics: the sum has no short period, so a torch the
        // player stands next to for a minute never visibly loops. Normalised by the sum
        // of the weights so `flicker` stays a true fraction of base intensity.
        const float f = ( 0.55f * std::sin( t + phase ) +          //
                          0.30f * std::sin( t * 2.37f + phase * 1.7f ) +
                          0.15f * std::sin( t * 4.11f + phase * 2.9f ) );

        const float intensity =
            std::max( 0.f, kind->intensity * scale * ( 1.0f + flicker * f ) );
        if( intensity <= 0.01f )
        {
            continue;
        }

        // The same three-harmonic trick on position, at different frequencies so the
        // drift does not simply track the brightness. Lateral only gets the full wobble;
        // vertical gets half, because a flame licks upward far more than it slides.
        const float wx = wobble * std::sin( t * 0.83f + phase * 2.1f );
        const float wy = wobble * std::sin( t * 1.19f + phase * 3.3f );
        const float wz = wobble * 0.5f * std::sin( t * 1.61f + phase * 1.3f );

        const double lx = double( mo->X() ) + wx;
        const double ly = double( mo->Y() ) + wy;
        const double lz = double( mo->Z() ) + double( kind->up ) + wz;

        const double dx = lx - vpos.X, dy = ly - vpos.Y, dz = lz - vpos.Z;
        const double d2 = dx * dx + dy * dy + dz * dz;
        if( d2 > maxDist2 )
        {
            continue;
        }

        cand.push_back( FlameCand{
            d2,
            float( lx ) * ONEGAMEUNIT_IN_METERS,
            float( ly ) * ONEGAMEUNIT_IN_METERS,
            float( lz ) * ONEGAMEUNIT_IN_METERS,
            intensity,
            // Stable across frames: derived from actor identity alone, never from the
            // tick. An id that moved would make RTGL1 see the whole set die and respawn
            // every frame, throwing away its temporal reservoirs.
            FlameLightId_Base + ( uint64_t( reinterpret_cast< uintptr_t >( mo ) ) & 0xFFFFFFFFull ),
            kind,
        } );
    }

    const size_t wanted = cand.size();
    if( wanted > size_t( budget ) )
    {
        std::partial_sort( cand.begin(),
                           cand.begin() + budget,
                           cand.end(),
                           []( const FlameCand& a, const FlameCand& b ) { return a.d2 < b.d2; } );
        cand.resize( size_t( budget ) );
    }

    for( const FlameCand& c : cand )
    {
        const float kR = ( ( c.kind->rgb >> 16 ) & 0xFF ) / 255.0f;
        const float kG = ( ( c.kind->rgb >> 8 ) & 0xFF ) / 255.0f;
        const float kB = ( c.kind->rgb & 0xFF ) / 255.0f;

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( kR, kG, kB, 1.0f ),
            .intensity = c.intensity,
            .position  = { c.px, c.py, c.pz },
            .radius    = srcRadius,
        };
        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sph,
            .uniqueID     = c.id,
            .isExportable = false,
        };
        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        if( cvar::rt_flame_light_debug )
        {
            auto markSph = RgLightSphericalEXT{
                .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                .pNext     = nullptr,
                .color     = rt.rgUtilPackColorByte4D( 0, 255, 255, 255 ),
                .intensity = 350.f,
                .position  = { c.px, c.py, c.pz },
                .radius    = 0.05f,
            };
            auto markInfo = RgLightInfo{
                .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                .pNext        = &markSph,
                .uniqueID     = c.id + ( 1ull << 41 ),
                .isExportable = false,
            };
            r = rt.rgUploadLight( &markInfo );
            RG_CHECK( r );
        }
    }

    if( cvar::rt_flame_light_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            Printf( "rt_flame_light: uploaded=%zu of %zu wanted (cap %d, within %.0fu) "
                    "scale=%.2f flicker=%.2f wobble=%.1f\n",
                    cand.size(),
                    wanted,
                    budget,
                    maxDist,
                    scale,
                    flicker,
                    wobble );
        }
    }
}

} // anonymous namespace

//
//
//

// Special extension
#define ext_RG_STRUCTURE_TYPE_START_FRAME_REMIX_PARAMS ( ( RgStructureType )1024 )
struct ext_RgStartFrameRemixParams
{
    RgStructureType sType;
    void*           pNext;
    RgBool32        rayReconstruction;
    RgBool32        taa;
    RgBool32        nis;
    RgBool32        reflex;
};

void RTFrameBuffer::RT_BeginFrame()
{
    // HACKHACK begin
    if( g_rt_skipinitframes == -10 )
    {
        g_rt_skipinitframes = cvar::hack_initialframesskip ? 0 : 2;
    }
    if( g_rt_skipinitframes >= 0 )
    {
        if( g_rt_skipinitframes == 0 )
        {
            RT_CloseLauncherWindow(); // renderer is ready, close launcher window
            PositionWindow( IsFullscreen() );
            g_rt_forcenofocuschange = false;
        }
        --g_rt_skipinitframes;
    }
    // HACKHACK end


    m_state->RT_BeginFrame();

    classic_toggle::Animate();


    auto resolution_params = RgStartFrameRenderResolutionParams{
        .sType             = RG_STRUCTURE_TYPE_START_FRAME_RENDER_RESOLUTION_PARAMS,
        .pNext             = nullptr,
        .preferDxgiPresent = cvar::rt_available_dxgi ? cvar::rt_dxgi : false,
    };
    RT_ResolutionToRtgl( &resolution_params, RT_GetCurrentWindowSize() );
    RT_UpscaleCvarsToRtgl( &resolution_params );

    ext_RgStartFrameRemixParams remix_params;
    if( g_isremix )
    {
        remix_params = ext_RgStartFrameRemixParams{
            .sType             = ext_RG_STRUCTURE_TYPE_START_FRAME_REMIX_PARAMS,
            .pNext             = nullptr,
            .rayReconstruction = ( cvar::rt_remix_rayreconstr ? 1u : 0u ),
            .taa               = ( cvar::rt_remix_taa > 0 ? 1u : 0u ),
            .nis               = 0,
            .reflex            = ( cvar::rt_remix_reflex ? 1u : 0u ),
        };

        switch( int( cvar::rt_remix_taa ) )
        {
            case 4:
                resolution_params.resolutionMode = RG_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE;
                break;
            case 3: resolution_params.resolutionMode = RG_RENDER_RESOLUTION_MODE_PERFORMANCE; break;
            case 2: resolution_params.resolutionMode = RG_RENDER_RESOLUTION_MODE_BALANCED; break;
            case 1: resolution_params.resolutionMode = RG_RENDER_RESOLUTION_MODE_QUALITY; break;
            case 6: resolution_params.resolutionMode = RG_RENDER_RESOLUTION_MODE_NATIVE_AA; break;
            case 5: resolution_params.resolutionMode = RG_RENDER_RESOLUTION_MODE_CUSTOM; break;
            default: remix_params.taa = 0; break;
        }

        remix_params.pNext      = resolution_params.pNext;
        resolution_params.pNext = &remix_params;
    }

    RT_MakeLightstyles();

    auto fluid_params = RgStartFrameFluidParams{
        .sType          = RG_STRUCTURE_TYPE_START_FRAME_FLUID_PARAMS,
        .pNext          = &resolution_params,
        .enabled        = cvar::rt_fluid_available ? cvar::rt_fluid : false,
        .reset          = g_resetfluid,
        .gravity        = { cvar::rt_fluid_gravity_x, //
                            cvar::rt_fluid_gravity_y,
                            cvar::rt_fluid_gravity_z },
        .color          = { cvar::rt_blood_color_r, //
                            cvar::rt_blood_color_g,
                            cvar::rt_blood_color_b },
        .particleBudget = uint32_t( std::max( 0, int( cvar::rt_fluid_budget ) ) ),
        .particleRadius = cvar::rt_fluid_pradius,
    };

    RgStaticSceneStatusFlags staticscene_status = 0;

    // Mod maps (RT_MapName like "d64rtr_v15_map01") must not auto-export until a scene exists:
    // export + uncull-all freezes the main thread on large UDMF maps.
    const char* mapname_for_rt = RT_GetMapName();
    const bool  is_mod_map =
        mapname_for_rt && strchr( mapname_for_rt, '_' ) != nullptr;
    const bool allow_autoexport = cvar::rt_autoexport && !is_mod_map;

    auto info = RgStartFrameInfo{
        .sType                  = RG_STRUCTURE_TYPE_START_FRAME_INFO,
        .pNext                  = &fluid_params,
        .pMapName               = mapname_for_rt,
        .ignoreExternalGeometry = false,
        .vsync                  = cvar::rt_vsync,
        .hdr                    = cvar::rt_hdr_available ? cvar::rt_hdr : false,
        .allowMapAutoExport     = allow_autoexport,
        .lightmapScreenCoverage = RT_ForceNoClassicMode() ? 0.0f : cvar::rt_classic,
        .lightstyleValuesCount  = uint32_t( g_sectorlightlevels.size() ),
        .pLightstyleValues8     = g_sectorlightlevels.data(),
        .pResultStaticSceneStatus = &staticscene_status,
        .staticSceneAnimationTime = g_rt_cutscenename ? RT_CutsceneTime() : 0,
    };
    g_resetfluid = false;

    RgResult r = rt.rgStartFrame( &info );
    RG_CHECK( r );


    auto l_clm = [ staticscene_status, is_mod_map ]() {
        // Doom64-RT: uncull-all (mode 2) on large UDMF mods freezes the main thread
        // for a long time (looks hung; needs force-close). Never do it for mod maps.
        if( !is_mod_map )
        {
            if( staticscene_status & RG_STATIC_SCENE_STATUS_EXPORT_STARTED )
            {
                return 2; // no cull as we need to upload all geometry for the first time
            }
            if( staticscene_status & RG_STATIC_SCENE_STATUS_NEW_SCENE_STARTED )
            {
                return 2; // touch everything, to upload all resources
            }
        }
        switch( int( cvar::rt_cpu_cullmode ) )
        {
            case 1: return 1;
            case 2: return 2;
            default: return 0;
        }
    };

    rt_cullmode = l_clm();
}

void RTFrameBuffer::RT_DrawFrame()
{
    const double   curtime      = RT_GetCurrentTime();
    const uint32_t powerupflags = RT_CalcPowerupFlags();

    RT_DrawTitle();

    if( bool{ cvar::rt_sun } && float{ cvar::rt_sun_intensity } > 0 )
    {
        float altitude = to_rad( float{ cvar::rt_sun_a } );
        float azimuth  = to_rad( float{ cvar::rt_sun_b } );

        float theta = std::clamp( pi() / 2 - altitude, 0.f, pi() );
        float phi   = std::fmod( azimuth, pi() * 2 );

        // negate, direction from the sun, not towards the sun
        auto dir = RgFloat3D{
            -sin( theta ) * cos( phi ),
            -sin( theta ) * sin( phi ),
            -cos( theta ),
        };

        auto s = RgLightDirectionalEXT{
            .sType                  = RG_STRUCTURE_TYPE_LIGHT_DIRECTIONAL_EXT,
            .pNext                  = nullptr,
            .color                  = cvarcolor_to_rtcolor( cvar::rt_sun_color ),
            .intensity              = float{ cvar::rt_sun_intensity },
            .direction              = dir,
            // The size gate for sky leaks, and the reason it is an ANGLE.
            //
            // At 0.5 degrees -- the real moon -- this light is effectively a
            // point, so its shadow ray is a single yes/no test. One unblocked
            // ray through a hand-width crack delivers exactly as much light as
            // an open doorway, which is why a pinhole leak reads as a full-
            // strength shaft and why no per-surface rule could fix it: the wall
            // holes MAP13 wants lit and the cracks it does not are the same kind
            // of geometry.
            //
            // Widen the disc and the test stops being binary. RTGL1 samples a
            // point on it per shadow ray (sampleDirectionalLight -> sampleDisk),
            // so an opening now admits light in proportion to how much of the
            // disc it actually reveals. A doorway reveals all of it and is
            // unchanged; a narrow band reveals a sliver and dims smoothly.
            //
            // Crucially it also falls off with DISTANCE, which is the behaviour
            // actually wanted here: an opening of size d seen from L away
            // subtends d/L, so the same band still lights the surfaces beside it
            // and stops washing a ceiling 2000 units off. That is a soft
            // rolloff, not a cutoff -- "too small a hole" is only meaningful
            // relative to how far away you are standing, so a hard threshold
            // could not have been right at any single value.
            //
            // Costs sharpness on the wanted shafts too: this is one knob for
            // both, traded with rt_sun_angdiam.
            .angularDiameterDegrees = std::clamp( float{ cvar::rt_sun_angdiam }, 0.01f, 90.f ),
        };

        auto i = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &s,
            .uniqueID     = SunLightId,
            .isExportable = false,
        };

        RgResult r = rt.rgUploadLight( &i );
        RG_CHECK( r );
    }

    RT_UploadExportableSectorLights();
    RT_UploadGzDoomDynamicLights();
    RT_UploadCeilingInsetLamps();
    RT_UploadHangingTechLamps();
    RT_UploadHandGlowLights();
    RT_UploadFlameLights();
    RT_UpdateSectorEmisThreshold();
    RT_UploadWallStripLights();
    RT_UploadCeilingEdgeLamps();
    RT_DebugNearbyWallTextures();

    auto tm_params = RgDrawFrameTonemappingParams{
        .sType                = RG_STRUCTURE_TYPE_DRAW_FRAME_TONEMAPPING_PARAMS,
        .pNext                = nullptr,
        .disableEyeAdaptation = false,
        .ev100Min             = cvar::rt_tnmp_ev100_min,
        .ev100Max             = cvar::rt_tnmp_ev100_max,
        .luminanceWhitePoint  = cvar::rt_classic_white,
        .saturation           = { cvar::rt_tnmp_saturation_r,
                                  cvar::rt_tnmp_saturation_g,
                                  cvar::rt_tnmp_saturation_b },
        .crosstalk            = { cvar::rt_tnmp_crosstalk_r,
                                  cvar::rt_tnmp_crosstalk_g,
                                  cvar::rt_tnmp_crosstalk_b },
        .contrast             = cvar::rt_tnmp_contrast,
        .hdrBrightness        = cvar::rt_hdr_brightness,
        .hdrContrast          = cvar::rt_hdr_contrast,
        .hdrSaturation        = { cvar::rt_hdr_saturation,
                                  cvar::rt_hdr_saturation,
                                  cvar::rt_hdr_saturation },
    };

    auto reflrefr_params = RgDrawFrameReflectRefractParams{
        .sType                   = RG_STRUCTURE_TYPE_DRAW_FRAME_REFLECT_REFRACT_PARAMS,
        .pNext                   = &tm_params,
        .maxReflectRefractDepth  = safe_uint( *cvar::rt_reflrefr_depth ),
        .typeOfMediaAroundCamera = RG_MEDIA_TYPE_VACUUM,
        .indexOfRefractionGlass  = cvar::rt_refr_glass,
        .indexOfRefractionWater  = cvar::rt_refr_water,
        .waterWaveSpeed          = cvar::rt_water_wavespeed,
        .waterWaveNormalStrength = cvar::rt_water_wavestren,
        .waterColor              = { std::clamp( *cvar::rt_water_r / 255.f, 0.f, 1.f ),
                                     std::clamp( *cvar::rt_water_g / 255.f, 0.f, 1.f ),
                                     std::clamp( *cvar::rt_water_b / 255.f, 0.f, 1.f ) },
        .waterWaveTextureDerivativesMultiplier = 1.0f,
        .waterTextureAreaScale                 = cvar::rt_water_areascale,
        .portalNormalTwirl                     = false,
        // Doom64-RT stylized water — see rt_water_style.
        .stylizedWaterStrength  = cvar::rt_water_style ? 1.0f : 0.0f,
        .stylizedWaterCaustic   = cvar::rt_water_caustic,
        .stylizedWaterReflMax   = cvar::rt_water_reflmax,
        .stylizedWaterRoughness = cvar::rt_water_rough,
        .stylizedWaterGlow      = cvar::rt_water_glow,
        .stylizedWaterVeinRef   = cvar::rt_water_veinref,
        .stylizedWaterTint      = { std::clamp( *cvar::rt_water_tint_r / 255.f, 0.f, 1.f ),
                                    std::clamp( *cvar::rt_water_tint_g / 255.f, 0.f, 1.f ),
                                    std::clamp( *cvar::rt_water_tint_b / 255.f, 0.f, 1.f ) },
        .stylizedWaterDebug     = cvar::rt_water_debug ? 1.0f : 0.0f,
        .stylizedWaterReflMin   = cvar::rt_water_reflmin,
        .waterCausticGain       = cvar::rt_water_caustics,
        .waterCausticScale      = cvar::rt_water_caustic_scale,
        .waterCausticSpeed      = cvar::rt_water_caustic_speed,
        // map units -> metres: RTGL world space is metres. Passing map units
        // straight through made the probe ray 6144 m long, so a wall anywhere
        // above any water in the map got lit.
        .waterCausticDist       = cvar::rt_water_caustic_dist * ONEGAMEUNIT_IN_METERS,
    };

    auto sky_params = RgDrawFrameSkyParams{
        .sType              = RG_STRUCTURE_TYPE_DRAW_FRAME_SKY_PARAMS,
        .pNext              = &reflrefr_params,
        .skyType            = m_wassky ? RG_SKY_TYPE_RASTERIZED_GEOMETRY : RG_SKY_TYPE_COLOR,
        // Dark space tint if raster sky failed (pure black often tonemaps to white voids).
        .skyColorDefault    = { 0.02f, 0.02f, 0.05f },
        .skyColorMultiplier = cvar::rt_sky,
        .skyColorSaturation = cvar::rt_sky_saturation,
        .skyViewerPosition  = { 0, 0, 0 },
    };

    auto volumetrics_params = RgDrawFrameVolumetricParams{
        .sType                   = RG_STRUCTURE_TYPE_DRAW_FRAME_VOLUMETRIC_PARAMS,
        .pNext                   = &sky_params,
        .enable                  = cvar::rt_volume_type != 0,
        .maxHistoryLength        = cvar::rt_volume_type == 1 ? cvar::rt_volume_history : 0.f,
        .useSimpleDepthBased     = cvar::rt_volume_type == 2,
        .volumetricFar           = cvar::rt_volume_far,
        .ambientColor            = { cvar::rt_volume_ambient,
                                     cvar::rt_volume_ambient,
                                     cvar::rt_volume_ambient },
        .scaterring              = cvar::rt_volume_scatter,
        .assymetry               = cvar::rt_volume_lassymetry,
        .useIlluminationVolume   = cvar::rt_illum_volume && cvar::rt_volume_type != 0,
        .fallbackSourceColor     = { 0, 0, 0 },
        .fallbackSourceDirection = { 0, -1, 0 },
        .lightMultiplier         = cvar::rt_volume_lintensity,
        .allowTintUnderwater     = false,
        .underwaterColor         = {},
    };

    auto texture_params = RgDrawFrameTexturesParams{
        .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_TEXTURES_PARAMS,
        .pNext = &volumetrics_params,
        .dynamicSamplerFilter =
            cvar::rt_smoothtextures ? RG_SAMPLER_FILTER_LINEAR : RG_SAMPLER_FILTER_NEAREST,
        .mipLodBiasOffset       = float( cvar::rt_mip_bias ),
        .normalMapStrength      = cvar::rt_normalmap_stren,
        .emissionMapBoost       = cvar::rt_emis_mapboost,
        .emissionMaxScreenColor = cvar::rt_emis_maxscrcolor,
        .minRoughness           = cvar::rt_refl_thresh,
        .heightMapDepth         = 0.02f * cvar::rt_heightmap_stren,
    };

    float dirtscale = ( ( powerupflags & RT_POWERUP_FLAG_RADIATIONSUIT_BIT ) ||
                        ( powerupflags & RT_POWERUP_FLAG_NIGHTVISION_BIT ) )
                          ? 15.f
                          : cvar::rt_bloom_dirt_scale;

    auto bloom_params = RgDrawFrameBloomParams{
        .sType             = RG_STRUCTURE_TYPE_DRAW_FRAME_BLOOM_PARAMS,
        .pNext             = &texture_params,
        .inputEV           = cvar::rt_bloom_ev,
        .inputThreshold    = cvar::rt_bloom_threshold,
        .bloomIntensity    = cvar::rt_bloom ? cvar ::rt_bloom_scale : 0.f,
        .lensDirtIntensity = cvar::rt_bloom_dirt ? dirtscale : 0.f,
    };

    auto illum_params = RgDrawFrameIlluminationParams{
        .sType                              = RG_STRUCTURE_TYPE_DRAW_FRAME_ILLUMINATION_PARAMS,
        .pNext                              = &bloom_params,
        .maxBounceShadows                   = safe_uint( *cvar::rt_shadowrays ),
        .enableSecondBounceForIndirect      = true,
        .cellWorldSize                      = 2.0f,
        .directDiffuseSensitivityToChange   = std::clamp( float( cvar::rt_illum_sens_direct ), 0.f, 1.f ),
        .indirectDiffuseSensitivityToChange = std::clamp( float( cvar::rt_illum_sens_indirect ), 0.f, 1.f ),
        .specularSensitivityToChange        = std::clamp( float( cvar::rt_illum_sens_spec ), 0.f, 1.f ),
        .polygonalLightSpotlightFactor      = 2.0f,
        .lightUniqueIdIgnoreFirstPersonViewerShadows = &FlashlightLightId,
        .enableRrTemporalPrefilter          = static_cast< RgBool32 >( bool( cvar::rt_rr_temporal ) ),
        .enableRrDisocclusionMask           = static_cast< RgBool32 >( bool( cvar::rt_rr_disocc ) ),
        .rrDisocclusionThreshold            = std::max( float( cvar::rt_rr_disocc_ratio ), 1.0f ),
        .rrDisocclusionMinDelta             = std::max( float( cvar::rt_rr_disocc_mindelta ), 0.0f ),
        .rrDisocclusionShowMask             = static_cast< RgBool32 >( bool( cvar::rt_rr_disocc_show ) ),
        .rrFireflyThreshold                 = std::max( float( cvar::rt_rr_firefly ), 0.0f ),
        .rrFireflyMinLum                    = std::max( float( cvar::rt_rr_firefly_minlum ), 0.0f ),
        .restirBlueNoise                    = static_cast< RgBool32 >( bool( cvar::rt_restir_bluenoise ) ),
        .shadowSamples                      = uint32_t( std::clamp( int( cvar::rt_shadow_samples ), 1, 8 ) ),
        .debugRestirM                       = static_cast< RgBool32 >( bool( cvar::rt_debug_restir_m ) ),
        .debugVisibility                    = uint32_t( std::clamp( int( cvar::rt_debug_visibility ), 0, 2 ) ),
        .restirTemporalJitter               = std::clamp( float( cvar::rt_restir_tjitter ), 0.0f, 8.0f ),
        .rrSpecularHitDistance              = static_cast< RgBool32 >( bool( cvar::rt_rr_spechitdist ) ),
        .directSamples                      = uint32_t( std::clamp( int( cvar::rt_spp_direct ), 1, 8 ) ),
        .indirectSamples                    = uint32_t( std::clamp( int( cvar::rt_spp_indirect ), 1, 8 ) ),
        .restirInitialSamples               = uint32_t( std::clamp( int( cvar::rt_restir_initial ), 1, 32 ) ),
        .restirSpatialSamples               = uint32_t( std::clamp( int( cvar::rt_restir_spatial ), 0, 16 ) ),
        .restirSpatialRadius                = std::clamp( float( cvar::rt_restir_spatial_radius ), 1.0f, 64.0f ),
        .restirTemporalMCap                 = uint32_t( std::clamp( int( cvar::rt_restir_mcap ), 1, 64 ) ),
        .rrGuideMin                         = std::clamp( float( cvar::rt_rr_guide_min ), 0.0f, 1.0f ),
        .rrGuideMode                        = uint32_t( std::clamp( int( cvar::rt_rr_guide_mode ), 0, 2 ) ),
        .restirIndirAntilag                 = static_cast< RgBool32 >( bool( cvar::rt_restir_indir_antilag ) ),
    };

    auto ef_wipe = RgPostEffectWipe{
        .stripWidth = 1.0f / 320.0f,
        .beginNow   = cvar::rt_melt_duration > 0.05f ? g_melt_requested : false,
        .duration   = cvar::rt_melt_duration > 0.05f ? cvar::rt_melt_duration : 0.0f,
    };
    g_melt_requested = false;

    if( ef_wipe.beginNow )
    {
        g_melt_endtime = curtime + static_cast< double >( ef_wipe.duration );
    }
    if( g_melt_endtime > 0 && curtime > g_melt_endtime )
    {
        g_melt_endtime = -1;
    }

    auto ef_radialblur = RgPostEffectRadialBlur{
        .isActive              = powerupflags & RT_POWERUP_FLAG_BERSERK_BIT,
        .transitionDurationIn  = 0.4f,
        .transitionDurationOut = 3.0f,
    };

    bool chrabr_from_powerup = ( powerupflags & RT_POWERUP_FLAG_NIGHTVISION_BIT ) ||
                               ( powerupflags & RT_POWERUP_FLAG_THERMALVISION_BIT ) ||
                               ( powerupflags & RT_POWERUP_FLAG_BERSERK_BIT );

    auto ef_chrabr = RgPostEffectChromaticAberration{
        .isActive              = chrabr_from_powerup || cvar::rt_ef_chraber > 0.f,
        .transitionDurationIn  = 0,
        .transitionDurationOut = 0,
        .intensity             = chrabr_from_powerup ? 1.2f : cvar::rt_ef_chraber,
    };
    // smooth out manually (because it's a constant active effect, i.e. without switching isActive)
    {
        constexpr auto Duration     = 0.5f;
        static double  begin_time   = curtime;
        static float   last_value   = ef_chrabr.intensity;
        static float   begin_value  = ef_chrabr.intensity;
        static float   target_value = ef_chrabr.intensity;

        if( std::abs( target_value - ef_chrabr.intensity ) > 0.001f )
        {
            begin_time   = curtime;
            begin_value  = last_value;
            target_value = ef_chrabr.intensity;
        }

        // if( begin_time <= curtime && curtime <= begin_time + double( Duration ) )
        {
            const float t = std::clamp( float( curtime - begin_time ) / Duration, 0.0f, 1.0f );
            ef_chrabr.intensity = std::lerp( begin_value, target_value, t );
        }
        last_value = ef_chrabr.intensity;
    }

    auto ef_invbw = RgPostEffectInverseBlackAndWhite{
        .isActive              = powerupflags & RT_POWERUP_FLAG_INVUNERABILITY_BIT,
        .transitionDurationIn  = 1.0f,
        .transitionDurationOut = 1.5f,
    };

    auto ef_hueshift = RgPostEffectHueShift{
        .isActive              = powerupflags & RT_POWERUP_FLAG_THERMALVISION_BIT,
        .transitionDurationIn  = 0.5f,
        .transitionDurationOut = 0.5f,
    };

    auto ef_nightvision = RgPostEffectNightVision{
        .isActive              = powerupflags & RT_POWERUP_FLAG_NIGHTVISION_BIT,
        .transitionDurationIn  = 0.5f,
        .transitionDurationOut = 0.5f,
    };

    auto ef_distortedsides = RgPostEffectDistortedSides{
        .isActive              = powerupflags & RT_POWERUP_FLAG_RADIATIONSUIT_BIT,
        .transitionDurationIn  = 1.0f,
        .transitionDurationOut = 1.0f,
    };

    // static, so prev state's transition durations
    // are preserved across frames, when flags are removed
    static auto ef_tint = RgPostEffectColorTint{};
    {
        ef_tint.isActive = false;

        if( auto dmg = RT_DamageIntensity() )
        {
            ef_tint = RgPostEffectColorTint{
                .isActive              = true,
                .transitionDurationIn  = 0.0f,
                .transitionDurationOut = remap01( *dmg, 0.5f, 1.7f ),
                .intensity             = remap01( *dmg, 1.5f, 3.0f ) * blood_fade_scalar,
                .color                 = { 1.f, 0.f, 0.f },
            };
        }
        else if( powerupflags & RT_POWERUP_FLAG_RADIATIONSUIT_BIT )
        {
            ef_tint = RgPostEffectColorTint{
                .isActive              = true,
                .transitionDurationIn  = 1.0f,
                .transitionDurationOut = 1.0f,
                .intensity             = 1.0f,
                .color                 = { 0.2f, 1.f, 0.4f },
            };
        }
        else if( powerupflags & RT_POWERUP_FLAG_BONUS_BIT )
        {
            ef_tint = RgPostEffectColorTint{
                .isActive              = true,
                .transitionDurationIn  = 0.0f,
                .transitionDurationOut = 0.7f,
                .intensity             = 0.5f * pickup_fade_scalar,
                .color                 = { 1.f, 0.91f, 0.42f },
            };
        }
    }

    const int vintage_crt = int{ cvar::rt_ef_vintage } == RT_VINTAGE_CRT ||
                            int{ cvar::rt_ef_vintage } == RT_VINTAGE_VHS_CRT;
    const int vintage_vhs = int{ cvar::rt_ef_vintage } == RT_VINTAGE_VHS ||
                            int{ cvar::rt_ef_vintage } == RT_VINTAGE_VHS_CRT;
    const int vintage_dither = int{ cvar::rt_ef_vintage } == RT_VINTAGE_200_DITHER ||
                               int{ cvar::rt_ef_vintage } == RT_VINTAGE_480_DITHER;

    auto ef_crt = RgPostEffectCRT{
        .isActive = vintage_crt || cvar::rt_ef_crt,
    };

    auto ef_vhs = RgPostEffectVHS{
        .isActive              = vintage_vhs || cvar::rt_ef_vhs > 0.f,
        .transitionDurationIn  = 0,
        .transitionDurationOut = 0,
        .intensity             = vintage_vhs ? 0.9f : float{ cvar::rt_ef_vhs },
    };

    auto ef_dither = RgPostEffectDither{
        .isActive              = vintage_dither || cvar::rt_ef_dither > 0.f,
        .transitionDurationIn  = 0,
        .transitionDurationOut = 0,
        .intensity             = vintage_dither ? 0.8f : float{ cvar::rt_ef_dither },
    };

    // some of the power-up effects need to be reset
    auto post_params = RgDrawFramePostEffectsParams{
        .sType                 = RG_STRUCTURE_TYPE_DRAW_FRAME_POST_EFFECTS_PARAMS,
        .pNext                 = &illum_params,
        .pWipe                 = &ef_wipe,
        .pRadialBlur           = g_resetposteffects ? nullptr : &ef_radialblur,
        .pChromaticAberration  = &ef_chrabr,
        .pInverseBlackAndWhite = g_resetposteffects ? nullptr : &ef_invbw,
        .pHueShift             = g_resetposteffects ? nullptr : &ef_hueshift,
        .pNightVision          = g_resetposteffects ? nullptr : &ef_nightvision,
        .pDistortedSides       = g_resetposteffects ? nullptr : &ef_distortedsides,
        .pColorTint            = g_resetposteffects ? nullptr : &ef_tint,
        .pCRT                  = &ef_crt,
        .pVHS                  = &ef_vhs,
        .pDither               = &ef_dither,
    };

    // DLSS-RR: flush temporal history this frame if any transient-light source
    // flagged an abrupt cut (flashlight on/off, a dynlight appearing/
    // disappearing, or a fresh level load -- see g_rt_lightcut's setters) or a
    // diagnostic cvar asked for it. Rate-limited so rapid triggers (e.g. quick
    // flashlight double-tap) don't chain resets back-to-back.
    bool wantResetHistory = bool{ cvar::rt_rr_reset_hold };

    // rt_rr_reset_debug tallies: how many flushes actually reached NGX this
    // second, and how many the rate limit swallowed. A trigger that over-fires
    // shows up as a fired count pinned at ~1000/rt_rr_reset_min_ms per second
    // with a large suppressed count behind it.
    static uint32_t s_rrFired      = 0;
    static uint32_t s_rrSuppressed = 0;
    static double   s_rrTallyAt    = 0.0;

    if( g_rt_lightcut )
    {
        g_rt_lightcut = false;
        if( curtime - g_rt_lastresetat >= double( cvar::rt_rr_reset_min_ms ) / 1000.0 )
        {
            wantResetHistory = true;
            g_rt_lastresetat = curtime;

            if( cvar::rt_rr_reset_debug )
            {
                ++s_rrFired;
                Printf( "rt_rr_reset: FLUSH (cause: %s)\n", g_rt_lightcut_why );
            }
        }
        else if( cvar::rt_rr_reset_debug )
        {
            ++s_rrSuppressed;
        }
    }

    if( cvar::rt_rr_reset_debug )
    {
        if( curtime - s_rrTallyAt >= 1.0 )
        {
            if( s_rrFired || s_rrSuppressed )
            {
                Printf( "rt_rr_reset: last second — %u flush(es), %u suppressed by "
                        "rt_rr_reset_min_ms\n",
                        s_rrFired,
                        s_rrSuppressed );
            }
            s_rrFired      = 0;
            s_rrSuppressed = 0;
            s_rrTallyAt    = curtime;
        }
    }

    if( bool{ cvar::rt_rr_reset_now } )
    {
        cvar::rt_rr_reset_now = false;
        wantResetHistory      = true;
        g_rt_lastresetat      = curtime;
    }

    auto info = RgDrawFrameInfo{
        .sType            = RG_STRUCTURE_TYPE_DRAW_FRAME_INFO,
        .pNext            = &post_params,
        .rayLength        = GetZFar() * ONEGAMEUNIT_IN_METERS,
        .presentPrevFrame = false,
        .resetHistory     = static_cast< RgBool32 >( wantResetHistory ),
        .currentTime      = curtime,
    };

    RgResult r = rt.rgDrawFrame( &info );
    RG_CHECK( r );

    if( g_cpu_latency_get )
    {
        g_cpu_latency = CalcCpuLatency();
    }

    // reset for next frame
    {
        m_wassky           = false;
        g_resetposteffects = false;
    }
}

//
//
//

bool RTRenderState::IsPerspectiveMatrix( const float* m )
{
    return std::abs( m[ 15 ] ) < std::numeric_limits< float >::epsilon();
}

bool RTRenderState::IsLikeIdentity( const float* m )
{
    auto areSimilar = []( float a, float b ) {
        return std::abs( a - b ) < 0.0000001f;
    };
    for( int a = 0; a < 4; a++ )
    {
        for( int b = 0; b < 4; b++ )
        {
            if( !areSimilar( m[ a * 4 + b ], ( a == b ? 1.0f : 0.0f ) ) )
            {
                return false;
            }
        }
    }
    return true;
}
bool RTRenderState::IsLikeIdentity( const double* m )
{
    auto areSimilar = []( double a, double b ) {
        return std::abs( a - b ) < 0.0000001;
    };
    for( int a = 0; a < 4; a++ )
    {
        for( int b = 0; b < 4; b++ )
        {
            if( !areSimilar( m[ a * 4 + b ], ( a == b ? 1.0 : 0.0 ) ) )
            {
                return false;
            }
        }
    }
    return true;
}

auto RT_MakeUpRightForwardVectors( const DRotator& rotation ) -> std::tuple< RgFloat3D, RgFloat3D, RgFloat3D >
{
    // based on HWDrawInfo::SetViewMatrix
    RgFloat3D up, right, forward;

    auto pitch = rotation.Pitch;
    // RT: invert yaw
    auto yaw  = FAngle::fromDeg( -( 270.0 - rotation.Yaw.Degrees() ) );
    auto roll = rotation.Roll;

    auto view = VSMatrix{ 1 };
    view.rotate( float( yaw.Degrees() ), 0, 0, 1 );   // around up
    view.rotate( float( pitch.Degrees() ), 1, 0, 0 ); // around right
    view.rotate( float( roll.Degrees() ), 0, 1, 0 );  // around forward
    const float* v = view.get();

    auto v100 = RgFloat3D{ -v[ 0 ], -v[ 1 ], -v[ 2 ] };
    auto v010 = RgFloat3D{ -v[ 4 ], -v[ 5 ], -v[ 6 ] };
    auto v001 = RgFloat3D{ v[ 8 ], v[ 9 ], v[ 10 ] };

    up      = v001;
    right   = v100;
    forward = v010;

    return { up, right, forward };
}

void RT_ForceCamera( const FVector3 position, const DRotator& rotation, float fovYDegrees )
{
    if( !rt.rgUploadCamera )
    {
        return;
    }

    const auto [ up, right, forward ] = RT_MakeUpRightForwardVectors( rotation );

    const float aspect = screen && screen->GetWidth() > 0 && screen->GetHeight() > 0
                             ? float( screen->GetWidth() ) / float( screen->GetHeight() )
                             : ( 16.f / 9.f );

    auto info = RgCameraInfo{
        .sType       = RG_STRUCTURE_TYPE_CAMERA_INFO,
        .pNext       = nullptr,
        .flags       = 0,
        .position    = { position[ 0 ], position[ 1 ], position[ 2 ] },
        .up          = up,
        .right       = right,
        .fovYRadians = fovYDegrees * pi::pif() / 180.0f,
        .aspect      = aspect,
        .cameraNear  = cvar::rt_znear,
        .cameraFar   = cvar::rt_zfar,
    };

    RgResult r = rt.rgUploadCamera( &info );
    assert( r == RG_RESULT_SUCCESS );
}

// A hack to access special+tag by a linenum
extern std::vector< std::pair< int, int > > rt_linesToSpecialAndTag;

extern auto RT_GetStairsSectors( int tag, line_t* line ) -> std::vector< int >;

namespace
{

std::unordered_set< int > g_tagsSafeToIgnore{};
std::unordered_set< int > g_stairsSectors{};

void RT_CacheTagsAndSpecials()
{
    if( !primaryLevel )
    {
        g_tagsSafeToIgnore.clear();
        g_stairsSectors.clear();
    }

    assert( rt_linesToSpecialAndTag.size() == primaryLevel->lines.size() );

    // 1 tag can be referenced by N specials
    // this is the mapping from tag to its list of specials
    std::unordered_map< int, std::unordered_set< int > > tagToSpecial{};
    for( const auto& [ special, tag ] : rt_linesToSpecialAndTag )
    {
        // tag < 0 -- ignored
        // tag = 0 -- has different behavior
        if( tag > 0 )
        {
            tagToSpecial[ tag ].emplace( special );
        }
    }

    // specials that do not move the geometry, so we can export it
    auto l_isSafeToIgnoreSpecial = []( int spec ) {
        switch( spec )
        {
            case Teleport:
            case Teleport_NoStop:
            case Teleport_NoFog:
            case Light_RaiseByValue:
            case Light_LowerByValue:
            case Light_ChangeToValue:
            case Light_Stop:
            case Light_MinNeighbor:
            case Light_MaxNeighbor:
            case Light_StrobeDoom: return true;
            default: return false;
        }
    };

    // make a list 
    std::unordered_set< int > tagsSafeToIgnore{};
    for( const auto& [ tag, specials ] : tagToSpecial )
    {
        // if no specials on a tag, it's safe
        if( specials.empty() )
        {
            assert( !tagsSafeToIgnore.contains( tag ) );
            tagsSafeToIgnore.emplace( tag );
            continue;
        }

        // if only one special on this tag
        if( specials.size() == 1 )
        {
            // and it's a safe special
            int spec = *specials.begin();
            if( l_isSafeToIgnoreSpecial( spec ) )
            {
                assert( !tagsSafeToIgnore.contains( tag ) );
                tagsSafeToIgnore.emplace( tag );
                continue;
            }
        }

        // surely, we can expand to specials.size() >= 2 (e.g. 1 tag is used for Teleport and Light_Stop => we can ignore),
        // but let's play safely for now..
    }

    g_tagsSafeToIgnore = std::move( tagsSafeToIgnore );


    assert( g_stairsSectors.empty() );
    for( uint32_t i = 0; i < rt_linesToSpecialAndTag.size(); i++ )
    {
        const auto& [ special, tag ] = rt_linesToSpecialAndTag[ i ];

        const auto sectornums = RT_GetStairsSectors( tag, &primaryLevel->lines[ i ] );
        g_stairsSectors.insert( sectornums.begin(), sectornums.end() );
    }
}


// NOTE: only linedef->special, and not sector->special, as it has only light change effects,
// sector that move has tag or one of its lines marked as lift/door/etc (linedef->special)


// If some line specials have tag==0,
// then line's backsector is a target of the special's action
bool IsTaggedByTag0( const line_t* linedef, const sector_t* target )
{
    if( !linedef || !primaryLevel )
    {
        return false;
    }

    // only backsectors
    if( linedef->backsector != target )
    {
        return false;
    }

    // tag == 0
    if( !primaryLevel->tagManager.RT_LineHasZeroTag( linedef ) )
    {
        return false;
    }

    switch( linedef->special )
    {
        // case ACS_Execute:
        // case ACS_ExecuteAlways:
        // case ACS_ExecuteWithResult:
        // case ACS_LockedExecute:
        // case ACS_LockedExecuteDoor:
        // case ACS_Suspend:
        // case ACS_Terminate:
        // case Autosave:
        case Ceiling_CrushAndRaise:
        case Ceiling_CrushAndRaiseA:
        case Ceiling_CrushAndRaiseDist:
        case Ceiling_CrushAndRaiseSilentA:
        case Ceiling_CrushAndRaiseSilentDist:
        case Ceiling_CrushRaiseAndStay:
        case Ceiling_CrushRaiseAndStayA:
        case Ceiling_CrushRaiseAndStaySilA:
        case Ceiling_CrushStop:
        case Ceiling_LowerAndCrush:
        case Ceiling_LowerAndCrushDist:
        case Ceiling_LowerByTexture:
        case Ceiling_LowerByValue:
        case Ceiling_LowerByValueTimes8:
        case Ceiling_LowerInstant:
        case Ceiling_LowerToFloor:
        case Ceiling_LowerToHighestFloor:
        case Ceiling_LowerToLowest:
        case Ceiling_LowerToNearest:
        case Ceiling_MoveToValue:
        case Ceiling_MoveToValueAndCrush:
        case Ceiling_MoveToValueTimes8:
        case Ceiling_RaiseByTexture:
        case Ceiling_RaiseByValue:
        case Ceiling_RaiseByValueTimes8:
        case Ceiling_RaiseInstant:
        case Ceiling_RaiseToHighest:
        case Ceiling_RaiseToHighestFloor:
        case Ceiling_RaiseToLowest:
        case Ceiling_RaiseToNearest:
        case Ceiling_Stop:
        case Ceiling_ToFloorInstant:
        case Ceiling_ToHighestInstant:
        case Ceiling_Waggle:
        // case ChangeCamera:
        // case ChangeSkill:
        // case ClearForceField:
        // case DamageThing:
        case Door_Animated:
        case Door_AnimatedClose:
        case Door_Close:
        case Door_CloseWaitOpen:
        case Door_LockedRaise:
        case Door_Open:
        case Door_Raise:
        case Door_WaitClose:
        case Door_WaitRaise:
        case Elevator_LowerToNearest:
        case Elevator_MoveToFloor:
        case Elevator_RaiseToNearest:
        // case Exit_Normal:
        // case Exit_Secret:
        // case ExtraFloor_LightOnly:
        case Floor_CrushStop:
        case Floor_Donut:
        case Floor_LowerByTexture:
        case Floor_LowerByValue:
        case Floor_LowerByValueTimes8:
        case Floor_LowerInstant:
        case Floor_LowerToHighest:
        case Floor_LowerToHighestEE:
        case Floor_LowerToLowest:
        case Floor_LowerToLowestCeiling:
        case Floor_LowerToLowestTxTy:
        case Floor_LowerToNearest:
        case Floor_MoveToValue:
        case Floor_MoveToValueAndCrush:
        case Floor_MoveToValueTimes8:
        case Floor_RaiseAndCrush:
        case Floor_RaiseAndCrushDoom:
        case Floor_RaiseByTexture:
        case Floor_RaiseByValue:
        case Floor_RaiseByValueTimes8:
        case Floor_RaiseByValueTxTy:
        case Floor_RaiseInstant:
        case Floor_RaiseToCeiling:
        case Floor_RaiseToHighest:
        case Floor_RaiseToLowest:
        case Floor_RaiseToLowestCeiling:
        case Floor_RaiseToNearest:
        case Floor_Stop:
        case Floor_ToCeilingInstant:
        case Floor_TransferNumeric:
        case Floor_TransferTrigger:
        case Floor_Waggle:
        case FloorAndCeiling_LowerByValue:
        case FloorAndCeiling_LowerRaise:
        case FloorAndCeiling_RaiseByValue:
        // case ForceField:
        // case FS_Execute:
        case Generic_Ceiling:
        case Generic_Crusher:
        case Generic_Crusher2:
        case Generic_Door:
        case Generic_Floor:
        case Generic_Lift:
        case Generic_Stairs:
        // case GlassBreak:
        // case HealThing:
        // case Light_ChangeToValue:
        // case Light_Fade:
        // case Light_Flicker:
        // case Light_ForceLightning:
        // case Light_Glow:
        // case Light_LowerByValue:
        // case Light_MaxNeighbor:
        // case Light_MinNeighbor:
        // case Light_RaiseByValue:
        // case Light_Stop:
        // case Light_Strobe:
        // case Light_StrobeDoom:
        // case Line_AlignCeiling:
        // case Line_AlignFloor:
        // case Line_Horizon:
        // case Line_Mirror:
        // case Line_QuickPortal:
        // case Line_SetAutomapFlags:
        // case Line_SetAutomapStyle:
        // case Line_SetBlocking:
        // case Line_SetHealth:
        // case Line_SetIdentification:
        // case Line_SetPortal:
        // case Line_SetPortalTarget:
        // case Line_SetTextureOffset:
        // case Line_SetTextureScale:
        // case NoiseAlert:
        case Pillar_Build:
        case Pillar_BuildAndCrush:
        case Pillar_Open:
        // case Plane_Align:
        // case Plane_Copy:
        case Plat_DownByValue:
        case Plat_DownWaitUpStay:
        case Plat_DownWaitUpStayLip:
        case Plat_PerpetualRaise:
        case Plat_PerpetualRaiseLip:
        case Plat_RaiseAndStayTx0:
        case Plat_Stop:
        case Plat_ToggleCeiling:
        case Plat_UpByValue:
        case Plat_UpByValueStayTx:
        case Plat_UpNearestWaitDownStay:
        case Plat_UpWaitDownStay:
        // case PointPush_SetForce:
        // case Polyobj_DoorSlide:
        // case Polyobj_DoorSwing:
        // case Polyobj_ExplicitLine:
        // case Polyobj_Move:
        // case Polyobj_MoveTimes8:
        // case Polyobj_MoveTo:
        // case Polyobj_MoveToSpot:
        // case Polyobj_OR_Move:
        // case Polyobj_OR_MoveTimes8:
        // case Polyobj_OR_MoveTo:
        // case Polyobj_OR_MoveToSpot:
        // case Polyobj_OR_RotateLeft:
        // case Polyobj_OR_RotateRight:
        // case Polyobj_RotateLeft:
        // case Polyobj_RotateRight:
        // case Polyobj_StartLine:
        // case Polyobj_Stop:
        // case Polyobj_StopSound:
        // case Radius_Quake:
        // case Scroll_Ceiling:
        // case Scroll_Floor:
        // case Scroll_Texture_Both:
        // case Scroll_Texture_Down:
        // case Scroll_Texture_Left:
        // case Scroll_Texture_Model:
        // case Scroll_Texture_Offsets:
        // case Scroll_Texture_Right:
        // case Scroll_Texture_Up:
        // case Scroll_Wall:
        // case Sector_Attach3dMidtex:
        // case Sector_ChangeFlags:
        // case Sector_ChangeSound:
        // case Sector_CopyScroller:
        // case Sector_Set3DFloor:
        // case Sector_SetCeilingGlow:
        // case Sector_SetCeilingPanning:
        // case Sector_SetCeilingScale:
        // case Sector_SetCeilingScale2:
        // case Sector_SetColor:
        // case Sector_SetContents:
        // case Sector_SetCurrent:
        // case Sector_SetDamage:
        // case Sector_SetFade:
        // case Sector_SetFloorGlow:
        // case Sector_SetFloorPanning:
        // case Sector_SetFloorScale:
        // case Sector_SetFloorScale2:
        // case Sector_SetFriction:
        // case Sector_SetGravity:
        // case Sector_SetHealth:
        // case Sector_SetLink:
        // case Sector_SetPlaneReflection:
        // case Sector_SetPortal:
        // case Sector_SetRotation:
        // case Sector_SetTranslucent:
        // case Sector_SetWind:
        // case SendToCommunicator:
        // case SetGlobalFogParameter:
        // case SetPlayerProperty:
        case Stairs_BuildDown:
        case Stairs_BuildDownDoom:
        case Stairs_BuildDownDoomSync:
        case Stairs_BuildDownSync:
        case Stairs_BuildUp:
        case Stairs_BuildUpDoom:
        case Stairs_BuildUpDoomCrush:
        case Stairs_BuildUpDoomSync:
        case Stairs_BuildUpSync:
        // case StartConversation:
        // case Static_Init:
        // case Teleport:
        // case Teleport_EndGame:
        // case Teleport_Line:
        // case Teleport_NewMap:
        // case Teleport_NoFog:
        // case Teleport_NoStop:
        // case Teleport_ZombieChanger:
        // case TeleportGroup:
        // case TeleportInSector:
        // case TeleportOther:
        // case Thing_Activate:
        // case Thing_ChangeTID:
        // case Thing_Damage:
        // case Thing_Deactivate:
        // case Thing_Destroy:
        // case Thing_Hate:
        // case Thing_Move:
        // case Thing_Projectile:
        // case Thing_ProjectileAimed:
        // case Thing_ProjectileGravity:
        // case Thing_ProjectileIntercept:
        // case Thing_Raise:
        // case Thing_Remove:
        // case Thing_SetConversation:
        // case Thing_SetGoal:
        // case Thing_SetSpecial:
        // case Thing_SetTranslation:
        // case Thing_Spawn:
        // case Thing_SpawnFacing:
        // case Thing_SpawnNoFog:
        // case Thing_Stop:
        // case ThrustThing:
        // case ThrustThingZ:
        case Transfer_CeilingLight:
        case Transfer_FloorLight:
        case Transfer_Heights:
        case Transfer_WallLight:
            // case TranslucentLine:
            // case UsePuzzleItem:
            return true;
        default: return false;
    }
}

bool RT_IsSectorMovable( const sector_t* sector )
{
    if( !sector )
    {
        return false;
    }

    auto isTaggedExplicitly = []( const sector_t& s ) {
        if( !primaryLevel )
        {
            return false;
        }

        if( g_stairsSectors.contains( s.Index() ) )
        {
            return true;
        }

        auto l_safeToIgnoreTag = [ & ]( int tag ) {
            return g_tagsSafeToIgnore.contains( tag );
        };

        // if there's at least one NON-safe tag on this sector, it's tagged
        const auto sectorTags = primaryLevel->tagManager.RT_GetAllSectorTags( &s );
        return !std::ranges::all_of( sectorTags, l_safeToIgnoreTag );
    };

    auto isTaggedImplicitly = []( const sector_t& s ) {
        for( const line_t* l : s.Lines )
        {
            if( IsTaggedByTag0( l, &s ) )
            {
                return true;
            }
        }
        return false;
    };

    return isTaggedExplicitly( *sector ) || isTaggedImplicitly( *sector );
}

bool RT_IsTexAnimated( int texnum, const std::vector< bool >& animatedTexnums )
{
    if( texnum < 0 || static_cast< uint32_t >( texnum ) >= animatedTexnums.size() )
    {
        assert( 0 );
        return false;
    }
    return animatedTexnums[ texnum ];
}

bool RT_IsSectorExportable( const sector_t*            sector,
                            bool                       ceiling,
                            const std::vector< bool >& animatedTexnums )
{
    if( !sector )
    {
        assert( 0 );
        return false;
    }

    // e.g. nukage, lava
    bool isAnimated = RT_IsTexAnimated(
        sector->GetTexture( ceiling ? sector_t::ceiling : sector_t::floor ).GetIndex(),
        animatedTexnums );

    return !isAnimated && !RT_IsSectorMovable( sector );
}

bool RT_IsWallExportable( const seg_t* seg, const std::vector< bool >& animatedTexnums )
{
    if( !seg )
    {
        assert( 0 );
        return false;
    }

    // e.g. switches
    auto isAnimated = [ &animatedTexnums ]( const side_t* side ) {
        if( side )
        {
            return RT_IsTexAnimated( side->GetTexture( 0 ).GetIndex(), animatedTexnums ) ||
                   RT_IsTexAnimated( side->GetTexture( 1 ).GetIndex(), animatedTexnums ) ||
                   RT_IsTexAnimated( side->GetTexture( 2 ).GetIndex(), animatedTexnums );
        }
        return false;
    };

    auto isAdjacentSectorMovable = []( const seg_t& s ) {
        if( s.linedef )
        {
            return RT_IsSectorMovable( s.linedef->backsector ) ||
                   RT_IsSectorMovable( s.linedef->frontsector );
        }
        return true;
    };

    return !isAnimated( seg->sidedef ) && !isAdjacentSectorMovable( *seg );
}

enum
{
    RT_WALL_PEGGED_TOP    = 1,
    RT_WALL_PEGGED_BOTTOM = 2,
};

// Pegged texture moves with a Sector that moves
uint8_t RT_WallPeggedFlags( const seg_t* seg )
{
    if( !seg || !seg->linedef )
    {
        return false;
    }

    // if double sided
    if( seg->backsector )
    {
        int fs = RT_WALL_PEGGED_TOP | RT_WALL_PEGGED_BOTTOM;

        if( seg->linedef->flags & ML_DONTPEGTOP )
        {
            fs = ( fs & ~( RT_WALL_PEGGED_TOP ) );
        }

        if( seg->linedef->flags & ML_DONTPEGBOTTOM )
        {
            fs = ( fs & ~( RT_WALL_PEGGED_BOTTOM ) );
        }
        
        return uint8_t( fs );
    }
    else
    {
        // one sided always pegged
        return RT_WALL_PEGGED_TOP | RT_WALL_PEGGED_BOTTOM;
    }
}

auto rt_sectorCeilingExportable = std::vector< bool >{};
auto rt_sectorFloorExportable   = std::vector< bool >{};
auto rt_wallExportable          = std::vector< bool >{};
auto rt_wallPegged              = std::vector< uint8_t >{};

} // anonymous namespace

void RT_BakeExportables( const std::vector< bool >& animatedTexnums )
{
    rt_sectorCeilingExportable.clear();
    rt_sectorFloorExportable.clear();
    rt_wallExportable.clear();
    rt_wallPegged.clear();
    g_tagsSafeToIgnore.clear();
    g_stairsSectors.clear();

    if( !primaryLevel )
    {
        return;
    }

    RT_CacheTagsAndSpecials();

    rt_sectorCeilingExportable.resize( primaryLevel->sectors.Size(), false );
    rt_sectorFloorExportable.resize( primaryLevel->sectors.Size(), false );
    for( uint32_t i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        rt_sectorCeilingExportable[ i ] =
            RT_IsSectorExportable( &primaryLevel->sectors[ i ], true, animatedTexnums );
        rt_sectorFloorExportable[ i ] =
            RT_IsSectorExportable( &primaryLevel->sectors[ i ], false, animatedTexnums );
    }

    rt_wallExportable.resize( primaryLevel->segs.Size(), false );
    for( uint32_t i = 0; i < primaryLevel->segs.Size(); i++ )
    {
        rt_wallExportable[ i ] = RT_IsWallExportable( &primaryLevel->segs[ i ], animatedTexnums );
    }

    rt_wallPegged.resize( primaryLevel->segs.Size(), false );
    for( uint32_t i = 0; i < primaryLevel->segs.Size(); i++ )
    {
        rt_wallPegged[ i ] = RT_WallPeggedFlags( &primaryLevel->segs[ i ] );
    }
}

bool RT_IsSectorExportable2( int sectornum, bool ceiling )
{
    if( sectornum >= 0 )
    {
        const auto& arr = ceiling ? rt_sectorCeilingExportable : rt_sectorFloorExportable;

        if( sectornum < int( arr.size() ) )
        {
            return arr[ sectornum ];
        }
    }
    return false;
}

bool RT_IsSectorExportable( const sector_t* sector, bool ceiling )
{
    if( sector )
    {
        return RT_IsSectorExportable2( sector->sectornum, ceiling );
    }
    return false;
}

bool RT_IsWallExportable( const seg_t* seg )
{
    if( seg && seg->segnum >= 0 )
    {
        const auto segnum = static_cast< uint32_t >( seg->segnum );

        if( segnum < rt_wallExportable.size() )
        {
            return rt_wallExportable[ segnum ];
        }
    }
    return false;
}

bool RT_IsWallNoMotionVectors( const seg_t* seg, side_t::ETexpart part )
{
    if( part == side_t::top || part == side_t::bottom )
    {
        if( seg && seg->segnum >= 0 && uint32_t( seg->segnum ) < rt_wallPegged.size() )
        {
            if( part == side_t::top )
            {
                // inverse logic, as top grows from bottom to up
                return !( ( rt_wallPegged[ seg->segnum ] ) & RT_WALL_PEGGED_TOP );
            }
            else
            {
                return ( rt_wallPegged[ seg->segnum ] ) & RT_WALL_PEGGED_BOTTOM;
            }
        }
    }
    return true;
}


void RT_SpawnFluid( int             count,
                    const FVector3& position,
                    const FVector3& velocity,
                    float           dispersionDegrees )
{
    if( count <= 0 || !cvar::rt_fluid_available || !cvar::rt_fluid )
    {
        return;
    }
    count = std::min( count, 10000 );

    if( rt.rgSpawnFluid )
    {
        auto info = RgSpawnFluidInfo{
            .sType                  = RG_STRUCTURE_TYPE_SPAWN_FLUID_INFO,
            .pNext                  = nullptr,
            .position               = { float( position.X ) * ONEGAMEUNIT_IN_METERS,
                                        float( position.Y ) * ONEGAMEUNIT_IN_METERS,
                                        float( position.Z ) * ONEGAMEUNIT_IN_METERS },
            .radius                 = 0.05f,
            .velocity               = { float( velocity.X ) * ONEGAMEUNIT_IN_METERS,
                                        float( velocity.Y ) * ONEGAMEUNIT_IN_METERS,
                                        float( velocity.Z ) * ONEGAMEUNIT_IN_METERS },
            .dispersionVelocity     = 0.9f,
            .dispersionAngleDegrees = dispersionDegrees,
            .count                  = uint32_t( count ),
        };

        RgResult r = rt.rgSpawnFluid( &info );
        RG_CHECK( r );
    }
}

void RT_RegisterFullscreenImage( const char* texture )
{
    if( !texture || texture[ 0 ] == '\0' )
    {
        return;
    }

    constexpr uint8_t empty[] = { 0, 0, 0, 0 };

    auto info = RgOriginalTextureInfo{
        .sType        = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO,
        .pNext        = nullptr,
        .pTextureName = texture,
        .pPixels      = empty,
        .size         = { 1, 1 },
        .filter       = RG_SAMPLER_FILTER_LINEAR,
        .addressModeU = RG_SAMPLER_ADDRESS_MODE_CLAMP,
        .addressModeV = RG_SAMPLER_ADDRESS_MODE_CLAMP,
    };

    RgResult r = rt.rgProvideOriginalTexture( &info );
    RG_CHECK( r );
}

void RT_DeleteFullscreenImage( const char* texture )
{
    if( !texture || texture[ 0 ] == '\0' )
    {
        return;
    }

    RgResult r = rt.rgMarkOriginalTextureAsDeleted( texture );
    RG_CHECK( r );
}

void RT_DrawFullscreenImage( const char* texture,
                             float       opacity,
                             FVector4    background_color,
                             FVector4    foreground_color,
                             float       splitef = 0,
                             float       scale   = 1 )
{
    // samplers are hardcoded to 'repeat' in the wrapper + primitive.color is ignored
    // so don't play anything :(
    if( g_isremix )
    {
        return;
    }

    if( !texture || texture[ 0 ] == '\0' )
    {
        return;
    }

    if( opacity < 0.001f )
    {
        return;
    }

    static constexpr uint32_t indices[] = { 0, 1, 2, 2, 3, 0 };

    static constexpr RgPrimitiveVertex verts_fullscreen[] = {
        { .position = { -1, +1, 0 }, .texCoord = { 0, 1 }, .color = 0xFFFFFFFF },
        { .position = { -1, -1, 0 }, .texCoord = { 0, 0 }, .color = 0xFFFFFFFF },
        { .position = { +1, -1, 0 }, .texCoord = { 1, 0 }, .color = 0xFFFFFFFF },
        { .position = { +1, +1, 0 }, .texCoord = { 1, 1 }, .color = 0xFFFFFFFF },
    };

    RgPrimitiveVertex verts_16by9[] = {
        verts_fullscreen[ 0 ],
        verts_fullscreen[ 1 ],
        verts_fullscreen[ 2 ],
        verts_fullscreen[ 3 ],
    };

    {
        const RgExtent2D wnd = RT_GetCurrentWindowSize();

        float xwin = ( float )wnd.width / ( float )wnd.height;
        float ximg = 16.0f / 9.0f;

        float tx, ty;
        if( ximg < xwin )
        {
            tx = ximg / xwin;
            ty = 1.0f;
        }
        else
        {
            tx = 1.0f;
            ty = xwin / ximg;
        }

#define VectorSet2( ptr, x, y ) \
    ( ptr )[ 0 ] = ( x );      \
    ( ptr )[ 1 ] = ( y )

        tx = ( 1 - 1 / tx ) / 2;
        ty = ( 1 - 1 / ty ) / 2;

        VectorSet2( verts_16by9[ 0 ].texCoord, tx, 1 - ty );
        VectorSet2( verts_16by9[ 1 ].texCoord, tx, ty );
        VectorSet2( verts_16by9[ 2 ].texCoord, 1 - tx, ty );
        VectorSet2( verts_16by9[ 3 ].texCoord, 1 - tx, 1 - ty );
    }

    // scale
    {
        for( RgPrimitiveVertex& v : verts_16by9 )
        {
            v.texCoord[ 0 ] = ( ( v.texCoord[ 0 ] - 0.5f ) / scale ) + 0.5f;
            v.texCoord[ 1 ] = ( ( v.texCoord[ 1 ] - 0.5f ) / scale ) + 0.5f;
        }
    }

    constexpr static float viewproj[ 16 ] = {
        1, 0, 0, 0, //
        0, 1, 0, 0, //
        0, 0, 1, 0, //
        0, 0, 0, 1, //
    };

    auto l_drawcolor = []( const RgPrimitiveVertex( &verts )[ 4 ],
                           RgColor4DPacked32        color ) {
        auto ui = RgMeshPrimitiveSwapchainedEXT{
            .sType           = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_SWAPCHAINED_EXT,
            .pNext           = nullptr,
            .flags           = 0,
            .pViewport       = nullptr,
            .pView           = nullptr,
            .pProjection     = nullptr,
            .pViewProjection = viewproj,
        };

        auto prim = RgMeshPrimitiveInfo{
            .sType                = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
            .pNext                = &ui,
            .flags                = RG_MESH_PRIMITIVE_TRANSLUCENT,
            .primitiveIndexInMesh = 0,
            .pVertices            = verts,
            .vertexCount          = uint32_t( std::size( verts ) ),
            .pIndices             = indices,
            .indexCount           = std::size( indices ),
            .pTextureName         = nullptr,
            .textureFrame         = 0,
            .color                = color,
            .emissive             = 0,
            .classicLight         = 1.0f,
        };

        RgResult r = rt.rgUploadMeshPrimitive( nullptr, &prim );
        RG_CHECK( r );
    };

    // back color
    if( background_color.W > 0 )
    {
        l_drawcolor( verts_fullscreen,
                     rt.rgUtilPackColorFloat4D( background_color.X, //
                                                background_color.Y,
                                                background_color.Z,
                                                background_color.W ) );
    }

    if( splitef > 0 )
    {
        RgPrimitiveVertex half[ 4 ];
        static_assert( sizeof( half ) == sizeof( verts_fullscreen ) );

        // left, rises top -> bottom
        {
            memcpy( half, verts_fullscreen, sizeof( verts_fullscreen ) );
            VectorSet2( half[ 0 ].position, -1, +1 );
            VectorSet2( half[ 1 ].position, -1, std::lerp( 1, -1, splitef ) );
            VectorSet2( half[ 2 ].position, 0, std::lerp( 1, -1, splitef ) );
            VectorSet2( half[ 3 ].position, 0, +1 );
            l_drawcolor( half, RG_PACKED_COLOR_WHITE );
        }
        // right, rises bottom -> top
        {
            memcpy( half, verts_fullscreen, sizeof( verts_fullscreen ) );
            VectorSet2( half[ 0 ].position, 0, std::lerp( -1, 1, splitef ) );
            VectorSet2( half[ 1 ].position, 0, -1 );
            VectorSet2( half[ 2 ].position, +1, -1 );
            VectorSet2( half[ 3 ].position, +1, std::lerp( -1, 1, splitef ) );
            l_drawcolor( half, RG_PACKED_COLOR_WHITE );
        }
    }

    // image
    {
        auto ui = RgMeshPrimitiveSwapchainedEXT{
            .sType           = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_SWAPCHAINED_EXT,
            .pNext           = nullptr,
            .flags           = 0,
            .pViewport       = nullptr,
            .pView           = nullptr,
            .pProjection     = nullptr,
            .pViewProjection = viewproj,
        };

        auto prim = RgMeshPrimitiveInfo{
            .sType                = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
            .pNext                = &ui,
            .flags                = RG_MESH_PRIMITIVE_TRANSLUCENT,
            .primitiveIndexInMesh = 0,
            .pVertices            = verts_16by9,
            .vertexCount          = std::size( verts_16by9 ),
            .pIndices             = indices,
            .indexCount           = std::size( indices ),
            .pTextureName         = texture,
            .textureFrame         = 0,
            .color                = rt.rgUtilPackColorFloat4D( 1.0f, 1.0f, 1.0f, opacity ),
            .emissive             = 0,
            .classicLight         = 1.0f,
        };

        RgResult r = rt.rgUploadMeshPrimitive( nullptr, &prim );
        RG_CHECK( r );
    }

    // foreground color
    if( foreground_color.W > 0 )
    {
        l_drawcolor( verts_fullscreen,
                     rt.rgUtilPackColorFloat4D( foreground_color.X, //
                                                foreground_color.Y,
                                                foreground_color.Z,
                                                foreground_color.W ) );
    }

    #undef VectorSet2
}

extern FSoundID T_FindSound( const char* name );

static int         g_title_begintick{ -1 };
static int         g_title_endtick{ -1 };
static int         g_title_fadeouttics{ 0 };
static std::string g_title_requested{};
static std::string g_title_uploaded{};
static bool        g_title_soundplayed{ false };

void RT_StartTitleImage( const char* imagepath,
                         int         begin_maptime,
                         int         end_maptime,
                         int         fadeout_tics )
{
    // samplers are hardcoded to 'repeat' in the wrapper + primitive.color is ignored
    // so don't play anything :(
    if( g_isremix )
    {
        return;
    }

    if( !imagepath || imagepath[ 0 ] == '\0' )
    {
        g_title_requested.clear();
        g_title_endtick     = -1;
        g_title_begintick   = -1;
        g_title_fadeouttics = 0;
        g_title_soundplayed = false;
        return;
    }

    g_title_requested   = imagepath;
    g_title_begintick   = begin_maptime;
    g_title_endtick     = end_maptime;
    g_title_fadeouttics = fadeout_tics;
    g_title_soundplayed = false;
}

static void RT_DrawTitle()
{
    if( g_title_requested.empty() )
    {
        RT_ClearTitles();
        return;
    }

    if( level.sectors.Size() <= 0 )
    {
        RT_ClearTitles();
        return;
    }

    if( level.maptime >= g_title_endtick )
    {
        RT_ClearTitles();
        return;
    }

    // upload texture
    if( g_title_uploaded != g_title_requested )
    {
        if( !g_title_uploaded.empty() )
        {
            RT_DeleteFullscreenImage( g_title_uploaded.c_str() );
        }

        RT_RegisterFullscreenImage( g_title_requested.c_str() );
        g_title_uploaded = g_title_requested;
    }

    if( g_title_begintick > 0 )
    {
        if( level.maptime < g_title_begintick )
        {
            return;
        }
    }

    float alpha = 1.0f;
    if( g_title_fadeouttics > 0 )
    {
        int ticksleft = g_title_endtick - level.maptime;
        if( ticksleft < g_title_fadeouttics )
        {
            alpha = float( ticksleft ) / float( g_title_fadeouttics );

            // gamma
            alpha = alpha * alpha;
        }
    }

    RT_DrawFullscreenImage( g_title_uploaded.c_str(), //
                            alpha,
                            { 0, 0, 0, alpha * 0.3f },
                            { 0, 0, 0, 0 } );
    
    if( !g_title_soundplayed )
    {
        g_title_soundplayed = true;

        if( soundEngine )
        {
            // HACKHACK
            if( g_title_uploaded == "title/iconofsin" )
            {
                return;
            }

            FSoundID sound = T_FindSound( "sounds/cutscene/boom.ogg" );
            soundEngine->StartSound(
                SOURCE_None, nullptr, nullptr, CHAN_AUTO, CHANF_UI, sound, 1.0f, ATTN_NONE );
        }
    }
}

static void RT_ClearTitles()
{
    if( !g_title_uploaded.empty() )
    {
        RT_DeleteFullscreenImage( g_title_uploaded.c_str() );
    }
    g_title_requested.clear();
    g_title_uploaded.clear();
    g_title_begintick   = -1;
    g_title_endtick     = -1;
    g_title_fadeouttics = 0;
    g_title_soundplayed = false;
}

extern bool rt_isdoom2;

static void RT_InjectTitleIntoDoomMap( const char* mapname )
{
    if( !rt_isdoom2 )
    {
        return;
    }
    
    if( !mapname || mapname[ 0 ] == '\0' )
    {
        return;
    }

    const char* titlename = nullptr;
    {
        if( stricmp( mapname, "map12" ) == 0 )
        {
            titlename = "title/ep2";
        }
        else if( stricmp( mapname, "map21" ) == 0 )
        {
            titlename = "title/ep3";
        }
    }

    if( !titlename )
    {
        return;
    }

    constexpr int BEGIN_TICS    = int( 1.5f * TICRATE );
    constexpr int DURATION_TICS = int( 5.0f * TICRATE );
    constexpr int FADEOUT_TICS  = int( 3.0f * TICRATE );

    RT_StartTitleImage( titlename, BEGIN_TICS, BEGIN_TICS + DURATION_TICS, FADEOUT_TICS );
}
