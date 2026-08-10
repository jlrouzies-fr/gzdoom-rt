// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2003-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//

#include "doomtype.h"
#include "g_level.h"
#include "filesystem.h"
#include "r_state.h"
#include "r_utility.h"
#include "g_levellocals.h"
#include "hw_skydome.h"
#include "hwrenderer/scene/hw_portal.h"
#include "hw_renderstate.h"
#include "skyboxtexture.h"

#if HAVE_RT
#include "rt/rt_state.h"
#include "texturemanager.h"
#include "hw_material.h"
#include "v_video.h"
#include "flatvertices.h"
#include "bitmap.h"   // FBitmap, for the CPU cloud-alpha sampling below
#include <cmath>

namespace cvar
{
EXTERN_CVAR( Bool,  rt_sun )
EXTERN_CVAR( Float, rt_sun_a )
EXTERN_CVAR( Float, rt_sun_b )
EXTERN_CVAR( Bool,  rt_moon_geo )
EXTERN_CVAR( Float, rt_moon_geo_size )

EXTERN_CVAR( Bool,  rt_clouds )
EXTERN_CVAR( Int,   rt_clouds_shells )
EXTERN_CVAR( Float, rt_clouds_horizon )
EXTERN_CVAR( Float, rt_clouds_curve )
EXTERN_CVAR( Float, rt_clouds_thick )
EXTERN_CVAR( Float, rt_clouds_tiles )
EXTERN_CVAR( Float, rt_clouds_alpha )
EXTERN_CVAR( Float, rt_clouds_dark )
EXTERN_CVAR( Float, rt_clouds_wind )
EXTERN_CVAR( Float, rt_clouds_wind_dir )
EXTERN_CVAR( Float, rt_clouds_shear )
EXTERN_CVAR( Float, rt_clouds_occlude )
EXTERN_CVAR( Float, rt_clouds_transmit )
EXTERN_CVAR( Color, rt_clouds_tint )
EXTERN_CVAR( Float, rt_clouds_flash )

EXTERN_CVAR( Bool,  rt_lightning )
EXTERN_CVAR( Bool,  rt_lightning_bolt )
EXTERN_CVAR( Float, rt_lightning_bolt_size )
}

// rt_main.cpp. Declared at the call site rather than in a header, the same way
// g_level.cpp declares RT_OnLevelLoad -- see the storm comment there.
extern float RT_LightningFlashLevel();
extern bool  RT_LightningAim( float* azimuth, float* altitude, int* variant );

