#include "doctest.h"
#include "Fixture.h"

LUAU_FASTFLAG(LuwuClasses)
LUAU_FASTFLAG(LuwuTraits)
LUAU_FASTFLAG(LuwuGenericNominals)
LUAU_FASTFLAG(LuwuDoExpr)

TEST_SUITE_BEGIN("Refactoring");

// Extract Variable

TEST_CASE_FIXTURE(Fixture, "extract_variable_simple_expression")
{
    auto source = R"(
local a = 1
local b = 2
local x = a + b
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    // Select "a + b" on line 3 (0-indexed), columns 10-15
    params.range = {{3, 10}, {3, 15}};
    params.context.only = {lsp::CodeActionKind::RefactorExtract};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Extract to local variable");
    REQUIRE(action);
    CHECK(action->kind == lsp::CodeActionKind::RefactorExtract);
    REQUIRE(action->data);

    // Resolve the action to get the edit
    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    auto& changes = resolved.edit->changes.at(uri);
    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, R"(
local a = 1
local b = 2
local extracted = a + b
local x = extracted
)");

    // Should trigger rename at the new variable name
    REQUIRE(resolved.command);
    CHECK_EQ(resolved.command->command, "luwu.rename");
}

TEST_CASE_FIXTURE(Fixture, "extract_variable_function_call")
{
    auto source = R"(
local x = tostring(42)
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    // Select "tostring(42)" on line 1, columns 10-22
    params.range = {{1, 10}, {1, 22}};
    params.context.only = {lsp::CodeActionKind::RefactorExtract};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Extract to local variable");
    REQUIRE(action);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    auto& changes = resolved.edit->changes.at(uri);
    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, R"(
local extracted = tostring(42)
local x = extracted
)");
}

TEST_CASE_FIXTURE(Fixture, "extract_variable_not_offered_for_local_ref")
{
    auto source = R"(
local x = 1
print(x)
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    // Select just "x" on line 2
    params.range = {{2, 6}, {2, 7}};
    params.context.only = {lsp::CodeActionKind::RefactorExtract};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Extract to local variable");
    CHECK_FALSE(action);
}

// Extract Function

TEST_CASE_FIXTURE(Fixture, "extract_function_single_statement")
{
    auto source = R"(
local x = 1
print(x)
local y = 2
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    // Select "print(x)" on line 2
    params.range = {{2, 0}, {2, 8}};
    params.context.only = {lsp::CodeActionKind::RefactorExtract};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Extract to function");
    REQUIRE(action);
    CHECK(action->kind == lsp::CodeActionKind::RefactorExtract);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    auto& changes = resolved.edit->changes.at(uri);
    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, R"(
local x = 1
local function extracted(x)
    print(x)
end
extracted(x)
local y = 2
)");
}

TEST_CASE_FIXTURE(Fixture, "extract_function_with_return_values")
{
    auto source = R"(
local a = 1
local b = a + 1
print(b)
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    // Select "local b = a + 1" on line 2
    params.range = {{2, 0}, {2, 15}};
    params.context.only = {lsp::CodeActionKind::RefactorExtract};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Extract to function");
    REQUIRE(action);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    auto& changes = resolved.edit->changes.at(uri);
    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, R"(
local a = 1
local function extracted(a)
    local b = a + 1
    return b
end
local b = extracted(a)
print(b)
)");
}

TEST_CASE_FIXTURE(Fixture, "extract_function_rejects_return_statement")
{
    auto source = R"(
local function foo()
    local x = 1
    return x
end
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    // Select "return x" on line 3
    params.range = {{3, 0}, {3, 12}};
    params.context.only = {lsp::CodeActionKind::RefactorExtract};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Extract to function");
    CHECK_FALSE(action);
}

