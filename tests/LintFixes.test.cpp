#include "doctest.h"
#include "Fixture.h"
#include "Flags.hpp"

#include <algorithm>

namespace
{

// Every Luwu flag on for the duration of a test, as the language server runs. Restores every flag it touched.
struct ScopedLuwuFlags
{
    std::vector<std::pair<Luau::FValue<bool>*, bool>> saved;

    ScopedLuwuFlags()
    {
        for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            saved.emplace_back(flag, flag->value);
        applyLuwuFlags(/* luauCompatibilityMode: */ false);
    }

    ~ScopedLuwuFlags()
    {
        for (auto [flag, value] : saved)
            flag->value = value;
    }
};

size_t offsetOf(const std::string& source, const lsp::Position& position)
{
    size_t offset = 0;
    for (size_t line = 0; line < position.line; ++line)
        offset = source.find('\n', offset) + 1;
    return offset + position.character;
}

std::string applyEdits(const std::string& source, std::vector<lsp::TextEdit> edits)
{
    std::sort(edits.begin(), edits.end(),
        [&](const lsp::TextEdit& a, const lsp::TextEdit& b)
        {
            return offsetOf(source, a.range.start) > offsetOf(source, b.range.start);
        });

    std::string result = source;
    for (const auto& edit : edits)
    {
        size_t begin = offsetOf(source, edit.range.start);
        result.replace(begin, offsetOf(source, edit.range.end) - begin, edit.newText);
    }
    return result;
}

struct AppliedFix
{
    std::string text;
    bool preferred;
};

// `source` in a `.luwu` file with the code action titled `title` applied, if the document has one
std::optional<AppliedFix> applyAction(Fixture& fixture, const std::string& source, const std::string& title, lsp::CodeActionKind kind)
{
    auto uri = fixture.newDocument("test.luwu", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{0, 0}, {static_cast<size_t>(std::count(source.begin(), source.end(), '\n')) + 1, 0}};
    params.context.only = {kind};

    auto result = fixture.workspace.codeAction(params, nullptr);
    if (!result)
        return std::nullopt;

    for (const auto& action : *result)
    {
        if (action.title == title && action.edit)
            return AppliedFix{applyEdits(source, action.edit->changes.at(uri)), action.isPreferred};
    }
    return std::nullopt;
}

std::optional<AppliedFix> applyQuickFix(Fixture& fixture, const std::string& source, const std::string& title)
{
    return applyAction(fixture, source, title, lsp::CodeActionKind::QuickFix);
}

std::optional<AppliedFix> applyFixAll(Fixture& fixture, const std::string& source, const std::string& lint)
{
    return applyAction(fixture, source, "Fix all '" + lint + "' warnings in this module", lsp::CodeActionKind::Source);
}

} // namespace

TEST_SUITE_BEGIN("LintFixes");

TEST_CASE_FIXTURE(Fixture, "nil_none_comparison")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function get(): number | none
    return none
end
local x = get()
if x ~= nil then
    print(x)
end
)",
        "Compare with 'none'");
    REQUIRE(fix);
    CHECK(fix->text.find("if x ~= none then") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "vararg_cast")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function f(...: number)
    return ... :: number
end
return f
)",
        "Wrap in parentheses to keep only the first value");
    REQUIRE(fix);
    CHECK(fix->text.find("return (... :: number)") != std::string::npos);
    CHECK_FALSE(fix->preferred);
}

TEST_CASE_FIXTURE(Fixture, "string_index_zero_fixes_byte_only")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(s: string)
    return string.byte(s, 0), string.sub(s, 0, 2)
end
return f
)";
    auto fix = applyFixAll(*this, source, "StringIndexZero");
    REQUIRE(fix);
    CHECK(fix->text.find("string.byte(s, 1), string.sub(s, 0, 2)") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "table_truthiness")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(xs: { number }, m: { [string]: number })
    if xs then
        print("a")
    end
    if not m then
        print("b")
    end