//-----------------------------------------------------------------------------
//
// Doom64-RT: draw the moon as GEOMETRY, on the sky, at the light's own bearing.
//
// The moon used to be painted into the sky texture, and everything awkward about
// it came from that. A Doom sky dome spans only 60 degrees of altitude
// (maxSideAngle, hw_skydome.cpp); above that Doom draws a flat CAP filled with
// one averaged colour, because it has no 3D skybox. So a painted moon could not
// be placed overhead at all, got sliced by the cap boundary when pushed near it,
// and needed four calibration cvars to line up with its own shafts even lower
// down.
//
// A quad drawn after the dome has none of those problems. It is placed directly
// along the rt_sun direction, so the disc is where the light comes from BY
// CONSTRUCTION -- no tracking, no sign to guess, no altitude ceiling. It draws
// over the cap because it is geometry, not texture.
//
// Coordinates: the sky is rendered with the viewpoint at the origin
// (di->SetupView(state, 0,0,0, ...)), and vertices here are (doom_x, doom_z,
// doom_y) -- height in the middle slot, doom Y last and NOT negated.
//
// rt_sun_b is the moon's own compass bearing (rt_main negates it to get the
// travel direction) and rt_sun_a its altitude, so the direction TO the moon is
// (sin t cos b, cos t, sin t sin b) with t = 90 - altitude.
//
//-----------------------------------------------------------------------------
static void RT_DrawSkyQuad(FRenderState& state, FSkyVertexBuffer* skyBuffer,
                           FGameTexture* tex, double aziDeg, double altDeg,
                           float heightDeg, float alpha)
{
	const double DEG = M_PI / 180.0;
	const double alt = clamp<double>(altDeg, -89.9, 89.9);
	const double azi = aziDeg * DEG;
	const double t   = (90.0 - alt) * DEG;

	// Vertex order is (doom_x, doom_z, doom_y) -- see hw_decal.cpp, which feeds
	// Set(dv.x, dv.z, dv.y), and the portal caps, which pass a doom Y straight
	// into the third slot. The third component is NOT negated. Getting that wrong
	// mirrors the quad east-west, which looks exactly like a misaligned disc:
	// moon on the left while the light arrives from the right.
	const FVector3 dir(
		float(std::sin(t) * std::cos(azi)),
		float(std::cos(t)),
		float(std::sin(t) * std::sin(azi)));

	// Radius only sets scale here, not order: HWSkyPortal::NeedDepthBuffer() is
	// false, so the depth test is OFF for everything in DrawContents and DRAW
	// ORDER alone decides what covers what. That is why the moon can be drawn
	// under the clouds simply by drawing it first.
	const float R      = 9000.f;
	const float halfH  = float(R * std::tan(clamp<float>(heightDeg, 0.5f, 120.f) * 0.5 * DEG));
	// Width follows the texture's aspect, so a tall bolt is never squashed onto
	// the square quad the moon uses.
	const float aspect = (tex->GetDisplayHeight() > 0.f)
	                   ? float(tex->GetDisplayWidth() / tex->GetDisplayHeight())
	                   : 1.f;
	const float halfW  = halfH * aspect;

	// Any stable basis perpendicular to dir. Picking the world up axis fails when
	// the quad IS straight up -- which is exactly MAP01's moon -- so fall back to
	// a different reference there instead of producing a degenerate quad.
	FVector3 up = (std::abs(dir.Y) > 0.99f) ? FVector3(0, 0, 1) : FVector3(0, 1, 0);
	FVector3 right = dir ^ up;
	if (right.LengthSquared() < 1e-6f) right = FVector3(1, 0, 0);
	right.MakeUnit();
	up = right ^ dir;
	up.MakeUnit();

	const FVector3 c = dir * R;

	// Position the static quad (mMoonStart, built once in CreateDome) with a model
	// matrix: columns are the quad's basis scaled to size, translation is where
	// it sits. A vertex (x,y,0) therefore lands at c + right*halfW*x + up*halfH*y.
	const float m[16] = {
		right.X * halfW, right.Y * halfW, right.Z * halfW, 0.f,
		up.X    * halfH, up.Y    * halfH, up.Z    * halfH, 0.f,
		dir.X,           dir.Y,           dir.Z,           0.f,
		c.X,             c.Y,             c.Z,             1.f,
	};

	state.SetMaterial(tex, UF_Texture, 0, CLAMP_XY, NO_TRANSLATION, -1);
	state.AlphaFunc(Alpha_Greater, 0.f);
	state.SetRenderStyle(STYLE_Translucent);
	state.EnableTexture(true);

	// SetObjectColor, NOT SetColor. This vertex buffer declares VATTR_COLOR, so
	// the Vulkan pipeline sets UseVertexData and main.vp assigns vColor from the
	// vertex attribute, ignoring uVertexColor -- SetColor silently does nothing
	// on anything drawn out of FSkyVertexBuffer. uObjectColor is a separate
	// uniform and main.fp applies it as `texel *= uObjectColor`, alpha included.
	// (It survives into RT too: rt_main packs uObjectColor * uVertexColor into
	// the primitive colour RTGL1's RsSky.frag multiplies in.)
	state.SetObjectColor(PalEntry(int(clamp(alpha, 0.f, 1.f) * 255.f), 255, 255, 255));

	state.mModelMatrix.loadMatrix(m);
	state.EnableModelMatrix(true);
	state.Draw(DT_TriangleStrip, skyBuffer->MoonIndex(), 4);
	state.EnableModelMatrix(false);
	state.SetObjectColor(0xffffffff);
}