TEST_CASE_FIXTURE(Fixture, "extract_function_allows_loop_with_break")
{
    auto source = R"(
for i = 1, 10 do
    if i == 5 then break end
    print(i)
end
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    // Select the entire for loop (lines 1-4)
    params.range = {{1, 0}, {4, 3}};
    params.context.only = {lsp::CodeActionKind::RefactorExtract};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Extract to function");
    // break inside the loop should be allowed since it doesn't escape the selection
    REQUIRE(action);
}

TEST_CASE_FIXTURE(Fixture, "extract_function_allows_callback_with_return")
{
    auto source = R"(
local t = {3, 1, 2}
table.sort(t, function(a, b) return a < b end)
print(t)
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    // Select the table.sort call on line 2
    params.range = {{2, 0}, {2, 47}};
    params.context.only = {lsp::CodeActionKind::RefactorExtract};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Extract to function");
    // return inside the callback should not be flagged as a control flow escape
    REQUIRE(action);
}

// Inline Variable

TEST_CASE_FIXTURE(Fixture, "inline_variable_simple")
{
    auto source = R"(
local x = 1
print(x)
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    // Cursor on "x" in the declaration (line 1, col 6)
    params.range = {{1, 6}, {1, 7}};
    params.context.only = {lsp::CodeActionKind::RefactorInline};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Inline variable 'x'");
    REQUIRE(action);
    CHECK(action->kind == lsp::CodeActionKind::RefactorInline);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    auto& changes = resolved.edit->changes.at(uri);
    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, R"(
print(1)
)");
}

TEST_CASE_FIXTURE(Fixture, "inline_variable_from_usage_site")
{
    auto source = R"(
local x = 1
print(x)
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    // Cursor on "x" at the usage site (line 2, col 6)
    params.range = {{2, 6}, {2, 7}};
    params.context.only = {lsp::CodeActionKind::RefactorInline};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Inline variable 'x'");
    REQUIRE(action);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    auto& changes = resolved.edit->changes.at(uri);
    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, R"(
print(1)
)");
}

TEST_CASE_FIXTURE(Fixture, "inline_variable_adds_parens")
{
    auto source = R"(
local a = 1
local b = 2
local x = a + b
local y = x * 3
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    // Cursor on "x" in "local x = a + b"
    params.range = {{3, 6}, {3, 7}};
    params.context.only = {lsp::CodeActionKind::RefactorInline};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Inline variable 'x'");
    REQUIRE(action);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    auto& changes = resolved.edit->changes.at(uri);
    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, R"(
local a = 1
local b = 2
local y = (a + b) * 3
)");
}

TEST_CASE_FIXTURE(Fixture, "inline_variable_multiple_references")
{
    auto source = R"(
local x = 42
print(x)
print(x)
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 6}, {1, 7}};
    params.context.only = {lsp::CodeActionKind::RefactorInline};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Inline variable 'x'");
    REQUIRE(action);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    auto& changes = resolved.edit->changes.at(uri);
    auto newSource = applyEdit(source, changes);
    CHECK_EQ(newSource, R"(
print(42)
print(42)
)");
}

TEST_CASE_FIXTURE(Fixture, "inline_variable_resolve_rejects_multi_var")
{
    auto source = R"(
local a, b = 1, 2
print(a)
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 6}, {1, 7}};
    params.context.only = {lsp::CodeActionKind::RefactorInline};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Inline variable 'a'");
    REQUIRE(action);

    // Resolve should produce no edit for multi-var declarations
    auto resolved = workspace.codeActionResolve(*action, nullptr);
    CHECK_FALSE(resolved.edit);
}

TEST_CASE_FIXTURE(Fixture, "inline_variable_resolve_rejects_no_init")
{
    auto source = R"(
local x
x = 1
print(x)
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 6}, {1, 7}};
    params.context.only = {lsp::CodeActionKind::RefactorInline};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Inline variable 'x'");
    REQUIRE(action);

    // Resolve should produce no edit when there is no initializer
    auto resolved = workspace.codeActionResolve(*action, nullptr);
    CHECK_FALSE(resolved.edit);
}

