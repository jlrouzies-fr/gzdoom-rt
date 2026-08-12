// The complete list of Doom64-RT cvars. Included TWICE, with the RT_CVAR* macros
// meaning different things each time:
//
//   rt_cvars.h   -- declaring face:  extern ValueToCVarRef<...> name;
//   rt_cvars.cpp -- defining face:   the storage + FCVarDecl registration
//
// One list, so adding a cvar stays a one-line edit and a declaration can never
// drift from its definition. Do NOT put anything here that is not an RT_CVAR*
// invocation or a comment -- both faces have to be able to swallow every line.
//
// Everything below sits inside `namespace cvar`, opened by whichever file
// includes this.

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
    RT_CVAR( rt_dynlight_flicker_scale, 0.25f,  "extra intensity scale applied ONLY to Flicker/RandomFlicker dynlights, i.e. "
                                                "exactly the class rt_dynlight_flicker gates.\n"
                                                "\n"
                                                "Exists because rt_dynlight_intensity is GLOBAL to every FDynamicLight. Using it "
                                                "to bring the 199-strong SMON monitor wall down to a comfortable level dimmed the "
                                                "key-door lights and every other 9800 along with it -- reported in play the same "
                                                "session and rolled straight back. 199 of the game's 205 9802s sit beside a SMON "
                                                "panel, so this knob is very nearly 'the wall monitors' and nothing else; the six "
                                                "that are not are MAP34/MAP08 torches at radius 32-72, all but one of them "
                                                "cap-limited and therefore unmoved by it.\n"
                                                "\n"
                                                "0.25 with rt_dynlight_intensity back at 40 reproduces the settled monitor look "
                                                "-- a 24/20 panel at 133..167, swing 1.25x -- while leaving doors, torches and "
                                                "every steady light exactly where they were (2026-08-12)" )

    RT_CVAR( rt_dynlight_blink_floor,   0.8f,   "dim end of a flicker/pulse dynlight's swing, as a fraction of its bright end. "
                                                "1 = dead steady, 0.15 = the old hardcoded value.\n"
                                                "\n"
                                                "GZDoom expresses a blink as a small change in map RADIUS -- Retribution's wall "
                                                "monitors use 24/20, only ~17%% -- which is invisible in a rasteriser and was "
                                                "therefore remapped here to a hard 0.15..1.0 so it would read as on/off. Once "
                                                "rt_dynlight_flicker was turned on and 199 SMON panels started blinking at once, "
                                                "that 6.7x swing read as strobing rather than as machinery. 0.55 keeps the panels "
                                                "visibly alive at roughly a 1.8x swing.\n"
                                                "\n"
                                                "Only meaningful together with the roll-off change made at the same time: the "
                                                "roll-off used to divide by the CURRENT radius, so raising this floor above ~0.36 "
                                                "inverted the pulse instead of calming it. It now divides by the fixture's nominal "
                                                "radius, so this cvar is monotonic -- raising it always means less flicker "
                                                "(2026-08-12). Arms in tools/ab-smon.cmd" )

    RT_CVAR( rt_dynlight_flicker,       true,   "upload Flicker/RandomFlicker FDynamicLights (9802).\n"
                                                "\n"
                                                "Was false, and that suppressed EVERY SMON WALL MONITOR LIGHT IN THE GAME. The "
                                                "skip below is unconditional, and 9802 PointLightFlicker is exactly how "
                                                "Retribution wires those panels -- a light thing 4-56 units off the face, never "
                                                "texture metadata (an _e mask glows but casts nothing). Measured over the WAD: "
                                                "SMONAA 88/88 wired this way, SMONDA 25/27, SMONCA 19/21, SMONEA 6/6. All "
                                                "dropped before upload, so the monitors animated and lit nothing.\n"
                                                "\n"
                                                "Found from play on MAP29, where exactly ONE of three otherwise identical "
                                                "SMONDA panels lit the room. Lines 922 and 937 carry a 9802 and were skipped; "
                                                "line 927 carries a 9801 PointLightPulse, which is not a flicker type and so "
                                                "was never skipped. One 9801 among two 9802s, one panel lit among three -- the "
                                                "map data predicts the reported symptom exactly.\n"
                                                "\n"
                                                "The old default's stated reason -- that ceiling head lights come from "
                                                "rt_ceiling_lamps instead -- does not cost anything here: 205 of these things "
                                                "exist game-wide and 199 sit beside a SMON panel, so this switch is very nearly "
                                                "'the wall monitors work'. Compare arms with tools/ab-smon.cmd (2026-08-11)" )

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

    RT_CVAR( rt_switch_lights,          true,   "a thrown switch lights the wall it is set into. Retribution's switches change "
                                                "art when used -- SWXC's demon face gains red eyes, SWXSG's plate gains a pink "
                                                "gem -- and gen_world_emissives.py makes those texels GLOW, but an emissive "
                                                "surface is not a light source under RTGL1 (see rt_wall_strips), so the switch "
                                                "lit up and the wall around it stayed black. This places one analytic light on "
                                                "the lit face. Table is GENERATED (tools/gen_switch_lights.py -> "
                                                "rt_switch_lights.h): position is the centroid of the texture's own emissive "
                                                "mask and colour is that mask's hue, so cast light and glow are measured from "
                                                "the same pixels and cannot drift. Covers the bare 32x32 faces AND the 56 "
                                                "CMPSW* wall panels that stamp them. Off = the eyes still glow, they just light "
                                                "nothing -- which is the old behaviour, unlike rt_flame_light_on (2026-08-11)" )
    RT_CVAR( rt_switch_light_intensity, 60.f,   "RT intensity for a switch face of ~7 lit texels (the SWXC eyes). Scaled up for "
                                                "bigger emitters by sqrt(lit/7) so the 46-texel SWXSG gem is brighter without a "
                                                "second table -- sqrt, not linear, or the gem lands 6.5x the eyes and reads as a "
                                                "floodlight. Deliberately modest: this is a detail on a wall, not room lighting, "
                                                "and 95 switch faces exist across 14 maps" )
    RT_CVAR( rt_switch_light_radius,    0.06f,  "RT source radius in meters. Small -- a pair of eyes is a hard little source, "
                                                "and a wide radius here smears the light across the whole plate" )
    RT_CVAR( rt_switch_light_ofs,       2.f,    "map units to push the light off the wall face, along the surface normal. "
                                                "Coplanar emitters self-shadow at grazing angles; this is the same 2.0 nudge "
                                                "RT_UploadWallStripLights uses, and for the same reason" )
    RT_CVAR( rt_switch_light_zofs,      0.f,    "map units added to the computed height, for CALIBRATING texture space against "
                                                "world space. The vertical position depends on the sidedef's pegging flags and "
                                                "row offset, and deriving that from an unverified convention is the mistake "
                                                "recorded at RT_UploadWallStripLights and again at rt_spin_panel_yaw. If every "
                                                "switch light sits a consistent distance above or below its eyes, correct it "
                                                "here and say so; if only SOME are wrong, the pegging branch is wrong and this "
                                                "cvar will not save it" )
    RT_CVAR( rt_switch_light_maxdist,   2048.f, "skip switch lights further than this from the camera. Tighter than the flame "
                                                "cull: a switch is a detail you read up close, not room light you notice "
                                                "arriving" )
    RT_CVAR( rt_switch_light_max,       48,     "nearest-first budget, split from every other family so a corridor of switch "
                                                "panels cannot starve the real fixtures" )
    // NOARCH like every other debug switch here: a marker cvar that persists into the
    // ini turns up weeks later as "why is the map full of cyan dots".
    RT_CVAR_NOARCH( rt_switch_light_debug, false, "cyan markers at each switch light plus a per-60-frame "
                                                "dump of texture, part, pegging and world position. The markers must sit ON the "
                                                "lit eyes; if they float above or below, see rt_switch_light_zofs" )

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
    RT_CVAR( rt_spin_panels,            true,   "orbit a light around the CTEL telemetry panel in step with its own artwork. "
                                                "CTEL1..8 is an ANIMDEFS loop whose eight rim gems carry the SAME luminance "
                                                "set cyclically shifted one frame each -- a chase light running CLOCKWISE, "
                                                "measured at ~45 degrees and ~26.6 texels of radius per step, with a combined "
                                                "output that is dead constant (891) in every frame. So the panel depicts a "
                                                "light that MOVES and does not change brightness.\n"
                                                "\n"
                                                "A map light thing cannot express that: things do not move, and GZDoom offers "
                                                "no per-thing phase offset to fake it with eight of them (PulseLight's phase "
                                                "comes from spawn time, and every thing spawns at map load). Hence an engine "
                                                "walk. The bearing is read from the CURRENT animation frame via "
                                                "TexMan.GetGameTexture(id, true), not from a timer, so the light cannot drift "
                                                "out of step with the art no matter what the frame rate does.\n"
                                                "\n"
                                                "Replaces the two PointLightPulse things this alcove had: those pulsed, and "
                                                "the pulsing was imitating the ACS Light_Glow that turned out to be the actual "
                                                "defect (2026-08-10)" )
    RT_CVAR_COLOR( rt_spin_panel_color, 0xDC0602, "colour of the spinning panel light (hex), used RAW like the faux/solo "
                                                "colours. A deep dark red: the gems' own peak texel is (222,57,24), which is "
                                                "noticeably orange, and the first pass (FF1E0A) kept enough of that to read "
                                                "warm rather than bloody. Green and blue are near zero here and red is off "
                                                "full, which deepens the hue.\n"
                                                "\n"
                                                "RAW means this is a MULTIPLIER, not just a hue: darkening the colour dims "
                                                "the light as well. DC vs FF on the red channel is 0.86x on its own, on top "
                                                "of whatever rt_spin_panel_intensity says -- so the two knobs multiply, and a "
                                                "colour change is never brightness-neutral here" )
    RT_CVAR( rt_spin_panel_intensity,   200.f,  "RT intensity of the spinning light. Halved from the first pass (400) on "
                                                "request; with the 0.86x from the deeper colour the alcove receives about "
                                                "0.43x of what it did, i.e. a bit over the 2x reduction asked for.\n"
                                                "\n"
                                                "Free of the dynlight radius trap the map-thing version hit -- this is "
                                                "uploaded directly, so the number means what it says and does not invert past "
                                                "rt_dynlight_rsoft" )
    RT_CVAR( rt_spin_panel_radius,      0.06f,  "RT source radius in meters. Small, like the solo bulbs: the emitter is one "
                                                "gem, and a wide source smears the travelling highlight into a flat wash" )
    RT_CVAR( rt_spin_panel_orbit,       26.6f,  "orbit radius in MAP UNITS from the sector centre. 26.6 is measured from the "
                                                "artwork (mean centroid distance of the eight rim clusters on a 64x64 tile, "
                                                "which is 1 texel per unit on a 64x64 sector), so the light sits over the gem "
                                                "that is lit rather than near it" )
    RT_CVAR( rt_spin_panel_zofs,        16.f,   "how far the light sits off its panel: above the floor flat, below the "
                                                "ceiling flat. Off the surface rather than on it, or the light is coplanar "
                                                "with the thing it is meant to light" )
    RT_CVAR( rt_spin_panel_yaw,         0.f,    "degrees added to the measured bearing, for calibrating texture space against "
                                                "world space. Flats map texel +v to world -y, which makes the artwork's "
                                                "clockwise a world clockwise -- but that is a convention, and deriving a "
                                                "direction from an unverified convention is exactly the mistake recorded at "
                                                "RT_UploadWallStripLights. If the light leads or lags the lit gem, correct it "
                                                "here; if it runs the wrong way round, use rt_spin_panel_cw" )
    RT_CVAR( rt_spin_panel_cw,          true,   "run the orbit clockwise, matching the artwork. Set 0 if it visibly counter-"
                                                "rotates against the gems -- see rt_spin_panel_yaw" )
    RT_CVAR( rt_spin_panel_max,         64,     "hard cap on spinning panel lights per frame, budgeted apart from every other "
                                                "family for the reason they are all split: a texture that appears on 15 maps "
                                                "must not be able to starve the real fixtures of light slots" )
    RT_CVAR( rt_spin_panel_debug,       false,  "periodic console dump of each spinning light: sector, current frame, bearing "
                                                "and world position" )
    // Answers "what is blinking" without needing to know which texture or sector to
    // look at first, which is the gap rt_tex_probe leaves: that one needs a name, and
    // naming the wrong surface produces silence rather than an answer.
    //
    // Each frame it compares every sector's lightlevel against the previous frame and
    // prints the ones that moved. If a surface visibly pulses and NOTHING prints, the
    // sector is not animating and the cause is a light or the denoiser -- which is a
    // real answer, arrived at in one run instead of a chain of eliminations.
    RT_CVAR_NOARCH( rt_lightlevel_watch, false, "print every sector whose lightlevel changes, as it changes: "
                                                "'sector 126: 200 -> 255'. The direct test for 'what is making "
                                                "this blink'. Silence while something visibly pulses means the "
                                                "sector is innocent" )
    // Deliberately a STRING and deliberately not archived. Set it to a texture name
    // prefix and every world surface drawn with a matching texture reports once a
    // second. Empty (the default, and what it returns to next launch) is off.
    //
    // It exists because a whole day went into "the change is not visible" without
    // being able to answer three basic questions about a single surface: which FILE
    // the texture actually came from (i.e. whether a replacement wad is winning the
    // load order at all), which animation frame is on screen, and whether the engine
    // is making the surface self-emit. Each of those was guessed at repeatedly, and
    // each guess cost a round trip. `rt_tex_probe CTEL` answers all three in one line.
    RT_CVAR_STRING( rt_tex_probe, "", CVAR_GLOBALCONFIG,
           "print a per-second report for every world surface whose texture name starts "
           "with this prefix (case-insensitive): source file, lump, sector lightlevel, "
           "rt_sector_emis contribution, vertex colour. Empty = off" )
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

    // Lava is a LIGHT, and it has to be an analytic one.
    //
    // textures.json has carried lightIntensity 140 + lightColor on all seven lava
    // entries for as long as the lava has existed, and it has never lit anything:
    // that meta only produces a light for SPRITES. A flat's emissive shades the
    // flat and nothing else, so a lava room path-traces as a black box with a
    // glowing net painted on the floor (screen/lavanoemit_needshader.png).
    //
    // Same conclusion as the flames, for the same reason, so the same shape of fix:
    // one analytic light per patch of surface, uploaded per frame, keyed on stable
    // ids. The difference is that a flame is a point and a lava lake is an AREA, so
    // these are scattered on a grid over the sector's own subsectors rather than
    // placed one per actor.
    // The lava SURFACE, as opposed to the lights it casts. Both halves exist
    // because neither can do the other's job: the lights cannot make the lava
    // itself look hot, and the emissive cannot light the room.
    //
    // rt_lava_emis is the answer to "why can the lava not bloom". Primary
    // emission is the raw _e sample, _e is 8-bit so it caps at 1.0, screen
    // emission is that times rt_emis_maxscrcolor (pinned at 3), and
    // rt_bloom_threshold is 16 -- so a lava flat tops out at a fifth of the
    // threshold and no painting can fix it. This multiplies lava surfaces ONLY,
    // which is why the geometry has to carry a LAVA flag: raising
    // rt_emis_maxscrcolor instead would rebalance every emissive in the game.
    RT_CVAR( rt_lava_emis,              1.0f,   "screen-emission multiplier for lava surfaces, on top of "
                                                "rt_emis_maxscrcolor. This is what lets the cracks cross "
                                                "rt_bloom_threshold. DELIBERATELY LOW NOW, and bloom is not the goal "
                                                "here: the tint is a multiply, so at 2 the result is about "
                                                "(2.0, 1.1, 0.6) -- red clips, green and blue do not, and the hue "
                                                "survives as molten rock. Push it to 20 for bloom and all three "
                                                "channels clip to equal values, i.e. WHITE. Brightness and hue are "
                                                "fighting over the same headroom, and hue won. The heat now lives "
                                                "in a volume ABOVE the surface instead -- see d64r-lava-fx.pk3." )
    RT_CVAR( rt_lava_flow,              0.0f,  "how far the drifting heat field swings the emission, 0..1. 0 = a "
                                                "still surface. The flat itself cannot supply this: its 5-frame "
                                                "animation had to be averaged away because the frames light "
                                                "different cracks, which read as blinking rather than flowing. OFF by "
                                                "default: modulating the texture's own emission IS changing the "
                                                "texture, and the heat belongs in the volume above it. Kept "
                                                "because it is the cheaper option if the haze ever has to go." )
    RT_CVAR( rt_lava_flow_speed,        0.03f,  "drift speed of the heat field. Slow on purpose -- this is molten "
                                                "rock crusting over, not a texture scroll." )
    RT_CVAR( rt_lava_flow_scale,        0.12f,  "heat field frequency, UV per METRE. Lower = larger hot and cold "
                                                "regions; around 0.12 a patch is roughly the size of a few crust "
                                                "plates." )
    RT_CVAR( rt_lava_flow_pixel,        0.25f,  "world size in METRES of one cell the field is QUANTIZED to before "
                                                "sampling. 0.25 m is about the flat's own texel; a smooth gradient "
                                                "sliding over 64x64 pixel art reads as a modern shader bolted onto "
                                                "the wrong texture. 0 = smooth." )
    RT_CVAR( rt_lava_pulse,             0.1f,  "depth of a slow whole-surface breath under the drift, 0..1" )
    RT_CVAR( rt_lava_pulse_speed,       0.35f,  "rate of that breath, radians per second" )
    // The lava as an AREA light, which is what a lake actually is.
    //
    // The analytic grid (rt_lava_light_*) works but shows its seams: each point
    // gets its own pool, and walking past one draws a circle of illumination
    // across the wall. A lake does not light a room that way. Indirect emission
    // does -- it is already the mechanism (emissiveMult, then rt_emis_mapboost
    // in RtRaygenIndirect), it was simply never strong enough on this material.
    // Raise this and drop rt_lava_light_on for the area-source version; it is
    // softer and correct, at the cost of being carried by the indirect pass,
    // which is noisier and has no sharp shadow.
    // The heat's hue. The flat's cracks photograph yellow-orange once boosted --
    // partly the art, partly that any bright emission drifts toward white -- and
    // molten rock should read RED. Applied to the screen emission and the GI
    // together, so the room does not bounce a different colour than the surface.
    RT_CVAR( rt_lava_tint_r,          255,      "lava emission tint Red [0,255]" )
    RT_CVAR( rt_lava_tint_g,           60,      "lava emission tint Green [0,255]. This is the one to pull DOWN for "
                                                "a redder, angrier lava; raise it toward 255 for the yellow-orange "
                                                "the raw texture gives." )
    RT_CVAR( rt_lava_tint_b,           76,      "lava emission tint Blue [0,255]" )
    RT_CVAR( rt_lava_gi,               20.f,   "multiplier on the lava's INDIRECT emission -- the lava lighting the "
                                                "room as an area source instead of via the analytic light grid. "
                                                "Default now that this is the shipping model: the grid is off and "
                                                "the lava lights the room by being a large warm surface." )
    // Unattended capture, for driving an A/B without a human at the keyboard.
    //
    // GZDoom's console "wait" takes no argument, so a deferred -exec script
    // fires everything on the first tic: screenshot at the title screen, then
    // quit before the map is even loaded. These count MAP time instead, so the
    // shot lands with the level up and the player wherever rt_lava_autogoto put
    // them. NOARCH -- an autoquit that stuck in the ini would be unpleasant.
    RT_CVAR_NOARCH( rt_autoshot,        0,      "take a screenshot this many TICS after the map starts (35 = 1s). "
                                                "0 = off. Pairs with rt_lava_autogoto for a hands-free A/B." )
    RT_CVAR_NOARCH( rt_autoquit,        0,      "quit this many tics after the map starts. 0 = off." )
    RT_CVAR_NOARCH( rt_autoshot_every,  0,      "after the first rt_autoshot, keep taking one every this many "
                                                "tics. 0 = a single shot. A moving effect cannot be judged from "
                                                "one frame, and relaunching for each moment is slower than "
                                                "shooting a burst." )
    RT_CVAR_NOARCH( rt_lava_debug,      false,  "paint every surface the shader sees as lava MAGENTA. Answers "
                                                "'does the LAVA flag reach the shader' on its own -- a boost that "
                                                "changes nothing cannot tell 'the multiply is not happening' apart "
                                                "from 'what it multiplies is already zero'." )
    RT_CVAR( rt_lava_light_on,          false,  "upload analytic lights over the lava flats (HLAVA*, D64LAVA*), scattered "
                                                "on a grid. Without this the lava lights nothing at all: the "
                                                "lightIntensity in textures.json only ever worked for sprites, so a lava "
                                                "room renders as a black box with a glowing net on the floor. OFF by "
                                                "default: it works, but a grid of point lights is not what a lake "
                                                "is, and it shows -- each point owns a pool, and walking past one "
                                                "drags a circle of illumination along the wall. rt_lava_gi does the "
                                                "same job as an area source. Kept for the A/B and for the case "
                                                "where a sharp shadow is wanted." )
    RT_CVAR( rt_lava_light_intensity,   1800.f, "intensity of ONE grid light, before the spacing correction. The "
                                                "correction keeps total output constant when rt_lava_light_spacing "
                                                "changes, so this is the knob for how bright the lava room is and "
                                                "spacing is the knob for how even it looks. HUNDREDS, not units: "
                                                "RgLightSphericalEXT.intensity is luminous flux in lumen and the rest "
                                                "of this file works in the same scale (flames 900, wall strip segment "
                                                "180, faux panel 500). Set by MEASUREMENT, not analogy: with the "
                                                "instrumentation reporting a light 47.9 map units from the camera and "
                                                "11 within 256, a 2000 lm control light one metre overhead was barely "
                                                "visible in this hall -- so 60 lm at 1.5 m was never going to show. "
                                                "Every earlier value here (0.9, then 180, then 60) was reasoned from "
                                                "what other families use rather than from what this room does. 600 "
                                                "lit the near walls and left the hall underwhelming, so 1800: a lake "
                                                "is not a fixture, and the thing it has to do is fill a room ~40 m "
                                                "across and 24 m tall." )
    RT_CVAR( rt_lava_light_spacing,     96.f,   "grid spacing in MAP UNITS. Smaller is smoother and more expensive; the "
                                                "per-light intensity is scaled by (spacing/96)^2 so changing this does "
                                                "not change the room's brightness, only the evenness of it." )
    RT_CVAR( rt_lava_light_radius,      0.3f,   "RT source radius in METRES. Wide on purpose — a lava lake is an area "
                                                "source, and a small radius gives every grid point its own hard little "
                                                "shadow, which reads as a row of lamps under the floor." )
    RT_CVAR( rt_lava_light_z,           40.f,   "how far ABOVE the floor plane each light sits, map units. Lights placed "
                                                "exactly on the plane self-shadow against it and half the output is "
                                                "lost into the floor; placed too close they burn a hard pool into it "
                                                "directly underneath. 24 units is 0.75 m, about a quarter of the grid "
                                                "spacing, which keeps the wash even." )
    RT_CVAR( rt_lava_light_max,         256,    "cap on lava lights per frame, nearest to the camera first. A big lake "
                                                "at 96-unit spacing is hundreds of grid points and they are all equally "
                                                "worthless once they are a room away." )
    RT_CVAR( rt_lava_light_dist,        2048.f, "cull radius from the camera, map units" )
    RT_CVAR( rt_lava_light_r,         255,      "lava light colour Red [0,255]" )
    RT_CVAR( rt_lava_light_g,          90,      "lava light colour Green [0,255]" )
    RT_CVAR( rt_lava_light_b,          20,      "lava light colour Blue [0,255]" )
    RT_CVAR( rt_lava_light_debug,       0,      "1 = print how many lava sectors matched, how many grid points they "
                                                "produced, how many survived the cap, and the TRUE nearest light. "
                                                "2 = also upload one 2000 lm light AT THE CAMERA. That last one "
                                                "separates the two questions that have been tangled all along: if a "
                                                "light on top of the camera does not light the room, then analytic "
                                                "lights are not working here at all and nothing about the lava grid "
                                                "matters; if it does, the grid is at fault and the camera light is "
                                                "the control." )
    // Diagnostic, NOARCH so it cannot stick: every "the lava lights nothing"
    // judgement in this feature's history was made from wherever the player
    // happened to be, and the debug line eventually showed the camera 48 METRES
    // from the nearest lava light, where 60 lumen is an irradiance of 0.03. The
    // observation was correct and measured nothing. This removes the step.
    RT_CVAR_NOARCH( rt_lava_autogoto,   false,  "on the first frame of a map that has lava, move the player onto it. "
                                                "For the A/B arms only -- it teleports you." )
    RT_CVAR( rt_flame_light_on,         true,   "upload one analytic, FLICKERING light per open flame — the standing torches "
                                                "(TL*/TS*), the wall torches (A030/A031/A032/GTCH), the loose fires "
                                                "(BFLM/GFLM/RFLM/YFLM), the bonfire (FIRE) and the candle (CAND) — "
                                                "instead of the sprite's "
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
    RT_CVAR_NOARCH( rt_sector_tint_presets, true, "apply the per-map albedo-tint table (RT_TINT_PRESETS) at level load, which "
                                                "softens rt_sector_tint_albedo on the maps whose colormaps are cool enough to "
                                                "neutralize a warm flashlight. A map with no row is left EXACTLY as it is -- "
                                                "notably MAP02, whose switch-triggered blue room is the effect the 1.0 default "
                                                "was chosen for. 0 turns the whole table off" )

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
    RT_CVAR( rt_flsh_intensity,         156.f,  "flashlight intensity (dimmer = more horror). 156, not the 90 this shipped with, "
                                                "and the difference is NOT a 73% brightness increase -- it is 1.2x the light, "
                                                "times 1.456x to pay for rt_flsh_u being raised. Raising the source scales the "
                                                "lit footprint by 1.206x in BOTH dimensions, so its area grows 1.456x and the "
                                                "same flux spread over it lands 0.687x as bright. At the old 90 the higher hold "
                                                "would have made the pool DIMMER than before. See rt_flsh_u" )
    RT_CVAR( rt_flsh_radius,            0.02f,  "flashlight source disk radius in meters")
    RT_CVAR( rt_flsh_angle,             42.f,   "flashlight width in degrees")
    RT_CVAR( rt_flsh_pitch,             22.f,   "degrees to tip flashlight aim toward the ground" )
    RT_CVAR( rt_flsh_r,                 -0.3f,  "flashlight position offset - right (in meteres)")
    RT_CVAR( rt_flsh_u,                 -0.58f, "flashlight position offset - up (in meteres). Negative = held below the eye. "
                                                "-0.58, raised from the -0.7 this shipped with: the low hold is deliberate "
                                                "horror framing, but it put the beam's centre on the floor only 1.44 m ahead. "
                                                "The camera is 41 map units up = 1.28 m, so this is 0.70 m above the floor "
                                                "rather than 0.58 m -- 1.206x. Because rt_flsh_pitch is unchanged the triangle "
                                                "is merely SCALED, so the lit spot moves out by the same 1.206x, to 1.74 m. "
                                                "That is why the pitch is not also touched: one number buys both the height "
                                                "and the reach, and moving both would compound them" )
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
    RT_CVAR( rt_sky_log,                false,  "record every SKY_VISIBILITY primitive submitted each frame so "
                                                "`rt_sky_here` can list the sky openings around you, with size and "
                                                "world position, and say whether each is a ceiling opening (FLAT) or "
                                                "a band/slot in a wall (WALL). The opening feeding a room is usually "
                                                "not one you can see, which is what makes this worth having." )
    RT_CVAR( rt_sun_require_sky,        true,   "THE SKY-LEAK FIX, and ON by default -- this is the corrected "
                                                "behaviour, not an experiment. Turn it OFF to see the stock leak "
                                                "(tools\\ab-skyleak.cmd noreq).\n"
                                                "It gates the one DIRECTIONAL light, so it now covers lightning as "
                                                "well as the moon -- and matters more there, because a strike is ~24x "
                                                "brighter and a pinhole that never showed under the moon is glaring "
                                                "under a bolt.\n"
                                                "THE FAILURE MODE TO WATCH is the opposite one: if a courtyard's sky "
                                                "geometry is not closed, legitimate light there is now REJECTED and it "
                                                "goes dark. That is the regression to look for when a map suddenly "
                                                "loses its outdoor lighting.\n"
                                                "Mechanism: a shadow ray that hits nothing is scored as LIT "
                                                "(RTGL1 RtMissShadowCheck.rmiss sets isShadowed = 0). Doom maps have no "
                                                "geometry above a ceiling and are full of T-junctions at wall/ceiling "
                                                "seams, so rays leave the level, hit nothing, and come back 'lit' -- the "
                                                "moon then washes rooms with no opening anywhere near them. This makes "
                                                "the ray prove it reached SKY geometry before it counts, which is the "
                                                "only legitimate way out of a Doom map. Costs one extra ray, and only "
                                                "on pixels the moon already lit." )
    // NOT archived, deliberately. Every other RT_CVAR is CVAR_ARCHIVE, so a
    // diagnostic left at 2 in one console session comes back on every later
    // launch -- and modes 1 and 2 both SUPPRESS rt_sun_require_sky, so a stuck
    // value makes the fix look like it does nothing. That cost a round trip
    // here, and it is the same trap rt_water_debug is NOARCH for.
    RT_CVAR_NOARCH( rt_sun_leak_debug,      0,  "DIAGNOSTIC, three modes. Must be an INT, not a bool -- as a bool the "
                                                "colour mode below is unreachable, because 2 collapses to 1.\n"
                                                "  0 = off.\n"
                                                "  1 = ISOLATE: the moon lights only the surfaces whose rays escaped "
                                                "the map, everything legitimate removed. Whatever is still lit IS the "
                                                "leak.\n"
                                                "  2 = COLOUR: keep both and paint by reason -- RED where the ray "
                                                "reached sky (a real shaft), GREEN where it escaped (a leak) -- "
                                                "green rather than blue because these interiors are near-black and "
                                                "blue reads as shadow in them. One "
                                                "screenshot then says which is which.\n"
                                                "Overrides rt_sun_require_sky when set." )
    RT_CVAR( rt_sun_skyprobe_dist,      0.f,    "max length of the sky probe ray in METERS (0 = RTGL's full ray "
                                                "length). Lower it only if the probe shows up in a profile." )
    RT_CVAR( rt_sun_angdiam,            0.5f,   "apparent diameter of the sun/moon disc in degrees, and the global "
                                                "size gate for sky leaks. 0.5 is the real moon and makes this a POINT "
                                                "light: one shadow ray, so a crack delivers as much light as a "
                                                "doorway. Widening it makes the shadow test proportional instead -- an "
                                                "opening admits light in proportion to how much of the disc it "
                                                "reveals, so narrow gaps dim smoothly and, because an opening of size "
                                                "d seen from L away subtends d/L, distant spill dies while light "
                                                "beside the opening survives. Softens the wanted shafts by the same "
                                                "amount: one knob, both effects. Try 6-15 for leaky maps." )

    RT_CVAR( rt_moon_geo,               true,   "draw the moon as GEOMETRY -- a quad on the sky at the rt_sun bearing "
                                                "(MOONDISC texture, HWSkyPortal::DrawContents) -- instead of painting "
                                                "it into the sky texture. This is what lets the moon sit anywhere, "
                                                "including straight overhead: a Doom sky dome only spans 60 degrees of "
                                                "altitude and puts a flat averaged-colour CAP above that, so a painted "
                                                "moon could not go up there, got sliced by the cap when pushed near it, "
                                                "and needed four calibration cvars to match its own shafts. The quad is "
                                                "placed along the light direction, so it agrees BY CONSTRUCTION and the "
                                                "rt_moon_tex_*/yawsign/pitch_scale family stops mattering. Off = fall "
                                                "back to the painted moon (rebuild the sky with --bake-disc)." )
    RT_CVAR( rt_moon_geo_size,         12.f,   "apparent diameter of the geometry moon, degrees. Independent of "
                                                "rt_sun_angdiam, which is the LIGHT's angular size (shadow softness); "
                                                "this is only how big the disc looks." )
    RT_CVAR( rt_moon_presets,           true,   "apply the per-map moon aim table (RT_MOON_PRESETS) at level load. "
                                                "Off = every map uses the launcher's rt_sun_a/b, which is what you "
                                                "want while hunting a bearing for a map that has no entry yet." )

    // ----------------------------------------------------------------------
    // Clouds. A stack of horizontal discs over the MOONSKY starfield, each one
    // a different horizontal CUT through one 3D density field. See
    // RT_DrawCloudDeck (hw_skyportal.cpp), the deck mesh in CreateDome
    // (hw_skydome.cpp) and tools/gen_clouds.py;
    // docs/rt-clouds-and-lightning.md has the whole story.
    //
    // This is a REPLACEMENT for art MAP11 already has, not an invention. The
    // map builds a two-layer CLOUDPRP skybox ROOM and scrolls it from ACS
    // (script 670 OPEN) -- but sector skybox rooms are ignored under RT, so
    // none of it ever reaches the screen.
    //
    // NOT layers on the sky dome, which is what this replaced. A dome layer
    // rotates about the viewer: the clouds orbit, come round again, and do it
    // identically wherever you stand. A horizontal deck translates along a wind
    // vector instead -- linear and unbounded -- and perspective crowds distant
    // clouds toward the horizon on its own.
    // ----------------------------------------------------------------------
    RT_CVAR( rt_clouds,                 true,   "draw the volumetric cloud deck (CLOUDV1..CLOUDV8 from d64r-rt-sky.pk3). "
                                                "Global, not MAP11-only: the sky is the same night starfield everywhere, "
                                                "and cloud is what makes the moon read as being BEHIND something. "
                                                "Off = bare MOONSKY, as before." )
    RT_CVAR( rt_clouds_presets,         true,   "apply the per-map cloud table (RT_CLOUD_PRESETS) at level load. The "
                                                "deck is OPT-IN: with this on, a map with no entry gets rt_clouds as "
                                                "the launcher left it (pinned OFF), and only listed maps get clouds. "
                                                "Turn this off to force the global settings everywhere, which is what "
                                                "you want while hunting values for a map that has no entry yet -- then "
                                                "type `clouds` and paste the row it prints." )
    RT_CVAR( rt_clouds_shells,          6,      "how many of the 8 baked slices to draw, 1..8, spaced evenly through "
                                                "the stack. THE cost knob: every shell is a full disc re-submitted to "
                                                "RTGL1 and rasterised into all six cubemap faces each frame, twice "
                                                "during a lightning flash. It is also the quality knob -- at 1 the "
                                                "deck is a single flat sheet with no volume at all." )
    RT_CVAR( rt_clouds_horizon,         9.f,    "altitude in degrees at which the deck's rim sits -- i.e. how close to "
                                                "the horizon the clouds reach. Sets the deck's radius and base height "
                                                "together, on a fixed sphere, so the geometry always stays inside the "
                                                "sky dome however this is set. Lower = clouds run further out and "
                                                "compress harder at the horizon." )
    RT_CVAR( rt_clouds_curve,           0.55f,  "how far the deck bows DOWN at its rim, as a fraction of its height. "
                                                "0 is a flat plane, which ends in a visible hard edge hanging in the "
                                                "sky; bowing it brings the far clouds down to meet the horizon." )
    RT_CVAR( rt_clouds_thick,           0.7f,   "vertical extent of the shell stack, as a fraction of the base height. "
                                                "This is the parallax: it is how far apart the slices actually are, so "
                                                "at 0 they collapse into one plane and the volume is gone." )
    RT_CVAR( rt_clouds_tiles,           6.f,    "how many times a slice tiles across the deck radius. Higher = smaller, "
                                                "more numerous clouds. Pushing it far up aliases badly at the rim, "
                                                "where perspective is already compressing the texture to nothing." )
    RT_CVAR( rt_clouds_alpha,           0.9f,   "per-shell opacity. Multiplies with the slice's own alpha and with the "
                                                "deck's baked radial fade, and compounds across shells -- so lowering "
                                                "it thins the whole deck, not just one layer." )
    RT_CVAR( rt_clouds_dark,            0.45f,  "brightness of the BOTTOM shell relative to the top one. The slice art "
                                                "already carries baked top-down transmittance; this ramp is on top of "
                                                "it and is what makes the deck read as lit from above at a glance." )
    RT_CVAR( rt_clouds_wind,            0.014f, "wind speed, in TILES PER SECOND of texture translation. Linear and "
                                                "unbounded -- this is the whole reason the clouds are on a deck rather "
                                                "than on the dome. Negative reverses." )
    RT_CVAR( rt_clouds_wind_dir,        30.f,   "wind bearing in degrees" )
    RT_CVAR( rt_clouds_shear,           0.09f,  "how far downwind each shell sits relative to the one below it, in "
                                                "tiles. Real cloud decks shear with height, and this is the cheapest "
                                                "thing here that makes the volume unmistakable: without it the shells "
                                                "line up vertically and the eye reads concentric rings instead of "
                                                "depth. Too much and the stack separates into visibly distinct sheets." )
    RT_CVAR_COLOR( rt_clouds_tint,    0xB4C0DC, "cloud COLOUR (hex), and the colour moonlight takes on when it comes "
                                                "through them. The slice art is deliberately near-achromatic -- it "
                                                "carries shape and luminance only -- so this cvar owns the hue "
                                                "outright, and any hue reproduces cleanly. That matters because "
                                                "SetObjectColor is a multiply: a blue-baked texture cannot be tinted "
                                                "purple, only muddied. Set per map in RT_CLOUD_PRESETS -- several "
                                                "levels have a dark purple skybox that the default cool blue fights." )
    RT_CVAR( rt_clouds_transmit,        0.22f,  "fraction of moonlight a FULLY covered patch of cloud still passes, "
                                                "coloured by rt_clouds_tint. This is what makes the tint reach the "
                                                "light and not just the picture: shafts under a purple deck come out "
                                                "purple. It is also the real floor on cloud occlusion -- at 0 a solid "
                                                "overcast takes a moon-lit map to black, which is worse than blocking "
                                                "too little." )
    RT_CVAR( rt_clouds_occlude,         1.f,    "how much the deck blocks the MOON, 0..1. The clouds are sky geometry, "
                                                "so they are never in the acceleration structure and cannot cast a "
                                                "shadow -- without this the moon pours through a solid overcast at full "
                                                "strength and the deck is wallpaper. Instead RT_DrawCloudDeck walks the "
                                                "moon's ray up through the shells, samples each slice's alpha where it "
                                                "crosses, and multiplies the transmittances; the result scales "
                                                "rt_sun_intensity. So cloud drifting over the moon really does dim the "
                                                "shafts, and it changes as the wind moves. Floored at 0.12 -- taking a "
                                                "moon-lit map to zero is a worse failure than blocking too little." )
    RT_CVAR( rt_clouds_flash,           2.2f,   "how hard a lightning strike lights the deck, as a multiplier on the "
                                                "strike envelope before it is clamped to a full additive pass. Above 1 "
                                                "the flash saturates early, which is right for lightning. The pass is "
                                                "weighted toward the LOWER shells -- the strike is under the deck." )

    // ----------------------------------------------------------------------
    // Lightning. Driven by the engine's own Hexen storm thinker
    // (DLightningThinker, a_lightning.cpp), which MAP11 turns on with the
    // MAPINFO `lightning` keyword. Nothing here fires on a map without it.
    // ----------------------------------------------------------------------
    RT_CVAR( rt_lightning,              true,   "RT lightning: on every strike the storm thinker reports, flash the "
                                                "clouds, draw a bolt on the sky, and fire an analytic DIRECTIONAL "
                                                "light down the bolt's own bearing.\n"
                                                "The directional light is the part that matters. RT's sky is a "
                                                "rasterised cubemap sampled on ray miss and is NOT importance-sampled, "
                                                "so a bright bolt painted into it is found only by rays that happen to "
                                                "point at it -- at 1 spp that is far too noisy to light a room, exactly "
                                                "as it is for the moon (see rt_moon_geo). The bolt is scenery; this "
                                                "light is the source. Same pairing, same rule: move one, move both." )
    RT_CVAR( rt_lightning_intensity,  2200.f,   "peak intensity of the lightning directional light. Large next to "
                                                "rt_sun_intensity (90) on purpose -- it lasts a fifth of a second and "
                                                "has to overwhelm the moon while it does." )
    RT_CVAR_COLOR( rt_lightning_color, 0xC8D8FF, "lightning colour (hex). Slightly cooler than the moon's B4C8FF." )
    RT_CVAR( rt_lightning_decay,        0.18f,  "exponential decay time of ONE stroke, seconds. The visible flash runs "
                                                "roughly 3x this." )
    RT_CVAR( rt_lightning_strokes,      3,      "max sub-strokes per strike, 1..6. Real lightning is a burst of return "
                                                "strokes a few tens of ms apart, and the stutter is most of what makes "
                                                "a flash read as lightning rather than as someone toggling a light. "
                                                "Strokes take the MAX of their envelopes, never the sum, so raising "
                                                "this makes a strike longer and twitchier, never brighter." )
    RT_CVAR( rt_lightning_angdiam,      6.f,    "angular diameter of the lightning light, degrees -- i.e. HOW SHARP THE "
                                                "SHADOWS IT THROWS ARE. Read rt_sun_angdiam's note first: a wide disc "
                                                "makes the shadow test proportional rather than binary, so pinhole sky "
                                                "leaks admit a sliver instead of a full-strength shaft, at the cost of "
                                                "softening every real shadow by the same amount.\n"
                                                "The strike casts genuine ray-traced shadows -- it is an analytic "
                                                "directional light, not a screen flash -- and this is the ONE knob that "
                                                "decides whether they read. The first value tried here was 14, chosen "
                                                "purely for leak safety, and at 14 the penumbra is so wide at room "
                                                "distances that the shadows turn to mush and the flash looks like a "
                                                "fullscreen brightness pulse. 6 keeps them clearly directional while "
                                                "staying 12x wider than the moon, which matters because the strike is "
                                                "~24x brighter and a leak that is invisible at rt_sun_intensity 90 is "
                                                "glaring at 2200. Drop toward 1.5 for hard dramatic shadows if the map "
                                                "does not leak." )
    RT_CVAR( rt_lightning_alt_min,      25.f,   "lowest altitude a strike is placed at, degrees" )
    RT_CVAR( rt_lightning_alt_max,      55.f,   "highest altitude a strike is placed at, degrees" )
    RT_CVAR( rt_lightning_bolt,         true,   "draw the visible bolt quad (BOLT1..BOLT4) at the strike bearing" )
    RT_CVAR( rt_lightning_bolt_size,    60.f,   "apparent HEIGHT of the bolt on the sky, degrees. Width follows from "
                                                "the texture's aspect, so the bolt is never stretched." )
    RT_CVAR( rt_lightning_sectorflash,  true,   "let the stock thinker keep flashing every F_SKY1 sector's lightlevel "
                                                "(MAP11 has 106 of them). That is the vanilla flash, and under RT it "
                                                "is ALSO a light: rt_sector_emis turns any sector over the map's "
                                                "threshold into a surface emitter, so a strike briefly makes the whole "
                                                "outdoors glow with nothing casting it -- the same sourceless-glow "
                                                "problem d64r-seqlight-fix.wad exists to remove. Kept on by default "
                                                "because here there IS a source and it is 0.15s long, but turn it off "
                                                "to see the strike lit purely by the directional light." )
    // NOARCH: every other RT_CVAR is CVAR_ARCHIVE, so a diagnostic left on in
    // one console session comes back on every later launch. Same trap as
    // rt_sun_leak_debug and rt_water_debug.
    RT_CVAR_NOARCH( rt_lightning_debug,     0,  "print each strike's bearing, altitude, stroke pattern and bolt "
                                                "variant, then one line per frame while the flash is live" )

    RT_CVAR( rt_reflrefr_depth,         8,      "max depth of reflect/refract")
    RT_CVAR( rt_refr_glass,             1.52f,  "glass index of refraction") 
    RT_CVAR( rt_refr_water,             1.33f,  "water index of refraction") 
    RT_CVAR( rt_refr_thinwidth,         0.0f,   "approx. width of thin media, e.g. thin glass (in meters)") 
    RT_CVAR( rt_refl_thresh,            0.0f,   "assume mirror if roughness is less than this value") 

    // Doom64-RT: this ALSO gates localised smoke (rt_smoke). The puff is spawned
    // from the flash's own resolved position -- which has already been traced
    // back out of any wall the raw offset would have put it inside -- so that
    // the light and the smoke it lights are one point rather than two similar
    // calculations that can drift apart. Turning the flash off therefore turns
    // muzzle smoke off with it, which is why the launcher pins this.
    RT_CVAR( rt_mzlflsh,                true,   "enable muzzle flash light source (activated on extralight). Also the "
                                                "source of the muzzle SMOKE spawn (rt_smoke): a puff is born at this "
                                                "light's resolved position, so 0 disables both." )
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

    // --- Illuminated fog (Doom64-RT) -------------------------------------
    //
    // Doom 64 fogs whole levels, and says so in MAPINFO: `fade` is the colour
    // and `fogdensity` is how thick. Nine Retribution maps carry it -- MAP26's
    // cyan "00 56 56" at density 200 is the one in screen/
    // doom64original_level26fog.png -- and under RT not one of them showed it,
    // because fade/fogdensity are rasterizer fog and the RT path never reads
    // them.
    //
    // This is not that fog rebuilt. Rasterizer fog is a per-pixel LERP toward a
    // colour by distance: it cannot be lit, so a lamp inside it has no halo and
    // a dark corner fogs exactly as brightly as a lit hall. What is built here
    // is the medium itself, in RTGL1's froxel volume -- light scatters through
    // it, so the fog around a torch glows and the fog in an unlit corridor
    // stays dark. The map's authored colour and density are the input.
    //
    // OPT-IN per map (RT_FOG_PRESETS), for the reason the cloud deck is: fog is
    // not neutral scenery, it changes every distance judgement in the level.
    RT_CVAR( rt_fog,                    true,   "master switch for the per-map illuminated fog. OFF: the volumetrics fall "
                                                "back to the plain global rt_volume_* values on every map, which is the "
                                                "stock behaviour." )
    RT_CVAR( rt_fog_presets,            true,   "apply the per-map fog table (RT_FOG_PRESETS) at level load. It writes "
                                                "rt_fog_color/_density/_far/_ambient/_illum AFTER the command line is "
                                                "parsed, so on a listed map it overrides a +rt_fog_* pin -- the same trap "
                                                "RT_CLOUD_PRESETS has. 0 to tune with the `fog` CCMD." )
    RT_CVAR_COLOR( rt_fog_color,      0x000000, "fog colour (hex): the medium's scattering albedo, so it tints the fog AND "
                                                "the haze around anything glowing inside it. BLACK (000000) is the "
                                                "sentinel for `use the map's own MAPINFO fade`, which is what every "
                                                "preset row wants -- a genuinely black fog would be invisible anyway, so "
                                                "nothing is given up by spending the value. Extinction stays monochrome, "
                                                "so distance fades TOWARD this colour rather than filtering by it." )
    RT_CVAR( rt_fog_density,            0.01f,  "fog density AT THE CAMERA -- the near end of the ramp whose far end is "
                                                "rt_fog_density_far. NEGATIVE means `derive it from the map's MAPINFO "
                                                "fogdensity`, scaled by rt_fog_density_mult. Feeds rt_volume_scatter, "
                                                "whose per-froxel coefficient is a flat 0.001 per cell over 64 cells -- "
                                                "so useful values here are tens, not fractions, and shortening "
                                                "rt_fog_far makes the same number read THICKER per metre." )
    // NEAR and FAR are separate because one density cannot be both thin enough
    // to see your own feet through and thick enough to swallow a corridor. The
    // froxel grid's slices are uniform in distance, so the split costs one
    // mix() per cell -- no second volume, no extra rays.
    RT_CVAR( rt_fog_density_far,        10.f,   "fog density at rt_fog_far, the far end of the ramp whose near end is "
                                                "rt_fog_density. NEGATIVE means `same as near`, i.e. a uniform medium, "
                                                "which is what a map inheriting its density from MAPINFO gets. Set this "
                                                "ABOVE rt_fog_density for the usual look -- clear air around the player, "
                                                "thickening with distance -- and BELOW it for the opposite, which is "
                                                "what a room full of smoke with a clear ceiling gap reads like." )
    RT_CVAR_COLOR( rt_fog_color_far,  0x000000, "fog colour at rt_fog_far. BLACK (000000) is the sentinel for `same as "
                                                "rt_fog_color`, which is what almost everything wants. Splitting it lets "
                                                "the distance go a different hue from the air in the room -- the fog is "
                                                "one medium either way, so this is stylization, not physics." )
    RT_CVAR( rt_fog_curve,              1.f,    "shape of the near->far ramp. 1 = linear. ABOVE 1 holds the near value "
                                                "longer and thickens late, so the room you are standing in stays clear "
                                                "and the far end still closes -- usually what you want when near and far "
                                                "are far apart. BELOW 1 thickens immediately." )
    RT_CVAR( rt_fog_density_mult,       0.3f,   "scale from MAPINFO fogdensity (0..255) to rt_fog_density. Doom 64's "
                                                "density is a rasterizer fog factor with no physical meaning here, so "
                                                "this is the one number that had to be settled by eye against "
                                                "screen/doom64original_level26fog.png." )
    RT_CVAR( rt_fog_far,                45.f,   "far plane of the fog volume, in METRES (1 map unit = 1/32 m, so 45 m is "
                                                "~1440 units). Everything beyond it is shaded with the far slice, i.e. "
                                                "fully fogged, which is what makes a bounded volume read as unbounded "
                                                "fog. It is also the precision knob: 64 slices are spread over this, so "
                                                "raising it coarsens the fog near the camera." )
    RT_CVAR( rt_fog_ambient,            1.f,    "unlit floor of the fog: what a froxel scatters with no light reaching "
                                                "it at all. At the shipping 1 this is the DOMINANT term -- the medium "
                                                "glows on its own and the level's lights modulate it rather than supply "
                                                "it, which is what makes a thin fog visible at all at rt_fog_density "
                                                "0.01. It was 0.02 while the fog was thick enough to be an occluder. "
                                                "Lower it toward 0 to hand the look back to the lights, and see "
                                                "tools/ab-fog.cmd noambient for what that costs." )
    RT_CVAR( rt_fog_light_near,         2.f,    "fog only: fade in-scattering out within this many METRES OF A LIGHT. A "
                                                "light standing in the medium lights the froxels around it by inverse "
                                                "square, so a light at ~0 m -- the FLASHLIGHT, a muzzle flash, anything "
                                                "carried -- floods the cells directly in front of the camera and the "
                                                "screen whites out. That is exactly what a headlight in fog does, and it "
                                                "is unplayable: the flashlight becomes a switch that blinds you. Keyed "
                                                "off the LIGHT's distance rather than the camera's on purpose, so what "
                                                "is removed is glare from a light you are HOLDING, while the beam's "
                                                "shaft further down the corridor -- the whole point of lit fog -- "
                                                "survives. Directional lights (moon, lightning) are unaffected: their "
                                                "sampled position is far away. 0 = physical, and blinding." )
    RT_CVAR( rt_fog_lightmult,          1.f,    "multiplier on light scattered by the fog (rt_volume_lintensity while a "
                                                "fog map is loaded)." )
    RT_CVAR( rt_fog_illum,              true,   "light the fog from ALL lights, not just one. The stock froxel pass "
                                                "scatters exactly the single light LightManager::TryGetVolumetricLight "
                                                "picks -- a RG_LIGHT_ADDITIONAL_VOLUMETRIC light if any exists, and "
                                                "nothing in this game sets that flag, so in practice the SUN. On a map "
                                                "with the moon turned off (MAP26) that leaves the fog with no source at "
                                                "all and it collapses to flat rt_fog_ambient. 1 runs the full direct "
                                                "estimate per froxel instead. Costs one NEE + shadow ray per cell of a "
                                                "160x88x64 grid, and is the reason this is per map rather than global." )
    RT_CVAR_NOARCH( rt_fog_debug,       false,  "log the fog resolved for each level: the map's MAPINFO fade/fogdensity, "
                                                "the preset row applied, and the values that actually reached RTGL." )

    // --- LOCALISED SMOKE ----------------------------------------------------
    //
    // The fog above is ONE density for the whole level and the froxel grid is
    // camera-fitted, so there is no way to say "there is smoke HERE". These add
    // a small list of world-space spheres whose density is ADDED to the fog's
    // inside RtVolumetric.rgen. It is the same medium, so the same lights
    // scatter through it -- a puff at the barrel is lit by its own muzzle flash.
    //
    // The puffs are simulated on the CPU, which is the only side that can see
    // the level: a puff pools under a low ceiling because P_PointInSector and
    // ceilingz are here, not on the GPU. The counts are tens, so the sim is
    // free, and the froxel grid rather than the particle budget is what caps
    // the detail -- see docs/rt-smoke.md.
    RT_CVAR( rt_smoke,                  true,   "master switch for localised volumetric smoke (muzzle smoke today). OFF: "
                                                "no puffs are spawned, RgDrawFrameSmokeParams carries a count of 0, and "
                                                "the froxel shader collapses to exactly the fog's arithmetic -- so this "
                                                "is also the A/B control that proves smoke costs a fog map nothing. "
                                                "NOTE rt_mzlflsh 0 also disables it: the puff is spawned at the muzzle "
                                                "flash's resolved position so the light and the smoke it lights are "
                                                "one point, which makes the flash the trigger for both." )
    RT_CVAR( rt_smoke_density,          6.f,    "optical depth per METRE at a puff's core. Unlike rt_fog_density this is "
                                                "NOT per froxel cell: the engine scales it by the volume's slice "
                                                "thickness before upload, so a puff reads the same whatever rt_smoke_far "
                                                "-- or a fogged map's rt_fog_far -- does to the grid. Raise it until the "
                                                "puff occludes; the fog is a luminous veil and does not, which is the "
                                                "whole difference between the two media sharing this volume." )
    RT_CVAR_COLOR( rt_smoke_color,    0x9E9689, "smoke colour (hex): the puff's scattering albedo, i.e. what it does "
                                                "with the light that reaches it. Warm grey by default -- powder smoke, "
                                                "not steam. A dark value reads as soot and, being an albedo, dims the "
                                                "puff as well as tinting it." )
    RT_CVAR( rt_smoke_count,            4,      "puffs spawned per shot, as the per-weapon rows multiply it. Each is an "
                                                "ellipsoid; several overlapping ones read as one irregular cloud where "
                                                "one reads as a ball. Capped by rt_smoke_budget across all live puffs. "
                                                "The ROCKET trail and the monster guns divide by this before "
                                                "multiplying back, so they emit a fixed number of parcels and moving "
                                                "this knob is purely a player-weapon change." )
    RT_CVAR( rt_smoke_budget,           48,     "maximum live puffs UPLOADED, of the 128 the uniform can carry "
                                                "(RG_MAX_SMOKE_PUFFS). When more exist the NEAREST survive, since the "
                                                "whole point is the smoke in front of your face. "
                                                "THIS IS A SHADER-TIME KNOB, not a memory one, and the two limits are "
                                                "different numbers: the CAP is uniform bytes (128 puffs = 8 KB, under "
                                                "the 16 KB Vulkan guarantees), while the BUDGET costs a loop iteration "
                                                "per uploaded puff in each of ~900k froxels. smoke_evalAt rejects a "
                                                "puff with one dot product before any of the expensive math, so the "
                                                "real cost tracks how many puffs are NEAR a cell rather than how many "
                                                "exist -- which is what makes ambient torch smoke affordable at all. "
                                                "Raise it if smoke pops out of existence in a crowded room; lower it "
                                                "first if the volume gets expensive." )
    RT_CVAR( rt_smoke_life,             2.2f,   "puff lifetime in seconds. Density falls off over it as (1 - age)^2, so "
                                                "the last third is nearly gone -- this is where a puff VANISHES, not "
                                                "where it stops being visible. RAISE IT WITH rt_smoke_growth LOWERED BY "
                                                "THE SAME FACTOR, or a longer-lived puff is simply a bigger one: it "
                                                "keeps expanding at the shared rate for the whole extra time, and the "
                                                "radius0/radius dilution then thins it back out. 1.6/0.7 and 2.2/0.5 "
                                                "reach the same final size; the second holds its density most of a "
                                                "second longer on the way there." )
    RT_CVAR( rt_smoke_radius,           0.35f,  "puff radius at spawn, in METRES. THE FLOOR IS THE FROXEL SLICE, and it "
                                                "is bigger than it looks: the volume's 64 slices spread over its reach, "
                                                "so at the shipping rt_volume_far 30 a slice is 0.47 m and a puff needs "
                                                "a radius over ~0.25 just to span one. 0.18 -- the first value tried -- "
                                                "is 0.77 of a cell across its whole DIAMETER, i.e. it lands inside a "
                                                "single froxel and reads as a flicker. Smoke only gets rt_smoke_far's "
                                                "finer slices when it owns the volume (no fog and rt_volume_type 0)." )
    RT_CVAR( rt_smoke_growth,           0.5f,   "how fast a puff expands, metres per second. Expansion is what makes "
                                                "smoke read as smoke rather than as a fading ball, and it thins the puff "
                                                "at the same time because the density is not renormalized. COUPLED TO "
                                                "rt_smoke_life: the final radius is radius + growth x life, so these two "
                                                "move in opposite directions to change how long smoke lasts without "
                                                "changing how big it gets." )
    RT_CVAR( rt_smoke_speed,            1.8f,   "initial speed along the barrel, metres per second." )
    RT_CVAR( rt_smoke_spread,           0.55f,  "random lateral velocity at spawn, m/s, so the puffs of one shot "
                                                "separate instead of travelling as a clump." )
    RT_CVAR( rt_smoke_rise,             0.65f,  "buoyancy, metres per second squared. Powder smoke rises slowly; steam "
                                                "would want several times this." )
    RT_CVAR( rt_smoke_drag,             1.9f,   "velocity damping per second. High enough that the muzzle velocity is "
                                                "spent in the first third of the life, which is what stops the smoke "
                                                "flying away from the gun like a projectile." )
    RT_CVAR( rt_smoke_inherit,          0.85f,  "how much of the PLAYER's velocity a puff is born with, 0..1. Below "
                                                "~0.8 the smoke visibly lags a strafing player and reads as stuck to "
                                                "the world; at 1 it is glued to the camera. This knob decides which of "
                                                "those two wrong answers you get." )
    RT_CVAR( rt_smoke_offset,           0.7f,   "where along the eye->muzzle segment a puff is born, as a FRACTION: 0 "
                                                "is at the eye, 1 at the muzzle flash itself. NOT a distance in metres, "
                                                "and that is the whole point. RT_AddMuzzleFlash already TRACES that "
                                                "segment against the level and pulls the flash back out of anything it "
                                                "hits, so every point on it is known to be in open space. Adding metres "
                                                "on top of the traced position -- which the first version did -- pushes "
                                                "the puff straight through the wall you are shooting at, where the "
                                                "per-pixel volumetric sample never reaches it: the smoke is behind the "
                                                "surface that terminates the ray. It looks exactly like smoke that was "
                                                "never created." )
    RT_CVAR( rt_smoke_repeat,           5,      "minimum TICS between spawns while the trigger is still held. A shot is "
                                                "detected as the rising edge of player->extralight, which the weapon's "
                                                "Flash state raises with A_Light1/2 and clears with A_Light0 -- but a "
                                                "fast weapon RE-ENTERS its flash state before A_Light0 runs, so on the "
                                                "chaingun and the plasma rifle extralight never returns to 0 and the "
                                                "edge fires once for an entire burst. This is the fallback that keeps "
                                                "automatic fire smoking. 5 tics is a little under the chaingun's 4-tic "
                                                "cycle, so a held trigger produces roughly one burst per shot without "
                                                "emptying rt_smoke_budget into the first half second. 0 = rising edge "
                                                "only, which is the arm that shows the problem." )
    RT_CVAR( rt_smoke_far,              14.f,   "far plane of the froxel volume while smoke is live and NO fog is (a "
                                                "fogged map always wins). A RESOLUTION knob, not a reach: the volume's "
                                                "64 slices are spread over it, so 14 m gives ~0.22 m cells -- fine "
                                                "enough to resolve a puff -- where rt_volume_far's 30 gives 0.47 and a "
                                                "puff reads as one slab. Short costs nothing here, because the base "
                                                "density is 0 in smoke-only mode and the far slice is empty." )
    RT_CVAR( rt_smoke_ambient,          0.08f,  "unlit floor for a smoke-only frame (no fog): what a puff scatters in a "
                                                "room with no light on it. Small on purpose -- smoke you can see in the "
                                                "dark is smoke that is not being lit, which is the failure this feature "
                                                "exists not to be. On a FOGGED map the fog's much larger rt_fog_ambient "
                                                "applies instead, because the ambient term is global to the volume." )
    RT_CVAR( rt_smoke_illum,            true,   "light smoke from ALL lights. Same cost as rt_fog_illum -- one NEE + "
                                                "shadow ray per froxel cell -- but paid only on frames that contain a "
                                                "puff, which is why muzzle smoke can afford what global fog could not. "
                                                "OFF, a puff gets ambient plus whichever single light "
                                                "TryGetVolumetricLight picked, i.e. usually nothing: flat grey soup." )
    RT_CVAR( rt_smoke_spp,              4,      "direct-lighting samples per froxel INSIDE a puff; 1 is stock. Every other "
                                                "noise fix here -- the spatial filter, the reprojected history -- smooths "
                                                "an estimate built from ONE sample; this improves the estimate itself, "
                                                "which is the only thing that helps when the noise is temporal or when a "
                                                "LIGHT is moving (a flying plasma bolt changes the radiance at a fixed "
                                                "world point, so no amount of temporal averaging can fix it). Affordable "
                                                "because a puff covers a few per cent of the 160x88x64 grid: 4x in those "
                                                "cells is a rounding error, where 4x globally would not be." )
    RT_CVAR( rt_smoke_maxlight,         6.f,    "ceiling on the in-scattered light inside a puff. A light carried at ~0 m "
                                                "-- the flashlight, the plasma rifle's blue glow -- lights the froxels "
                                                "around it by INVERSE SQUARE, so a dense puff in front of the camera "
                                                "saturates to pure white. That is what rt_fog_light_near exists to stop, "
                                                "and smoke disables that fade on purpose so a muzzle flash can light its "
                                                "own puff -- so this bounds the RESULT instead of the cause, keeping the "
                                                "flash while refusing to blow out the screen. Fog is untouched. 0 = no "
                                                "clamp, which is the A/B." )
    RT_CVAR( rt_smoke_light_near,       0.5f,    "the near-light fade (rt_fog_light_near) applied INSIDE a puff, in "
                                                "metres. 0 -- no fade -- is the shipping value and is deliberately the "
                                                "opposite of the fog's 2: the fog needs the fade because a carried light "
                                                "whites out the screen, while the muzzle flash lighting the smoke at the "
                                                "barrel is the entire effect. Chosen PER FROXEL in RtVolumetric.rgen, so "
                                                "fog cells keep rt_fog_light_near exactly and this cannot retune a "
                                                "fogged map." )
    RT_CVAR( rt_smoke_illum_blend,      0.2f,   "temporal blend for the all-lights estimate inside a puff. The stock "
                                                "0.05 is right for fog, which changes only as fast as the player walks; "
                                                "a muzzle flash lasts 2-3 frames, and at 0.05 the volume needs ~0.7 s to "
                                                "respond and as long again to let go -- the smoke would light up AFTER "
                                                "the flash and then linger. Higher is more responsive and noisier, and "
                                                "the volumetric is not denoised. Per froxel, like rt_smoke_light_near." )
    RT_CVAR( rt_smoke_rocket,           true,   "smoke on the ROCKET rather than at the launcher's muzzle: a trail along "
                                                "the projectile's flight and a burst where it dies. The launcher itself "
                                                "makes none -- a backblast that hangs in front of you while the rocket "
                                                "leaves is the wrong read, and it blocks the shot you just took." )
    RT_CVAR( rt_smoke_rocket_every,     1,      "TICS between trail parcels dropped by a rocket in flight. A rocket covers "
                                                "about 0.6 m a tic, so every OTHER tic leaves 1.2 m gaps between parcels "
                                                "9 cm across -- a dotted line of separate beads, which is exactly what it "
                                                "looked like. Every tic, with a parcel wide enough to bridge the gap, is "
                                                "what reads as a continuous trail." )
    RT_CVAR( rt_smoke_rocket_radius,    0.16f,  "radius of a rocket trail parcel at birth, in METRES. It has to be at "
                                                "least half the distance the rocket covers between drops or the trail is "
                                                "beads rather than a line -- at 0.6 m a tic that means ~0.3 m of diameter, "
                                                "which is what 0.16 gives. Reduce the trail with its LIFE (in "
                                                "RT_UpdateSmokePuffs), not with this: a shorter-lived parcel makes a "
                                                "shorter plume, a smaller one makes a dotted plume of the same length." )
    RT_CVAR( rt_smoke_boom,             5,     "parcels in the burst when a rocket dies. This is the one place a fat "
                                                "cloud is right, because the explosion is meant to obscure -- but ten of "
                                                "them was the noisiest thing in the game, each an independent one-sample "
                                                "estimate stacked on the others." )
    RT_CVAR( rt_smoke_boom_radius,      0.34f,  "radius of an explosion parcel at birth, in METRES." )
    RT_CVAR( rt_smoke_monster,          true,   "smoke off a MONSTER's gun as well as the player's -- the zombieman's "
                                                "rifle and the shotgun guy's wide bore. There is no code hook to hang "
                                                "this on: a monster's attack is a DECORATE state, so the trigger is the "
                                                "SPRITE FRAME (POSS/SPOS/CPOS/PLAY/SSWV frame F, the same frames "
                                                "gen_fx_emissives.py puts a muzzle emissive on), read on the tic it is "
                                                "entered. See RT_MONSTER_GUNS in rt_smoke.cpp." )
    RT_CVAR( rt_smoke_monster_scale,    1.f,    "how much monster gun smoke, as a multiplier on both the parcel COUNT and "
                                                "the density of the RT_MONSTER_GUNS rows. Both together on purpose: "
                                                "halving the count alone only makes the survivors conspicuous. 0 is the "
                                                "same as rt_smoke_monster 0, and the A/B for whether it is worth the "
                                                "budget at all when six of them are shooting at once." )
    RT_CVAR( rt_smoke_monster_far,      18.f,   "METRES beyond which a monster's shot makes no smoke. Not a cosmetic cull: "
                                                "the puff pool overflows OLDEST-OUT while the upload keeps the NEAREST "
                                                "puffs, so a firefight across the map would quietly push the smoke off "
                                                "your own barrel out of the array. Past the volume's reach a puff is "
                                                "not represented at all anyway. 0 disables the cull." )
    RT_CVAR( rt_smoke_projectile,       true,   "smoke on flying projectiles OTHER than the rocket: the revenant tracer "
                                                "(TRCR), the Mother Demon's ball (RBAL) and fire (FIRE), and the "
                                                "mancubus fireball (MANF). Same machinery as the rocket -- a trail along "
                                                "the flight and a burst where MF_MISSILE clears -- but the per-projectile "
                                                "numbers live in RT_PROJECTILE_SMOKE rather than in cvars, because five "
                                                "of them is more than anyone will tune by hand. Separate from "
                                                "rt_smoke_rocket because they are separate judgements: a rocket "
                                                "obviously smokes, a fireball is a taste call." )
    RT_CVAR( rt_smoke_barrel,           true,   "a smoke burst when an exploding barrel goes off. Triggered on the "
                                                "sprite frame A_Explode sits on (BEXP frame E), NOT on the actor going "
                                                "away -- 64ExplosiveBarrel lingers through a 1050-tic respawn timer, so "
                                                "the disappearance rule the projectiles use would fire the smoke about "
                                                "twenty seconds late." )
    RT_CVAR( rt_smoke_barrel_scale,     1.f,    "how much of it, scaling the barrel burst's parcel count and density "
                                                "together. This is the one explosion allowed to obscure -- unlike the "
                                                "rocket's, it does not have to leave the shot you just took visible." )
    RT_CVAR( rt_smoke_ambient_fx,       true,   "a thin column of smoke rising off every FLAME in the level: the twelve "
                                                "torch and sconce sprites, the loose floor fires, 64BigFire's bonfire, "
                                                "and the Lost Soul's burning head. See RT_AMBIENT_FLAMES in "
                                                "rt_smoke.cpp. Note the name: rt_smoke_ambient is the unlit floor for "
                                                "smoke-only frames and has nothing to do with this." )
    RT_CVAR( rt_smoke_ambient_scale,    1.f,    "how much flame smoke, scaling its density. A flame's wisp is meant to "
                                                "read as heat carrying soot upward, not as a puff -- a torch that "
                                                "coughs like a shotgun looks like it is going out. 0 is the same as "
                                                "rt_smoke_ambient_fx 0." )
    RT_CVAR( rt_smoke_ambient_budget,   40,     "ceiling on how many AMBIENT puffs may be alive at once. This is the "
                                                "knob that keeps flames from eating the feature: every other source is "
                                                "an EVENT and stops on its own, but a torch never does, so ambient "
                                                "smoke would win every contest for a pool slot simply by outlasting "
                                                "everything else. Emission stops at this cap rather than pushing other "
                                                "smoke out -- and separately, the pool evicts ambient puffs BEFORE any "
                                                "other kind, so a shot always finds room even when flames are at their "
                                                "cap. Must stay well under rt_smoke_budget." )
    RT_CVAR( rt_smoke_ambient_far,      9.f,    "METRES beyond which a flame makes no smoke. Tighter than "
                                                "rt_smoke_monster_far because a level holds far more flames than "
                                                "gunmen -- 117 placements of 64BigFire alone across nine maps -- and a "
                                                "torch two rooms away is a slot spent on something the froxel grid "
                                                "cannot resolve. 0 disables the cull, which on a torch-lit map is how "
                                                "you spend the entire budget in one frame." )
    RT_CVAR( rt_smoke_curl,             1.6f,   "lateral turbulence, m/s^2 at one second of age. A barrel wisp leaves the "
                                                "muzzle LAMINAR -- a straight thread for the first fraction of a second "
                                                "-- and only breaks into curls once it has risen, so this scales with the "
                                                "square of a parcel's age rather than being constant. Each parcel carries "
                                                "its own phase, or a whole trail waves in unison and reads as a ribbon. "
                                                "0 = straight columns." )
    RT_CVAR( rt_smoke_trail,            1.f,    "scale on the per-weapon TRAIL -- the parcels a shot keeps releasing from "
                                                "the muzzle point after the bang. This is what makes a filament: a "
                                                "single burst is a ball at any radius, because a thin column is a shape "
                                                "in TIME, not in size. The pistol releases seven parcels over about a "
                                                "fifth of a second and each rises as it is born, so what you see is a "
                                                "thread of separated beads rather than a cloud. 0 disables trails and "
                                                "gives every weapon a single burst, which is the A/B." )
    RT_CVAR( rt_smoke_light_far,        6.f,    "METRES beyond which a light stops lighting smoke. Smoke runs the "
                                                "ALL-LIGHTS estimate while the rest of the volume runs the single-light "
                                                "path, so a puff picks up every emissive in the room -- an armour "
                                                "bonus, a barrel, a monitor across the hall -- and a saturated one far "
                                                "away can colour the whole puff. Physically that light does reach it, "
                                                "but at 1 sample per froxel it arrives as a colour cast rather than as "
                                                "a subtle tint. The fade is smooth over the last quarter of the range. "
                                                "0 = no limit, which is the physical answer and the A/B." )
    RT_CVAR( rt_smoke_perweapon,        true,   "bend the smoke profile per weapon (RT_SMOKE_PROFILES in rt_main.cpp). "
                                                "A pistol breathes a thin filament off the barrel; a shotgun coughs a "
                                                "cloud; plasma and the Unmaker are barely there, because they are not "
                                                "combustion. The rows are MULTIPLIERS on the rt_smoke_* cvars, so tuning "
                                                "a cvar still moves every weapon together and a row only states how that "
                                                "weapon differs. 0 gives every gun the same profile, which is the A/B." )
    RT_CVAR_NOARCH( rt_smoke_autospawn, 0,      "DIAGNOSTIC: spawn a puff directly in front of the camera every this "
                                                "many TICS, with no weapon and no trigger involved. 0 = off. This "
                                                "exists so the RENDER path can be tested without the input path: if "
                                                "autospawn shows smoke and firing does not, the fault is in the "
                                                "muzzle-flash trigger, and vice versa. Pairs with rt_autoshot / "
                                                "rt_autoquit for an unattended capture." )
    RT_CVAR_NOARCH( rt_smoke_debug,     0,      "1 logs the chain: A0/gate (did extralight rise), A/trigger, B/spawn, "
                                                "C/sent and RTGL1's own D/received. 2 and 3 are SHADER probes -- 2 "
                                                "paints magenta in the froxels a puff covers, 3 paints green over the "
                                                "whole screen whenever the puff list is non-empty, with no position "
                                                "test. The pair separates `the shader cannot read the uniform` from "
                                                "`it reads it and no cell passes the sphere test`, which engine-side "
                                                "logging cannot tell apart. Legacy text follows: log the live puff count, the nearest puff's position and radius in "
                                                "metres, and the slice thickness the density was scaled by -- so `no "
                                                "smoke visible` can be told apart from `no puffs spawned`." )

    RT_CVAR( rt_volume_occlude_emis,    true,   "attenuate SCREEN EMISSION by the volumetric medium. It is added after the "
                                                "volumetric composite and outside its guard, so without this an emissive "
                                                "panel shines through fog and through smoke at full strength -- lights "
                                                "leaking out of a cloud that should hide them. It also draws a dark "
                                                "outline around anything emissive seen through smoke, because those "
                                                "pixels stay bright while everything around them is dimmed. 0 = stock." )
    RT_CVAR( rt_volume_dither,          2.f,    "how far, in FROXELS, each pixel jitters its volume sample. Stock 2 hides "
                                                "the grid in a low-contrast medium, but against dense smoke the jitter "
                                                "reaches across geometry SILHOUETTES into columns belonging to a "
                                                "different surface -- which is what draws dark outlines around things "
                                                "seen through smoke. Lower trades those outlines for more visible grid." )
    RT_CVAR( rt_volume_blur,            1.f,    "0..1 blend of a 5-tap spatial filter on the froxel volume, taken where the "
                                                "is integrated. The volume is lit at ONE sample per cell with one shadow "
                                                "ray and is the only buffer here with no spatial filter at all -- the "
                                                "surface path has A-SVGF or DLSS-RR, the volume has only a temporal blend "
                                                "that is INVALID whenever the camera moves, because the grid is "
                                                "camera-attached and its history is not reprojected. So its noise reaches "
                                                "the screen raw, which is what makes smoke boil under a flashlight. "
                                                "Blurring costs almost nothing visually because scattering is "
                                                "low-frequency; XY only, since Z is the integration axis. 0 = stock." )
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
    // Doom 64 has FOUR liquids, not one: water, nukage, sludge and blood are the
    // same 64-frame animated flat design in four palettes, and all four are the
    // near-black opaque floor flats the stylized path was written for. They run
    // the same shader and differ only in these colours, selected per primitive
    // by a 2-bit liquid id packed into the geometry instance flags.
    //
    // Every liquid gets a BODY colour (what it tends to where the flat is black)
    // and a CREST colour (what the caustic veins tend to at full mask). Crest is
    // never the body brightened: water's body is deep blue but its crests are
    // pale cyan, which is what the source art's brightest texels actually are.
    // Pale, not white — white crests read as foam or plastic.
    RT_CVAR( rt_water_liquids,          true,   "apply the stylized water surface to nukage / sludge / blood as "
                                                "well as water (D64N*, D64S*, D64B*). 0 = water only, the other "
                                                "three go back to being flat near-black floors." )
    RT_CVAR( rt_water_tint_r,           1,      "stylized water: body colour Red [0,255]" )
    RT_CVAR( rt_water_tint_g,           1,      "stylized water: body colour Green [0,255]" )
    RT_CVAR( rt_water_tint_b,          15,      "stylized water: body colour Blue [0,255]" )
    RT_CVAR( rt_water_crest_r,        140,      "stylized water: caustic vein colour Red [0,255]" )
    RT_CVAR( rt_water_crest_g,        204,      "stylized water: caustic vein colour Green [0,255]" )
    RT_CVAR( rt_water_crest_b,        255,      "stylized water: caustic vein colour Blue [0,255]" )
    RT_CVAR( rt_nukage_tint_r,          2,      "stylized nukage (D64N*): body colour Red [0,255]" )
    RT_CVAR( rt_nukage_tint_g,         15,      "stylized nukage: body colour Green [0,255]" )
    RT_CVAR( rt_nukage_tint_b,          4,      "stylized nukage: body colour Blue [0,255]" )
    RT_CVAR( rt_nukage_crest_r,       153,      "stylized nukage: caustic vein colour Red [0,255]" )
    RT_CVAR( rt_nukage_crest_g,       255,      "stylized nukage: caustic vein colour Green [0,255]" )
    RT_CVAR( rt_nukage_crest_b,       115,      "stylized nukage: caustic vein colour Blue [0,255]" )
    RT_CVAR( rt_sludge_tint_r,         15,      "stylized sludge (D64S*): body colour Red [0,255]" )
    RT_CVAR( rt_sludge_tint_g,          9,      "stylized sludge: body colour Green [0,255]" )
    RT_CVAR( rt_sludge_tint_b,          2,      "stylized sludge: body colour Blue [0,255]" )
    RT_CVAR( rt_sludge_crest_r,       255,      "stylized sludge: caustic vein colour Red [0,255]" )
    RT_CVAR( rt_sludge_crest_g,       204,      "stylized sludge: caustic vein colour Green [0,255]" )
    RT_CVAR( rt_sludge_crest_b,       115,      "stylized sludge: caustic vein colour Blue [0,255]" )
    RT_CVAR( rt_blood_tint_r,          15,      "stylized blood (D64B*): body colour Red [0,255]" )
    RT_CVAR( rt_blood_tint_g,           2,      "stylized blood: body colour Green [0,255]" )
    RT_CVAR( rt_blood_tint_b,           2,      "stylized blood: body colour Blue [0,255]" )
    RT_CVAR( rt_blood_crest_r,        255,      "stylized blood: caustic vein colour Red [0,255]" )
    RT_CVAR( rt_blood_crest_g,        115,      "stylized blood: caustic vein colour Green [0,255]" )
    RT_CVAR( rt_blood_crest_b,        102,      "stylized blood: caustic vein colour Blue [0,255]" )
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
    RT_CVAR( rt_water_caustic_rise,     64.f,   "how far ABOVE the water the caustics still reach, map units. "
                                                "Deliberately separate from rt_water_caustic_dist: real caustics "
                                                "climb only a little way up a wall, while the probe has to reach "
                                                "much further sideways to clear a pool's ledge. Sharing one "
                                                "range ran the pattern up the full height of every wall." )
    RT_CVAR( rt_water_caustic_wall,     4.0f,   "extra caustic gain on VERTICAL surfaces only. The same pattern "
                                                "is much fainter on a wall than on the pool bottom -- grazing "
                                                "angle, and the light has spread further -- so one gain tuned on "
                                                "the floor leaves the walls invisible, while raising it brightens "
                                                "the floor just as much. 1 = no boost." )
    RT_CVAR( rt_water_caustic_slant,    0.6f,   "how far the caustic probe tilts along the surface normal. 0 = "
                                                "straight down, which only finds water for surfaces standing "
                                                "directly in it; higher lets a wall look out over a pool ledge." )
    RT_CVAR( rt_water_caustic_dist,     512.f,  "how far the caustic probe reaches, in map units. The probe is "
                                                "slanted along the surface normal, so this is a slant range, not "
                                                "a pure height: 512 lights roughly the lower 400 units of a wall "
                                                "standing a ledge-width back from the pool. Larger reaches higher "
                                                "but also lets water light things it should not." )
    // NOT archived: a diagnostic left at 1 in the ini would paint every water
    // surface magenta on every later launch, and this project has lost time to
    // exactly that class of stuck cvar.
    RT_CVAR_NOARCH( rt_water_debug,     0,      "diagnostic. 1 = paint the water SURFACE magenta where the "
                                                "stylized branch runs, green where RTGL flagged it water but the "
                                                "gate rejected it, plus the caustic probe on everything else. "
                                                "2 = caustic probe ONLY -- the water surface is left showing its "
                                                "own probe result, which is the test for whether the probe can "
                                                "see water AT ALL (it fires from the water and should hit "
                                                "itself 5cm below). Probe colours: black = hit nothing, blue = "
                                                "hit non-water geometry, green = hit water. "
                                                "Mode 1 also means: no colour at all on the surface means the "
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
