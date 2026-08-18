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
// COST WHEN IDLE. glcycle_t is gzdoom's own rdtsc counter and compiles to a
// no-op unless a `stat` is active -- glcycle_t::Clock() tests a static bool
// (stats.h:273) that hw_clock.cpp's checkBenchActive() sets only while `stat
// rendertimes` or `stat rt` is on the screen. So the thunks cost one indirect
// call per upload when nobody is looking. The counters are plain ints.

#include "stats.h"

// The four phases of an RT frame, in call order.
//   RTStartFrame  -- rgStartFrame: swapchain acquire, scene import, the N-2 fence
//   RTLightGen    -- OUR light walks (rt_main.cpp's RT_Upload*Lights block).
//                    This is the one that is entirely Doom64-RT's own code.
//   RTUploadPrim  -- summed rgUploadMeshPrimitive, via the thunk. Happens during
//                    the drawlist walk, so it overlaps none of the others.
//   RTUploadLight -- summed rgUploadLight, via the thunk. Mostly INSIDE
//                    RTLightGen, which is why it is reported as a subset of it
//                    rather than added into the total twice.
//   RTDrawFrame   -- rgDrawFrame: BLAS/TLAS build, every GPU pass, present
extern glcycle_t RTStartFrame, RTLightGen, RTUploadPrim, RTUploadLight, RTDrawFrame;

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
