#include "doctest.h"
#include "Fixture.h"

LUAU_FASTFLAG(LuwuClasses)
LUAU_FASTFLAG(DebugLuwuMatchExpressions)

TEST_SUITE_BEGIN("DocumentSymbol");

/// `count` arms of a `match` statement at `indent`, each one line for its `case` and one for its body
static std::string statementArms(const std::string& indent, int count)
{
    std::string arms;
    for (int i = 0; i < count; ++i)
        arms += indent + "case is " + std::to_string(i) + " then\n" + indent + "    print(" + std::to_string(i) + ")\n";
    return arms;
}

/// `count` arms of a `match` expression at `indent`, one line each
static std::string expressionArms(const std::string& indent, int count)
{
    std::string arms;
    for (int i = 0; i < count; ++i)
        arms += indent + "case is " + std::to_string(i) + " then \"" + std::to_string(i) + "\"\n";
    return arms;
}

static const lsp::DocumentSymbol* findSymbol(const std::vector<lsp::DocumentSymbol>& symbols, const std::string& name)
{
    for (const auto& symbol : symbols)
    {
        if (symbol.name == name)
            return &symbol;
    }
    return nullptr;
}

static std::vector<lsp::DocumentSymbol> documentSymbols(Fixture& fixture, const std::string& source)
{
    auto uri = fixture.newDocument("main.luwu", source);
    lsp::DocumentSymbolParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    auto result = fixture.workspace.documentSymbol(params);
    REQUIRE(result);
    return *result;
}

TEST_CASE_FIXTURE(Fixture, "a_long_match_statement_is_listed_inside_its_function")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag matchExpressions{FFlag::DebugLuwuMatchExpressions, true};

    std::string source = "local function scan(text: string)\n"
                         "    for index = 1, #text do\n"
                         "        local code = string.byte(text, index)\n"
                         "        match code\n" +
                         statementArms("        ", 12) +
                         "        end\n"
                         "    end\n"
                         "end\n";
    auto symbols = documentSymbols(*this, source);

    const lsp::DocumentSymbol* scan = findSymbol(symbols, "scan");
    REQUIRE(scan);
    const lsp::DocumentSymbol* match = findSymbol(scan->children, "match code");
    REQUIRE(match);
    CHECK_EQ(match->kind, lsp::SymbolKind::Operator);
    CHECK_EQ(match->range.start.line, 3);
    CHECK_EQ(match->range.end.line, 3 + 12 * 2 + 1);
}

TEST_CASE_FIXTURE(Fixture, "a_short_match_statement_is_not_listed")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag matchExpressions{FFlag::DebugLuwuMatchExpressions, true};

    std::string source = "local function describe(code: number)\n"
                         "    match code\n" +
                         statementArms("    ", 3) +
                         "    end\n"
                         "end\n";
    auto symbols = documentSymbols(*this, source);

    const lsp::DocumentSymbol* describe = findSymbol(symbols, "describe");
    REQUIRE(describe);
    CHECK_FALSE(findSymbol(describe->children, "match code"));
}

TEST_CASE_FIXTURE(Fixture, "a_long_match_expression_in_a_local_is_listed_by_its_header_name")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag matchExpressions{FFlag::DebugLuwuMatchExpressions, true};

    std::string source = "local function pick(): number return 1 end\n"
                         "local label = match const picked = pick()\n" +
                         expressionArms("    ", 20) +
                         "    else \"many\"\n";
    auto symbols = documentSymbols(*this, source);

    CHECK(findSymbol(symbols, "label"));
    const lsp::DocumentSymbol* match = findSymbol(symbols, "match picked");
    REQUIRE(match);
    CHECK_EQ(match->range.start.line, 1);
}

TEST_CASE_FIXTURE(Fixture, "a_long_match_expression_is_named_after_its_subject_and_holds_nested_symbols")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag matchExpressions{FFlag::DebugLuwuMatchExpressions, true};

    std::string source = "local function describe(code: number): string\n"
                         "    return match code   *   2\n"
                         "        case is 100 then do\n"
                         "            local function zero() return \"zero\" end\n"
                         "            give zero()\n" +
                         expressionArms("        ", 20) +
                         "        else \"many\"\n"
                         "end\n";
    auto symbols = documentSymbols(*this, source);

    const lsp::DocumentSymbol* describe = findSymbol(symbols, "describe");
    REQUIRE(describe);
    const lsp::DocumentSymbol* match = findSymbol(describe->children, "match code * 2");
    REQUIRE(match);
    CHECK(findSymbol(match->children, "zero"));
}

TEST_SUITE_END();