end
return f
)";
    auto fix = applyFixAll(*this, source, "TableTruthiness");
    REQUIRE(fix);
    CHECK(fix->text.find("if next(xs) ~= nil then") != std::string::npos);
    CHECK(fix->text.find("if next(m) == nil then") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "discarded_result")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(s: string)
    s:upper()
    return s
end
return f
)";
    auto discard = applyQuickFix(*this, source, "Discard the result with 'const _ ='");
    REQUIRE(discard);
    CHECK(discard->text.find("    const _ = s:upper()\n") != std::string::npos);

    auto keep = applyQuickFix(*this, source, "Assign the result back to 's'");
    REQUIRE(keep);
    CHECK(keep->text.find("    s = s:upper()\n") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "float_index")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local xs = { 1, 2, 3 }
print(xs[#xs / 2])
)",
        "Divide to an integer with '//'");
    REQUIRE(fix);
    CHECK(fix->text.find("print(xs[#xs // 2])") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "pairs_and_ipairs")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local t = { 1, 2 }
for k, v in pairs(t) do
    print(k, v)
end
for i, v in ipairs(t) do
    print(i, v)
end
local p = pairs
)";
    auto pairsFix = applyFixAll(*this, source, "Pairs");
    REQUIRE(pairsFix);
    CHECK(pairsFix->text.find("for k, v in t do") != std::string::npos);
    // Passing `pairs` around isn't something iteration replaces
    CHECK(pairsFix->text.find("local p = pairs") != std::string::npos);

    auto ipairsFix = applyQuickFix(*this, source, "Iterate directly instead of with 'ipairs'");
    REQUIRE(ipairsFix);
    CHECK(ipairsFix->text.find("for i, v in t do") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "self_assignment")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(x: number?, s: string?)
    x = x
    x = x or return nil
    if const s = s then
        print(s)
    end
    return x
end
return f
)";
    auto fix = applyFixAll(*this, source, "SelfAssignment");
    REQUIRE(fix);
    CHECK_EQ(fix->text, R"(
local function f(x: number?, s: string?)
    x or return nil
    if s then
        print(s)
    end
    return x
end
return f
)");
}

TEST_CASE_FIXTURE(Fixture, "floor_division")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(a: number, b: number)
    print(math.floor(a / b))
    print(-math.floor(a / b))
    print(2 / math.floor(a / b))
end
return f
)";
    auto fix = applyFixAll(*this, source, "FloorDivision");
    REQUIRE(fix);
    CHECK(fix->text.find("print(a // b)") != std::string::npos);
    CHECK(fix->text.find("print(-(a // b))") != std::string::npos);
    CHECK(fix->text.find("print(2 / (a // b))") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "discarded_values")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function two(): (number, number)
    return 1, 2
end
local function f(c: boolean)
    print(if c then two() else 0)
end
return f
)",
        "Wrap in parentheses to keep only the first value");
    REQUIRE(fix);
    CHECK(fix->text.find("print(if c then (two()) else 0)") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "any_annotation_and_cast")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(x: any, y: number)
    print(y :: any)
    return x
end
return f
)";
    auto annotation = applyFixAll(*this, source, "AnyAnnotation");
    REQUIRE(annotation);
    CHECK(annotation->text.find("local function f(x: unknown, y: number)") != std::string::npos);

    auto cast = applyQuickFix(*this, source, "Use 'unknown' instead of 'any'");
    REQUIRE(cast);
    CHECK_FALSE(cast->preferred);
}

TEST_CASE_FIXTURE(Fixture, "return_self")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
trait Aliased
    public function annotated(self): Aliased
        return self
    end
end
)",
        "Return 'Self'");
    REQUIRE(fix);
    CHECK(fix->text.find("public function annotated(self): Self") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "return_on_next_line")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function maybe(): number?
    return nil
end
local function f()
    local a = maybe() or do return
        maybe()
    local b = maybe() or return
        maybe()
    return a, b
