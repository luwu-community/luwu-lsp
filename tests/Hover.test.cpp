#include "doctest.h"
#include "Fixture.h"
#include "LSP/DocumentationParser.hpp"
#include "LSP/KeywordHovers.hpp"
#include "Flags.hpp"

LUAU_FASTFLAG(DebugLuauUserDefinedClasses)
LUAU_FASTFLAG(LuwuBetterUserDefinedClasses)

TEST_SUITE_BEGIN("Hover");

TEST_CASE_FIXTURE(Fixture, "show_string_length_on_hover")
{
    auto source = R"(
        local x = "this is a string"
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "string (16 bytes)"));
}

/// Luau compatibility mode (`--luau-compat`) for the duration of a test. Restores every flag it touched.
struct ScopedLuauCompatibilityMode
{
    std::vector<std::pair<Luau::FValue<bool>*, bool>> saved;

    ScopedLuauCompatibilityMode()
    {
        for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            saved.emplace_back(flag, flag->value);
        applyLuwuFlags(/* luauCompatibilityMode: */ true);
    }

    ~ScopedLuauCompatibilityMode()
    {
        applyLuwuFlags(/* luauCompatibilityMode: */ false);
        for (auto [flag, value] : saved)
            flag->value = value;
    }
};

TEST_CASE_FIXTURE(Fixture, "hover_code_blocks_are_luau_only_for_luau_files_in_compatibility_mode")
{
    ScopedLuauCompatibilityMode compat;

    auto source = R"(
        local x = "this is a string"
    )";

    lsp::HoverParams params;
    params.position = lsp::Position{1, 18};

    params.textDocument = lsp::TextDocumentIdentifier{newDocument("foo.luau", source)};
    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "string (16 bytes)"));

    // Compatibility mode is for Luau code; a .luwu file is always Luwu
    params.textDocument = lsp::TextDocumentIdentifier{newDocument("bar.luwu", source)};
    result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "string (16 bytes)"));
}

TEST_CASE_FIXTURE(Fixture, "show_string_utf8_characters_on_hover")
{
    auto source = R"(
        local x = "this is an emoji: 😁"
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "string (22 bytes, 19 characters)"));
}

TEST_CASE_FIXTURE(Fixture, "basic_type_alias_declaration")
{
    auto source = R"(
        type Identity = string
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type Identity = string"));
}

TEST_CASE_FIXTURE(Fixture, "type_alias_declaration_with_single_generic")
{
    auto source = R"(
        type Identity<T> = T
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type Identity<T> = T"));
}

TEST_CASE_FIXTURE(Fixture, "type_alias_declaration_with_generic_default_value")
{
    auto source = R"(
        type Identity<T = string> = T
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type Identity<T = string> = T"));
}

TEST_CASE_FIXTURE(Fixture, "type_alias_declaration_with_multiple_generics")
{
    auto source = R"(
        type Identity<T, U = string> = T
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type Identity<T, U = string> = T"));
}

TEST_CASE_FIXTURE(Fixture, "type_alias_declaration_generic_type_pack")
{
    auto source = R"(
        type Identity<T...> = (any) -> T...
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type Identity<T...> = (any) -> (T...)"));
}

TEST_CASE_FIXTURE(Fixture, "type_alias_declaration_generic_type_pack_with_default")
{
    auto source = R"(
        type Identity<T... = ...string> = (any) -> T...
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type Identity<T... = ...string> = (any) -> (T...)"));
}