static void RT_DrawMoonQuad(HWDrawInfo* di, FRenderState& state,
                            FSkyVertexBuffer* skyBuffer)
{
	if (!cvar::rt_moon_geo || !cvar::rt_sun)
	{
		return;
	}

	auto texid = TexMan.CheckForTexture("MOONDISC", ETextureType::Any);
	if (!texid.isValid())
	{
		return;
	}
	auto tex = TexMan.GetGameTexture(texid, true);
	if (!tex || !tex->isValid())
	{
		return;
	}

	RT_DrawSkyQuad(state, skyBuffer, tex, cvar::rt_sun_b, cvar::rt_sun_a,
	               clamp<float>(cvar::rt_moon_geo_size, 0.5f, 60.f), 1.f);
}

//-----------------------------------------------------------------------------
//
// Doom64-RT: the volumetric cloud deck.
//
// MAP11 is authored WITH clouds -- a two-layer CLOUDPRP skybox room scrolled
// from ACS (script 670 OPEN: Scroll_Ceiling(1,4,4) and Scroll_Floor(2,15,15)) --
// and under RT you never see a pixel of it, because sector skybox rooms are
// ignored (the white/black box bug; see hw_walls.cpp and d64r-rt-sky's ZSCRIPT)
// and rt_sky_always fills every F_SKY1 opening with the flat night sky instead.
// This puts the clouds back where the RT path can actually render them: as sky
// geometry, which HWSkyPortal rasterises into the cubemap RTGL1 samples on ray
// miss.
//
// A STACK OF HORIZONTAL DISCS, not layers on the dome. The first version of
// this scrolled two sheets around the dome, and that is the one motion a cloud
// layer must never have: a dome layer rotates about the viewer, so clouds orbit
// and come round again, and they do it identically wherever you stand. A
// horizontal deck translates its texture along a fixed wind vector instead --
// linear, unbounded, never repeating a lap -- and perspective supplies the rest
// for nothing, crowding distant clouds toward the horizon on its own.
//
// VOLUMETRIC means the shells really are at different heights. Each draws a
// different horizontal CUT through one 3D density field (CLOUDV1..CLOUDV8,
// baked by tools/gen_clouds.py, index 1 = cloud base), so they occlude each
// other correctly, they parallax against each other as the viewer moves, and
// the deck has a visible underside and visible tops. It is not N copies of one
// sheet at N heights; that arrangement produces obvious repeated ghosts.
//
// The slice art carries baked top-down transmittance, so shading is mostly
// already in the texture. What is left here is the two things a bake cannot
// know: the per-shell brightness ramp (rt_clouds_dark, dark base to lit top,
// which reinforces the bake) and the lightning flash, which lights the deck
// from BELOW and therefore weights the LOWER shells hardest -- the one lighting
// direction that does not exist most of the time.
//
// DRAW ORDER IS THE ONLY DEPTH THERE IS. NeedDepthBuffer() is false for this
// portal, so nothing z-tests. Shells go top-down (far to near for a viewer who
// is always underneath), after the moon so cloud can pass in front of it.
//
//-----------------------------------------------------------------------------
static constexpr int RT_CLOUD_SLICES = 8;