end
return f
)";
    auto fix = applyFixAll(*this, source, "ReturnOnNextLine");
    REQUIRE(fix);
    CHECK(fix->text.find("local a = maybe() or (do return)\n") != std::string::npos);
    CHECK(fix->text.find("local b = maybe() or (return)\n") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "or_continue")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function maybe(): number?
    return nil
end
print(continue)
for i = 1, 3 do
    local x = maybe() or continue
    print(x)
end
)",
        "Continue the loop with 'or do continue'");
    REQUIRE(fix);
    CHECK(fix->text.find("local x = maybe() or do continue\n") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "binding_swallows_condition")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function find(id: number): number?
    return id
end
if const n = find(1) and n > 1 then
    print(n)
end
)",
        "Test the binding with 'when'");
    REQUIRE(fix);
    CHECK(fix->text.find("if const n = find(1) when n > 1 then") != std::string::npos);
    CHECK(fix->preferred);
}

TEST_CASE_FIXTURE(Fixture, "one_line_statement")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(x: number)
    if x > 1 then print(x) elseif x < 0 then print(-x) else print(0) end
    for i = 1, x do print(i); print(i * 2) end
end
return f
)";
    auto fix = applyFixAll(*this, source, "OneLineStatement");
    REQUIRE(fix);
    CHECK_EQ(fix->text, R"(
local function f(x: number)
    if x > 1 then
        print(x)
    elseif x < 0 then
        print(-x)
    else
        print(0)
    end
    for i = 1, x do
        print(i);
        print(i * 2)
    end
end
return f
)");
}

TEST_CASE_FIXTURE(Fixture, "lua_guard")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(x: number?, y: number, v: { ok: boolean })
    if not x then return end
    if x == y then return 1 end
    if x > 1 then return end
    if not v.ok then return x, y end
    return x
end
return f
)";
    auto fix = applyFixAll(*this, source, "LuaGuard");
    REQUIRE(fix);
    CHECK(fix->text.find("    x or return;\n") != std::string::npos);
    CHECK(fix->text.find("    x ~= y or return 1\n") != std::string::npos);
    // `not (x > 1) or return` can't start a statement
    CHECK(fix->text.find("    if x > 1 then return end\n") != std::string::npos);
    CHECK(fix->text.find("    v.ok or (return x, y)\n") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "inline_or_leave")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    auto fix = applyQuickFix(*this, R"(
local function f(x, path)
    local a = 1
    local b = 2
    local found = locate_the_thing_we_are_looking_for(path, x, "with a long argument") or return nil
    local c = 3
    local d = 4
    return a, b, c, d, found
end
return f
)",
        "Move to its own line");
    REQUIRE(fix);
    CHECK(fix->text.find("    local found = locate_the_thing_we_are_looking_for(path, x, \"with a long argument\")\n        or return nil\n") !=
          std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "or_return_comma")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(x, a, b)
    local q, r = x or return a, b
    return q, r
end
return f
)";
    auto both = applyQuickFix(*this, source, "Return both values");
    REQUIRE(both);
    CHECK(both->text.find("local q, r = x or (return a, b)") != std::string::npos);

    auto keep = applyQuickFix(*this, source, "Keep the 'return' to one value");
    REQUIRE(keep);
    CHECK(keep->text.find("local q, r = (x or return a), b") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "lua_string_helper_fix_all")
{
    ScopedLuwuFlags luwu;
    ENABLE_NEW_SOLVER();

    std::string source = R"(
local function f(s: string, p: string)
    if s:sub(1, #p) == p then
        return s:gsub("%.luau", ".luwu")
    end
    if string.find(s, "x", 1, true) == nil then
        return s
    end
    return s
end
return f
)";
    auto fix = applyFixAll(*this, source, "LuaStringHelper");
    REQUIRE(fix);
    CHECK(fix->text.find("if s:startswith(p) then") != std::string::npos);
    CHECK(fix->text.find("if not s:contains(\"x\") then") != std::string::npos);
}

TEST_SUITE_END();
