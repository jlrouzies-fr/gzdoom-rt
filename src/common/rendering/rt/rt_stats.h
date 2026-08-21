#pragma once

// Doom64-RT: frame-cost instrumentation for the RT path.
//
// WHY THIS EXISTS. gzdoom's own `stat rendertimes` (hw_clock.cpp) stops at the
// draw call: it measures the BSP walk and the wall/flat/sprite setup, and then
// everything the RT renderer actually does -- every rgUploadMeshPrimitive, every
// rgUploadLight, and the whole of rgDrawFrame -- happens inside a DLL that has
// no timers of its own. RTGL1 contains no vkCmdWriteTimestamp and no query pool,
// so before this file there was no way to say whether a frame was spent in the
// BSP, in our light walks, or inside RTGL1. "GPU is not at 100%" was as precise
// as the diagnosis could get.
//
// WHAT IT MEASURES. Wall-clock CPU on the main thread, split into the four
// places the frame can go, plus the two counts that decide most of that cost:
// how many primitives and how many lights we hand RTGL1 each frame.
//
// HOW THE UPLOAD TIMERS WORK -- and why there is no call-site clutter. There are
// 36 call sites for the two upload entry points across nine files. Rather than
// wrap each one, RT_InstallStatThunks() swaps the two function pointers inside
// the RgInterface struct for counting thunks that forward to the originals. One
// install, total coverage, and a call site added later is instrumented for free.
//
// AVERAGES CANNOT SEE A STUTTER, which is the one thing this file learned the
// hard way. Every number below is a mean: the FRAME line is frames over elapsed
// nanoseconds across half a second, rt_stat_every samples it once a second, and
// the phase columns are the same frame's brackets. A single 24 ms frame inside
// a 500 ms window moves the reported average by about 0.02 ms. On 2026-08-19 a
// MAP02 run taken specifically to explain two felt hitches reported a flat
// 8.7 ms / 115 fps end to end, and the two samples that looked like spikes were
// drawframe up 7x with `start` down by the same amount and the total unchanged
// -- the CPU blocking inside rgDrawFrame rather than at the N-2 fence, which is
// what being GPU-bound looks like and not a hitch at all.
//
// So the stat also carries the OUTLIER: `worst` is the longest single frame of
// the same half-second window, and rt_stat_spike prints one line per frame over
// a threshold with the phase split, the counts, and the position and sector to
// walk back to. Read `worst` first. If it is near the average, there was no
// stutter to explain, whatever it felt like.
//
// COST WHEN IDLE. glcycle_t is gzdoom's own rdtsc counter and compiles to a
// no-op unless a `stat` is active -- glcycle_t::Clock() tests a static bool
// (stats.h:273) that hw_clock.cpp's checkBenchActive() sets only while `stat
// rendertimes` or `stat rt` is on the screen. So the thunks cost one indirect
// call per upload when nobody is looking. The counters are plain ints.

#include "stats.h"

// The four phases of an RT frame, in call order.
//   RTStartFrame  -- rgStartFrame: swapchain acquire, scene import, the N-2 fence
//   RTLightGen    -- OUR light walks (rt_main.cpp's RT_Upload*Lights block).
//   RTFx          -- OUR particle/effect systems: smoke, projectile impacts,
//                    sparks, arcs, dust. Split from RTLightGen because the two
//                    grow for different reasons and have different fixes: the
//                    light walks scale with the LEVEL, the effects scale with
//                    what is lying on the floor after a firefight.
//   RTUploadPrim  -- summed rgUploadMeshPrimitive, via the thunk. Happens during
//                    the drawlist walk, so it overlaps none of the others.
//   RTUploadLight -- summed rgUploadLight, via the thunk. Mostly INSIDE
//                    RTLightGen, which is why it is reported as a subset of it
//                    rather than added into the total twice.
//   RTDrawFrame   -- rgDrawFrame: BLAS/TLAS build, every GPU pass, present
extern glcycle_t RTStartFrame, RTLightGen, RTFx, RTUploadPrim, RTUploadLight, RTDrawFrame;

// NOT an RT phase, and here because of what the phases could not explain. On
// 2026-08-19 every logged spike on a loaded MAP02 save had 30-78% of its frame
// outside all five brackets above -- the RT side held flat at 9-11 ms while the
// frame went to 16-48. Renderer instrumentation cannot name time the renderer
// does not spend, and the largest thing in a gzdoom frame that is not the
// renderer is the playsim.
//
// RTPlaysim brackets D_DoomLoop's TryRunTics, so it covers the tic itself and
// the GC step inside it. It is the difference between "the frame was slow" and
// "the frame was slow and it was not us". A tic runs 35 times a second, so at
// 115 fps only about one frame in three carries one -- which is also why a
// heavy tic shows up as periodic hitching rather than a lower frame rate.
extern glcycle_t RTPlaysim;