//-----------------------------------------------------------------------------
//
// Doom64-RT: does the deck stand between the moon and the map?
//
// The clouds are sky geometry -- rasterised into RTGL1's cubemap, never in the
// acceleration structure -- so they cannot cast a shadow, and the moon's
// directional light would otherwise pour straight through an overcast sky at
// full strength. A cloud drifting across the moon has to dim the moonlight, or
// the deck is wallpaper.
//
// So it is computed rather than traced: walk the moon's ray up through the same
// shells the draw loop is about to place, sample each slice's ALPHA where the
// ray crosses it, and multiply the transmittances. That is Beer-Lambert again,
// the same model the slice art itself was baked with, just evaluated along one
// ray instead of straight down.
//
// Sampling happens on the CPU on a cached 64x64 reduction of each slice, not on
// the full 1024x1024 image: this runs once per frame and the moon is a
// half-degree disc, so per-texel precision would be measuring nothing. The
// cache is built lazily on first use and keyed by texture pointer.
//
// The result is published to rt_main, which owns the light. It is deliberately
// NOT a light cut: cover changes at wind speed, over seconds, so it must not
// flush the denoiser -- that is for lights that switch.
//
//-----------------------------------------------------------------------------
namespace
{
struct CloudAlphaCache
{
	static constexpr int N = 64;
	const void* key = nullptr;
	float a[N * N] = {};
	bool  ready = false;
};

CloudAlphaCache g_cloudAlpha[RT_CLOUD_SLICES];

const CloudAlphaCache* RT_CloudAlpha(int idx, FGameTexture* tex)
{
	CloudAlphaCache& c = g_cloudAlpha[idx];
	if (c.ready && c.key == tex)
	{
		return &c;
	}

	auto src = tex->GetTexture();
	if (!src) return nullptr;

	FBitmap bmp = src->GetBgraBitmap(nullptr, nullptr);
	const int w = bmp.GetWidth();
	const int h = bmp.GetHeight();
	if (w <= 0 || h <= 0) return nullptr;

	const uint8_t* px = bmp.GetPixels();
	for (int y = 0; y < CloudAlphaCache::N; y++)
	{
		const int sy = y * h / CloudAlphaCache::N;
		for (int x = 0; x < CloudAlphaCache::N; x++)
		{
			const int sx = x * w / CloudAlphaCache::N;
			// BGRA
			c.a[y * CloudAlphaCache::N + x] = px[(sy * w + sx) * 4 + 3] * (1.f / 255.f);
		}
	}
	c.key = tex;
	c.ready = true;
	return &c;
}

float RT_CloudAlphaAt(const CloudAlphaCache* c, float u, float v)
{
	constexpr int N = CloudAlphaCache::N;
	// Wrap into the tile, then bilinear -- the deck tiles, and a nearest lookup
	// here would make the moon's brightness step as the wind crossed texel
	// boundaries.
	float fu = (u - std::floor(u)) * N;
	float fv = (v - std::floor(v)) * N;
	int x0 = int(fu) % N, y0 = int(fv) % N;
	int x1 = (x0 + 1) % N, y1 = (y0 + 1) % N;
	float tx = fu - std::floor(fu), ty = fv - std::floor(fv);

	const float a00 = c->a[y0 * N + x0], a10 = c->a[y0 * N + x1];
	const float a01 = c->a[y1 * N + x0], a11 = c->a[y1 * N + x1];
	return (a00 * (1 - tx) + a10 * tx) * (1 - ty) + (a01 * (1 - tx) + a11 * tx) * ty;
}
} // namespace

extern void RT_SetCloudSunTransmittance(float r, float g, float b);

