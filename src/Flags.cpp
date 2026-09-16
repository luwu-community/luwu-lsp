#include "Flags.hpp"

#include "Luau/Common.h"

#include <cstring>
#include <iostream>

#ifdef LSP_BUILD_WITH_SENTRY
// sentry.h pulls in <windows.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif
#define SENTRY_BUILD_STATIC 1
#include <sentry.h>
#endif

LUAU_FASTFLAG(LuauSolverV2)
LUAU_FASTINT(LuauTarjanChildLimit)
LUAU_FASTINT(LuauTableTypeMaximumStringifierLength)

void registerFastFlags(std::unordered_map<std::string, std::string>& fastFlags, ErrorCallback onError, ErrorCallback onWarning)
{
    for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag; flag = flag->next)
    {
        if (fastFlags.find(flag->name) != fastFlags.end())
        {
            std::string valueStr = fastFlags.at(flag->name);

            if (valueStr == "true" || valueStr == "True")
                flag->value = true;
            else if (valueStr == "false" || valueStr == "False")
                flag->value = false;
            else
            {
                onError(std::string("Bad flag option, expected a boolean 'True' or 'False' for flag ") + flag->name);
            }

            fastFlags.erase(flag->name);
        }
    }

    for (Luau::FValue<int>* flag = Luau::FValue<int>::list; flag; flag = flag->next)
    {
        if (fastFlags.find(flag->name) != fastFlags.end())
        {
            std::string valueStr = fastFlags.at(flag->name);

            int value = 0;
            try
            {
                value = std::stoi(valueStr);
            }
            catch (...)
            {
                onError(std::string("Bad flag option, expected an int for flag ") + flag->name);
            }

            flag->value = value;
            fastFlags.erase(flag->name);
        }
    }

    for (auto& [key, _] : fastFlags)
    {
        onWarning(std::string("Unknown FFlag: ") + key);
    }

#ifdef LSP_BUILD_WITH_SENTRY
    sentry_set_tag("luau.new_solver_enabled", FFlag::LuauSolverV2 ? "true" : "false");
#endif
}

void registerFastFlagsCLI(std::unordered_map<std::string, std::string>& fastFlags)
{
    registerFastFlags(
        fastFlags,
        [](const std::string& message)
        {
            std::cerr << message << '\n';
            std::exit(1);
        },
        [](const std::string& message)
        {
            std::cerr << message << '\n';
        });
}

// Luwu's own language features (classes, default arguments, `none`, managed refs, ...) sit behind
// `Luwu`-prefixed flags, plus the upstream `DebugLuau`-prefixed flags that gate class parsing and
// runtime support. This is Luwu's language server, so they're all on unless the user asks for Luau
// compatibility mode. Applied before user-provided flags, so an explicit `--flag:Name=false` (or
// `luau-lsp.fflags.override`) still wins.
static bool luwuFeaturesEnabled_ = true;

bool luwuFeaturesEnabled()
{
    return luwuFeaturesEnabled_;
}

void applyLuwuFlags(bool luauCompatibilityMode)
{
    luwuFeaturesEnabled_ = !luauCompatibilityMode;

    static constexpr const char* kLuwuDebugFlags[] = {"DebugLuauUserDefinedClasses", "DebugLuauUserDefinedClassesRuntime"};

    const bool enabled = !luauCompatibilityMode;

    // The new type solver is required for essentially everything modern, Luwu classes included, so
    // it's on in every configuration -- compatibility mode included. Still overridable per-flag.
    FFlag::LuauSolverV2.value = true;

    for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag; flag = flag->next)
    {
        if (strncmp(flag->name, "Luwu", 4) == 0)
        {
            flag->value = enabled;
            continue;
        }

        for (const char* debugFlag : kLuwuDebugFlags)
        {
            if (strcmp(flag->name, debugFlag) == 0)
            {
                flag->value = enabled;
                break;
            }
        }
    }
}

void applyRequiredFlags()
{
    // Manually enforce a LuauTarjanChildLimit increase
    // TODO: re-evaluate the necessity of this change
    if (FInt::LuauTarjanChildLimit > 0 && FInt::LuauTarjanChildLimit < 15000)
        FInt::LuauTarjanChildLimit.value = 15000;
    // This flag value is enforced due to Studio, but modern editors can handle this easily. Let's remove the restriction by setting it to zero.
    // NOTE: 40 is the current value on Studio. We check against that so that we don't inadvertently change people's overrides
    if (FInt::LuauTableTypeMaximumStringifierLength == 40)
        FInt::LuauTableTypeMaximumStringifierLength.value = 0;
}
