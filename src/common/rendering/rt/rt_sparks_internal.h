#pragma once

// Shared internals of the impact-FX system, for the files split out of
// rt_sparks.cpp: rt_spark_surfaces.cpp, rt_impacts.cpp, rt_spark_draw.cpp.
//
// NOT a public header: nothing outside common/rendering/rt should include it,
// and nothing outside the four impact-FX files has any business in it. The
// public entry points stay in rt_internal.h beside every other RT feature's.
//
// WHY THIS EXISTS, which is the same reason rt_internal.h exists one level up.
// rt_sparks.cpp reached 4126 lines -- the largest file in this directory by
// 65%, larger than rt_main.cpp, which was itself split for being unmanageable.
// Every type and helper in it lived in a file-local anonymous namespace, and
// internal linkage is exactly what makes a file unsplittable: no second
// translation unit can see any of it. Promoting the shared surface to a NAMED
// namespace is the whole of the mechanical change.
//
// Everything lives in `namespace rtsp` rather than at global scope for the
// reason rtx exists: names like `Spark`, `QuadBatch` and `hash01` were only ever
// safe because they were file-local, and promoting them to real external symbols
// across the whole gzdoom link is asking for a collision. Each impact-FX
// translation unit says `using namespace rtsp;` once.
//
// WHAT LIVES WHERE:
//   rt_spark_surfaces.cpp  what a wall is made of, and the debris rows
//   rt_sparks.cpp          the particle pool, the hitscan spawn, the sim
//   rt_impacts.cpp         projectile impacts: the walk, marks, arcs, embers
//   rt_spark_draw.cpp      all geometry building and light upload

#include "rt_internal.h"

#include <array>
#include <vector>