TEST_CASE_FIXTURE(Fixture, "inline_variable_resolve_rejects_reassigned")
{
    auto source = R"(
local x = 1
x = 2
print(x)
)";
    auto uri = newDocument("test.luau", source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {{1, 6}, {1, 7}};
    params.context.only = {lsp::CodeActionKind::RefactorInline};

    auto result = workspace.codeAction(params, nullptr);
    auto action = findCodeAction(result, "Inline variable 'x'");
    REQUIRE(action);

    // Resolve should produce no edit when variable is reassigned
    auto resolved = workspace.codeActionResolve(*action, nullptr);
    CHECK_FALSE(resolved.edit);
}

// Inline Function

// The source after the inline refactoring titled `title`, offered with the cursor at `position`. Nullopt when it isn't
// offered.
static std::optional<std::string> applyInline(
    Fixture& fixture,
    const std::string& fileName,
    const std::string& source,
    lsp::Position position,
    const std::string& title
)
{
    auto uri = fixture.newDocument(fileName, source);

    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = {position, position};
    params.context.only = {lsp::CodeActionKind::RefactorInline};

    auto action = findCodeAction(fixture.workspace.codeAction(params, nullptr), title);
    if (!action)
        return std::nullopt;

    auto resolved = fixture.workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);
    return applyEdit(source, resolved.edit->changes.at(uri));
}

TEST_CASE_FIXTURE(Fixture, "inline_function_single_return")
{
    auto source = R"(
-- Whether n is big
local function is_big(n)
    return n > 10
end

local x = 5
if is_big(x) then
    print(x)
end
)";
    // Cursor on "is_big" in the call
    auto inlined = applyInline(*this, "test.luau", source, {7, 5}, "Inline function 'is_big'");
    REQUIRE(inlined);
    CHECK_EQ(*inlined, R"(
local x = 5
if x > 10 then
    print(x)
end
)");
}

TEST_CASE_FIXTURE(Fixture, "inline_function_parenthesizes_operands_and_keeps_other_uses")
{
    auto source = R"(
local function sum(a, b)
    return a + b
end
local x = sum(1, 2) * 3
local y = sum(x, 4)
)";
    auto inlined = applyInline(*this, "test.luau", source, {4, 11}, "Inline function 'sum'");
    REQUIRE(inlined);
    CHECK_EQ(*inlined, R"(
local function sum(a, b)
    return a + b
end
local x = (1 + 2) * 3
local y = sum(x, 4)
)");
}

TEST_CASE_FIXTURE(Fixture, "inline_function_not_offered_when_a_name_is_shadowed")
{
    auto source = R"(
local limit = 10
local function over(n)
    return n > limit
end
do
    local limit = 3
    print(over(5))
end
)";
    CHECK_FALSE(applyInline(*this, "test.luau", source, {7, 11}, "Inline function 'over'"));
}

TEST_CASE_FIXTURE(Fixture, "inline_variable_not_offered_for_functions")
{
    auto source = R"(
local function f()
    return 1
end
print(f())
)";
    CHECK_FALSE(applyInline(*this, "test.luau", source, {4, 6}, "Inline variable 'f'"));
}

// Luwu Do Expressions (rfcs/do-expressions.md): a body that isn't a single `return` is inlined as a do expression

TEST_CASE_FIXTURE(Fixture, "inline_function_as_do_expression")
{
    ScopedFastFlag luwuDoExpr{FFlag::LuwuDoExpr, true};

    auto source = R"(
const function count_of(text: string): number
    if text == "" then
        return 0
    end
    const n = tonumber(text)
    return n or 0
end

for _, text in { "1", "2" } do
    const total = count_of(text)
    print(total)
end
)";
    auto inlined = applyInline(*this, "test.luwu", source, {10, 19}, "Inline as do-give expression");
    REQUIRE(inlined);
    CHECK_EQ(*inlined, R"(
for _, text in { "1", "2" } do
    const total = do
        if text == "" then
            give 0
        end
        const n = tonumber(text)
        give n or 0
    print(total)
end
)");
}