TEST_CASE_FIXTURE(Fixture, "complex_type_alias_declaration_with_generics")
{
    auto source = R"(
        type Identity<T, U = number, V... = ...string> = (T, U) -> V...
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type Identity<T, U = number, V... = ...string> = (T, U) -> (V...)"));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_type_table")
{
    auto source = R"(
        --- This is documentation for Foo
        type Foo = {
        }
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{2, 14};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type Foo = {  }") + kDocumentationBreaker + "This is documentation for Foo\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_type_table_when_hovering_over_variable_with_type")
{
    auto source = R"(
        --- This is documentation for Foo
        type Foo = {
        }
        local x: Foo = nil
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{4, 14};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "local x: {  }") + kDocumentationBreaker + "This is documentation for Foo\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_member_of_a_type_table")
{
    auto source = R"(
        --- This is documentation for Foo
        type Foo = {
            --- This is a member bar
            bar: string,
        }
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{4, 13};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "string") + kDocumentationBreaker + "This is a member bar\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_member_of_a_type_table_when_hovering_over_property")
{
    auto source = R"(
        --- This is documentation for Foo
        type Foo = {
            --- This is a member bar
            bar: string,
        }
        local x: Foo
        local y = x.bar
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{7, 21};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "string") + kDocumentationBreaker + "This is a member bar\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_member_of_an_intersected_type_table_when_hovering_over_property")
{
    auto source = R"(
        type A = {
            --- Example sick number
            Hello: number
        }

        type B = {
            --- Example sick string
            Heya: string
        } & A

        local item: B = nil
        print(item.Heya)
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{12, 21};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "string") + kDocumentationBreaker + "Example sick string\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_function")
{
    auto source = R"(
        --- This is documentation for Foo
        function foo()
        end
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{2, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "function foo(): ()") + kDocumentationBreaker + "This is documentation for Foo\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_function_call")
{
    auto source = R"(
        --- This is documentation for Foo
        function foo()
        end
        foo()
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{4, 9};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "function foo(): ()") + kDocumentationBreaker + "This is documentation for Foo\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_type_alias_declarations")
{
    auto source = R"(
        --- The metre (or meter in [US spelling]; symbol: m) is the [base unit] of [length]
        --- in the [International System of Units] (SI)
        export type Meters = number
    )";

    auto uri = newDocument("meters.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{3, 21};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type Meters = number") + kDocumentationBreaker +
                                         "The metre (or meter in [US spelling]; symbol: m) is the [base unit] of [length]\n" +
                                         "in the [International System of Units] (SI)\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_type_alias_declarations_of_intersected_tables")
{
    auto source = R"(
        type Foo = {
            foo: "Foo",
        }

        type Bar = {
            bar: "Bar",
        }

        --- The terms foobar (/ˈfuːbɑːr/), foo, bar, baz, qux, quux, and others are used as
        --- metasyntactic variables and placeholder names in computer programming or computer-related documentation
        export type Foobar = Foo & Bar
    )";

    auto uri = newDocument("meters.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{11, 21};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type Foobar = {\n    bar: \"Bar\"\n} & {\n    foo: \"Foo\"\n}") + kDocumentationBreaker +
                                         "The terms foobar (/ˈfuːbɑːr/), foo, bar, baz, qux, quux, and others are used as\n" +
                                         "metasyntactic variables and placeholder names in computer programming or computer-related documentation\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_type_references")
{
    auto source = R"(
        type Foo = {
            foo: "Foo",
        }

        type Bar = {
            bar: "Bar",
        }

        --- This is the intersection of two types
        export type Foobar = Foo & Bar

        function consumer(value: Foobar)
        end
    )";

    auto uri = newDocument("meters.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{12, 36};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type Foobar = {\n    bar: \"Bar\"\n} & {\n    foo: \"Foo\"\n}") + kDocumentationBreaker +
                                         "This is the intersection of two types\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_external_type_references")
{
    auto source = newDocument("types.luau", R"(
        --- This is a type
        export type Value = string
    )");

    auto uri = newDocument("source.luau", R"(
        local Types = require("types.luau")

        local x: Types.Value
    )");

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{3, 25};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type Types.Value = string") + kDocumentationBreaker + "This is a type\n");
}

