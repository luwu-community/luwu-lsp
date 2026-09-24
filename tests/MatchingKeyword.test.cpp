#include "doctest.h"
#include "Fixture.h"

LUAU_FASTFLAG(DebugLuauUserDefinedClasses)
LUAU_FASTFLAG(LuwuBetterUserDefinedClasses)

TEST_SUITE_BEGIN("MatchingKeyword");

/// Source annotated with a `|` where the cursor is and a `^` where the jump should land. Both
/// markers are removed before the source is parsed.
struct AnnotatedSource
{
    std::string source;
    lsp::Position cursor;
    std::optional<lsp::Position> expected;
};

static AnnotatedSource parseAnnotatedSource(const std::string& annotated)
{
    AnnotatedSource result;
    size_t line = 0;
    size_t column = 0;

    for (auto c : annotated)
    {
        if (c == '|')
            result.cursor = lsp::Position{line, column};
        else if (c == '^')
            result.expected = lsp::Position{line, column};
        else
        {
            result.source += c;
            if (c == '\n')
            {
                line++;
                column = 0;
            }
            else
                column++;
        }
    }

    return result;
}

static void checkJump(Fixture* fixture, const std::string& annotated)
{
    auto annotatedSource = parseAnnotatedSource(dedent(annotated));
    auto uri = fixture->newDocument("matching.luau", annotatedSource.source);

    lsp::MatchingKeywordParams params;
    params.textDocument.uri = uri;
    params.position = annotatedSource.cursor;

    auto result = fixture->workspace.matchingKeyword(params);

    if (!annotatedSource.expected)
    {
        CHECK(!result.has_value());
        return;
    }

    REQUIRE(result.has_value());
    CHECK(result->start == annotatedSource.expected);
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_end_to_function")
{
    checkJump(this, R"(
        ^function foo()
            print("hi")
        |end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_function_to_end")
{
    checkJump(this, R"(
        |function foo()
            print("hi")
        ^end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_position_just_after_end")
{
    checkJump(this, R"(
        ^function foo()
        end|
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_local_function")
{
    checkJump(this, R"(
        local ^function foo()
        |end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_anonymous_function")
{
    checkJump(this, R"(
        local foo = ^function()
        |end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_to_innermost_construct")
{
    checkJump(this, R"(
        function outer()
            ^function inner()
            |end
        end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_end_to_if")
{
    checkJump(this, R"(
        ^if true then
            print(1)
        |end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_end_to_start_of_if_chain")
{
    checkJump(this, R"(
        ^if true then
            print(1)
        elseif false then
            print(2)
        else
            print(3)
        |end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_elseif_to_end")
{
    checkJump(this, R"(
        if true then
        |elseif false then
        ^end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_else_to_end")
{
    checkJump(this, R"(
        if true then
        |else
        ^end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_then_to_end")
{
    checkJump(this, R"(
        if true |then
        ^end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_until_to_repeat")
{
    checkJump(this, R"(
        ^repeat
            print(1)
        |until true
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_repeat_to_until")
{
    checkJump(this, R"(
        |repeat
            print(1)
        ^until true
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_end_to_numeric_for")
{
    checkJump(this, R"(
        ^for i = 1, 10 do
            print(i)
        |end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_end_to_generic_for")
{
    checkJump(this, R"(
        ^for key, value in pairs({}) do
            print(key)
        |end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_in_to_end")
{
    checkJump(this, R"(
        for key, value |in pairs({}) do
        ^end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_loop_do_to_end")
{
    checkJump(this, R"(
        for i = 1, 10 |do
        ^end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_end_to_while")
{
    checkJump(this, R"(
        ^while true do
            print(1)
        |end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_end_to_bare_do_block")
{
    checkJump(this, R"(
        ^do
            print(1)
        |end
    )");
}

TEST_CASE_FIXTURE(Fixture, "nested_blocks_match_the_right_end")
{
    checkJump(this, R"(
        ^while true do
            if true then
            end
            for i = 1, 10 do
            end
        |end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_end_to_class")
{
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    checkJump(this, R"(
        ^class Cat(name)
            public name
        |end
    )");
}

TEST_CASE_FIXTURE(Fixture, "jumps_from_end_to_class_method")
{
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    checkJump(this, R"(
        class Cat(name)
            ^function meow(self)
            |end
        end
    )");
}

TEST_CASE_FIXTURE(Fixture, "no_match_on_an_ordinary_identifier")
{
    checkJump(this, R"(
        local fo|o = 1
    )");
}

TEST_CASE_FIXTURE(Fixture, "no_match_on_a_keyword_without_a_block")
{
    checkJump(this, R"(
        function foo()
            re|turn 1
        end
    )");
}

TEST_CASE_FIXTURE(Fixture, "no_match_for_an_unterminated_block")
{
    checkJump(this, R"(
        |function foo()
    )");
}

TEST_CASE_FIXTURE(Fixture, "no_match_inside_a_string_or_comment")
{
    checkJump(this, R"(
        local foo = "e|nd"
    )");
}

TEST_SUITE_END();
