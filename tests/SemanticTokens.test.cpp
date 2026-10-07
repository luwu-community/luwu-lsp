#include "doctest.h"
#include "Fixture.h"
#include "LSP/SemanticTokens.hpp"
#include "Flags.hpp"

LUAU_FASTFLAG(LuwuClasses)
LUAU_FASTFLAG(LuwuTraits)
LUAU_FASTFLAG(LuwuIfLocal)
LUAU_FASTFLAG(LuwuTableComprehensions)
LUAU_FASTFLAG(DebugLuwuDoExpr)

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
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
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

TEST_CASE_FIXTURE(Fixture, "object_typed_as_a_trait_is_not_coloured_as_the_trait")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    check(R"(
trait Entry
    expect public name: string
end
class File(public name: string) implements Entry end
local function entry(): Entry
    return File("main.luau")
end
local ent = entry()
print(ent, Entry)
)");

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());

    // A plain variable gets no token of its own (the grammar colours it), and certainly not the trait's
    auto object = getSemanticToken(tokens, Luau::Position{9, 6});
    CHECK((!object || object->tokenType != lsp::SemanticTokenTypes::Interface));

    auto trait = getSemanticToken(tokens, Luau::Position{9, 11});
    REQUIRE(trait);
    CHECK_EQ(trait->tokenType, lsp::SemanticTokenTypes::Interface);
}

TEST_CASE_FIXTURE(Fixture, "none_is_left_to_the_luwu_grammar")
{
    check("local x = none");

    // The Luau grammar doesn't know `none`, so in a Luau file the token is what marks it as a builtin
    auto luauTokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());
    auto luauToken = getSemanticToken(luauTokens, Luau::Position{0, 10});
    REQUIRE(luauToken);
    CHECK_EQ(luauToken->tokenModifiers, lsp::SemanticTokenModifiers::DefaultLibrary | lsp::SemanticTokenModifiers::Readonly);

    auto luwuTokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule(), /* grammarHighlightsNone= */ true);
    CHECK_FALSE(getSemanticToken(luwuTokens, Luau::Position{0, 10}));
}

// The position of the `occurrence`th (0-based) `word` on `line` of `source`
static Luau::Position wordPosition(const std::string& source, unsigned int line, const std::string& word, size_t occurrence = 0)
{
    size_t lineStart = 0;
    for (unsigned int i = 0; i < line; i++)
        lineStart = source.find('\n', lineStart) + 1;
    std::string text = source.substr(lineStart, source.find('\n', lineStart) - lineStart);

    size_t column = text.find(word);
    for (size_t i = 0; i < occurrence; i++)
        column = text.find(word, column + 1);
    REQUIRE(column != std::string::npos);
    return Luau::Position{line, static_cast<unsigned int>(column)};
}

static void checkKeywordToken(const std::vector<SemanticToken>& tokens, const Luau::Position& position)
{
    auto token = getSemanticToken(tokens, position);
    REQUIRE(token);
    CHECK_EQ(token->tokenType, lsp::SemanticTokenTypes::Keyword);
}

static void checkNotKeywordToken(const std::vector<SemanticToken>& tokens, const Luau::Position& position)
{
    auto token = getSemanticToken(tokens, position);
    if (token)
        CHECK_NE(token->tokenType, lsp::SemanticTokenTypes::Keyword);
}

TEST_CASE_FIXTURE(Fixture, "table_comprehension_when_and_give_have_keyword_semantic_tokens")
{
    ScopedFastFlag luwuIfLocal{FFlag::LuwuIfLocal, true};
    ScopedFastFlag luwuTableComprehensions{FFlag::LuwuTableComprehensions, true};
    ENABLE_NEW_SOLVER();

    // `when` and `give` ending their lines are what the grammar can't tell apart from names
    std::string source = "local xs = { 1, 2 }\n"
                         "local ys = { for _, v in xs when\n"
                         "    v > 1 give\n"
                         "    v * 2 }\n"
                         "local zs = { for _, v in xs when const w = v when w > 1 give w }\n"
                         "local ns = { for i = 1, 10 give i }\n";
    check(source);

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());
    checkKeywordToken(tokens, wordPosition(source, 1, "when"));
    checkKeywordToken(tokens, wordPosition(source, 2, "give"));
    checkKeywordToken(tokens, wordPosition(source, 4, "when", 0));
    checkKeywordToken(tokens, wordPosition(source, 4, "when", 1));
    checkKeywordToken(tokens, wordPosition(source, 4, "give"));
    checkKeywordToken(tokens, wordPosition(source, 5, "give"));
}

TEST_CASE_FIXTURE(Fixture, "if_local_when_has_keyword_semantic_token")
{
    ScopedFastFlag luwuIfLocal{FFlag::LuwuIfLocal, true};
    ENABLE_NEW_SOLVER();

    std::string source = "local function f(s: string?)\n"
                         "    if local n = s when\n"
                         "        #n > 0 then\n"
                         "    end\n"
                         "    local y = if local m = s when #m > 0 then m else nil\n"
                         "end\n";
    check(source);

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());
    checkKeywordToken(tokens, wordPosition(source, 1, "when"));
    checkKeywordToken(tokens, wordPosition(source, 4, "when"));
}

TEST_CASE_FIXTURE(Fixture, "do_expression_give_has_keyword_semantic_token")
{
    ScopedFastFlag doExpr{FFlag::DebugLuwuDoExpr, true};
    ENABLE_NEW_SOLVER();

    std::string source = "local a = do\n"
                         "    give\n"
                         "        5\n";
    check(source);

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());
    checkKeywordToken(tokens, wordPosition(source, 1, "give"));
}

TEST_CASE_FIXTURE(Fixture, "variables_named_give_and_when_are_not_keywords")
{
    ScopedFastFlag luwuIfLocal{FFlag::LuwuIfLocal, true};
    ScopedFastFlag luwuTableComprehensions{FFlag::LuwuTableComprehensions, true};
    ScopedFastFlag doExpr{FFlag::DebugLuwuDoExpr, true};
    ENABLE_NEW_SOLVER();

    std::string source = "local give = 1\n"
                         "local when = give\n";
    check(source);

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());
    checkNotKeywordToken(tokens, wordPosition(source, 0, "give"));
    checkNotKeywordToken(tokens, wordPosition(source, 1, "when"));
    checkNotKeywordToken(tokens, wordPosition(source, 1, "give"));
}

TEST_SUITE_END();
