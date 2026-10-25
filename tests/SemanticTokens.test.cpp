#include "doctest.h"
#include "Fixture.h"
#include "LSP/SemanticTokens.hpp"
#include "Flags.hpp"

LUAU_FASTFLAG(LuwuClasses)
LUAU_FASTFLAG(LuwuTraits)
LUAU_FASTFLAG(LuwuIfLocal)
LUAU_FASTFLAG(LuwuTableComprehensions)
LUAU_FASTFLAG(LuwuDoExpr)
LUAU_FASTFLAG(LuwuInLuwuDocs)
LUAU_FASTFLAG(LuwuDestructuring)

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
    ScopedFastFlag doExpr{FFlag::LuwuDoExpr, true};
    ENABLE_NEW_SOLVER();

    std::string source = "local a = do\n"
                         "    give\n"
                         "        5\n";
    check(source);

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());
    checkKeywordToken(tokens, wordPosition(source, 1, "give"));
}

TEST_CASE_FIXTURE(Fixture, "type_destructure_colors_only_the_type_names")
{
    ScopedFastFlag luwuDestructuring{FFlag::LuwuDestructuring, true};
    ENABLE_NEW_SOLVER();

    // `type .{Content} = chimera` desugars to `type Content = chimera.Content`, a reference whose prefix is written
    // after its name: `Content` is coloured as a type, and nothing lands on `chimera`
    std::string source = "local chimera = {}\n"
                         "type .{ Content } = chimera\n"
                         "local x: chimera.Content\n";
    check(source);

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());

    auto content = getSemanticToken(tokens, wordPosition(source, 1, "Content"));
    REQUIRE(content);
    CHECK_EQ(content->end.column - content->start.column, 7);

    auto module = wordPosition(source, 1, "chimera");
    for (const auto& token : tokens)
        if (token.start.line == 1)
            CHECK(token.end.column <= module.column);

    // a written `chimera.Content` still colours both parts, by their own locations
    auto prefix = getSemanticToken(tokens, wordPosition(source, 2, "chimera"));
    REQUIRE(prefix);
    CHECK_EQ(prefix->tokenType, lsp::SemanticTokenTypes::Namespace);
    CHECK_EQ(prefix->end.column - prefix->start.column, 7);
    auto name = getSemanticToken(tokens, wordPosition(source, 2, "Content"));
    REQUIRE(name);
    CHECK_EQ(name->end.column - name->start.column, 7);
}

TEST_CASE_FIXTURE(Fixture, "destructured_names_get_one_token_each_and_the_pattern_none")
{
    ScopedFastFlag luwuDestructuring{FFlag::LuwuDestructuring, true};
    ENABLE_NEW_SOLVER();

    // The hidden local an unnamed pattern binds its value to spans the whole `.{...}`, and each field's desugared
    // read sits on its key: neither is source anyone wrote
    std::string source = "local t = { h1 = function() end, h2 = function() end }\n"
                         "local function f() end\n"
                         "const .{ h1, h2 } = t\n"
                         "const .{ x } = f\n";
    check(source);

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());

    for (unsigned int line : {2u, 3u})
        CHECK_FALSE(getSemanticToken(tokens, wordPosition(source, line, ".{")));

    for (const char* name : {"h1", "h2"})
    {
        auto position = wordPosition(source, 2, name);
        size_t count = std::count_if(tokens.begin(), tokens.end(),
            [&](const SemanticToken& token)
            {
                return token.start == position;
            });
        CHECK_EQ(count, 1);
        auto token = getSemanticToken(tokens, position);
        REQUIRE(token);
        CHECK_EQ(token->tokenType, lsp::SemanticTokenTypes::Function);
    }
}

TEST_CASE_FIXTURE(Fixture, "destructured_keys_read_under_another_name_are_properties")
{
    ScopedFastFlag luwuDestructuring{FFlag::LuwuDestructuring, true};
    ENABLE_NEW_SOLVER();

    std::string source = "local t = { h1 = 1, h2 = 2, sub = { gz = 1 } }\n"
                         "const .{ h1 as H1, h2, sub as .{ gz as Gz } } = t\n"
                         "for _, p.{ h1 as a } in { t } do end\n";
    check(source);

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());

    for (const char* key : {"h1", "sub", "gz"})
    {
        auto token = getSemanticToken(tokens, wordPosition(source, 1, key));
        REQUIRE(token);
        CHECK_EQ(token->tokenType, lsp::SemanticTokenTypes::Property);
    }
    auto loopKey = getSemanticToken(tokens, wordPosition(source, 2, "h1"));
    REQUIRE(loopKey);
    CHECK_EQ(loopKey->tokenType, lsp::SemanticTokenTypes::Property);

    // The names that get bound aren't properties
    for (const char* name : {"H1", "h2", "Gz"})
    {
        auto token = getSemanticToken(tokens, wordPosition(source, 1, name));
        CHECK((!token || token->tokenType != lsp::SemanticTokenTypes::Property));
    }
}