TEST_CASE_FIXTURE(Fixture, "inline_function_as_do_expression_binds_arguments_with_effects")
{
    ScopedFastFlag luwuDoExpr{FFlag::LuwuDoExpr, true};

    auto source = R"(
const function twice(n: number): number
    return n + n
end

const function read(): number
    return 1
end

const x = twice(read())
)";
    auto inlined = applyInline(*this, "test.luwu", source, {9, 11}, "Inline as do-give expression");
    REQUIRE(inlined);
    CHECK_EQ(*inlined, R"(
const function read(): number
    return 1
end

const x = do
    const n: number = read()
    give n + n
)");
}

TEST_CASE_FIXTURE(Fixture, "inline_function_as_do_expression_not_offered_in_luau")
{
    ScopedFastFlag luwuDoExpr{FFlag::LuwuDoExpr, true};

    auto source = R"(
local function count_of(text: string): number
    local n = tonumber(text)
    return n or 0
end
local total = count_of("1")
)";
    CHECK_FALSE(applyInline(*this, "test.luau", source, {5, 15}, "Inline as do-give expression"));
}

// Luwu Traits (rfcs/classes/traits.md): Extract class into a trait

static std::optional<lsp::CodeAction> extractTraitAction(Fixture& fixture, const lsp::DocumentUri& uri, lsp::Range range, const std::string& title)
{
    lsp::CodeActionParams params;
    params.textDocument.uri = uri;
    params.range = range;
    params.context.only = {lsp::CodeActionKind::RefactorExtract};

    auto result = fixture.workspace.codeAction(params, nullptr);
    return findCodeAction(result, title);
}

TEST_CASE_FIXTURE(Fixture, "extract_class_into_trait")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    auto source = R"(
class Cat
    public name: string = "Taz"
    public lives: number = 9

    public function __init(self)
    end

    public function speak(self): string
        return `{self.name} meows`
    end

    public function lose_life(self)
        self.lives -= 1
    end
end
)";
    auto uri = newDocument("test.luau", source);

    // the cursor on the class's name
    auto action = extractTraitAction(*this, uri, {{1, 7}, {1, 7}}, "Extract class 'Cat' into a trait");
    REQUIRE(action);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    auto& changes = resolved.edit->changes.at(uri);
    auto newSource = applyEdit(source, changes);

    // fields with defaults and functions move; construction stays with the class
    CHECK_EQ(newSource, R"(
trait CatBehavior
    public name: string = "Taz"
    public lives: number = 9

    public function speak(self): string
        return `{self.name} meows`
    end

    public function lose_life(self)
        self.lives -= 1
    end
end

class Cat implements CatBehavior
    public function __init(self)
    end
end
)");

    // the new trait's name is offered for renaming
    REQUIRE(resolved.command);
    CHECK_EQ(resolved.command->command, "luwu.rename");
}

TEST_CASE_FIXTURE(Fixture, "extract_class_into_trait_expects_fields_without_defaults")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    auto source = R"(
class Dog(private const owner: string)
    public name: string?
    public tricks = 0

    public function learn(self)
        self.tricks += 1
    end
end
)";
    auto uri = newDocument("test.luau", source);

    // the cursor on the `class` keyword
    auto action = extractTraitAction(*this, uri, {{1, 0}, {1, 0}}, "Extract class 'Dog' into a trait");
    REQUIRE(action);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    auto newSource = applyEdit(source, resolved.edit->changes.at(uri));

    // a field without a default, and a primary constructor parameter, stay with the class and are expected
    CHECK_EQ(newSource, R"(
trait DogBehavior
    expect public name: string?
    expect private const owner: string

    public tricks = 0

    public function learn(self)
        self.tricks += 1
    end
end

class Dog(private const owner: string) implements DogBehavior
    public name: string?
end
)");
}