static void RT_DrawCloudDeck(HWDrawInfo* di, FRenderState& state,
                             FSkyVertexBuffer* skyBuffer)
{
	if (!cvar::rt_clouds || skyBuffer->CloudCount() <= 0)
	{
		RT_SetCloudSunTransmittance(1.f, 1.f, 1.f);
		return;
	}

	FGameTexture* slice[RT_CLOUD_SLICES] = {};
	int have = 0;
	for (int i = 0; i < RT_CLOUD_SLICES; i++)
	{
		char name[16];
		mysnprintf(name, sizeof(name), "CLOUDV%d", i + 1);
		auto texid = TexMan.CheckForTexture(name, ETextureType::Any);
		if (texid.isValid())
		{
			auto t = TexMan.GetGameTexture(texid, true);
			if (t && t->isValid()) { slice[i] = t; ++have; }
		}
	}
	if (have == 0)
	{
		// d64r-rt-sky.pk3 not loaded, or built before gen_clouds.py grew slices.
		RT_SetCloudSunTransmittance(1.f, 1.f, 1.f);
		return;
	}

	const double DEG = M_PI / 180.0;

	// Deck placement. The rim altitude is the intuitive control -- "how close to
	// the horizon do the clouds reach" -- and the radius and base height follow
	// from it, on a sphere of fixed radius so the deck always stays INSIDE the
	// dome (10000) no matter how the angle is set. Only the ratio matters: this
	// is all direction from the origin, drawn with no depth test.
	const float rim = clamp<float>(cvar::rt_clouds_horizon, 2.f, 60.f);
	const float dist = 9200.f;
	const float R = float(dist * std::cos(rim * DEG));
	const float H = float(dist * std::sin(rim * DEG));

	const float curve = clamp<float>(cvar::rt_clouds_curve, 0.f, 0.95f);
	const float thick = clamp<float>(cvar::rt_clouds_thick, 0.f, 4.f);
	const float tiles = max<float>(0.25f, cvar::rt_clouds_tiles);
	const float dark  = clamp<float>(cvar::rt_clouds_dark, 0.f, 1.f);
	const float alpha = clamp<float>(cvar::rt_clouds_alpha, 0.f, 1.f);

	const int shells = clamp<int>(cvar::rt_clouds_shells, 1, RT_CLOUD_SLICES);

	// Playsim time, not wall clock: this is weather in the level, so it should
	// stop when the level stops. TicFrac keeps it smooth between tics.
	const double now = (di->Level->maptime + di->Viewpoint.TicFrac) / double(TICRATE);
	const double wdir = cvar::rt_clouds_wind_dir * DEG;
	const float  wu = float(std::cos(wdir) * cvar::rt_clouds_wind * now);
	const float  wv = float(std::sin(wdir) * cvar::rt_clouds_wind * now);

	const float flash = clamp<float>(RT_LightningFlashLevel() * float(cvar::rt_clouds_flash),
	                                 0.f, 1.f);

	state.AlphaFunc(Alpha_Greater, 0.f);
	state.EnableTexture(true);

	// Wind shear: each shell offset a little further downwind than the one under
	// it, so the stack leans instead of sitting square. Real cloud decks shear
	// with height, and it is the cheapest thing here that makes the volume
	// unmistakable -- without it the shells line up vertically and the eye reads
	// concentric rings rather than depth.
	const float shear = cvar::rt_clouds_shear;
	const float su = float(std::cos(wdir)) * shear;
	const float sv = float(std::sin(wdir)) * shear;

	// Moon occlusion, accumulated as the shells are placed so it cannot drift
	// out of step with what is actually drawn.
	const double moonAlt = clamp<double>(cvar::rt_sun_a, -89.9, 89.9);
	const double moonAzi = cvar::rt_sun_b * DEG;
	const double mt = (90.0 - moonAlt) * DEG;
	const FVector3 moonDir(float(std::sin(mt) * std::cos(moonAzi)),
	                       float(std::cos(mt)),
	                       float(std::sin(mt) * std::sin(moonAzi)));

	// Cloud colour. The slice art is near-achromatic on purpose (gen_clouds.py),
	// so this owns the hue -- of the picture AND of the moonlight that comes
	// through it. `pass` is what a fully covered patch still transmits, coloured;
	// with it, moonlight under a purple deck arrives purple.
	const uint32_t tintc = *(cvar::rt_clouds_tint);
	const float tint[3] = { RPART(tintc) / 255.f, GPART(tintc) / 255.f, BPART(tintc) / 255.f };
	const float pass = clamp<float>(cvar::rt_clouds_transmit, 0.f, 1.f);

	// How much of the moon's ray is still clear sky, accumulated over the shells
	// as they are placed. The tint and `pass` are applied ONCE at the end, to
	// the covered fraction, rather than per shell.
	//
	// That distinction is the whole behaviour of the knob. Folding them in per
	// shell -- which is what this used to do -- raises `pass` to the power of
	// the shell count: at the shipping 0.22 over 6 shells a fully covered ray
	// transmitted 1e-4, which is not "a dim tinted remainder", it is nothing.
	// Every covered ray then hit the hard floor in RT_SetCloudSunTransmittance,
	// and because that floor was a per-channel clamp it landed on
	// (0.02, 0.02, 0.02) -- grey. So the documented headline feature, moonlight
	// under a purple deck arriving purple, did not happen at all under actual
	// cover: the deck went opaque and the tint was clamped back out of it. It
	// only ever showed in the narrow partial-cover band.
	//
	// Treating the deck as one slab of coverage A = 1 - clearsky makes `pass`
	// mean what its description says -- what a FULLY covered patch transmits --
	// independently of how many shells are stacked to build that patch. Shell
	// count then changes the picture and the volume, not the light, which is
	// what makes rt_clouds_shells safe to raise.
	float clearsky = 1.f;

	state.AlphaFunc(Alpha_Greater, 0.f);
	state.EnableTexture(true);

	// Top shell first. Two passes per shell rather than two loops over all of
	// them, so the additive flash on a shell lands directly on that shell's own
	// translucent pass instead of on everything drawn since.
	for (int s = shells - 1; s >= 0; s--)
	{
		// 0 at the base, 1 at the top.
		const float f = (shells == 1) ? 0.f : float(s) / float(shells - 1);

		int idx = (shells == 1) ? RT_CLOUD_SLICES / 2
		                        : int(f * float(RT_CLOUD_SLICES - 1) + 0.5f);
		// Fall back toward the base if a slice is missing, so a partial pk3
		// degrades to fewer clouds rather than to none.
		while (idx > 0 && !slice[idx]) --idx;
		if (!slice[idx]) continue;

		const float h = H * (1.f + thick * f);
		const float ou = wu + su * float(s);
		const float ov = wv + sv * float(s);

		// Where the moon's ray crosses this shell. Flat-plane intersection, not
		// the bowed surface: solving against y = h(1 - curve*r^2) needs
		// iteration, and the error only matters out near the rim where the deck
		// has already faded to nothing.
		if (moonDir.Y > 0.02f)
		{
			const FVector3 p = moonDir * (h / moonDir.Y);
			const float r = std::sqrt(p.X * p.X + p.Z * p.Z) / R;
			if (r < 1.f)
			{
				if (const auto* cache = RT_CloudAlpha(idx, slice[idx]))
				{
					// Same radial fade the mesh bakes into vertex alpha.
					float fade = clamp((r - 0.45f) / 0.55f, 0.f, 1.f);
					fade = 1.f - fade * fade * (3.f - 2.f * fade);

					const float a = clamp(RT_CloudAlphaAt(cache,
					                                      (p.X / R) * tiles + ou,
					                                      (p.Z / R) * tiles + ov)
					                      * fade * alpha, 0.f, 1.f);
					// Accumulate how much of the ray is still CLEAR sky. The
					// tint and `pass` are applied once, after the loop, to the
					// covered fraction -- see the note where `clearsky` is
					// declared for why this is not folded in per shell.
					clearsky *= (1.f - a);
				}
			}
		}

		// Column-major, as in RT_DrawSkyQuad. Maps mesh (x, -f^2, z) to
		// (R*x, h - h*curve*f^2, R*z): a disc of radius R at height h, bowed
		// down at the rim by `curve`.
		const float m[16] = {
			R,    0.f,        0.f, 0.f,
			0.f,  h * curve,  0.f, 0.f,
			0.f,  0.f,        R,   0.f,
			0.f,  h,          0.f, 1.f,
		};

		// Translate THEN scale, so the wind offset is in whole tiles and stays
		// independent of rt_clouds_tiles. VSMatrix ops post-multiply, so this
		// builds T*S and a texcoord maps to tiles*uv + offset.
		state.mTextureMatrix.loadIdentity();
		state.mTextureMatrix.translate(ou, ov, 0.f);
		state.mTextureMatrix.scale(tiles, tiles, 1.f);
		state.EnableTextureMatrix(true);

		state.mModelMatrix.loadMatrix(m);
		state.EnableModelMatrix(true);

		state.SetMaterial(slice[idx], UF_Texture, 0, CLAMP_NONE, NO_TRANSLATION, -1);

		// Brightness ramp toward the lit tops, the deck's tint, and the shell's
		// own opacity, all in one multiply. SetObjectColor rather than SetColor
		// -- see the note in RT_DrawSkyQuad.
		const float b = dark + (1.f - dark) * f;
		state.SetRenderStyle(STYLE_Translucent);
		state.SetObjectColor(PalEntry(int(alpha * 255.f),
		                              int(clamp(b * tint[0], 0.f, 1.f) * 255.f),
		                              int(clamp(b * tint[1], 0.f, 1.f) * 255.f),
		                              int(clamp(b * tint[2], 0.f, 1.f) * 255.f)));
		state.Draw(DT_TriangleStrip, skyBuffer->CloudIndex(), skyBuffer->CloudCount());

		if (flash > 0.002f)
		{
			// Lit from below: the strike is under the deck, so the base slices
			// take most of it and the tops barely brighten. Weighting this the
			// other way (or evenly) makes the whole deck flash like a lamp
			// behind a curtain, which is the one thing that reads as fake.
			const float w = flash * (1.f - 0.7f * f);
			state.SetRenderStyle(STYLE_Add);
			state.SetObjectColor(PalEntry(int(clamp(w, 0.f, 1.f) * 255.f), 210, 224, 255));
			state.Draw(DT_TriangleStrip, skyBuffer->CloudIndex(), skyBuffer->CloudCount());
		}
	}

	state.EnableModelMatrix(false);
	state.EnableTextureMatrix(false);
	state.SetObjectColor(0xffffffff);
	state.SetRenderStyle(STYLE_Translucent);

	// The deck as one slab: the clear fraction passes everything, the covered
	// fraction passes `pass`, tinted. Per channel, which is the whole point --
	// a scalar here would dim the moon without ever colouring it.
	const float covered = 1.f - clearsky;
	float transmittance[3];
	for (int c = 0; c < 3; c++)
	{
		transmittance[c] = clearsky + covered * tint[c] * pass;
	}

	// Blend toward white by rt_clouds_occlude so the effect can be dialled back
	// without turning the deck off. rt_clouds_transmit is the real floor here
	// (see there); rt_main clamps a hard minimum on top purely as a guard against
	// a cvar combination that would black out a moon-lit map.
	const float k = clamp<float>(cvar::rt_clouds_occlude, 0.f, 1.f);
	RT_SetCloudSunTransmittance(1.f - k * (1.f - transmittance[0]),
	                            1.f - k * (1.f - transmittance[1]),
	                            1.f - k * (1.f - transmittance[2]));
}