TEST_CASE_FIXTURE(Fixture, "variables_named_give_and_when_are_not_keywords")
{
    ScopedFastFlag luwuIfLocal{FFlag::LuwuIfLocal, true};
    ScopedFastFlag luwuTableComprehensions{FFlag::LuwuTableComprehensions, true};
    ScopedFastFlag doExpr{FFlag::LuwuDoExpr, true};
    ENABLE_NEW_SOLVER();

    std::string source = "local give = 1\n"
                         "local when = give\n";
    check(source);

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());
    checkNotKeywordToken(tokens, wordPosition(source, 0, "give"));
    checkNotKeywordToken(tokens, wordPosition(source, 1, "when"));
    checkNotKeywordToken(tokens, wordPosition(source, 1, "give"));
}

TEST_CASE_FIXTURE(Fixture, "moonwave_tags_at_the_start_of_doc_lines")
{
    auto source = R"(
        --- Adds things
        --- @param count how many
        --- reads @std/fs, which isn't a tag
        -- @param not a doc comment
        local function add(count: number) end
    )";

    auto uri = newDocument("foo.luau", source);
    auto moduleName = workspace.fileResolver.getModuleName(uri);
    workspace.checkStrict(moduleName, nullptr);

    std::vector<SemanticToken> tokens;
    addMoonwaveTagTokens(tokens, workspace.frontend.getSourceModule(moduleName), workspace.fileResolver.getTextDocument(uri));

    REQUIRE_EQ(tokens.size(), 2);
    CHECK_EQ(tokens[0].start.line, 2);
    CHECK_EQ(tokens[0].start.column, 12);
    CHECK_EQ(tokens[0].end.column, 18);
    CHECK_EQ(tokens[0].tokenType, lsp::SemanticTokenTypes::Decorator);
    CHECK_EQ(tokens[1].start.column, 19);
    CHECK_EQ(tokens[1].tokenType, lsp::SemanticTokenTypes::Parameter);
}

TEST_CASE_FIXTURE(Fixture, "doc_reference_code_is_highlighted_like_code")
{
    ScopedFastFlag inLuwuDocs{FFlag::LuwuInLuwuDocs, true};

    std::string source = "local lib = { go = function() end, count = 1 }\n"
                         "type Options = { x: number }\n"
                         "--- uses [lib.go] and [lib.count], see [type Options] and [Options]\n"
                         "--- [fmt = lib]\n"
                         "--- then [fmt.go] and [string.split], [@std/str] and [a link]\n"
                         "---\n"
                         "--- [a link]: https://example.com\n"
                         "local function f(n: number) end\n";
    check(source);

    auto tokens = getSemanticTokens(workspace.frontend, getMainModule(), getMainSourceModule());
    auto expect = [&](Luau::Position position, lsp::SemanticTokenTypes type)
    {
        auto token = getSemanticToken(tokens, position);
        REQUIRE_MESSAGE(token, "no token at " << position.line << ":" << position.column);
        CHECK_MESSAGE(token->tokenType == type, "at " << position.line << ":" << position.column << ": " << int(token->tokenType));
    };

    expect(wordPosition(source, 2, "lib"), lsp::SemanticTokenTypes::Variable);
    expect(wordPosition(source, 2, "go"), lsp::SemanticTokenTypes::Function);
    expect(wordPosition(source, 2, "count"), lsp::SemanticTokenTypes::Property);
    expect(wordPosition(source, 2, "type"), lsp::SemanticTokenTypes::Keyword);
    expect(wordPosition(source, 2, "Options"), lsp::SemanticTokenTypes::Type);
    expect(wordPosition(source, 2, "Options", 1), lsp::SemanticTokenTypes::Type);
    expect(wordPosition(source, 3, "fmt"), lsp::SemanticTokenTypes::Variable);
    expect(wordPosition(source, 3, "lib"), lsp::SemanticTokenTypes::Variable);
    // through the binding
    expect(wordPosition(source, 4, "go"), lsp::SemanticTokenTypes::Function);
    expect(wordPosition(source, 4, "string"), lsp::SemanticTokenTypes::Variable);
    // the string library's functions take a `self`, so `string.split` is a method in code too
    expect(wordPosition(source, 4, "split"), lsp::SemanticTokenTypes::Method);
    expect(wordPosition(source, 4, "@std/str"), lsp::SemanticTokenTypes::String);

    // the brackets and dots are punctuation, and all of it is marked as code in documentation
    expect(wordPosition(source, 2, "[lib"), lsp::SemanticTokenTypes::Punctuation);
    expect(wordPosition(source, 2, ".go"), lsp::SemanticTokenTypes::Punctuation);
    expect(wordPosition(source, 2, "] and"), lsp::SemanticTokenTypes::Punctuation);
    expect(wordPosition(source, 3, " = "), lsp::SemanticTokenTypes::Punctuation);
    auto go = getSemanticToken(tokens, wordPosition(source, 2, "go"));
    REQUIRE(go);
    CHECK_EQ(go->tokenModifiers, lsp::SemanticTokenModifiers::Documentation);

    // a markdown link is prose
    CHECK_FALSE(getSemanticToken(tokens, wordPosition(source, 4, "a link")));
    CHECK_FALSE(getSemanticToken(tokens, wordPosition(source, 2, "uses")));
}

TEST_SUITE_END();