TEST_CASE_FIXTURE(Fixture, "show_type_of_global_variable")
{
    auto source = R"(
        print(DocumentedGlobalVariable)
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 23};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type DocumentedGlobalVariable = number"));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_global_type_table_from_definitions_file")
{
    auto source = R"(
        local x: DocumentedTable = nil
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 24};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type DocumentedTable = {\n"
                                                       "    member1: string\n"
                                                       "}") +
                                         kDocumentationBreaker + "This is a documented table\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_global_type_table_from_definitions_file_when_hovering_over_variable_with_type")
{
    auto source = R"(
        local x: DocumentedTable = nil
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 14};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "local x: {\n"
                                                       "    member1: string\n"
                                                       "}") +
                                         kDocumentationBreaker + "This is a documented table\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_global_type_table_from_definitions_file_when_hovering_over_property")
{
    auto source = R"(
        local x: DocumentedTable = nil
        local y = x.member1
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{2, 23};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "string") + kDocumentationBreaker + "This is documented member1 of the table\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_global_function_call_from_definitions_file")
{
    auto source = R"(
        DocumentedGlobalFunction()
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 20};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value,
        codeBlock("luwu", "function DocumentedGlobalFunction(): number") + kDocumentationBreaker + "This is a documented global function\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_when_hovering_over_class_type_from_definitions_file")
{
    auto source = R"(
        local x: DocumentedClass
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 23};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(
        result->contents.value,
        codeBlock("luwu", "extern type DocumentedClass\n    member1: string\n    function function1(self): number\nend") + kDocumentationBreaker +
            "This is a documented class\n"
    );
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_when_hovering_over_variable_with_class_type")
{
    auto source = R"(
        local x: DocumentedClass
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 14};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(
        result->contents.value,
        codeBlock("luwu", "extern type DocumentedClass\n    member1: string\n    function function1(self): number\nend") + kDocumentationBreaker +
            "This is a documented class\n"
    );
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_when_hovering_over_class_type_property")
{
    auto source = R"(
        local x: DocumentedClass
        local y = x.member1
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{2, 23};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "string") + kDocumentationBreaker + "This is a documented member1 of the class\n");
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_when_hovering_over_class_type_method_call")
{
    auto source = R"(
        local x: DocumentedClass
        local y = x:function1()
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{2, 23};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value,
        codeBlock("luwu", "function DocumentedClass:function1(): number") + kDocumentationBreaker + "This is a documented function1 of the class\n");
}

// TEST_CASE_FIXTURE(Fixture, "includes_documentation_when_hovering_over_global_variable_from_definitions_file")
//{
//     auto source = R"(
//          print(DocumentedGlobalVariable)
//      )";
//
//     auto uri = newDocument("foo.luau", source);
//
//     lsp::HoverParams params;
//     params.textDocument = lsp::TextDocumentIdentifier{uri};
//     params.position = lsp::Position{1, 23};
//
//     auto result = workspace.hover(params, nullptr);
//     REQUIRE(result);
//     CHECK_EQ(result->contents.value,
//         codeBlock("luwu", "type DocumentedGlobalVariable = number") + kDocumentationBreaker + "This is a documented global variable\n");
// }

TEST_CASE_FIXTURE(Fixture, "includes_documentation_when_all_parts_of_union_point_to_same_location")
{
    auto uri = newDocument("foo.luau", R"(
        type BaseNode<HOS> = {
	        --[[
		        Indicates if the node has only a single supporter, this is purely internal
		        and only used by `object_tree.closest_empty_node`,
		        as an optimization for trees that have a taper type of "Flat" or "Slope".
	        ]]
	        has_one_supporter: HOS,
        }

        export type Node = BaseNode<true> | BaseNode<false>

        local x: Node = {} :: any

        x.has_one_supporter
    )");

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{14, 17}; // 'x.has_one_supporter'

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", FFlag::LuauSolverV2 ? "boolean" : "false | true") + kDocumentationBreaker +
                                         "Indicates if the node has only a single supporter, this is purely internal\n"
                                         "and only used by `object_tree.closest_empty_node`,\n"
                                         "as an optimization for trees that have a taper type of \"Flat\" or \"Slope\".\n");
}

TEST_CASE_FIXTURE(Fixture, "handles_type_references_without_types_graph")
{
    auto source = newDocument("types.luau", R"(
        --- This is a type
        export type Value = string
    )");

    auto uri = newDocument("source.luau", R"(
        local Types = require("types.luau")

        local x: Types.Value
    )");

    // This test explicitly expects type graphs to not be retained (i.e., the required module scope was cleared)
    // We should still be able to find the type references.
    workspace.checkSimple(workspace.fileResolver.getModuleName(uri), /* cancellationToken= */ nullptr);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{3, 25};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "type Types.Value = string") + kDocumentationBreaker + "This is a type\n");
}