namespace rtsp
{

// ---------------------------------------------------------------------------
// THE PALETTES, and every one of them was sampled from the game's own art
// rather than invented. Shared here because the draw and the impact spawner
// both index them, and because one place for all of them makes the story
// legible: a particle that indexes the palette of the sprite playing at the
// same point, at the same moment, matches it by construction and cannot
// drift out of agreement later. That is the single decision that made these
// colours stop needing tuning.
//
// constexpr at namespace scope means each translation unit gets its own
// copy. They are a few dozen bytes each; that is not worth an accessor.
// ---------------------------------------------------------------------------

// PUFF's OWN PALETTE, sampled from the PLTE of PUFFA0..PUFFF0 in D64RTR_v15.WAD.
//
// A spark indexes this by age, so it cools through exactly the colours of the
// sprite it fires alongside -- the two match by construction rather than by eye,
// which is the whole reason the puff art was the starting point for this work.
// Hot pale yellow at birth, dark brown ember at death.
constexpr uint32_t RT_SPARK_RAMP[] = {
    0xF8F8B0, 0xB8A868, 0x786040, 0x604820, 0x482818, 0x301808, 0x180800,
};
constexpr int RT_SPARK_RAMP_N = int( std::size( RT_SPARK_RAMP ) );

// THE ARC RAMPS, and they are sampled the same way PUFF's was -- read out of the
// PLTE of the projectile's OWN death sprite in D64RTR_v15.WAD, not invented.
//
// That is not tidiness for its own sake. It is the single decision that made the
// spark colours stop needing tuning: a particle that indexes the palette of the
// sprite playing at the same point, at the same moment, matches it by
// construction and cannot drift out of agreement later. Every entry below is a
// colour the game already puts on screen at a plasma or BFG impact.
//
// PLSE A0-F0, the player plasma ball's Death frames -- HUE-SHIFTED TOWARD BLUE,
// and this is the one ramp on this page that is not straight off the sprite.
//
// The sprite's bright end is genuinely cyan: its top three entries are #52F7FF,
// #18D6F7 and #009CDE, which have as much green in them as blue. On a 16-pixel
// sprite playing for a fifth of a second that reads as "hot plasma"; drawn as
// hairlines held on a wall it reads as TEAL, which was the note from play. The
// difference is exposure -- the same colour sat in front of the eye for half a
// second is judged as a colour rather than as a flash.
//
// So the dark half below is the sprite's own (#00397B down is already blue and
// carries the match), and the bright half is pulled off the green axis. Recorded
// as a deliberate art call rather than left to look like sampling drift: the
// "match the source palette by construction" rule is still the default, this is
// a considered exception to it, and the numbers to go back to are in the comment
// above if the teal is ever wanted again.
constexpr uint32_t RT_ARC_PLASMA_RAMP[] = {
    0x8CB4FF, 0x4C7CFF, 0x2A55F0, 0x1436D2, 0x0A2AA8, 0x00397B, 0x002152, 0x001029,
};
constexpr int RT_ARC_PLASMA_RAMP_N = int( std::size( RT_ARC_PLASMA_RAMP ) );

// BFE2 A0-F0, the BFG's own explosion sprite. White core, then the green the
// weapon is entirely known for. Reaches pure white at the top where the plasma
// ramp reaches cyan -- BFE2 really does carry FFFFFF and PLSE does not, so the
// two impacts differ in more than hue.
constexpr uint32_t RT_ARC_BFG_RAMP[] = {
    0xFFFFFF, 0x31FF39, 0x10E710, 0x10CE00, 0x08A500, 0x087B00, 0x004200, 0x002100,
};
constexpr int RT_ARC_BFG_RAMP_N = int( std::size( RT_ARC_BFG_RAMP ) );

// APLS A0-H0, the arachnotron's plasma. A GENUINELY DIFFERENT PALETTE from the
// player's, and worth its own row for one table's cost: where PLSE is saturated
// cyan-into-navy, APLS is a desaturated indigo that never leaves the blue-violet
// family. Giving the monster the player's ramp would have been the same mistake
// as handing three different torches one flame colour.
constexpr uint32_t RT_ARC_ARACH_RAMP[] = {
    0xF8F8F8, 0xD8D8F8, 0xB0B0F0, 0x9090E8, 0x7070E0, 0x5050D8, 0x3838A8, 0x282870, 0x101030,
};
constexpr int RT_ARC_ARACH_RAMP_N = int( std::size( RT_ARC_ARACH_RAMP ) );

// ROCKET EMBERS, from MISL B0-F0 -- the rocket explosion's own frames, sampled
// the same way PUFF's and PLSE's were. This one needed no hue correction at all:
// the sprite runs pale yellow through orange to a dark brown-black, which is
// exactly what a cooling ember does, so the artist's ramp IS the physics here.
//
// Ten entries rather than seven, because an ember is watched for several seconds
// where a spark is gone in a fraction of one. Banding that no one can see on a
// 5-frame spark is very visible on something lying still on the floor.
constexpr uint32_t RT_EMBER_RAMP[] = {
    0xFFFFAD, 0xFFD64A, 0xFFB542, 0xFF9C31, 0xD67321,
    0xB56321, 0x9C5218, 0x6B3910, 0x4A2908, 0x311808,
};
constexpr int RT_EMBER_RAMP_N = int( std::size( RT_EMBER_RAMP ) );

// ---------------------------------------------------------------------------
// THE BARREL, and the first thing to say about it is that the obvious guess was
// wrong. An exploding-barrel chunk was designed as "green metal with a rust
// band" from memory of the PC barrel; Doom 64's BAR1A0 is nothing of the sort.
//
// Dumped from D64RTR_v15.WAD, BAR1A0 is 48x50 with exactly FOURTEEN distinct
// opaque colours, and they form one clean value ramp from near-black to a cold
// blue-grey highlight -- 080810, 101010, 101018, 181821, 212121, 212129,
// 292931, 393939, 4A4A4A, 5A5A5A, 63636B, 73737B, 8C8C94, 9CADC6. There is no
// green and no rust anywhere in it. The barrel is DARK, COLD, GREY-BLUE METAL,
// and a chunk of it painted green would have read as belonging to a different
// game -- which is precisely why the sprite was dumped before a colour was
// chosen rather than after the result looked wrong.
//
// TWO TABLES, because a shard needs two different things from the art:
//
// SHADES is the PICK palette. A shard chooses one entry and keeps it for life,
// so a burst comes out assorted the way torn plate is -- some pieces catching
// the light, some nearly black. Spread across the sprite's value range on
// purpose rather than weighted by pixel count: weighting by frequency would
// pick the three darkest entries almost every time (they are two thirds of the
// sprite) and the burst would come out uniformly black.
//
// The 9CADC6 highlight is deliberately NOT here. In the sprite it is a specular
// rim -- a lighting result, not a material -- and a whole chunk painted with it
// would read as a piece of blue plastic. The path tracer supplies the highlight
// itself; see docs' "keep the art, add shading on top".
constexpr uint32_t RT_BARREL_SHADES[] = {
    0x101018, 0x1D1D25, 0x292931, 0x393939, 0x4A4A4A, 0x5A5A5A, 0x63636B, 0x73737B,
};
constexpr int RT_BARREL_SHADES_N = int( std::size( RT_BARREL_SHADES ) );

// RAMP is the AGE curve, in the same role the other ramps play for debris: the
// draw reads how much its first entry has darkened by and multiplies the
// shard's own colour down by that fraction. So a shard keeps its identity and
// merely dulls, rather than being recoloured into something the sprite never
// contained. The sprite's own value ramp, coarsened.
constexpr uint32_t RT_BARREL_RAMP[] = {
    0x73737B, 0x5A5A5A, 0x4A4A4A, 0x393939, 0x292931, 0x1D1D25, 0x101018, 0x080810,
};
constexpr int RT_BARREL_RAMP_N = int( std::size( RT_BARREL_RAMP ) );

// ---------------------------------------------------------------------------
// Surface classification -- rt_spark_surfaces.cpp
// ---------------------------------------------------------------------------

// What a wall is MADE OF, as labelled by the PBR material pass in
// tools/_material_labels and consumed via rt/data/spark_surfaces.txt.
enum class SurfKind : uint8_t
{
    Metal,
    Concrete,
    Wood,
    Dirt,
    Flesh,
    Fluid,
    Other,
    // NOT A WALL, and it is here anyway on purpose. Every other entry is a class
    // a texture can be LABELLED with; Barrel is a class a fragment can BE. It
    // rides this enum because DebrisProfile is indexed by it and the profile
    // system -- one row of multipliers stating only how a material differs -- is
    // exactly what a barrel chunk needs; a parallel table would have been the
    // same fifteen fields under a different name.
    //
    // Nothing can be labelled into it: ParseSurfKind never returns Barrel, so no
    // line of rt/data/spark_surfaces.txt can turn a wall into one however it is
    // spelled. APPENDED rather than inserted -- RT_DEBRIS_PROFILES is positional
    // and Other's index is load-bearing as the out-of-range fallback.
    Barrel,
    COUNT,
};

SurfKind    ParseSurfKind( const FString& s );
const char* SurfKindName( SurfKind k );
bool        SurfThrowsDebris( SurfKind k );

// The average colour of a texture, cached. 0xRRGGBB; 0x808080 is the FALLBACK
// and worth recognising, because a grey chip and a failed sample look alike.
uint32_t AverageTextureColor( FTextureID tex );

// What is this surface? Answers Other for anything unlabelled, which is the
// opt-in rule the whole classification rests on.
SurfKind SurfaceKindOf( FTextureID tex, FString* outName, bool* outListed );

void LoadSparkSurfaces();

// Whether the table was found at all, and where it was looked for -- so
// "nothing is classified" can be told from "the file is missing".
extern bool    s_surfacesFound;
extern FString s_surfacesPath;

// PER-CLASS DEBRIS, as MULTIPLIERS on the rt_spark_debris_* cvars. A row states
// only how its class DIFFERS; see RT_DEBRIS_PROFILES for the reasoning.
struct DebrisProfile
{
    const uint32_t* ramp;
    int             rampN;
    const char*     texture;
    float           count;
    float           size;
    float           life;
    float           speed;
    float           gravity;
    float           bounce;
    float           friction;
    float           aspectLo; // ABSOLUTE, not a multiplier: shape is the class
    float           aspectHi;
    float           spin;
    float           tint;   // x rt_spark_debris_tint
    float           albedo; // x rt_spark_debris_albedo
};

const DebrisProfile& ProfileFor( SurfKind k );

// ---------------------------------------------------------------------------
// Projectile impact flavours -- rt_impacts.cpp
//
// Declared up here rather than in the impacts section below because SparkFlash
// carries one: a flash resolves its colour at SPAWN, since the particles that
// produced it may all be dead by the time the light is uploaded.
// ---------------------------------------------------------------------------

enum class ArcFlavor : uint8_t
{
    Plasma, // the player's plasma rifle
    Arach,  // the arachnotron's, which has its own palette
    BFG,
    COUNT,
};

// What an impact PRODUCES. The projectile walk is shared -- same MF_MISSILE
// edge, same surface probe -- and only the spawn differs.
enum class ImpactFx : uint8_t
{
    Arc,   // electric filigree on the wall: plasma, BFG
    Ember, // a scorch with a few coals still glowing in it: the rocket
};

struct ArcStyle
{
    const uint32_t* ramp;
    int             rampN;
    // ONE multiplier on the mark's size and duration -- width, reach and life
    // together. The cvars are authored for the PLASMA RIFLE; a flavour states
    // only how it differs.
    float           scale;
    const char*     name; // rt_arc_debug only
};

const ArcStyle& ArcStyleFor( ArcFlavor f );

// ---------------------------------------------------------------------------
// The particle pool -- rt_sparks.cpp
// ---------------------------------------------------------------------------

enum class SparkKind : uint8_t
{
    Spark,  // hot, additive, casts a flash
    Debris, // dull, OPAQUE ray-traced geometry, casts nothing
    // A BARREL CHUNK, and it is a third kind rather than big debris because the
    // difference is structural, not a size multiplier.
    //
    // Debris is a CAMERA-FACING billboard: the quad is built from the screen
    // basis and only spun within it. That is invisible at 2 cm -- a chip has no
    // silhouette to give it away -- and it falls apart the moment the piece is
    // 25 cm across, because a plate that keeps turning to face you as you walk
    // round it reads as a sprite rather than as a thing lying on the floor.
    //
    // A shard is therefore WORLD-ORIENTED and genuinely curved: a section of the
    // barrel's own cylinder, torn at both ends, with real normals across the
    // bend. That is what makes it read as a piece of the barrel instead of a
    // large square particle. It shares the pool, the sim and the debris colour
    // path with Debris and diverges only in geometry and in how its colour is
    // resolved.
    Shard,
};

// Anything that is not a hot spark: opaque traced geometry, thrown by gravity,
// bounced, settled, coloured from art rather than from the heat ramp. Written
// out because `kind == Debris` was the test in five places and every one of them
// meant "not a spark" -- Shard silently failing four of them is the exact shape
// of bug this file's history is made of.
inline bool IsChunk( SparkKind k )
{
    return k != SparkKind::Spark;
}

// Pool ceilings. The cvars bound how much is used, so raising a cvar past its
// ceiling is clamped rather than corrupting.
constexpr uint32_t RT_SPARK_HARDMAX   = 4096;
constexpr uint32_t RT_SPARK_FLASH_MAX = 64;

struct Spark
{
    FVector3  pos;     // METRES
    FVector3  vel;     // metres / second
    float     age;
    float     life;
    float     size;    // metres, edge of the square
    sector_t* sec;     // cached, so "did this step leave its sector" is free
    bool      settled; // came to rest; no longer integrated
    SparkKind kind;
    SurfKind  surf;
    float     phase;
    float     spin;
    float     aspect;
    uint32_t  baseRgb; // debris: the hit texture's average colour
    FVector3  nrm;     // the surface it came off; debris shades with it
    uint32_t  sid;     // IDENTITY, and the only thing a glow light's id may use
};

// The per-IMPACT flash. One per impact, never one per particle.
struct SparkFlash
{
    FVector3  pos; // METRES
    float     age;
    float     life;
    bool      isArc;
    ArcFlavor arc; // unread when isArc is false
};

extern std::array< Spark, RT_SPARK_HARDMAX >        s_sparks;
extern std::array< SparkFlash, RT_SPARK_FLASH_MAX > s_flashes;

// THE POOL IS SHARED, SO THE MASTER GATE IS THE UNION OF THE MASTERS. Sim, draw
// and lights all test this; the SPAWN sites test their own cvar, so each effect
// can still be judged with the others out of the way.
bool SparkSystemOn();

// A slot in the shared pool, or nullptr if the pool is configured to zero.
// EVICTS WITHIN THE KIND: sparks live ~5 s, debris ~20 s and shards longer
// still, so a plain oldest-out rule would evict the long-lived population every
// time and their lifetimes would be numbers that never happened.
Spark* AllocSpark( SparkKind kind );

// Monotonic, never reused. A particle's identity: its light's uniqueID and the
// seed of every stable hash its geometry uses.
uint32_t NextSparkSid();

// Tier 2 of the collision: only ever called when a step LEFT its sector.
bool SparkHitWall( Spark& sp, const FVector3& from, const FVector3& to, float bounce, float fric );

// ---------------------------------------------------------------------------
// Impact marks -- rt_impacts.cpp
//
// A mark is a thing burnt ONTO a surface: a scorch, optionally carrying an
// electric filigree (plasma, BFG) or a few embers (rocket). Deliberately not
// particles -- that was built twice and was wrong twice, and the note at the
// top of rt_impacts.cpp says why.
// ---------------------------------------------------------------------------

constexpr int      RT_ARC_MAX_BRANCH = 24;
constexpr int      RT_ARC_MAX_SEG    = 16;
// GREW AGAIN WITH "FOREVER". Once rt_arc_burn_life defaults to 0 the only thing
// that ever removes a scorch is eviction, so this is no longer headroom -- it IS
// the answer to "how far back does the wall damage go", and a pool that fills in
// a few seconds of rocket fire makes permanent marks look like fading ones.
// An ArcMark is ~80 bytes; 192 of them is nothing.
constexpr uint32_t RT_ARC_MARK_MAX   = 192;

struct ArcMark
{
    FVector3  at;  // METRES, on the surface
    FVector3  nrm; // unit surface normal
    FVector3  tan; // the in-plane basis, FIXED AT SPAWN
    FVector3  bit;
    // ONE CLOCK, TWO LIFETIMES. `age` runs to `life`, the SCORCH's. The filigree
    // reads the same age against `arcLife` and stops drawing past it.
    float     age;
    float     life;
    float     arcLife;
    uint32_t  seed; // the whole filigree is re-derived from this every frame
    uint32_t  uid;  // MONOTONIC, never reused: every light id derives from it
    ArcFlavor flavor;
    bool      arcs;
    float     burnScale; // multiplier on rt_arc_burn_radius
    ImpactFx  fx;
    float     nextSmoke; // ember marks only
};

extern std::array< ArcMark, RT_ARC_MARK_MAX > s_arcs;
extern uint32_t                               s_arcCount;

void RT_ClearArcMarks();
void AgeArcMarks( float dt );

// Burn one mark onto a surface. `at` is METRES and lies ON the surface;
// `normal` is a unit surface normal. The DEFAULTS LIVE HERE, not on the
// definition in rt_impacts.cpp -- a default argument may be stated once per
// scope, and once a second translation unit calls this it has to be the
// declaration that carries them.
void SpawnArcMark( const FVector3& at,
                   const FVector3& normal,
                   ArcFlavor       flavor,
                   bool            withArcs  = true,
                   float           burnScale = 1.f,
                   ImpactFx        fx        = ImpactFx::Arc );

// Where a mark's embers sit, in world metres. SHARED by the draw and the smoke
// so the two agree by construction rather than by both being handed the same
// numbers -- the arcs' creepers had exactly that split once.
FVector3 EmberPos( const ArcMark& m, int i, float burnRad );

// How many projectiles are being tracked right now, for the `sparks` ladder.
size_t TrackedProjectileCount();

// ---------------------------------------------------------------------------
// Barrel destruction -- rt_barrel.cpp
//
// Rides the projectile walk rather than opening a second thinker iteration:
// RT_UpdateProjectileImpacts is already visiting every actor once a tic, and a
// barrel is just another actor in that sweep. It does NOT ride rt_smoke's walk,
// which is where the barrel's smoke burst lives -- that would have made barrel
// fire and debris silently depend on rt_smoke and rt_smoke_barrel, the
// three-deep gating the impact plan rejects.
// ---------------------------------------------------------------------------

// One actor, one tic. Detects the rising edge of the explosion and fires
// everything off it. Safe to call for any actor; it filters.
void BarrelWalkActor( AActor* mo, int tic );

// Drop marks for barrels not seen this tic. Called once after the walk.
void BarrelSweep( int tic );

// Drop every tracked barrel. On a level change or a load, every AActor* held
// belongs to the old level and must not be touched again.
void BarrelForgetAll();

// Throw a burst of barrel plate from a point. `at` is METRES. Public because
// the `barrel_here` lab command wants it without an actor.
void SpawnBarrelShards( const FVector3& at, const FVector3& up );

// ---------------------------------------------------------------------------
// Geometry batching and lights -- rt_spark_draw.cpp
// ---------------------------------------------------------------------------

struct QuadBatch
{
    std::vector< RgPrimitiveVertex > verts;
    std::vector< uint32_t >          idx;
};

// Light candidates collected DURING THE DRAW, from the geometry that was
// actually emitted, and drained by the light pass. Recomputing them in a second
// pass would be the same work twice and two places for the churn to disagree,
// which shows up as lights sitting where no geometry is.
struct ArcLightCand
{
    FVector3 pos;
    float    r, g, b;
    float    k; // 0..1, the source's own fade x crackle x pulse
    // CARRIED PER CANDIDATE: arcs and embers share this list and the budget but
    // are not the same kind of light, and one shared cvar meant tuning one
    // silently moved the other.
    float    intensity;
    float    radius; // emitter SIZE in metres; reach comes from intensity
    uint64_t id;
};

extern std::vector< ArcLightCand > s_arcLights;

// A batched primitive. `additive` picks the TRANSLUCENT+emissive path, which
// RTGL1 turns into a real additive blend.
void UploadBatch( const QuadBatch&  b,
                  uint64_t          meshId,
                  bool              additive,
                  const char*       texName,
                  RgColor4DPacked32 primColor );

// ONE PRIMITIVE PER BLOB. Batching decals into one primitive is what produced
// the AO "lines"; the shipping sprite AO uploads one per actor.
void UploadAoBlob( const QuadBatch& b, uint64_t meshId );

// The mesh-ID ranges. RTGL1 keeps ONE upload per id and it is the NEW primitive
// that loses, so these must not overlap. Bits 60-62 are beyond the ceiling on a
// Windows x64 user-space pointer, so none can collide with an actor-derived id.
constexpr uint64_t RT_SPARK_MESH_ID     = 0x1000000000000000ull;
constexpr uint64_t RT_DEBRIS_MESH_ID    = 0x1000000000000001ull;
constexpr uint64_t RT_DEBRIS_AO_MESH_ID = 0x1000000000000100ull;
constexpr uint64_t RT_ARC_BURN_MESH_ID  = 0x1000000000001000ull;

// The shared additive batch. Sparks, arc filigree and embers all land here:
// same blend, same untextured white material, same mesh id, so a second batch
// would be a second uploadMeshPrimitive for no difference in state.
extern QuadBatch s_batchSpark;

// ---------------------------------------------------------------------------
// Private RNG and hashing. NEVER M_Random: the gameplay RNG is part of the
// simulation, and drawing from it in the renderer desyncs demos and netgames
// invisibly -- and the desync would not show up in single-player testing.
// ---------------------------------------------------------------------------
float rnd11();
float rnd01();
float hash01( uint32_t x );
float snap( float v, float grid );

// NOTE: rt_pi() and to_rad() are NOT here. They already live in `rtx`
// (rt_internal.h), and every impact-FX file says `using namespace rtx;` as well
// as `using namespace rtsp;` -- so a second pair in this namespace is not a
// redefinition, it is an AMBIGUOUS OVERLOAD at every call site. Twenty of them,
// which is how it announced itself.

// ---------------------------------------------------------------------------
// Diagnostics, shared so every file tallies into ONE ladder. The whole point of
// it is that "nothing spawned" and "spawned but not drawn" are identical on
// screen, and this project has lost sessions to not telling them apart.
// ---------------------------------------------------------------------------
extern int s_dbgHits;
extern int s_dbgRejected;
extern int s_dbgSpawned;
extern int s_dbgQuads;
extern int s_dbgLights;
extern int s_dbgTraces;
extern int s_dbgAo;
extern int s_dbgEmberSmoke;
extern int s_dbgMetal;
extern int s_dbgConcrete;
extern int s_dbgOther;
extern int s_dbgUnlisted;

void NoteUnlisted( const FString& name );
void SparkReportUnlisted();

} // namespace rtsp
