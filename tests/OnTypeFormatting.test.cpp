#include "doctest.h"
#include "Fixture.h"
#include "LSP/LanguageServer.hpp"

TEST_SUITE_BEGIN("OnTypeFormatting");

static lsp::DocumentOnTypeFormattingResult processOnTypeFormatting(Fixture* fixture, const std::string& source, const lsp::Position& position)
{
    auto uri = fixture->newDocument("foo.luau", source);

    lsp::DocumentOnTypeFormattingParams params;
    params.textDocument.uri = uri;
    params.position = position;
    params.ch = source.at(position.character);
    params.options = {4, true};

    return fixture->workspace.onTypeFormatting(params);
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_disabled_by_default")
{
    auto [source, marker] = sourceWithMarker(R"(
        print("aaa {|")
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(!edits.has_value());
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_converts_quotes")
{
    client->globalConfig.format.convertQuotes = true;
    auto [source, marker] = sourceWithMarker(R"(
        print("aaa {|")
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(edits.has_value());
    REQUIRE(edits->size() == 2);

    CHECK_EQ(edits->at(0).newText, "`");
    CHECK_EQ(edits->at(1).newText, "`");

    CHECK_EQ(applyEdit(source, edits.value()), R"(
        print(`aaa {`)
    )");
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_converts_quotes_in_unfinished_string")
{
    client->globalConfig.format.convertQuotes = true;
    auto [source, marker] = sourceWithMarker(R"(
        print("aaa {|
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(edits.has_value());
    REQUIRE(edits->size() == 1);
    CHECK_EQ(edits->at(0).newText, "`");

    CHECK_EQ(applyEdit(source, edits.value()), R"(
        print(`aaa {
    )");
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_converts_single_quoted_string")
{
    client->globalConfig.format.convertQuotes = true;
    auto [source, marker] = sourceWithMarker(R"(
        print('aaa {|')
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(edits.has_value());
    REQUIRE(edits->size() == 2);

    CHECK_EQ(edits->at(0).newText, "`");
    CHECK_EQ(edits->at(1).newText, "`");

    CHECK_EQ(applyEdit(source, edits.value()), R"(
        print(`aaa {`)
    )");
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_converts_unfinished_single_quoted_string")
{
    client->globalConfig.format.convertQuotes = true;
    auto [source, marker] = sourceWithMarker(R"(
        print('aaa {|
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(edits.has_value());
    REQUIRE(edits->size() == 1);
    CHECK_EQ(edits->at(0).newText, "`");

    CHECK_EQ(applyEdit(source, edits.value()), R"(
        print(`aaa {
    )");
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_handles_escaped_quote_in_unfinished_string")
{
    client->globalConfig.format.convertQuotes = true;
    auto [source, marker] = sourceWithMarker(R"(
        print("aaa \"bbb {|
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(edits.has_value());
    REQUIRE(edits->size() == 1);
    CHECK_EQ(edits->at(0).newText, "`");

    CHECK_EQ(applyEdit(source, edits.value()), R"(
        print(`aaa \"bbb {
    )");
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_ignores_backtick_content")
{
    client->globalConfig.format.convertQuotes = true;
    auto [source, marker] = sourceWithMarker(R"(
        print("aa`a {|")
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(!edits.has_value());
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_ignores_backtick_content_in_unfinished_string")
{
    client->globalConfig.format.convertQuotes = true;
    auto [source, marker] = sourceWithMarker(R"(
        print("aa`a {|
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(!edits.has_value());
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_ignores_backtick_before_quote_in_unfinished_string")
{
    client->globalConfig.format.convertQuotes = true;
    auto [source, marker] = sourceWithMarker(R"(
        print(`" {|
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(!edits.has_value());
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_ignores_string_before_bracket")
{
    client->globalConfig.format.convertQuotes = true;
    auto [source, marker] = sourceWithMarker(R"(
        print("aaa" {|)
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(!edits.has_value());
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_handles_multiple_strings_in_line")
{
    client->globalConfig.format.convertQuotes = true;
    auto [source, marker] = sourceWithMarker(R"(
        x = "foo"; y = "bar {|
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(edits.has_value());
    REQUIRE(edits->size() == 1);
    CHECK_EQ(edits->at(0).newText, "`");

    CHECK_EQ(applyEdit(source, edits.value()), R"(
        x = "foo"; y = `bar {
    )");
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_handles_mixed_quotes_in_line")
{
    client->globalConfig.format.convertQuotes = true;
    auto [source, marker] = sourceWithMarker(R"(
        x = "foo"; y = 'bar {|
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(edits.has_value());
    REQUIRE(edits->size() == 1);
    CHECK_EQ(edits->at(0).newText, "`");

    CHECK_EQ(applyEdit(source, edits.value()), R"(
        x = "foo"; y = `bar {
    )");
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_handles_backtick_string_and_string_in_line")
{
    client->globalConfig.format.convertQuotes = true;
    auto [source, marker] = sourceWithMarker(R"(
        x = `foo`; y = "bar {|
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(edits.has_value());
    REQUIRE(edits->size() == 1);
    CHECK_EQ(edits->at(0).newText, "`");

    CHECK_EQ(applyEdit(source, edits.value()), R"(
        x = `foo`; y = `bar {
    )");
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_ignores_string_after_unfinished_backtick")
{
    client->globalConfig.format.convertQuotes = true;
    auto [source, marker] = sourceWithMarker(R"(
        x = `foo "bar {|
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(!edits.has_value());
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_handles_quote_after_another_unclosed_quote")
{
    client->globalConfig.format.convertQuotes = true;
    auto [source, marker] = sourceWithMarker(R"(
        x = 'foo "bar {|
    )");

    auto edits = processOnTypeFormatting(this, source, marker);
    REQUIRE(edits.has_value());
    REQUIRE(edits->size() == 1);
    CHECK_EQ(edits->at(0).newText, "`");

    CHECK_EQ(applyEdit(source, edits.value()), R"(
        x = `foo "bar {
    )");
}

static lsp::DocumentOnTypeFormattingResult typeBracket(Fixture* fixture, const std::string& source, const lsp::Position& position, const std::string& ch)
{
    auto uri = fixture->newDocument("foo.luau", source);

    lsp::DocumentOnTypeFormattingParams params;
    params.textDocument.uri = uri;
    params.position = position;
    params.ch = ch;
    params.options = {4, true};

    return fixture->workspace.onTypeFormatting(params);
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_inserts_missing_comma_after_paren")
{
    auto [source, marker] = sourceWithMarker(R"(
        local t = {
            Paragraph(|)
            note("x"),
        }
    )");

    auto edits = typeBracket(this, source, marker, "(");
    REQUIRE(edits.has_value());
    REQUIRE(edits->size() == 1);

    CHECK_EQ(applyEdit(source, edits.value()), R"(
        local t = {
            Paragraph(),
            note("x"),
        }
    )");
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_inserts_missing_comma_after_nested_brace")
{
    auto [source, marker] = sourceWithMarker(R"(
        local t = {
            Paragraph({|}) -- a comment
            note("x"),
        }
    )");

    auto edits = typeBracket(this, source, marker, "{");
    REQUIRE(edits.has_value());
    REQUIRE(edits->size() == 1);

    CHECK_EQ(applyEdit(source, edits.value()), R"(
        local t = {
            Paragraph({}), -- a comment
            note("x"),
        }
    )");
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_inserts_comma_after_last_table_element")
{
    auto [source, marker] = sourceWithMarker(R"(
        local t = {
            note("x"),
            Paragraph(|)
        }
    )");

    auto edits = typeBracket(this, source, marker, "(");
    REQUIRE(edits.has_value());

    CHECK_EQ(applyEdit(source, edits.value()), R"(
        local t = {
            note("x"),
            Paragraph(),
        }
    )");
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_inserts_comma_after_string_element")
{
    auto [source, marker] = sourceWithMarker(R"(
        local t = {
            "|"
            'x',
        }
    )");

    auto edits = typeBracket(this, source, marker, "\"");
    REQUIRE(edits.has_value());

    CHECK_EQ(applyEdit(source, edits.value()), R"(
        local t = {
            "",
            'x',
        }
    )");
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_inserts_comma_after_keyed_element")
{
    auto [source, marker] = sourceWithMarker(R"(
        local t = {
            name = '|'
        }
    )");

    auto edits = typeBracket(this, source, marker, "'");
    REQUIRE(edits.has_value());

    CHECK_EQ(applyEdit(source, edits.value()), R"(
        local t = {
            name = '',
        }
    )");
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_no_comma_for_call_argument")
{
    auto [source, marker] = sourceWithMarker(R"(
        print(
            "a",
            Paragraph(|)
        )
    )");

    auto edits = typeBracket(this, source, marker, "(");
    CHECK(!edits.has_value());
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_no_comma_inside_one_line_table")
{
    auto [source, marker] = sourceWithMarker(R"(
        local t = { Paragraph(|) }
    )");

    auto edits = typeBracket(this, source, marker, "(");
    CHECK(!edits.has_value());
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_no_comma_for_statement_call")
{
    auto [source, marker] = sourceWithMarker(R"(
        print(|)
        print("x")
    )");

    auto edits = typeBracket(this, source, marker, "(");
    CHECK(!edits.has_value());
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_no_comma_when_already_separated")
{
    auto [source, marker] = sourceWithMarker(R"(
        local t = {
            Paragraph(|),
            note("x"),
        }
    )");

    auto edits = typeBracket(this, source, marker, "(");
    CHECK(!edits.has_value());
}

TEST_CASE_FIXTURE(Fixture, "on_type_formatting_insert_missing_comma_can_be_disabled")
{
    client->globalConfig.format.insertMissingComma = false;
    auto [source, marker] = sourceWithMarker(R"(
        local t = {
            Paragraph(|)
            note("x"),
        }
    )");

    auto edits = typeBracket(this, source, marker, "(");
    CHECK(!edits.has_value());
}

TEST_SUITE_END();
