#include "doctest.h"
#include "Analyze/CliConfigurationParser.hpp"

TEST_SUITE_BEGIN("CliConfigurationParser");

TEST_CASE("convert dotted dictionary")
{
    auto output = parseDottedConfiguration(R"DOTTED_CONFIG(
        {
            "luau-lsp.foo.bar": true,
            "luau-lsp.foo.baz": 1,
            "luau-lsp.bar": "testing"
        }
    )DOTTED_CONFIG");

    REQUIRE(output.is_object());
    REQUIRE(output.contains("foo"));
    REQUIRE(output.contains("bar"));
    REQUIRE(output["foo"].is_object());

    CHECK_EQ(output["foo"]["bar"], true);
    CHECK_EQ(output["foo"]["baz"], 1);
    CHECK_EQ(output["bar"], "testing");
}

TEST_CASE("settings with comments and trailing commas parse like VS Code reads them")
{
    auto output = parseDottedConfiguration(R"DOTTED_CONFIG(
        {
            // a line comment, with a comma, before a key
            "luwu.fflags.override": {
                "DebugLuwuTryExpressions": "true", /* a block comment, } with a brace */
            },
            "luwu.ignoreGlobs": [
                "**/tests/**", // a trailing comma after the last item
            ],
            "luwu.url": "https://example.com/a,]",
        }
    )DOTTED_CONFIG");

    REQUIRE(output.contains("fflags"));
    CHECK_EQ(output["fflags"]["override"]["DebugLuwuTryExpressions"], "true");
    REQUIRE(output["ignoreGlobs"].is_array());
    CHECK_EQ(output["ignoreGlobs"].size(), 1);
    CHECK_EQ(output["url"], "https://example.com/a,]");
}

TEST_SUITE_END();