TEST_CASE_FIXTURE(Fixture, "extract_class_into_trait_not_offered_with_nothing_to_move")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    auto source = R"(
class Point
    x: number

    function __init(self)
        self.x = 0
    end
end
)";
    auto uri = newDocument("test.luau", source);

    CHECK_FALSE(extractTraitAction(*this, uri, {{1, 7}, {1, 7}}, "Extract class 'Point' into a trait"));
}

TEST_CASE_FIXTURE(Fixture, "extract_class_into_trait_not_offered_inside_class_body")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    auto source = R"(
class Point
    x = 0
end
)";
    auto uri = newDocument("test.luau", source);

    CHECK(extractTraitAction(*this, uri, {{1, 7}, {1, 7}}, "Extract class 'Point' into a trait"));
    CHECK_FALSE(extractTraitAction(*this, uri, {{2, 4}, {2, 4}}, "Extract class 'Point' into a trait"));
}

static const char* const catWithFunctions = R"(
class Cat
    public name: string = "Taz"
    public lives: number = 9

    public function __init(self)
    end

    public function speak(self): string
        return `{self.name} {self:sound()}`
    end

    private function sound(self): string
        return "meows"
    end

    public function lose_life(self)
        self.lives -= 1
    end
end
)";

TEST_CASE_FIXTURE(Fixture, "extract_function_into_trait")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    auto uri = newDocument("test.luau", catWithFunctions);

    // the cursor on the function's name
    auto action = extractTraitAction(*this, uri, {{8, 20}, {8, 20}}, "Extract function 'speak' into a trait");
    REQUIRE(action);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    auto newSource = applyEdit(catWithFunctions, resolved.edit->changes.at(uri));

    // what the function uses of the class, a field and another function, is expected
    CHECK_EQ(newSource, R"(
trait CatBehavior
    expect public name: string
    expect private function sound(self): string

    public function speak(self): string
        return `{self.name} {self:sound()}`
    end
end

class Cat implements CatBehavior
    public name: string = "Taz"
    public lives: number = 9

    public function __init(self)
    end

    private function sound(self): string
        return "meows"
    end

    public function lose_life(self)
        self.lives -= 1
    end
end
)");

    REQUIRE(resolved.command);
    CHECK_EQ(resolved.command->command, "luwu.rename");
}

TEST_CASE_FIXTURE(Fixture, "extract_selected_functions_into_trait")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    auto uri = newDocument("test.luau", catWithFunctions);

    auto action = extractTraitAction(*this, uri, {{12, 4}, {18, 7}}, "Extract 2 functions into a trait");
    REQUIRE(action);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    auto newSource = applyEdit(catWithFunctions, resolved.edit->changes.at(uri));

    CHECK_EQ(newSource, R"(
trait CatBehavior
    expect public lives: number

    private function sound(self): string
        return "meows"
    end

    public function lose_life(self)
        self.lives -= 1
    end
end

class Cat implements CatBehavior
    public name: string = "Taz"
    public lives: number = 9

    public function __init(self)
    end

    public function speak(self): string
        return `{self.name} {self:sound()}`
    end
end
)");
}

TEST_CASE_FIXTURE(Fixture, "extract_function_from_trait_into_needed_trait")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    auto source = R"(
trait Greeter
    expect name: string
    greeting = "hello"

    function greet(self)
        return `{self.greeting}, {self.name}`
    end
end

class Cat implements Greeter
    name = "whiskers"