// Also not an RT phase. RTPlaysim answered half the question and raised the
// other half: on 2026-08-20 four logged stutters had start=0.05, the five RT
// phases summing to ~2 ms, playsim at ~0.15 ms -- and frames of 30, 34, 51 and
// 61 ms. Ninety-five per cent of those frames was in neither the renderer nor
// the playsim, and there was no counter anywhere that could say what it was.
//
// RTDisplay brackets D_Display, which is everything the RT phases sit inside
// plus gzdoom's own scene walk, the 2D pass, the HUD and any texture created on
// demand during the drawlist. One counter splits the unexplained time in two:
//
//   display large, RT phases small -> gzdoom's own render or a texture upload
//   display small too              -> outside both, i.e. events, sound, the
//                                     swapchain, or the OS taking the core away
extern glcycle_t RTDisplay;

// The completed value of the above, for the frame that just ended. It cannot be
// read off the glcycle_t directly: RT_StatsNewFrame -- which is where every
// counter is reset and where the spike is reported -- runs INSIDE D_Display, so
// at that moment RTDisplay's clock is still running and its accumulator has just
// been zeroed under it. That produced display=-0.467, a negative millisecond
// count, on the first run that printed it. d_main.cpp stores the finished value
// here after Unclock, and the spike line reads this instead.
extern double rt_display_ms;

// TEXTURE UPLOAD, thunked like the other two entry points. This is the third
// swappable pointer in RgInterface and it was the missing one: RTHardwareTexture's
// constructor calls rgProvideOriginalTexture, and gzdoom creates a hardware
// texture LAZILY -- the first time a texture is actually needed to draw. That
// happens in the middle of the drawlist walk, inside D_Display, and outside every
// other bracket in this file.
//
// Which is exactly the shape of the 2026-08-20 in-play stutters: display was
// within 0.2 ms of the whole frame, the RT phases summed to 13-16 ms of it, bsp
// was 0.05, and 38-63 ms had nowhere to go. Walking into a new area is precisely
// when unseen textures are first drawn, so "traversal stutter" is the literal
// description rather than an analogy.
extern glcycle_t RTTexUpload;
extern int       rt_tex_uploaded;

// The single most expensive upload of the frame, by name. The count alone was
// ambiguous in the worst way: the 2026-08-20 log showed textures=1 costing
// 11-60 ms, repeatedly, across 250+ tics in one room. One texture is not a new
// area being paged in -- it is the SAME work happening again and again, and only
// the name can tell those apart. If one name recurs, gzdoom is destroying and
// recreating that RTHardwareTexture (see the "HACKHACK: why is this called only
// on Release? (and destroying actually used textures)" note on its destructor in
// rt_buffers.h) and every recreation re-runs the whole override lookup.
extern double    rt_tex_slowest_ms;
extern char      rt_tex_slowest_name[ 64 ];

// TEXTURE UPLOAD, thunked like the other two entry points. This is the third
// swappable pointer in RgInterface and it was the missing one: RTHardwareTexture's
// constructor calls rgProvideOriginalTexture, and gzdoom creates a hardware
// texture LAZILY -- the first time a texture is actually needed to draw. That
// happens in the middle of the drawlist walk, inside D_Display, and outside every
// other bracket in this file.
//
// Which is exactly the shape of the 2026-08-20 in-play stutters: display was
// within 0.2 ms of the whole frame, the RT phases summed to 13-16 ms of it, bsp
// was 0.05, and 38-63 ms had nowhere to go. Walking into a new area is precisely
// when unseen textures are first drawn, so "traversal stutter" is the literal
// description rather than an analogy.
extern glcycle_t RTTexUpload;
extern int       rt_tex_uploaded;

// Per-frame counts. rt_prims_uploaded is the number that decides cost centre #1
// (one BLAS per primitive, rebuilt every frame); rt_lights_uploaded is the one
// that decides #4 -- and it is the ONLY way to see RTGL1's 4096-light array
// overflowing, because LightManager::AddInternal drops the overflow silently in
// a release build (an assert(0) then return).
extern int rt_prims_uploaded;
extern int rt_lights_uploaded;
extern int rt_prims_failed;

// Peak since the last `rt_stat_reset`, so a spike that lasts one frame in a
// firefight is still visible on a stat line sampled once a second.
extern int rt_prims_peak;
extern int rt_lights_peak;

// Called once per frame from RT_BeginFrame, before rgStartFrame.
void RT_StatsNewFrame();

// Called at the end of RT_DrawFrame. Honours rt_stat_every: prints the same
// numbers the stat shows into the console, so an unattended -timedemo run
// leaves a log with measurements in it.
void RT_StatsPeriodicDump();

// Called once, after rgLoadLibraryAndCreate has filled the RgInterface.
void RT_InstallStatThunks();

// Options -> Quality (rt_quality.cpp). Applies the archived preset once the
// level exists -- see the note at its definition for why the cvar's own handler
// cannot do it. Declared here rather than in a header of its own because this is
// the only other thing RT_BeginFrame calls for bookkeeping.
void RT_ApplyQualityPresetOnce();