//-----------------------------------------------------------------------------
//
// Doom64-RT: the visible bolt.
//
// Scenery, not a light. The light is the analytic directional in rt_main.cpp,
// aimed down this same bearing -- see the storm comment at RT_OnLightningFlash
// for why a bright thing in the sky cubemap cannot carry a room at 1 spp.
//
// Alpha tracks the strike envelope, so the bolt inherits the multi-stroke
// stutter for free: each sub-stroke restarts the envelope and the bolt blinks
// with it, which is most of what makes a flash read as lightning. It is cut off
// below 0.12 rather than faded all the way out, because a lightning channel
// does not dim gracefully -- a faint ghost of a bolt hanging in the sky for a
// quarter second is the one thing here that looks obviously fake.
//
//-----------------------------------------------------------------------------
static void RT_DrawLightningBolt(HWDrawInfo* di, FRenderState& state,
                                 FSkyVertexBuffer* skyBuffer)
{
	if (!cvar::rt_lightning || !cvar::rt_lightning_bolt)
	{
		return;
	}

	const float flash = RT_LightningFlashLevel();
	if (flash < 0.12f)
	{
		return;
	}

	float azi = 0.f, alt = 0.f;
	int variant = 0;
	if (!RT_LightningAim(&azi, &alt, &variant))
	{
		return;
	}

	char name[16];
	mysnprintf(name, sizeof(name), "BOLT%d", clamp(variant, 0, 3) + 1);
	auto texid = TexMan.CheckForTexture(name, ETextureType::Any);
	if (!texid.isValid())
	{
		return;
	}
	auto tex = TexMan.GetGameTexture(texid, true);
	if (!tex || !tex->isValid())
	{
		return;
	}

	RT_DrawSkyQuad(state, skyBuffer, tex, azi, alt,
	               clamp<float>(cvar::rt_lightning_bolt_size, 5.f, 120.f),
	               std::min(1.f, flash * 1.6f));
}
#endif

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------
void HWSkyPortal::DrawContents(HWDrawInfo *di, FRenderState &state)
{
#if HAVE_RT
	auto rttype = rtstate.push_type(RtPrim::Sky);
	auto rttemp = rtstate.push_uniqueid(this);
#endif

	bool drawBoth = false;
	auto &vp = di->Viewpoint;

	// We have no use for Doom lighting special handling here, so disable it for this function.
	auto oldlightmode = di->lightmode;
	if (isSoftwareLighting(oldlightmode))
	{
		di->SetFallbackLightMode();
		state.SetNoSoftLightLevel();
	}


	state.ResetColor();
	state.EnableFog(false);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	state.SetRenderStyle(STYLE_Translucent);
	bool oldClamp = state.SetDepthClamp(true);

	di->SetupView(state, 0, 0, 0, !!(mState->MirrorFlag & 1), !!(mState->PlaneMirrorFlag & 1));

	state.SetVertexBuffer(vertexBuffer);
	auto skybox = origin->texture[0] ? dynamic_cast<FSkyBox*>(origin->texture[0]->GetTexture()) : nullptr;
	if (skybox)
	{
		vertexBuffer->RenderBox(state, skybox, origin->x_offset[0], origin->sky2, di->Level->info->pixelstretch, di->Level->info->skyrotatevector, di->Level->info->skyrotatevector2);
	}
	else
	{
		if (origin->texture[0]==origin->texture[1] && origin->doublesky) origin->doublesky=false;	

		if (origin->texture[0])
		{
			state.SetTextureMode(TM_OPAQUE);
			vertexBuffer->RenderDome(state, origin->texture[0], origin->x_offset[0], origin->y_offset, origin->mirrored, FSkyVertexBuffer::SKYMODE_MAINLAYER, !!(di->Level->flags & LEVEL_FORCETILEDSKY));
			state.SetTextureMode(TM_NORMAL);
		}
		
		state.AlphaFunc(Alpha_Greater, 0.f);
		
		if (origin->doublesky && origin->texture[1])
		{
			vertexBuffer->RenderDome(state, origin->texture[1], origin->x_offset[1], origin->y_offset, false, FSkyVertexBuffer::SKYMODE_SECONDLAYER, !!(di->Level->flags & LEVEL_FORCETILEDSKY));
		}

		if (di->Level->skyfog>0 && !di->isFullbrightScene()  && (origin->fadecolor & 0xffffff) != 0)
		{
			PalEntry FadeColor = origin->fadecolor;
			FadeColor.a = clamp<int>(di->Level->skyfog, 0, 255);

			state.EnableTexture(false);
			state.SetObjectColor(FadeColor);
			state.Draw(DT_Triangles, 0, 12);
			state.EnableTexture(true);
			state.SetObjectColor(0xffffffff);
		}
	}
#if HAVE_RT
	// Order is everything in here: the depth test is off for this portal
	// (NeedDepthBuffer() == false), so each of these simply covers the one
	// before it. Dome and caps, then the moon, then clouds that can drift in
	// front of the moon, then the bolt over all of it.
	RT_DrawMoonQuad(di, state, vertexBuffer);
	RT_DrawCloudDeck(di, state, vertexBuffer);
	RT_DrawLightningBolt(di, state, vertexBuffer);
#endif

	di->lightmode = oldlightmode;
	state.SetDepthClamp(oldClamp);
}

const char *HWSkyPortal::GetName() { return "Sky"; }