end
)";
    auto uri = newDocument("test.luau", source);

    auto action = extractTraitAction(*this, uri, {{5, 14}, {5, 14}}, "Extract function 'greet' into a trait");
    REQUIRE(action);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    auto newSource = applyEdit(source, resolved.edit->changes.at(uri));

    // a trait needs the new trait rather than implementing it
    CHECK_EQ(newSource, R"(
trait GreeterBehavior
    expect name: string
    expect greeting

    function greet(self)
        return `{self.greeting}, {self.name}`
    end
end

trait Greeter needs GreeterBehavior
    expect name: string
    greeting = "hello"
end

class Cat implements Greeter
    name = "whiskers"
end
)");
}

TEST_CASE_FIXTURE(Fixture, "extract_function_into_trait_picks_an_unused_name")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    auto source = R"(
trait CatBehavior
end

class Cat implements CatBehavior
    function speak(self)
    end
end
)";
    auto uri = newDocument("test.luau", source);

    auto action = extractTraitAction(*this, uri, {{5, 14}, {5, 14}}, "Extract function 'speak' into a trait");
    REQUIRE(action);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    CHECK_EQ(applyEdit(source, resolved.edit->changes.at(uri)), R"(
trait CatBehavior
end

trait CatBehavior2
    function speak(self)
    end
end

class Cat implements CatBehavior, CatBehavior2
end
)");
}

TEST_CASE_FIXTURE(Fixture, "extract_function_into_trait_not_offered_inside_a_function_body")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ENABLE_NEW_SOLVER();

    auto uri = newDocument("test.luau", catWithFunctions);

    CHECK_FALSE(extractTraitAction(*this, uri, {{9, 8}, {9, 14}}, "Extract function 'speak' into a trait"));
    CHECK_FALSE(extractTraitAction(*this, uri, {{5, 22}, {5, 22}}, "Extract function '__init' into a trait"));
}

TEST_CASE_FIXTURE(Fixture, "extract_generic_class_into_trait")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ScopedFastFlag luwuGenericNominals{FFlag::LuwuGenericNominals, true};
    ENABLE_NEW_SOLVER();

    auto source = R"(
class Box<T = string>
    value: T?

    function get(self): T?
        return self.value
    end
end
)";
    auto uri = newDocument("test.luau", source);

    auto action = extractTraitAction(*this, uri, {{1, 7}, {1, 7}}, "Extract class 'Box' into a trait");
    REQUIRE(action);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    // the trait takes the generic parameters as written, and the class passes them on
    CHECK_EQ(applyEdit(source, resolved.edit->changes.at(uri)), R"(
trait BoxBehavior<T = string>
    expect value: T?

    function get(self): T?
        return self.value
    end
end

class Box<T = string> implements BoxBehavior<T>
    value: T?
end
)");
}

TEST_CASE_FIXTURE(Fixture, "extract_function_from_generic_trait_into_trait")
{
    ScopedFastFlag luwuClasses{FFlag::LuwuClasses, true};
    ScopedFastFlag luwuTraits{FFlag::LuwuTraits, true};
    ScopedFastFlag luwuGenericNominals{FFlag::LuwuGenericNominals, true};
    ENABLE_NEW_SOLVER();

    auto source = R"(
trait Signal<T, A...>
    expect handlers: { (A...) -> T }

    function fire(self, ...: A...)
        for _, handler in self.handlers do
            handler(...)
        end
    end
end
)";
    auto uri = newDocument("test.luau", source);

    auto action = extractTraitAction(*this, uri, {{4, 14}, {4, 14}}, "Extract function 'fire' into a trait");
    REQUIRE(action);

    auto resolved = workspace.codeActionResolve(*action, nullptr);
    REQUIRE(resolved.edit);

    CHECK_EQ(applyEdit(source, resolved.edit->changes.at(uri)), R"(
trait SignalBehavior<T, A...>
    expect handlers: { (A...) -> T }

    function fire(self, ...: A...)
        for _, handler in self.handlers do
            handler(...)
        end
    end
end

trait Signal<T, A...> needs SignalBehavior<T, A...>
    expect handlers: { (A...) -> T }
end
)");
}

TEST_SUITE_END();
