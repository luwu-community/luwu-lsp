#include "doctest.h"
#include "Fixture.h"
#include "LSP/SemanticTokens.hpp"
#include "Flags.hpp"

LUAU_FASTFLAG(DebugLuauUserDefinedClasses)
LUAU_FASTFLAG(LuwuBetterUserDefinedClasses)

TEST_SUITE_BEGIN("SemanticTokens");

std::optional<SemanticToken> getSemanticToken(const std::vector<SemanticToken>& tokens, const Luau::Position& start)
{
    for (const auto& token : tokens)
        if (token.start == start)
            return token;
    return std::nullopt;
}

TEST_CASE_FIXTURE(Fixture, "explicit_self_method_has_method_semantic_token")
{
    check(R"(
        type myClass = {
            foo: (self: myClass) -> ()
        }
        local a: myClass = nil
        a:foo()
    )");

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());
    REQUIRE(!tokens.empty());

    auto token = getSemanticToken(tokens, Luau::Position{5, 10});
    REQUIRE(token);
    CHECK_EQ(token->tokenType, lsp::SemanticTokenTypes::Method);
    CHECK_EQ(token->tokenModifiers, lsp::SemanticTokenModifiers::None);
}

TEST_CASE_FIXTURE(Fixture, "explicit_self_overloaded_method_has_method_semantic_token")
{
    check(R"(
        type myClass = {
            foo: ((self: myClass) -> ()) & ((self: myClass, myArg: boolean) -> ()),
        }
        local a: myClass = nil
        a:foo()
    )");

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());
    REQUIRE(!tokens.empty());

    auto token = getSemanticToken(tokens, Luau::Position{5, 10});
    REQUIRE(token);
    CHECK_EQ(token->tokenType, lsp::SemanticTokenTypes::Method);
    CHECK_EQ(token->tokenModifiers, lsp::SemanticTokenModifiers::None);
}

TEST_CASE_FIXTURE(Fixture, "references_of_self_within_a_method_have_semantic_tokens")
{
    check(R"(
        local T = {}
        function T:foo()
            self.value = true
        end
    )");

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());
    REQUIRE(!tokens.empty());

    auto token = getSemanticToken(tokens, Luau::Position{3, 12});
    REQUIRE(token);
    CHECK_EQ(token->tokenType, lsp::SemanticTokenTypes::Property);
    CHECK_EQ(token->tokenModifiers, lsp::SemanticTokenModifiers::DefaultLibrary);
}

TEST_CASE_FIXTURE(Fixture, "references_of_explicitly_declared_self_in_a_non_method_function_definition_have_semantic_token")
{
    check(R"(
        local T = {}
        function T.foo(self: any)
            self.value = true
        end
    )");

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());
    REQUIRE(!tokens.empty());

    auto parameter = getSemanticToken(tokens, Luau::Position{2, 23});
    REQUIRE(parameter);
    CHECK_EQ(parameter->tokenType, lsp::SemanticTokenTypes::Property);
    CHECK_EQ(parameter->tokenModifiers, lsp::SemanticTokenModifiers::DefaultLibrary);

    auto usage = getSemanticToken(tokens, Luau::Position{3, 12});
    REQUIRE(usage);
    CHECK_EQ(usage->tokenType, lsp::SemanticTokenTypes::Property);
    CHECK_EQ(usage->tokenModifiers, lsp::SemanticTokenModifiers::DefaultLibrary);
}

TEST_CASE_FIXTURE(Fixture, "explicitly_declared_self_parameter_does_not_have_semantic_token_modifier_in_method_function_definition")
{
    check(R"(
        local T = {}
        function T:foo(self: any)
        end
    )");

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());
    REQUIRE(!tokens.empty());

    auto parameter = getSemanticToken(tokens, Luau::Position{2, 23});
    REQUIRE(parameter);
    CHECK_EQ(parameter->tokenType, lsp::SemanticTokenTypes::Parameter);
    CHECK_EQ(parameter->tokenModifiers, lsp::SemanticTokenModifiers::None);
}

TEST_CASE_FIXTURE(Fixture,
    "references_of_explicitly_declared_self_in_a_non_method_function_definition_does_not_have_semantic_token_if_self_was_not_first_parameter")
{
    check(R"(
        local T = {}
        function T.foo(input: any, self: any)
            self.value = true
        end
    )");

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());
    REQUIRE(!tokens.empty());

    auto parameter = getSemanticToken(tokens, Luau::Position{2, 35});
    REQUIRE(parameter);
    CHECK_EQ(parameter->tokenType, lsp::SemanticTokenTypes::Parameter);
    CHECK_EQ(parameter->tokenModifiers, lsp::SemanticTokenModifiers::None);

    auto usage = getSemanticToken(tokens, Luau::Position{3, 12});
    REQUIRE(usage);
    CHECK_EQ(usage->tokenType, lsp::SemanticTokenTypes::Parameter);
    CHECK_EQ(usage->tokenModifiers, lsp::SemanticTokenModifiers::None);
}

TEST_CASE_FIXTURE(Fixture, "semantic_tokens_respects_cancellation")
{
    auto cancellationToken = std::make_shared<Luau::FrontendCancellationToken>();
    cancellationToken->cancel();
    
    auto document = newDocument("a.luau", "local x = 1");
    CHECK_THROWS_AS(workspace.semanticTokens(lsp::SemanticTokensParams{{{document}}}, cancellationToken), RequestCancelledException);
}

TEST_CASE_FIXTURE(Fixture, "array_sugar_type_does_not_produce_overlapping_synthetic_token")
{
    check(R"(
local x: { string } = {}
)");

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());

    // The synthetic `number` index type introduced by `{ T }` desugaring must not produce
    // a semantic token, as it would overlap with the real `string` type reference token.
    auto synthetic = getSemanticToken(tokens, Luau::Position{1, 9});
    CHECK(!synthetic);

    auto real = getSemanticToken(tokens, Luau::Position{1, 11});
    REQUIRE(real);
    CHECK_EQ(real->tokenType, lsp::SemanticTokenTypes::Type);
}

TEST_CASE_FIXTURE(Fixture, "class_field_restating_a_primary_constructor_parameter_has_parameter_semantic_token")
{
    // A field that restates a primary constructor parameter is that parameter, so it reads as one;
    // a field the constructor knows nothing about stays an ordinary property.
    ScopedFastFlag sffs[] = {{FFlag::DebugLuauUserDefinedClasses, true}, {FFlag::LuwuBetterUserDefinedClasses, true}};
    ENABLE_NEW_SOLVER();

    check(R"(
class Cat(name, actuallyaflerken)
    public name
    public age = 0
    private actuallyaflerken: number
end
)");

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());

    auto restated = getSemanticToken(tokens, Luau::Position{2, 11});
    REQUIRE(restated);
    CHECK_EQ(restated->tokenType, lsp::SemanticTokenTypes::Parameter);

    auto annotated = getSemanticToken(tokens, Luau::Position{4, 12});
    REQUIRE(annotated);
    CHECK_EQ(annotated->tokenType, lsp::SemanticTokenTypes::Parameter);

    // `age` is not a parameter of the constructor, so it must not be coloured as one
    auto ownField = getSemanticToken(tokens, Luau::Position{3, 11});
    CHECK((!ownField || ownField->tokenType != lsp::SemanticTokenTypes::Parameter));
}

TEST_SUITE_END();
