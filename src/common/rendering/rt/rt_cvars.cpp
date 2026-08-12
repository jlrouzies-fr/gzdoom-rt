// The defining face of rt_cvars.inc: the storage and the FCVarDecl registration
// for every Doom64-RT cvar. Split out of rt_main.cpp, where 1800 lines of pure
// declaration sat between the includes and the first line of actual logic.
//
// rt_cvars.h includes the same .inc with the macros redefined to plain externs,
// so there is exactly one list and a declaration cannot drift from its definition.

#include "rt_cvars.h"

#include "c_cvars.h"

// clang-format off

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

#define RT_CVAR_STRING( name, default_value, flags, description ) \
    CVARD( String, name, default_value, flags, description )


namespace cvar
{

#include "rt_cvars.inc"

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