TEST_CASE_FIXTURE(Fixture, "hover_respects_cancellation")
{
    auto cancellationToken = std::make_shared<Luau::FrontendCancellationToken>();
    cancellationToken->cancel();

    auto document = newDocument("a.luau", "local x = 1");
    CHECK_THROWS_AS(workspace.hover(lsp::HoverParams{{{document}}}, cancellationToken), RequestCancelledException);
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_comment_inside_local_function_body_does_not_show_function_type")
{
    auto [source, marker] = sourceWithMarker(R"(
        local function add1(n: number): number
            -- hovering | over me should not show function type
            return n + 1
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    CHECK_FALSE(result.has_value());
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_comment_inside_global_function_body_does_not_show_function_type")
{
    auto [source, marker] = sourceWithMarker(R"(
        function add1(n: number): number
            -- hovering | over me should not show function type
            return n + 1
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    CHECK_FALSE(result.has_value());
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_comment_inside_anonymous_function_body_does_not_show_function_type")
{
    auto [source, marker] = sourceWithMarker(R"(
        local add1 = function(n: number): number
            -- hovering | over me should not show function type
            return n + 1
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    CHECK_FALSE(result.has_value());
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_whitespace_inside_function_body_does_not_show_function_type")
{
    auto [source, marker] = sourceWithMarker(R"(
        local function add1(n: number): number
          |  return n + 1
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    CHECK_FALSE(result.has_value());
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_base_table_member_of_setmetatable_type")
{
    auto source = R"(
        local meta = {
            __index = {
                --- Documentation for prop_b.
                prop_b = "hello",
            }
        }

        local obj = setmetatable({
            --- Documentation for prop_a.
            prop_a = "world",
        }, meta)

        local y = obj.prop_a
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{13, 22};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("Documentation for prop_a.") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_index_member_of_setmetatable_type")
{
    auto source = R"(
        local meta = {
            __index = {
                --- Documentation for prop_b.
                prop_b = "hello",
            }
        }

        local obj = setmetatable({
            --- Documentation for prop_a.
            prop_a = "world",
        }, meta)

        local y = obj.prop_b
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{13, 22};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("Documentation for prop_b.") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_const_class_property_shows_const")
{
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class Cat
            public const |name: string

            function __init(self, name: string)
                self.name = name
            end
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "public const name: string"));
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_class_keyword_of_exported_class_shows_class_keyword_docs")
{
    // Regression test: `export class Foo ... end` used to record the location of `export` (not
    // `class`) as AstStatClass::keywordLocation, so hovering the literal `class` keyword found no
    // match here and hovering `export` matched against text that isn't in keyword_hovers.json --
    // in both cases, nothing showed up.
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        export cl|ass Cat
            name: string

            function __init(self, name: string)
                self.name = name
            end
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, getKeywordHoverDocs("class"));
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_class_name_shows_class_value_summary_with_constructor")
{
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class |Cat
            public const name: string
            private age: number

            function __init(self, name: string, age: number)
                self.name = name
                self.age = age
            end

            public function meow(self): string
                return self.name
            end
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("class Cat(name: string, age: number)") != std::string::npos);
    CHECK(result->contents.value.find("object of") == std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_private_before_primary_constructor_shows_private_constructor_docs")
{
    // The `private` between a class's name and its primary constructor's parameter list qualifies
    // the constructor, not the class or a field, so it must not fall back to the plain `private`
    // docs -- those describe something else entirely at this position.
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class Particle pri|vate (
            public position: Vector2,
            public velocity: Vector2,
            private mass: number
        )
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, getKeywordHoverDocs("primary_constructor_private"));
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_public_before_primary_constructor_shows_public_constructor_docs")
{
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class Seal pub|lic (name: string)
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, getKeywordHoverDocs("primary_constructor_public"));
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_public_primary_constructor_parameter_shows_param_docs")
{
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class Particle private (
            pub|lic position: Vector2,
            public velocity: Vector2,
            private mass: number
        )
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, getKeywordHoverDocs("primary_constructor_param_public"));
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_private_primary_constructor_parameter_shows_param_docs")
{
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class Particle private (
            public position: Vector2,
            public velocity: Vector2,
            priv|ate mass: number
        )
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, getKeywordHoverDocs("primary_constructor_param_private"));
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_const_primary_constructor_parameter_shows_param_const_docs")
{
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class SshKey private (
            public co|nst public_key: string,
            private const private_key: string
        )
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, getKeywordHoverDocs("primary_constructor_param_const"));
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_class_name_with_primary_constructor_shows_positional_constructor")
{
    // A primary constructor is a terser spelling of `function __init`, so the summary should show
    // the same positional argument list -- not the `Name{ field = value }` table-literal shape a
    // class with no constructor at all gets.
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class |Particle(position: number, velocity: number)
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("class Particle(position: number, velocity: number)") != std::string::npos);
    CHECK(result->contents.value.find("{") == std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_object_of_class_with_primary_constructor_lists_parameter_fields")
{
    // The fields of this class are declared entirely by its primary constructor's parameters --
    // they're not AstClassMembers at all, so the summary has to pick them up from the constructor.
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class Particle private (
            public position: number,
            public velocity: number,
            private mass: number
        )
            public function spawn(): Particle
                return Particle(0, 0, 1)
            end
        end

        const |particle = Particle.spawn()
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("public position: number") != std::string::npos);
    CHECK(result->contents.value.find("public velocity: number") != std::string::npos);
    // `mass` is private, so it has no business showing up in a summary of the public API surface.
    CHECK(result->contents.value.find("mass") == std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_primary_constructor_parameter_name_shows_the_field_it_declares")
{
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class Particle private (
            public position: number,
            public vel|ocity: number,
            private mass: number
        )
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "public velocity: number"));
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_private_const_primary_constructor_parameter_name_shows_qualifiers")
{
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class SshKey private (
            public const public_key: string,
            private const priv|ate_key: string
        )
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "private const private_key: string"));
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_unannotated_primary_constructor_parameter_name_shows_inferred_type")
{
    // No annotation on the parameter, so the field's type has to be read back off the class's own
    // instance type rather than off an AstType node that doesn't exist.
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class Point(x, |y)
        end

        const point = Point(1, 2)
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("public y") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "class_summary_lists_all_fields_before_any_function")
{
    // Source order puts a static function first and interleaves a method between the fields; the
    // summary should still read fields-then-functions, with the static function ahead of the
    // instance methods.
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class |Vector2
            function zero(): Vector2
                return Vector2 { x = 0, y = 0 }
            end

            x: number

            function add(self, other: Vector2): Vector2
                return Vector2 { x = self.x + other.x, y = self.y + other.y }
            end

            y: number

            function __init(self, x: number, y: number)
                self.x = x
                self.y = y
            end
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);

    auto x = result->contents.value.find("x: number");
    auto y = result->contents.value.find("y: number");
    auto zero = result->contents.value.find("function zero");
    auto add = result->contents.value.find("function add");
    REQUIRE(x != std::string::npos);
    REQUIRE(y != std::string::npos);
    REQUIRE(zero != std::string::npos);
    REQUIRE(add != std::string::npos);

    CHECK(x < y);
    CHECK(y < zero);
    CHECK(zero < add);
}

TEST_CASE_FIXTURE(Fixture, "class_value_summary_shows_private_members_but_object_summary_hides_them")
{
    // The class value is hovered from inside the scope its privates are reachable in, so hiding
    // them there makes the summary lie about the class's shape; an object is typically held from
    // outside that scope, where they aren't reachable at all.
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, classMarker] = sourceWithMarker(R"(
        class |Particle private (
            public position: number,
            private mass: number
        )
            public function spawn(): Particle
                return Particle(0, 1)
            end

            private function decay(self): number
                return self.mass
            end
        end

        const particle = Particle.spawn()
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = classMarker;

    auto classResult = workspace.hover(params, nullptr);
    REQUIRE(classResult);
    CHECK(classResult->contents.value.find("public position: number") != std::string::npos);
    CHECK(classResult->contents.value.find("private mass: number") != std::string::npos);
    CHECK(classResult->contents.value.find("private function decay") != std::string::npos);

    auto [objectSource, objectMarker] = sourceWithMarker(R"(
        class Particle private (
            public position: number,
            private mass: number
        )
            public function spawn(): Particle
                return Particle(0, 1)
            end

            private function decay(self): number
                return self.mass
            end
        end

        const |particle = Particle.spawn()
    )");

    auto objectUri = newDocument("bar.luau", objectSource);
    params.textDocument = lsp::TextDocumentIdentifier{objectUri};
    params.position = objectMarker;

    auto objectResult = workspace.hover(params, nullptr);
    REQUIRE(objectResult);
    CHECK(objectResult->contents.value.find("public position: number") != std::string::npos);
    CHECK(objectResult->contents.value.find("mass") == std::string::npos);
    CHECK(objectResult->contents.value.find("decay") == std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "class_summary_marks_a_private_primary_constructor_in_the_header")
{
    // Without the `private`, the header reads as an invitation to call `ParticleSystem(...)` --
    // which would fail at runtime -- and makes the `new` factory below it look redundant.
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class |ParticleSystem private (gravity: number, particles: number)
            private particles
            private gravity

            public function new(gravity: number): ParticleSystem
                return ParticleSystem(gravity, 0)
            end
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("class ParticleSystem private (") != std::string::npos);
    // Restated in the class body as private, so they're fields of the class and must be listed
    // as such -- the header's parameter list is the constructor's signature, not the class's shape.
    CHECK(result->contents.value.find("private particles") != std::string::npos);
    CHECK(result->contents.value.find("private gravity") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "generic_class_value_summary_shows_its_generic_parameters")
{
    // The class value is the factory, not an instance, so it has nothing instantiated to print and
    // used to render as a bare `class List` -- leaving the `{T}` in its own constructor signature
    // and field lines referring to a parameter the header never introduced.
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class |List<T> private (inner: { T })
            private inner

            public function new(): List<T>
                return List({})
            end
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("class List<T> private (") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "primary_constructor_parameter_hover_uses_visibility_restated_in_the_class_body")
{
    // The parameter list is bare here, so the parameter's own qualifiers say "public" by default --
    // but the class body restates the field as private, and that restatement is the declaration.
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class List<T> private (in|ner: { T })
            private inner
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "private inner: {T}"));
}

TEST_CASE_FIXTURE(Fixture, "primary_constructor_parameter_hover_prefers_its_own_qualifier_over_the_body")
{
    // The other direction: an explicit qualifier on the parameter is the declaration, and a
    // restatement in the body may only agree with it (the parser rejects a contradiction).
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class Frame private (
            private const na|me: string,
            private size: number
        )
            private name
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luwu", "private const name: string"));
}

TEST_CASE_FIXTURE(Fixture, "class_summary_works_for_a_class_required_from_another_module")
{
    // The summary is built from the class's AST, which lives in the module that declared it -- this
    // used to bail out whenever that wasn't the module being hovered in, so a class reached through
    // a require (the ordinary way to use one) fell back to printing just its name.
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    newDocument("list.luau", R"(
        export class List private (inner: { number })
            private inner

            public function new(): List
                return List({})
            end

            public function len(self): number
                return #self.inner
            end
        end

        return { List = List }
    )");

    auto [source, marker] = sourceWithMarker(R"(
        local |List = require("./list").List
    )");
    auto uri = newDocument("main.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("class List private (") != std::string::npos);
    CHECK(result->contents.value.find("private inner") != std::string::npos);
    CHECK(result->contents.value.find("function new") != std::string::npos);
    CHECK(result->contents.value.find("function len") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "hover_includes_summary_of_class_referenced_by_the_type")
{
    // A class referenced inside a hovered type (here, one side of a union) prints as a bare name --
    // the summary below the type is the only place its shape shows up, same as a `where` clause
    // does for a type alias.
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class Cat(name: string)
            public function meow(self): string
                return self.name
            end
        end

        local |pet: Cat | string = "none"
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("local pet: Cat | string") != std::string::npos);
    CHECK(result->contents.value.find("class Cat\n    name: string\n    function meow(self): string\nend") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "hover_includes_summary_of_extern_type_referenced_by_the_type")
{
    auto [source, marker] = sourceWithMarker(R"(
        local function |make(): DocumentedClass?
            return nil
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("extern type DocumentedClass\n    member1: string\n    function function1(self): number\nend") !=
          std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "hover_truncates_summaries_of_referenced_classes_more_than_the_hovered_class")
{
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        class Point(a: number, b: number, c: number, d: number, e: number)
        end

        local function |make(): Point?
            return nil
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("class Point\n    a: number\n    b: number\n    c: number\n    -- ⋯ 2 more members\nend") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "hover_truncates_large_where_clause_tables_without_unbalancing_brackets")
{
    // ToString's maxTypeLength drops every emit past the limit, closing brackets included -- a large
    // alias in a `where` clause used to be cut off mid-signature, leaving an unclosed `{`/`(` that
    // broke highlighting of everything after it in the hover.
    std::string alias = "type Library = {\n";
    for (int i = 0; i < 40; i++)
        alias += "    function_number_" + std::to_string(i) + ": (first_argument: number, second_argument: string?) -> boolean,\n";
    alias += "}\n";

    auto [source, marker] = sourceWithMarker(alias + R"(
        local |lib: { inner: Library } = nil :: any
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    const std::string& hover = result->contents.value;
    CHECK(hover.find("type Library = {") != std::string::npos);
    CHECK(hover.find("-- ⋯ ") != std::string::npos);
    CHECK(hover.find("more properties\n}") != std::string::npos);
    CHECK(hover.find("TRUNCATED") == std::string::npos);
}

TEST_SUITE_END();
